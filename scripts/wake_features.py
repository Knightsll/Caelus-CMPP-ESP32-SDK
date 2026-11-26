#!/usr/bin/env python3
"""Prepare speaker-disjoint microWakeWord features from wake_qwen_corpus.py output."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
import soundfile as sf
from scipy.signal import resample_poly, lfilter
from mmap_ninja.ragged import RaggedMmap
from pymicro_features import MicroFrontend
import yaml

def features(audio):
    pcm = np.clip(audio * 32768, -32768, 32767).astype("<i2").tobytes()
    frontend = MicroFrontend()
    result, offset = [], 0
    while offset + 320 <= len(pcm):
        frame = frontend.process_samples(pcm[offset:offset + 320])
        if frame.samples_read <= 0:
            raise RuntimeError("Frontend did not consume input")
        offset += frame.samples_read * 2
        if frame.features:
            result.append(frame.features)
    value = np.asarray(result, dtype=np.float32)
    assert value.ndim == 2 and value.shape[1] == 40 and np.isfinite(value).all()
    return value

def trim(audio):
    active = np.flatnonzero(np.abs(audio) > max(0.0005, np.max(np.abs(audio)) * 0.008))
    if not len(active):
        raise ValueError("Silent source clip")
    return audio[max(0, active[0] - 480):min(len(audio), active[-1] + 801)]

def augment(audio, rng, clean=False):
    if not clean:
        rate = int(rng.uniform(85, 116))
        audio = resample_poly(audio, 100, rate)
        # Small, bounded delayed reflections; never join halves into a false positive.
        if rng.random() < 0.7:
            dry = audio
            audio = np.pad(dry, (0, 4000))
            for _ in range(3):
                delay = rng.integers(320, 4000)
                audio[delay:delay + len(dry)] += dry * rng.uniform(0.05, 0.3)
        audio = audio * (10 ** rng.uniform(-1.4, -0.05)) / max(np.max(np.abs(audio)), 0.01)
    audio = np.pad(audio, (rng.integers(3200, 8000), rng.integers(160, 2400)))
    # Give the actual frontend background context. Padding feature tensors with
    # zeros only for short positives would teach a shortcut absent in a live mic.
    audio = np.pad(audio, (max(0, 64000 - len(audio)), 0))
    if not clean:
        noise = rng.normal(0, 1, len(audio))
        if rng.random() < 0.5:
            noise = lfilter([1], [1, -0.9], noise)
        audio = audio + noise * 10 ** rng.uniform(-4.2, -2.5)
    return np.clip(audio, -0.99, 0.99).astype(np.float32)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--noise-dir", type=Path, help="Public background WAVs in training/validation/testing folders")
    args = parser.parse_args()
    manifest_text = (args.corpus / "manifest.json").read_text()
    corpus = json.loads(manifest_text)
    speakers = {s: {c["voice"] for c in corpus if c["split"] == s} for s in ("training", "validation", "testing")}
    assert all(speakers.values()) and not any(speakers[a] & speakers[b] for a, b in (("training", "validation"), ("training", "testing"), ("validation", "testing")))
    if args.output.exists():
        raise FileExistsError("Use a new output directory to keep training runs reproducible")
    args.output.mkdir(parents=True)
    (args.output / 'corpus-manifest.json').write_text(manifest_text)
    rng = np.random.default_rng(20261124)
    counts = {}
    rejected = []
    for split in speakers:
        backgrounds = []
        if args.noise_dir:
            for path in sorted((args.noise_dir / split).glob('*.wav')):
                audio, rate = sf.read(path, dtype='float32')
                assert rate == 16000
                if audio.ndim == 2:
                    audio = audio.mean(axis=1)
                backgrounds.append(audio)
        for label in (True, False):
            selected = [c for c in corpus if c["split"] == split and c["label"] == label]
            repeats = 24 if split == "training" else (6 if label else 2)
            sources = []
            for case in selected:
                path = args.corpus / case["file"]
                assert hashlib.sha256(path.read_bytes()).hexdigest() == case["sha256"]
                audio, rate = sf.read(path, dtype="float32")
                assert rate == 16000 and audio.ndim == 1
                audio = trim(audio)
                if label and not 0.4 <= len(audio) / rate <= 2.5:
                    rejected.append(dict(id=case['id'], seconds=len(audio)/rate, reason='target cannot fit whole in model window after augmentation'))
                    continue
                sources.append(audio)
            def generate():
                for audio in sources:
                    for rep in range(repeats):
                        source = audio
                        if not label:
                            # Match positive onset/duration. Long negatives alone
                            # let the classifier learn "quiet then any speech".
                            length = int(16000 * (3.5 if rep % 5 == 0 else rng.uniform(0.5, 2.5)))
                            start = 0 if rep == 0 else rng.integers(max(1, len(audio) - length + 1))
                            source = audio[start:start + length]
                        sample = augment(source, rng, clean=rep == 0)
                        if backgrounds and rep and rng.random() < 0.7:
                            noise = backgrounds[rng.integers(len(backgrounds))]
                            start = rng.integers(len(noise))
                            noise = np.take(noise, np.arange(start, start + len(sample)), mode='wrap')
                            gain = np.sqrt(np.mean(sample ** 2) + 1e-9) / np.sqrt(np.mean(noise ** 2) + 1e-9)
                            sample = np.clip(sample + noise * gain * 10 ** (-rng.uniform(8, 30) / 20), -0.99, 0.99)
                        yield features(sample)
            out = args.output / ("positive" if label else "negative") / split / "qwen_mmap"
            out.parent.mkdir(parents=True)
            RaggedMmap.from_generator(out_dir=str(out), sample_generator=generate(), batch_size=100, verbose=False)
            counts[f"{split}_{label}"] = len(sources) * repeats
            print(split, label, counts[f"{split}_{label}"], flush=True)
        # Synthesized continuous background is an integration check, not measured household FAPH.
        if split != "training":
            ambient = []
            for case in corpus:
                if case["split"] == split and not case["label"]:
                    audio, _ = sf.read(args.corpus / case["file"], dtype="float32")
                    ambient.append(augment(trim(audio), rng))
            out = args.output / "negative" / (split + "_ambient") / "qwen_mmap"
            out.parent.mkdir(parents=True)
            chunks = [features(np.concatenate(ambient[i:i + 20])) for i in range(0, len(ambient), 20)]
            RaggedMmap.from_generator(out_dir=str(out), sample_generator=iter(chunks), batch_size=100, verbose=False)
        if backgrounds:
            # Keep public recordings disjoint; never mix test noise into training.
            out = args.output / 'negative' / (split if split == 'training' else split + '_ambient') / 'background_mmap'
            out.parent.mkdir(parents=True, exist_ok=True)
            segments = (a[start:start + 80000] for a in backgrounds for start in range(0, len(a) - 79999, 40000)) if split == 'training' else iter(backgrounds)
            RaggedMmap.from_generator(out_dir=str(out), sample_generator=(features(a) for a in segments), batch_size=20, verbose=False)
            if split == 'training':
                onsets = (features(augment(a[start:start + 24000], rng)) for a in backgrounds for start in range(0, len(a) - 23999, 80000))
                RaggedMmap.from_generator(out_dir=str(out.parent / 'background_onsets_mmap'), sample_generator=onsets, batch_size=20, verbose=False)
    # Quiet and colored noise negatives, including near-zero microphone input.
    def noise_features():
        for _ in range(200):
            yield features(rng.normal(0, 10 ** rng.uniform(-5, -1.5), 80000).astype(np.float32))
    RaggedMmap.from_generator(out_dir=str(args.output / "negative/training/noise_mmap"), sample_generator=noise_features(), batch_size=100, verbose=False)
    config = dict(window_step_ms=10, train_dir=str(args.output.resolve() / "model"), features=[
        dict(features_dir=str(args.output.resolve() / name), sampling_weight=1.0, penalty_weight=1.0, truth=truth, truncation_strategy="truncate_start", type="mmap")
        for name, truth in (("positive", True), ("negative", False))
    ], training_steps=[1200, 1600, 1200], positive_class_weight=[1], negative_class_weight=[1, 3, 6], learning_rates=[0.001, 0.0003, 0.0001], batch_size=64,
       time_mask_max_size=[5], time_mask_count=[1], freq_mask_max_size=[3], freq_mask_count=[1], mix_up_prob=[0], freq_mix_prob=[0],
       eval_step_interval=200, clip_duration_ms=6000, target_minimization=0.05, minimization_metric="loss", maximization_metric="auc")
    (args.output / "training.yaml").write_text(yaml.safe_dump(config))
    (args.output / "rejected_positive.json").write_text(json.dumps(rejected, ensure_ascii=False, indent=2))
    (args.output / "dataset.json").write_text(json.dumps({"seed": 20261124, "counts": counts, "speakers": {s: sorted(v) for s, v in speakers.items()}, "noise_sources": str(args.noise_dir.resolve() / 'sources.json') if args.noise_dir else None, "limitation": "Mandarin speech is synthesized; requires independent real speech and household acceptance."}, indent=2))

if __name__ == "__main__":
    main()
