# nRF9151 + 中国移动 NB-IoT + nRF Cloud 接入实战记录（可直接复现）

本文档基于当前工程 `locationfrsky` 的已验证流程整理，目标是：
- 设备：nRF9151（空片）
- 网络：中国移动 NB-IoT
- 云端：nRF Cloud（MQTT）
- 功能：定位并上报到 nRF Cloud

---

## 1. 最终结论（先看）

1. 证书问题已经解决：`sec_tag=16842753` 下 `type 0/1/2` 都已写入。  
2. 云连接问题已经解决：日志出现 `nRF Cloud connection ready`。  
3. 后续定位失败（如 `40410`）属于云定位数据命中/场景问题，不是证书或 MQTT 链路问题。  
4. 当前工程已改为 **GNSS 优先，Cellular 兜底**，不再使用“1秒 GNSS 超时”的演示逻辑。

---

## 2. 工程内关键配置

### 2.1 主配置（`prj.conf`）

当前已启用并验证可用：
- `CONFIG_NRF_CLOUD=y`
- `CONFIG_NRF_CLOUD_MQTT=y`
- `CONFIG_NRF_CLOUD_REST=y`
- `CONFIG_NRF_CLOUD_CHECK_CREDENTIALS=y`
- `CONFIG_MODEM_KEY_MGMT=y`
- `CONFIG_NRF_CLOUD_SEC_TAG=16842753`

### 2.2 新增 overlay

- `overlay-cmcc-nbiot.conf`：中国移动 NB-IoT 相关网络参数。
- `overlay-network-debug.conf`：更稳搜网调试参数（双模/放宽策略）。
- `overlay-cloud-uuid.conf`：使用 UUID 作为 nRF Cloud 设备 ID。

> 说明：本次你的 MQTT 认证在 IMEI ID 下失败，切换 UUID 后成功连接。

---

## 3. 代码行为修改（已完成）

文件：`src/main.c`

已把示例中的“故意 1 秒超时触发 Cellular fallback”改为真实策略：
- GNSS 优先，`timeout = 180s`
- Cellular 兜底，`timeout = 60s`

日志会显示：
- `Requesting location with GNSS priority and cellular fallback...`

---

## 4. 官方推荐的证书下发路径（已验证可走通）

你这次最终跑通的是 `nrf_device_provisioning` 路线，而不是手工 `%CMNG=0` 粘贴证书。

原因：AT 手工粘贴大证书在某些串口/换行/工具链组合下很容易失败（你遇到过 `ERROR`/`CME 527`）。

推荐流程：

1. 先烧录 `nrf_device_provisioning` 样例（建议带 `overlay-at_shell.conf`）。
2. 让设备联网并完成 provisioning。
3. 日志看到 `Provisioning done`。
4. 用 AT 校验证书：

```at
AT%CMNG=1,16842753
```

期望看到三条：
- `16842753,0`（CA）
- `16842753,1`（Client cert）
- `16842753,2`（Private key）

---

## 5. `locationfrsky` 编译烧录命令（推荐）

### 5.1 中国移动 NB-IoT + UUID 云身份

```bash
cd /Volumes/macmini/Nordic/locationfrsky
west build -p -b <你的board> -- -DEXTRA_CONF_FILE="overlay-cmcc-nbiot.conf;overlay-cloud-uuid.conf"
west flash
```

### 5.2 若首次搜网不稳（调试）

```bash
west build -p -b <你的board> -- -DEXTRA_CONF_FILE="overlay-network-debug.conf;overlay-cloud-uuid.conf"
west flash
```

---

## 6. 上电后正确日志判据

### 6.1 网络层
- `Connected to LTE`
- `%XTIME ...`（时间有效）

### 6.2 云连接层
- `Device ID: 5043...`（UUID）
- `Sec tag: 16842753`
- `Host name: mqtt.nrfcloud.com`
- `nRF Cloud connection ready`

### 6.3 业务层（定位）
- 有 `Got location` 打印
- 出现 `Location published to nRF Cloud`

---

## 7. 常见错误对照

### `Could not connect ... err -111`
- 多见于证书未完整写入或网络未通。
- 先检查 `%CMNG=1,16842753` 是否有 `0/1/2`。

### `MQTT input error: -128`
- 多见于设备身份不匹配（Device ID 与证书/云端绑定不一致）。
- 本项目通过 `overlay-cloud-uuid.conf` 切 UUID 后已解决。

### `nrf_cloud_location ... 40410`
- 云端位置服务对当前小区数据无结果，常见于 NB-IoT 场景。
- 不是证书问题，也不是 MQTT 链路问题。

### `Failed ... no valid datetime reference (-116)`
- 时间源无效导致 provisioning/定位流程失败。
- 先保证 LTE 注册并拿到 `%XTIME`；必要时启用 NTP 兜底。

### `+CME ERROR: 527`
- 常见于 `%CMNG` 写入证书格式/内容/通道处理异常（多行证书、转义、换行等）。
- 建议改用 `nrf_device_provisioning` 自动下发。

---

## 8. 量产建议（简版）

1. 固定走 provisioning 样例+云端规则，不走人工 AT 粘贴证书。  
2. 统一 `sec_tag=16842753`。  
3. 统一使用 UUID 作为 nRF Cloud 设备 ID。  
4. 产测增加两条强校验：
   - `AT%CMNG=1,16842753` 有 `0/1/2`
   - 应用日志出现 `nRF Cloud connection ready`

