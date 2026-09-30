"""Offline contracts for migrated tools. No WPR, cloud, device or disk-full run."""
import contextlib
import io
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]


class DiagnosticTools(unittest.TestCase):
    @unittest.skipUnless(sys.platform == 'win32', 'Windows native stderr contract')
    def test_wpr_native_stderr_preserves_exit_code_and_evidence(self):
        def quote(value):
            return "'" + str(value).replace("'", "''") + "'"
        with tempfile.TemporaryDirectory(prefix='wpr stderr ') as directory:
            for code in (0, 7):
                tag = 'stderr-' + str(code)
                command = '''
Import-Module Microsoft.PowerShell.Utility -ErrorAction Stop
function wpr {
    & $env:ComSpec /c 'echo native diagnostic 1>&2 & exit __CHILD_EXIT__'
    Set-Variable -Name LASTEXITCODE -Value $LASTEXITCODE -Scope 1
}
'''.replace('__CHILD_EXIT__', str(code))
                command += '& ' + quote(ROOT / 'tests/runtime/etw/handle_wpr.ps1')
                command += ' -Action start -OutputDirectory ' + quote(directory) + ' -Tag ' + tag
                run = subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                                      '-Command', command], capture_output=True, timeout=30, env=dict(os.environ))
                self.assertEqual(run.returncode == 0, code == 0, run.stderr)
                marker = json.loads((Path(directory) / (tag + '-start.json')).read_text(encoding='utf-8-sig'))
                self.assertEqual(marker['exit_code'], code)
                self.assertIn('native diagnostic', marker['output'])

    @unittest.skipUnless(sys.platform == 'win32', 'Windows redirected process regression')
    def test_etw_redirected_child_exit_status(self):
        # Replace only the WPR command and probe executable selection. The runner
        # still owns a real, short-lived, redirected Windows child process.
        def quote(value):
            return "'" + str(value).replace("'", "''") + "'"
        with tempfile.TemporaryDirectory(prefix='probe exit ') as directory:
            for code in (0, 7):
                output = Path(directory) / str(code)
                command = '''
Import-Module Microsoft.PowerShell.Utility -ErrorAction Stop
function wpr { Set-Variable -Name LASTEXITCODE -Value 0 -Scope 1; 'offline WPR stub' }
function Start-Process {
    param($FilePath,$ArgumentList,$WindowStyle,[switch]$PassThru,
          $RedirectStandardOutput,$RedirectStandardError)
    Microsoft.PowerShell.Management\\Start-Process -FilePath $env:ComSpec -ArgumentList '/c exit __CHILD_EXIT__' -PassThru -WindowStyle Hidden -RedirectStandardOutput $RedirectStandardOutput -RedirectStandardError $RedirectStandardError
}
'''.replace('__CHILD_EXIT__', str(code))
                command += '& ' + quote(ROOT / 'tests/runtime/etw/invoke_handle_probe.ps1')
                command += ' -Executable ' + quote(sys.executable)
                command += ' -OutputDirectory ' + quote(output) + ' -Scenario external'
                run = subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                                      '-Command', command], capture_output=True, timeout=30,
                                     env=dict(os.environ))
                self.assertEqual(run.returncode == 0, code == 0, run.stderr)
                result = json.loads((output / 'result.json').read_text(encoding='utf-8-sig'))
                self.assertEqual(result['probe_exit_codes']['external'], code)
                stop = json.loads(next(output.glob('*-stop.json')).read_text(encoding='utf-8-sig'))
                self.assertEqual(stop['exit_code'], 0)

    def test_etw_decode_preserves_loss_and_duplicate_boundaries(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / 'lifecycle.log'
            log.write_text('\n'.join('[LIFECYCLE] ' + json.dumps(dict(
                pid=42, epoch_ms=ms, cycle=cycle, phase='stopped', handles=count
            )) for ms, cycle, count in ((1001, 1, 1), (1003, 2, 2))))
            lines = [
                'OS Version: test Trace Start: 116444736010000000 Events Lost: 7\n',
                'HandleCreate, 500, test_probe (42), 1, 0xa, 0x10, ALPC Port\n',
                'Stack, 500, test_probe (42), 1, 0x1000, module\n',
                'HandleCreate, 2000, test_probe (42), 1, 0xb, 0x20, ALPC Port\n',
                'HandleDuplicate, 2100, test_probe (42), 1\n',
            ]
            class Process:
                stdout = lines
                def wait(self):
                    return 0
            script = ROOT / 'tests/runtime/etw/extract_handle_trace.py'
            argv = [str(script), str(root / 'trace.etl'), str(log)]
            with patch.object(sys, 'argv', argv), patch('subprocess.Popen', return_value=Process()), contextlib.redirect_stdout(io.StringIO()):
                runpy.run_path(str(script), run_name='__main__')
            result = json.loads((root / 'trace-handles.json').read_text())
            self.assertEqual(result['events_lost'], 7)
            self.assertEqual(result['duplicate_events_not_applied'], 1)
            self.assertEqual(result['retained_after_warmup'], [1])
            self.assertEqual(result['samples'][0]['trace_open_count'], 1)
            # Re-running must not overwrite existing evidence, even before xperf starts.
            with patch.object(sys, 'argv', argv), patch('subprocess.Popen') as launch:
                with self.assertRaises(FileExistsError):
                    runpy.run_path(str(script), run_name='__main__')
                launch.assert_not_called()

    @unittest.skipUnless(sys.platform == 'win32', 'PowerShell routing is Windows only')
    def test_scenario_plan_and_option_rejection(self):
        with tempfile.TemporaryDirectory(prefix='probe plan ') as directory:
            script = ROOT / 'tests/runtime/tools/diagnostics/invoke_diagnostic_probe.ps1'
            command = ['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script),
                       '-PreparedDirectory', directory, '-Instance', 'i-offline',
                       '-ServiceUrl', 'ws://127.0.0.1:17880', '-Plan']
            for scenario in ('soak', 'render', 'recovery', 'share'):
                extra = ['-Binary', sys.executable] if scenario in ('recovery', 'share') else []
                run = subprocess.run(command + ['-Scenario', scenario] + extra,
                                     capture_output=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stderr)
                plan = json.loads(run.stdout.decode('utf-8-sig'))
                self.assertEqual(plan['execution'], 'NOT_RUN')
                self.assertEqual(plan['scenario'], scenario)
                self.assertEqual(Path(plan['parameters']['PreparedDirectory']), Path(directory))
            rejected = subprocess.run(command + ['-Scenario', 'render', '-Attribution'],
                                      capture_output=True, timeout=30)
            self.assertNotEqual(rejected.returncode, 0)
            invalid_url = list(command)
            invalid_url[invalid_url.index('-ServiceUrl') + 1] = 'ws://user:secret@host:80'
            rejected = subprocess.run(invalid_url + ['-Scenario', 'soak'],
                                      capture_output=True, timeout=30)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertEqual(list(Path(directory).iterdir()), [])


if __name__ == '__main__':
    unittest.main()
