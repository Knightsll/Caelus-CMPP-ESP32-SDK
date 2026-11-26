# 小白小白：实验模型 xiaobai_micro_v1

2026-11-24 自训练的 INT8 microWakeWord，直接嵌入 Caelus ESP-IDF 固件；不兼容 WakeNet 权重格式。模型 SHA256、PCM 自检样本、数据清单和独立评估结果位于同目录 JSON。**尚未通过真人远场或家庭误唤醒验收。**

## 数据与效果

- 用户授权使用千问中文 TTS；实际使用 DashScope `qwen3-tts-flash`，28 音色、280 条原始 WAV。先按音色划分训练 20、验证 4、测试 4，再做增强。每音色 4 条目标词、6 条负样本；负样本含普通中文、单独“小白”“小白小黑”“小黑小白”“小白菜”。
- 训练增加变速、音量、回声、噪声；公开背景声来自 Microsoft MS-SNSD 固定 revision `fe61c4ba0d9ac8dd7e23d719cc79f8947e1dc742`，23 个文件按源文件隔离。来源、SHA256 见 `noise-sources.json`。音频原有许可应单独核对，不能用代码 MIT 许可代替；上游说明涉及 Freesound CC0 与 DEMAND CC-BY-SA 3.0。
- 过长的正样本不参与特征训练，避免裁剪成半句；`rejected-positive.json` 记录原因。外部测试仍包含所有测试音色的正样本，没有因训练长度过滤而排除测试难例。
- 阈值只在验证集选择：0.90、连续 9 次预测（270 ms）、启动前 33 次预测抑制。验证 50/64（未增强 14/16），643.65 秒负样本 1 次误触发。固定参数后的独立测试 49/64（未增强 14/16），580.58 秒负样本 1 次误触发。测试误触发来自 Seren 的普通中文段落。不能将这些短样本结果外推为家庭每小时误唤醒率。
- 模型对背景上下文和发音速度仍敏感；语料为合成语音，独立 ASR 请求返回 403，未得到 ASR 校对或真人识别证据。板上自检只能验证执行链路，不能代替声学验收。

## 复现

建议 Python 3.11 独立环境，依赖见 `scripts/requirements-wake.txt`。从仓库根目录操作：

```bash
git clone https://github.com/OHF-Voice/micro-wake-word .esp-tools/wake-training/micro-wake-word
git -C .esp-tools/wake-training/micro-wake-word checkout 4665173cd35f1cff9a61e06fc427f124766c488e
git -C .esp-tools/wake-training/micro-wake-word apply "$PWD/scripts/microwakeword-class-weights.patch"
python -m pip install -r scripts/requirements-wake.txt
python -m pip install --no-deps -e .esp-tools/wake-training/micro-wake-word
python scripts/train_voice_wake.py --check-weights
python scripts/wake_qwen_corpus.py --help
python scripts/wake_noise_corpus.py --help
python scripts/wake_features.py --corpus .esp-tools/wake-training/qwen-corpus --noise-dir .esp-tools/wake-training/noise --output .esp-tools/wake-training/reproduce
python scripts/train_voice_wake.py .esp-tools/wake-training/reproduce/training.yaml
```

先用合成脚本 `--plan` 查看规模；云 API 会计费。凭据只从私有文件读取，临时签名下载链接只缓存于私有 receipt，不能提交 Git。历史 WAV 缓存在忽略目录，重新合成的音频可能不同；以清单哈希区分。当前模型来自 run-05，使用 run-04 的相同特征及修正权重 `[1,3,6]`，4000 步、batch 64、种子 20261124。前四轮模型效果不合格，未烧录。

上游 `train_ground_truth` 为 `(batch,1)`，原权重计算广播成 `(batch,batch)`，导致负样本惩罚失效。补丁将标签展平，`--check-weights` 通过 BCE 梯度验证负样本 16 倍权重确实生效。

独立流式测试（参数固定后运行测试集，勿用测试集调阈值）：

```bash
python scripts/test_voice_micro_features.py
python scripts/wake_evaluate.py --model main/wake_models/xiaobai.tflite --corpus .esp-tools/wake-training/qwen-corpus --noise-dir .esp-tools/wake-training/noise --split testing --threshold 0.9 --consecutive 9 --output /tmp/xiaobai-testing.json
bash scripts/build_voice.sh -D CMPP_MICRO_MODEL_FILE="$PWD/main/wake_models/xiaobai.tflite" build
```

旧 `build-voice/sdkconfig` 不会自动覆盖已有空模型名，需确认 `CONFIG_CMPP_VOICE_WAKE_MODEL="xiaobai_micro_v1"` 与主任务栈 8192。CPU TensorFlow 2.20 训练；板端 esp-tflite-micro 1.4.1、esp-nn 1.5.0、esp-micro-speech-features 1.2.3。30 ms 窗/10 ms 步长/40 维，16 kHz PCM16，3 帧一次推理。特征 C/Python 一致性检查覆盖静音、音调、噪声。

`positive.pcm` / `negative.pcm` 是验证音色 Aiden 的目标词与“小白小黑”，带低幅背景前导和静音尾部。用于每次启动的 TFLite 芯片执行自检，无播放、无网络上传；不是额外准确率样本。完整流程和实际烧录状态以共同接口文档 6.31.3 为准。
