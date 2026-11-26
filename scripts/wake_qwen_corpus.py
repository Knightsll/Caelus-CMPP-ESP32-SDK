#!/usr/bin/env python3
"""Bounded, resumable Qwen TTS corpus. Credentials and signed URLs never enter manifests."""
import argparse
import concurrent.futures
import hashlib
import functools
import json
import os
from pathlib import Path
import subprocess
import socket
import shutil
import time
import urllib.error
import urllib.request
import wave

MODEL = "qwen3-tts-flash"
ENDPOINT = "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation"
# Split speakers BEFORE synthesis/augmentation; held-out speakers never train.
VOICES = {
    "training": "Cherry Ethan Chelsie Momo Vivian Moon Maia Kai Nofish Bella Ryan Katerina Mia Mochi Bellona Vincent Bunny Neil Elias Arthur".split(),
    "validation": "Serena Aiden Nini Pip".split(),
    "testing": "Jennifer Seren Stella Andre".split(),
}
POSITIVE = ["小白小白", "小白，小白。", "小白！小白！", "小白，小白？"]
NEGATIVE = [
    "小白", "小白小黑", "小黑小白", "小白菜", "小白兔", "小白白", "白小白", "小白拜拜",
    "拜拜，拜拜", "小爱小爱", "小度小度", "小麦小麦", "小贝小贝", "小百货商店", "我叫小白", "小白在家吗",
    "请打开客厅的灯", "关掉空调", "今天晚上吃什么", "天气不错出去走走", "现在几点钟了", "把音量调低一点",
    "你怎么还没回家", "请帮我拿一杯水", "我们明天再见", "收到消息请回复", "这里是今天的新闻", "这道菜很好吃",
    "等一下我马上过来", "窗外下雨了", "爸爸妈妈在做饭", "没有什么事情", "一二三四五六七八九十", "打开卧室的窗帘",
    "这个小孩很可爱", "白色的杯子在哪里", "现在开始播放音乐", "谁把钥匙放在桌上了", "我要去超市买牛奶", "谢谢你的帮助",
]

COLORS = {"white": "白", "black": "黑", "red": "红", "green": "绿"}

def cases(color="white"):
    char = COLORS[color]
    name = "小" + char
    positive = [name + name, f"{name}，{name}。", f"{name}！{name}！", f"{name}，{name}？"]
    result = []
    for split, voices in VOICES.items():
        for voice in voices:
            # Never concatenate "小白" with a phrase starting "小白": together
            # they ARE the wake word. Keep single 小白 isolated and use spoken
            # separators between similar phrases (punctuation is insufficient).
            negative_clips = ["小白", "小白小黑"]
            negative_clips += ["。".join(NEGATIVE[i:i + 10]) + "。" for i in (20, 30)]
            negative_clips += ["小黑小白", "小白菜"]
            if color != "white":
                near = {"black": "小黑猫", "red": "小红帽", "green": "小绿灯"}[color]
                negative_clips = [name, name + "小白", negative_clips[2], negative_clips[3], "小白" + name, near]
            for label, texts in ((True, positive), (False, negative_clips)):
                for index, text in enumerate(texts):
                    case = dict(model=MODEL, voice=voice, text=text, label=label, split=split)
                    digest = hashlib.sha256(json.dumps(case, sort_keys=True, ensure_ascii=False).encode()).hexdigest()[:16]
                    case["id"] = f"{voice}-{int(label)}-{index:02}-{digest}"
                    result.append(case)
    return result

