"""Summarize the bounded quality soak without replacing historical verdicts."""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import statistics


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    root = args.directory
    limits_path = Path('tests/runtime/tools/product_acceptance/product_external_limits.json')
    limits = json.loads(limits_path.read_text())
    manifest = json.loads((root / 'summary.json').read_text())
    events = [json.loads(line.split(' ', 1)[1]) for line in
              (root / 'events.log').read_text().splitlines()
              if line.startswith('[QUALITY_RUNTIME] ')]
    samples = [e for e in events if e.get('event') == 'soak_sample']
    rows = [json.loads(line) for line in (root / 'resources.jsonl').read_text(encoding='utf-8-sig').splitlines()]
    rows = [r for r in rows if r.get('phase') == 'soak' and r.get('process_alive')]
    if not samples or not rows:
        raise RuntimeError('soak_evidence_missing')
    start = datetime.fromisoformat(rows[0]['utc'])
    for row in rows:
        row['elapsed'] = (datetime.fromisoformat(row['utc']) - start).total_seconds()
    # Fixed before the run: exclude initial 60 s, compare 60-120 s with
    # the final 60 s. These are continuous-load windows, not room-release
    # windows; do not reuse this verdict for the separate product soak.
    baseline = [r for r in rows if 60 <= r['elapsed'] < 120]
    end = max(r['elapsed'] for r in rows)
    final = [r for r in rows if end - 60 <= r['elapsed'] <= end]
    checks = {}
    for field, limit in [('private_bytes', limits['private_growth_mib'] * 2**20),
                         ('handles', limits['handles_growth']),
                         ('threads', limits['threads_growth']),
                         ('cpu_pct', limits['cpu_increase_percentage_points']),
                         ('gpu_dedicated_bytes', limits['wddm_gpu_growth_mib'] * 2**20),
                         ('gpu_shared_bytes', limits['wddm_gpu_growth_mib'] * 2**20)]:
        first = [r[field] for r in baseline if r.get(field) is not None]
        last = [r[field] for r in final if r.get(field) is not None]
        if len(first) < 10 or len(last) < 10:
            checks[field] = dict(status='DEFERRED', reason='insufficient_counter_samples')
            continue
        a, b = statistics.median(first), statistics.median(last)
        values = [(r['elapsed'], r[field]) for r in rows if r['elapsed'] >= 60 and r.get(field) is not None]
        mx, my = statistics.mean(x for x, _ in values), statistics.mean(y for _, y in values)
        slope = sum((x-mx)*(y-my) for x, y in values) / sum((x-mx)**2 for x, _ in values)
        checks[field] = dict(status='PASS' if b-a <= limit else 'FAIL', baseline_median=a,
                             final_median=b, growth=b-a, limit=limit,
                             slope_per_minute=slope*60, maximum=max(y for _, y in values))
    fps = [e['decoded_frames'] / 10 for e in samples]
    complete = manifest.get('status') == 'PASS' and len(samples) == 180 and samples[-1]['elapsed'] == 1800
    result = dict(status='PASS' if complete and all(c['status'] == 'PASS' for c in checks.values()) else 'PARTIAL',
                  run_id=manifest['run_id'], duration_complete=complete, sample_count=len(samples),
                  baseline_window_seconds=[60,120], final_window_seconds=[end-60,end],
                  resource_checks=checks, decoded_fps=dict(minimum=min(fps), median=statistics.median(fps),maximum=max(fps)),
                  counter_availability=sorted({r['gpu_availability'] for r in rows}),
                  limits_sha256=hashlib.sha256(limits_path.read_bytes()).hexdigest(),
                  evidence_sha256={name:hashlib.sha256((root/name).read_bytes()).hexdigest()
                                   for name in ['summary.json','events.log','resources.jsonl']},
                  boundary='Generated pattern; continuous load; WDDM counters are not DXGI budgets or subjective quality.')
    if any(c['status'] == 'FAIL' for c in checks.values()) or not complete:
        result['status'] = 'FAIL'
    (root / 'resource-analysis.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result,indent=2))


if __name__ == '__main__':
    main()
