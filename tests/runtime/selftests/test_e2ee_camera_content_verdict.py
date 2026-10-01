import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/media'))
from invoke_e2ee_product import evaluate_camera_content

class CameraContentVerdict(unittest.TestCase):
    def evidence(self):
        p = [dict(event='product_camera_content_finished', errors=0, pending=0)]
        f = []
        for epoch in (4, 6):
            for _ in range(3):
                p.append(dict(event='product_camera_correspondence', epoch=epoch, valid=True, mean_luma_error=8))
                f.append(dict(event='product_camera_sample_sent', epoch=epoch))
        return p, f
    def test_pass(self):
        self.assertTrue(all(evaluate_camera_content(*self.evidence()).values()))
    def test_missing(self):
        self.assertFalse(all(evaluate_camera_content([], []).values()))
    def test_each_invalid(self):
        for mutation in ('mismatch', 'error', 'pending', 'missing_final', 'sampling', 'missing_epoch'):
            p, f = self.evidence()
            if mutation == 'mismatch': p[1]['valid'] = False
            if mutation == 'error': p[0]['errors'] = 1
            if mutation == 'pending': p[0]['pending'] = 1
            if mutation == 'missing_final': p.pop(0)
            if mutation == 'sampling': f.append(dict(event='product_camera_sample_failed'))
            if mutation == 'missing_epoch': p = [e for e in p if e.get('epoch') != 6]
            with self.subTest(mutation=mutation):
                self.assertFalse(all(evaluate_camera_content(p, f).values()))

if __name__ == '__main__': unittest.main()
