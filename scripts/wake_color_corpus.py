#!/usr/bin/env python3
"""Build four target-specific, speaker-disjoint corpora with other colors as negatives."""
import argparse
import hashlib
import json
from pathlib import Path
from wake_qwen_corpus import COLORS

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--white', type=Path, required=True)
    p.add_argument('--colors', type=Path, required=True, help='black/red/green raw corpus folders')
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    assert not a.output.exists(), 'Use a new output directory to preserve dataset identity'
    sources = {c: a.white if c == 'white' else a.colors / c for c in COLORS}
    manifests = {}
    for color, directory in sources.items():
        records = json.loads((directory / 'manifest.json').read_text())
        assert len(records) == 280 and len({r['voice'] for r in records}) == 28
        for r in records:
            assert hashlib.sha256((directory / r['file']).read_bytes()).hexdigest() == r['sha256']
        manifests[color] = records
    for target in COLORS:
        directory = a.output / target
        directory.mkdir(parents=True)
        merged = []
        for color, records in manifests.items():
            for original in records:
                if color != target and not original['label']:
                    continue
                item = dict(original)
                item.update(id=color + ':' + original['id'], file=color + '__' + original['file'],
                            label=color == target and original['label'],
                            source_wake_word=('小' + COLORS[color]) * 2 if original['label'] else None)
                (directory / item['file']).symlink_to((sources[color] / original['file']).resolve())
                merged.append(item)
        assert len(merged) == 616
        speakers = {s: {r['voice'] for r in merged if r['split'] == s} for s in ('training','validation','testing')}
        assert not (speakers['training'] & speakers['validation'] or speakers['training'] & speakers['testing'] or speakers['validation'] & speakers['testing'])
        (directory / 'manifest.json').write_text(json.dumps(merged, ensure_ascii=False, indent=2) + '\n')
        print(target, 'positive', sum(r['label'] for r in merged), 'negative', sum(not r['label'] for r in merged))

if __name__ == '__main__':
    main()
