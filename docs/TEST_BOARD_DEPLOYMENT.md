# ESP32-C3 测试板部署指南

本文档描述当前第一版测试板固件的实板接线、编译刷写、首次配网、持久化
行为和常见故障。它对应仓库中的 `cmpp_test_board` 应用，不是通用 CMPP
Core API 的替代说明。

## 1. 硬件映射

| 功能 | 网络名 | GPIO | 有效电平 | 启动/安全态 |
|---|---|---:|---|---|
| 指示灯 1–6 | PWM1–PWM6 | 7、8、9、10、20、21 | 高电平点亮 | 0% |
| 继电器 1–4 | DL1–DL4 | 3、2、1、0 | 高电平驱动 NPN、继电器吸合 | 断开 |

PWM 采用 LEDC 5 kHz、10 bit，CMPP 归一化输出范围是 `0..10000`。继电器
切换最短间隔是 100 ms。板级配置位于
[`main/board_config.c`](../main/board_config.c)，功能语义位于
[`components/caelus_device_function`](../components/caelus_device_function)。

GPIO2、GPIO8、GPIO9 是 ESP32-C3 启动绑带相关引脚。外部 LED、三极管和
上下拉在软件运行前就已经接入，必须实测冷启动、下载模式、电源上升沿和
棕断行为。GPIO20/21 已不再承担应用控制台，控制台使用原生 USB
Serial/JTAG。

## 2. 编译和测试

在仓库根目录执行：

```bash
bash scripts/test_cmpp_host.sh
bash scripts/build.sh
```

主机测试覆盖 Core、PWM/Relay Backend 和测试板 Device Function Module。ESP-IDF
构建产物为 `build/cmpp_test_board.bin`。

## 3. 刷写和日志

先确认串口：

```bash
ls -l /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

原生 USB Serial/JTAG 通常是 `/dev/ttyACM0`，外置 USB-UART 通常是
`/dev/ttyUSB0`。使用原生 USB 时：

```bash
ESPPORT=/dev/ttyACM0 bash scripts/flash_monitor.sh
```

刷写卡在 `Connecting...` 时，按住 BOOT，点按 RESET/EN，先松 RESET/EN，
再松 BOOT。`flash_monitor.sh` 只写 bootloader、分区表和应用，不会主动擦除
NVS。

不要把下面的命令用于普通升级：

```bash
idf.py erase-flash
```

它会删除 Wi-Fi、设备身份、Bearer Token 和配对状态。

## 4. 首次配网流程

未完成配对时设备启动：

1. 同时创建 `Caelus-Board-XXXXXX` BLE 服务和恢复热点。
2. 推荐由 `caelus-pair-device` Skill 在 BLE 上建立 Espressif Security 1 会话。
3. 通过标准 `prov-config` 提交 2.4 GHz Wi-Fi，再通过 `custom-data` 设置设备名
   和 hostname，并取得 device UID 和 Bearer Token。
4. Skill 将 MCP 连接信息原子保存到 `~/.config/caelus/mcp-devices.json`，重新
   读取并比对。
5. 只有保存成功后，Skill 才发送 `complete`。
6. 设备释放 BLE 内存、关闭恢复热点并留在家庭 Wi-Fi。

BLE 服务 UUID 是 `021a9004-0382-4aea-bff4-6b3f1c5adfb4`，能力标记是
`caelus_pairing_v1`。首次使用 Skill 需要：

```bash
python3 skills/caelus-pair-device/scripts/setup_ble.py --yes
python3 skills/caelus-pair-device/scripts/caelus_ble.py discover
python3 skills/caelus-pair-device/scripts/caelus_ble.py provision --dry-run
python3 skills/caelus-pair-device/scripts/caelus_ble.py provision --yes
```

没有 BLE 时仍可连接同名热点，在 `http://192.168.4.1` 完成原有网页流程。

仅提交 Wi-Fi 不会完成配对。若客户端没有发送 `complete`，设备下次启动会认为
配对仍处于 pending 状态，重新广播 BLE 和热点，以便恢复同一个 Token。

当前测试板使用开发 PoP `caelus1234`。量产固件必须从制造分区读取每台设备
唯一的 PoP（或升级到 Security 2 的唯一 salt/verifier），并通过机身标签或 QR
码交付；不得继续使用测试默认值。

## 5. 持久化状态

启动日志会打印：

```text
Persistent setup state: wifi=saved|missing, pairing=complete|pending
```

只有 `wifi=saved` 且 `pairing=complete` 时，设备才会直接进入 STA 模式。

持久化内容分为两部分：

- 设备身份和配对状态：NVS namespace `mcp_light`
- Wi-Fi STA 配置备份：NVS namespace `cmpp_wifi`，key `sta_config`

保留 `mcp_light` namespace 是为了让已有测试板刷入新固件后不更换 UID 和
Bearer Token。Wi-Fi 配置在 `/api/configure` 成功后同时写入 ESP-IDF Wi-Fi
存储和 `cmpp_wifi/sta_config`；设备启动时先恢复该备份。

如果日志显示：

```text
wifi=missing, pairing=complete
```

说明配对标志已保存，但 Wi-Fi 配置为空。连接配网热点重新提交一次 Wi-Fi
并完成配对即可；不需要擦除 Flash。

## 6. mDNS 行为

BLE/配网 AP 阶段不发布正式 MCP mDNS 服务。完成配对并连接家庭 Wi-Fi后，设备
才发布：

```text
_cmpp._tcp.local
```

当前固件不会发布 `_caelus-hw._tcp.local`。mDNS 是本地链路发现，只能在同一
局域网/二层网络内工作；跨 VLAN 或跨家庭网络需要 mDNS reflector、网关代理
或其他显式连接方式。BLE 发现不依赖局域网；热点恢复入口始终可以通过固定地址
`192.168.4.1` 访问。

## 7. 启动状态排查

| 日志 | 含义 | 处理 |
|---|---|---|
| `wifi=saved, pairing=complete` | 持久化条件满足 | 应直接连接家庭 Wi-Fi |
| `wifi=saved, pairing=pending` | Wi-Fi 已保存但未完成配对 | 重新运行 BLE Skill，或访问配网页面恢复连接信息并完成配对 |
| `wifi=missing, pairing=complete` | Wi-Fi 配置未保存 | 重新提交 Wi-Fi；不要先擦除 Flash |
| `wifi=missing, pairing=pending` | 全新或已被清空 | 执行完整首次配网 |

如果状态正确但仍无法发现设备，先确认设备和客户端在同一局域网，再检查
UDP 5353/mDNS 是否被访客网络或 VLAN 隔离。mDNS TXT 信息只用于定位候选
设备，不能替代 Bearer Token 鉴权。

## 8. 容量边界

启用 NimBLE、protocomm 和 `network_provisioning` 1.4 后，当前构建镜像约
`0x167f60` 字节（约 1.44 MiB），`0x177000` app 分区仅剩约 `0xf0a0` 字节
（约 60 KiB，4%）。这足以运行首版 BLE 配网，但不适合直接继续叠加 OTA 双分区、
大型证书包或较重的 Stream/任务队列。增加这些功能前先调整分区表或使用更大
Flash，并重新测量运行期最小堆余量。