def synthesize(case, root, key):
    path = root / (case["id"] + ".wav")
    if not path.exists():
        body = json.dumps({"model": MODEL, "input": {"text": case["text"], "voice": case["voice"], "language_type": "Chinese"}}).encode()
        request = urllib.request.Request(ENDPOINT, body, {"Authorization": "Bearer " + key, "Content-Type": "application/json"})
        receipt = path.with_suffix(".private.json")
        if receipt.exists():
            result = json.loads(receipt.read_text())
        else:
            for attempt in range(5):
                try:
                    with urllib.request.urlopen(request, timeout=90) as response:
                        result = json.load(response)
                    break
                except urllib.error.HTTPError as error:
                    if error.code in (429, 500, 502, 503, 504) and attempt < 4:
                        time.sleep(3 * (attempt + 1))
                        continue
                    raise RuntimeError(f"TTS HTTP {error.code} for {case['id']}") from None
                except (OSError, urllib.error.URLError):
                    if attempt < 2:
                        time.sleep(3 * (attempt + 1))
                        continue
                    raise RuntimeError(f"TTS connection failed for {case['id']}") from None
            receipt.write_text(json.dumps(result))
        url = result["output"]["audio"]["url"]
        if url.startswith("http:"):
            url = "https:" + url[5:]
        for attempt in range(3):
            try:
                with urllib.request.urlopen(url, timeout=60) as response:
                    audio = response.read()
                break
            except (OSError, urllib.error.URLError):
                if attempt == 2:
                    raise RuntimeError(f"Audio download failed for {case['id']}; private receipt retained") from None
                time.sleep(2)
        source = path.with_suffix(".source.wav")
        source.write_bytes(audio)
        temporary = path.with_suffix(".part.wav")
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(source), "-ac", "1", "-ar", "16000", "-c:a", "pcm_s16le", str(temporary)], check=True)
        temporary.replace(path)
        source.unlink()
    with wave.open(str(path)) as wav:
        assert (wav.getframerate(), wav.getnchannels(), wav.getsampwidth()) == (16000, 1, 2)
        duration = wav.getnframes() / 16000
        if not 0.2 < duration < 120:
            raise ValueError(f"Invalid duration: {case['id']}")
    return {**case, "file": path.name, "seconds": duration, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--key-file", type=Path, default=Path.home() / ".local/state/caelus/smart-home-native/qwen.key")
    parser.add_argument("--limit", type=int, default=1, help="Maximum cases this invocation; cached cases are free")
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--color", choices=COLORS, default="white")
    parser.add_argument("--cache-dir", type=Path, action="append", default=[], help="Reuse matching model/voice/text WAVs without another TTS call")
    parser.add_argument("--plan", action="store_true", help="Print counts only; never calls TTS")
    args = parser.parse_args()
    all_cases = cases(args.color)
    assert len({c["id"] for c in all_cases}) == len(all_cases)
    assert len(set(sum(VOICES.values(), []))) == sum(map(len, VOICES.values()))
    if args.plan:
        print(json.dumps({"cases": len(all_cases), "characters": sum(len(c["text"]) for c in all_cases), "speakers": {s: len(v) for s, v in VOICES.items()}}, ensure_ascii=False))
        return
    if not 1 <= args.limit <= len(all_cases) or not 1 <= args.workers <= 4:
        parser.error("limit/workers outside bounded range")
    os.umask(0o077)
    args.output.mkdir(parents=True, exist_ok=True)
    # Labels/indices differ by target; the spoken waveform only depends on TTS input.
    cached = {}
    for directory in args.cache_dir:
        for item in json.loads((directory / 'manifest.json').read_text()):
            cached[(item['model'], item['voice'], item['text'])] = (directory, item)
    for case in all_cases[:args.limit]:
        match = cached.get((case['model'], case['voice'], case['text']))
        destination = args.output / (case['id'] + '.wav')
        if match and not destination.exists():
            directory, item = match
            source = directory / item['file']
            assert hashlib.sha256(source.read_bytes()).hexdigest() == item['sha256']
            shutil.copyfile(source, destination)
    key = args.key_file.read_text().strip()
    if not key:
        raise ValueError("Empty TTS credential")
    # Reuse successful DNS results during this bounded batch; the RK resolver is intermittent.
    socket.getaddrinfo = functools.lru_cache(maxsize=32)(socket.getaddrinfo)
    results = []
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = {pool.submit(synthesize, case, args.output, key): case for case in all_cases[:args.limit]}
        for index, future in enumerate(concurrent.futures.as_completed(futures), 1):
            try:
                result = future.result()
            except Exception as error:
                failed += 1
                print(f"FAILED {futures[future]['id']}: {type(error).__name__}", flush=True)
                continue
            results.append(result)
            # Save after each completion so a network failure is resumable.
            temporary = args.output / "manifest.part.json"
            temporary.write_text(json.dumps(sorted(results, key=lambda x: x['id']), ensure_ascii=False, indent=2) + "\n")
            temporary.replace(args.output / "manifest.json")
            print(f"{index}/{args.limit} {result['id']} {result['seconds']:.2f}s", flush=True)
    print(f"Completed: {len(results)} successful, {failed} failed", flush=True)
    if failed:
        raise SystemExit(1)

if __name__ == "__main__":
    main()
