"""Run the existing UIA gates serially, keeping each verdict and binary identity."""
from __future__ import annotations
import argparse
import ctypes
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess


CASES = {
    'settings-zh_CN': ('settings', 'uia_entry_fixture', 'zh_CN'),
    'join-zh_CN': ('join', 'uia_entry_fixture', 'zh_CN'),
    'meeting-zh_CN': ('meeting', 'test_participant_window_remediation', 'zh_CN'),
    'console-zh_CN': ('console', 'uia_console_fixture', 'zh_CN'),
    'whiteboard-zh_CN': ('whiteboard', 'uia_whiteboard_fixture', 'zh_CN'),
    'whiteboard_clear-zh_CN': ('whiteboard_clear', 'uia_whiteboard_clear_fixture', 'zh_CN'),
    'whiteboard_files-zh_CN': ('whiteboard_files', 'uia_whiteboard_files_fixture', 'zh_CN'),
    'whiteboard_files-en_US': ('whiteboard_files', 'uia_whiteboard_files_fixture', 'en_US'),
    'whiteboard_file_edges-zh_CN': ('whiteboard_file_edges', 'uia_whiteboard_files_fixture', 'zh_CN'),
    'whiteboard_file_edges-en_US': ('whiteboard_file_edges', 'uia_whiteboard_files_fixture', 'en_US'),
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=CASES, action='append')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    verifier = Path(__file__).resolve().parents[1] / 'diagnostics/verify_runtime_binary.ps1'
    result = dict(status='RUNNING', configuration='RelWithDebInfo',
        started_utc=datetime.now(timezone.utc).isoformat(), runner_sha256=sha(Path(__file__)),
        head=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
        scope='Independent UIA operations on production Qt controls; no service or media acceptance.', cases=[])
    identities = {}
    manifest = args.output/'index.json'
    def save():
        manifest.write_text(json.dumps(result, indent=2), encoding='utf-8')
    save()
    for name in args.case or CASES:
        scenario, target, language = CASES[name]
        directory = args.output/name
        directory.mkdir()
        case = dict(case=name, status='RUNNING')
        result['cases'].append(case)
        save()
        child = None
        try:
            binary = (args.binary_dir/(target+'.exe')).resolve()
            if target not in identities:
                checked = subprocess.run(['pwsh','-NoProfile','-File',str(verifier),
                    '-Executable',str(binary),'-ExpectedExecutableName',binary.name,
                    '-Configuration','RelWithDebInfo'], capture_output=True, text=True,
                    creationflags=subprocess.CREATE_NO_WINDOW)
                if checked.returncode:
                    raise RuntimeError('RelWithDebInfo_binary_verification_failed')
                identities[target] = json.loads(checked.stdout)
            case['binary_identity'] = identities[target]
            if sha(binary) != identities[target]['binary_sha256']:
                raise RuntimeError('binary_changed_before_launch')
            command = ['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',
                'tests/uia/test_'+scenario+'.ps1','-Fixture',str(binary),
                '-OutputDirectory',str(directory.resolve())]
            if scenario not in ('settings','join','meeting'):
                command += ['-Language',language]
            child_env = os.environ.copy()
            # A pwsh parent can export its PS7 module directories. Windows
            # PowerShell 5.1 must resolve its own inbox Utility/Security modules.
            windows_path = ctypes.create_unicode_buffer(32768)
            if not ctypes.windll.kernel32.GetWindowsDirectoryW(windows_path, len(windows_path)):
                raise RuntimeError('Windows_module_root_unavailable')
            child_env['PSModulePath'] = os.path.join(windows_path.value,
                'System32','WindowsPowerShell','v1.0','Modules')
            with (directory/'runner.log').open('wb') as log:
                child = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                    env=child_env, creationflags=subprocess.CREATE_NO_WINDOW)
                try:
                    child.wait(timeout=120)
                except subprocess.TimeoutExpired:
                    # Only this freshly created runner and its children.
                    if child.poll() is None:
                        subprocess.run(['taskkill','/PID',str(child.pid),'/T','/F'],
                            capture_output=True,creationflags=subprocess.CREATE_NO_WINDOW)
                        child.wait(timeout=10)
                    raise RuntimeError('original_120_second_UIA_gate_timeout')
            raw_log = (directory/'runner.log').read_bytes()
            try:
                decoded_log = raw_log.decode('utf-8')
            except UnicodeDecodeError:
                decoded_log = raw_log.decode('gb18030', errors='replace')
            (directory/'runner.log').write_text(decoded_log,encoding='utf-8')
            case['exit_code'] = child.returncode
            evidence = list(directory.glob('*/result.json'))
            if len(evidence) != 1:
                raise RuntimeError('one_UIA_result_required')
            record = json.loads(evidence[0].read_text(encoding='utf-8-sig'))
            case.update(evidence=evidence[0].relative_to(args.output).as_posix(),
                sha256=sha(evidence[0]), checks=len(record['checks']),
                original_verdict=record['verdict'], detail=record['detail'], step=record['step'])
            case['binary_unchanged'] = sha(binary) == identities[target]['binary_sha256']
            case['status'] = 'PASS' if child.returncode == 0 and record['verdict']=='PASS' and case['binary_unchanged'] else 'FAIL'
        except Exception as error:
            case.update(status='FAIL', failure=type(error).__name__+':'+str(error))
        case['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()
        print(name+' '+case['status'],flush=True)
        if case['status'] != 'PASS':
            break
    result['status'] = 'PASS' if len(result['cases']) == len(args.case or CASES) and all(
        x['status']=='PASS' for x in result['cases']) else 'FAIL'
    result['finished_utc'] = datetime.now(timezone.utc).isoformat()
    save()
    return 0 if result['status']=='PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
