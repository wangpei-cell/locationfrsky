/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <nrf_modem_at.h>
#include <modem/lte_lc.h>
#include <modem/location.h>
#include <modem/nrf_modem_lib.h>
#include <date_time.h>
#include <net/nrf_cloud.h>
#include <net/nrf_cloud_codec.h>
#include <net/nrf_cloud_defs.h>

LOG_MODULE_REGISTER(locationfrsky, LOG_LEVEL_INF);

static K_SEM_DEFINE(location_event, 0, 1);

static K_SEM_DEFINE(lte_connected, 0, 1);

static K_SEM_DEFINE(time_update_finished, 0, 1);
static K_SEM_DEFINE(cloud_ready, 0, 1);

static bool cloud_is_ready;
static const char app_version[] = "locationfrsky-1.0.0";

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
