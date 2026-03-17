/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <nrf_modem_at.h>
#include <modem/lte_lc.h>
#include <modem/location.h>
#include <modem/nrf_modem_lib.h>
#include <date_time.h>
#include <net/nrf_cloud.h>
#include <net/nrf_cloud_codec.h>
#include <net/nrf_cloud_defs.h>
#if defined(CONFIG_NPM13XX_CHARGER)
#include <zephyr/drivers/sensor/npm13xx_charger.h>
#endif

LOG_MODULE_REGISTER(locationfrsky, LOG_LEVEL_INF);

static K_SEM_DEFINE(location_event, 0, 1);

static K_SEM_DEFINE(lte_connected, 0, 1);

static K_SEM_DEFINE(time_update_finished, 0, 1);
static K_SEM_DEFINE(cloud_ready, 0, 1);

static bool cloud_is_ready;
static const char app_version[] = "locationfrsky-1.0.0";

/* PMIC telemetry sampling interval */
#define PMIC_SAMPLE_INTERVAL K_SECONDS(5)
/* nPM13xx CHG_STATUS bit masks */
#define NPM13XX_CHG_STATUS_COMPLETE_MASK BIT(1)
#define NPM13XX_CHG_STATUS_TRICKLE_MASK  BIT(2)
#define NPM13XX_CHG_STATUS_CC_MASK       BIT(3)
#define NPM13XX_CHG_STATUS_CV_MASK       BIT(4)

static const struct device *npm1300_charger_dev;
static bool board_led_toggle;

/* Board bring-up peripherals from overlay-board-bringup.overlay */
static const struct device *icm_i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c3));
#if DT_NODE_EXISTS(DT_NODELABEL(user_led0))
static const struct gpio_dt_spec user_led0 = GPIO_DT_SPEC_GET(DT_NODELABEL(user_led0), gpios);
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(user_led1))
static const struct gpio_dt_spec user_led1 = GPIO_DT_SPEC_GET(DT_NODELABEL(user_led1), gpios);
#endif

static uint8_t icm_i2c_addr;
static bool icm_ready;
static bool gd25q_info_logged;
static uint32_t gd25q_test_cnt;
static uint32_t gd25q_fail_cnt;

#if DT_NODE_EXISTS(DT_NODELABEL(gd25q256e))
static const struct device *gd25q_flash_dev = DEVICE_DT_GET(DT_NODELABEL(gd25q256e));
static const struct spi_dt_spec gd25q_spi =
	SPI_DT_SPEC_GET(DT_NODELABEL(gd25q256e), SPI_WORD_SET(8), 0);
#endif

#if DT_NODE_EXISTS(DT_NODELABEL(gd25q256e))
static int gd25q_read_reg(uint8_t reg_cmd, uint8_t *val)
{
	uint8_t cmd = reg_cmd;
	uint8_t rx = 0;
	const struct spi_buf tx_bufs[] = {
		{ .buf = &cmd, .len = 1 },
		{ .buf = NULL, .len = 1 },
	};
	const struct spi_buf_set tx_set = {
		.buffers = tx_bufs,
		.count = ARRAY_SIZE(tx_bufs),
	};
	const struct spi_buf rx_bufs[] = {
		{ .buf = NULL, .len = 1 },
		{ .buf = &rx, .len = 1 },
	};
	const struct spi_buf_set rx_set = {
		.buffers = rx_bufs,
		.count = ARRAY_SIZE(rx_bufs),
	};
	int err = spi_transceive_dt(&gd25q_spi, &tx_set, &rx_set);

	if (!err) {
		*val = rx;
	}

	return err;
}

static int gd25q_cmd_only(uint8_t cmd)
{
	const struct spi_buf tx_bufs[] = {
		{ .buf = &cmd, .len = 1 },
	};
	const struct spi_buf_set tx_set = {
		.buffers = tx_bufs,
		.count = ARRAY_SIZE(tx_bufs),
	};

	return spi_write_dt(&gd25q_spi, &tx_set);
}

static int gd25q_wait_wip_clear(k_timeout_t timeout)
{
	int64_t end = k_uptime_get() + k_ticks_to_ms_floor64(timeout.ticks);

	while (k_uptime_get() <= end) {
		uint8_t sr1 = 0;
		int err = gd25q_read_reg(0x05, &sr1);

		if (err) {
			return err;
		}
		if ((sr1 & 0x01) == 0) {
			return 0;
		}
		k_msleep(2);
	}

	return -ETIMEDOUT;
}

