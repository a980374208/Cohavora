"""Offline tests: no SDK, credentials, services or native library required."""
import asyncio
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest
from unittest.mock import patch

PATH = Path(__file__).resolve().parents[1] / 'tools/diagnostics/native_exit/publisher_exit_phases.py'
spec = importlib.util.spec_from_file_location('publisher_exit_phases', PATH)
tracing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tracing)


class ExitPhasesTests(unittest.TestCase):
    def test_source_hash_mismatch_rejected_before_instrumentation_or_recorder(self):
        with tempfile.TemporaryDirectory() as root:
            source = Path(root) / 'source.py'
            source.write_bytes(b'invalid Python, must never compile')
            trace = Path(root) / 'trace.jsonl'
            with patch.object(tracing, 'instrument') as compile_source, \
                    patch.object(tracing, 'Recorder') as recorder:
                with self.assertRaisesRegex(ValueError, 'publisher_source_sha256_mismatch'):
                    tracing.main(['--diagnostic-exit-phases', str(trace),
                                  '--source-sha256', '0' * 64, '--', str(source)])
                compile_source.assert_not_called()
                recorder.assert_not_called()
            self.assertFalse(trace.exists())

    def test_ast_preserves_cleanup_and_process_exit(self):
        template = """import asyncio
import atexit
import sys
from types import SimpleNamespace
class FFI:
    def livekit_ffi_drop_handle(self, value):
        return True
    def livekit_ffi_dispose(self):
        pass
ffi = FFI()
sys.modules['livekit.rtc._ffi_client'] = SimpleNamespace(FfiClient=SimpleNamespace(_instance=SimpleNamespace(_ffi_lib=ffi)))
atexit.register(lambda: ffi.livekit_ffi_dispose())
async def publish(code):
    try:
        pass
    finally:
        for task in []:
            pass
        for result in []:
            pass
        for source_kind in [0]:
            ffi.livekit_ffi_drop_handle(1)
        for room in [0]:
            pass
        for source in [0]:
            pass
        0
    return code
if __name__ == '__main__':
    raise SystemExit(asyncio.run(publish(int(sys.argv[1]))))
"""
        with tempfile.TemporaryDirectory() as root:
            source = Path(root) / 'fake_load.py'
            source.write_text(template)
            for code in (0, 7):
                trace = Path(root) / f'{code}.jsonl'
                result = subprocess.run([sys.executable, str(PATH),
                    '--diagnostic-exit-phases', str(trace), '--', str(source), str(code)],
                    capture_output=True)
                self.assertEqual(result.returncode, code, result.stderr.decode())
                rows = [json.loads(line) for line in trace.read_text().splitlines()]
                begins = [r['phase'] for r in rows if r['kind'] == 'begin']
                self.assertEqual(begins, ['cleanup', 'capture_drain', 'audio_clear',
                    'native_drop_handle', 'room_disconnect', 'source_close',
                    'result_commit', 'python_atexit', 'native_ffi_dispose'])
                self.assertEqual(rows[-1]['phase'], 'integrity')
                self.assertTrue(rows[-1]['success'])
                self.assertEqual([r['phase'] for r in rows if r['phase'].endswith('_return')],
                                 ['publish_return', 'asyncio_run_return'])

    def test_actual_publish_and_runner_outcomes(self):
        with tempfile.TemporaryDirectory() as root:
            recorder = tracing.Recorder(Path(root) / 'trace.jsonl')
            recorder.active = True
            async def publish(code):
                with recorder.phase('capture_drain'):
                    pass
                return code
            for code in (0, 7):
                self.assertEqual(recorder.runner(asyncio.run, recorder.publish(publish, code)), code)
            async def failing():
                raise LookupError('never logged')
            with self.assertRaises(LookupError):
                recorder.runner(asyncio.run, recorder.publish(failing))
            recorder.finish()
            os.close(recorder.fd)
            rows = [json.loads(line) for line in (Path(root) / 'trace.jsonl').read_text().splitlines()]
            self.assertEqual([r['phase'] for r in rows[:4]],
                             ['capture_drain', 'capture_drain', 'publish_return', 'asyncio_run_return'])
            self.assertFalse(rows[-3]['success'])
            self.assertFalse(rows[-2]['success'])
            self.assertNotIn('never logged', str(rows))

    def test_ffi_order_arguments_and_original_exception_survive_write_failure(self):
        with tempfile.TemporaryDirectory() as root:
            recorder = tracing.Recorder(Path(root) / 'trace.jsonl')
            events = []
            class FakeFFI:
                def livekit_ffi_drop_handle(self, value):
                    events.append(('drop', value))
                    return False
                def livekit_ffi_dispose(self):
                    events.append(('dispose',))
                    raise RuntimeError('native failure')
            ffi = FakeFFI()
            recorder.wrap_ffi(ffi)
            recorder.active = True
            with patch.object(tracing.os, 'write', side_effect=OSError('disk full')):
                self.assertIs(ffi.livekit_ffi_drop_handle(19), False)
                with self.assertRaisesRegex(RuntimeError, 'native failure'):
                    ffi.livekit_ffi_dispose()
            recorder.finish()
            os.close(recorder.fd)
            self.assertEqual(events, [('drop', 19), ('dispose',)])
            row = json.loads((Path(root) / 'trace.jsonl').read_text())
            self.assertEqual(row['phase'], 'integrity')
            self.assertFalse(row['success'])

    def test_frozen_source_compiles_without_sdk_and_file_is_exclusive(self):
        source = Path(__file__).resolve().parents[1] / 'tools/product_acceptance/product_pilot_load.py'
        tracing.instrument(source.read_text(encoding='utf-8'), str(source))
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'trace.jsonl'
            recorder = tracing.Recorder(path)
            recorder.emit('capture_drain', 'begin')
            self.assertEqual(path.stat().st_size, 0)
            with self.assertRaises(FileExistsError):
                tracing.Recorder(path)
            recorder.active = True
            recorder.MAX_RECORDS = 3
            for _ in range(9):
                recorder.emit('native_drop_handle', 'begin')
            recorder.finish()
            os.close(recorder.fd)
            rows = [json.loads(line) for line in path.read_text().splitlines()]
            self.assertEqual(len(rows), 3)
            self.assertFalse(rows[-1]['success'])
            self.assertEqual(rows[-1]['dropped'], 7)


if __name__ == '__main__':
    unittest.main()
