#!/usr/bin/env python3
"""Evaluate all four wake models against identical held-out color-word waveforms."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
import soundfile as sf
from wake_evaluate import predict
from wake_features import trim
from wake_qwen_corpus import COLORS

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--white', type=Path, required=True)
    p.add_argument('--colors', type=Path, required=True)
    p.add_argument('--models', type=Path, required=True, help='JSON model paths and validation-selected gates')
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    models = json.loads(a.models.read_text())
    assert set(models) == set(COLORS)
    for model in models.values():
        assert hashlib.sha256(Path(model['path']).read_bytes()).hexdigest() == model['sha256']
    records = []
    matrix = {c: {w: dict(detected=0, total=0) for w in COLORS} for c in COLORS}
    for word in COLORS:
        directory = a.white if word == 'white' else a.colors / word
        cases = json.loads((directory / 'manifest.json').read_text())
        selected = [r for r in cases if r['split'] == 'testing' and r['label']]
        assert len(selected) == 16
        for case in selected:
            path = directory / case['file']
            assert hashlib.sha256(path.read_bytes()).hexdigest() == case['sha256']
            audio, rate = sf.read(path, dtype='float32')
            assert rate == 16000 and audio.ndim == 1
            rng = np.random.default_rng(int(case['sha256'][:16], 16))
            sample = np.concatenate([rng.normal(0, .0001, 64000), trim(audio), np.zeros(16000)]).astype(np.float32)
            for target, model in models.items():
                maximum, detections, _ = predict(model['path'], sample, model['threshold'], model['consecutive'])
                cell = matrix[target][word]
                cell['total'] += 1
                cell['detected'] += bool(detections)
                records.append(dict(target=target, spoken_word=word, source_id=case['id'], source_sha256=case['sha256'], voice=case['voice'], max_probability=maximum, detections_seconds=detections))
    result = dict(split='testing', models=models, matrix=matrix, records=records,
                  limitation='16 held-out synthesized clips per spoken word; identical waveform for every model. Not human or household acceptance.')
    a.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(matrix, ensure_ascii=False))

if __name__ == '__main__':
    main()