static int gd25q_raw_read_03(uint32_t addr, uint8_t *dst, size_t len)
{
	uint8_t hdr[4] = {
		0x03,
		(uint8_t)((addr >> 16) & 0xFF),
		(uint8_t)((addr >> 8) & 0xFF),
		(uint8_t)(addr & 0xFF),
	};
	const struct spi_buf tx_bufs[] = {
		{ .buf = hdr, .len = sizeof(hdr) },
		{ .buf = NULL, .len = len },
	};
	const struct spi_buf_set tx_set = {
		.buffers = tx_bufs,
		.count = ARRAY_SIZE(tx_bufs),
	};
	const struct spi_buf rx_bufs[] = {
		{ .buf = NULL, .len = sizeof(hdr) },
		{ .buf = dst, .len = len },
	};
	const struct spi_buf_set rx_set = {
		.buffers = rx_bufs,
		.count = ARRAY_SIZE(rx_bufs),
	};

	return spi_transceive_dt(&gd25q_spi, &tx_set, &rx_set);
}

static int gd25q_raw_pp_02(uint32_t addr, const uint8_t *src, size_t len)
{
	uint8_t hdr[4] = {
		0x02,
		(uint8_t)((addr >> 16) & 0xFF),
		(uint8_t)((addr >> 8) & 0xFF),
		(uint8_t)(addr & 0xFF),
	};
	const struct spi_buf tx_bufs[] = {
		{ .buf = hdr, .len = sizeof(hdr) },
		{ .buf = (void *)src, .len = len },
	};
	const struct spi_buf_set tx_set = {
		.buffers = tx_bufs,
		.count = ARRAY_SIZE(tx_bufs),
	};

	return spi_write_dt(&gd25q_spi, &tx_set);
}

static int gd25q_raw_se_20(uint32_t addr)
{
	uint8_t pkt[4] = {
		0x20,
		(uint8_t)((addr >> 16) & 0xFF),
		(uint8_t)((addr >> 8) & 0xFF),
		(uint8_t)(addr & 0xFF),
	};
	const struct spi_buf tx_bufs[] = {
		{ .buf = pkt, .len = sizeof(pkt) },
	};
	const struct spi_buf_set tx_set = {
		.buffers = tx_bufs,
		.count = ARRAY_SIZE(tx_bufs),
	};

	return spi_write_dt(&gd25q_spi, &tx_set);
}

static int gd25q_raw_read_13(uint32_t addr, uint8_t *dst, size_t len)
{
	uint8_t hdr[5] = {
		0x13,
		(uint8_t)((addr >> 24) & 0xFF),
		(uint8_t)((addr >> 16) & 0xFF),
		(uint8_t)((addr >> 8) & 0xFF),
		(uint8_t)(addr & 0xFF),
	};
	const struct spi_buf tx_bufs[] = {
		{ .buf = hdr, .len = sizeof(hdr) },
		{ .buf = NULL, .len = len },
	};
	const struct spi_buf_set tx_set = {
		.buffers = tx_bufs,
		.count = ARRAY_SIZE(tx_bufs),
	};
	const struct spi_buf rx_bufs[] = {
		{ .buf = NULL, .len = sizeof(hdr) },
		{ .buf = dst, .len = len },
	};
	const struct spi_buf_set rx_set = {
		.buffers = rx_bufs,
		.count = ARRAY_SIZE(rx_bufs),
	};

	return spi_transceive_dt(&gd25q_spi, &tx_set, &rx_set);
}

static int gd25q_raw_pp_12(uint32_t addr, const uint8_t *src, size_t len)
{
	uint8_t hdr[5] = {
		0x12,
		(uint8_t)((addr >> 24) & 0xFF),
		(uint8_t)((addr >> 16) & 0xFF),
		(uint8_t)((addr >> 8) & 0xFF),
		(uint8_t)(addr & 0xFF),
	};
	const struct spi_buf tx_bufs[] = {
		{ .buf = hdr, .len = sizeof(hdr) },
		{ .buf = (void *)src, .len = len },
	};
	const struct spi_buf_set tx_set = {
		.buffers = tx_bufs,
		.count = ARRAY_SIZE(tx_bufs),
	};

	return spi_write_dt(&gd25q_spi, &tx_set);
}

static int gd25q_raw_se_21(uint32_t addr)
{
	uint8_t pkt[5] = {
		0x21,
		(uint8_t)((addr >> 24) & 0xFF),
		(uint8_t)((addr >> 16) & 0xFF),
		(uint8_t)((addr >> 8) & 0xFF),
		(uint8_t)(addr & 0xFF),
	};
	const struct spi_buf tx_bufs[] = {
		{ .buf = pkt, .len = sizeof(pkt) },
	};
	const struct spi_buf_set tx_set = {
		.buffers = tx_bufs,
		.count = ARRAY_SIZE(tx_bufs),
	};

	return spi_write_dt(&gd25q_spi, &tx_set);
}
#endif

