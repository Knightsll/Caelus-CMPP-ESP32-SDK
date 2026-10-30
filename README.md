# Caelus CMPP ESP32-C3 Test Board

ESP32-C3 测试板固件，使用通用 CMPP SDK 暴露 6 路板载 PWM 指示灯和 4 路继电器。
旧版 WS2812 灯带驱动与 Legacy Light MCP 命令路径已经移除。

## 原理图对应关系

| 功能 | 原理图网络 | ESP32-C3 GPIO | 电气逻辑 | 启动/安全状态 |
|---|---|---:|---|---|
| PWM 指示灯 1 | PWM1 | 7 | 高电平点亮 | 0% |
| PWM 指示灯 2 | PWM2 | 8 | 高电平点亮 | 0% |
| PWM 指示灯 3 | PWM3 | 9 | 高电平点亮 | 0% |
| PWM 指示灯 4 | PWM4 | 10 | 高电平点亮 | 0% |
| PWM 指示灯 5 | PWM5 | 20 | 高电平点亮 | 0% |
| PWM 指示灯 6 | PWM6 | 21 | 高电平点亮 | 0% |
| 继电器 1 | DL1 | 3 | 高电平使 NPN 导通、继电器吸合 | 断开 |
| 继电器 2 | DL2 | 2 | 高电平使 NPN 导通、继电器吸合 | 断开 |
| 继电器 3 | DL3 | 1 | 高电平使 NPN 导通、继电器吸合 | 断开 |
| 继电器 4 | DL4 | 0 | 高电平使 NPN 导通、继电器吸合 | 断开 |

PWM 使用 5 kHz、10 bit LEDC，CMPP 归一化范围为 `0..10000`。继电器最短切换
间隔为 100 ms。所有通道上电初始化和 `safe_stop` 都关闭。

GPIO8、GPIO9 和 GPIO2 属于 ESP32-C3 启动绑带相关引脚，GPIO20/21 又是默认
UART0 引脚。固件已把控制台切换到 USB Serial/JTAG，释放 GPIO20/21；但绑带引脚
上的 LED/三极管负载在软件启动前就已经存在，量产前必须实测冷启动、电源斜率和
下载模式，并按 ESP32-C3 数据手册复核外部上下拉等效电阻。

## CMPP 能力

`test_board.pwm`：

- `test_board.pwm.set`
- 参数：`channel` 为 1–6，`output` 为 0–10000，可选 `transition_ms` 为
  0–60000。

`test_board.relay`：

- `test_board.relay.set`：参数 `channel` 为 1–4、`on` 为布尔值。
- `test_board.relay.all_off`：参数为空对象。

继电器命令要求 `safety_class: "reversible"` 和 `requires_ack: true`。两类能力都
支持 `dry_run`。当前硬件没有触点反馈或 LED 电流反馈，因此状态表示驱动器的命令
状态，不代表负载侧物理反馈。

MCP Binding：

- Tool：`cmpp.execute`
- Resources：`cmpp://manifest`、`cmpp://state`、`cmpp://health`、
  `cmpp://limitations`、`cmpp://commands/latest-result`、
  `cmpp://events/latest`
- DNS-SD：`_cmpp._tcp.local`
- MCP endpoint：`/mcp`
- Bearer Token 鉴权：强制启用

完整协议见 [CMPP MCP Binding](docs/CMPP_MCP_BINDING.md)，SDK 架构见
[CMPP SDK 文档](docs/CMPP_SDK.md)，实板部署和配网故障排查见
[测试板部署指南](docs/TEST_BOARD_DEPLOYMENT.md)。

## 构建和测试

```bash
bash scripts/test_cmpp_host.sh
bash scripts/build.sh
bash scripts/flash_monitor.sh
```

串口日志通过 ESP32-C3 原生 USB Serial/JTAG 输出，GPIO20/21 不再用于 UART0。

## 首次配网

未配对设备会同时广播 `Caelus-Board-XXXXXX` BLE 配网服务和同名临时热点。
推荐安装仓库中的 [`caelus-pair-device`](skills/caelus-pair-device/SKILL.md)
Skill，通过 BLE 完成发现、2.4 GHz Wi-Fi 配置、MCP 凭证持久化和只读验证，
主机不需要切换 Wi-Fi。Codex 和 Hermes 使用同一个 Skill 目录。

热点 `http://192.168.4.1` 保留为恢复入口。无论走 BLE 还是热点，客户端都必须
先安全保存 `caelus.mcp.connection.v1`（稳定 `device_uid`、endpoint 和设备独有
的 256-bit Bearer Token），然后发送“完成配对”。完成后设备才停止 BLE/热点
广播并启动 MCP 服务。

BLE 首次使用：

```bash
python3 skills/caelus-pair-device/scripts/setup_ble.py --yes
python3 skills/caelus-pair-device/scripts/caelus_ble.py discover
python3 skills/caelus-pair-device/scripts/caelus_ble.py provision --dry-run
python3 skills/caelus-pair-device/scripts/caelus_ble.py provision --yes
```

当前测试固件使用开发 PoP `caelus1234`；量产必须改为每台设备唯一的制造凭证，
不能沿用这个默认值。

配置完成后可通过以下命令发现设备：

```bash
avahi-browse -rt _cmpp._tcp
```

加入 BLE 后应用镜像约 1.44 MiB，在当前 1.5 MiB app 分区中只剩约 60 KiB。
继续加入 OTA、较大的任务队列或 Stream 功能前，应先调整分区表或 Flash 规格。

## 命令示例

PWM 指示灯 1 调到 40%：

```json
{
  "schema": "cmpp.command.v1",
  "protocol_version": "1.0",
  "command_id": "cmd-pwm-001",
  "trace_id": "trace-pwm-001",
  "target": {
    "device_id": "hw_REPLACE_ME",
    "capability_id": "test_board.pwm"
  },
  "command_type": "test_board.pwm.set",
  "requested_by": "manual-test",
  "reason": "Test indicator channel 1",
  "safety_class": "benign",
  "arguments": {"channel": 1, "output": 4000, "transition_ms": 250},
  "ttl_ms": 5000,
  "idempotency_key": "pwm-test-001",
  "dry_run": false,
  "requires_ack": false
}
```

吸合继电器 1：

```json
{
  "schema": "cmpp.command.v1",
  "protocol_version": "1.0",
  "command_id": "cmd-relay-001",
  "trace_id": "trace-relay-001",
  "target": {
    "device_id": "hw_REPLACE_ME",
    "capability_id": "test_board.relay"
  },
  "command_type": "test_board.relay.set",
  "requested_by": "manual-test",
  "reason": "Test relay channel 1",
  "safety_class": "reversible",
  "arguments": {"channel": 1, "on": true},
  "ttl_ms": 5000,
  "idempotency_key": "relay-test-001",
  "dry_run": false,
  "requires_ack": true
}
```

上述对象作为 MCP `cmpp.execute` 的 `command` 参数提交。
