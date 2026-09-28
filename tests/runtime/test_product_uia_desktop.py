"""Opt-in real desktop UIA/WM_NULL integration. No meeting, media or C++ build."""
import argparse
import json
from pathlib import Path
import shutil

from product_uia_retest import ROOT, profile, supervise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--scenario', choices=('Complete', 'Hang', 'Crash', 'Stall'), action='append')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    executable = Path(shutil.which('powershell.exe')).resolve()
    expected = {'Complete': 'declared_ui_scope_complete', 'Hang': 'PRODUCT_UI_HANG',
                'Crash': 'PRODUCT_UNEXPECTED_EXIT', 'Stall': 'UIA_OPERATION_WATCHDOG_TIMEOUT'}
    results = []
    for scenario, reason in expected.items():
        if args.scenario and scenario not in args.scenario:
            continue
        settings = profile('probe')
        settings.update(operation_timeout=12, hang_timeout=1, maximum_seconds=30)
        result = supervise(args.output/scenario, executable, settings, command=[str(executable),
            '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
            str(ROOT/'tests/uia/retest_fault_fixture.ps1'), '-Scenario', scenario], poll_seconds=.2)
        results.append(dict(scenario=scenario, expected=reason, actual=result['reason'],
                            passed=result['reason'] == reason, elapsed_s=result['elapsed_s']))
        print(json.dumps(results[-1]), flush=True)
    (args.output/'desktop-checks.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    return 0 if all(row['passed'] for row in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
