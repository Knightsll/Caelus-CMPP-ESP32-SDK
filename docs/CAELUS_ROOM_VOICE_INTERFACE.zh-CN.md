# Caelus 房间语音终端：原理图评审、接口约定与联调清单

创建日期：2026-11-17；最近更新：2026-11-27。源码候选0.5.2-dev按用户要求保持纯按键对讲，结束对话键改为SW1，详见6.31.13–6.31.14；本次未刷板。D6FD1C 已于 2026-11-25 更新到 0.4.9-dev 可调打断门槛实验版（保留自训练“小白小白”模型），保留身份/配网，扩容 app 分区并修正旧引导镜像的 Flash 容量头；板端正/负 PCM 自检通过。用户已确认两轮连续对话和唤醒提示音；0.4.3 仍被反馈重复提醒，不能视为通过安静退出验收，实际记录见 6.31.3–6.31.11。此前 0.3.1 的 PSRAM/资源预算实测见 6.30。单板已配网并完成 LAN 鉴权，固件包含运行中 SW1 重新配网及协商式 SW2 播放打断。真实 PCM 下行、drained 及断连清理已有自动检查；播放中实板按键打断及最终声学验收待完成。基础线协议见 6.7、6.9，后续实现以 6.24–6.26、6.30–6.31 增量为准；BLE 与配网观察见 6.27–6.28，模型按需连接的软件候选见 6.29。下文早期范围和方案保留为历史记录，后续已明确实施的变更以对应增量章节为准；多房间及生产安全方案仍为调研。

这是硬件与软件共同使用的唯一接口文档。硬件任务统一维护本文；软件任务「调研HA替代智能家居方案」（01a1e253-6fa6-7923-9c4d-125db9675fb7）负责服务端软件及其测试。变更通过任务消息确认，不能把建议直接升级为现有要求。

## 0. 当前范围：只做语音通道入口

2026-11-24 用户新增确定要求：同时提供按键说话与语音唤醒，唤醒词为“小白小白”。实现状态、工程默认参数及实机验证边界统一见 6.31.3；下述“唤醒暂缓”描述为此前阶段范围。

后续明确授权扩展：双方协作增加网页CMPP添加向导，以及板端重新配网/恢复出厂。软件产品由Smart Home插件内模块经Core提供，网页负责向导，不依赖Codex技能目录或技能CLI；已有CLI只作为独立联调工具。板端维护操作见6.8，未扩大到云对话、唤醒模型或房间控制。

用户已明确：“这里只想用这个板子作为一个语音通道入口，先不用处理其他内容。”本节优先于下文之前整理的整机规划。

最新实施要求进一步收敛为：“板子可以通过CMPP被自动配网，然后可以连接传输语音。”当前只实现单板的BLE配网、CMPP鉴权/能力发现、准备语音通道、按键录音上传与PCM回放。配网使用本项目已有的CMPP配套BLE流程，不把Wi-Fi密码交互误称为cmpp_core自身能力；已有配网工具自动完成配置和凭据保存，首次仍需可用网络配置、确定目标及相应凭据使用授权。首版不接云模型、网页常驻服务或家居控制；本地唤醒模型、多房间、空间权限仍保留为后续调研，不作为当前闭环前置条件。

本板当前职责仅为：麦克风采音、按键说话、本地语音唤醒、通过 Wi-Fi 向 Caelus 上传音频，以及接收并播放回复。基础配网、连接鉴权和断线清理只服务于这一条语音链路。

首轮验证单块板的“配网 → 盒子连接 → 按键采音上传 → 音频回放”。语言理解、模型调用、个人记忆、权限决策和家居控制由 RK3576 软件侧负责，本板不承担这些业务。此前要求的本地唤醒保留为后续能力，当前实验固件仅提供SW2按键触发。

Zigbee/串口协调器、整机服务部署、家居设备联调、说话人识别、全屋多终端仲裁、屏幕和存储扩展均不在本轮范围。下文相关内容保留为历史调研，不作为当前实施要求或语音原型的前置条件。播放中唤醒、双讲、OTA及续航优化保留为后续项，不先扩展本轮工作。

引脚映射、音频驱动、电源和已发现的输入电压问题仍属于语音板自身的必要核对。小智音频/唤醒能力继续作为复用候选，不要求部署完整小智后端。

最新接入约束：用户要求根据本项目 CMPP SDK 构建方案。因此以现有 CMPP 设备身份、配网及能力模型为主线，第6节已改为 CMPP 语音能力设计。此前的小智整套网络协议/OTA引导仅作调研记录，不再作为默认接入要求。

### 0.1 用户确认的多房间产品目标（后续验收，非现有能力）

2026-11-17用户进一步明确：期望约五个房间的ESP32同时传输音频时，Caelus盒子仍能区分来源房间，并按各自语音指令控制对应房间的智能家居。记录为产品目标；本板仍只承担语音入口，首轮单板验证不变，未据此启动全屋实现。

建议的软件接口约束：盒侧保存已鉴权设备UID与房间的可信绑定，每个终端的语音会话分别关联来源设备、房间及回复通道。房间来源来自绑定关系，不靠声音猜位置，也不直接信任终端任意声明的房间。“关灯”等未指定房间的指令可默认来源房间；明确指定其他房间也不能突破下述session空间权限。来源房间不等于说话人身份或自动取得操作授权；也不意味着每句话都操作该房间的全部设备。手持终端移到另一房间时，原绑定不会自动变成实时位置，需要另定房间更新规则。

用户随后明确的权限要求：盒子按独立session异步处理不同终端的信息，每个session权限局限于其绑定空间。这一要求取代此前“明确指定其他房间可按既有授权跨房间解析”的宽泛建议。设计上有效权限应取既有调用身份/终端授权与session空间范围的交集；绑定房间不自动授予该空间的全部动作权限。服务端从可信配对/配置记录绑定session空间，模型输出、口头指定、唤醒词或终端任意字段均不能自行扩大范围。设备查询和动作执行均应受限，最终执行处核对真实设备归属、动作及当前授权；仅过滤提示词或模型看到的设备列表不足以实现此边界。授权撤销后旧session不能继续沿用权限。具体执行接口由软件侧设计，本轮未实现。

独立session意味着音频、会话上下文、调用结果及回复路由分别归属，并以异步方式避免等待一个房间的整段对话结束才服务另一个房间；不承诺每个共享资源上的操作都能无等待并行。并发性能仍按下述场景实测。不同房间使用不同唤醒词是用户提出的可选触发方案，词表/定制能力待唤醒模型确认；唤醒词不是权限凭据。

后续分别验证五路在线、五路音频传输和五路并发语音处理/控制，不能用网络带宽计算代替RK3576及模型服务并发实测。各房间音频、会话和回复不得串线；同一句话被相邻终端同时唤醒导致重复执行的问题单独评估。上述接口约束是设计建议，具体实现及并发上限待软件任务评审和双方联调。

软件任务于同日只读复核，确认以下现状差距；未启动软件改造：

| 项目 | 当前实现与限制 |
|---|---|
| 房间关系 | `Store`无房间模型，尚无可信终端UID→房间、家居设备ID→房间两套关系。`agent_context.device_catalog`只按已有用户权限筛选设备；`home_control`/`home_apply`按明确`entity_id`操作，尚无房间目标解析。设备名称或fixture中的房间字样不是生产绑定能力。 |
| 会话隔离 | `voice.py`每连接有独立upstream、Protocol、UUID调用命名空间、去重集合和单线程工具执行器，可作为隔离基础；但连接均取`local-admin`、使用同一home-agent客户端，缺少ESP32鉴权/终端身份、房间上下文及面向已鉴权终端的专属回复路由。不能直接让五个终端共享管理员会话。 |
| 并发边界 | `intelligence.py`的`SLOTS=2`只限制`run_message`文本/附件Agent入口，直接Realtime工具路径不经该信号量，不能据此断言语音只支持两路。实时会话虽分别接收音频，`_dispatch`中大量设备/个人操作持有共享`host.gate`，插件RPC也有共享wire互斥；五路收音不保证五路控制无等待。修改常量为5不等于通过并发验证。 |
| 跨终端重复指令 | 现有命令幂等处理同一请求标识的重复；相邻终端生成的不同请求即使文字相同，也属于独立命令。尚无跨终端语音去重，不能靠简单文字相同或固定时间窗吞掉不同用户合法的相同指令。首轮不实现全屋仲裁。 |

后续软件最小边界建议：终端记录保存可信UID和房间，家居设备另有房间归属；每个会话绑定来源终端、来源房间、调用身份及授权上下文，独立管理音频队列、云会话/上下文与回传连接。终端或模型传入的`room`/`user`字段不能替换可信来源。来源房间与目标房间分开，房间默认值由业务解析，Core仍按真实设备ID和动作鉴权；只在提示词中写房间不等于实现路由或权限。多个同类设备或指代不清时，需要明确澄清/分组规则，不能默认控制全部。板端继续只负责采播、按键与唤醒，不识别人或决定家居权限；移动后的绑定更新方式待定。

后续验收对应T11及T12：先模拟五路独立标识、不同音频和指令，验证回复原终端、只命中预期设备、单路断线/云失败不清掉其他路、撤销授权/越权目标、重连迟到回调隔离；补充同一调用身份下不同空间session相互隔离、口头跨房间指令及直接提交其他空间设备ID均被拒绝、查询不泄露范围外设备、房间内未授权动作仍被拒绝。注入慢查询/慢工具，检查不阻塞其他房间音频。随后在RK3576、实际模型服务和设备上验证资源占用、会话/速率限制与响应延迟；模拟不能替代实机，尚未设置或承诺并发上限。这些是后续验收条件，不改变本轮单板范围。

核查依据由软件任务提供：`Caelus_Smart_Home_Dev/apps/smart-home/`下的`voice.py`、`agent_context.py`、`home_tool_schemas.py`、`authority_identity.py`、`intelligence.py`、`store.py`，及`Caelus_Core/products/authority-core/src/native.rs`中的wire互斥。

## 1. 结论与证据范围

AI_REC_v2 具备做房间语音终端的基本硬件：ESP32-S3、双模拟麦克风、ES7210 ADC、ES8311 DAC、NS4150 功放、实体按键、Wi-Fi、USB 与电池电路。按键说话和上电待机时的语音唤醒均具备实现基础；远场距离、播放中唤醒、双讲和续航必须实测。

用户已明确“带 PSRAM”；2026-11-17经esptool实板读取，芯片为ESP32-S3 QFN56 rev0.2，内置8MB PSRAM（AP_3v3），Flash为16MB、eFuse配置3.3V。完整丝印与PCB/BOM仍待核对。图纸 U1 标注 16 MB Flash，这不等于 16 MB PSRAM。当前不建议直接按图进入最终生产：USB 插入检测分压存在明确的设计疑点，具体 PSRAM 型号影响 GPIO47/48 电压兼容性，电源与声学仍需验证。

硬件分工：每房间是 ESP32-S3 语音终端，RK3576 是中心盒子。USB Zigbee 协调器接中心盒子；图中 S3 没有 Zigbee 射频，不能充当 Zigbee 协调器，也不建议用 Zigbee 传语音。

证据：

- [用户原理图，两页](hardware/AI_REC_v2.pdf)，已逐页渲染并放大核对连线。没有 PCB、BOM、网表及实物测量。
- 软件三个实际工作树为 `/home/autograph/Project/Caelus Kernel/{Caelus_Core,Caelus_Official_Plugins,Caelus_Smart_Home_Dev}`，包含未提交修改，不能以旧 HEAD 代替现状。
- 已读取 `voice.py`、`voice_protocol.py`、`web/chat-voice.js`、`web/voice-worklet.js`、插件 `runtime/zigbee.py`、依赖文件及 RK 部署文档；软件任务已通过消息核实本节与接口缺口。
- 本仓库当前固件是 ESP32-C3 PWM/继电器测试板，见 [README](../README.md)。它不是这块 S3 音频板的可用固件，不能直接刷入。

## 2. 原理图已确认的接口

下表按 U4 引脚连线识别；网络名不是 GPIO 号。PCB 网表及实物通断测量仍需复核。

| 功能 | 图纸网络/器件 | 实际 S3 GPIO | 说明 |
|---|---|---:|---|
| 音频 I2C | IO10_SCL / IO11_SDA | 10 / 11 | ES7210 与 ES8311 共用；地址需扫描及寄存器读回确认 |
| 音频时钟 | IO12_MCLK / IO13_SCLK / IO14_LRCK | 12 / 13 / 14 | ADC/DAC 共用 MCLK/BCLK/LRCK |
| 麦克风数据 | IO15_ASDOUT | 15 | ES7210 SDOUT1 经 R16=0Ω；SDOUT2 的 R17 标 NC |
| 播放数据 | IO16_DSDIN | 16 | S3 输出至 ES8311 ISDIN |
| SW2 按键 | IO1 | 1 | 上拉、按下接地；当前固件为按住说话、松开结束采音的PTT键 |
| SW1 按键 | **IO36** | **17** | U4第23脚是GPIO17，不能按网络名配置成GPIO36；0.5.2源码增加运行中短按结束对话；启动维护见6.8，运行中长按重配网见6.24/6.31.14 |
| SW4 按键 | IO48 | 48 | 上拉到3V3、按下接地；0.4.3–0.5.1用于结束对话；0.5.2源码不再读取此键作为结束键；不是音量/配网键；需按具体S3型号核对IO电源域 |
| USB 原生数据 | USB_DM / USB_DP | 19 / 20 | 22Ω 串阻，USB-C 数据与供电；不是现成 USB 声卡 |
| USB 插入检测 | USB_IN_DET | 39 | U4 MTCK；见下一节分压问题 |
| 音频电源使能 | ADC_PW | 47 | 控制 U8 EN；需核实 1.8V/3.3V IO 与 EN 门限 |
| 电源保持 | SYS_PWR | 38 | 驱动 Q3，自锁供电须固件及时接管 |
| 电源键检测 | PWR_EN / SW3 | 18 | 与电池开机电路关联，不宜直接复用为 PTT |
| 电池测量/充电状态 | BAT_DET / CH_DET | 2 / 9 | 电池分压受 Q6 及供电状态影响 |
| 功放使能信号 | MUTE | 21 | 用户已明确确认连接NS4150 CTRL；高电平开启，本轮固件pa_pin=21，GPIO输入读回1；听感待确认（6.19） |
| microSD | SCK / D0 / CMD | 40 / 41 / 42 | 当前连法可考虑 1-bit SD；语音首版非必需 |
| LCD | CS/SCK/BL/RST/DC/MOSI | 3/4/5/6/7/8 | 屏幕具体型号未标；语音不要求先启用屏幕 |

音频路径：

1. 两只模拟麦克风分别进入 ES7210 MIC1/MIC2。
2. ES8311 OUTP/OUTN 一路经耦合进入 NS4150，再到 PK1 喇叭。
3. OUTP/OUTN 另一路经 R18～R25、C83～C89 等进入 ES7210 MIC3，是 DAC 输出的回声参考。
4. MIC4 未接第三只外部麦克风。不能把“四通道 ADC”解释成“四麦阵列”。

这条参考取自功放之前，不能反映功放削顶与扬声器全部非线性。具备 AEC 接线条件不等于 AEC 已生效。只有一条 SDOUT1 数据线，双麦加参考需匹配 ES7210 的 TDM/时隙配置、驱动通道映射和 AFE 输入排列。

## 3. 上电前及改板前必须核对

### H01：USB 检测分压（高优先级，图纸计算）

第2页 R56=10kΩ 接 5V，R57=47kΩ 接地，USB_IN_DET 连接 GPIO39。忽略钳位与输入漏电，检测点为 `5 × 47 / (10 + 47) ≈ 4.12V`。D7 标 6.2V，不能把该点限制到 3.3V GPIO 的允许范围。[S3 数据手册，DC characteristics](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf) 给出的输入高电平上限为对应 VDD+0.3V。

处理要求：先核实 BOM/网表是否与 PDF 相同；相同则重算分压或改电平检测电路，覆盖 USB 电压与阻值误差、MCU 未供电时倒灌及输入门限。不要仅通过软件忽略这个脚。实板限流上电后测 T1/USB_IN_DET 波形。

### H02：PSRAM 型号和 IO 电压（实板已识别8MB AP_3v3，电压测量待完成）

GPIO33～37 在八线存储配置下被占用；本图这些 MCU 引脚没有接 SW1，SW1 的 IO36 网络实际落在 GPIO17，**没有证据支持“SW1 占用 GPIO36”这一判断**。

真正需要核对的是：S3R8V/S3R16V 等型号的 GPIO47/48 可处于 1.8V 域，而本图 SW4 上拉至 3.3V，GPIO47 又驱动音频 LDO EN；Flash 还由 VDD_SPI 供电。必须按完整型号、Flash 电压、eFuse 与引脚电源配置一起核对。不能只写“带 PSRAM”后套用 N16R8 固件。[官方数据手册第2章](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf)

### H03：电源、充电与掉电（电路存在，性能未验收）

- HTM9016 充电、Q2/Q4/D18 电源切换、SY8089A 降压及 U8 音频 LDO 均已画出；没有电池容量、放电能力、保护板/温度检测与充电电流的完整依据。
- U8 同时列出 WL2801E-30、ME6211/3v3、CL9195A33L5M，实际型号、输出电压和 EN/BP 引脚兼容性需 BOM 固定。
- 测 USB/电池切换、Wi-Fi 发射+最大音量瞬态、低电量欠压复位、充电温升与音频噪声；降压电路不能在电池接近或低于目标电压时凭空维持 3.3V。
- BAT_DET 下臂通过 Q6 接地，不能把它当作始终有效的固定二分压；要测关机时 BAT_DET 是否向未供电 MCU 倒灌，并验证 CH_DET 的实际逻辑电平。
- NS4150 CTRL 当前由 3V3 电阻网络控制，未看到 GPIO 独立关闭功放的连线。若要求低待机电流及无开关机爆音，需验证现电路或增加可控 PA 使能。

### H04：唤醒与声学边界

WakeNet/AFE 运行时需要 MCU、麦克风与 ADC 工作。此图没有独立的常开语音唤醒芯片；断电关机或关闭音频前端后，不能继续靠语音唤醒。电池省电 PTT 模式与常听模式需要分别定义和测续航。

建议固定摆放时常供电监听，拿起时保留 PTT；这只是产品建议。深睡/关机后由哪个键唤醒、是否先开机再按 PTT，需单独明确，不能承诺所有按键都能唤醒。

远场效果依赖麦克风实际型号、间距、开孔、密封、手持摩擦与喇叭隔振。至少测安静/电视声/风扇声、不同方向距离、播放中唤醒和双讲。未实测前不承诺“全房间无死角”或固定距离。

GPIO0/3/45/46 启动配置、J1/J2 外接电路与恢复下载路径也需检查；USB 已有数据线并不证明坏固件可自动恢复。

## 4. 开源方案调研与适配判断

检索日期为 2026-11-17。以下是项目能力与适配判断，不代表本板已经获得上游支持；未用论坛转述代替官方代码。项目主分支会变化，实施时锁定版本。

