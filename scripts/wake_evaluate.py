#!/usr/bin/env python3
"""Streaming INT8 evaluation with the same quantization and trigger gate as firmware."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
import soundfile as sf
from ai_edge_litert.interpreter import Interpreter
from wake_features import features, trim, augment

def predict(model, audio, threshold, consecutive=9):
    # New interpreter clears resource variables and CALL_ONCE state for each independent clip.
    interpreter = Interpreter(model_path=str(model), num_threads=1)
    interpreter.allocate_tensors()
    input_info, = interpreter.get_input_details()
    output_info, = interpreter.get_output_details()
    assert input_info['shape'].tolist() == [1, 3, 40] and input_info['dtype'] == np.int8
    scale, zero = input_info['quantization']
    out_scale, out_zero = output_info['quantization']
    assert scale > 0 and out_scale > 0 and output_info['dtype'] == np.uint8
    values = features(audio)
    values = np.clip(np.floor(values / scale + 0.5) + zero, -128, 127).astype(np.int8)
    hits, windows, maximum = 0, 0, 0.0
    detections = []
    probabilities = []
    for start in range(0, len(values) - 2, 3):
        interpreter.set_tensor(input_info['index'], values[start:start + 3][None, :, :])
        interpreter.invoke()
        probability = (int(interpreter.get_tensor(output_info['index']).item()) - out_zero) * out_scale
        maximum = max(maximum, probability)
        probabilities.append(probability)
        windows += 1
        hits = hits + 1 if windows >= 34 and probability >= threshold else 0
        if hits == consecutive:
            detections.append((start + 5) * 0.01)
    return maximum, detections, probabilities

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--corpus', type=Path, required=True)
    parser.add_argument('--split', choices=['validation', 'testing'], default='testing')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--threshold', type=float, default=0.9)
    parser.add_argument('--consecutive', type=int, default=9)
    parser.add_argument('--noise-dir', type=Path)
    parser.add_argument('--sweep', action='store_true', help='Calibrate threshold/consecutive windows on validation only')
    args = parser.parse_args()
    assert 0 < args.threshold < 1 and 1 <= args.consecutive <= 100
    rng = np.random.default_rng(20261125)
    records = []
    for case in json.loads((args.corpus / 'manifest.json').read_text()):
        if case['split'] != args.split:
            continue
        path = args.corpus / case['file']
        assert hashlib.sha256(path.read_bytes()).hexdigest() == case['sha256']
        audio, rate = sf.read(path, dtype='float32')
        assert rate == 16000 and audio.ndim == 1
        audio = trim(audio)
        for variant in range(4 if case['label'] else 1):
            sample = audio if variant == 0 else augment(audio, rng)
            sample = np.concatenate([rng.normal(0, 0.0001, 64000), sample, np.zeros(16000)]).astype(np.float32)
            maximum, detected, probabilities = predict(args.model, sample, args.threshold, args.consecutive)
            records.append(dict(id=case['id'], voice=case['voice'], label=case['label'], variant=variant, seconds=len(sample)/16000,
                                max_probability=maximum, detections_seconds=detected, probabilities=probabilities))
    if args.noise_dir:
        for path in sorted((args.noise_dir / args.split).glob('*.wav')):
            sample, rate = sf.read(path, dtype='float32')
            assert rate == 16000
            if sample.ndim == 2:
                sample = sample.mean(axis=1)
            maximum, detected, probabilities = predict(args.model, sample, args.threshold, args.consecutive)
            records.append(dict(id='public_noise/' + path.name, voice='public_recording', label=False, variant=0,
                                seconds=len(sample)/16000, max_probability=maximum, detections_seconds=detected, probabilities=probabilities))
    positive = [r for r in records if r['label']]
    negative = [r for r in records if not r['label']]
    assert positive and negative
    summary = dict(model_sha256=hashlib.sha256(args.model.read_bytes()).hexdigest(), split=args.split, threshold=args.threshold, consecutive=args.consecutive,
                   positive_clips=len(positive), positive_detected=sum(bool(r['detections_seconds']) for r in positive),
                   clean_positive_clips=sum(r['variant'] == 0 for r in positive),
                   clean_positive_detected=sum(bool(r['detections_seconds']) for r in positive if r['variant'] == 0),
                   negative_clips=len(negative), negative_seconds=sum(r['seconds'] for r in negative),
                   false_triggers=sum(len(r['detections_seconds']) for r in negative),
                   limitation='Held-out synthetic voices and, if supplied, public noise. No real household FAPH or human recall claim.', records=records)
    args.output.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({k:v for k,v in summary.items() if k != 'records'}, ensure_ascii=False))
    if args.sweep:
        assert args.split == 'validation', 'Never calibrate on the held-out test set'
        for threshold in (0.7, 0.8, 0.9, 0.95, 0.98, 0.99):
            for required in (3, 5, 7, 9, 11):
                tp = clean = fp = 0
                for record in records:
                    hits = count = 0
                    for window, probability in enumerate(record['probabilities'], 1):
                        hits = hits + 1 if window >= 34 and probability >= threshold else 0
                        count += hits == required
                    if record['label']:
                        tp += count > 0
                        clean += count > 0 and record['variant'] == 0
                    else:
                        fp += count
                print(json.dumps(dict(threshold=threshold, consecutive=required, positive_detected=tp, clean_positive_detected=clean, false_triggers=fp)))

if __name__ == '__main__':
    main()