---

## 9. 你这次已达成状态（记录）

- LTE 附着成功。  
- 时间有效（`%XTIME` 有返回）。  
- 证书三件套已在 `16842753`。  
- MQTT 已成功连上 nRF Cloud（`nRF Cloud connection ready`）。  
- 当前剩余优化点仅在定位成功率（GNSS 天线环境 / 小区定位命中率）。

---

## 10. `nrf_device_provisioning` 如何连到“你的” nRF Cloud（详细版）

这一节回答核心问题：为什么设备不是随便连某个云，而是连到你的 Team。

### 10.1 参与对象

- 设备端：`nrf_device_provisioning` 应用（运行在 nRF9151）
- 云端服务：nRF Cloud Provisioning Service
- 身份凭据：你的 nRF Cloud API Key（归属某个 Team）
- 设备标识：IMEI / UUID（最终用于设备唯一身份）

### 10.2 绑定关系是怎么建立的

1. 你本地执行 `claim_and_provision_device` 时，带了 `--api-key <你的key>`。  
2. nRF Cloud 用这个 key 识别你的账号/Team。  
3. 设备被 `claim` 到这个 Team（你日志中返回了 `ownerId` / `tags` / `status`）。  
4. 设备上线跑 provisioning 时，只会命中这个 Team 下的 provisioning 规则。  
5. 规则返回证书下发命令，设备写入 `%CMNG`。  
6. 后续 MQTT 连接成功后，设备自然出现在该 Team 的设备列表与数据流中。

一句话：**API key 决定 Team 归属，claim 决定设备归属，provisioning rule 决定下发内容。**

### 10.3 建议的标准操作顺序（一次完整跑通）

#### A. 云端准备

1. 登录 nRF Cloud，确认当前选中的 Team 正确。  
2. 准备/确认 API key（有 claim/provision 权限）。  
3. 确认已有可命中的 provisioning rule（例如 tag: `nrf-cloud-onboarding`）。

#### B. 设备准备

1. 烧录 `nrf_device_provisioning`（建议带 `overlay-at_shell.conf`）。  
2. 设备插卡并确保可注册网络。  
3. 串口能看到 provisioning 日志。

#### C. 执行 claim（把设备归属到你的 Team）

示例（按你实际串口和 key 替换）：

```bash
claim_and_provision_device \
  --port <串口> \
  --cmd-type at \
  --api-key <your_api_key> \
  --provisioning-tags "nrf-cloud-onboarding" \
  --id-str nrf- --id-imei \
  --unclaim
```

判据：
- 返回 `Claim device response: 201 - Created` 说明 claim 成功。
- 若返回 404 的 unclaim，不影响，表示之前没 claim 过。

#### D. 让设备在线执行 provisioning

看设备日志：
- `Provisioning started`
- `nRF Provisioning requires device to deactivate network`
- `...activate network`
- `Sending response to server`
- 最终 `Provisioning done`

> 中间多次断开/重连是正常行为，表示在执行需要离线写入的命令（如证书写入）。

#### E. 设备侧验收（最关键）

```at
AT%CMNG=1,16842753
```

必须同时看到：
- `16842753,0`（CA）
- `16842753,1`（Client cert）
- `16842753,2`（Private key）

只要缺任何一个，MQTT 鉴权大概率失败。

#### F. 切回业务应用连接云

烧录 `locationfrsky`，并保持：
- `CONFIG_NRF_CLOUD_SEC_TAG=16842753`
- `CONFIG_NRF_CLOUD_CLIENT_ID_SRC_INTERNAL_UUID=y`（你当前环境已验证需要）

日志验收：
- `nRF Cloud connection ready`

### 10.4 你这次日志如何对应这个流程

你的日志已经覆盖了完整链路：

1. claim 成功（`201 Created`）  
2. provisioning 执行成功（`Provisioning done`）  
3. `%CMNG` 验证有 `0/1/2`  
4. 业务应用中 `nRF Cloud connection ready`

这说明从 Team 归属到设备证书再到 MQTT 已经闭环。

### 10.5 常见失败点与处理

1. claim 成功但设备没拿到证书  
原因：设备端 provisioning 没跑通、时间无效、网络抖动、规则未命中。  
处理：先看设备日志是否到 `Provisioning done`，再查 `%CMNG`。

2. `%CMNG` 只有 type 2  
原因：CA/Client cert 未成功写入（你之前就遇到过）。  
处理：优先使用 provisioning 自动下发，不建议手工 AT 粘贴 PEM。

3. MQTT `-128`  
原因：设备 ID 与云端身份不一致（IMEI/UUID 混用）。  
处理：统一用 UUID（本项目已用 `overlay-cloud-uuid.conf` 解决）。

4. `-116 no valid datetime`  
原因：时间无效导致 provisioning 拒绝。  
处理：先确保 LTE 注册与 `%XTIME` 有效，必要时开 NTP 兜底。

### 10.6 量产落地建议

1. 固化“claim + provisioning + `%CMNG`校验 + MQTT ready”四步产测。  
2. 统一 ID 策略（建议 UUID）并写入开发规范。  
3. 产线不要手工下发证书，统一走云端 provisioning 规则。
