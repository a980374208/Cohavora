"""Offline subscriber tracing tests: no SDK or service."""
import ast
import asyncio
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
PATH = ROOT / 'tests/runtime/tools/diagnostics/native_exit/subscriber_exit_phases.py'
spec = importlib.util.spec_from_file_location('subscriber_exit_phases', PATH)
tracing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tracing)
FROZEN = PATH.parent / 'fixtures/subscriber.py'

class SubscriberExitTests(unittest.TestCase):
    def test_hash_rejection_before_recorder(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'source.py'
            path.write_text('invalid source')
            with patch.object(tracing, 'Recorder') as recorder:
                with self.assertRaisesRegex(ValueError, 'subscriber_source_sha256_mismatch'):
                    tracing.main(['--diagnostic-exit-phases', str(Path(root)/'phases'), '--', str(path)])
                recorder.assert_not_called()

    def test_frozen_shape_and_no_added_gc_or_dispose(self):
        source = FROZEN.read_text(encoding='utf-8')
        self.assertEqual(hashlib.sha256(FROZEN.read_bytes()).hexdigest(), tracing.FROZEN_SOURCE_SHA256)
        code = tracing.instrument(source, str(FROZEN))
        self.assertIsNotNone(code)
        with self.assertRaisesRegex(ValueError, 'unsupported_subscriber_cleanup_shape'):
            tracing.instrument(source.replace('tasks.clear()', 'tasks = {}'), 'changed.py')
        # Execute only frozen cleanup/retirement AST against fake owners.
        tree = ast.parse(source)
        run = next(n for n in tree.body if isinstance(n, ast.AsyncFunctionDef) and n.name == 'run')
        cleanup = next(n for n in run.body if isinstance(n, ast.Try) and n.finalbody)
        cli = next(n for n in tree.body if isinstance(n, ast.AsyncFunctionDef) and n.name == 'run_cli')
        retirement = cli.body[0].finalbody
        events = []
        class FakeRecorder:
            def activate(self): events.append('cleanup')
            def phase(self, name):
                import contextlib
                @contextlib.contextmanager
                def context():
                    events.append(name)
                    yield
                return context()
        # Capture transformed tree before compile to verify exact GC/dispose count.
        real_compile = compile
        captured = []
        with patch('builtins.compile', side_effect=lambda obj,*a,**k: (captured.append(obj) if isinstance(obj,ast.Module) else None) or real_compile(obj,*a,**k)):
            tracing.instrument(source, str(FROZEN))
        transformed = captured[-1]
        original_calls = [ast.unparse(n.func) for n in ast.walk(tree) if isinstance(n, ast.Call)]
        new_calls = [ast.unparse(n.func) for n in ast.walk(transformed) if isinstance(n, ast.Call)]
        self.assertEqual(new_calls.count('gc.collect'), original_calls.count('gc.collect'))
        self.assertFalse(any('dispose' in name for name in new_calls))
        run2 = next(n for n in transformed.body if isinstance(n,ast.AsyncFunctionDef) and n.name=='run')
        clean2 = next(n for n in run2.body if isinstance(n,ast.Try) and n.finalbody)
        phases = [n.items[0].context_expr.args[0].value for n in clean2.finalbody if isinstance(n,ast.With)]
        self.assertEqual(phases, ['subscription_cancel_gather','result_commit','reference_clear'])
        self.assertEqual(clean2.finalbody[2].body[0].items[0].context_expr.args[0].value, 'room_disconnect')

    def test_return_failure_and_ffi_exception_transparency(self):
        with tempfile.TemporaryDirectory() as root:
            path=Path(root)/'trace'
            recorder=tracing.Recorder(path)
            recorder.active=True
            async def run(code): return code
            self.assertEqual(recorder.runner(asyncio.run, recorder.retirement(recorder.subscriber_run, run, 1)),1)
            class FFI:
                def livekit_ffi_drop_handle(self, value): return value
                def livekit_ffi_dispose(self): raise LookupError('unchanged')
            ffi=FFI()
            recorder.wrap_ffi(ffi)
            self.assertEqual(ffi.livekit_ffi_drop_handle(9),9)
            with self.assertRaisesRegex(LookupError,'unchanged'): ffi.livekit_ffi_dispose()
            recorder.finish()
            os.close(recorder.fd)
            rows=[json.loads(line) for line in path.read_text().splitlines()]
            self.assertEqual([r['phase'] for r in rows[:3]],['subscriber_run_return','subscriber_retirement_return','asyncio_run_return'])
            self.assertFalse(any(r['success'] for r in rows[:3]))
            self.assertFalse(next(r['success'] for r in rows if r['phase']=='native_ffi_dispose' and r['kind']=='end'))

if __name__=='__main__': unittest.main()