/* ICM-42688 bank0 registers */
#define ICM_REG_BANK_SEL      0x76
#define ICM_REG_WHO_AM_I      0x75
#define ICM_REG_PWR_MGMT0     0x4E
#define ICM_REG_GYRO_CONFIG0  0x4F
#define ICM_REG_ACCEL_CONFIG0 0x50
#define ICM_REG_INT_STATUS    0x2D
#define ICM_REG_ACCEL_DATA_X1 0x1F
#define ICM_WHO_AM_I_EXPECTED 0x47

static int soc_estimate_from_mv(int32_t mv)
{
	/* Simple Li-ion voltage-based estimate for bring-up visibility only. */
	const int32_t vmin = 3300;
	const int32_t vmax = 4200;

	if (mv <= vmin) {
		return 0;
	}
	if (mv >= vmax) {
		return 100;
	}

	return (int)((mv - vmin) * 100 / (vmax - vmin));
}

static const char *chg_status_to_str(int32_t status)
{
	if (status & NPM13XX_CHG_STATUS_COMPLETE_MASK) {
		return "COMPLETE";
	}
	if (status & NPM13XX_CHG_STATUS_TRICKLE_MASK) {
		return "TRICKLE";
	}
	if (status & NPM13XX_CHG_STATUS_CC_MASK) {
		return "CC";
	}
	if (status & NPM13XX_CHG_STATUS_CV_MASK) {
		return "CV";
	}

	return "IDLE";
}

static int icm_read_reg(uint8_t reg, uint8_t *val)
{
	return i2c_write_read(icm_i2c_dev, icm_i2c_addr, &reg, 1, val, 1);
}

static int icm_write_reg(uint8_t reg, uint8_t val)
{
	uint8_t tx[2] = {reg, val};

	return i2c_write(icm_i2c_dev, tx, sizeof(tx), icm_i2c_addr);
}

static int icm_read_burst(uint8_t start_reg, uint8_t *buf, size_t len)
{
	return i2c_write_read(icm_i2c_dev, icm_i2c_addr, &start_reg, 1, buf, len);
}

static int16_t be16_to_i16(const uint8_t *p)
{
	return (int16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void board_leds_update(void)
{
#if DT_NODE_EXISTS(DT_NODELABEL(user_led0))
	if (gpio_is_ready_dt(&user_led0)) {
		(void)gpio_pin_set_dt(&user_led0, board_led_toggle ? 1 : 0);
	}
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(user_led1))
	if (gpio_is_ready_dt(&user_led1)) {
		(void)gpio_pin_set_dt(&user_led1, board_led_toggle ? 0 : 1);
	}
#endif
	board_led_toggle = !board_led_toggle;
}

static void board_leds_init(void)
{
#if DT_NODE_EXISTS(DT_NODELABEL(user_led0))
	if (gpio_is_ready_dt(&user_led0)) {
		(void)gpio_pin_configure_dt(&user_led0, GPIO_OUTPUT_INACTIVE);
	}
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(user_led1))
	if (gpio_is_ready_dt(&user_led1)) {
		(void)gpio_pin_configure_dt(&user_led1, GPIO_OUTPUT_INACTIVE);
	}
#endif
}

static void icm_probe(void)
{
	uint8_t who = 0;
	int err;
	const uint8_t candidate_addrs[] = {0x68, 0x69};

	if (!device_is_ready(icm_i2c_dev)) {
		printk("ICM: i2c3 not ready\n");
		return;
	}

	for (size_t i = 0; i < ARRAY_SIZE(candidate_addrs); i++) {
		icm_i2c_addr = candidate_addrs[i];
		err = icm_read_reg(ICM_REG_WHO_AM_I, &who);
		if (!err) {
			icm_ready = true;
			printk("ICM: found at 0x%02X, WHO_AM_I=0x%02X%s\n",
			       icm_i2c_addr, who,
			       (who == ICM_WHO_AM_I_EXPECTED) ? " (OK)" : " (UNEXPECTED)");

			/* Basic sensor bring-up:
			 * - select bank0
			 * - accel/gyro low-noise mode
			 * - set accel/gyro ODR to 200Hz
			 */
			(void)icm_write_reg(ICM_REG_BANK_SEL, 0x00);
			(void)icm_write_reg(ICM_REG_PWR_MGMT0, 0x0F);
			(void)icm_write_reg(ICM_REG_ACCEL_CONFIG0, 0x07);
			(void)icm_write_reg(ICM_REG_GYRO_CONFIG0, 0x07);
			k_msleep(50);

			return;
		}
	}

	printk("ICM: probe failed on 0x68/0x69\n");
}

