"""Offline evidence checks for the diagnostic stack and API ownership boundary."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest


ROOT = Path(__file__).resolve().parents[1] / 'tools/diagnostics/native_exit'


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / (name + '.py'))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


stack = module('parse_rust_backtrace')
api = module('publisher_exit_api')


class DiagnosticEvidenceTests(unittest.TestCase):
    def test_native_addresses_exclude_panic_payload_and_use_mapping_offset(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            stderr = root / 'stderr.log'
            stderr.write_text("thread 'worker' panicked at source.rs:1:\nSECRET-JWT-DO-NOT-EXPORT\n"
                "stack backtrace:\n   0: 0x1023 - symbol::name\n at /private/source.rs:22\n"
                "panic in a function that cannot unwind\n")
            maps = root / 'maps.json'
            maps.write_text(json.dumps(dict(snapshots=[dict(stage='before_stop', mappings=[
                dict(start=4096, end=8192, offset=512, path='/private/libsdk.so')])])) )
            result = stack.extract(stderr, maps)
            self.assertEqual(result['status'], 'NATIVE_RUST_BACKTRACE_CAPTURED')
            self.assertEqual(result['frames'][0]['mapped_file_offset'], '0x223')
            self.assertEqual(result['frames'][0]['module'], 'libsdk.so')
            self.assertNotIn('SECRET-JWT', json.dumps(result))
            self.assertNotIn('/private/source', json.dumps(result))
            self.assertTrue(result['non_unwinding_panic'])

    def test_file_offset_is_translated_through_elf_load_segment(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'sample.so'
            header = struct.pack('<16sHHIQQQIHHHHHH', b'\x7fELF\x02\x01'+b'\x00'*10,
                3, 62, 1, 0, 64, 0, 0, 64, 56, 1, 0, 0, 0)
            ph = struct.pack('<IIQQQQQQ', 1, 5, 0x200, 0x1200, 0, 0x500, 0x500, 4096)
            path.write_bytes(header + ph)
            self.assertEqual(stack.elf_address(path, 0x223), 0x1223)
            self.assertIsNone(stack.elf_address(path, 0x800))

    def test_no_stack_and_oversized_stderr_remain_unproven(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'stderr.log'
            path.write_text('panic in a function that cannot unwind\n')
            self.assertEqual(stack.extract(path)['status'], 'NO_NATIVE_STACK_INCONCLUSIVE')
            path.write_bytes(b'x' * (stack.MAX_BYTES + 1))
            with self.assertRaises(ValueError):
                stack.extract(path)

    def test_room_membership_cannot_delete_foreign_or_replaced_identity(self):
        run_id = '1' * 32
        with self.assertRaises(api.GateError):
            api.check_members([SimpleNamespace(identity='unrelated-user')], run_id, empty=True)
        api.check_members([], run_id, empty=True)
        with self.assertRaises(api.GateError):
            api.identities('1' * 33)


if __name__ == '__main__':
    unittest.main()
