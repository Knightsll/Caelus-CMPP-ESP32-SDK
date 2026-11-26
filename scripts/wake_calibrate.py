#!/usr/bin/env python3
"""Choose an experimental gate on validation only; never optimize against test clips."""
import argparse
import json
from pathlib import Path

def events(probabilities, threshold, consecutive):
    hits = 0
    result = []
    for window, probability in enumerate(probabilities, 1):
        hits = hits + 1 if window >= 34 and probability >= threshold else 0
        if hits == consecutive:
            result.append((window * 3 + 2) * .01)
    return result

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('validation', type=Path, nargs='?')
    p.add_argument('--output', type=Path)
    p.add_argument('--self-test', action='store_true')
    a = p.parse_args()
    if a.self_test:
        assert events([1.] * 50, .9, 9) == [1.28]
        assert not events([0.] * 34 + [1.] * 8, .9, 9)
        assert len(events([0.] * 34 + [1.] * 9 + [0.] + [1.] * 9, .9, 9)) == 2
        print('PASS: startup gate, sustained evidence and reset between events')
        return
    assert a.validation and a.output
    data = json.loads(a.validation.read_text())
    assert data['split'] == 'validation', 'Do not calibrate on testing'
    grid = []
    for threshold in (.5, .6, .7, .8, .9, .95, .98, .99):
        for consecutive in (3, 5, 7, 9, 11):
            row = dict(threshold=threshold, consecutive=consecutive, positive_detected=0, clean_positive_detected=0, false_triggers=0)
            for record in data['records']:
                detections = events(record['probabilities'], threshold, consecutive)
                if record['label']:
                    row['positive_detected'] += bool(detections)
                    row['clean_positive_detected'] += bool(detections) and record['variant'] == 0
                else:
                    row['false_triggers'] += len(detections)
            grid.append(row)
    eligible = [row for row in grid if row['clean_positive_detected'] >= .75 * data['clean_positive_clips']]
    selected = min(eligible or grid, key=lambda r: (r['false_triggers'], -r['positive_detected'], -r['clean_positive_detected'], -r['threshold'], r['consecutive']))
    result = dict(split='validation', model_sha256=data['model_sha256'],
                  rule='Require >=75% clean validation recall, minimize false trigger events, then maximize total recall. Experimental gate, not product acceptance.',
                  validation_recall_floor_met=bool(eligible), selected=selected, candidates=grid)
    a.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(selected))

if __name__ == '__main__':
    main()
