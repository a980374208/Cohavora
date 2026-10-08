"""Offline bootstrap contracts; no native library or SDK is loaded."""
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import hashlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[1] / 'tools/diagnostics/native_exit/publisher_exit_launch.py'
SPEC = importlib.util.spec_from_file_location('publisher_exit_launch', SOURCE)
launch = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(launch)


class LaunchContracts(unittest.TestCase):
    def test_only_exact_owned_publisher_or_two_receiver_trace_paths(self):
        class FrozenPath(PurePosixPath):
            def resolve(self): return self
            def is_symlink(self): return False
            def is_dir(self): return self.name == 'pthread-exit-trace'
            def is_file(self): return self.suffix in ('.so', '.py')
            def iterdir(self): return iter(())
            def read_bytes(self): return b'observer'
        owner=FrozenPath('/root/livekit-product-acceptance/native-exit-'+'a'*32)
        library=owner/'bundle/observer.so'; wrapper=owner/'bundle/wrapper.py'
        sha=hashlib.sha256(b'observer').hexdigest()
        with patch.object(launch, 'Path', FrozenPath), patch.dict(os.environ, {}, clear=True):
            for prefix in ('', 'subscriber-1/', 'subscriber-2/'):
                trace=owner/('full-exit-capture/'+prefix+'pthread-exit-trace')
                command,env=launch.prepare(library,sha,trace,[str(wrapper)])
                self.assertEqual(env['NATIVE_EXIT_TRACE_DIR'],str(trace))
                self.assertEqual(env['LD_PRELOAD'],str(library))
            for prefix in ('subscriber-3/', 'other/', 'subscriber-1/other/'):
                with self.assertRaisesRegex(ValueError,'observer_trace_scope_invalid'):
                    launch.prepare(library,sha,owner/('full-exit-capture/'+prefix+'pthread-exit-trace'),[str(wrapper)])

    def test_parent_override_rejected_before_any_file_access(self):
        for key in ('LD_PRELOAD', 'LIVEKIT_LIB_PATH'):
            with self.subTest(key=key), patch.dict(os.environ, {key: 'unapproved'}):
                with self.assertRaisesRegex(ValueError, 'parent_native_override_forbidden'):
                    launch.prepare(Path('missing'), '0'*64, Path('missing'), [])

    def test_unknown_owner_rejected_without_native_load(self):
        with patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(ValueError, 'observer_trace_scope_invalid'):
                launch.prepare(Path('/root/other/observer.so'), '0'*64,
                               Path('/root/other/pthread-exit-trace'), ['wrapper.py'])

    def test_boot_clears_preload_before_wrapper_and_preserves_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'ctypes.py').write_text('''
class Ready:
    def __call__(self): return 1
class Observer:
    native_exit_observer_ready = Ready()
def CDLL(value): return Observer()
c_int = int
''')
            wrapper = root/'wrapper.py'
            wrapper.write_text('''
import json, os
from pathlib import Path
Path('wrapper.json').write_text(json.dumps({'preload':os.environ.get('LD_PRELOAD')}))
raise SystemExit(7)
''')
            env = dict(os.environ, NATIVE_EXIT_TRACE_DIR=str(root),
                       NATIVE_EXIT_OBSERVER_SHA='a'*64, LD_PRELOAD='')
            result = subprocess.run([sys.executable, '-B', '-c', launch.BOOT, str(wrapper)],
                                    cwd=root, env=env, capture_output=True)
            self.assertEqual(result.returncode, 7, result.stderr.decode())
            self.assertIsNone(json.loads((root/'wrapper.json').read_text())['preload'])
            ready = json.loads((root/'observer-ready.json').read_text())
            self.assertEqual(ready['observer_sha256'], 'a'*64)
            self.assertFalse(ready['preload_in_child_environment'])
            self.assertFalse(ready['rtc_imported'])


if __name__ == '__main__':
    unittest.main()