| 方案 | 适配本板的依据 | 接 Caelus 的工作 | 判断 |
|---|---|---|---|
| [小智 xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) | ESP32-S3、ESP-SR 唤醒、Opus、按键/电源管理；BoxAudioCodec 已含 ES8311+ES7210 | 可参考音频/唤醒实现；设备接入遵循本项目 CMPP SDK | **音频实现参考候选**，不预设整套小智协议；查询时仓库约33.3k stars，非质量验收指标 |
| [ESPHome Voice Assistant](https://esphome.io/components/voice_assistant/) + [microWakeWord](https://esphome.io/components/micro_wake_word/) | 官方有 [ES7210](https://esphome.io/components/audio_adc/es7210/) 与 [ES8311](https://esphome.io/components/audio_dac/es8311/) 组件，支持 PTT/本地唤醒 | 标准语音流程连接 HA；去 HA 需要实现兼容接收端/另写传输 | 芯片适配好，现架构下不是直接可用方案 |
| [ESP-SR AFE](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/audio_front_end/README.html) + [esp_codec_dev](https://github.com/espressif/esp-audio-dev/tree/main/esp_codec_dev) | 官方 ADC/DAC 驱动、降噪、VAD、WakeNet、AEC | 自行完成配网、会话、传输、OTA与产品状态 | 更可控的长期底座；工程量高于复用小智，组件许可/模型资源另核对 |
| [Linux Voice Assistant](https://github.com/OHF-Voice/linux-voice-assistant) / [Wyoming Satellite](https://github.com/rhasspy/wyoming-satellite) | Linux 语音卫星生态；前者支持 ARM64，后者已归档 | 可研究 RK 直连音频场景；不是 S3 固件，前者仍面向 HA 协议 | 不作为本房间板首选；不要把归档项目列作新主线 |

[xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server) 是可自部署的小智后端，可用作隔离的硬件音频对照验证。它不是 Caelus 现有组件；不建议为接语音板直接再部署整套重复 Agent/权限/记忆服务。小智设备端的 MCP 功能也不等于已经兼容 Caelus Core 授权或连续音频协议。

当前候选快照：`78/xiaozhi-esp32@aaf1eb7798cabee87a0c6a0649aabf4d17e9e383`（GitHub API 查询，提交时间 2026-11-16 UTC）。这是调研/原型基线，尚非用户选定的生产版本，未编译、烧录或兼容性验收。

- [固定版本音频驱动](https://github.com/78/xiaozhi-esp32/blob/aaf1eb7798cabee87a0c6a0649aabf4d17e9e383/main/audio/codecs/box_audio_codec.cc)：当前默认输出给应用的是单麦，或单麦+参考；不是自动利用本板两只麦克风。代码注明 TDM 次序涉及 MIC1/MIC3/MIC2/MIC4，须逐通道实测，不能照搬通用 MMR 排列。
- [固定版本依赖](https://github.com/78/xiaozhi-esp32/blob/aaf1eb7798cabee87a0c6a0649aabf4d17e9e383/main/idf_component.yml)：esp_codec_dev `~1.6.2`、esp-sr `==2.4.7`。不要把最新版 codec v2 API 混入此快照；IDF 具体版本随候选构建锁定。
- ADC/DAC 共用时钟，codec 实现要求相同的硬件采样率；云侧16/24k混合不能直接分别配置两只 codec。建议首版本地 AFE/I2S 16k，云侧重采样在 RK；下行若选24k，需要明确在 RK 输出16k还是端上转换。
- 自定义“Caelus”中文唤醒词需模型及效果验证；不能假定修改字符串就能得到可用唤醒模型。

## 5. 已核对的软件现状与 RK 条件（整机部分留档）

软件任务已回复确认下表。其测试主要在 x86_64，不能外推为 RK3576 通过。

| 项目 | 当前实现/实际条件 | 未确定或缺口 |
|---|---|---|
| 身份与控制 | Core 管身份、客户端、授权、工具执行与回执；设备动作仍经 Core | 当前 Core 没有媒体流授权 API；不能声称已有音频租约 |
| 语音登录 | 浏览器 Cookie + Origin + 首帧 CSRF；当前取 local-admin 身份 | 没有独立终端凭证/配对/room/说话人认证，不能借管理员 Cookie 接 S3 |
| 音频 | JSON/base64 PCM16；千问输入16k/100ms，另一现有桥输入24k/20ms，输出均24k | 这是桥的现配置，非所有服务商硬性帧要求；不是小智二进制 Opus |
| 会话 | 入站 audio/close；千问 server_vad，speech_started 转 interrupt | 无 PTT 松开提交/取消，不能拿 close 代替语段提交；无跨断线恢复 |
| 麦克风/扬声器 | 现网页由浏览器 getUserMedia/AudioContext 采播 | RK 没有已实现的本地 ALSA 采播入口；本架构不必要求 RK 本地声卡 |
| Zigbee | 独立 ZHA 2.4.3 / zigpy 2.4.1，znp/bellows/deCONZ 相关依赖；本地字符设备 | 不是 Zigbee2MQTT；实际协调器型号、协调器固件、USB桥均未选定 |
| 串口 | `CAELUS_ZIGBEE_SERIAL`、`CAELUS_ZIGBEE_RADIO`、`CAELUS_ZIGBEE_BAUDRATE`；默认znp/115200 | 默认值不是采购要求；未显式传流控，已安装 ZHA 默认 `hardware`，需结合 RTS/CTS 实物冻结 |
| 恢复 | 监督器管理子进程，systemd 重启整组；未知动作不可盲目重放 | Zigbee 初次初始化失败会退出线程，当前无外层拔插重启入口；运行期 SDK 重连不能覆盖所有故障 |
| OS | aarch64 Linux、Python3.12+、systemd 用户 scope、D-Bus/cgroup、user namespaces、bubblewrap/util-linux | RK BSP、内核选项、libc与 ARM64 依赖需上板验证；不能复制 x86二进制 |
| 网络 | 模型与品牌服务需对应外网；终端需要2.4GHz Wi-Fi连接局域网 | 盒子可用以太网或 Wi-Fi；AP隔离、DNS/校时/TLS、证书发放、丢包需验收 |

硬件需提供至少一个可稳定供电的 USB Host 接口给协调器；若还有存储/调试外设，数量与 Hub 预算单独核算。协调器需刷 Zigbee coordinator/NCP 对应固件，不能把 router、Thread RCP 或多协议固件视为等价。

Linux 按实际 USB 芯片启用 `cp210x`、`ch341`、`cdc_acm` 或对应驱动，不要求三者全部存在才可运行。优先 `/dev/serial/by-id/...`；无唯一序列号时明确 udev 稳定别名。实际串口所属组/ACL授予服务用户读写权限，不用 `chmod 777` 代替部署。RK 板载 UART 则另查 DTS/pinmux、电平、serial-console占用和 RTS/CTS，不能直接套 USB 配置。

若将来采用 USB Audio Class 固件，RK 才需 `snd_usb_audio`、ALSA 稳定声卡选择与音频权限，并由软件任务新增对应入口。当前 USB 只是可用硬件能力，尚无 UAC 固件或 `/dev/snd` 接入要求。

部署依据：[RK3576部署文档](</home/autograph/Project/Caelus Kernel/Caelus_Smart_Home_Dev/apps/smart-home/docs/RK3576_DEPLOY.zh-CN.md>)、[用户服务模板](</home/autograph/Project/Caelus Kernel/Caelus_Smart_Home_Dev/apps/smart-home/deployment/caelus-home.service>)。

- 无登录自启需用户 linger、用户 D-Bus/cgroup 可用；服务 `Restart=on-failure`，重启间隔5秒。启动失败、启动时没网络/没USB需另测。
- 网页默认 localhost:18890，开发 LAN HTTPS为18894，对应实时端口为网页端口+10；**这些不是新终端端口约定**。开发 CA 需安装信任，不能把它当量产自动可信证书。
- 持久数据：`$XDG_STATE_HOME/caelus/{authority-home,smart-home-native,smart-home-native-services}`（默认 `~/.local/state`）；Zigbee DB 在第三项。身份、品牌授权、个人记忆、命令去重/unknown回执必须跨断电保留。
- ZeroClaw 安装器遵循 `$XDG_CACHE_HOME/caelus`（默认 `~/.cache`）；监督器的适配器 Python 默认仍为 `~/.cache/caelus/smart-home-native/venv/bin/python`，可用 `CAELUS_ADAPTER_PYTHON` 覆盖，不能假定所有依赖均随 XDG_CACHE_HOME 自动移动。离线启动依赖这些文件，不能因名为cache就随意清空。审计/记忆等尚无全局空间上限，存储容量和告警待定。

## 6. 以现有 CMPP SDK 接入盒子的设计

6.1～6.6保留实现前的核查和设计依据，表内“已有/缺失”描述的是该次核查时的代码。当前实验实现及双方具体对接约定以6.7为准；它不是既有CMPP媒体标准或实机交付证明。只实现单块板的语音通道。CMPP 定义设备能力及控制语义，MCP 是已有传输绑定；音频连续数据通过受同一设备会话约束的媒体通道传输。

### 6.1 可直接复用与真实缺口

| 层 | 本项目已有能力 | 语音板需要新增/适配 |
|---|---|---|
| CMPP Core | 注册、校验、TTL、串行执行、回执、状态版本、内存幂等缓存 | 不加入I2S/网络分支；通过公开API注册语音Function Module |
| 平台 | FreeRTOS、NVS、时钟、分配器等 | S3构建和内存预算，板级音频初始化 |
| 应用身份/配网 | `main/device_identity.c`稳定UID及Bearer、`network.c` BLE/SoftAP配网与完成标志 | 复用流程；这是应用层而非cmpp_core提供的功能 |
| 设备发现/控制 | `_cmpp._tcp.local`，板端`/mcp`，鉴权后读`cmpp://manifest`及调用`cmpp.execute` | 从测试板改为语音profile/设备描述；保持通用适配器按描述符枚举 |
| 音频数据 | 当前没有音频backend或媒体帧协议 | ES7210/ES8311驱动、音频任务、有界缓冲和媒体绑定 |
| 盒侧连接 | 旧指南描述过Pairing Plugin/Board Connector | 软件任务确认当前精简分支无板端发现、HTTP MCP客户端、manifest注册读取或凭据导入路径；需补最小连接器 |

依据：[SDK](CMPP_SDK.md)、[公开API](../components/caelus_cmpp_core/include/cmpp/cmpp.h)、[MCP绑定](CMPP_MCP_BINDING.md)、[Function Module规则](CAPABILITY_MODULE_GUIDE.md)、[配网协议](../skills/caelus-pair-device/references/pairing-protocol.md)。

现有Core没有二进制音频类型、媒体租约或通用设备事件发布API；`cmpp_event_callback_t`收到的是命令响应。MCP通知为best effort，只保留latest，因此不能用它可靠传递PTT按下/松开或每帧录音。`streamable_http`是MCP传输名称，不表示已有实时音频流。

软件任务补充核实：盒侧`smart_setup.py`当前只接tuya/zha/xiaomi_miot；UI或schema里的cmpp字样不是已实现的接入。Authority Core的`native/installation.rs`加载签名本地stdio进程，要求`server/discover`及其插件契约；不能直接把ESP32 HTTP设备当作该本地插件。设备端SDK与盒侧Rust包都叫CMPP，并不意味着线协议相同。现阶段只能复用**设备端已有UID/Bearer体系**，盒侧仍需新增安全配对记录/凭据导入，不能声称盒子已经持有板端凭据。

### 6.2 最小分层

```text
Caelus盒子：CMPP连接器 ── MCP控制/状态 ── 板端MCP Adapter
                                                │
                                           CMPP Runtime
                                                │
                                      语音Function Module
                                                │
Caelus盒子：语音桥    ══ 双向音频通道 ══ 媒体适配器 ── 音频任务
                                                           │
                                                ES7210 / ES8311
```

应用装配层配置实际GPIO和codec，Function Module通过 `cmpp_runtime_register_capability()` 注册；音频任务负责采播及本地唤醒。网络对象、WebSocket和鉴权留在适配器，Function Module接收传输无关的参数。不修改通用MCP适配器去硬编码语音命令，不另造第二套CMPP执行路径。

### 6.3 语音能力草案

建议新profile为 `caelus.voice-terminal.v1`，先注册一个 `voice.channel` 能力。名字均为待双方冻结的草案。

| 操作 | 语义 | 回执边界 |
|---|---|---|
| `voice.channel.open` | 为指定会话准备音频格式、有限缓冲及本地触发许可；不因远端open自动上传麦克风 | completed/committed仅表示准备状态已建立；不等于云模型就绪或录音完成 |
| `voice.channel.close` | 禁止该会话继续上传/播放，清旧缓冲并释放通道 | 只有确认停止后才回完成；无法确认时如实报错 |
| `observe` | 读取准备/录音/播放/故障及格式等真实本地状态 | 使用已有`cmpp://state`，不推断盒端模型或家居状态 |
| `safe_stop` | 立即回到停止上传与播放的安全状态，可重复调用 | 不把runtime shutdown钩子误当作网络掉线自动处理；掉线需显式触发本地关闭 |

`execute`只做有界的准备/关闭操作，不能持有CMPP串行执行锁跑完整段对话，也不能在事件回调里重新dispatch。同一命令的缓存回执不能证明通道现在仍活着；重连先读当前状态，再创建新会话。状态版本只代表当前Core定义的命令提交，不自动覆盖每次本地按键边沿。

### 6.4 连接和传音流程

1. 板子沿用现有配网流程取得家庭Wi-Fi，保留稳定UID和板端凭据；盒子安全保存配对记录后才完成配对。板端pairing标志不能证明盒侧连接器已安装或授权。
2. 盒子根据mDNS定位候选，用已存凭据连接板端MCP；核对UID、profile及语音能力。mDNS不是身份认证，也不能用其TXT覆盖可信凭据。板端MCP版本沿用本SDK实际绑定，不套用Core内部协议版本。
3. 盒侧可信应用在授权范围内发`voice.channel.open`。媒体端点的位置/绑定版本需另外约定，不能假定现有manifest已包含自定义媒体字段；凭据不进入命令arguments、回执或普通日志。
4. **双方认可的原型候选：盒子主动连接板端WebSocket媒体端点**，延续现有“盒子访问已配对设备”的方向；待盒侧完成配对记录导入后，复用该板身份/凭据来源。现有板端服务器尚无媒体端点，需新增。端点同时验证有效会话，仅允许当前绑定连接；端点发现字段、握手和安全方式尚未冻结。不能仅凭mDNS把Bearer发送到任意地址。不是默认让板子连接小智服务器。
5. 用户PTT或本地唤醒开始语段。开始、末帧及结束消息通过同一个有序媒体会话处理；不依赖best-effort MCP事件。松键表示结束并提交该段，取消录音与停止播放另行区分。有限预录缓冲避免丢首字，未就绪时不得无限积压。
6. 盒子把音频送入现有语音服务，将回复经同一媒体会话返回；板子只播放。分别记录数据收齐与硬件缓冲播完，不能用命令成功代替实际播完。
7. 断线清除旧会话及音频，停止上传/播放；连接恢复后重新鉴权和准备，不补发旧录音。板重启后幂等缓存不存在，旧会话无效；保留UID不意味着恢复旧录音。

本地唤醒检测本身需要麦克风采样；“open不自动上传”不禁止本地唤醒前端运行。媒体或云未就绪时需明确提示何时可以讲话，不能静默丢首字。松键只结束当前语段，不等于`voice.channel.close`；队列容量应在实现前以毫秒/字节列明，慢消费者/超限显式处理，不任意丢帧或无限积压。尾音不足一帧时的长度或补齐规则仍需在线协议中明确。

当前 `/mcp` 是局域网HTTP+Bearer，**未加密**，不提供服务端密码学身份保证；不能因新增媒体端点便宣称已实现WSS/双向认证。控制与媒体都需统一确定加密和首次信任方案，不能只加密音频而把共用Token留在明文控制通道。方案冻结前不部署为安全完成态。

### 6.5 音频格式与验收

首版建议双向PCM16LE、单声道、16kHz、20ms/帧，即每帧640字节。双向纯PCM合计约64kB/s，不含协议开销；这是候选参数和带宽计算，不是无线性能实测。RK负责云侧16/24k重采样，本板ADC/DAC使用同一硬件时钟。Opus在有实测带宽需求时再评估，不因小智使用它就要求本板首版必须采用。

软件任务认可上述PCM参数用于原型：当前千问桥可累计5帧形成100ms输入，现有24k/20ms桥需重采样，云端24k输出转16k下发。实际供应商语段提交/取消映射由软件侧核对并实现。硬件时钟、采播效果与缓冲验收后才能冻结这些参数。

下一步冻结媒体端点/帧与语段边界、连接鉴权/首次信任、PCM格式、最大缓冲及错误行为。只验证“配对 → 发现并核验manifest → 准备通道 → 按键/唤醒 → 收发音频 → 停止/重连”。此时不做家居控制、说话人识别、全屋仲裁或完整小智后台。

### 6.6 空闲连接生命周期（检索建议，尚未冻结/实现）

2026-11-17针对用户关于常驻或唤醒后建连的提问，建议首版常听模式保持板子与盒子的局域网WebSocket；未触发时只有必要的保活/状态消息，不上传环境音频。本地唤醒前端仍采样。PTT或唤醒后使用已有连接开始语段。语段/对话结束通常不关闭LAN通道；显式`voice.channel.close`、停用、撤销授权、服务关闭及连接故障按各自生命周期清理。云模型会话可按需建立，优先在明确结束对话后释放；空闲自动释放的触发语义、时长及供应商行为仍待确定。本节经软件任务只读复核，双方认可候选方向，不代表已经实现或冻结。

软件侧确认的现状：`Caelus_Smart_Home_Dev/apps/smart-home/voice.py`的`handle`在浏览器鉴权后立即创建云连接，`receive.finally`在云端结束或异常后关闭浏览器连接，浏览器退出也关闭云连接。当前没有“LAN保持、云会话释放/重开”、云空闲计时器或终端退避重连。最小软件差异是分离终端传输连接与一次云对话的生命周期；供应商事件映射和工具桥可供复用，现有`handle`不能直接作为常驻终端处理器。此为软件任务对`voice.py`及`voice_protocol.py`的核查结论，本轮不修改实现。

语段结束、模型一轮`response.done`和完整对话结束是不同事件：松开PTT不立即销毁云会话，多轮可继续复用；模型完成不代表扬声器已播完。释放云会话前需核对待提交输入、模型回复、工具处理及待发送音频，不能只用“最后一块音频的时间”计空闲。已提交的设备动作不因通话结束被视为取消，也不得重放。云会话释放后，临时上下文不会因此获得自动恢复保证；本轮不增加个人记忆功能。

- 依据现有候选方向，由盒子主动连接板端并负责断线后的退避重连。心跳属于WebSocket传输存活机制，与CMPP设备状态分开。当前浏览器服务`ping_timeout=None`、千问上游显式`ping_interval=None`，不能直接沿用为终端半开连接检测策略；LAN保活、故障检测及退避参数待测，不从RFC推导统一秒数。重连恢复传输/鉴权，重新读取状态并准备新会话，不自动补发录音或重放CMPP写命令。
- 云故障不必关闭仍健康的LAN连接，可通过它反馈错误和就绪提示；LAN断线则必须停止旧语段、清理旧会话音频，恢复后不补发。旧连接或旧轮回调不得进入新语段。
- LAN连接已建立不等于云端ready，首次或空闲后的唤醒仍可能等待云建连。需要明确可讲话提示；如果支持即唤即说，应在实现前确定有限预录缓冲的容量和溢出行为，不能无限缓存或静默丢首尾字。
- 如果改成空闲断开、唤醒才建连，必须补充板端向盒子发送可靠唤醒通知的路径，或明确改为板端主动连接盒子。当前CMPP的latest/best-effort事件不能直接承担此保证；不能假定关掉连接后盒子自然知道按键发生。
- 电池纯按键模式可另评估深睡后按键唤醒、重新联网建连；不作为当前默认。关闭WebSocket、断开Wi-Fi、进入深睡是三个不同操作。该原理图无独立常开唤醒处理器，S3深睡时不能继续运行本地语音唤醒。Wi-Fi Modem-sleep可维持关联，但本板运行AFE时的延迟和整机功耗仍需实测。
- 联调补充并入T06/T12：空闲无音频上传、长时间空闲后首句、连续多轮对话、断网但socket未报错、云掉线而LAN保留、长回复/工具处理中不被误判空闲关闭、云重开前ready提示、重连后旧回调隔离、盒子及板子重启。记录首尾字完整性和唤醒到可讲话延迟，均无实测通过结论。待机电流和续航另由硬件实测，不根据“常驻WS”单独推算。

外部依据：[小智WebSocket文档](https://github.com/78/xiaozhi-esp32/blob/aaf1eb7798cabee87a0c6a0649aabf4d17e9e383/docs/websocket.md)描述唤醒/按键打开音频连接，结束会话时关闭；[ESPHome Native API](https://esphome.io/components/api/)是自定义TCP API、通常由主机连接设备，不能称作WebSocket实现；[RFC 6455 §5.5.2](https://www.rfc-editor.org/rfc/rfc6455.html#section-5.5.2)提供Ping/Pong保活机制，没有统一的应用空闲断开秒数；[ESP-IDF S3低功耗说明](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/low-power-mode/low-power-mode-wifi.html)区分维持Wi-Fi的Modem-sleep和断开Wi-Fi、CPU关闭的Deep-sleep。

### 6.7 单板实验实现与线协议 v1

#### 实现范围与文件

板端通过`scripts/build_voice.sh`单独构建ESP32-S3，配置在`build-voice/sdkconfig`，依赖锁为`dependencies.voice.lock`，不覆盖C3配置。产物是`build-voice/cmpp_voice_terminal.bin`；此镜像当前使用保守的2MB分区布局（图纸标16MB Flash并不自动采用），无OTA槽。首版禁用PSRAM且不依赖它；之后实板识别为8MB AP_3v3，尚未启用或进行内存压力测试。

| 文件 | 职责 |
|---|---|
| `main/voice_board.c` | 注册`voice.channel`、验证有限参数、执行准备/关闭、提供状态和回执 |
| `main/voice_media.c` | 单媒体连接鉴权、会话生命周期、有界采播队列与任务、网络异常清理 |
| `main/voice_audio.c` | AI_REC_v2模拟电源GPIO47、SW2 GPIO1、I2C/I2S及ES7210/ES8311；音量/增益可调 |
| `main/voice_recovery.c` / `voice_recovery_logic.c` | GPIO38电源保持、SW1启动维护窗口、操作意图持久化与中断恢复 |
| `main/voice_wire.c` | 可在主机测试的JSON/PCM语段边界校验 |
| 原`device_identity.c`/`network.c`/CMPP适配器 | 继续复用身份、BLE/SoftAP配网、NVS、鉴权MCP和发现；未另造配网协议 |

音频依赖固定`esp_codec_dev 1.6.2`。首版选择ES7210 MIC1作为网络单声道源，芯片使用MIC1/MIC2标准双槽配置，ADC/DAC均16k/16bit双槽时钟；下行mono复制到DAC两个槽。MIC3参考、AEC、双麦处理与唤醒算法未启用，MIC1实际槽位/幅度仍待实测。初始化增益24dB、音量35%仅为可调整的试验起点，DAC/PA增益模型也需按实际供电校准。GPIO38保持电源、GPIO47开启模拟电源；GPIO39 USB检测未使用，但不用该GPIO不能消除原理图过压风险。未核对H01分压、GPIO47电压域及实物料号前，不应按编译成功直接接USB烧录。

#### BLE能力文档与可选端点（按当前板端SDK生成器核对）

当前`main/network.c`用`network_prov_mgr_set_app_info("caelus", "1.0", ...)`注册应用信息。`manager.c`的`app_info_write_entry`/`network_prov_write_info_json`将应用与官方配网能力分别输出，示例：

```json
{
  "caelus": {"ver": "1.0", "cap": ["caelus_pairing_v1"]},
  "prov": {"ver": "netprov-v1.2", "sec_ver": 1, "cap": ["wifi_prov", "wifi_scan"]}
}
```

- `caelus.cap`用于检查应用配对协议；`prov.cap`里的`wifi_prov`用于决定是否发送标准Wi-Fi配置；`prov.sec_ver`须与当前Security1实现相符。不能从`prov.cap`找`caelus_pairing_v1`。
- 保存Wi-Fi但尚未complete的SESSION_ONLY恢复模式，`prov.cap`不含`wifi_prov`，GATT中也**没有**`prov-config`。连接时只要求常驻端点，通过真实描述符取得`custom-data`等映射；读取能力后才按需要求`prov-config`。不能把`prov-config`列成所有连接都必需的端点，否则连接阶段就会失败，根本到不了跳过Wi-Fi的恢复逻辑。
- `prov.ver`是SDK配网文档版本，不等于CMPP版本或依赖包版本；示例可能含额外`sec_patch_ver`等字段，不应要求逐字节等同。

网页部署后的只读交叉审查发现：产品`cmpp_ble.py`及此前技能CLI均存在混用能力层级、连接时强制要求`prov-config`的问题；旧模拟fixture复现的是客户端假设，不能证明与板端兼容。软件任务已修复工程内产品模块：分别读取`caelus.cap`/`prov.cap`，检查Security1，连接必需端点仅`prov-session`/`proto-ver`，通过实际描述符确认`custom-data`和按需的`prov-config`。硬件任务已只读核对修正源码。新增测试使用实际Transport_BLE与BLE_Bleak_Client、模拟Bleak外设，覆盖首次配置、中断恢复及不兼容能力/安全版本/缺失端点拒绝。软件任务报告开发机和RK3588共用的7项CMPP测试均通过，修正源码已同步盒侧；这不是实板BLE验收。旧技能CLI本轮未修复，不再作为产品依赖，也不能在修复前作为可用的BLE联调入口。

#### 控制与媒体绑定

- BLE配对沿用已有流程：发现`Caelus-Voice-*` → Security 1配置Wi-Fi → 保存并回读UID/Bearer → complete。之后发布`_cmpp._tcp.local`和`/mcp`（默认8080）；原生manifest profile为`caelus.voice-terminal.v1`，capability为`voice.channel`。原C3测试profile保留。
- `voice.channel.open`：arguments为`{}`，safety_class为`reversible`、requires_ack为`true`。只准备资源，未按键不上传音频。板生成随机32hex `channel_id`。已有通道或旧连接尚未清理时返回冲突，不抢占。
- `voice.channel.close`：arguments为`{"channel_id":"本次ID"}`。匹配当前通道才关闭；WS已断开且ID匹配最近关闭通道、停止已确认时返回`completed/no_effect`。有新通道时旧ID不能关掉新通道。重启不保留这些临时ID；停止硬件输出失败如实报告失败/unknown。
- 两操作支持dry_run，测试不产生新通道或关闭效果。状态通过已有CMPP资源读取；命令回执缓存不代替当前通道状态检查。

成功open的原生`cmpp.command-result.v1`回执中，`result`对象示例（MCP包装为`structuredContent.result`，不是`result.data`）：

```json
{
  "channel_id": "00112233445566778899aabbccddeeff",
  "ws_path": "/voice", "ws_port": 8081,
  "format": "pcm_s16le", "sample_rate": 16000,
  "channels": 1, "frame_ms": 20,
  "ready": false, "recording": false, "playing": false, "awaiting_response": false
}
```

盒子只使用已配对MCP端点的同一主机及此回执端口，连接`/voice`；HTTP升级前验证同一个Bearer，拒绝第二连接。随后盒发`{"type":"hello","protocol":"caelus.voice.v1","channel_id":"本次ID"}`，板回同protocol/channel_id的`type:"ready"`。channel_id不是替代Bearer的凭据；不放Token到URL。未完成hello不能传PCM。媒体端点使用独立HTTP server，不修改通用MCP Adapter。

#### 音频帧、触发与结束

| 方向 | 有序消息 |
|---|---|
| 板→盒 | `{"type":"input.start","turn_id":"in_1"}` → 二进制PCM帧 → `{"type":"input.end","turn_id":"in_1"}` |
| 盒→板 | `{"type":"output.start","turn_id":"盒侧唯一ID"}` → 二进制PCM帧 → `{"type":"output.end","turn_id":"相同ID"}` |
| 板→盒 | 输出队列及I2S尾部排空后，`{"type":"output.drained","turn_id":"相同ID"}` |

PCM为16bit小端、单声道16kHz，常规帧640字节/20ms，尾帧允许1～640范围内的正偶数字节；短尾帧之后不能再跟PCM，只能结束语段。JSON输入最大512字节，turn_id限定1～64位ASCII字母/数字/连字符/下划线，客户端须在连接内不复用。首版不支持WebSocket分片或压缩。未知消息、错误ID、非法边界、超长/奇数字节帧均拒绝并结束通道。

SW2需在媒体ready后先处于松开状态，再按下才开始上传；掉线时按住不会在重连后自动恢复录音。按键使用两次采样消抖，保留一个20ms本地前置帧覆盖消抖等待；松键后尾帧先发、再发input.end。持续本地采样不等于网络录音；没有云端建连等待或持续环境音上传。当前无AEC，已收敛为半双工；播放及等待模型回复期间不启动PTT。最新灯语和session.state扩展见6.9。

每方向最多10个队列项，纯PCM上限6400字节/200ms（控制消息也占队列项），另有固定DMA、20ms前置帧和当前处理帧；不是整机内存上限。队列满、采样溢出或驱动错误结束通道，不无限缓存或补发旧音频。output.end只表示发送完；板需处理已排队PCM，并用60ms零样本排空40ms DMA后才回output.drained。该事件不代表已经通过扬声器声学验收。客户端等待drained后再断开/close，不以任意固定等待替代确认。

首次open后30秒未连接释放；连接后5秒未hello释放；30秒未收到入站帧视为失联。盒侧Ping间隔/超时为10/10秒。均为本次试验参数，后续实测调整。掉线停止传输/播放、清旧会话及队列，旧回调按连接代次隔离。命令行每次运行一个新通道，异常退出不自动重连、重放命令或录音；常驻服务不在本轮实现范围。

#### 构建、联调与证据

硬件仓库构建与主机测试：

```bash
bash scripts/test_cmpp_host.sh
bash scripts/build_voice.sh
```

已执行：CMPP原有3套主机测试、voice_wire及voice_recovery共5套主机测试均通过；S3固件及原C3固件回归构建均通过。S3应用约1.36MiB、当前分区剩余7%，C3原测试应用分区剩余4%；未添加OTA，不能据此声称还有完整唤醒模型空间。2026-11-17按用户明确指令完成目标S3烧录、写入校验、启动日志及开发机实际BLE广播核验，见下方记录。随后用户已完成网页配网，LAN鉴权和新镜像PCM下行自动检查通过，最新证据见6.9。尚未完成：SW1实物手势、异常断电恢复、真实MIC1人声录音、扬声器听感、I2S时钟/首尾字与队列压力验收。

#### 设备类别：CMPP 语音终端（CMPP Voice Terminal）

这是Caelus项目对一类专门采集、传输和播放语音的CMPP设备的定义，不是单块板的昵称，也不宣称是CMPP通用标准已规定的设备类别。不同硬件可以实现这一类别，AI_REC_v2是当前第一种硬件实现。

| 层次 | 约定 | 用途 |
|---|---|---|
| 类别名称 | CMPP 语音终端 / CMPP Voice Terminal | 表示设备专门提供语音通道 |
| 设备类型 | `cmpp-voice-terminal` | 当前发布在mDNS TXT，仅作未鉴权发现提示，不是manifest字段 |
| 接口配置 | `caelus.voice-terminal.v1` | 已有manifest profile，标识本项目语音终端接口版本 |
| 能力 | `voice.channel` | 已有能力：打开/关闭语音通道；音频经关联WebSocket采播 |
| 硬件实现 | `AI_REC_v2` | 原理图/驱动适配型号，作为工程记录，不是新增协议字段或通用类别名 |
| 单台身份 | 鉴权后的CMPP UID | 区分同类别下的不同终端；显示名、MAC后缀及房间名称不代替UID |

本类当前最小职责为BLE配网、CMPP接入、按键采音上传及接收音频播放。语言理解、Agent、家居控制由盒侧处理。本地唤醒是后续可选能力，不是当前所有语音终端都已实现的功能。盒侧根据已鉴权设备的`profile_id=caelus.voice-terminal.v1`与`voice.channel`及其命令确认接口兼容性；mDNS的`device_type`、设备显示名和`AI_REC_v2`型号都不能替代该检查。将来其他硬件只有实现对应接口并通过联调，才能使用此profile。

#### 当前单板联调清单（优先于第7节历史整机清单）

| 项目 | 通过条件 | 当前证据/负责人 |
|---|---|---|
| V01 类型适配 | 仅接受已鉴权UID、`caelus.voice-terminal.v1`和`voice.channel`命令；错误profile拒绝 | 板端已声明；盒侧精确profile校验及负例已部署，18项语音模拟测试通过 |
| V02 固件与原板兼容 | S3可构建、5套主机协议/恢复测试通过，原C3仍可构建 | 硬件已通过；不代表实板音频通过 |
| V03 烧录准备 | 目标USB身份/芯片/Flash核实，原固件私有备份，H01实际分压与电源确认 | 16MB备份核实全为空白；按用户指令烧录及校验通过、启动/BLE广播通过；电气核对仍待实测，V03未完整通过 |
| V04 盒侧配网环境 | BlueZ可用、明确的2.4GHz网络配置来源、私有registry可写，dry-run目标唯一 | 用户已通过产品向导配网；新启动日志确认板端连接kelite、2.4GHz信道1，IP192.168.0.138，盒侧MCP鉴权可达 |
| V05 实板配网 | BLE安全会话配置网络→持久化并回读凭据→complete→LAN鉴权manifest通过 | 用户网页配网后，软件任务确认RK保存completed、官方MCP客户端鉴权manifest/state成功；持久化回读及断电保持证据仍待补齐，私有token不写入本文 |
| V06 采音上传 | 盒侧receive等待；SW2按下/松开形成完整语段，16k/16bit/mono WAV可辨人声 | 待实板；由硬件采音与软件接收共同验证 |
| V07 音频回放 | 盒侧play发送录音；接收output.drained，实板扬声器实际可听 | 真实测试音下行、playing状态及匹配drained已通过；人声回放和人耳听感待现场确认 |
| V08 清理与恢复 | 关闭/断网停止采播，旧通道不复用；冷启动保存配网身份，显式新连接可采播 | 实际WS关闭/MCP close媒体标志清空、重刷重启保存配对身份通过；云端明确空闲到期后自动建立新通道并恢复ready通过（6.10）；恢复后人声采播、真实网络中断和异常断电仍待测 |

盒侧环境：软件任务确认蓝牙控制器已上电，`bluetooth`与`NetworkManager`均active；此前未烧录时BLE发现列表为空。烧录后开发机已实际扫描到本板；软件任务进一步回报开发机`http://127.0.0.1:18890`及盒侧`http://192.168.0.18:18890`网页“我的家→添加设备→Caelus / CMPP 设备”均自动发现`Caelus-Voice-D6FCC4`各1条，无页面脚本错误。测试仅发现，未点击设备、发送凭据或执行pair/complete，测试浏览器已关闭。配网脚本已部署至`/home/neardi/.local/share/caelus/pair-device/scripts`，BLE环境位于`/home/neardi/.cache/caelus-pair-device/venv`，客户端位于`/home/neardi/Caelus/Caelus_Smart_Home_Dev/tools/cmpp_voice.py`；CLI配对凭据使用盒侧私有`~/.config/caelus/mcp-devices.json`，产品向导使用插件私有registry，二者不自动互通。盒子和开发机当前均连5GHz；开发机主动扫描发现其他2.4GHz AP，但尚不能确定目标网络及同LAN关系，不猜测共享密码、不切换盒子网络。

软件任务负责盒侧部署和修改，硬件任务负责固件和本表维护。当前盒侧联调地址为`192.168.0.18`（主机名`RK3588`，软件任务部署）；早期目标RK3576的指标不能当作该实机测量结果。当前步骤不加入云对话、房间授权或网页常驻语音服务；新增网页CMPP添加向导不等于常驻语音服务。

#### 实板登记与命名（2026-11-17）

- 设备类别：**CMPP 语音终端**；当前硬件型号：**AI_REC_v2**；用途：该类别首块实板的CMPP配网与语音采播验证。
- 固件默认显示名称：`Caelus Voice Terminal <STA MAC后六位>`；配网建议名与未设置名称时的回退值保持一致。已保存的自定义名称继续沿用。该固件已写入实板，配网广播名称已实测。
- 配网广播/SoftAP名称：`Caelus-Voice-<STA MAC后六位>`；主机名：`caelus-voice-<STA MAC后六位小写>`。名称方便辨认；盒侧仍以完成鉴权的CMPP UID识别设备，不以名称或MAC代替身份。
- 本次曾观察到USB：Espressif `303a:1001`，产品`USB JTAG/serial debug unit`，USB序列号`44:B1:76:D6:FC:C4`，当时端口`/dev/ttyACM0`，稳定路径`/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_44:B1:76:D6:FC:C4-if00`。
- 串口曾短暂消失后恢复，原因未确定。随后按上述by-id路径执行esptool 5.3.1 `chip-id`和`flash-id`，两次均成功；仅复位并加载RAM stub，无Flash写入。
- 实板读取：ESP32-S3 QFN56 rev0.2，40MHz晶振，内置8MB PSRAM（AP_3v3）；MAC `44:b1:76:d6:fc:c4`；Flash JEDEC厂家`5e`/器件`4018`、16MB，eFuse报告quad、3.3V。此读取不等于PSRAM读写或GPIO电压实测通过。
- 烧录准备：已读取安全状态，Secure Boot与Flash Encryption均未启用；未修改eFuse。已只读备份完整16MB Flash，文件位于`~/.local/state/caelus/hardware-backups/voice-d6fcc4/20261117T034358Z/original-flash.bin`，权限0600；SHA-256为`dffab0dd410657cb30c7b2fd7f2586a4792e8472e58882b3532581f8111a646d`。原固件可能含配置凭据，不纳入源码或共享附件。
- 本板固件默认名：**`Caelus Voice Terminal D6FCC4`**；默认主机名`caelus-voice-d6fcc4`。尚未通过配对取得并鉴权核实CMPP UID。H01分压与实际电源仍待测量。

#### 首次烧录与待配网实测（2026-11-17）

用户直接要求烧录后，再次核实USB身份、S3芯片及安全状态。原16MB备份重新计算SHA-256一致，逐字节检查均为`0xff`，没有旧分区或配置。esptool 5.3.1写入bootloader `0x0`、分区表`0x8000`、应用`0x10000`，三段均返回`Hash of data verified`；没有整片擦除、额外擦NVS或写eFuse。应用1,433,152字节，SHA-256 `36def13818c676fb573529108bda6a8c3a0aa7844bd9486cff6cf6b94c082074`，对应当前未提交的工作区构建产物。

USB启动日志显示应用启动、维护未请求、`wifi=missing, pairing=pending`，随后开启`Caelus-Voice-D6FCC4`热点及BLE服务，捕获窗口未见panic/abort。当前镜像使用2MB布局，真实Flash为16MB，启动会报告容量大于镜像头并使用2MB；这不是已启用16MB分区或PSRAM的证明。私有启动日志：`~/.local/state/caelus/hardware-backups/voice-d6fcc4/flash-20261117-boot.log`。

开发机Bleak扫描12秒实际收到：名称`Caelus-Voice-D6FCC4`，BLE地址`44:B1:76:D6:FC:C6`，服务UUID `021a9004-0382-4aea-bff4-6b3f1c5adfb4`，RSSI约−46～−47dBm。BLE地址不同于STA/USB的`...C4`，名称后缀仍来自STA MAC。此次仅扫描，未连接BLE、未发送网络凭据、未执行pair/complete；音频尚未初始化或验收。首次进入配网无需按键。后续重新配网使用启动时按住SW1、应用计时5～不足12秒后松开，建议操作约6～8秒，具体手感尚待实测；SW2为PTT。电气H01和供电验收仍未通过。

盒侧实现由软件任务维护：`Caelus_Smart_Home_Dev/tools/cmpp_voice.py`及`cmpp_voice_control.py`/`cmpp_voice_media.py`。软件任务此前报告17项模拟语音客户端测试、既有180项应用测试通过；本轮新增错误profile负例后，在盒侧18项语音客户端模拟测试及19项BLE/registry工具测试均通过。均不替代本板实测。具体安装与命令见该仓库`apps/smart-home/docs/CMPP_VOICE_CLIENT.zh-CN.md`。从其仓库根目录，用安装了websockets 15及配网工具的Python运行：

```bash
python tools/cmpp_voice.py --allow-insecure-lan verify <已保存的UID>
python tools/cmpp_voice.py --allow-insecure-lan --timeout 60 receive <已保存的UID> --output <新WAV绝对路径>
python tools/cmpp_voice.py --allow-insecure-lan play <已保存的UID> --input <录音WAV绝对路径>
```

盒侧部署后实际命令（当前还没有真实配对UID，以下须在V05完成后执行；`hw_实际UID`需替换为已保存UID）：

```bash
cd /home/neardi/Caelus/Caelus_Smart_Home_Dev
export CAELUS_PAIRING_SCRIPTS="$HOME/.local/share/caelus/pair-device/scripts"
voice_python="$HOME/.cache/caelus/smart-home-native/venv/bin/python"
mkdir -p "$HOME/.local/state/caelus/voice-tests"
chmod 700 "$HOME/.local/state/caelus/voice-tests"
"$voice_python" tools/cmpp_voice.py --allow-insecure-lan verify hw_实际UID
"$voice_python" tools/cmpp_voice.py --allow-insecure-lan --timeout 60 receive hw_实际UID --output "$HOME/.local/state/caelus/voice-tests/input.wav"
"$voice_python" tools/cmpp_voice.py --allow-insecure-lan play hw_实际UID --input "$HOME/.local/state/caelus/voice-tests/input.wav"
```

`receive`不会覆盖已有WAV，重测时须指定新的文件名。录音只用于本轮用户按键测试，保存在私有state目录，不上传云服务。命令可执行环境已验证，以上真实设备收发尚未执行。

先用已有`caelus-pair-device`工具discover/provision完成配对，凭据留在原私有registry，不复制Token。receive由用户按键触发、逐帧写入显式指定的新0600文件；失败不保留不完整录音，play回放该WAV完成最小闭环。当前只有命令行联调工具，没有网页常驻语音入口、云模型或家居控制。

控制HTTP与媒体WS当前仍是开发明文LAN，客户端对非loopback默认拒绝，`--allow-insecure-lan`显式选择此模式；BLE开发PoP也未升级为制造唯一凭据。没有声称生产TLS或密码学服务端身份已完成。

#### 盒侧真实BLE认证验证（2026-11-17，软件任务回报）

软件任务从已刷固件的`build-voice/sdkconfig`中安全读取`CONFIG_CMPP_BOARD_BLE_POP`，补齐开发机和RK3588产品服务的私有`CAELUS_CMPP_POP`配置（文件0600），未输出值。该固件使用非空PoP；网页删除了重复环境状态拦截，但官方Security1认证仍保留，不允许以Security0或跳过认证替代。

RK3588针对`Caelus-Voice-D6FCC4`、BLE `44:B1:76:D6:FC:C6`完成真实连接：`proto-ver`确认`prov.sec_ver=1`且`caelus.cap`含`caelus_pairing_v1`；官方Security1两次交换认证成功，随后正常断开并释放BLE。此次只访问`proto-ver`与`prov-session`，未调用`prov-config`或`custom-data`，没有发送Wi-Fi凭据或执行pair/complete。因此仅确认盒侧发现和安全会话可用，V05完整配网及LAN身份验证仍未通过，V06/V07语音采播仍待测。

#### 用户网页配网后的LAN核验（2026-11-17，软件任务回报）

用户在网页完成配网保存后，软件任务确认RK3588保存的设备`hw_eb61f632f1e3f23c`配对状态为`completed`，通过官方MCP客户端只读读取manifest/state成功：profile为`caelus.voice-terminal.v1`，仅有`voice.channel.open`和`voice.channel.close`命令，`ready=false`、`recording=false`。这是对前述“尚未pair/complete”测试时点的后续进展，确认配对后的LAN鉴权接口可达；不是语音通道已连接或采播实测通过。硬件任务未进行复位、重刷或音频动作；仍待记录网络条件、配网持久化/断电保持及PTT/回放结果。软件任务正修复已配对设备未显示在“我的家”的问题和添加后引导。

### 6.8 SW1启动维护与重新配网/恢复出厂（新实验固件）

以下为启动维护实现记录；“运行中不响应”的限制已在6.24修复，运行中仅开放重配网，不开放恢复出厂。

**状态：源代码已实现并通过主机逻辑测试，2026-11-17已刷入实板并验证普通首次启动进入配网；SW1手势尚未实物验证。以下时长是新固件的实现约定，不是已验证的按键手感或已发布产品步骤。** SW1在图纸上网名为`IO36`，实际连接U4第23脚`GPIO17`，按下接地，外部上拉3.3V。代码只读GPIO17；SW2 GPIO1继续用于PTT，GPIO0/BOOT与SW3电源键都不是维护键。仅按住SW1上电会进入维护窗口，普通运行时按/长按SW1不触发重配网或擦除，避免录音过程中误操作和并发修改NVS。

| 操作 | 精确条件（去抖后的固件计时） | 后果 |
|---|---|---|
| 正常重启 | 启动时未按SW1 | 保留已有设置；完成配对且有Wi-Fi时直接联网，不自动进入BLE |
| 取消 | 首次按住不足5秒即松手；或持续按住达20秒；或确认阶段短按/超时 | 不写操作意图，不清除设置，继续正常启动 |
| 重新配网 | 启动时按住SW1，持续至少5秒但不足12秒后松手 | 清Wi-Fi并置配对未完成；保留有效UID/Token及其他项目设置，进入BLE/SoftAP配置 |
| 请求恢复出厂 | 启动时按住SW1，持续至少12秒但不足20秒后松手 | 仅进入待确认，不清数据 |
| 确认恢复出厂 | 在上一步松手后的10秒内，再次按住SW1至少3秒并松手；按下与松手均须在10秒窗口内 | 持久记录意图后清本项目设备配置；之后生成新UID/Token并进入配网 |

GPIO采样10ms，输入稳定50ms后更新状态。初始500ms内不能读到稳定按键时取消维护；一轮确认失败不会自动开启下一轮。成功操作后继续本次启动，无须再次按键。按键计时从应用读取稳定按下开始，不从USB插入的物理瞬间开始；硬件启动延迟与去抖手感仍须实测。网页不得提示“长按任何键复位”，也不得把这套步骤默认用于C3或第三方CMPP设备。

当前反馈为USB串口阶段日志：维护开始、5秒可重配网、12秒待出厂确认、确认窗口、可以松手确认、取消、清理开始/完成/失败。成功后可以重新扫描到`Caelus-Voice-D6FCC4`。早期固件未实现LED或蜂鸣状态提示；当前LED增量实现与验证见6.9，SW1维护手势本身仍只有串口阶段反馈。2026-11-17重新核对：原理图LED1接法为3V3→R64（3kΩ）→LED1→GPIO46，低电平点亮；ESP32-S3官方GPIO文档及本项目ESP-IDF v6.0.2的`soc_caps.h`均表明GPIO46支持输出，先前将其视为仅输入、无法驱动LED的判断有误，现予纠正。LED1现已在固件中配置为状态输出；实际焊装和肉眼亮度仍待现场确认，GPIO46也是启动配置脚，须保留其启动/下载约束。官方依据：https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/gpio.html 。网页需标出“仅新实验固件、尚未实板验证”；不能把等待广播出现当作操作已经成功的证明。

#### 清除范围和断电恢复

| 数据 | 重新配网 | 恢复出厂 |
|---|---|---|
| ESP-IDF Wi-Fi持久配置 | 通过`esp_wifi_restore()`复位（不只删除应用副本） | 同左 |
| `cmpp_wifi`（应用保存的STA配置） | 清除 | 清除 |
| `mcp_light`内paired | 设为0 | 整个命名空间清除 |
| UID盐、MCP Token、自定义显示名/hostname | 保留有效值；配网表单明确修改名称时才更新 | 清除，下次创建新身份/Token和默认名称 |
| `cmpp_mqtt`（可选broker配置）、`cmpp`（SDK持久状态） | 保留 | 清除 |
| 固件、分区表、eFuse、其他NVS命名空间/其他Flash分区 | 保留 | 保留 |
| 盒侧registry、用户/家居授权、备份文件 | 板端不修改 | 板端不修改；盒侧旧记录需用户管理 |

这不是整片Flash安全擦除，也不会删除以前的私有固件备份。恢复出厂后的新服务不会接受旧Token；新UID不得继承旧设备授权。同UID意外出现不同Token时，盒侧仍应拒绝自动替换。

确认动作先写入独立`cmpp_recovery/action`并提交、回读核对，再执行清理。启动先读取待完成意图，完成前不创建CMPP运行时、不加载身份、不启动HTTP/MCP/媒体服务；Wi-Fi驱动只初始化、不启动无线服务。每一步错误立即停止并保留意图；下次启动幂等重试。清理全部成功后才将意图置0。出厂依次清Wi-Fi、MQTT、CMPP状态及身份，整个过程中不会生成可供盒子误认的中间身份。S3不再沿用NVS满/版本不兼容时自动擦全NVS的旧C3行为；遇到此类错误停止正常启动，需要工程诊断，不能悄悄当成用户确认的出厂操作。

#### 盒侧兼容约定

- 标准BLE Security1、`caelus_pairing_v1`、`pair`/`complete`及语音协议保持不变，没有新增远程复位接口。
- 重新配网清掉SDK与应用两份Wi-Fi，故`has_saved_wifi=false`，标准`wifi_prov`能力重新开放；盒侧以`proto-ver`实际能力决定是否执行Wi-Fi配置，不自行擦除设备身份。
- 若Wi-Fi已保存但配对中断，仍采用原恢复流程：`wifi_prov`不开放，重新保存并核对配对凭据后complete。与用户主动换网分开。
- 同UID同Token只更新已确认连接信息；名称/hostname如需修改由表单明确给出。现有紧凑pair响应不返回名称/hostname，盒侧不能省略请求里的hostname却凭空猜测板端保留值；重新配对已存记录时可复用记录的hostname，首次配对需发送明确hostname并据此构造连接地址。
- 新UID按新设备处理，无控制授权。发现、Wi-Fi配置、凭据保存、complete和LAN身份验证分别显示，BLE广播消失不等于所有步骤成功。
- 新启动顺序先维护检查和BLE配网，后音频初始化；坏codec不会阻挡首次BLE，但complete后若codec初始化失败，LAN manifest/MCP验证仍可能失败，网页必须保留“配对完成、LAN验证未通过”的区别。
- 产品网页适配位于Smart Home插件工程内CMPP模块，通过Core配置动作调用，不调用技能CLI；前述CLI作为独立工程验证工具保留。

已验证：手势边界、普通启动/运行时按键不触发、短按/卡键/超时取消、第二次长按且松手才确认、两个清理范围，以及每个清理阶段写入前/后失败后重试的主机模拟。模拟不代表真实NVS掉电、GPIO电平、Wi-Fi驱动清理、BLE重配网或旧Token失效实板测试已通过。

新增联调：V09测SW1电平、去抖和各时长边界；V10同UID/同Token换网、正常重启不再广播、保存Wi-Fi未complete时恢复；V11每个清理阶段断电后恢复，出厂新UID/Token拒绝旧凭据、固件/其他命名空间保留；V12网页明确选目标、选网络、配置、私有保存与身份核验，确认配置动作不授予家居控制。V09～V11由硬件主导，V12由软件主导，均待实板联调。

### 6.9 状态灯、半双工与模型等待窗口（2026-11-17增量，优先于早期说明）

用户通过软件任务明确要求完成状态灯和语音采播。硬件实现GPIO46低有效LED；应用在正常启动、完成启动维护检查后才配置输出，先预置熄灭电平，不改eFuse或启动上下拉。每50ms由ESP-IDF定时器根据真实网络/媒体状态更新GPIO。没有通过固定等待时间假设已连接或模型完成。状态来源为配网热点成功启动、STA获得/失去IP、有效hello、PTT输入事件、成功提交I2S音频及排空/静音结果。

| 状态 | LED1灯语 | 明确含义 |
|---|---|---|
| 配网入口已开启 | 亮500ms、灭500ms | 可进行配网，不表示已入网 |
| 启动后联网/断网重连 | 每2秒双短闪：亮100ms、灭100ms、亮100ms，其余灭 | 尚无STA IP |
| 有IP，尚无媒体ready | 熄灭 | 已联网不等于可说话；熄灭也不能单独证明断电 |
| 媒体ready且可开始新一轮 | 每2秒亮100ms | 板端媒体就绪；盒侧须在模型可收音时才恢复listening |
| SW2实际启动采音 | 常亮 | 已发input.start；不是看到按键按下便假定录音 |
| 等待整轮回复 | 亮1500ms、灭1500ms | input.end后/盒侧thinking/已开始回复，等待显式listening |
| 已开始提交回放PCM、尚未排空静音 | 亮250ms、灭250ms | 下行播放阶段，不是人耳听感验收 |
| 检测到队列/采播/帧处理错误 | 亮100ms、灭100ms | 通道停止；新hello成功后清除错误 |

优先级：故障、配网、无网络、媒体未ready、录音、播放、等待回复、空闲ready。正常close/断连清媒体标志；Wi-Fi掉线后下一采样循环主动停止通道、静音、清上传/播放队列及旧会话。不会重新播放旧音频或因按住SW2自动恢复录音。状态增加`playing`与`awaiting_response`两个布尔字段，原字段保留。它们是运行状态，不是音量/播放控制命令。

#### 线协议增量与半双工约定

保持`caelus.voice.v1`的hello/ready和PCM16LE、16kHz、单声道、20ms/640字节。新增盒→板严格JSON：

```json
{"type":"session.state","state":"thinking"}
{"type":"session.state","state":"listening"}
```

- 板端在发送`input.end`的同一临界区立即置`awaiting_response=true`，禁止新PTT，避免等待盒侧thinking消息的往返竞态。盒侧可用thinking同步等待状态；没有超时自动恢复listening。
- `output.start`也进入等待状态；第一帧PCM成功写入I2S才置`playing=true`。播放阶段按SW2不录音、不缓存按键请求，不声称支持打断或双讲。
- `output.end`标记发送结束。排完队列后写入60ms零样本以排空40ms DMA、DAC静音，再发同turn_id的`output.drained`并清playing。每轮只在首次播放时解除DAC静音，去掉了每20ms重复I2C解除静音调用。
- drained只结束一段下行播放，仍保持awaiting_response。盒侧确认模型整轮（包括工具/文字）已完成且所有音频段已drained后，发送listening。无音频轮也必须发送listening。解锁后须先松开再按SW2才会采音。
- 录音时收到output.start或session.state、输出仍busy时收到listening，视为状态冲突，停止通道。盒侧必须先收到匹配input.end才发送下行；各输出段等待前一段drained。
- 单个WS一次只接纳一个媒体通道；断线清全部会话标志，新hello从可监听状态开始。盒侧应在模型就绪后建立媒体，或马上发thinking保持等待。既有单轮CLI receive/play后关闭仍可用，多轮连接需支持新增session.state；产品应检查状态字段，不能把此扩展强发给旧固件。

#### 新镜像与已执行证据

6套主机测试通过（新增状态灯优先级/时序，扩展PTT半双工、等待后松开再按及session.state解析边界），S3构建和原C3构建通过。新应用1,435,232字节、分区剩余7%，SHA-256 `c6f0e8573649ce3a9f6d4f0f894118b65b26716e195f66e264a80f95ba45cced`。

重刷前RAM stub读Flash两次在约12KB处中断，未写Flash；改用esptool ROM方式成功备份当前前2MB及单独NVS，两次NVS内容逐字节一致。私有目录`~/.local/state/caelus/hardware-backups/voice-d6fcc4/pre-led-rom-20261117/`（0700，文件0600），2MB SHA-256 `7e763414ff56403ed9036ddd9c098b0b0f5d39393ac5ad9ec1a33878858748ad`；不得共享其中配对凭据。仅写应用区`0x10000`并通过Flash哈希校验，保留bootloader/分区/NVS，无恢复出厂。

重启日志确认wifi=saved、pairing=complete、IP `192.168.0.138`、ES8311/ES7210初始化成功。RK官方MCP客户端使用原私有凭据鉴权成功，UID仍为`hw_eb61f632f1e3f23c`；新playing/awaiting_response初始false，证明此次重刷后身份及联网配置可用，不代替异常掉电测试。

可复用硬件诊断脚本为`scripts/verify_voice_terminal.py`，通过官方MCP Python客户端与websockets读取盒侧私有registry，凭据仅留进程内。已执行真实play：MCP open→WS hello/ready→38,600字节PCM测试音（含短尾帧）→匹配output.drained；MCP并行读到playing=true，drained后playing=false/awaiting_response=true；显式listening恢复，然后WS断连和MCP close清除ready/recording/playing/awaiting_response。测试本身不调用模型。首次诊断脚本误用官方SDK返回字段名已修正为structured_content/is_error；当次未连接WS的open在30秒后释放，未当作播放通过。

现场诊断注意：在首轮PTT等待期间打开USB日志串口导致`rst:0x15 (USB_UART_CHIP_RESET)`，旧MCP会话失效并返回404；该轮录音未通过。随后新MCP会话再次鉴权成功，身份与Wi-Fi保留。此复位来自USB串口操作，不能写成固件崩溃；后续媒体验证期间不得开关该USB串口，需在测试开始前准备日志或只通过网络观察。

第二轮仅通过网络开启60秒PTT窗口，WS完成hello且保持心跳，没有收到input.start，最终本地等待超时；没有生成录音文件。WS退出及MCP close成功，随后新MCP会话只读核验四个媒体标志均false，设备凭据仍有效。该结果仅证明本轮空闲连接/清理，不足以判断物理SW2是否正常，因为没有用户现场按键确认。媒体连接已释放给软件任务开展产品桥接。

仍待现场：LED可见性及配网灯语、SW2实际按键录音/首尾字、喇叭是否出声及回放清晰度、播放时按键抑制的物理验证、长时间和网络异常测试；H01电气核对仍未完成。产品Realtime桥接的后续真实下行结果见下方；独立播放证据与产品模型链路证据分别记录。

#### 产品模型到真实终端的下行验收（2026-11-17）

软件任务运行RK3588产品`TerminalSession`：合成问候输入（明确不操作设备）→真实千问Realtime→实际Core签名插件授权媒体→真实语音终端`hw_eb61f632f1e3f23c`。硬件任务只读检查本机`~/.cache/caelus/rk3588-deploy/terminal-device-final.log`，最终结构化记录为`pcm_bytes=637440`、`drained=true`、`listening=true`、`input=synthetic`、`acoustic_verified=false`。软件任务确认drained匹配输出轮次，整轮完成后发送listening，退出后由Core取消媒体任务。

据此确认产品模型到真实设备的下行传输与结束/恢复监听控制通过。该轮没有实板麦克风输入，也没有独立观察硬件playing状态或现场听感；playing状态证据仍来自前述硬件独立测试。V06实板按键人声录音以及V07扬声器声学部分仍待现场验证，不写成全链路人声对话验收完成。软件任务随后恢复产品自动会话，硬件任务不再占用MCP/WS/BLE，不开关USB串口或复位。

### 6.10 关闭竞态与故障诊断补丁（已刷入，实机控制验证通过）

用户确认连续快闪后，只读复核发现当前已刷镜像将FAULT单独锁存，正常stop仅清媒体标志；这解释了为何四个媒体布尔值全false仍可能快闪，但没有原始事件记录，不能据此断定用户这一次的历史原因。软件任务正在旧镜像上采集真实连接断开证据；本阶段硬件仅修改和构建，不连接设备、不打开USB串口、不刷写。

确定的旧代码路径：MCP close清channel_id后、HTTP close回调完成前，迟到帧被当作无效帧置FAULT；关闭回调还会对已成功停止的会话重复静音/冲洗；hello超时检查未排除已经关闭但fd未释放的会话。补丁将会话active/generation和诊断集中到可做主机测试的`voice_lifecycle`，收帧前及读完payload后均检查同一活跃会话。已经关闭的帧/旧发送回调不再触发新故障，同一成功stop的回调不再重复I2S清理，不覆盖最初原因。若真实停止清理失败仍记录audio_stop_failed并保留故障灯。

`cmpp://state`中`capabilities["voice.channel"]`新增三个字段：

| 字段 | 含义 |
|---|---|
| `fault` | 当前故障锁存布尔值，直接对应故障灯依据；新有效hello清除 |
| `last_fault_reason` | 最近一次实际错误事件，恢复后仍保留历史；启动初始`none` |
| `last_stop_reason` | 最近会话首次停止的实际原因；迟到帧/重复close不覆盖 |

普通停止原因：`requested`、`peer_closed`、`network_lost`、`hello_timeout`、`heartbeat_timeout`、`open_timeout`，不会自行制造故障，也不会清除之前尚未恢复的真故障。错误来源：`send_failed`、`receive_failed`、`capture_failed`、`upload_failed`、`playback_failed`、`playback_queue_full`、`invalid_frame`、`state_conflict`、`audio_stop_failed`。例如正常请求关闭但静音失败会保留`last_stop_reason=requested`、`last_fault_reason=audio_stop_failed`、`fault=true`。不返回原始帧、Token、网络密码或推测的模型状态。该诊断是RAM内最近事件，重启清空，不是持久审计日志。

空闲排查：无录音时ADC读取错误本身不会触发原有故障灯；纯媒体超时也不会触发，除非停止清理失败。无效/冲突控制消息、发送错误、关闭竞态和清理驱动错误仍可能发生于用户未说话时。板子不知道“模型超时”，只能记录实际观察到的媒体与驱动事件；不得将正常连接中断直接解释为硬件损坏。

本次应用`build-voice/cmpp_voice_terminal.bin`大小1,436,288字节，SHA-256 `791e98e913bc635fae2a5137a2f8168c42545801ee1e1188e73c049c74e766e0`，分区剩余6%；S3构建、7套主机测试及diff空白检查通过。新增测试覆盖关闭时迟到帧/旧回调代际隔离、重复关闭保留首次原因、普通超时不制造故障、真实错误保留、打开但未hello不清故障、新hello清灯但保留历史、音频清理失败区别于关闭请求。主机测试不代替HTTP/FreeRTOS调度实测。

已准备`scripts/verify_voice_terminal.py --mode lifecycle`，只发送控制帧、无音频：正常WS close、五轮MCP close交错thinking/listening帧、无效控制帧触发真实fault、重连hello清fault并保留历史。软件任务保存旧镜像断开证据并明确暂停产品后，已刷入该镜像并执行上述实板测试，结果见下方。

#### 诊断补丁的实板验证与现场交接

软件任务先记录旧固件约五分钟空闲断开：盒侧媒体ConnectionClosedError/1011，同次模型关闭码1007；该关联仍不能证明先前快闪的具体源头。待其通过网页API暂停产品并明确可刷写后，硬件经ROM读取并私有保存当前NVS，目录`~/.local/state/caelus/hardware-backups/voice-d6fcc4/fault-fix-20261117T070907Z/`（含候选镜像、SHA、刷写日志及lifecycle-evidence.json）。仅写`0x10000`应用，通过Flash哈希校验；未擦NVS或修改身份，未打开USB日志串口。

RK沿用原registry通过官方MCP客户端鉴权，UID仍为`hw_eb61f632f1e3f23c`。初始新诊断为fault=false、两个reason均none。实际无发音测试全部通过：

- 正常WS关闭：fault=false、last_stop_reason=peer_closed。
- 五轮MCP close与thinking/listening控制帧交错：均fault=false、last_stop_reason=requested，迟到关闭回调不覆盖原因。
- 显式发送非法session.state作为负例：fault=true，last_fault_reason与last_stop_reason均invalid_frame，证明真故障未被抹掉。
- 新hello成功：fault=false，仍保留历史last_fault_reason=invalid_frame。
- 最后正常WS关闭并释放：四个媒体布尔值均false，fault=false、last_stop_reason=peer_closed。

**交接时的历史invalid_frame是本次有意的负向测试记录，不是产品新故障。** 产品应以fault布尔值识别当前锁存，历史原因仅用于追溯。测试没有发送PCM、没有验证声音或新的按键录音。硬件已通知软件任务恢复产品复测；不以这些控制测试推断旧镜像用户快闪的历史根因。

#### 云端空闲断开独立取证与恢复进展

软件任务在完全不连接硬件的独立Qwen Realtime测试中复现：310.54秒后关闭，code=1007，服务端原因原文为`Your session was closed because no response was generated for 300 seconds.`。硬件任务只读核查本机`~/.cache/caelus/rk3588-deploy/qwen-idle-isolated-2.log`确认以上结构化证据。由此确认云端空闲到期可独立导致该类会话断开；不能由此反推出旧板那一次FAULT由哪个硬件代码分支置位。

软件任务报告已部署恢复逻辑：仅当会话此前ready且服务端明确报告上述空闲到期原因时，先完全结束旧Core媒体任务，再建立新会话；不以本地300秒计时推测原因、不发送静音保活、不重放旧音频或动作，认证/配置等错误不走此恢复分支。单个1007关闭码不足以匹配这一恢复条件，须同时核对服务端原因。

#### 云端空闲恢复组合验证（2026-11-17）

软件任务定位到真实事件顺序为：供应商先发空闲到期error通知，再发1007 Close。原应用在处理error通知时提前抛异常，退出Unix媒体连接上下文并触发1011关闭，错过了真实模型关闭原因。现将通知与连接关闭分开处理，继续读取服务端Close后决定是否自动恢复；真实本地WebSocket回归覆盖了“error通知→1007空闲Close→媒体清理”顺序。该结论解释本轮软件自动恢复失败，不反推旧固件曾出现的快闪原因。

随后真实云端、RK盒子和本板组合恢复通过。硬件任务只读核查软件保存的`~/.cache/caelus/rk3588-deploy/terminal-recovery-web-final.log`及`terminal-idle-recovery-final.log`，未另行连接设备。以下为日志UTC时间（北京时间加8小时）：

| 时刻 | 证据 |
|---|---|
| 07:43:47.062 | 页面状态收到供应商300秒无回复的空闲通知 |
| 07:43:57.744 | 状态进入“待机连接已到期，正在恢复语音” |
| 07:43:58.789 | 状态进入“正在连接实时语音” |
| 07:44:03.061 | 页面恢复ready，提示按住SW2说话 |

鉴权设备状态记录中，channel_id由`588026b140ce7f9fd93c2446571799cf`变为`abc98708603ee3d54646df5c483c83a0`；新通道`ready=true`、`fault=false`、`last_stop_reason=peer_closed`。历史`last_fault_reason=invalid_frame`仍是前述有意注入的负例。软件任务确认期间没有手动重连，也没有重放音频或家居动作。此次证明一次真实空闲到期后的自动新建通道和恢复就绪，不证明多轮长期稳定性或恢复后实板人声采播。

网页采样日志在上述ready记录之后以`Failed to fetch`结束；已有设备状态独立佐证本次恢复，不能把该采样日志当作后续持续在线证据。软件任务随后仅将已识别空闲通知改为中文恢复进度，报告本地WebSocket回归再次通过、最终部署进行中；此文不将该文案版本记为再次通过300秒实机测试。硬件继续保持空闲。

### 6.11 第二块AI_REC_v2语音板烧录与待配网（2026-11-17）

用户更换接入另一块CMPP板，报告已焊接喇叭和电池，并授权重新烧录、由其重新走配网流程。此次实测目标USB/STA MAC为`44:B1:76:D6:FD:1C`，不同于前述D6FCC4板；ESP32-S3 QFN56 rev0.2、8MB内置PSRAM（AP_3v3）、16MB Flash，eFuse报告quad Flash及3.3V。不能沿用旧板UID或凭据识别新板。

烧录前ROM模式读取完整16MB备份，确认原Flash全为0xFF，SHA256为`dffab0dd410657cb30c7b2fd7f2586a4792e8472e58882b3532581f8111a646d`。私有证据保存在`~/.local/state/caelus/hardware-backups/voice-d6fd1c/initial-20261117/`，包括原始镜像、候选镜像、元数据及刷写/启动/发现日志；目录0700、文件0600。

S3重新构建和7项主机测试通过，应用仍为已验证的`791e98e913bc635fae2a5137a2f8168c42545801ee1e1188e73c049c74e766e0`，1,436,288字节。以esptool ROM模式分别写入bootloader（0x0）、partition table（0x8000）、application（0x10000），三项写入哈希校验均通过；未做整片擦除。沿用现有2MB固件分区布局，不表示物理Flash只有2MB。

启动日志确认`cmpp_voice_terminal`、`wifi=missing`、`pairing=pending`，BLE及SoftAP配网入口均启动。开发机被动扫描实际发现名称`Caelus-Voice-D6FD1C`、BLE地址`44:B1:76:D6:FD:1E`、服务UUID `021a9004-0382-4aea-bff4-6b3f1c5adfb4`。新板首次配网无需按键；本次未发送Wi-Fi凭据或执行pair/complete，尚无新UID的LAN鉴权或音频采播验收。用户可在产品网页自行添加；此时旧UID由软件任务保持暂停，后续解除见6.12。串口检查和扫描均已结束，不占用后续流程。

按键用途再次核对：SW2/IO1=GPIO1为PTT；SW1的网络名IO36实际连GPIO17，用于启动维护/重配网，运行时按下不触发维护；SW4/IO48=GPIO48，当前固件未分配功能。喇叭、电池焊接状态来自用户报告；本次未测实际出声、电池续航、充放电或电气指标，不能直接继承上一块板的音频测试结果。

### 6.12 正常断线自动重连（软件部署，物理恢复待验收）

正常断开设备不要求先在后台暂停；暂停仅表示用户主动停用，之前暂停是刷写维护期间的隔离措施。软件任务报告已部署`source-20261117-voice-disconnect.tar.gz`且RK服务active：实际传输断开后清理旧授权任务和音频，待旧线程退出并间隔15秒后新建连接，不重放旧录音或家居动作。配置/协议ValueError不走自动重试，主动暂停优先，每次新连接重新经过Core授权。

软件任务报告应用213项回归通过，包含真实本地WebSocket模拟终端断开后的Core任务清理、重连标记与不重放，以及管理器旧线程退出、15秒间隔和暂停优先测试；这些是软件回归，不是实体拔电恢复验收。其已通过正式API解除旧UID `hw_eb61f632f1e3f23c`的维护暂停（HTTP200），当次网页仍显示“语音连接已断开，正在自动重连；未重发上一轮内容”，尚未ready。此变更未刷写固件，也未由硬件任务连接新板。

双方仍待实测：设备实际断电后再上电，保存配网/身份有效，旧媒体任务释放，新通道ready，新的按键语音可采播且旧音频/动作不重放；以及两块实体板同时通话。新板已接电池，拔USB不一定断电，断电测试须以板实际掉电为准。盒侧详细验收记录见软件仓库`apps/smart-home/docs/TERMINAL_VOICE_ACCEPTANCE.zh-CN.md`。

### 6.13 配网网页服务不可用的盒侧修复

用户反馈配网页面“服务不可用”后，软件任务复现RK正式网页CMPP status/discover返回HTTP503，底层adapter记录可读，Core报告`plugin input unavailable; no replay`，智能家居插件进程已退出。其定位并修复一个确定的阻塞路径：voice任务ack同步等待离线MCP清理，可能超过Core的5秒RPC时限；修改为立即标记撤销/ack，真实清理完成后才回收任务，不放宽授权或超时。该阻塞路径的修复不等于已证明插件退出的全部原因。

软件任务报告签名部署并重启后，正式网页status/discover均HTTP200，实际发现`Caelus-Voice-D6FD1C`及两条已保存网络，无notices；未执行配对或播放。该证据确认盒侧配网入口恢复，不是新板配网完成、LAN鉴权或音频验收通过；硬件无需因此重刷或恢复出厂。其报告214项应用、75项插件Python、3项Rust测试通过，包括慢清理测试。

另由软件任务实现卡片删除设备：经Core移除对应UID的本机凭据并撤销该设备媒体，支持离线删除，不对板端恢复出厂。手机浏览器删除模拟测试已通过，未删除真实用户设备。删除本机记录与SW1板端重配网/恢复出厂是不同操作，不能互相视为已完成。

后续用户报告点击连接等待一两分钟后提示“与中枢的连接中断”。软件任务报告已把长HTTP等待改为查询现有Core任务回执并部署RK；真实Core/签名插件慢配网模拟、网页丢响应及刷新不重复发送测试通过。此时设备registry为空，不代表新板已完成配对。

最新盒侧实测：使用仓库官方Espressif `Transport_BLE`，对新板D6FD1C（BLE地址尾号FD1E）只读`proto-ver`成功，耗时14.2秒；SDK的Discovering、Connecting、Getting Services、Disconnecting阶段完成，返回支持Security1。未建立安全会话，未发送Wi-Fi凭据或pair/complete，BLE已释放。因此此前一次25秒超时不能作为当前固件故障结论；此次结果也不等于Security1握手或完整配网成功。软件报告正式网页status/discover正常、新板可发现、保存设备仍为0，实际配网待用户验收。硬件同期只读复核烧录后旧日志，未连接或复位设备；旧日志无欠压/崩溃证据，不能排除本次现场供电问题。

### 6.14 配网结果未确认的联合定位（恢复配对复测通过，前次根因未定）

用户实际配网后收到“配网结果未确认，请查看我的家中的连接记录”。软件任务提供两次Core任务：09:14:48 UTC的`cmpp:e050bf39-ff81-4b82-a2cd-88538b911afa`、09:15:45 UTC的`cmpp:d11fd412-c724-4690-8839-46f7862072e3`，最终均failed/effect unknown，adapter commands 53/54同为unknown，设备registry为空。该证据仅证明未成功保存配对凭据，不能单凭它判断板端是否收到pair；原worker缺少最后成功阶段诊断，由软件任务补齐。不自动重发结果未知的配网操作，不以重启Core/App排障。

硬件只读核查本地固件、实际voice build编译数据库和Espressif SDK；未开串口、复位、刷写、连接BLE或自行配网。编译PoP与项目默认配置一致，新旧板归档应用与当前镜像相同；只报告一致性，不记录PoP内容。软件任务随后确认运行adapter的实际PoP与编译配置匹配，并在新板完成真实Security1握手（15.7秒），最终断开释放BLE。此前诊断脚本将EnvironmentFile引号当作值导致握手失败，已确认是诊断路径错误，不作为产品故障证据。最新只读`proto-ver`为Security1、`prov.cap=[]`，Caelus能力包含`caelus_pairing_v1`；未执行GetStatus、set/apply、pair/complete。

按当前烧录SDK源码，配网与诊断需遵守以下边界：

| 环节 | 确定行为及诊断限制 |
|---|---|
| 初次启用Wi-Fi配网 | 无保存Wi-Fi时，固件调用enable_provisioning，先注册端点并置enabled位，再生成proto-ver；成功时输出wifi_prov/wifi_scan，不因SESSION_ONLY天然为空 |
| cap空但GATT描述符仍在 | SDK停止Wi-Fi provisioning后移除处理器、清enabled位并更新cap，同时保留原BLE/GATT表和custom-data；残留描述符不表示仍能配置Wi-Fi，也不能仅凭cap空推断当前有IP或配对完成 |
| set / apply | set暂存配置；apply写Wi-Fi NVS并安排连接，返回成功不等于已获IP；SDK最多尝试5次，获IP才发CRED_SUCCESS |
| GetWifiStatus | Connected分支会调用mgr_done；获IP后也有30秒自动停止计时。停止时保留BLE会话但移除Wi-Fi处理器，并切STA模式，因此此查询不是完全无状态变化的诊断 |
| 状态解析 | 同时检查protobuf消息类型、status、wifi_sta_state及对应字段。官方Python辅助函数未拒绝非Success状态，不能只信其返回字符串；Security1 CTR响应只解密一次 |
| custom pair | 写名称/hostname并将paired置0，返回UID/Bearer；不可作为只读探针；fresh-boot且未连接Wi-Fi时返回wifi_not_connected，恢复配对路径有例外 |
| custom complete | 写paired到NVS，应用等待1秒后拆BLE/SoftAP；回包丢失不等于设备没有完成，先核对已保存凭据与任务阶段 |
| 无副作用状态接口 | 当前custom-data没有status/info，仅pair/complete；SoftAP /api/pairing返回Bearer，/api/networks触发扫描，均不能当作无敏感信息的纯状态查询。已有凭据可做LAN鉴权manifest/state检查，mDNS仅为未鉴权提示 |

另发现源码中的待验证恢复缺口：BLE配网不设置s_connect_requested，SDK停配网后应用s_portal_active要到custom complete才清除。若这一窗口STA断线，应用可能直接忽略重连，SDK事件处理也已注销；fresh-boot的后续pair可能返回wifi_not_connected。尚无本次错误枚举或断线证据，不能把此路径认定为根因，未修改或重刷固件。后续成功复测见下方，不基于此前推测修改固件。

#### 用户主动复测：恢复配对及LAN身份核验通过

软件任务补充分阶段诊断，仅由短生命周期worker重新加载，未重启Core、App或adapter。诊断只记录阶段、异常类和白名单设备错误码，不记录凭据或原始回包；每份日志上限64KiB、保留一份轮转备份。以下是软件任务对新板真实复测的回报，时间为2026-11-17 UTC：

| 时刻 | 已确认阶段 |
|---|---|
| 09:27:53 | 用户主动发起配对请求 |
| 09:28:12 | BLE连接完成 |
| 09:28:14 | Security1握手完成；device_pair返回ok；credential_store成功 |
| 09:28:15 | device_complete返回ok |
| 09:28:16 | BLE断开、pair_request完成 |
| 09:28:19、09:28:26 | LAN身份verification=verified |

新板登记为`hw_3af24949f40555a2`，名称`Caelus-Voice-D6FD1C`；registry记录`pairing=completed`、`credential=stored`，Core回执为`completed/committed`。本次没有执行Wi-Fi set/apply/status，因设备Wi-Fi provisioning能力已关闭，沿用已有网络完成恢复配对。未复位、刷写固件或重启Core/App，硬件任务未访问设备。

该结果证明已有Wi-Fi条件下的Security1、pair、凭据保存、complete及LAN身份核验完整通过；不是恢复出厂后的全新配网验收，也不证明新板实际人声采播或供电稳定性。此前两次unknown没有原始异常，根因仍未确认，不能将此次成功称为已修复它们的根因。新板后续测试以此UID定位，不使用旧板身份。

### 6.15 新板无声与频繁语音重连的联合诊断（进行中）

用户报告新板显示回复完毕但未听见声音，并频繁提示语音断开、自动重连。软件任务负责RK日志、唯一设备状态采样及隔离媒体测试；硬件任务仅只读本地固件/SDK和原理图，不开USB串口、不刷写/复位、不接BLE、不并发建立MCP/WS控制连接。

软件任务通过官方MCP只读state取得：新UID `hw_3af24949f40555a2`，channel_id=`471b9c6e79b0b2952bc8d883a29132d9`，ready=true，recording/playing/awaiting_response/fault均false，last_fault_reason及last_stop_reason均upload_failed。桥接日志有InvalidMessage、ConnectionClosedError及device_close_code1006；应用两次phase=listening时ConnectionError，清理后的model/media关闭码1000不能直接代表首个失败原因。

本地代码核查确定：

- `upload_failed`来自板内上传队列入队或`httpd_queue_work`失败；队列10项，PCM与input.start/input.end/output.drained文本共用，连续PCM容量约200ms。实际socket发送失败另记send_failed，I2S录音错误另记capture_failed，播放错误另记playback_failed；不能把upload_failed直接解释成云端失败。新hello清fault但保留历史原因，当前false不抹去历史错误。
- 板端收到PCM16LE/16k单声道后复制到I2S两个槽，再经ES8311 DAC、NS4150功放到PK1；没有Opus/MP3解码。首帧成功提交I2S后才置playing；尾队列、三帧零样本和静音API调用完成后发送drained，仍不是人耳出声证据。
- 当前14项状态字段没有PCM字节/帧数、峰值/RMS或I2C寄存器读回，也无网络日志下载接口；不能凭事后ready快照还原整轮声音。软件侧应记录非敏感的每轮字节数、首末帧时刻、幅度统计、drained及首个异常，不必保存用户录音。
- SDK的`esp_codec_dev_set_out_mute`忽略底层mute返回值，ES8311 mute函数又未上报寄存器写错误；因此无playback_failed或有drained也不能排除DAC静音控制写失败。此为已确认的可观测性缺口，尚未证明本板发生该错误，未修改库或刷固件。
- 盒侧turn_done路径在没有output_turn时也可显示“回复已播放完”，该文案不能证明生成/发送过PCM。需先区别无音频模型回复和实际非零音频下行。

原理图第1页U6/NS4150的CTRL（1脚）通过R29=10k下拉，R28=2.2k另一端未见明确网络名/驱动连接；图中GPIO21的MUTE也未见与该点连通。当前固件配置`pa_pin=-1`，没有驱动外部功放使能。厂商[NS4150手册（Espressif镜像）](https://dl.espressif.com/dl/schematics/NS4150B.pdf)规定CTRL低电平关断、高电平启用（VIH最低1.2V）。若非零PCM、playing和drained均证实而仍无声，应先用实际PCB连通性和U6 CTRL/VDD对GND电压核对；图纸疑点不能代替实测，不猜测GPIO21用途或盲目飞线。

另当前输出音量35使用codec默认0～100映射-50～0dB的曲线，35对应约-32.5dB目标，再由codec扣除硬件增益；它不是35%输出功率。低音量可作为候选，但不据此直接拉满。隔离非零PCM下行、实际扬声器听感及断线首因仍待验证；本节不把无声和上传故障认定为同一根因。

### 6.16 最大音量测试固件与有限诊断（2026-11-17）

软件任务经产品 Core/插件向新板发送两次实际非零合成 PCM：首轮640000字节/1000帧、峰值28671、最高帧RMS11242，收到output.drained后回到listening，但用户确认完全无声；第二轮752640字节/1176帧、峰值24063、最高帧RMS10385，同样drained/listening。两次均不是声学通过证据；用户未完成电压测量，功放CTRL/VDD状态仍未知。

用户通过软件任务明确要求最大音量测试。软件暂停并释放UID `hw_3af24949f40555a2`后，硬件以USB by-id和芯片读取双重确认MAC `44:B1:76:D6:FD:1C`，实板分区表与构建表一致。仅将应用写入0x10000，未写引导程序/分区表/NVS；写入哈希及独立verify-flash通过，24KiB NVS在烧录前后逐字节一致，随后硬复位并释放USB。未再打开串口或自行连接媒体，运行态/听感由软件任务复核。

- 本次应用SHA256：`1abc02af662d4f24743f291bbad05b009d5f37bb145a00cfc4e74693fdefe79d`；1437712字节，应用分区剩余6%。
- 官方`esp_codec_dev_set_out_vol`使用本次实际编译配置100，通用Kconfig默认仍为35。这是本次最大音量实验镜像，不能把100当成已验收的日常默认音量。
- 私有归档：`~/.local/state/caelus/hardware-backups/voice-d6fd1c/volume100-20261117T094516Z/`，目录0700/文件0600，包含应用、sdkconfig、NVS前后备份及烧录校验日志；备份内容不进入仓库。
- 7套主机测试及ESP32-S3构建通过；新增检查覆盖两个上传失败分类、元数据、成功发送字节与失败计数区分、最大耗时和UINT32饱和边界。构建的ROM GDB辅助文件因环境变量缺失产生非致命警告，不影响固件生成及校验。

`voice.channel`原14个状态字段保留，新增`diagnostics`对象：

| 字段 | 含义 |
|---|---|
| scope | `since_boot`，跨通道重连保留，重启清零 |
| upload_pcm_queued_bytes / upload_pcm_sent_bytes | 成功入队/完整发送成功的PCM字节；不含文本，入队不等于已发送 |
| output_pcm_received_bytes / output_pcm_i2s_bytes | 通过协议校验的下行PCM/完整提交I2S成功的PCM字节；I2S计数不含flush补零，不代表实际出声 |
| upload_queue_high_water | 上传队列占用峰值，包括PCM和文本包，队列容量仍为10 |
| upload_queue_full_count | 上传入队失败次数；对应`upload_queue_full` |
| upload_work_schedule_failed_count | HTTP工作调度失败次数；对应`upload_work_schedule_failed` |
| upload_send_failed_count / upload_send_max_us | 实际send_frame失败次数/调用最大耗时，覆盖文本和PCM；不包含等待媒体锁的时间 |
| last_upload_failure_generation / last_upload_failure_packet / last_upload_failure_reason | 最近上传入队/调度失败对应的代际、静态包类别和原因，不含用户音频/凭据 |
| last_upload_schedule_error / last_upload_schedule_errno | 最近调度失败的ESP错误码和当时errno；后续队列满不覆盖该历史调度错误 |
| configured_output_volume | 编译传给codec的音量值，本次为100，不是寄存器回读或声压测量 |

计数及耗时上限为UINT32_MAX，达到后饱和。通道首次停止规则和过时代际过滤保留；实际socket写失败仍为`send_failed`，不可混同调度失败。此版不扩容队列、不丢帧、不改未知PA/GPIO，不调整TCP_NODELAY或Wi-Fi省电；这些候选需结合新增计数再判断。

官方`esp_codec_dev_get_out_vol`/`get_out_mute`返回缓存，不能作为DAC状态读回。`esp_codec_dev_read_reg`确实调用ES8311 I2C读取且传递读取错误，但单次默认超时100ms；本次没有在实时媒体锁内增加轮询，以免引入新的音频阻塞。静音写错误传播缺口仍在，不能把drained当作已证明DAC解静音。待软件核验运行态字段、复播及用户听感后补充验收。

最大音量临时版复播补充：软件报告1054720字节/1648帧、约32.96秒非零PCM，峰值22655、最高帧RMS10529，匹配output.drained；用户明确确认“仍然完全无声”。因此不能把原音量35认定为无声根因。I2S字节计数和寄存器读回证据仍需补齐，CTRL/VDD未测。

进一步静态核对：GPIO12/13/14/16与原理图ES8311 MCLK/SCLK/LRCK/DSDIN一致；固件单声道复制两物理槽，SDK确实启用I2S通道。官方ES8311系数表支持16kHz、MCLK4096000Hz，固件配置256倍；这证明配置一致，不证明板上实际时钟。另SDK的set_fs忽略格式/位宽/采样率设置函数返回值，部分初始化ret也会被覆盖；初始化成功不等于所有寄存器设置已验真。候选软件验证路径是固定寄存器的官方read_reg读回，避免把SDK音量/静音缓存当硬件状态；尚未为此修改或刷写寄存器诊断固件。

### 6.17 运行时音量共同契约（本地候选，尚未刷入）

用户要求网页可调0～100，不能以重编译固定100代替。双方确认保留`profile_id=caelus.voice-terminal.v1`和媒体协议；`voice.channel`能力版本增至1.1.0，新增`voice.channel.volume.set`。软件任务负责Core已有授权路径、插件及网页，硬件负责codec设置和持久化。

| 项目 | 约定 |
|---|---|
| 参数 | 仅`{"volume":整数0..100}`；拒绝bool、浮点、负值、超100及多余字段 |
| 读取 | 原状态保留，新增顶层`volume`整数；命令result为完整状态，软件读取`receipt.result.volume` |
| 含义 | 官方codec API接受的音量配置；不是寄存器读回、输出功率百分比或实际听感 |
| dry_run | 验证参数但不写codec/NVS，result.volume仍为当前值 |
| 静音 | 0同时调用官方mute；后续播放不能覆盖零音量静音；调高音量保持空闲/关闭通道静音，正在输出时恢复播放 |
| 保存 | 复用NVS `mcp_light/voice_volume`（u8）；重启及重新配网保留，现有恢复出厂流程清除；没有保存值时默认35 |
| 失败 | codec API或NVS失败返回failed/effect unknown、`volume_set_failed`；运行配置可能已变，返回状态据实展示，不能把保存失败当完成。重复设置同值可重试持久化 |
| 同值 | 已接受且已保存的相同值不重复NVS commit |
| 默认诊断 | 原`diagnostics.configured_output_volume`保留为编译初始默认值；实际运行配置读顶层volume，避免混用 |

在既有媒体锁内串行执行设置/保存和静音，不新增常驻轮询、PA控制、远程任意寄存器写或缓冲策略。用户操作时会发生I2C/NVS访问，播放中调节的连续性仍须实板验证。SDK `esp_codec_dev_set_out_vol`同样忽略底层set_vol错误，不能据其成功回执断言I2C电气链路或扬声器正常。

候选应用SHA256 `7de45f78bddf39f4fcc9ea3575197dab18b1f4672bcdff0854bf664871d38924`，1438544字节，9/9主机测试、S3构建通过。测试直接编译生产`voice_volume.c`及`voice_board.c`，隔离codec/NVS/媒体边界，覆盖参数/命令形状、dry_run、返回状态、0/100、静音、模拟重启恢复及存储失败重试；这些不是实板验收。私有归档`~/.local/state/caelus/hardware-backups/voice-d6fd1c/runtime-volume-candidate-20261117T094946Z/`；等待软件正式链路就绪并释放设备后仅更新应用。

联调待办：新manifest包含命令→官方产品设置100并核对result/state→0/中间值/100调节→播放中调节及静音→断电重启配置保留→配网身份不变→非法参数及旧固件不支持时网页如实提示。用户听感、电气和持续播放仍需现场验收。

### 6.18 官方ES8311固定寄存器诊断（候选未刷）

软件读取100临时版实板状态：`output_pcm_received_bytes=output_pcm_i2s_bytes=1054720`，与发送端完全一致；`upload_queue_high_water=1`、queue_full/schedule_failed/send_failed均0，最大send耗时1715µs；fault=false、last_fault_reason=none、last_stop_reason=peer_closed。这证明该轮PCM接收/提交I2S闭环，不证明DAC输出或喇叭出声；该轮也未复现此前upload_failed。

经软件任务协调，在运行时音量候选并入只读寄存器诊断，控制命令不变。新顶层`codec_readback`对象中，`source=es8311_i2c_cached`明确表示官方`esp_codec_dev_read_reg`实际I2C读取后的缓存；state查询不触发任何I2C操作。

| 字段 | 采样与失败语义 |
|---|---|
| boot_values / boot_errors | 固定16个寄存器的启动读值/错误码对象，key是十六进制地址字符串；音频初始化结束后、采播任务创建前读取一次 |
| volume_reg32 / volume_error | 官方音量设置调用后读取0x32；不是SDK音量缓存 |
| mute_reg31 / mute_error / requested_mute | 官方静音调用后读取0x31及本次请求状态；保留播放结束后的静音结果 |
| last_unmute_reg31 / last_unmute_error / unmute_read_count | 保留最近请求实际解除静音后的0x31读回，便于播放结束后检查；count=0表示尚未采样，值初始-1 |
| read_count / read_failed_count | 本次启动累计读次数/失败次数，UINT32饱和，通道重连不清零 |

每次读取错误码0才可解释读值；失败时值=-1且保留原始SDK错误码，绝不把失败当0或沿用旧成功值。诊断读取失败不改控制API回执、不写寄存器、不自动重试，也不猜测PA/GPIO。运行时仅音量设置额外读取0x32与0x31、静音操作额外读取0x31，不在每帧或state请求时轮询。每次底层默认超时100ms，异常I2C可能增加一次操作的延迟；播放连续性与队列压力仍待实测，不能把候选测试当性能验收。

以下期望从项目锁定`esp_codec_dev 1.6.2`的ES8311驱动初始化、16k/4.096MHz系数及当前配置推导，非实测值：

| 地址 | 判读依据/当前期望 |
|---|---|
| 0x00、0x01 | reset/start从机及外部MCLK配置，期望0x80、0x3f |
| 0x02、0x03、0x04、0x05 | 分频/过采样配置，期望0x00、0x10、0x20、0x00 |
| 0x06、0x07、0x08 | 驱动采样系数：0x06低5位=3、0x07低6位=0、0x08=0xff；从机下不能将这些配置值当板上BCLK测量 |
| 0x09 | DSDIN数据接口，低5位=0x0c（16bit、正常I2S），bit6应清零 |
| 0x0d、0x0e、0x12、0x37 | start的电源/DAC配置写值，期望0x01、0x02、0x00、0x08 |
| 0x31 | 驱动mute掩码0x60；请求解除静音后这两位应为0，播放结束后置0x60属正常 |
| 0x32 | 驱动音量映射字节；当前配置pa_voltage=5、dac_voltage=3.3、pa_gain=0，按官方计算volume35约0x85、100约0xc6、0约0x06并额外mute；这些电压是当前驱动配置值，不是现场测量 |

新候选SHA256 `1dfc1540ef4f2d852853906faeead8dfeba0feec1b1c123c791c180568401ca2`，1440032字节，9/9主机测试和S3构建通过；替代6.17的未刷候选。测试新增“API报成功但模拟静音写没生效时读回仍为0x60”、读取失败值=-1、逐寄存器失败标记、getter零I2C访问以及嵌套状态输出。私有归档`~/.local/state/caelus/hardware-backups/voice-d6fd1c/codec-readback-candidate-20261117T095514Z/`。当前未刷，等待软件完成网页链路并释放设备。

### 6.19 用户确认GPIO21连接及拉高播放（2026-11-17）

用户明确恢复限定测试“直接给GPIO21脚一些高低电平测试”，随后确认“MUTE连接到了CTRL这个功放的脚”，再收敛为“直接拉高，然后播放声音”。因此GPIO21→MUTE→NS4150 CTRL的连接现在有用户实物信息支持；前文“未见明确连接”是图纸审查历史，不再用它否定该接线。NS4150 CTRL高有效，正式音频初始化改用官方ES8311 driver的`pa_pin=21`，默认不反相；codec开启时驱动高，codec关闭时低。

USB JTAG因主机设备节点权限无法访问，未修改系统权限；改用临时本地测试固件。相同1kHz PCM峰值6000，在低/高/低/高阶段各提交3秒，GPIO21输入读回分别0/1/0/1，各阶段ES8311解除静音0x31=0x00、音量0x32=0xc6且读取错误码均0。用户改为直接拉高时测试已执行，采集进程中断后finally完成原100镜像恢复、独立校验与NVS相同确认；没有声学结论，不把GPIO读回当“喇叭响了”。临时main.c已恢复，测试源/日志单独私有归档`gpio21-test-20261117T100025Z`。

按最新指令随后刷入GPIO21高有效镜像，包含此前已完成的运行时音量和codec诊断，编译默认100。应用SHA256 `a3ba8d2f5840c6c5273e9d0da15f82fd7113099d6aa084578d2eff32b66ee247`，1440224字节；9/9主机测试与S3构建通过。USB/芯片MAC核对D6FD1C，实际分区表一致，仅写0x10000应用，写入/verify通过，NVS前后逐字节一致。使用同一串口连接完成刷写后启动捕获，未为读日志再次打开串口造成额外复位。

启动日志实际确认`PA GPIO21 high requested; input readback=1`和MCP Ready。这是GPIO数字输入读回，不是功放脚电压测量。归档`~/.local/state/caelus/hardware-backups/voice-d6fd1c/gpio21-high-20261117T100322Z/`，已释放USB并通知软件任务恢复目标、播放已有合成音频；Core/App和其他软件保持不变。软件随后通过正式Core→签名插件→设备lease播放固定hello-16000.pcm，156160字节/4.88秒、峰值16063，匹配output.drained。实板received与i2s均156160，volume100，last_unmute_reg31=0/error0/unmute_read_count1，volume_reg32=198（0xc6）/error0；启动16个寄存器读取全成功且与6.18期望匹配，fault=false/read_failed_count0。lease取消后网页语音恢复自动连接。随后按用户要求以设备音量100再次播放同一素材，匹配drained，received/i2s累计均312320（本轮各增加156160），last_unmute_reg31=0/error0、volume_reg32=0xc6/error0、unmute_read_count=2；结束恢复网页语音连接。用户确认已听到声音，并进一步明确“有声音，但破音、杂音或断续”。因此GPIO21使能后的实际出声已确认，但音质验收未通过；完整字节计数不能排除I2S供数间隙或模拟链路失真。随后通过现有官方音量接口设50，completed回执及state.volume均50，播放同一素材一次完成drained；received/i2s均从312320增至468480，reg32=0x94/error0，last_unmute_reg31=0/error0、unmute_read_count=3，fault=false且相关发送/寄存器读取错误均0。结束恢复网页连接，实际音量保留50（diagnostics.configured_output_volume=100仅为编译默认）。50音量听感尚待用户确认，不预先认定功放削顶或网络为原因。

用户随后要求更长语音且最大音量：通过官方接口将实际音量设回100并核对state，同一hello-16000.pcm连续重复6次，在一个output轮次内实时发送29.28秒、936960字节/1464帧，匹配drained。received/i2s均由468480增至1405440（各增加936960），fault=false，reg32=0xc6/error0，last_unmute_reg31=0/error0，unmute_read_count=4，寄存器读取及上传队列/调度/发送错误均0。测试结束恢复网页连接，音量保持100；本轮听感待用户确认。

用户表示更换喇叭后要求重播：保持实际音量100，同一素材重复6次共29.28秒/936960字节，匹配output.drained；received/i2s均从1405440增至2342400，增量完整，reg31解除静音读回0、reg32=0xc6且读取错误均0，fault=false。换喇叭后的听感待用户确认。此次播放前已有上行历史异常：queued=13440、sent=5760、queue_high_water=10、queue_full_count=1、send_max_us=1485872、last_failure_packet=pcm、reason=upload_queue_full、generation=19，schedule/send_failed均0；本轮full_count未增加，不能归因于本次下行播放。取消lease后首次MCP状态读取RemoteProtocolError，finally已恢复网页连接，随后只读复查ready=true且volume=100；未重复播放或改动固件。


### 6.20 松键断连定位与TCP_NODELAY单变量候选（2026-11-17）

真实按键复现：input.start后第一组3200字节约1.4秒才到盒子，云端发送耗时仅1–2毫秒；断开前没有收到input.end/commit_begin/commit_sent。随后设备ready=false/fault=true，last_stop及last_fault均upload_queue_full，full_count由1增至3，failure_packet=pcm，generation=29，queued=44160/sent=23680，send_max_us=1485872；调度/发送失败计数仍0。已经确认PCM上行队列溢出，但网络阻塞的具体原因仍待单变量验证。

固件正常松键只依序入队最后PCM和input.end，再置awaiting_response，不主动关闭。10项上行队列约200ms（控制事件同样占项），非阻塞入队失败会关闭通道。upload_work运行于HTTP server任务；IDF6.0.2的httpd_ws_send_frame_async实际上先后同步发送header和payload，名字async不保证非阻塞。input.end日志在入队失败时也会打印，不能代替盒侧接收证据。

候选仅在语音HTTP server的open_fn设置并读回TCP_NODELAY=1，失败由HTTP server拒绝该新连接。未改Wi-Fi省电、队列容量、协议、音量或SDK。main/voice_socket.h复用官方socket API；真实loopback accepted socket主机测试验证选项0→1、重复设置与无效fd失败。10/10主机测试和S3构建通过，应用SHA256 d66d89ac7901289416341973a39d739637f24c33df07f09762c466407c568bfe，1442400字节；经软件审阅并暂停释放目标后已仅刷入应用，独立verify通过；原应用分区备份且核对旧镜像a3ba8d…，NVS前后相同，启动MCP Ready及GPIO21输入读回1。USB已释放，归档为`~/.local/state/caelus/hardware-backups/voice-d6fd1c/nodelay-20261117T102424Z/`。尚无刷后真实按键通过结论。预期验证先无云接收多轮3–5秒和15–30秒长按，再接云验证commit/回复；检查start/PCM/end完整、queued/sent增量一致、溢出计数不增长及send耗时，不用主机测试代替实板结论。

NODELAY单变量实板无云测试仍失败：新连接握手成功、初始计数全0，收到input.start但无input.end即关闭；queued=13440/sent=6400、queue_high_water=10/full_count=1、send_failed=1、send_max_us=1209555，generation=1、failure_packet=pcm、首个stop=upload_queue_full。因此NODELAY单独不足，未宣称修复成功。下一方案是保留当前NODELAY基线、仅Voice禁用Wi-Fi modem-sleep并读回模式，尚未实施。官方默认MIN_MODEM只是文档/源码依据，当前设备实际get_ps值尚未取得。I2S持续空闲读取、每次320双声道采样对应20ms，4个160采样DMA约40ms，接收完成队列3项；可短突发，未发现数秒积压机制，也尚无运行时间隔实测。

PS_NONE诊断候选已构建：仅Voice媒体启动时get_ps→set NONE→get_ps验证，并输出前后模式、错误码、Wi-Fi工作模式/RSSI及其获取错误码；读取失败或模式未改变不视为成功。NODELAY保持，缓存/超时/音频数据不变。新增主机测试覆盖每一步API失败与写成功但读回不变，11/11测试及S3构建通过。应用SHA256 `82771d91020b8ab942e1adac53e3df1d0f0be03b4bb5500babcaf68ca88832e2`，1443200字节，未刷入；候选归档`/home/autograph/.local/state/caelus/hardware-backups/voice-d6fd1c/ps-none-candidate-20261117T102958Z`。软件先采集当前NODELAY基线TCP元数据，设备保持原运行镜像。

PS_NONE候选错误处理修订：省电设置或读回失败仅输出诊断无效警告并继续现有媒体服务，不新增整板复位条件；仍以before/after/error判定该轮是否有效切到NONE。替代上一未刷候选，最新SHA256 `225d126d8634a7211124914c93192410b45d17572b67e66382c32cc955e28ce3`，1443216字节，11/11测试及构建通过；归档`/home/autograph/.local/state/caelus/hardware-backups/voice-d6fd1c/ps-none-nonfatal-candidate-20261117T103055Z`，尚未刷入。软件基线TCP元数据显示接收序列缺口及重复ACK，当前不据此直接断定RF故障，继续核对完整时序。

PS_NONE刷写前备份受阻：软件暂停释放后，USB读取现应用连续三次Packet content transfer stopped（460800整段、115200整段、115200按64KB分块）；三次都在任何write_flash执行前失败，未部署225d126d候选，未写NVS。最后一次异常路径执行hard-reset并释放串口；fuser未发现占用。当前仍应为已验证NODELAY镜像d66d89，需盒侧复查上线。已请用户重插USB，不能将后续现板测试标为PS_NONE。失败日志保存在ps-none-20261117T103407Z、ps-none-20261117T103430Z、ps-none-20261117T103501Z私有归档中。


### 6.21 对照小智后的上行生命周期修正（2026-11-17，代码候选未刷）

用户要求依据小智设计重评，双方重新审查官方固定版本 `78/xiaozhi-esp32@aaf1eb7798cabee87a0c6a0649aabf4d17e9e383`。硬件仓库及祖先目录无适用AGENTS；只读上游AGENTS/音频架构后核对实际实现：

| 项目 | 小智固定版本 | 此前CMPP Voice | 当前代码候选 |
|---|---|---|---|
| 采集与编码 | AudioInputTask→encode queue→OpusCodecTask，16k/60ms Opus | capture task读取16k PCM，每20ms上传640B | 保留PCM格式和采集任务，本次没有移植Opus、AFE或唤醒 |
| 上行排队 | audio_service.h发送上限40包≈2.4s；audio_service.cc满时丢最旧音频、继续编码 | PCM和控制共10槽≈200ms，满即关闭整个通道 | PCM120槽≈2.4s，另4个控制槽；统一顺序，只淘汰最旧待发PCM，控制不随音频淘汰 |
| 松键 | HandleStopListeningEvent发送listen.stop并离开录音态，不在该路径关闭audio channel | 尾PCM与input.end争最后一个槽，可能满队列断线；失败也打印end | 尾PCM后顺序排end，控制保留槽避免被PCM挤掉；日志只在入队成功时输出queued |
| 发送调度 | 应用事件负责网络发送，与音频编码分工 | HTTP callback循环直到队列空 | 仍是单HTTP writer，每次work仅一帧，后继work保留一次ticket，允许HTTP任务处理入站事件 |
| 故障/拥塞 | 有界实时丢旧音频；端点实际失败另处理 | 短时背压变成连接故障 | 音频容量压力只计drop，不伪装无损；socket真失败/断网仍清理连接 |

官方对照链接：[采集/编码队列](https://github.com/78/xiaozhi-esp32/blob/aaf1eb7798cabee87a0c6a0649aabf4d17e9e383/main/audio/audio_service.cc)、[容量与Opus参数](https://github.com/78/xiaozhi-esp32/blob/aaf1eb7798cabee87a0c6a0649aabf4d17e9e383/main/audio/audio_service.h)、[PTT/应用生命周期](https://github.com/78/xiaozhi-esp32/blob/aaf1eb7798cabee87a0c6a0649aabf4d17e9e383/main/application.cc)、[WebSocket发送](https://github.com/78/xiaozhi-esp32/blob/aaf1eb7798cabee87a0c6a0649aabf4d17e9e383/main/protocols/websocket_protocol.cc)。这是按上游实时排队原则修正PCM桥接，不宣称完整复制小智；Opus可实质降低32KB/s上行负载，但需双方共同确定能力协商、帧长、解码和flash预算，未随本候选实施。小智的listen.stop应用调度顺序也不等同本项目统一FIFO的尾PCM→end约定；保留现有双方约定。

实现与可观测性：`voice_upload_queue.c/h`在固定槽池中移动索引，不复制整段音频，控制槽满独立记录`upload_control_queue_full`；工作票据在reset后保留，已排队或发送中的旧callback负责交还，避免第二writer和新会话卡住。原generation检查保留。`voice.channel.diagnostics`从16项增加到19项，新增`upload_pcm_dropped_frames`、`upload_pcm_dropped_bytes`、`upload_control_queue_full_count`。`upload_queue_full_count`在本候选中也记录PCM淘汰次数，已不等于断连次数；新drop不更新last_failure，历史真正故障仍保留。queued/sent仍分别为成功入队累计及发送API成功累计，不把差值全部视为丢帧（其中也可能有待发送和断连清空）；高水位包含控制，最大124。所有计数继续since_boot并饱和到uint32最大值。

容量与内存：S3 ELF类型实际sizeof(packet)=652、sizeof(upload_queue)=81232字节；CONFIG_SPIRAM未启用，当前走内部8bit heap，未擅自启用PSRAM。保留原10帧下行队列。链接报告DIRAM静态129812、链接余量211948字节，并不表示联网/任务创建后的实际可用heap。新增启动allocated bytes/free_internal/largest_internal日志，仍须实板测量。分配或任务创建失败回收资源；Voice初始化失败不再让main整板复位，标故障、停用语音并保持网络和MCP管理。真实运行内存尚未验收。

测试：11/11主机测试与S3构建、diff检查通过。生产queue/schedule/lifecycle函数回归包含40ms/3s/15s样本序列、200/754/1500ms发送停顿模拟、超2.4s后仅丢旧PCM并计数、控制/尾帧顺序、124槽高水位、第5个控制槽独立失败、已排队/发送中的旧callback跨reset隔离、schedule错误释放票据、注入socket失败后清理。模型时间推进和错误注入不等于真实RTOS/网络验收。新增状态字段验证使用非零计数。尚未连接USB/刷入/实板按键，旧板仍运行NODELAY基线。未部署的PS_NONE实验代码和无用helper/tests/stub已移除，不混入当前修正。

候选应用SHA256 `dec4f491f0ffbd5fac888df92748b0c826f25e81bc7c0f4efcae68c09314dfde`，1443600字节，应用分区余92400字节；私有归档`/home/autograph/.local/state/caelus/hardware-backups/voice-d6fd1c/xiaozhi-queue-candidate-20261117T110139Z`。软件任务已另行完成App/插件的实际loopback协议测试，最终审阅与整机实测结论以双方后续共同回执为准。


### 6.22 小智队列修正版实际更新（2026-11-17）

软件任务审阅通过并通过API暂停、释放唯一Voice后，执行设备侧原镜像MD5、小块flash重复读取、MAC/分区及当前NVS读取预检，全通过。未再重复传输整段旧应用；使用已验证的NODELAY镜像作回退。随后仅写0x10000应用，独立verify通过，NVS前后逐字节相同。实际部署应用为6.21的`dec4f491f0ffbd5fac888df92748b0c826f25e81bc7c0f4efcae68c09314dfde`，1443600字节。不是PS_NONE候选，未改变Wi-Fi省电策略。

启动实际确认GPIO21高读回1，120PCM+4control队列分配81232字节，音频/队列初始化后的内部空闲heap154016字节、最大连续内部块112640字节；随后MCP Ready。该heap记录点在全部HTTP/MCP运行时任务创建完成之前，不代表网络/实时音频峰值余量。90秒同一串口句柄只读日志采集完成，未见Voice初始化ENOMEM；脚本exit0，USB已释放，已交软件任务恢复网页会话和核对SDK状态。本轮未做额外声音或物理按键测试；尚不能将上线等同断连已修复。

归档`~/.local/state/caelus/hardware-backups/voice-d6fd1c/xiaozhi-queue-update-20261117T110421Z/`包含新app、已校验回退bin、NVS前后、sdkconfig、flash/boot日志和metadata。Core/App未重启。之前三次read_flash失败尚无物理线故障结论，本次受控小块预检和应用写入均成功，无需用户额外重插。


### 6.23 修正版两轮真实SW2语音验收（2026-11-17）

软件任务恢复网页生产千问Voice后，App与官方SDK均ready，19项diagnostics存在且初始为零，Core/Web主进程未重启。随后用户按统一指引完成两轮真实SW2对话，明确反馈“两次均正常，有声音”。软件任务只读官方SDK回执如下：

- ready=true、fault=false；last_fault_reason=none、last_stop_reason=none。
- 上传queued/sent均282880字节，下行received/I2S均232960字节。
- upload_pcm_dropped_frames/bytes、upload_queue_full_count、upload_control_queue_full_count、upload_work_schedule_failed_count、upload_send_failed_count均为0。
- upload_queue_high_water=71（含控制项），upload_send_max_us=929249。

结论限定为修正版的两轮现场语音通过，期间未见断连、队列溢出或计数显示的PCM丢失；用户确认实际出声。发送仍曾耗时约929ms，高水位达到71，不能据此宣称LAN抖动已消失，或所有网络条件下永不丢音频。长按、耐久、LED观感和高负载运行内存未作本轮新验收。按双方约定收尾，不再打开USB、追加物理测试或修改固件；现板保持`dec4f491...`修正版。

### 6.24 运行中 SW1 重新配网修复（2026-11-17）

用户报告运行中长按无法重新配网。根因是`voice_recovery_boot()`只在`app_main`初始化阶段运行，之后没有SW1监听；且配网完成释放BLE运行内存，不能直接重新调用配网初始化来解决。

- 新增运行按键任务，在启动恢复、LED初始化之后、阻塞等待联网之前启动。因此已配对、Wi-Fi离线等待、配网未完成均可监听SW1；不依赖语音codec、媒体或Core在线。
- SW1实际GPIO17，低有效。启动监听后必须先松开，再按住至少5秒、不足12秒并松手；推荐6–8秒。输入按下/松开均50ms消抖、10ms采样。5秒后LED常亮表示可松手；达到12秒取消并恢复原灯态，必须松开再按才开始下一轮。运行时没有恢复出厂动作。
- 松手后将NETWORK恢复意图写入NVS、commit并读回，成功才`esp_restart()`。正在运行的服务不直接修改Wi-Fi/身份；重启后在服务启动前沿用现有journal清理Wi-Fi、置未配对并重开BLE/SoftAP。保留UID、Token、名称、hostname、音量和其他项目配置；清理失败保留意图供下次启动重试。
- 重配网会中断当前语音连接，不需要拔USB或断开电池。SW2按键说话、GPIO21功放使能、音频队列及媒体协议保持既有实现。图纸没有独立复位按钮，SW3是GPIO18电源键；RESET接U4第4脚CHIP_PU/EN，不是GPIO。J2的1脚RESET、2脚GPIO0，不应将J2两脚短接当复位。
- 主机11/11测试通过，新增覆盖运行时短按、5秒/12秒边界、启动残留按住、卡键、释放抖动、取消后重试与LED优先级；旧出厂二次确认、恢复中断/保留项测试继续通过。S3构建通过；候选应用SHA256 `c0140e3bf3d33673030cd9cda2195b07fd0d4db6e9d146254e67117919ac0bc9`，1444592字节。

已刷入D6FD1C并释放USB：芯片MAC/分区表/旧app摘要及小块读回预检通过，仅写0x10000应用，写入与独立verify-flash通过，24KiB NVS前后逐字节一致。普通启动日志确认`Runtime SW1 enabled: GPIO17`、GPIO21高电平、音频队列初始化及MCP Ready；内部heap检查点free=150888、largest=110592字节（不是峰值负载验收）。归档`~/.local/state/caelus/hardware-backups/voice-d6fd1c/runtime-pairing-update-20261117T113112Z/`含app、已核验回退镜像、NVS前后、构建配置、刷写/启动日志和metadata。

已请用户执行一次运行中SW1手势；实物SW1时长/LED观感、运行长按后重新广播及网页重新添加、仅电池下软件重启供电保持仍待现场反馈，不能仅凭主机测试写成通过。软件任务收到新约定及部署成功通知，负责更新页面；Core/App无需重启。

软件回执：网页已热部署新运行中SW1说明，旧固件启动方法折叠保留；390px浏览器隔离回归通过LAN-only指引、BLE连接表单，无设备写入。Core/App PID95408持续active，未重启；现场手势验收仍由硬件任务等待用户反馈。

后续重添加联调：软件记录11:42:44 UTC本轮`device_complete`完成；此前配网窗口内开发机和盒侧均仅见旧PTR、hostname未解析且旧IP超时，不能据此断定固件mDNS故障。complete后开发机两个接口均解析`caelus-44b176d6fd1e.local`到`192.168.0.104`，服务UID与paired正确；无鉴权GET MCP返回401，说明端口可达且要求认证。软件于11:44:25 UTC官方产品verify通过，UID正确、profile=`caelus.voice-terminal.v1`。这些证据确认重新配对后的LAN和身份核验恢复，未单独证明SW1物理时长、LED观感或仅电池重启。

当前语音状态另记：软件报告Core活动voice任务为0、插件lease遗留opening，设备ready=false/fault=false，语音尚未恢复。软件已为旧bridge取消后占用未释放添加实际asyncio任务done_callback清理，9项测试通过；运行适配器尚未加载修复。因旧监督器会连带重启全套，按用户约束未重启Core/App/adapter；硬件未重启、未刷写或再次配网。网页自动能力读取已热部署并通过隔离回归。下一步由软件恢复通道，恢复前不请求用户按SW2测试；不能把LAN核验成功表述为整条语音链路已恢复。

后续恢复回执（覆盖上一段运行状态）：用户明确批准一次维护重启，软件于11:56:15 UTC完成并加载broker清理修复，监督器PID157148。网页voice=ready，官方设备state ready=true/fault=false；产品“重新连接语音”返回connecting，约3秒后回到ready。设备授权、模型配置保留，硬件未刷写、未操作USB/配网/SW2。当前确认通道及重连状态恢复，尚未追加本次恢复后的真实采音/播放验收；现场验证由软件任务统一协调。

### 6.25 回复后断连与 HTTPD 默认超时修复

用户在维护恢复后报告成功回复一句随后断连。软件实际读到`ready=false/fault=true`，`last_stop_reason=last_fault_reason=send_failed`，上传queued=324480、sent=209280字节，high_water=121、full_count=3、dropped_frames=3、send_failed_count=1、send_max_us=1822925；下行received/I2S均261120字节。之后产品自动重连回ready、Core活动任务1。以上是自启动累计诊断，最大耗时不是失败帧专属耗时；不能直接推断唯一根因为socket超时，也不能把下行全部交给I2S等同于声学验收。

只读代码结论：

- 标准WS Ping/Pong已支持，HTTPD显式向handler传递控制帧；正常`output.end`排空后发`output.drained`，不主动关通道。host应在drained后发listening，否则`output_busy`仍为true时会state_conflict。drained入队与清output_busy在同一锁内，无正常收到drained后仍busy的发送竞态。
- 音频/控制上传worker和Ping/Pong handler均在单一HTTPD任务执行，没有两个线程交错写帧；阻塞send期间会延后接收和Pong处理。每次work仅发一帧，再允许处理session。`upload_send_failed_count`只在上传worker记录，Pong发送失败不计入它，因此该计数1说明队列帧发送确实至少失败一次。
- PCM达到120帧时丢弃最旧PCM继续运行；4个控制槽单独保留，high_water含控制。3次PCM替换本身不关闭连接。本次记录的是send_failed，而非queue_full关闭。
- 当前部署`c0140e3b…`把HTTPD收发socket等待都覆盖为1秒；本地ESP-IDF6.0.2的`HTTPD_DEFAULT_CONFIG()`原值均为5秒。这是单次I/O等待，不是空闲1秒就断连。无协议要求必须缩成1秒，故本次仅删除`config.recv_wait_timeout = config.send_wait_timeout = 1;`，继承SDK默认值。保留真实发送失败后的关闭处理，避免部分WS头/载荷已写出后在同连接盲目重发整帧。
- SDK`httpd_ws_send_frame_async`实际同步发送头和载荷，仅检查负返回值，未验证正数短写；当前无本次短写证据，也未修改SDK。未新增重发/重试。小智固定参考SHA `aaf1eb7798cabee87a0c6a0649aabf4d17e9e383`中`websocket_protocol.cc`把发送委托`WebSocket::Send`，`application.cc:265`发送失败丢弃剩余待发送音频；它不是本板HTTPD服务端栈，不能将本次5秒默认值说成照搬小智底层超时。

11/11主机测试、S3构建及diff检查通过。候选SHA256 `45d2dae9eadffd4d786d872ab03596567b79d7cbdfc28b1dd1e692759009d5f8`，1444592字节；归档`~/.local/state/caelus/hardware-backups/voice-d6fd1c/httpd-default-timeout-candidate-20261117T120519Z/`含应用、sdkconfig、单行补丁、metadata。**仅代码和候选产物完成，尚未刷入；实板仍是c014版本。** 本轮未开USB、未复位、未配网、未重启Core/App；软件继续排查云端发送阻塞向上游传播的可能性。现场改善与完整根因仍待联调，不能写成已修复断连。

后续部署：按软件任务传达的本轮修复/维护安排，先由软件调用正常产品暂停接口，返回HTTP200/status=paused后，硬件于12:07 UTC开始更新同D6FD1C。MAC、分区、旧c014摘要和重复小块读回预检通过；只写0x10000 app，独立verify-flash成功；新app首4KiB含描述符读回与候选一致，24KiB NVS前后逐字节一致。运行启动ELF前缀`f6710080e`与候选完整摘要`f6710080e3959bff27601d0223d383306529bed667b587ca21b77fd028c38fc6`相符，Runtime SW1、GPIO21高、120帧队列及MCP Ready正常。USB已关闭释放，语音暂停状态未再改，由软件统一恢复并核对产品状态。**当前已部署45d2版本，覆盖上段“候选未刷入”的历史状态；尚未证明断连已消除。** 归档`~/.local/state/caelus/hardware-backups/voice-d6fd1c/httpd-default-timeout-update-20261117T120736Z/`包含回退c014镜像、NVS前后、flash/boot日志和metadata。硬件未操作Core/Web，也未追加用户SW2测试请求。

部署后的实测回执：软件恢复产品voice约3秒后ready；设备ready=true/fault=false，重启后的诊断初值为0。用户随后发起实际通话，官方state上传queued/sent均178560字节，下行received/I2S均1122560字节，发送失败/丢帧/队列满计数均0，持续ready=true/fault=false。`upload_send_max_us=1487619`，超过旧1秒配置的数值，但本轮未断开；它仍是完整发送调用耗时，不是单次socket timeout的直接测量。盒侧约160秒仅记录TCP头的诊断中，同一媒体端口37052，FIN/RST均0；观察到设备零窗口后恢复，未关闭连接。证据支持本轮真实双向传输和观测窗口连接通过，不仅是ready；尚待用户两句最终听感，不将单轮结果推广为所有网络条件稳定，也不宣称已唯一证明旧超时为根因。Core157171/adapter157159保持运行，软件仅重启已获批准的Web加载两项修复；硬件保持45d2，无追加操作。

### 6.26 SW2 播放中打断（固件已部署，实板打断待验收）

用户明确报告播放时按键无效；现部署45d2按6.9半双工约定屏蔽播放/等待窗口PTT，盒侧也拒绝output_turn非空时的input.start。双方现共同改为按键立即停止旧播报并开始新输入，不能仅解除板侧按键限制。

兼容协商与下行屏障：

- 新板`ready`保留type/protocol/channel_id，额外`features:["ptt_interrupt_v1"]`。新盒识别后发送`{"type":"session.configure","interrupt":true}`启用；旧盒不发送则维持原半双工，旧板不广告则新盒不发送。不增加用户配置步骤，不改CMPP身份/权限。
- 启用后每个`input.start`仍只带type/turn_id，并作为新输入和取消旧回复的信号。盒须作废旧模型response及迟到输出/工具回调，在同一有序下行发送路径停止旧输出后，发`{"type":"input.accepted","turn_id":"in_N"}`。这不是等待云模型回复的ACK，而是不能再越过它发送旧PCM的屏障；已经完成的外部动作不因打断被假定撤销。
- 板检测有效按键后立即静音、清播放队列并提升独立playback_generation，连已出队但未执行的旧PCM/尾标记也失效。TX disable后将全部DMA预装零样本再enable，RX不禁用，避免同步等待60ms静音排空；实际切换延迟与音频首字仍需实板验证。
- 板同步开始新input.start/PCM，**不等accepted才采音**。pending期间及按住录音期间消费并丢弃合法旧output.start/end/PCM/session.state；匹配最新in_N的accepted只清pending，不结束采音；旧/重复accepted忽略。新input.end之后且最新屏障已到，才允许下轮播放。Ping/Pong正常处理；重连清协商和等待状态；未新增固定超时。
- 未协商时原严格顺序、半双工及诊断保持；中断丢弃旧播放队列后received/I2S差额不再可直接当作网络丢音。仍无声学AEC/语音唤醒打断，当前目标是物理PTT打断。

硬件代码及候选S3构建通过，11/11主机测试通过，新增协议测试覆盖旧盒默认、协商边界、播放中开新输入、早/迟/旧ACK、连续打断、录音时阻止旧下行、断线清状态。主机任务另做模型取消、response_id代际过滤、串行屏障及真WS模拟测试；两端就绪前不单独刷板，不请求用户重复测试。实际静音、DMA切换、播放出队竞态及新一轮人声完整性尚未现场验收。

候选应用SHA256 `589eae885f08e03f9f7c16cdd609b455f5911b36268170ee736cded8c05023bf`，1448672字节，归档`~/.local/state/caelus/hardware-backups/voice-d6fd1c/ptt-interrupt-candidate-20261117T122247Z/`。软件回执17项终端回归通过，包含真实本地WS模拟播放中立即输入、旧音频/工具/完成事件迟到和同连接下一轮；另补工具执行中打断竞态检查。此为代码及模拟验证，不替代板上和云模型联合验收。

后续部署（覆盖前述未刷入状态）：用户明确允许本次联合加载，软件最终18项终端回归通过，并修正旧response.done后迟到/陌生response_id误归当前代的问题。产品API返回HTTP200/status=paused后，硬件于12:33 UTC执行589e应用更新；目标MAC、分区、原45d2摘要和重复小块读回预检通过，仅写0x10000，独立verify-flash及新app首4KiB读回通过，24KiB NVS前后逐字节一致。启动ELF前缀`5861b79ba`匹配候选，GPIO21高、Runtime SW1、队列及MCP Ready正常。USB已关闭释放，产品暂停保持，由软件加载并恢复。归档`~/.local/state/caelus/hardware-backups/voice-d6fd1c/ptt-interrupt-update-20261117T123311Z/`含45d2回退镜像、NVS、校验与启动记录。尚未执行真实播放中按键打断，不能将启动成功等同于听感通过。

主机加载回执：仅重启本次获准的maintenance Web，新PID170349、HTTP200；Core157171/adapter157159保持原进程。产品connect已受理，但恢复遇到千问会话建立超时，由软件继续检查。尚不能记录为语音已恢复或打断实测通过。新版state未新增interrupt协商字段，不为验证另开媒体socket；实际按键测试由软件统一安排。

后续恢复回执（覆盖上一段连接状态）：网页已恢复ready，官方`cmpp://state`读回`ready=true/fault=false/last_fault_reason=none`，本次启动音频计数为零，尚无新一轮实测证据。软件已统一向用户发出播报中按SW2的测试步骤，等待现场反馈；当前仅确认连接恢复，不代表实际停播、采音完整性或旧回复隔离已通过。

软件另项回执：复现Qwen的`home_apply`把`actions[].entity_id`写为数组并使用错误亮度/色温字段，参数拒绝后重复调用。软件仅补共享`CONTROL.md`准确参数示例，同一合成语音在RK、真实隔离Core/插件及模拟SDK上一次完成亮度50%、色温4000K调用；这不是物理灯具验收，也不是本板采音/打断验收。通过正常产品重连加载说明，Web170349/Core157171/adapter157159未再次重启，未修改设备状态推断或增加自动重试。硬件589e保持不变，继续等待用户实物反馈。

上传积压另记：软件后续快照queued=450560、sent=291200、丢帧249/159360B、send_failed仍0、send_max约1.95s。249×640恰等于queued-sent差额，明确存在累计音频丢失，不能宣称45d2已全面解决。只读源码/配置确认HTTPD work无固定sleep，单次只发一帧后立刻再投；`CONFIG_HTTPD_QUEUE_WORK_BLOCKING`未启用；TCP SND_BUF/WND均5760B、NODELAY已开启。零窗口方向及重传/吞吐需结合TCP头时序再判断；未擅自改省电、队列容量或增加新重试。

已有约160秒、单端口37052的TCP头捕获早于上述丢帧快照，不能直接关联249帧损失。启发式统计中，设备载荷段至盒端首个覆盖其末尾的累计ACK中位约0.24ms、最大2.475s，39段超过200ms；设备和盒端分别有21/66条相同seq/len的重复载荷记录。这不是完整TCP重传分析，不能仅凭该统计认定根因。盒端广告窗口无零；设备零窗口属于下行接收方向。双方据此决定本次打断候选不夹带未经验证的TCP/省电/队列容量变更，上传完整性仍单列待查。

### 6.27 BLE 连接超时只读检查（2026-11-18）

软件报告RK侧09:13:08、09:14:06 UTC开始BLE连接，约15秒后TimeoutError，未进入协议版本、Security1或Wi-Fi写入；同时内核记录hci0 Opcode 0x200e failed: -16。该阶段尚无本轮Wi-Fi配置已提交的证据。用户经软件任务确认板一直通电且在盒子附近；软件此前独立电脑/RK扫描窗口均未发现Caelus。

硬件随后在开发机约09:17 UTC执行10秒BLE发现，实际收到`NEW Device 44:B1:76:D6:FD:1E Caelus-Voice-D6FD1C`及RSSI从-67到-53的更新；这是本窗口收到广播的证据，不只是缓存。缓存服务UUID匹配protocomm，Connected=no，未尝试连接。不同扫描时窗的可见性不同，尚不能认定是射频、占用或广播异常。开发机无Espressif USB枚举，也无目标串口，未打开串口、复位或刷写；无法读取当前板端日志。mDNS只见目标PTR提示，hostname解析超时，不能判为LAN在线。

源码检查：已部署589e包含运行SW1恢复；配对完成会拆BLE并释放内存，重新配网通过持久恢复意图及重启恢复BLE。NimBLE的protocomm广告使用BLE_HS_FOREVER，连接失败、断开、广告完成均重新启动广告；广告启动自身错误只记录并返回，无独立定时重试，但本轮无证据说明此路径发生。Linux本地头文件将0x200e定义为LE_CREATE_CONN_CANCEL，不能仅凭盒侧此报错断定板端初始化失败。既有Wi-Fi成功后重连窗口的待证问题发生在后续阶段，不作为本次BLE连接前失败的根因。未修改固件、配对凭据或RK服务；软件负责更正明确BLE失败被显示为“结果未确认”的提示，并继续连接侧诊断。

后续软件实测：RK的12秒bluetoothctl扫描收到目标NEW事件；电脑经Bleak按地址发现取得BLEDevice后成功建立GATT并确认配网服务，RK使用同样API也成功连接，建立连接耗时4.33秒。两端均已断开，未写Wi-Fi、Token或complete；未修改SDK或重启蓝牙。此证据确认这些时窗内广播发现及GATT可用，不能推断此前失败根因或全流程配网成功；软件继续按产品原样Transport_BLE.connect及proto-ver核对。硬件不另行扫描或连接。

软件最终诊断与只读验证回执：生产BLE示例给BleakClient传地址导致隐式重扫，改为传已发现的BLEDevice；广告UUID不恰好为一个时，原`services[None]`会在新Bleak的UUID规范化中抛ValueError，改为非空才查询广告UUID并沿用原fallback。两处兼容补丁已记录上游SHA，Security1及配网协议未改；插件本机15项、RK隔离14项、网页8项测试通过并部署按需加载文件。后续BlueZ设备对象消失，软件确认无活动蓝牙连接后仅重启RK bluetooth.service，未操作rfkill/Wi-Fi/Core/App。最终生产`Transport_BLE.connect + proto-ver`只读验证成功，总9.98秒，sec_ver=1且custom-data端点存在，随后断开。Core1594/插件1055/网页1615 PID未变；软件已请用户网页新提交配网，不自动重放旧密码。上述证明修复后本次连接和协议读取通过，实际Wi-Fi配置、complete及LAN验证仍待用户提交；硬件未刷写、复位或改变凭据。

后续09:35:33–38 UTC网页再次在ble_connect报BleakError，未进入Security1/Wi-Fi；前一次成功不代表持续稳定。软件独占BLE/HCI诊断，硬件仅审计源码，不扫描、连接或操作串口。代码证据如下，均不是对本次实物根因的认定：

| 路径 | 本地构建源码证据与边界 |
|---|---|
| 普通重复连接、断开 | IDF6.0.2 `protocomm_nimble.c:241–270`成功连接后不重启广播，失败连接及断开会重播。若旧链路仍有效，后续无广告符合该路径；须由HCI真实连接/断开完成事件佐证，不能只看BlueZ缓存 |
| 未完成安全握手便断开 | `security1.c:450–500`关闭并清空单个会话；新连接也先清旧会话，没有每连接新增会话对象的累计路径。特征响应按attribute handle复用，写入时释放前次响应，不是每次proto-ver无限累积 |
| 配网结束/释放BLE | 应用仅初始化失败或custom/portal complete后deinit；`scheme_ble.c:310–324`释放BLE内存仅响应DEINIT。仅连接、服务发现、proto-ver读取不会触发它；Wi-Fi自动停止需先收配置并获IP，SESSION_ONLY保留transport |
| 广播恢复失败 | `protocomm_nimble.c:200–227`设置广告/响应或启动广告失败后记录错误并返回，无主动定时重试；若命中且无后续GAP/sync事件，可能持续无广播。当前没有板端错误码/日志，不能判定本次发生，也未据此改固件 |

本轮未找到足以确认09:35失败由固件生命周期缺陷引起的因果；这不排除尚无运行证据的板端异常。已将具体路径交软件任务与其独占HCI记录对照。

软件后续HCI/BlueZ定位与回移验证回执：扫描停止时BlueZ主动InterfacesRemoved目标Device1及GATT对象，未见外部RemoveDevice调用，导致刚获得的BLEDevice随即不可连接。BlueZ5.64以广告Flags判断`device_is_connectable`，不含Flags的scan response将其覆盖为0，扫描结束清理temporary且非connectable设备；实测HCI广告0x0013/scan response0x001b均标为Connectable，MGMT scan response标志0x20也非NotConnectable。因此该设备对象删除问题有主机侧因果证据，不应归因于板端未发可连接广告。软件基于保留Ubuntu5.64安全补丁的版本回移上游提交`3fb4e1cca2eaddc413d1ebed9d862084351efdbc`，20组原生测试通过，仅替换独立bluetooth.service，Core/App/插件PID未变。部署后目标扫描结束被删除的问题未再出现。

连接稳定性仍未通过：随后三轮完整SDK只读测试为一次成功、一次超时、一次服务发现断开，追加一次proto-ver成功；RK RSSI约-90至-95dBm，软件同轮电脑扫描约-51dBm。此差异支持继续核查盒侧天线、接收环境及测试位置，不能单凭RSSI断言天线损坏，也不能把对象生命周期修复等同于完整配网稳定。软件报告诊断连接与抓包均已结束，未下发Wi-Fi/Token；硬件不额外访问设备，留给用户网页配网。

### 6.28 配网完成至LAN服务可达的观察（2026-11-19）

软件日志08:26:27 UTC确认wifi_provision完成，08:26:28确认device_complete及completion_store完成，UID及hostname保持。随后RK一度无法解析hostname或发现服务；约08:34 UTC已解析到`192.168.0.132`，网页自动读manifest及真实state成功，`ready=true/fault=false`。Core/App未重启，硬件未刷写、复位或连接设备。开发机没有目标USB串口，无法取得本轮板端启动时间线。

只读源码顺序：complete写paired并置事件位→1000ms回包宽限→BLE manager deinit→关闭SoftAP/DNS/配网HTTP→必要时等待GOT_IP→音频初始化→语音WS→MCP HTTP及端点→mDNS。故complete回包不是服务ready回执，mDNS失败也不能单独证明HTTP不可达。音频初始化返回错误时主流程记录Voice unavailable并继续管理服务；语音WS启动错误则触发ESP_ERROR_CHECK，发生在MCP启动前。本次没有这些错误的运行证据。

源码可见NimBLE stop等待host事件、HTTPD stop等待任务退出、失去Wi-Fi连接位后等待GOT_IP等无应用级总超时的等待点，但无证据将本轮时间差定位到其中一处。DNS stop直接删任务返回，音频I2C单次事务超时100ms；未发现固定约8分钟的启动延迟。11/17普通启动归档曾记录Wi-Fi连接4158ms至MCP4288ms，不能据此推定今日配网清理耗时。约08:34是首次观测到可达，不等同于板子此刻才启动服务；广播传播、查询窗口和网页验证重试时间也需计入分析。当前连接已恢复，延迟根因未确认，不以推测修改固件。

### 6.29 设备媒体与模型连接分离（软件候选，尚待加载）

软件按用户新要求完成App提交`d47db67`：Voice至Caelus/Core媒体任务保持，收到`input.start`才连接Qwen，建连期间将输入音频暂存于内存；Qwen空闲1007/180秒后不自动重拨，也不关闭设备媒体，下次PTT才重连模型。软件报告RK隔离21项终端测试及真实Qwen两轮合成语音、180秒空闲测试通过，正在等待用户批准仅网页后台加载。当前不能将候选行为当作已部署行为；无需固件变更、刷机或复位，硬件不接管设备。后续诊断须区分设备媒体断连与模型正常空闲关闭，不仅凭模型关闭认定设备peer_closed；板端LED具体表现仍以加载后的实际协议事件为准。

### 6.30 本地 MCP 结果构造与连接资源预算修复（2026-11-20）

软件侧被动抓包确认普通 `tools/call` 多次返回 JSON-RPC `-32002`，reason 为 `tool callback returned null`，并见 `resources/read` 的 `Resource read failed`；这些失败发生于模型连接前。SDK 的工具结果分配、深拷贝或最终 JSON 序列化失败可返回 NULL；普通 `channel_conflict` 应在正常工具结果内返回 `rejected/no_effect`。这些证据支持内存压力候选，尚未定位某一个实际失败的分配点，不能把所有断连归为同一原因。

已部署旧 `589e…` 的刷写归档配置确认：PSRAM 未启用、全局 socket 上限 10、SSE replay 深度 128。8 项 CMPP 幂等缓存是有覆盖释放的 RAM 环形缓存，不是无限增长或 NVS journal，但每项保存完整语音快照。目标 Xtensa ABI 实测 `cmpp_value_t=16B`、对象成员项 `8B`、缓存项 `528B`；完整快照有 81 个对象成员和 990B 的键字符串，8 份快照加缓存项至少 27,696B，未含字符串值、分配器开销及返回过程的临时 JSON 副本。上传队列另占 81,232B 内部堆；SSE 历史还复制完整命令通知，按条数限制而非字节上限。两个 HTTP 服务自身占 6 个 socket，两套 MCP 客户端各占 2–3 条连接再加媒体需 11–13 个总名额；这证明旧容量不足以保证该并发设计，但不能解释仅观察到三条连接时的全部错误。

本次用户明确授权修复和烧录。软件任务先通过现有产品接口暂停该 Voice；硬件确认 USB/芯片 MAC `44:B1:76:D6:FD:1C`、启动 UID `hw_3af24949f40555a2` 及名称，未操作 Light 或重启 RK 服务。大块 Flash 读取曾中断，随后分块备份完成：24KiB NVS 两次读取一致、分区核对一致、原应用摘要精确匹配 `589e…`。保留原 bootloader 和分区，仅写 `0x10000` 应用区域。

先使用独立、不启网络、不写 NVS 的测试应用验证 PSRAM：实测 AP 8MB、octal 40MHz；IDF 启动内存测试通过，四轮各 7MiB 地址相关数据写入/读回均通过，堆完整性检查通过。此为本单板测试，不替代所有硬件批次和温度条件下的长期验收。随后正式固件使用相同 PSRAM 模式和每次启动内存测试。

0.3.1 的接口和资源变化：

- `voice.channel.open/close/volume.set` 的 `result` 保留 15 个必要字段，包括通道、媒体参数、状态、故障摘要和音量；不再携带 `diagnostics`、`codec_readback` 两个嵌套树。完整诊断仍通过 `cmpp://state` 读取。软件任务已确认使用字段兼容。请求、鉴权、执行状态及 8 项幂等语义不变。
- S3 的 SSE replay 从 128 条降至 8 条，仅缩短事件补发历史窗口；不删除命令幂等缓存或设备当前状态。需要完整诊断的客户端主动读取状态。
- S3 全局 socket 上限从 10 调整为 16，容纳现有两个 HTTP 服务和两套 MCP 客户端加媒体连接。
- 仅将 CPU 使用的 81,232B 上传队列显式分配到 PSRAM；普通 `malloc` 仍使用内部堆，I2S DMA 缓冲不搬移。启动日志确认队列实际位于 PSRAM；每 30 秒记录内部 free/min/largest 和 PSRAM 可用容量供联调观察。

离线验证：11/11 主机测试通过，补充了精简命令回执、媒体参数、音量和完整只读状态的回归断言；既有 core 测试覆盖幂等重放和 200 次缓存循环的分配计数稳定。ESP32-S3 构建通过，应用 1,457,744B，小于原应用分区。

正式应用 SHA256：`796dc342d1c636b9024ab10466534678b2639cb5837686ef1655343b43b0f4bf`；ELF SHA256：`842f1e4580c3db91fef52d2cc774a455eb9c1a04615137522b78a9826e726e83`。写入校验、独立 `verify-flash`、应用首 4KiB 读回均通过；正式启动前 NVS 24KiB 与备份逐字节一致，分区未变。串口确认版本 0.3.1、ELF 前缀、`wifi=saved/pairing=complete`、原 UID/名称及 MCP Ready。相同 audio/queue 初始化阶段内部可用堆从 150,580B 提升至 228,644B，最大块从 110,592B 提升至 188,416B；这不是负载峰值或长期稳定性的保证。

私有验证归档：`~/.local/state/caelus/hardware-backups/voice-d6fd1c/resource-fix-20261120/`，含原应用、NVS 前后、分区、PSRAM 测试源码/镜像/日志、正式候选源码/配置/镜像及校验日志。NVS 不进入仓库或公开日志。软件任务负责在既有 App 连接上做三次暂停/连接和至少五分钟在线观察；硬件只读采集串口，不新建 MCP/媒体客户端，不发音频或调用模型。该观察结果另记于本节，不代替音频压力及最终听感验收。

联合观察结果：软件任务记录 UTC 04:12:51.641–04:18:05.813 连续 314.17 秒，63 个样本为 ready 50 / listening 3 / thinking 3 / replying 7，error/connecting/paused 均为 0；该 UID 新增 App/adapter 失败和 HTTP 错误均为 0。60 秒被动抓包 6064 包、丢包 0，MCP JSON-RPC error 为 0、8081 FIN/RST 为 0。用户在此期间实际按键通话，硬件串口记录至少两轮完整 input.start/input.end 和 output.drained，末尾另有一次开始；不代用户宣称听感通过。

硬件最终串口采样内部 free=160,592B、运行最低值=115,300B、largest=98,304B、PSRAM free=8,301,924B；稳定窗口中最大内部连续块维持 98,304B，未见分配失败或 panic。唯一 `Channel stopped: peer_closed` 出现在早先主动暂停时，恢复后未新增。整个串口窗口仍有 20 条 HTTPD 通用 socket 警告（104/128），没有端口标注，不能报告“所有底层零错误”或据此判定媒体断开；软件被动媒体观察未见 FIN/RST。

三次快速生命周期循环**未全部通过**：App 暂停立即返回后，VoiceBroker 旧任务尚未完成异步清理，新连接被主机侧以“设备语音通道已被使用”拒绝；软件任务确认这是主机竞态，未记为板端 `channel_conflict`。等待清理后恢复连接并完成上述稳定窗口。该竞态留给软件任务处理，本次未因此修改固件、重启 Core/插件/App 或操作其他设备。诊断监视均已结束，串口释放，Voice 保持现有连接。

### 6.31 按键与本地语音唤醒候选（2026-11-24）

> 本节至 6.31.2 为早期 WakeNet 方案和 0.4.0-dev 历史记录；当前 microWakeWord 模型、配置、烧录状态以 6.31.3 为准，勿再按下方 WakeNet 外部模型路径操作。

**确定要求与交付边界**：用户要求保留 SW2 按键说话，同时支持“小白小白”唤醒。ESP-SR 2.6.1 官方模型包和公开模型列表未提供该词；当前集成不是已训练好的“小白小白”产品固件。模型名默认留空，状态 `wake_available=false`、`wake_status=model_not_configured`，按键保持可用。不把另一唤醒词改名后当作对应模型，也不自动改成“你好小智”。最初开发阶段未检测到 USB 串口，未做烧录或声学验收；用户随后接板并明确要求烧录，执行结果见本节末尾的烧录记录。

**已实现的候选代码**：

- 复用 ES7210 MIC1 的 16 kHz / 16-bit / mono 采样；WakeNet 在独立、低于采播优先级的任务中运行。识别输入队列上限 12 个 20 ms 帧，队列与采样缓冲显式分配到 PSRAM，不在媒体锁下执行神经网络推理。
- 本地触发前只在板上处理音频；媒体 ready、没有播放/等待回复时才检测唤醒。身份仍来自既有鉴权和 UID，唤醒词不改变房间或权限。无媒体通道时不会自行建立连接或缓存待传对话。
- 按键、唤醒复用同一个 `input.start → PCM → input.end`；每轮仍使用唯一 `in_N`。按键优先，语音录音中按住按键可接管同一轮，不再次发 start；松键仍按原去抖逻辑结束。按键触发不被 VAD 截断。
- 语音触发保留最多 320 ms 的本地预录，触发成功后按时间顺序上传，减少检测延迟引起的首字损失；可能包含唤醒词尾部，不能据此承诺完全去除唤醒词。
- WebRTC VAD 用于语音触发后的结束判断。工程默认值为：累计至少 100 ms 语音后连续静音 1200 ms 结束；未检测到足够语音则约 5 秒结束；触发后采集上限 15 秒（另含最多 320 ms 预录）。15 秒限制同时在采音任务按墙钟执行，识别任务落后也不能无限录音。这些是可调实现参数，不是用户已验收的声学阈值。
- 播放、等待回复、按键录音期间停止语音唤醒，结束后保留 700 ms 恢复间隔，避免扬声器尾音触发。没有新增 AEC 或“说唤醒词打断播报”；实体按键继续使用原 `ptt_interrupt_v1`。
- 通道 generation、识别模式切换、读麦错误或队列溢出会使旧识别结果失效。队列溢出重置识别，不阻塞按键采播；未对过载条件下的唤醒成功率作保证。
- 模型配置缺失、分区未刷/结构不符、词条不匹配或初始化失败均保留 PTT，并显示不可用原因。只读 `cmpp://state` 新增 `wake_available`、`wake_status`、`wake_word`；命令回执仍为原 15 个字段，避免扩大缓存。`wake_available=true` 只表示模型/任务初始化完成，不等于媒体在线或声学验收通过。

**软件侧共同约定**：软件任务于 2026-11-24 复核 VoiceBroker 与 App 的 `terminal_audio.Audio.receive`，确认它们不读取 SW2 或触发来源，原协议可直接接收语音唤醒生成的输入，无需 source 字段或额外 MCP 命令。最后一帧 PCM 必须先于 `input.end`，帧长保持 1–640 字节且为偶数；回复仍按 `output.start` 的 turn_id 返回 `output.drained`。软件任务报告 11 项 Broker 回归通过，本次未部署或重启软件。 软件任务随后逐条复核本节并确认一致；上行 start/end 共享输入 turn_id，下行由 App 独立生成输出 turn_id，设备不得将输入 ID 当作 drained ID。

**模型接入步骤（对应模型未取得，尚未执行）**：

1. 取得适用于 ESP32-S3、ESP-SR 2.6.1 的单词条 WakeNet9 模型，目标普通话“小白小白”（`xiao3 bai2 xiao3 bai2`）。保留原始模型目录名、`_MODEL_INFO_` 和权重文件；本版校验元数据词条必须准确等于配置的“小白小白”。实际模型命名、所需 Flash/PSRAM、召回率和误触发率以交付模型及实测为准。
2. `bash scripts/build_voice.sh menuconfig`：在 `ESP Speech Recognition` 的 `Load models from external path` 配置只含该模型的目录；在 `Caelus CMPP Board` 设置 `CMPP_VOICE_WAKE_MODEL` 为真实目录名，保留 `CMPP_VOICE_WAKE_WORD="小白小白"`。不要同时勾选其他 WakeNet/MultiNet 模型。`CMPP_VOICE_WAKE_THRESHOLD=0` 使用模型默认阈值，需要调试时使用 40–99 的百分值。
3. 执行 `bash scripts/build_voice.sh build`，核对 `srmodels/srmodels.bin` 内恰好一个模型、版本/元数据/哈希及尺寸。权重改变后应重新生成模型打包产物，不能只复用旧 `srmodels.bin`。
4. 新分区表保持 NVS `0x9000/0x6000`、PHY `0xf000/0x1000` 和 app 起点 `0x10000`；app 扩至 `0x3f0000`，模型分区位于 `0x400000`、大小 `0x400000`。首次迁移必须核对设备 MAC、备份现有 app/分区/NVS，写入相匹配的分区表、app、模型并逐项校验，不能沿用 0.3.1 的仅刷 app 步骤。不得擦除 NVS；实际断电恢复仍待验证。具体写入命令在接板核对现有布局后执行。

模型来源与定制渠道：[官方模型列表](https://github.com/espressif/esp-sr/blob/master/wakeword_list.md)、[乐鑫社区 TTS 训练请求](https://github.com/espressif/esp-sr/issues/88)、[官方定制说明](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/ESP_Wake_Words_Customization.html)。本次只调研渠道，没有向第三方提交请求、接受协议或付费；官方渠道是否提供该词及交付时间尚未确认。

**已执行验证**：12 项主机测试通过，新增测试覆盖无语音超时、静音结束、句中停顿、持续语音最长时限、未 ready/阻塞时拒绝触发、语音录音不因松键结束、按键接管与同时触发不重复 start，以及只读唤醒状态/精简命令回执。使用官方 `wn9_hilexin`（元数据“嗨，乐鑫”）单独执行完整模型启用编译，app 为 1,810,112 字节、模型包 291,149 字节；这只是编译验证，不是目标词实现或识别准确率证明。参考配置/产物放在忽略目录 `build-voice/wake-reference-validation/`，没有烧录；默认配置重新构建为模型未配置状态。

**双方联调清单（全部待对应模型与实机）**：

| 项目 | 需保留的证据 | 负责人/状态 |
|---|---|---|
| 模型身份与启动 | 模型来源/版本/哈希、词条、实际 Flash/PSRAM、wake_available/status | 硬件；待模型 |
| 按键回归 | 按住录音、松开结束、等待/播报时原协议打断、SW1 重配网 | 双方；待实机 |
| “小白小白”触发 | 0.3/1/3 m、不同方向和说话人；分别记录尝试/成功次数、首字完整性与延迟 | 硬件；待实机，不先承诺阈值 |
| 自动结束 | 说完静音、句中短停、只唤醒不说话、持续说话到上限；PCM 尾帧早于 end | 双方；待实机 |
| 并发触发与旧事件 | 唤醒同时按键、语音录音中按键接管、断网/暂停/重连、迟到识别结果 | 双方；逻辑测试已过，实机待测 |
| 自唤醒与噪声 | 回复包含唤醒词、播报尾音、电视/风扇；记录误唤醒次数/小时 | 硬件；待实机 |
| 资源与恢复 | 常听+Wi-Fi+采播堆余量/最大块、识别队列溢出、I2S 丢样、看门狗、断电重启和 UID/凭据保留 | 硬件；待实机 |
| 盒子完整轮次 | ready → input.start → PCM → input.end → 回复 → output.drained，确认同 UID/turn 不串轮 | 软件协作；Broker 回归已过，端到端待测 |

#### 6.31.1 用户授权后的实际烧录记录（2026-11-24）

- 目标为 USB MAC `44:B1:76:D6:FD:1C`，显示名 `Caelus-Voice-D6FD1C`；只刷 `0x10000` app，没有写入引导程序、分区表或模型分区。实际 factory 容量 1,536,000 字节，当前 app 1,459,680 字节，容量校验通过。本次模型仍为空，不需要迁移分区；取得模型后再按上面的迁移步骤核对。
- app 版本 `0.4.0-dev`，SHA256 `ef613c41e83efc9928cf05b7593323a611bebe56d0da5654bbf99395f39fc963`。写入校验和独立 `verify-flash` 均通过；NVS、分区表、bootloader 的刷前/刷后摘要相同。备份时一次 USB 分块读取中断发生在写入前，随后重新连接并用设备端摘要验证完整 NVS、分区备份及既有 0.3.1 回退镜像后才开始写入。
- 启动实测：版本正确，8 MB PSRAM 启动内存测试通过，`wifi=saved`、`pairing=complete`，UID 保持 `hw_3af24949f40555a2`，自动连接到 `192.168.31.89` 并启动 MCP。约 35 秒时内部空闲 140,104 字节、最大连续块 96,256 字节，PSRAM 空闲 8,301,788 字节；这是启动观察，不是长稳或唤醒推理资源测试。
- RK3588 软件任务使用原配对凭据只读验证 `cmpp://manifest` 与 `cmpp://state` 成功：profile 为 `caelus.voice-terminal.v1`，`wake_available=false`、`wake_status=model_not_configured`、`wake_word=小白小白`。此次没有打开新的媒体通道、触发音频或重启 RK 服务。
- 未验证目标词识别、PTT 实际声学效果或新增 VAD 的实机表现；目标唤醒模型仍缺失。烧录不是“小白小白”功能验收通过。
- 私有备份位置：`~/.local/state/caelus/hardware-backups/voice-d6fd1c/wake-candidate-20261124-172434/`，包含回退镜像、已验证 NVS/分区备份、候选 ELF/bin/config/source、启动日志和元数据；不得提交凭据备份到 Git。固件来源是 `322a892` 基础上的当前未提交语音唤醒改动，不把此前远端分支误称为包含该版本。

#### 6.31.2 自行训练的可行路径（2026-11-24 调研补充）

用户指出可自行训练。修正前述限制：缺少现成 WakeNet 模型只限制当前 WakeNet 后端，不意味着 Caelus 必须等待乐鑫交付。官方 ESP-SR 提供预训练模型与推理库，所查公开仓库未提供可直接本地复训 WakeNet 的完整训练链路；自行训练可采用独立的开源引擎。

- 已核对 [microWakeWord 训练源码](https://github.com/OHF-Voice/micro-wake-word)，revision `4665173cd35f1cff9a61e06fc427f124766c488e`，入口为 `notebooks/basic_training_notebook.ipynb`。训练使用 16 kHz 单声道，导出可供 TFLite Micro 运行的量化流式模型；训练入口可用不等于目标中文词已经达到可部署精度。
- [现有模型库](https://github.com/esphome/micro-wake-word-models) revision `40ff33f57f8fc6ad71a75ef085abbf742495b225` 没有“小白小白”。现有 `.tflite` 模型与 WakeNet 权重不兼容，不可重命名后塞入当前 `voice_wake.c`。
- 已核对 ESPHome 当前 micro_wake_word 实现使用 `espressif/esp-tflite-micro` 1.4.1、`espressif/esp-nn` 1.5.0、`esphome/esp-micro-speech-features` 1.2.3。这是板端原生 ESP-IDF 适配的参考，仍需在本工程验证依赖/算子/实时性，不要求更换 CMPP 或部署 Home Assistant。
- 中文正样本可从 [MeloTTS-Chinese](https://huggingface.co/myshell-ai/MeloTTS-Chinese) 等中文 TTS 起步，再加入其他声线和独立真人测试；需配置中文对话、近音词、背景噪声作为负样本，并按说话人/声源划分训练和评估数据。仅在相同合成声线的随机切分上表现良好，不能证明真人远场唤醒可靠。
- 本机只读检查发现 RTX 5080、16 GB 显存，检查时空闲约 14 GB，磁盘空闲约 194 GB，具备小型唤醒模型实验的硬件条件。TensorFlow/CUDA 兼容性和中文数据流水线仍待实际运行验证。

本轮只完成源码、模型来源和训练/部署路径核对，没有启动训练、生成“小白小白”权重或再次烧录。后续自训练采用 microWakeWord 时，需新增/替换板端特征提取与推理后端，复用已经完成的 PTT/唤醒仲裁、VAD 结束和现有媒体协议；成功标准仍包含量化后的独立误唤醒/漏唤醒评估及板端压力测试。

#### 6.31.3 千问语料自训练与原生 ESP-IDF 接入（2026-11-24）

**当前交付等级：实验版，未完成真人声学验收。** 不将板端推理成功或合成语音召回率等同于家庭可用性。已训练“小白小白”模型，替换早期 WakeNet 后端；保持 CMPP 配网、身份、鉴权和 `input.start → PCM → input.end` / `output.drained` 约定不变。软件任务已确认无需新增触发来源字段或修改 App。

- 权重：`main/wake_models/xiaobai.tflite`，50,888 字节，SHA256 `feea9bbcbf97fedd7079e9f206abf4f0b3db3ac5bf066cd8418d068df3909b08`。模型 ID `xiaobai_micro_v1`，词条“小白小白”；不依赖 WakeNet 定制服务。语料、来源、限制及复现入口见同目录 README、manifest、验证/测试 JSON。
- 用户指定用千问/豆包合成中文；实际由千问 `qwen3-tts-flash` 生成 280 条 WAV、28 个音色，训练/验证/测试按 20/4/4 音色隔离。背景声为 Microsoft MS-SNSD 固定版本的 23 个独立文件，来源和哈希留档。云端临时下载链接、API key、配对凭据不入库。ASR 核对因现有凭据返回 403 未执行成功；没有人工逐条校听证明。
- 本轮修复 microWakeWord 上游训练器的样本权重广播错误，补丁和梯度验证保存在 `scripts/`。先前四轮不合格模型均未烧录。最终参数由验证集选定：阈值 0.90、连续 9 次 30 ms 预测、启动抑制前 33 次预测；测试集不参与参数选择。
- 验证集 50/64（未增强 14/16）识别，643.65 秒负样本 1 次误触发；独立测试 49/64（未增强 14/16），580.58 秒负样本 1 次误触发。负样本包含近音词、普通中文和公开噪声；这是短时离线测试，不能推算家庭每小时误触发率。长正样本在特征训练中排除但保留于外部测试。
- 原生固件使用 esp-tflite-micro 1.4.1、esp-nn 1.5.0、esp-micro-speech-features 1.2.3；ESP-SR 2.6.1 只保留 VAD 用途。16 kHz PCM16、30 ms 窗/10 ms 步长/40 维特征、每 3 帧推理一次；模型 arena 预留 256 KiB PSRAM。启动包含不播放、不上传的正/负 PCM 自检，失败时禁用唤醒、保留 PTT。
- `sdkconfig.voice.defaults` 模型 ID 已启用，主任务栈为 8192；旧 build 配置需同步。模型嵌入 app，无需额外写模型分区；原预留 model 分区保留。首次扩容 app 仍须备份、验证分区/NVS，不能越过旧 factory 容量直接刷 app。
- 保留 SW2/GPIO1 按键说话；唤醒只在媒体 ready 且不播放/不等待回复时常听。没有新增播放中语音打断或 AEC。声音结束仍按 VAD，按键接管不重复 start，输入与输出 turn_id 仍独立。房间/权限不从唤醒词推断。

**验证与联调状态（随实测更新）**：

| 项目 | 结果与证据 |
|---|---|
| 主机协议/仲裁/结束逻辑 | 12/12 通过 |
| 训练/固件特征一致性 | C 与 Python 对静音、音调、噪声逐帧一致 |
| 样本权重修正 | BCE 梯度验证负样本 16 倍惩罚生效 |
| 量化模型独立测试 | 见上方数据，存在漏检和误触发，不作产品验收 |
| 目标板烧录与 PCM 自检 | 0.4.1-dev 写入/校验通过；正样本触发、近音负样本未触发；20 ms 帧最大处理时间 10,995 µs，实际 arena 26,524 字节 |
| 原凭据、UID 与盒侧状态 | RK3588 原凭据鉴权通过；UID `hw_3af24949f40555a2`、profile `caelus.voice-terminal.v1`；wake_available=true / ready / 小白小白；既有媒体 ready=true，未另开会话 |
| 真人唤醒、真实 PCM 完整轮次、PTT 接管 | 待用户现场配合和双方日志核对 |
| 距离/方向/噪声、自唤醒、断电恢复、长稳/电池续航 | 待测；没有用软件测试替代硬件验收 |

软件任务只读确认 RK→D6FD1C:8081 的已有连接归属运行中 `caelus-home.service` Smart Home 适配器，VoiceBroker socket 有已连接端点；recording/playing/awaiting_response/fault 均为 false。没有重启服务、主动触发音频或新增竞争媒体会话。

短时串口观察另记录一次麦克风推理触发（非内置 PCM 自检）：`in_1` start，约 5 秒后 end，随后收到输出并 drained；尚无用户现场确认，不能判为目标词真人识别成功，也不能排除误唤醒。此次观察打开串口时默认 DTR 导致一次 USB 复位，第二次启动自检也通过；后续采集虽改为打开前设置 DTR/RTS=false，仍观察到 USB_UART_CHIP_RESET；因此串口观察不能视为无重启监测，也不是断电测试。观察期间未见推理失败、队列溢出或 panic；媒体负载时内部空闲 130,884 字节、最低 103,272 字节、最大块 79,872 字节，PSRAM 空闲 8,019,520 字节，仅为短时样本。

软件侧随后只读确认设备已回到媒体 ready 空闲，recording/playing/awaiting_response/fault 均为 false；累计上行入队=送出 171,520 字节（5.36 秒），下行接收=写入 I²S 274,880 字节（8.59 秒），上行丢帧/发送失败均为 0。RK 日志没有对应输出 ID，累计计数不能逐轮归因或判断波形是否静音，第二次 drained 也不能归到 `in_1`。当前仅证明存在媒体活动且设备正常收尾，未确认真人目标词或问答语义。

**实际烧录记录**：目标仍为 `44:B1:76:D6:FD:1C`，app 为 2,112,336 字节，SHA256 `91d769a6f2f523e960c67a6960d4f51a7b3053e70553ba3b751e60feff65fbfc`。写入 `0x8000` 分区表、`0x10000` app 并独立校验；NVS `0x9000/0x6000` 和 PHY `0xf000/0x1000` 刷前刷后摘要相同。

首次启动拒绝分区，原因是原 boot 镜像头声明 2 MB，实际 JEDEC 确认为 16 MB。核对旧 boot 与历史备份完全相同后，只将镜像头 Flash size 改成 16 MB 并重算尾部 SHA256，boot 可执行内容和 Flash 模式/频率不变，写入并验证。**后续首次扩容流程必须同时检查 boot 容量头，不能只检查 app 配置。** 修正后启动确认 0.4.1-dev、8 MB PSRAM、`wifi=saved` / `pairing=complete`，约 9 秒时 `Local wake ready`；采播队列/模型初始化后内部空闲 202,296 字节、最大连续块 159,744 字节（尚非稳定媒体负载值）。保留了原 codec 初始化的两条“尚未启用时 disable I2S”日志，不把其记为已修复。

私有备份 `~/.local/state/caelus/hardware-backups/voice-d6fd1c/micro-wake-20261124-182155/` 含刷前 NVS/分区/原 boot、回退 app、候选 ELF/bin/config/source、模型 manifest、启动日志；禁止提交其中凭据数据。原样回退需恢复原分区和 app，boot 16 MB 头兼容原小分区；如需精确恢复，再使用已验证原 boot 备份。本轮没有擦除 NVS、修改 eFuse 或烧录测试随机权重。

#### 6.31.4 唤醒一次后的连续对话（2026-11-24）

用户新增确定要求：一次唤醒后可连续说多句，安静一段时间再退出。0.4.2-dev 固件实现本地对话窗口，仍使用原“小白小白”模型（哈希不变）；新增黑/红/绿模型的离线训练任务不改变本次板端词条。

- 首次唤醒后启用连续对话；每轮回复播放完并经过 700 ms 尾音冷却，进入 VAD 候听，累计连续 100 ms VAD 语音才开始下一轮，并保留 320 ms 本地预录。静音不会定时生成空 input.start/input.end。
- 默认 30,000 ms 无新发言退出连续对话，重新要求唤醒词。录音、等待回复、播放期间刷新截止时间，不会让长回复耗尽用户下一次发言机会。参数 `CMPP_VOICE_FOLLOWUP_TIMEOUT_MS` 可编译配置为 5–120 秒。
- 退出只清除板端连续对话状态，不关闭 CMPP/WebSocket 媒体；每句话沿用独立输入 turn_id 和已有输出 drained 约定。掉线、关闭通道或重新配网时清除连续对话，旧 VAD 结果通过 epoch 丢弃。
- 继续保留 SW2 按住说话和按键接管。仍为半双工：播放期间不做语音打断；协商式 PTT 打断保留。电视或旁人说话可能被 VAD 当作下一句，未实现说话人识别。
- `cmpp://state` 增加只读 `conversation_active` 和 `followup_timeout_ms`；命令回执仍保持精简，不增加媒体消息类型。

软件任务确认正常 output.drained 不关闭终端媒体，Conversation 保持等待新 input.start；云端按需连接不要求每句话重新唤醒。App 四项按需连接回归通过，无需软件协议修改或线上重启。硬件主机 12 项回归通过，新增覆盖静音不启动、短噪声不累加、连续语音门限、长回复不提前超时、下一轮延长窗口和断线清除。目标固件完整编译通过。

已向 D6FD1C 仅刷入 0.4.2-dev app，大小 2,113,008 字节、SHA256 `6a6440715c86ef862a9c58ba17ce487f8fc13bf507bbb8a616b76993bcffe93c`，写入和独立摘要验证通过；原“小白小白”模型、NVS、boot、分区与 PHY 保留。启动确认版本正确、原模型正/负 PCM 自检通过（20 ms 帧最大处理 14,820 µs）、wake ready。备份位于 `~/.local/state/caelus/hardware-backups/voice-d6fd1c/followup-20261124-185216/`。RK 盒侧原凭据只读复核：conversation_active=false（空闲）、followup_timeout_ms=30000、wake_available=true / ready / 小白小白、媒体 ready=true、fault=false，没有新开竞争媒体会话。用户随后现场确认“两句都能正常回复”：先说“小白小白，你好”，等回复结束后不重复唤醒词，直接问“现在几点了”，仍收到正常回复。记录为单次真人两轮连续对话通过；30 秒安静退出、长稳、噪声与更多说话人仍待验证，不外推为整体声学验收。

#### 6.31.5 提示音、15 秒静默退出与 SW4 结束键（2026-11-24）

用户反馈续听会反复提醒，随后明确要求 15 秒无发言退出、唤醒提示音和随时按键结束；结束后仍可再次语音唤醒。软件任务现场只读观察两次提醒前各新增约 3.06/2.92 秒上行 PCM，证明两次均先由板端起轮，不能归因为盒子每 10 秒定时催问；尚未通过波形区分回声、背景音或真人。原软件没有定时 response.create；另修复零 PCM 输入仍触发回复的缺口。

已刷 0.4.3-dev，app SHA256 `ac68f8725314f73362dbf92510760da368e241ee1332cf089de12b1c6b9e31b0`、2,115,008 字节，NVS/boot/分区/PHY/小白权重均保留，写入和独立摘要验证通过。启动模型正/负样本自检通过，最大帧耗时 10,696 µs。备份位于 `~/.local/state/caelus/hardware-backups/voice-d6fd1c/stop-cue-20261124-191351/`。

- 默认连续候听改为 15,000 ms。超时安静退出，结束云端当前对话但保留媒体连接，需下次唤醒；录音/等待/播放不消耗下一句的空闲窗口。
- 唤醒只打开本地候听并播放 120 ms 短提示音，随后 VAD 检测到发言才上传 input.start/PCM；仅叫唤醒词不再立刻生成云端空指令。提示音无云端 TTS 请求、无 output.drained；提示音期间关闭语音检测并留尾音间隔。建议听到提示音后说指令。
- SW4（网名 IO48、GPIO48，按下低电平）短按结束：立即停止录音/播报、清待发 PCM/播放队列、清连续对话状态，再要求 App 丢弃未提交输入并取消旧云会话。按住不重复触发，不清配网；SW2/GPIO1 仍为 PTT，SW1/GPIO17 仍为配网维护。
- ready.features 增加 `session_stop_v1`；App 通过 `session.configure` 的 `session_stop:true` 显式协商。设备发 `{type:session.stop, stop_id:唯一ID}`，不发 input.end；App 静默回 `{type:session.state,state:listening,stop_id:原ID}`。确认前丢弃旧下行，匹配确认后可新唤醒。旧软件未协商时或 5 秒未确认时，结束键退化为关闭媒体通道，不向旧软件发送未知消息。
- 主机 12 项通过，新增 stop 屏障测试覆盖未协商拒绝、旧音频/普通 listening/错误 ID 丢弃、匹配确认、重复确认及新输入。软件 Application `7bb3fbe` 在本机和 RK 隔离环境各 27 项通过；用户明确授权“允许本次仅重载网页后台”后已加载，Core/适配器/插件进程保持。旧0.4.2配置报文形状亦验证兼容。
- RK 已验证原身份、媒体 ready、timeout=15000、wake ready、小白小白、fault=false；用户已确认听到提示音。SW4现场中止、15秒安静退出和重复提醒是否消失仍待现场逐项确认。0.4.3仍为半双工；MIC3/AEC及播放中语音打断是下一步候选，不是本版已验收能力。

### 6.31.6 重复提醒排查与 AEC 候选（2026-11-24）

用户在 0.4.3 提示音已确认后继续反馈“好像还是会一直提醒”。板端没有每十秒提醒定时器，但自动续听只需 100 ms VAD 阳性就开启新输入，录音/等待/播放会持续刷新连续窗口；环境噪声或回声误判可形成无限开轮。此前现场两次重复回复之前分别出现 3.06 / 2.92 秒新上行 PCM；这能证明新输入发生，不能判断音频里是真人、背景声或回声。

软件任务已完成另一个候选 3f4a4b6：终端 Qwen 3.1 Plus/Flash 在提交音频后等待对应 item_id 的最终转写，非空文字才请求回复，空白/纯标点/失败静默回 listening，隔离重复和跨轮迟到结果。两端各 29 项回归通过，RK 独立云调用验证非零噪声/数字静音均不请求回复、有中文才请求。此记录时生产尚未加载，需遵守用户逐次网页重载授权；该修复仍不能拒绝可转写的人声或喇叭回采。

硬件候选 0.4.4-dev：

- 启用 ES7210 的 MIC3 模拟参考（原理图 ES8311 输出回送），RX 四个 16bit TDM 槽取 MIC1/MIC3；TX 仍为 16bit 数据、32bit 槽，匹配共享时钟。MIC3 单独 0 dB，麦克风沿用 24 dB。
- 使用现有 ESP-SR FD_LOW_COST AEC；将引擎帧长衔接到 20 ms 输入。AEC 在采集任务运行，清理后的 PCM 同时用于唤醒、续听、上行。参考在功放之前，不能据此保证消除功放/喇叭失真的非线性回声。
- 续听需连续 200 ms VAD 阳性，且均方能量至少 6400、超过待机噪声底四倍；播放打断需 300 ms。阈值为初始工程参数，远场灵敏度/误起轮尚需实测。
- 只有协商 interrupt 且实测参考非零、连续 500 ms 输出能量衰减后才允许播放中语音起轮，播放开始另设保护时间。未满足条件仍可按键打断；不能把算法初始化当成 AEC 声学验收。
- 原 SW4/GPIO48 短按停止、匹配 stop_id 确认、15 秒本地安静窗口和唤醒提示音保留。状态新增 stop_supported/stop_pending、echo_available/echo_ready、mic_rms/reference_rms/clean_rms/echo_worst_us；echo_ready 是运行门控状态，不是验收结论。
- 12 项主机回归通过；`python3 scripts/test_voice_echo.py` 用确定性替代引擎执行真实桥接代码，验证六种引擎帧长、原地 PCM、顺序、FIFO 和参考门控，不验证 ESP-SR 消回声效果。已 app-only 烧录 D6FD1C，镜像 2,167,760 字节，SHA-256 `8dc2a9fb59fe42bcb78444e57b4938aa6cb3d94c92b74cc260f7cd6e51636a00`；回读校验与 NVS/boot/partition/PHY 不变检查通过。启动实际 RX TDM 4×16、TX 2×32 时钟配置成功，AEC 帧长 512 样本，板端正/负 PCM 自检 1/0、最大 20ms 帧处理 11,731µs；媒体 TCP 已重连。此处自检仅验证唤醒模型，实际参考信号、停止键与声学效果仍待确认。

RK 使用原配对凭据只读复核：媒体 ready=true、fault=false，stop_supported=true、stop_pending=false，已直接确认本次停止协议协商；15 秒配置不变。17.3 秒/16 次自然空闲采样未录/未播、无新上下行，mic_rms 23–123、reference_rms=0、clean_rms 17–158、echo_ready=false；没有播放时参考为零不代表线路失败。AEC 最大处理耗时 28,404µs，需按 512 样本/32ms 引擎块周期并结合采集溢出评估，不能仅凭最大值判持续超期。当前串口未见持续识别队列溢出或故障。

补充 RK 只读采样 30 秒/61 点：仍未捕获用户唤醒或对话，conversation_active/recording/playing/awaiting_response/stop_pending 始终 false，ready=true、fault=false，上下行及上传丢帧均为零。mic_rms 26–229、reference_rms=0、clean_rms 19–394、echo_ready=false；AEC 单次最大耗时升至 31,071µs，接近 32ms 引擎块周期，需在实际采播及唤醒并行时检查 DMA/识别队列溢出。该窗口不能算作唤醒后安静退出或声学验收，也不能据单个最大值判断持续超期。

当前验收缺口：仅唤醒后安静是否不再开云轮且 15 秒退出；正常两轮继续对话；SW4 在录音/等待/播放时停止且不串入旧音频；回复中真人插话；没有真人时喇叭播放不得自行起轮。尚未完成的项目不标记通过。

新增颜色模型交付在 [models/wake-colors/2026-11-24](../models/wake-colors/2026-11-24/README.md)，小黑/小红/小绿各为独立 INT8 模型，含各自阈值、音色独立测试和四词交叉测试。未替换现役小白，未烧录或声学验收；小黑漏检与小红负样本误触发仍有明确限制。

### 6.31.7 第一轮回复后续听漏检与采集失败排查（2026-11-25）

用户明确现象：“第一句有回复，接着说第二句没反应”。RK 生产仍为 7bb3fbe，空转写修复 3f4a4b6 尚未加载。16.4 秒/15 点只读采样：媒体 ready=true、fault=false，未处于录音/播放/等待/连续窗口；累计上传 558,080 字节，下行接收及写 I²S 各 1,362,880 字节，上传丢帧零且本窗口计数不变。历史 last_fault_reason/last_stop_reason 均为 capture_failed；没有对应失败时刻的串口记录，不能据此确定本次用户故障原因。

软件复核正常路径为 output.end → 匹配的 output.drained → listening，RK 生产代码 7 项隔离回归通过；工具前导播报后会等待后续答复，不能将这一等待当成漏发 listening。软件另记录云异常清理时可能与新录音竞态的路径，未现场捕获，本次未修改或重载盒子软件。

硬件 0.4.5-dev 候选包含：

1. 普通续听仍要求 VAD、RMS 至少 80、功率超过待机噪声底四倍，但从“连续 200ms，任意低能量帧清零”改为“最近 400ms 内累计 200ms 合格帧，当前帧也合格”。回归复现两个 100ms 音节中间一个 20ms 弱帧旧判定无法触发，新判定通过；孤立点击不能跨窗口累计。播放中打断仍要求连续 300ms，不随普通续听放宽。预滚从 320ms 增至 480ms，覆盖有界判定窗口与工作线程延迟。
2. I²S DMA 从 4×160 样本（40ms）增至 8×160（80ms），缓解 AEC 单块最慢约31ms再叠加媒体锁等待时的瞬时积压。TX 共用配置也变化，正常播放与提示音排空相应增加到超过完整 DMA 环的 100ms 静音；正常播放排空逐20ms释放媒体锁，允许采集、PTT/SW4及时执行。不能将增大缓冲视为已证明长时间处理吞吐充足。
3. 增加不含音频/文本的诊断日志：实际监听模式转换、每秒 VAD/能量门槛通过帧数、峰值与底噪、收到 listening/stop 确认、I²S 读失败/短读/DMA 溢出/AEC 桥接失败。用于区分状态未恢复、VAD漏检和采集异常，避免再次仅凭空闲快照判断成功。

验证：12 项主机回归、六种帧长 AEC 桥接测试与 ESP-IDF 构建通过。候选 SHA-256 `ae3c8a75c2a290cd51f338b3da16cc8776daae18cc0a766e933914ae5b4005fb`，已 app-only 烧录，镜像2,171,456字节，回读匹配；NVS/boot/partition/PHY不变。启动自检正/负1/0，最大20ms帧处理11,450µs，媒体重连。启动首次采集发现模型自检期间累计396次RX队列覆盖，闲置时丢弃该不连续帧；不能将此启动累计数当作会话中溢出。随后日志捕获输入流唤醒命中：t=14.365s启动本地窗口，14.765s进入续听，未发input.start；29.495s安静超时并发送session.stop，29.525s收到匹配确认，30.205s回到唤醒待机。该序列验证状态机安静退出与停止确认，尚未获得用户对这轮声学体验或两轮对话的确认。预期保留“小白小白”、提示音、SW4结束和现有身份/配网；本次不替换颜色模型。

RK 补充45秒/91点只读核对：原UID/profile、ready=true、stop_supported=true、stop_pending=false、15000ms配置，采样起点窗口已激活，9.08秒后退出（这是剩余窗口时间）；全程没有录音/播放/等待、没有新增音频字节，last_fault_reason/last_stop_reason=none。板端另一次t=56.855s唤醒命中后进入续听，71.965s安静退出，71.995s停止确认；两次窗口 VAD 均为0，不能将它们当作真人两句复测通过。用户随后明确反馈“现在是可以的”，确认0.4.5连续对话恢复；但同时反馈仍不能语音打断。此前历史 capture_failed 的具体成因仍未由对应波形/错误日志证明。播放打断进入下一项现场排查，不能因连续对话通过而标记AEC/双讲通过。

### 6.31.8 播放中语音打断门控排查（2026-11-25）

用户确认0.4.5连续对话恢复，但“说话完全不能打断”。RK复位前只读60秒/253点，其中207点正在播放，估算覆盖46.7秒；echo_ready全程false，未捕获播放中开始录音。MIC3参考确实非零，播放期mic/ref/clean范围19–28046 / 0–801 / 1–12174。62个播放点同时达到mic>=128、ref>=64，其中42点达到瞬时clean*2<mic。观测约4.2Hz，不能还原每20ms连续25帧；用户同时插话，不将所有clean残留归为回声失败。已确定首道echo_ready门控未打开，后续VAD是否满足尚未验证。

代码发现两项门控缺陷：AEC桥接将输出延后512样本/32ms，但原门控比较的是未延迟的原始输入；连续25帧要求遇到语音停顿/低参考会全部清零。0.4.6候选让原始mic/ref统计随输出同一FIFO延迟，再在1秒有界窗口中要求至少200ms真实参考帧，且这些帧上的麦克风RMS>=128、总输出能量低于总输入能量1/4（约6dB衰减）。无参考帧不参与衰减累计，不能由无参考时的降噪来获得就绪。echo_ready仍是工程门控，不是全场景AEC或双讲验收结论。

算法模式、增益、普通续听判定、15秒窗口及停止键不变；此次先修已有证据证明未开启的第一道门控，不直接跳过参考检查，也未放宽后续300ms播放打断VAD。统计对齐仅补偿本项目桥接引入的延迟，不声称测定ESP-SR内部或实际声学路径延迟。

主机桥接测试覆盖六种引擎帧长、原地处理和样本顺序、无衰减不就绪、无参考不就绪、参考过少不就绪，以及100ms发声/200ms停顿下真实衰减可就绪。12项主机回归、桥接测试和ESP-IDF构建通过；已app-only烧录，镜像2,172,352字节，SHA-256 `a0e6b2f9a24bfad79a246214d5ec9725ff268c91450686c92987cfe5050e900b`，回读校验通过且NVS/boot/partition/PHY不变。启动正/负PCM自检1/0、最大20ms帧10,786µs。启动模型自检期间累计384次RX队列覆盖，首次闲置读取丢弃；后续播放无插话稳定性和真人打断结果待实测。

### 6.31.9 音量为零与播放持锁导致采集积压（2026-11-25）

0.4.6现场播放测试途中用户反馈“现在声音太小了听不到”。RK用原配对凭据读到voice.channel.volume=0、ES8311 volume_reg32=6；按用户反馈仅将此设备volume.set为70，回执completed/committed，回读volume=70、reg32=168。空闲时mute=96、last_unmute=-1不等于播放时仍静音。当前没有证据确定谁或哪个先前操作将音量设为零，不归因于烧录；烧录前后NVS校验未变。恢复70后的60.05秒/191点没有实际播放，不能将这段空闲采样当作有声回声门控或打断验收。

0.4.6串口同时捕获播放期间RX DMA覆盖累计384（启动自检积压基线）→424，增量40，集中在约82–99秒；没有因正在录音而关闭媒体，但缺失PCM会妨碍持续AEC和播放中VAD。代码显示普通播放每20ms包仍在media互斥锁内等待TX硬件；即使此前排空阶段逐帧让锁，这条普通播放路径仍可阻塞采集取状态。0.4.7将cue/PCM/drain统一改为非阻塞I²S短写，保存已写样本偏移；DMA满时释放media锁并让出调度，再核对代际/连接/停止屏障后重试。停止或新一轮已清环时，旧包不得继续回填。单帧硬件无进展250ms才报错，不允许无限忙等。

验证：12项主机回归通过；`python3 scripts/test_voice_playback_transfer.py`编译实际生产传输函数，用确定性驱动验证部分/零写、样本无重发漏发、等待时让锁、新代际/关闭/停止屏障取消及250ms超时。候选编译通过，SHA-256 `c5bcda10fc27d21d4d10127a8e764881cf174ac8748d0f1783a8f1f14c75c423`。已app-only烧录，镜像2,172,688字节，回读匹配且NVS/boot/partition/PHY保持；启动正/负PCM自检1/0、最大帧10,908µs，媒体TCP已重连。启动自检积压RX计数389作为基线。RK一次原凭据只读确认ready=true、fault=false、stop_supported=true、volume=70，音量重启保持已验证。读取时未录未播；非零音量播放时门控就绪、播放期采集不再溢出、无插话不自触发、真人插话停播及后续回复仍需现场补录；不能将代码回归视为声学通过。

### 6.31.10 打断后故障快闪与采集实时预算（2026-11-25）

用户明确“快速闪烁，接着说话没反应”。0.4.7串口已捕获打断真正进入新一轮：78.125秒echo门控就绪；79.325秒进入BARGE，79.795秒新input.start in_2，82.525秒input.end，83.565秒播放新回复，85.045秒drained并收到listening。之后in_3录音于91.315秒RX覆盖→capture_failed，in_4同样失败；另一次138.095秒播放中起in_6，138.265秒RX覆盖→失败。没有该轮录音内容，不能判断此前所有语音起轮来自真人还是残余回声，但用户确认打断后发生异常。

故障时LED flags=0x22（网络+故障，100ms亮/100ms灭）；重连握手后flags=0x06（网络+就绪，每2秒短闪100ms），所以需要区分故障快闪和正常待机心跳。用户此次描述属于前者。RK随后只读确认原UID、ready=true/fault=false已重连，历史last_fault_reason/last_stop_reason=capture_failed，echo_ready=true、音量70，上传丢帧/发送失败为零。AEC历史最大调用49,174µs，超过32ms引擎块周期；单个最大值不代表所有块超期，但与板端RX覆盖一起证明实时余量需要继续核对。

发现现役构建仍为CPU160MHz、COMPILER_OPTIMIZATION_DEBUG，PSRAM40MHz。0.4.8候选将CPU配置为芯片支持的240MHz、使用性能优化O2，保持PSRAM时钟40MHz和现有检测算法；增加echo_average_us（每次320样本桥接调用的累计平均耗时，不能误称512样本引擎每块平均值）和采集read+AEC/等待媒体锁/持锁工作三段最大耗时，便于区分计算过载与调度阻塞。不是通过关闭故障灯或继续提交丢失音频掩盖问题。CPU性能调整的实际负载余量、持续功耗/续航仍需相应场景测量。

12项主机回归、六帧长桥接及非阻塞传输取消回归通过。全局O2首次构建暴露固定MCP SDK 2.0.1两处strncpy诊断；已核对目标缓冲由零初始化/calloc保证尾零，因此保留该非音频组件原有Og编译级别，未关闭告警或修改上游SDK，音频/DSP组件继续O2。构建和app-only烧录校验通过，镜像2,148,496字节，SHA-256 `8a94ed923bc9f7a904e8459b4c183d93452ed96cdf656ade083342df93feb273`；NVS/boot/partition/PHY不变。启动实际cpu freq=240000000 Hz，自检正/负1/0、最大20ms帧10,100µs，媒体TCP重连。启动自检积压RX覆盖基线320，须与后续运行增量区分。有声双讲、打断后完整录音及无RX覆盖现场验证仍待补。

### 6.31.11 独立打断响度门槛与播放队列背压（2026-11-25）

用户提出：需要响度阈值，避免其他人的声音容易打断；使用距离约半米到一米。响度只能减少较轻/较远声音的干扰，不能识别说话人或保证距离边界。同样响的近处人声、残余回声仍可能触发。

0.4.9默认barge_min_rms=1000（去回声后PCM RMS数字刻度，不是声压级dB SPL，也不是百分比）。仅打断BARGE阶段（播放中或等待回复）使用此门槛，仍需AEC就绪、VAD、连续300ms和相对噪声底条件；普通唤醒、续听的80 RMS门槛及15秒窗口不变。1000是初始工程值，半米到一米真人通过率/背景声误触发尚待实测；不是已校准的距离承诺。

新增独立能力`voice.barge_threshold`，只声明命令`voice.channel.barge_threshold.set`，严格参数`{rms: integer 80..8000}`，拒绝缺项/额外字段/小数/越界。新能力状态与紧凑回执为`{barge_min_rms: 当前值}`，voice.channel详态亦新增同字段；voice.channel旧命令schema/回执形状不变。仅录音、播放、等待、提示音、连续窗口、等待stop确认均结束时允许保存，忙碌返回voice_busy；dry-run无写，写NVS mcp_light/barge_rms成功后才更新工作线程门槛，同值已保存时不重复写。启动读回持久值，没有持久值才用构建默认。

软件已在隔离App→真实Core→签名插件→模拟CMPP设备验证新命令动态目录、80/450/8000原样下发，79/8001/80.5未下发；通用网页表单可读取整数范围，无需新增页面代码或重载。此项是隔离兼容性证明，不等于真机保存或声学验收。

0.4.8现场已出现多轮BARGE→input.end→新回复，没有此前的capture_failed序列，但111.679秒出现playback_queue_full，随后停止/排空带来RX覆盖。0.4.9对PCM和output.end入播放队列提供最多250ms背压等待，期间释放media锁，保持音频包代际，停止/打断后旧包由播放器丢弃；短突发不再因零等待入队失败立即断开，持续满队列仍会明确报错。

验证：12项主机回归、独立阈值边界/普通续听不变、设置事务六类忙碌拒绝/dry-run/提交失败/幂等、真实播放传输函数短写/等待让锁/取消/有限队列背压，以及六帧长AEC桥接均通过。构建和app-only烧录校验通过，镜像2,150,160字节，SHA-256 `8f7fcb493f9c4e9c9e0706f028258fd96cc8903d75c16b01dfd9f474c9e50119`，烧录保持boot/partition/NVS/PHY。软件任务使用原凭据验证真机独立能力及两处1000状态；dry-run 1200返回no_effect且状态仍1000，实际set 1000返回committed，再次同值返回no_effect。随后一次复位串口确认`Barge threshold RMS=1000 source=nvs`、CPU240MHz、正/负PCM自检1/0（最大20ms帧8,346µs）。阈值设置时音量70、fault=false，但媒体ready=false；复位后原App会话仍为error，未自动恢复。软件任务确认既有devices/cmpp/voice connect仅重建指定UID会话，经原后台接口对本设备执行一次重连，App error→connecting→ready，设备独立回读ready=true/fault=false，适配器至8081仅一条连接；未重载服务、未建竞争媒体客户端或主动录播。复位后两处阈值1000、音量70均保留。半米到一米声学门槛及长时间稳定性仍待实测。

### 6.31.12 取消唤醒词，SW2发起及按键+人声打断（2026-11-26）

基于已推送428a396修改，候选版本0.5.0-dev；本次范围为源码、回归、编译及提交推送，不刷板。以下取代历史自动唤醒及无按键语音打断行为，设备现役版本不因此改变。

- 空闲时按住SW2/GPIO1开始录音，松开发送；首次输入即激活连续对话。正常回复后直接续问，保留VAD结束与15秒本地静默退出。超时后须再次SW2发起。
- 播放或等待回复期间按住SW2只进入检测，不清播放、不发input.start、不上传。AEC就绪（播放时）、VAD与响度/相对噪声底、连续300ms条件满足后，才递增播放代际、丢弃旧播放队列及DMA、发独立turn_id的input.start，按先预滚后当前帧顺序上传；松开发input.end。等待回复但没有播放时不要求有播放参考。
- 预滚扩至720ms，覆盖300ms打断检测（续听400ms窗口）、最多240ms工作队列及主任务取结果延迟；检测模式变化清旧预滚，松手使识别epoch失效，迟到结果不得触发打断。硬件调度长暂停、溢出和实际首音保真仍须实测。
- 只按不说、没说就松手均不通知盒端；即使旧回复自然结束，仍按着的这次按键继续要求人声，不能自动变成空输入。未按键的播放阶段不运行起轮检测，旁人/扬声器声音不会走该打断入口。按住时AEC/VAD不能识别说话人，残余回声与他人声音的声学误检仍属待验收项。
- SW4/GPIO48主动结束、session_stop_v1、output.drained和旧回复屏障保持。无interrupt协商、stop待确认、离线均禁止新轮；播放结束后的700ms普通续听抑制保留。媒体长连接及CMPP身份/配网不变。
- 产品构建不编译voice_micro.cpp、不嵌入模型/正负自检PCM，移除TFLite与micro-speech-features直接依赖。历史训练代码和模型归档保留。voice_wake模块名暂保留以缩小变更，内部只运行VAD/端点/噪声底；esp-sr为VAD/AEC保留。取消唤醒提示音及其不可达播放分支。
- 兼容状态字段保留，固定wake_available=false、wake_status=disabled_by_design、wake_word为空。软件任务确认App/插件不以这些字段限制媒体或续聊，盒端29项回归通过，无须改协议或部署。

验证：12项主机测试通过（新增按键+人声、静默长按、松手迟到、首次按键、连续轮和停止/离线门控）；实际capture发送分支提取测试验证静默不取消、确认后仅一次取消/start、环形预滚顺序及尾帧先于end；播放短写/背压/取消、五类忙碌设置事务和六帧长AEC桥接通过。ESP-IDF编译通过；ELF无voice_micro、TFLite、xiaobai及正负自检PCM符号，VAD/AEC符号存在，ESP-SR模型包仅4字节零条目头。构建记录：ESP-IDF6.0.2，应用1,516,672字节，SHA-256 `c9249f3792c6c3f66965abe1046858db2a68a697838f3b2292418d2829e13ec1`，镜像仅在本机构建/归档，未刷板。实板清单：首次SW2录音及回复；免按键连续两句；15秒静默退出后人声不再起轮；播放中静默按住/松手均不停播；无按键旁人/扬声器声音不打断；按住说话仅一次start、首字完整、松手后新回复；旧回复自然结束时静默长按不新建输入；等待云端回复时按住说话可打断；SW4在录音/等待/播放均可结束；不同音量下AEC、队列及断线恢复稳定性。未刷板，以上均不得标记实机通过。

### 6.31.13 回到纯按键对讲（2026-11-27）

用户要求“按住按键就说话，松开就回复，回复中按住按键就打断说话”。基于decc3a3实施0.5.1-dev，保留后续音频队列/取消修复，不直接回退整个Git历史。

SW2/GPIO1经过原有两次采样消抖后直接input.start，播报/等待期间同样立即递增输出代际、清旧队列与DMA，再录新轮；不要求AEC就绪、人声或响度达标。保持按住期间不会重复start，松开消抖后最后PCM先于input.end，盒端随后回复。仅保留20ms按键预滚，不使用720ms人声检测缓冲。未按键不建立输入、不上传，不再免按键续聊或VAD自动结束；每句话均按住说、松开发送。SW4主动结束、15秒空闲结束当前会话、媒体长连接、interrupt/stop协商与旧输出屏障保留；超时只清会话，不恢复自动收音。

产品移除VAD工作任务、起轮/端点辅助代码及门槛判断，AEC仍用于收音质量，不参与按键许可。历史voice.barge_threshold能力/NVS数值兼容保留并标明当前纯按键模式不使用，避免破坏已有目录和配置。wake_available=false、wake_status=disabled_by_design、wake_word为空保持。

盒端任务核对：已协商ptt_interrupt_v1时新input.start即可打断，input.end提交当前输入，无需软件协议修改或部署。本次未刷板、未重载任何服务。12项主机测试、实际capture发送分支的按键取消/单start/预滚及尾帧顺序、播放背压/取消、设置持久化事务和AEC桥接检查均通过。ESP-IDF6.0.2编译通过，应用1,503,648字节，SHA-256 `5e36ade00fd91410eb6d0559fa1ec7d6116456492d44d06c16003c57fb141bfa`；ELF确认PTT/AEC存在，VAD起轮、keyword及TFLite符号不存在。

实机待验收：普通按住录音/松开回复；播放中仅按键即停止旧回复并完整收新句；等待云回复时再次按键；不按键时人声/背景声不触发；长按不被静音/VAD切断；SW4及断线恢复。没有本版烧录/实测记录，不得将主机测试当成实机通过。

### 6.31.14 SW1短按结束对话（2026-11-27）

按用户要求，0.5.2-dev保持纯PTT、不自动续聊，结束键从SW4/GPIO48改为SW1/GPIO17。图纸网名IO36不等于GPIO36。SW1按下经过两帧消抖即调用既有stop路径，停止录播/等待、清旧输出并使用协商的session.stop；无需长按。SW1按住时阻止SW2开始输入，结束后须重新松开再按SW2。SW4不再触发结束。

SW1原有启动维护与运行中长按配网代码不变：长按先结束当前对话，持续5至不足12秒后松开仍重启进入配网；普通短按不清凭据、不重启。12秒以上的运行中长按按原逻辑取消配网，启动恢复出厂仍沿用原确认手势。媒体线程与维护线程只读取同一GPIO，均使用输入上拉配置，互不写GPIO电平。

本次仅源码/构建，未刷板。待实机：录音、等待和播放时短按SW1均停止；不误进配网；SW1按住期间SW2不发输入；SW4不再结束；5至不足12秒维护手势与启动维护仍正常。12项主机回归通过（含SW1长按边界、配网取消与断电恢复逻辑），ESP-IDF6.0.2编译通过；应用1,503,648字节，SHA-256 `929a2c8b7fc460e731a6f24e7710c98ce6ac22408ebe2496e826463bcaf4d572`。GPIO映射已核对初始化与读取均为17，未作实板按键验证。

## 7. 验证清单与历史整机项目（所有未执行项均未通过）

当前只选取 T01～T07 中语音板基础检查、T10 的连接鉴权/握手、T12 的音频断线清理部分进行语音通道验证。T08～T09、T11、T13～T19及身份/家居业务验证均暂缓；下表保留原编号供后续引用，不表示本轮要完成整机交付。

每项记录：硬件版本/BOM、芯片完整料号、固件commit、RK镜像/内核、供应商配置、测试条件、日志或波形、结论及负责人。日志不得包含密钥或未经授权的家庭录音。以下时长/距离是建议测试条件，最终通过阈值需双方与用户确认。

| ID | 项目与方法 | 通过依据 | 负责人/现状 |
|---|---|---|---|
| T01 | 核对BOM/PCB网表、PSRAM、H01分压和H02电源域 | 电压/引脚均符合实际器件手册 | 硬件；待资料/实物 |
| T02 | 限流启动，测3.3V/AVCC、复位、充电/电池/USB切换 | 无过压、倒灌、异常温升或音频峰值复位 | 硬件；待测 |
| T03 | 读取Flash/PSRAM容量，内存压力与音频/Wi-Fi并行 | 与BOM匹配，无内存错误/持续分配失败 | 硬件；待测 |
| T04 | I2C寄存器读回；逐麦输入与MIC3参考分离录制 | 通道顺序、极性、增益、底噪和削顶范围有记录 | 硬件；待测 |
| T05 | 测MCLK/BCLK/LRCK；16k采播及24k云回复转换 | 无时钟冲突、倍速/慢速、持续丢样 | 双方；待测 |
| T06 | 按住/松开/短按/连按/启动或断线时按住；记录最后音频帧、语段结束与云commit顺序，含不足一帧的尾音 | 不丢首尾字，不把松键当取消；提示与录音一致，尾帧先于该语段提交 | 双方；待实现/测试 |
| T07 | 安静/电视/风扇，手持与桌放，0.3/1/3m及多方向 | 记录唤醒成功率、误唤醒/小时、首字丢失与响应延迟 | 硬件；待测/定阈值 |
| T08 | 低/中/高音量播放时唤醒、双讲、连续打断；同一连接abort后注入旧轮迟到音频/文本/工具回调 | 参考正常，无自唤醒/持续回声；旧轮输出被隔离，已提交动作不被重放或假定撤销 | 双方；待测/定范围 |
| T09 | 连续采播与Wi-Fi运行，常听/PTT两种模式功耗 | 测实际续航/温度/峰值，不用额定电池直接推算承诺 | 硬件；待电池/喇叭资料 |
| T10 | 可信WSS、配对/撤销/重配对、身份；冷启动/慢网下hello与云ready时序 | 无管理员Cookie借用/跨用户记忆；撤销拒绝连接；握手及有限缓存符合冻结策略 | 软件；待实现/测试 |
| T11 | 两房间同时会话，同房间同时唤醒、不同回复 | 不串音/串房间，仲裁行为符合冻结约定 | 双方；待定策略/测试 |
| T12 | Wi-Fi掉线、AP重启、盒子重启、跨连接及同连接旧轮迟到回调、网络恢复 | 旧音频/文本/工具回调隔离，新会话可用；未知家居动作不盲目重试 | 双方；待测 |
| T13 | USB协调器枚举、稳定路径、用户权限、RTS/CTS | 类型/固件/速率/流控一致，同一设备重插能定位 | 双方；未选型号/待测 |
| T14 | 协调器缺席启动、启动中插入、运行中拔插 | 明确显示不可用，能按规定自动或手动恢复，无双进程占用 | 软件恢复缺口待修；硬件待测 |
| T15 | 实际Zigbee配对、查询、开关及网络恢复 | 分别记录命令受理/提交回执、SDK状态上报、物理动作观察；submitted且state_confirmed=false不等于物理成功；恢复保留网络关系 | 双方；待实物，建网/清网须明确操作 |
| T16 | RK冷启动、不登录启动、断网启动与子服务崩溃 | 用户scope/沙箱有效，服务可恢复并可诊断 | 软件+硬件；未上板 |
| T17 | 写入中断电、供电抖动、再开机 | 身份/数据库可恢复，unknown不重放，Zigbee不被重建 | 双方；待测 |
| T18 | ESP32首次配网、重刷后配对恢复、升级中断电、异常恢复下载 | CMPP UID/配对记录可恢复或明确重配对；媒体按冻结绑定建立，能救砖且不重放旧音频 | 双方；后续项，待方案/测试 |
| T19 | 建议24h连续压力起步，再扩展到多日家庭运行 | 记录RSS/PSS/CPU/温度/磁盘增长/延迟，阈值冻结后判定 | 双方；待实物 |

## 8. 尚需收齐的信息与协作记录

用户已确认：有 PSRAM、已有实物且已连接开发机，当前只做语音通道入口。已通过esptool读取S3、8MB AP_3v3 PSRAM和16MB Flash；板级核对仍缺完整丝印、PCB/BOM、麦克风及喇叭规格，以及实际电压测量。电池续航优化、协调器选型、整机部署及全屋仲裁资料本轮不追问、不作为入口原型的前置条件。

2026-11-17：硬件任务已向软件任务发送现状差异；软件任务已确认浏览器与终端协议/身份缺口、Core没有媒体流API、ZHA默认硬件流控、Zigbee启动失败恢复缺口及RK部署条件，并指定硬件任务统一维护本文。

第二轮：软件任务已只读复核第5～8节，认可软件现状、16k单声道Opus/60ms双向原型候选和未执行的T01～T19清单；提出的7项修正（abort语义、尾帧竞态、hello时序、同连接迟到输出、物理验收分层、缓存路径、首次引导协议选择）均已合入。软件复核不代表重新审定电气设计，不是生产选型批准。新终端服务端尚未新增；候选固件、用户可见权限与多终端策略仍待冻结，本文件不是整机验收通过证明。

范围修订：按用户最新指示收敛为单板语音通道，整机扩展项目暂缓，以第0节为准；已同步软件任务。

接入修订（此前调研阶段）：用户要求基于本项目CMPP SDK。硬件任务已对照实际API和配网/身份代码，将第6节改为CMPP语音能力及受控媒体通道；小智协议不再默认沿用。当时仅为设计，未修改固件或盒子程序。

单板实施阶段：用户将范围明确收敛为自动配网和连接传音后，硬件任务新增S3按键PCM固件，软件任务新增盒侧命令行鉴权/收发客户端。具体共同协议与已执行测试以6.7为准，实板电气、配网与采播验收仍未完成。多房间/空间权限及云端对话没有随此实施。

CMPP复核：软件任务已确认盒侧客户端/发现/凭据导入缺口，并认可`voice.channel`、盒子主动连板端媒体端点、PCM16LE/16k/20ms及有序语段边界作为最小原型方向。设备端与盒侧同名CMPP的不同绑定、队列限制、未就绪反馈及尾帧规则已补入第6节。具体Core适配承载、媒体端点schema、加密/首次信任仍待实现前冻结；没有启动软件改造。