static void icm_log_once(void)
{
	uint8_t raw[12];
	uint8_t int_status = 0;
	int err;
	int16_t ax;
	int16_t ay;
	int16_t az;
	int16_t gx;
	int16_t gy;
	int16_t gz;

	if (!icm_ready) {
		return;
	}

	err = icm_read_reg(ICM_REG_INT_STATUS, &int_status);
	if (err) {
		printk("ICM int_status read failed: %d (addr=0x%02X)\n", err, icm_i2c_addr);
		return;
	}
	if ((int_status & BIT(3)) == 0) {
		printk("ICM: data not ready yet (INT_STATUS=0x%02X)\n", int_status);
		return;
	}

	err = icm_read_burst(ICM_REG_ACCEL_DATA_X1, raw, sizeof(raw));
	if (err) {
		/* -5 = EIO, usually bus NACK/no response. */
		printk("ICM read failed: %d (addr=0x%02X)\n", err, icm_i2c_addr);
		return;
	}

	ax = be16_to_i16(&raw[0]);
	ay = be16_to_i16(&raw[2]);
	az = be16_to_i16(&raw[4]);
	gx = be16_to_i16(&raw[6]);
	gy = be16_to_i16(&raw[8]);
	gz = be16_to_i16(&raw[10]);

	printk("ICM raw ACC[%d,%d,%d] GYR[%d,%d,%d]\n", ax, ay, az, gx, gy, gz);
}

