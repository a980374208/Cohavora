import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/media'))
from invoke_e2ee_performance import compare, PEERS, summarize_audio, observation_window

class PerformanceVerdictTests(unittest.TestCase):
    def trials(self):
        return [dict(mode=mode, metrics={role: dict(cpu_p95=20 if mode == 'on' else 15,
            private_mib_p95=320 if mode == 'on' else 300, decoded_fps=28 if mode == 'on' else 30, pcm_gap_p95_upper_ms=15 if mode == 'on' else 10,
            audio_jitter_buffer_delay_ms_p95=25 if mode == 'on' else 20)
            for role in PEERS}) for mode in ('off', 'on', 'on', 'off')]

    def test_all_three_clients_must_meet_frozen_budget(self):
        trials = self.trials()
        self.assertTrue(all(all(v['checks'].values()) for v in compare(trials).values()))
        trials[1]['metrics']['late']['decoded_fps'] = 1
        self.assertFalse(compare(trials)['late']['checks']['fps'])

    def test_partial_or_zero_decode_baseline_cannot_pass(self):
        with self.assertRaises(ValueError): compare(self.trials()[:3])
        trials = self.trials()
        for row in trials:
            if row['mode'] == 'off': row['metrics']['publisher']['decoded_fps'] = 0
        with self.assertRaises(ValueError): compare(trials)

    def test_pcm_histogram_requires_actual_valid_continuous_input(self):
        first = dict(event='product_pcm', time_ms=0, track='audio1', samples=100, frames=10,
                     invalid_frames=0, clock_order_errors=0, gap_bins_us=[0] * 10)
        last = dict(first, time_ms=45000, samples=10000, frames=110, gap_bins_us=[0, 96, 4] + [0] * 7)
        self.assertEqual(summarize_audio([first, last], 0, 45000)['pcm_gap_p95_upper_ms'], 10)
        with self.assertRaises(ValueError): summarize_audio([], 0, 45000)
        for bad in [dict(last, samples=100), dict(last, invalid_frames=1), dict(last, clock_order_errors=1),
                    dict(last, gap_bins_us=[-1] + [0] * 9), dict(last, time_ms=10000)]:
            with self.assertRaises(ValueError): summarize_audio([first, bad], 0, 45000)

    def test_observation_requires_shared_duration_and_each_track_progress(self):
        rows = [dict(event='product_state', state=5, time_ms=0),
                dict(event='product_leave_requested', time_ms=620000)]
        for timestamp in range(20000, 620000, 1000):
            for track in ('a', 'b'):
                rows.append(dict(event='product_pcm', time_ms=timestamp, track=track, samples=timestamp))
                rows.append(dict(event='product_rtp', time_ms=timestamp, track=track,
                                 direction='rx', kind='video', framesDecoded=timestamp))
        self.assertEqual(observation_window({role: rows for role in PEERS})['verified_seconds'], 600)
        too_short = [dict(e, time_ms=619999) if e['event'] == 'product_leave_requested' else e for e in rows]
        with self.assertRaises(ValueError): observation_window({role: too_short for role in PEERS})
        stalled = [dict(e, samples=1) if e['event'] == 'product_pcm' and 30000 <= e['time_ms'] < 40000 else e for e in rows]
        with self.assertRaises(ValueError): observation_window({role: stalled for role in PEERS})

if __name__ == '__main__': unittest.main()
