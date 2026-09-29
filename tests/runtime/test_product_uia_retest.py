"""Offline fault injection; never certifies product/media or long-run stability."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

import product_uia_retest as retest


PEER = r'''
import json, os, pathlib, sys, time
p = pathlib.Path(os.environ['UIA_RETEST_TEST_OUTPUT']) / 'uia'
p.mkdir()
run = os.environ['LIVEKIT_UIA_RUN_ID']
(p/'product-identity.json').write_text(json.dumps(dict(run_id=run,pid=os.getpid())))
time.sleep(.2)
if sys.argv[1] == 'exit':
    sys.exit(3)
if sys.argv[1] == 'complete':
    (p/'uia-result.json').write_text(json.dumps(dict(run_id=run,verdict='PROBED')))
    sys.exit(0)
if sys.argv[1] == 'wrong-result':
    (p/'uia-result.json').write_text(json.dumps(dict(run_id='bad',verdict='PROBED')))
    sys.exit(0)
time.sleep(30)
'''


class FakeProduct:
    """Fault injection boundary; real process/window API is separately tested."""
    response = True
    def __init__(self, identity, executable):
        self.pid = identity['pid']
    def alive(self):
        return True
    def responsive(self):
        return self.response
    def sample(self):
        return dict(private_bytes=123456, working_set_bytes=234567, handles=10)
    def stop(self):
        pass
    def close(self):
        pass


class SupervisorTests(unittest.TestCase):
    def run_peer(self, mode, product=FakeProduct, **overrides):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            peer = base/'peer.py'
            peer.write_text(PEER)
            settings = retest.profile('probe')
            settings.update(operation_timeout=1, hang_timeout=.1,
                            minimum_free_bytes=0, **overrides)
            result = retest.supervise(base/'run', Path(sys.executable), settings,
                command=[sys.executable, str(peer), mode], product_factory=product,
                desktop_check=lambda: True, poll_seconds=.03)
            self.assertEqual(result['l3_status'], 'NOT_RUN')
            with zipfile.ZipFile(base/'run.zip') as archive:
                manifest = json.loads(archive.read('manifest.json'))
                for entry in manifest['files']:
                    import hashlib
                    self.assertEqual(hashlib.sha256(archive.read(entry['name'])).hexdigest(), entry['sha256'])
                self.assertNotIn('profile/Settings', archive.namelist())
            return result

    def test_success_archive_is_not_l3(self):
        result = self.run_peer('complete')
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['memory']['steady']['status'], 'INSUFFICIENT_DATA')

    def test_driver_crash(self):
        self.assertEqual(self.run_peer('exit')['reason'], 'DRIVER_EXIT_WITHOUT_RESULT')

    def test_stuck_driver(self):
        self.assertEqual(self.run_peer('idle')['reason'], 'UIA_OPERATION_WATCHDOG_TIMEOUT')

    def test_hung_window(self):
        class Hung(FakeProduct):
            response = False
        self.assertEqual(self.run_peer('idle', Hung)['reason'], 'PRODUCT_UI_HANG')

    def test_wrong_run_cannot_pass(self):
        self.assertEqual(self.run_peer('wrong-result')['reason'], 'DRIVER_FAILED')

    def test_absolute_deadline(self):
        self.assertEqual(self.run_peer('idle', maximum_seconds=.15)['reason'], 'RUN_WATCHDOG_TIMEOUT')


class EvidenceTests(unittest.TestCase):
    def test_partial_line_and_truncation(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)/'rows.jsonl'
            reader = retest.JsonlTail(path)
            path.write_bytes(b'{"sequence":1}\n{"sequence":')
            self.assertEqual(reader.read()['sequence'], 1)
            with path.open('ab') as stream:
                stream.write(b'2}\n')
            self.assertEqual(reader.read()['sequence'], 2)
            path.write_bytes(b'')
            with self.assertRaises(retest.Failure):
                reader.read()

    def test_full_duration_guard(self):
        with self.assertRaises(ValueError):
            retest.profile('retest', 1799, 7200)
        with self.assertRaises(ValueError):
            retest.profile('retest', 1800, 7199)
        self.assertEqual(retest.profile('retest')['mixed_cycles'], 12)
        self.assertEqual(retest.profile('smoke')['steady_seconds'], 10)


@unittest.skipUnless(os.name == 'nt', 'Windows process handles')
class ProcessIdentityTests(unittest.TestCase):
    def test_wrong_creation_time_rejected_without_killing_process(self):
        peer = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])
        try:
            with self.assertRaisesRegex(retest.Failure, 'PRODUCT_IDENTITY_MISMATCH'):
                retest.Product(dict(pid=peer.pid, start_ticks=1), Path(sys.executable))
            self.assertIsNone(peer.poll())
        finally:
            peer.kill()
            peer.wait()


if __name__ == '__main__':
    unittest.main()