static void gd25q_rw_test_periodic(void)
{
#if DT_NODE_EXISTS(DT_NODELABEL(gd25q256e))
	static uint8_t tx[256];
	static uint8_t rx[256];
	uint8_t cmd = 0x9f;
	uint8_t dummy[3] = {0};
	uint8_t jedec[3] = {0};
	uint8_t sr1 = 0;
	uint8_t sr2 = 0;
	uint8_t sr1_after_wren = 0;
	const off_t offs = 0x001000 + ((gd25q_test_cnt % 16U) * 0x1000U);
	const size_t erase_sz = 4096;
	int err;

	if (!device_is_ready(gd25q_flash_dev)) {
		printk("GD25Q: flash device not ready\n");
		return;
	}
	if (!spi_is_ready_dt(&gd25q_spi)) {
		printk("GD25Q: SPI bus not ready\n");
		return;
	}

	if (!gd25q_info_logged) {
		const struct spi_buf tx_bufs[] = {
			{ .buf = &cmd, .len = 1 },
			{ .buf = dummy, .len = sizeof(dummy) },
		};
		const struct spi_buf_set tx_set = {
			.buffers = tx_bufs,
			.count = ARRAY_SIZE(tx_bufs),
		};
		const struct spi_buf rx_bufs[] = {
			{ .buf = NULL, .len = 1 },
			{ .buf = jedec, .len = sizeof(jedec) },
		};
		const struct spi_buf_set rx_set = {
			.buffers = rx_bufs,
			.count = ARRAY_SIZE(rx_bufs),
		};

		err = spi_transceive_dt(&gd25q_spi, &tx_set, &rx_set);
		if (err) {
			printk("GD25Q: RDID failed err=%d\n", err);
		} else {
			printk("GD25Q: JEDEC ID = %02X %02X %02X\n",
			       jedec[0], jedec[1], jedec[2]);
		}
	}

	if (!gd25q_info_logged) {
		err = gd25q_read_reg(0x05, &sr1);
		if (!err) {
			err = gd25q_read_reg(0x35, &sr2);
		}
		if (err) {
			printk("GD25Q: read status reg failed err=%d\n", err);
		} else {
			printk("GD25Q: SR1=0x%02X SR2=0x%02X\n", sr1, sr2);
		}
	}

	/* Manual write-enable path check: WEL bit (SR1 bit1) should become 1 */
	if (!gd25q_info_logged) {
		err = gd25q_cmd_only(0x06); /* WREN */
		if (err) {
			printk("GD25Q: WREN cmd failed err=%d\n", err);
		} else {
			err = gd25q_read_reg(0x05, &sr1_after_wren);
			if (err) {
				printk("GD25Q: read SR1 after WREN failed err=%d\n", err);
			} else {
				printk("GD25Q: SR1 after WREN = 0x%02X (WEL=%u)\n",
				       sr1_after_wren, (sr1_after_wren >> 1) & 0x1);
			}
		}
		(void)gd25q_cmd_only(0x04); /* WRDI */
		gd25q_info_logged = true;
	}

	for (size_t i = 0; i < sizeof(tx); i++) {
		tx[i] = (uint8_t)((i ^ 0xA5) + (gd25q_test_cnt & 0xFF));
	}

	err = flash_erase(gd25q_flash_dev, offs, erase_sz);
	if (err) {
		printk("GD25Q: erase failed err=%d\n", err);
		return;
	}

	err = flash_write(gd25q_flash_dev, offs, tx, sizeof(tx));
	if (err) {
		printk("GD25Q: write failed err=%d\n", err);
		return;
	}

	err = flash_read(gd25q_flash_dev, offs, rx, sizeof(rx));
	if (err) {
		printk("GD25Q: read failed err=%d\n", err);
		return;
	}

	if (memcmp(tx, rx, sizeof(tx)) == 0) {
		printk("GD25Q[%u]: PASS addr=0x%06x len=%u (fail=%u)\n",
		       gd25q_test_cnt,
		       (unsigned int)offs,
		       (unsigned int)sizeof(tx),
		       gd25q_fail_cnt);
	} else {
		size_t bad = 0;
		gd25q_fail_cnt++;

		while ((bad < sizeof(tx)) && (tx[bad] == rx[bad])) {
			bad++;
		}

		printk("GD25Q[%u]: FAIL addr=0x%06x mismatch @%u exp=0x%02X got=0x%02X (fail=%u)\n",
		       gd25q_test_cnt,
		       (unsigned int)offs,
		       (unsigned int)bad,
		       (unsigned int)tx[bad],
		       (unsigned int)rx[bad],
		       gd25q_fail_cnt);
		printk("GD25Q: readback[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
		       rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[6], rx[7]);
		if (!gd25q_read_reg(0x05, &sr1) && !gd25q_read_reg(0x35, &sr2)) {
			printk("GD25Q: SR1/SR2 after fail = 0x%02X/0x%02X\n", sr1, sr2);
		}

		/* Raw SPI opcode path to separate driver issue from device issue */
		{
			uint8_t raw_tx[16];
			uint8_t raw_rx[16];
			bool raw_ok = false;

			for (size_t i = 0; i < sizeof(raw_tx); i++) {
				raw_tx[i] = (uint8_t)(0x5A ^ i);
			}

			err = gd25q_cmd_only(0x06); /* WREN */
			if (!err) {
				err = gd25q_raw_se_20((uint32_t)offs);
			}
			if (!err) {
				err = gd25q_wait_wip_clear(K_SECONDS(2));
			}
			if (!err) {
				err = gd25q_cmd_only(0x06); /* WREN */
			}
			if (!err) {
				err = gd25q_raw_pp_02((uint32_t)offs, raw_tx, sizeof(raw_tx));
			}
			if (!err) {
				err = gd25q_wait_wip_clear(K_SECONDS(1));
			}
			if (!err) {
				err = gd25q_raw_read_03((uint32_t)offs, raw_rx, sizeof(raw_rx));
			}
			if (!err && memcmp(raw_tx, raw_rx, sizeof(raw_tx)) == 0) {
				raw_ok = true;
			}

			if (raw_ok) {
				printk("GD25Q: RAW SPI 0x02/0x03 test PASS\n");
			} else if (err) {
				printk("GD25Q: RAW SPI test FAIL err=%d\n", err);
			} else {
				printk("GD25Q: RAW SPI test FAIL (exp=%02X got=%02X)\n",
				       raw_tx[0], raw_rx[0]);
			}

			/* 4-byte opcode path check: some parts default to 4-byte commands */
			err = gd25q_cmd_only(0x06); /* WREN */
			if (!err) {
				err = gd25q_raw_se_21((uint32_t)offs);
			}
			if (!err) {
				err = gd25q_wait_wip_clear(K_SECONDS(2));
			}
			if (!err) {
				err = gd25q_cmd_only(0x06); /* WREN */
			}
			if (!err) {
				err = gd25q_raw_pp_12((uint32_t)offs, raw_tx, sizeof(raw_tx));
			}
			if (!err) {
				err = gd25q_wait_wip_clear(K_SECONDS(1));
			}
			if (!err) {
				err = gd25q_raw_read_13((uint32_t)offs, raw_rx, sizeof(raw_rx));
			}
			if (!err && memcmp(raw_tx, raw_rx, sizeof(raw_tx)) == 0) {
				printk("GD25Q: RAW SPI 0x12/0x13 test PASS\n");
			} else if (err) {
				printk("GD25Q: RAW SPI 0x12/0x13 test FAIL err=%d\n", err);
			} else {
				printk("GD25Q: RAW SPI 0x12/0x13 test FAIL (exp=%02X got=%02X)\n",
				       raw_tx[0], raw_rx[0]);
			}
		}
	}
	gd25q_test_cnt++;
#else
	printk("GD25Q: node missing in devicetree\n");
#endif
}

