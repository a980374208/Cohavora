import base64
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/media'))
from e2ee_output_scan import scan_outputs

class OutputScanTests(unittest.TestCase):
    def test_boundary_and_persistence_encodings_are_detected_without_echo(self):
        marker = 'public-unit-test-marker'
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for index, payload in enumerate([marker.encode(), marker.encode('utf-16-le'),
                    marker.encode('utf-16-be'), base64.b64encode(marker.encode())]):
                (root / str(index)).write_bytes(b'x' * 65530 + payload)
            result = scan_outputs(root, [marker])
            self.assertEqual(result['files_with_secret'], 4)
            self.assertNotIn(marker, str(result))

    def test_empty_needles_fail_and_clean_tree_passes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); (root / 'public.txt').write_text('ordinary public fixture')
            self.assertEqual(scan_outputs(root, ['public-test-secret'])['status'], 'PASS')
            with self.assertRaises(ValueError): scan_outputs(root, [''])

    def test_missing_and_empty_output_cannot_pass(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with self.assertRaises(ValueError): scan_outputs(root / 'absent', ['public-test-secret'])
            with self.assertRaises(ValueError): scan_outputs(root, ['public-test-secret'])

if __name__ == '__main__': unittest.main()
