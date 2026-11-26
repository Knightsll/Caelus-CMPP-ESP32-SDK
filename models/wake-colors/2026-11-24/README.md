# 三色唤醒模型实验交付（2026-11-24）

三个独立 INT8 流式模型已实际训练/导出；没有烧录，没有替换现役“小白小白”模型。每个子目录含 model.tflite、SHA256/门限 manifest、原始语料清单、训练配置、验证与测试记录。它们仍是实验结果，不能作为家庭场景可用性验收。

| 模型 | 固定概率门限 / 连续30ms预测数 | 独立测试原始正例 | 含增强正例 | 负例时长 / 误触发事件 |
|---|---|---|---|---|
| 小黑小黑 | 0.99 / 3 | 11/16 | 39/64 | 902.18秒 / 1 |
| 小红小红 | 0.80 / 5 | 16/16 | 64/64 | 894.16秒 / 5 |
| 小绿小绿 | 0.70 / 7 | 15/16 | 59/64 | 902.24秒 / 2 |

小黑存在明显漏检，小红仍有普通中文/近音负例误触发；不因四词互不混淆就判定产品合格。测试包含所有原始正例（训练过长样本过滤不影响外部测试），负例包括另外三种颜色目标词、普通中文、近音短语和公开背景声。

四模型×四词采用完全相同的音频输入，4种独立测试音色×4种目标词表达=每格16条；矩阵每行是模型、每列是实际说的词：

| 模型＼实际词 | 小白 | 小黑 | 小红 | 小绿 |
|---|---:|---:|---:|---:|
| 小白（原模型） | 14/16 | 0/16 | 0/16 | 0/16 |
| 小黑 | 0/16 | 11/16 | 0/16 | 0/16 |
| 小红 | 0/16 | 0/16 | 16/16 | 0/16 |
| 小绿 | 0/16 | 0/16 | 0/16 | 15/16 |

这是64条合成语音的交叉检查，不是对不同房间、真人或电视背景的保证。完整逐条记录见 cross-testing.json。

## 训练与复现

复用 scripts/wake_qwen_corpus.py，--color black/red/green，--cache-dir 指向小白语料缓存。原始每色280条（28音色），新增合成616条，其余普通中文/黑白近音复用缓存；使用 qwen3-tts-flash。按音色20/4/4划分训练/验证/测试。scripts/wake_color_corpus.py 为每个目标加入其他三色正词作为负例，每目标616条原始数据。公开噪声来源/哈希见 noise-sources.json。

microWakeWord上游 revision 4665173cd35f1cff9a61e06fc427f124766c488e，使用 scripts/microwakeword-class-weights.patch 修正样本权重广播问题。Python3.11 / tensorflow-cpu2.20，训练4000步、batch64、seed20261124。黑/红在验证集上比较两版负例权重，选择[0.5,1,2]；绿使用[1,3,6]。版本选择见 selection.json；最终测试在模型和门限固定后运行，没有据测试结果再调整参数。

复现入口：scripts/wake_features.py → scripts/train_voice_wake.py → scripts/wake_evaluate.py（validation）→ scripts/wake_calibrate.py → scripts/wake_evaluate.py（testing），最后 scripts/wake_color_cross.py。依赖锁见 scripts/requirements-wake.txt。云端重新合成不保证音频字节一致，应以 corpus-manifest.json 的源文件哈希核对缓存；API key/签名URL不在交付目录。

## 板端边界

16kHz/mono/PCM16，30ms特征窗、10ms步长、40维、输入[1,3,40]int8，输出uint8；各模型50,888字节。共用当前microWakeWord/TFLite Micro计算形式，但未在ESP32运行这三个新模型。当前固件仍校验小白模型ID，并使用小白门限；后续接入必须同时适配词条、ID、模型文件和各自门限/连续次数，不能只改显示文字或替换权重文件。

小白权重SHA256仍为 feea9bbcbf97fedd7079e9f206abf4f0b3db3ac5bf066cd8418d068df3909b08。三个新模型的完整SHA256在index.json及各manifest中。无真人、远场、AEC或声学打断验收记录。