static void npm1300_telemetry_log_once(void)
{
#if DT_NODE_EXISTS(DT_NODELABEL(npm1300_charger))
	struct sensor_value voltage = {0};
	struct sensor_value current = {0};
	struct sensor_value status = {0};
	struct sensor_value vbus_present = {0};
	int err;
	int32_t mv;
	int32_t ua;

	if (!npm1300_charger_dev) {
		npm1300_charger_dev = DEVICE_DT_GET(DT_NODELABEL(npm1300_charger));
	}

	if (!device_is_ready(npm1300_charger_dev)) {
		printk("PMIC: charger not ready\n");
		return;
	}

	err = sensor_sample_fetch(npm1300_charger_dev);
	if (err) {
		printk("PMIC: sample_fetch err=%d\n", err);
		return;
	}

	err = sensor_channel_get(npm1300_charger_dev, SENSOR_CHAN_GAUGE_VOLTAGE, &voltage);
	if (err) {
		printk("PMIC: voltage read err=%d\n", err);
		return;
	}

	err = sensor_channel_get(npm1300_charger_dev, SENSOR_CHAN_GAUGE_AVG_CURRENT, &current);
	if (err) {
		printk("PMIC: current read err=%d\n", err);
		return;
	}

	err = sensor_channel_get(npm1300_charger_dev, SENSOR_CHAN_NPM13XX_CHARGER_STATUS, &status);
	if (err) {
		printk("PMIC: status read err=%d\n", err);
		return;
	}

	err = sensor_attr_get(npm1300_charger_dev,
			      SENSOR_CHAN_NPM13XX_CHARGER_VBUS_STATUS,
			      SENSOR_ATTR_NPM13XX_CHARGER_VBUS_PRESENT, &vbus_present);
	if (err) {
		printk("PMIC: vbus read err=%d\n", err);
		return;
	}

	mv = voltage.val1 * 1000 + (voltage.val2 / 1000);
	ua = current.val1 * 1000000 + current.val2;

	printk("PMIC: V=%d mV, I=%d uA, SoC(est)=%d%%, CHG_STATUS=0x%02X(%s), VBUS=%d\n",
	       mv, ua, soc_estimate_from_mv(mv), status.val1, chg_status_to_str(status.val1),
	       vbus_present.val1);
#else
	printk("PMIC: charger node missing in devicetree\n");
#endif
}

static void npm1300_telemetry_thread(void)
{
	board_leds_init();
	icm_probe();

	while (1) {
		board_leds_update();
		gd25q_rw_test_periodic();
		icm_log_once();
		npm1300_telemetry_log_once();
		k_sleep(PMIC_SAMPLE_INTERVAL);
	}
}

K_THREAD_DEFINE(npm1300_telemetry_tid, 1024, npm1300_telemetry_thread,
		NULL, NULL, NULL, 7, 0, 0);

static void date_time_evt_handler(const struct date_time_evt *evt)
{
	k_sem_give(&time_update_finished);
}

static void lte_event_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if ((evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
		     (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			printk("Connected to LTE\n");
			k_sem_give(&lte_connected);
		}
		break;
	default:
		break;
	}
}

static void npm1300_debug_probe(void)
{
#if DT_NODE_EXISTS(DT_NODELABEL(npm1300_pmic))
	const struct device *pmic = DEVICE_DT_GET(DT_NODELABEL(npm1300_pmic));

	printk("nPM1300 probe start\n");
	printk("  pmic ready: %s\n", device_is_ready(pmic) ? "yes" : "no");

#if DT_NODE_EXISTS(DT_NODELABEL(npm1300_charger))
	const struct device *charger = DEVICE_DT_GET(DT_NODELABEL(npm1300_charger));
	int err;

	printk("  charger ready: %s\n", device_is_ready(charger) ? "yes" : "no");

	if (device_is_ready(charger)) {
		err = sensor_sample_fetch(charger);
		printk("  charger sample_fetch: %d\n", err);

#if defined(CONFIG_NPM13XX_CHARGER)
		if (!err) {
			struct sensor_value status = {0};
			struct sensor_value chg_err = {0};
			struct sensor_value vbus_present = {0};

			err = sensor_channel_get(charger, SENSOR_CHAN_NPM13XX_CHARGER_STATUS, &status);
			printk("  charger status read: %d, value=%d\n", err, status.val1);
			err = sensor_channel_get(charger, SENSOR_CHAN_NPM13XX_CHARGER_ERROR, &chg_err);
			printk("  charger error read: %d, value=%d\n", err, chg_err.val1);
			err = sensor_attr_get(charger,
					      SENSOR_CHAN_NPM13XX_CHARGER_VBUS_STATUS,
					      SENSOR_ATTR_NPM13XX_CHARGER_VBUS_PRESENT,
					      &vbus_present);
			printk("  vbus present read: %d, value=%d\n", err, vbus_present.val1);
		}
#endif
	}
#else
	printk("  charger node missing in devicetree\n");
#endif
#else
	printk("nPM1300 node missing in devicetree\n");
#endif
}

