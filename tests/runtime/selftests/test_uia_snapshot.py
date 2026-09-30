"""Checkpoint lifecycle contracts; --desktop additionally reads an owned WPF fixture."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

# Support direct execution and unittest discovery from any working directory.
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/desktop"))

from uia_snapshot import snapshot, _checkpoint_slot


class SnapshotTests(unittest.TestCase):
    def setUp(self):
        directory=tempfile.TemporaryDirectory(prefix='uia-rate-test-')
        self.addCleanup(directory.cleanup)
        patcher=patch('uia_snapshot.STATE_DIRECTORY',Path(directory.name))
        patcher.start();self.addCleanup(patcher.stop)

    def test_interval_survives_module_reload_and_failed_attempt(self):
        with patch('uia_snapshot.time.monotonic',return_value=100):
            with self.assertRaises(ValueError):
                with _checkpoint_slot(123,456,sys.executable):
                    raise ValueError('failed read')
        # A newly imported CLI module must see the on-disk interval too.
        import importlib.util
        spec=importlib.util.spec_from_file_location('isolated_snapshot',(Path(__file__).resolve().parents[1] / 'tools/desktop/uia_snapshot.py'))
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        with patch.object(module,'STATE_DIRECTORY',__import__('uia_snapshot').STATE_DIRECTORY), patch.object(module.time,'monotonic',return_value=159), patch.object(module.subprocess,'Popen') as launch:
            with self.assertRaisesRegex(RuntimeError,'at least 60 seconds'):
                module.snapshot(123,456,sys.executable,['different-control'])
            launch.assert_not_called()
        with patch('uia_snapshot.time.monotonic',return_value=160):
            with _checkpoint_slot(123,456,sys.executable):pass

    def test_concurrent_slot_rejected_and_new_process_identity_independent(self):
        with _checkpoint_slot(123,456,sys.executable):
            with self.assertRaisesRegex(RuntimeError,'already active'):
                with _checkpoint_slot(123,456,sys.executable):pass
            with _checkpoint_slot(123,789,sys.executable):pass
    def test_invalid_request_does_not_start_client(self):
        with patch('uia_snapshot.subprocess.Popen') as launch:
            for ids in ([], ['same', 'same'], ['']):
                with self.assertRaises(ValueError):
                    snapshot(123, 456, sys.executable, ids)
            with self.assertRaises(ValueError):
                snapshot(123, 456, sys.executable, ['id'], timeout=31)
            launch.assert_not_called()

    def test_timeout_kills_only_owned_client_without_retry(self):
        with patch('uia_snapshot.subprocess.Popen') as launch:
            child=launch.return_value
            child.wait.side_effect=[subprocess.TimeoutExpired('worker',1), 0]
            child.poll.return_value=0
            with self.assertRaises(TimeoutError):
                snapshot(123,456,sys.executable,['id'],timeout=1)
            launch.assert_called_once()
            child.kill.assert_called_once()

    def test_mismatched_response_rejected_after_client_exit(self):
        def launch(argv, **kwargs):
            request=json.loads(Path(argv[argv.index('-RequestFile')+1]).read_text())
            target=Path(argv[argv.index('-ResultFile')+1])
            target.write_text(json.dumps(dict(request_id=request['request_id'],product_pid=999,
                client_pid=321,values=[dict(automation_id='id')])),encoding='utf-8')
            from unittest.mock import Mock
            return Mock(pid=321,wait=Mock(return_value=0),poll=Mock(return_value=0))
        with patch('uia_snapshot.subprocess.Popen',side_effect=launch):
            with self.assertRaisesRegex(RuntimeError,'identity mismatch'):
                snapshot(123,456,sys.executable,['id'])


def desktop_check():
    # Only an owned disposable UI, no meeting, hardware, WPR, or product build.
    fixture=Path(__file__).resolve().parents[2]/'uia/retest_fault_fixture.ps1'
    child=subprocess.Popen(['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass',
                            '-File',str(fixture),'-Role','Product'],
                           stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,
                           creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        deadline=time.monotonic()+15
        while time.monotonic()<deadline:
            raw=subprocess.check_output(['powershell.exe','-NoProfile','-Command',
                f'$p=Get-Process -Id {child.pid}; @{{ticks=$p.StartTime.ToUniversalTime().Ticks;path=$p.Path;window=$p.MainWindowHandle.ToInt64()}}|ConvertTo-Json'],timeout=10)
            info=json.loads(raw.decode('utf-8-sig'))
            if info['window']:
                break
            time.sleep(.2)
        else:
            raise AssertionError('fixture window did not appear')
        results=[snapshot(child.pid,info['ticks'],info['path'],['retestAction'])]
        with unittest.TestCase().assertRaisesRegex(RuntimeError,'at least 60 seconds'):
            snapshot(child.pid,info['ticks'],info['path'],['retestAction'])
        # Advance only the policy clock; UIA still reads the real owned fixture.
        future=time.monotonic()+61
        with patch('uia_snapshot.time.monotonic',return_value=future):
            results.append(snapshot(child.pid,info['ticks'],info['path'],['retestAction']))
        assert results[0]['client_pid']!=results[1]['client_pid']
        assert all(r['client_exit_code']==0 and r['values'][0]['name']=='Run test action' for r in results)
        assert child.poll() is None
        with unittest.TestCase().assertRaises(RuntimeError):
            snapshot(child.pid,info['ticks']+1,info['path'],['retestAction'])
        assert child.poll() is None
        print(json.dumps(dict(desktop='PASS',reads=2,distinct_clients=True,product_alive=True,
                              rapid_repeat_rejected=True,wrong_identity_rejected=True,media='NOT_RUN')))
    finally:
        if child.poll() is None:
            child.kill()
        child.wait(timeout=10)


if __name__=='__main__':
    if '--desktop' in sys.argv:
        desktop_check()
    else:
        unittest.main()
