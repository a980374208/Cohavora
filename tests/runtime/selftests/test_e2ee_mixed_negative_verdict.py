import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/media'))
from invoke_e2ee_product import evaluate_negative_peer


class MixedNegativeTests(unittest.TestCase):
    def evidence(self):
        return [dict(event='connected'), dict(event='closed'),
                dict(event='subscribed', track='video'), dict(event='subscribed', track='audio'),
                dict(event='crypto_state', direction='rx', state='kMissingKey')]

    def test_rejection_is_required_and_received_plaintext_cannot_pass(self):
        rows = self.evidence()
        self.assertTrue(all(evaluate_negative_peer(rows).values()))
        self.assertFalse(evaluate_negative_peer(rows[:-1])['crypto_rejection_observed'])
        rows.append(dict(event='product_chat_received', valid=True))
        self.assertFalse(evaluate_negative_peer(rows)['no_data_delivery'])

    def test_any_decoded_media_or_authenticated_frame_fails(self):
        rows = self.evidence() + [dict(event='rtp', direction='rx', counters={'framesDecoded': 1, 'totalAudioEnergy': 0.1}),
                                 dict(event='crypto_state', direction='rx', state='kOk')]
        checks = evaluate_negative_peer(rows)
        self.assertFalse(checks['no_decoded_video'])
        self.assertFalse(checks['no_decoded_audio_energy'])
        self.assertFalse(checks['no_authenticated_receive'])


if __name__ == '__main__': unittest.main()