static int cloud_location_publish(const struct location_event_data *event_data)
{
	int err;
	NRF_CLOUD_OBJ_JSON_DEFINE(msg_obj);
	struct nrf_cloud_tx_data tx_data = {
		.obj = &msg_obj,
		.topic_type = NRF_CLOUD_TOPIC_MESSAGE,
		.qos = MQTT_QOS_1_AT_LEAST_ONCE,
		.id = NCT_MSG_ID_USE_NEXT_INCREMENT,
	};

	err = nrf_cloud_obj_msg_init(&msg_obj, NRF_CLOUD_JSON_APPID_VAL_LOCATION,
				     NRF_CLOUD_JSON_MSG_TYPE_VAL_DATA);
	if (!err) {
		err = nrf_cloud_obj_num_add(&msg_obj, NRF_CLOUD_LOCATION_JSON_KEY_LAT,
					    event_data->location.latitude, true);
	}
	if (!err) {
		err = nrf_cloud_obj_num_add(&msg_obj, NRF_CLOUD_LOCATION_JSON_KEY_LON,
					    event_data->location.longitude, true);
	}
	if (!err) {
		err = nrf_cloud_obj_num_add(&msg_obj, NRF_CLOUD_LOCATION_JSON_KEY_UNCERT,
					    event_data->location.accuracy, true);
	}
	if (!err) {
		err = nrf_cloud_obj_str_add(&msg_obj, "method",
					    location_method_str(event_data->method), true);
	}
	if (!err) {
		err = nrf_cloud_send(&tx_data);
	}

	nrf_cloud_obj_free(&msg_obj);
	return err;
}

static void cloud_event_handler(const struct nrf_cloud_evt *nrf_cloud_evt)
{
	switch (nrf_cloud_evt->type) {
	case NRF_CLOUD_EVT_READY:
		printk("nRF Cloud connection ready\n");
		cloud_is_ready = true;
		k_sem_give(&cloud_ready);
		break;
	case NRF_CLOUD_EVT_TRANSPORT_CONNECT_ERROR:
		printk("nRF Cloud transport connect error: %d\n", nrf_cloud_evt->status);
		break;
	case NRF_CLOUD_EVT_TRANSPORT_DISCONNECTED:
		printk("nRF Cloud disconnected\n");
		cloud_is_ready = false;
		break;
	case NRF_CLOUD_EVT_ERROR:
		printk("nRF Cloud error: %d\n", nrf_cloud_evt->status);
		break;
	default:
		break;
	}
}

static void location_event_handler(const struct location_event_data *event_data)
{
	int err;

	switch (event_data->id) {
	case LOCATION_EVT_LOCATION:
		printk("Got location:\n");
		printk("  method: %s\n", location_method_str(event_data->method));
		printk("  latitude: %.06f\n", event_data->location.latitude);
		printk("  longitude: %.06f\n", event_data->location.longitude);
		printk("  accuracy: %.01f m\n", (double)event_data->location.accuracy);
		if (event_data->location.datetime.valid) {
			printk("  date: %04d-%02d-%02d\n",
				event_data->location.datetime.year,
				event_data->location.datetime.month,
				event_data->location.datetime.day);
			printk("  time: %02d:%02d:%02d.%03d UTC\n",
				event_data->location.datetime.hour,
				event_data->location.datetime.minute,
				event_data->location.datetime.second,
				event_data->location.datetime.ms);
		}
		printk("  Google maps URL: https://maps.google.com/?q=%.06f,%.06f\n\n",
			event_data->location.latitude, event_data->location.longitude);

		if (cloud_is_ready) {
			err = cloud_location_publish(event_data);
			if (err) {
				printk("Publishing location to nRF Cloud failed, error: %d\n", err);
			} else {
				printk("Location published to nRF Cloud\n");
			}
		}
		break;

	case LOCATION_EVT_TIMEOUT:
		printk("Getting location timed out\n\n");
		break;

	case LOCATION_EVT_ERROR:
		printk("Getting location failed\n\n");
		break;

	case LOCATION_EVT_GNSS_ASSISTANCE_REQUEST:
		printk("Getting location assistance requested (A-GNSS). Not doing anything.\n\n");
		break;

	case LOCATION_EVT_GNSS_PREDICTION_REQUEST:
		printk("Getting location assistance requested (P-GPS). Not doing anything.\n\n");
		break;

	default:
		printk("Getting location: Unknown event\n\n");
		break;
	}

	k_sem_give(&location_event);
}

static void location_event_wait(void)
{
	k_sem_take(&location_event, K_FOREVER);
}

/**
 * @brief Retrieve location with GNSS as first priority and cellular as fallback.
 */
static void location_gnss_first_with_fallback_get(void)
{
	int err;
	struct location_config config;
	enum location_method methods[] = {LOCATION_METHOD_GNSS, LOCATION_METHOD_CELLULAR};

	location_config_defaults_set(&config, ARRAY_SIZE(methods), methods);
	/* Give GNSS enough time before falling back to cellular. */
	config.methods[0].gnss.timeout = 180 * MSEC_PER_SEC;
	config.methods[1].cellular.timeout = 60 * MSEC_PER_SEC;

	printk("Requesting location with GNSS priority and cellular fallback...\n");

	err = location_request(&config);
	if (err) {
		printk("Requesting location failed, error: %d\n", err);
		return;
	}

	location_event_wait();
}

/**
 * @brief Retrieve location with default configuration.
 *
 * @details This is achieved by not passing configuration at all to location_request().
 */
