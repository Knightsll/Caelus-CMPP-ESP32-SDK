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

## 可选 MQTT 双栈

4 MB 及以上 Flash 的产品可以同时保留 MCP，并启用标准
[CMPP MQTT Binding](docs/CMPP_MQTT_BINDING.md)。MQTT 和 MCP 都只负责将
`cmpp.command.v1` 送入同一个 CMPP Runtime；硬件能力、授权后的安全检查、
幂等和执行回执不会分叉。

```text
Agent -> MCP Adapter  --+
                       +-> CMPP Core -> Function Module -> Hardware
Broker -> MQTT Adapter -+
```

双栈构建：

```bash
idf.py -B build-mqtt \
  -DSDKCONFIG="$PWD/build-mqtt/sdkconfig" \
  -DSDKCONFIG_DEFAULTS=sdkconfig.mqtt.defaults build
```

生成的 `build-mqtt/cmpp_test_board.bin` 只能刷入已确认至少 4 MB Flash 的
板子。当前双栈镜像约 1.59 MiB，在 3 MiB app 分区中约剩 47%；该开发分区
没有 OTA 槽位。

## 构建和测试

本项目将专门负责语音采集、传输与播放的设备定义为 **CMPP 语音终端（CMPP Voice Terminal）**：
设备类型 `cmpp-voice-terminal`，接口 profile `caelus.voice-terminal.v1`，能力 `voice.channel`。
AI_REC_v2 是该类别当前的硬件实现；单台设备仍由鉴权后的 CMPP UID 区分。

AI_REC_v2 ESP32-S3 单板语音实验使用独立入口 `bash scripts/build_voice.sh`，
产物在 `build-voice/cmpp_voice_terminal.bin`；复用 BLE 配网和 CMPP，按键上传
PCM 并接收回放。完整协议、盒侧测试工具及未完成的电气/实物检查见
[共同接口文档第 6.7 节](docs/CAELUS_ROOM_VOICE_INTERFACE.zh-CN.md#67-单板实验实现与线协议-v1)。
当前单板已确认有 8 MB PSRAM 和 16 MB Flash；实验固件尚未完成全部实板验收，电气与声学待确认项见共同接口文档。
SW2（GPIO1）用于按键说话。运行中按住 SW1（实际 GPIO17）满 5 秒且不足 12 秒后松开，
可保留设备身份重新进入配网，建议按住 6–8 秒；恢复出厂使用独立的启动维护确认流程。
手势、清除范围与断电恢复见共同接口文档第 6.8、6.24 节。
当前固件提供 GPIO46 状态灯，并通过 `ptt_interrupt_v1` 协商支持播放中按键打断；
盒侧须实现对应协议，未协商时保持半双工。媒体握手、PCM 播放/drained、断连清理已有自动检查，
播放中实板按键打断及最终声学效果仍待验收，详见第 6.26 节。后续 BLE 和配网观察见第 6.27–6.28 节。
0.3.1 已在 D6FD1C 验证 8 MB PSRAM，并将上传队列移入外部内存；命令回执移除大诊断树，
完整诊断仍从 `cmpp://state` 读取。SSE 历史窗口为 8 条，socket 上限为 16，详见接口文档第 6.30 节。

0.4.9-dev 保留自训练“小白小白” microWakeWord 实验模型，保留 SW2/GPIO1 按键、VAD 自动结束和原 CMPP 语音协议。
新增唤醒后连续对话：每轮回复后直接说下一句，配置为安静约 15 秒后重新等待唤醒；新增唤醒提示音、SW4/GPIO48 短按结束当前对话，媒体长连接保留。用户仍反馈重复提醒，尚未通过安静退出验收。0.4.4 已接入 MIC3 参考/AEC 和续听能量门控，实际声学效果待验收；0.4.5 进一步允许续听音节间隙并增加音频缓冲，用户已确认0.4.5连续对话可用；0.4.6修正回声统计对齐及打断门控，0.4.7让播放等待DMA时释放采集所需的锁，已捕获打断进入新轮，但后续录音出现采集溢出；0.4.8提高CPU/编译性能并增加分段耗时，0.4.9增加可调打断响度门槛（初始1000、范围80–8000，空闲设置并持久化）和有限播放队列背压。设置与重启保持已实测，半米到一米的打断灵敏度及稳定性待声学验收；响度门槛不能识别说话人。双方进展见共同接口文档 6.31.6–6.31.11。
模型为 50,888 字节 INT8，采用千问 28 音色语料；独立测试原始正样本 14/16，含增强 49/64，约 9.7 分钟负样本仍有 1 次误触发。**尚未通过真人远场、家庭误唤醒与长稳验收。**
`wake_available` / `wake_status` 可从 `cmpp://state` 检查；当前模型和复现说明见 [wake_models](main/wake_models/README.md)，实际烧录结果与双方联调清单见[共同接口文档第 6.31.3 节](docs/CAELUS_ROOM_VOICE_INTERFACE.zh-CN.md#6313-千问语料自训练与原生-esp-idf-接入2026-11-24)。

下列原构建命令仍用于 C3 测试板，不能将 C3 镜像刷入 S3。

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

默认 MCP+BLE 应用镜像约 1.41 MiB，在当前 1.5 MiB app 分区中只剩约 60 KiB。
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