static void location_default_get(void)
{
	int err;

	printk("Requesting location with the default configuration...\n");

	err = location_request(NULL);
	if (err) {
		printk("Requesting location failed, error: %d\n", err);
		return;
	}

	location_event_wait();
}

/**
 * @brief Retrieve location with GNSS low accuracy.
 */
static void location_gnss_low_accuracy_get(void)
{
	int err;
	struct location_config config;
	enum location_method methods[] = {LOCATION_METHOD_GNSS};

	location_config_defaults_set(&config, ARRAY_SIZE(methods), methods);
	config.methods[0].gnss.accuracy = LOCATION_ACCURACY_LOW;

	printk("Requesting low accuracy GNSS location...\n");

	err = location_request(&config);
	if (err) {
		printk("Requesting location failed, error: %d\n", err);
		return;
	}

	location_event_wait();
}

/**
 * @brief Retrieve location with GNSS high accuracy.
 */
static void location_gnss_high_accuracy_get(void)
{
	int err;
	struct location_config config;
	enum location_method methods[] = {LOCATION_METHOD_GNSS};

	location_config_defaults_set(&config, ARRAY_SIZE(methods), methods);
	config.methods[0].gnss.accuracy = LOCATION_ACCURACY_HIGH;

	printk("Requesting high accuracy GNSS location...\n");

	err = location_request(&config);
	if (err) {
		printk("Requesting location failed, error: %d\n", err);
		return;
	}

	location_event_wait();
}

#if defined(CONFIG_LOCATION_METHOD_WIFI)
/**
 * @brief Retrieve location with Wi-Fi positioning as first priority, GNSS as second
 * and cellular as third.
 */
static void location_wifi_get(void)
{
	int err;
	struct location_config config;
	enum location_method methods[] = {
		LOCATION_METHOD_WIFI,
		LOCATION_METHOD_GNSS,
		LOCATION_METHOD_CELLULAR};

	location_config_defaults_set(&config, ARRAY_SIZE(methods), methods);

	printk("Requesting Wi-Fi location with GNSS and cellular fallback...\n");

	err = location_request(&config);
	if (err) {
		printk("Requesting location failed, error: %d\n", err);
		return;
	}

	location_event_wait();
}
#endif

/**
 * @brief Retrieve location periodically with GNSS as first priority and cellular as second.
 */
static void location_gnss_periodic_get(void)
{
	int err;
	struct location_config config;
	enum location_method methods[] = {LOCATION_METHOD_GNSS, LOCATION_METHOD_CELLULAR};

	location_config_defaults_set(&config, ARRAY_SIZE(methods), methods);
	config.interval = 30;

	printk("Requesting 30s periodic GNSS location with cellular fallback...\n");

	err = location_request(&config);
	if (err) {
		printk("Requesting location failed, error: %d\n", err);
		return;
	}
}

int main(void)
{
	int err;

	printk("Location sample started\n\n");

	err = nrf_modem_lib_init();
	if (err) {
		printk("Modem library initialization failed, error: %d\n", err);
		return err;
	}

	npm1300_debug_probe();

	if (IS_ENABLED(CONFIG_DATE_TIME)) {
		/* Registering early for date_time event handler to avoid missing
		 * the first event after LTE is connected.
		 */
		date_time_register_handler(date_time_evt_handler);
	}

	printk("Connecting to LTE...\n");

	lte_lc_register_handler(lte_event_handler);

	lte_lc_connect();

	k_sem_take(&lte_connected, K_FOREVER);

	/* A-GNSS/P-GPS needs to know the current time. */
	if (IS_ENABLED(CONFIG_DATE_TIME)) {
		printk("Waiting for current time\n");

		/* Wait for an event from the Date Time library. */
		k_sem_take(&time_update_finished, K_MINUTES(10));

		if (!date_time_is_valid()) {
			printk("Failed to get current time. Continuing anyway.\n");
		}
	}

	struct nrf_cloud_init_param cloud_params = {
		.event_handler = cloud_event_handler,
		.application_version = app_version,
	};

	err = nrf_cloud_init(&cloud_params);
	if (err) {
		printk("nRF Cloud init failed, error: %d\n", err);
		return err;
	}

	err = nrf_cloud_connect();
	if (err) {
		printk("nRF Cloud connect failed, error: %d\n", err);
		return err;
	}

	k_sem_take(&cloud_ready, K_FOREVER);

	err = location_init(location_event_handler);
	if (err) {
		printk("Initializing the Location library failed, error: %d\n", err);
		return -1;
	}

	location_gnss_first_with_fallback_get();

	location_default_get();

	location_gnss_low_accuracy_get();

	location_gnss_high_accuracy_get();

#if defined(CONFIG_LOCATION_METHOD_WIFI)
	location_wifi_get();
#endif

	location_gnss_periodic_get();

	return 0;
}
