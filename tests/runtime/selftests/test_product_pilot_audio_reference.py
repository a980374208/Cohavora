"""Independent control must never count as product media; cleanup is mandatory."""
import asyncio
import contextlib
import io
import json
from pathlib import Path
import sys
from types import SimpleNamespace as NS
import unittest
from tempfile import TemporaryDirectory

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools/product_acceptance'))
from product_pilot_audio_reference import AudioReference, reference_evidence
from analyze_product_timing import analyze


class ReferenceContracts(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.events, self.streams = [], []
        owner = self
        class Room:
            def __init__(self):
                self.remote_participants = {}
                self.handlers = {}
                self.disconnected = False
            def on(self, event, callback):self.handlers[event] = callback
            async def connect(self, *args):pass
            async def disconnect(self):self.disconnected = True
        class Stream:
            def __init__(self, *args, **kwargs):
                self._ffi_handle = NS(handle=77)
                self.queue = asyncio.Queue()
                self.closed = False
                owner.streams.append(self)
            def __aiter__(self):return self
            async def __anext__(self):return await self.queue.get()
            async def aclose(self):self.closed = True
        self.rtc = NS(Room=Room, AudioStream=Stream, RoomOptions=lambda **kwargs:kwargs,
            TrackKind=NS(KIND_AUDIO=1), TrackSource=NS(SOURCE_MICROPHONE=2))
        self.stop = asyncio.Event()
        self.observer = AudioReference(self.rtc, 'ws://test', 'not-a-real-token', 'a'*32,
            lambda event, **values:self.events.append(dict(event=event, **values)), self.stop,
            ready_timeout=.05, sample_seconds=.005)
        self.peer = NS(identity='pilot-aaaaaaaa-load-00')
        self.pub = NS(sid='reference', kind=1, source=2, set_subscribed=lambda value:None)

    async def drain(self):
        for _ in range(3):await asyncio.sleep(0)

    async def test_only_exact_fixed_audio_is_subscribed(self):
        calls = []
        self.pub.set_subscribed = calls.append
        for identity, kind, source in [('product',1,2),('pilot-aaaaaaaa-load-01',1,2),
                (self.peer.identity,2,2),(self.peer.identity,1,3),(self.peer.identity,1,2)]:
            self.pub.kind, self.pub.source = kind, source
            self.observer.subscribe(self.pub, NS(identity=identity))
        self.assertEqual(calls,[True])

    async def test_shutdown_closes_stream_and_connection_without_product_events(self):
        runner = asyncio.create_task(self.observer.run())
        await self.drain()
        self.observer.subscribed(NS(kind=1), self.pub, self.peer)
        await self.drain()
        await self.streams[0].queue.put(NS(frame=NS(samples_per_channel=960)))
        await self.drain()
        self.stop.set()
        await runner
        self.assertTrue(self.streams[0].closed)
        self.assertTrue(self.observer.room.disconnected)
        self.assertEqual(self.events[-1]['status'],'COMPLETE')
        self.assertTrue(self.events[-1]['streams_closed'])
        self.assertEqual(self.events[-1]['frames'],1)
        self.assertTrue(all(r['event'].startswith('diagnostic.audio_reference_') for r in self.events))

    async def test_missing_audio_fails_and_disconnects(self):
        with self.assertRaisesRegex(RuntimeError,'reference_audio_observer_failed'):
            await self.observer.run()
        self.assertTrue(self.observer.room.disconnected)
        self.assertEqual(self.events[-1]['status'],'FAILED')

    async def test_wrong_subscription_and_duplicate_fail_closed(self):
        with self.assertRaises(ValueError):
            self.observer.subscribed(NS(kind=1), self.pub, NS(identity='product'))
        self.observer.subscribed(NS(kind=1), self.pub, self.peer)
        await self.drain()
        with self.assertRaises(ValueError):
            self.observer.subscribed(NS(kind=1), self.pub, self.peer)
        task = self.observer.tasks[self.pub.sid]
        task.cancel()
        await asyncio.gather(task,return_exceptions=True)
        self.assertTrue(self.streams[0].closed)


class ReferenceEvidenceContracts(unittest.TestCase):
    def records(self):
        remote = [dict(event='diagnostic.audio_reference_started', release_eligible=False),
            dict(event='diagnostic.audio_reference_stream_started',sid='ref',ffi_stream_handle=77),
            dict(event='diagnostic.audio_reference_sample',monotonic_s=1,frames=1,samples=960,window_max_gap_ms=20,silence_ms=0),
            dict(event='receiver.sample',kind='audio',sid='product',monotonic_s=1.1,
                utc='2026-10-03T00:00:00Z',window_max_gap_ms=220,silence_ms=0),
            dict(event='diagnostic.audio_reference_sample',monotonic_s=2,frames=51,samples=48960,window_max_gap_ms=20,silence_ms=0),
            dict(event='diagnostic.audio_reference_stream_closed',sid='ref',ffi_stream_handle=77,frames=51,active=False),
            dict(event='diagnostic.audio_reference_stopped',status='COMPLETE',streams_closed=True,release_eligible=False)]
        timing = [dict(event='audio_delivery',stream=77,monotonic_s=1,ffi_gap_ms=20,python_gap_ms=20),
            dict(event='audio_delivery',stream=88,monotonic_s=1,ffi_gap_ms=220,python_gap_ms=220)]
        return remote, timing

    def test_simultaneous_control_does_not_borrow_product_delay(self):
        remote, timing = self.records()
        proof = reference_evidence(remote,timing)
        self.assertEqual(proof['verdict'],'REFERENCE_EVIDENCE_COMPLETE')
        self.assertEqual(proof['maximum_ffi_gap_ms'],20)
        self.assertEqual(proof['product_failure_windows'][0]['product_pcm_gap_ms'],220)
        self.assertEqual(proof['product_failure_windows'][0]['reference_ffi_gap_ms'],20)
        self.assertFalse(proof['release_eligible'])

    def test_missing_control_delivery_or_closure_never_passes(self):
        remote, timing = self.records()
        for bad_remote, bad_timing in ((remote[:-1],timing),(remote,timing[1:])):
            self.assertEqual(reference_evidence(bad_remote,bad_timing)['verdict'],'REFERENCE_EVIDENCE_INCOMPLETE')

    def test_reference_counter_reset_or_coverage_gap_fails_closed(self):
        remote, timing = self.records()
        for field, value in (('frames',0),('monotonic_s',7)):
            saved = remote[4][field]
            remote[4][field] = value
            self.assertEqual(reference_evidence(remote,timing)['verdict'],'REFERENCE_EVIDENCE_INCOMPLETE')
            remote[4][field] = saved

    def test_analyzer_keeps_control_out_of_product_delay(self):
        remote, timing = self.records()
        run = 'a'*32
        timing = [dict(event='timing.started',monotonic_s=.1)] + timing + [dict(event='timing.stopped',monotonic_s=3,lost=0)]
        for row in timing:
            if row['event'] == 'audio_delivery':row.update(ffi_to_python_ms=1,copy_ms=1)
        with TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'plan.json').write_text(json.dumps(dict(run_id=run,diagnostic_only=True,
                release_eligible=False,diagnostic_audio_reference=dict(required=True))))
            for name, rows in (('remote',remote),('timing',timing)):
                (root/(name+'.jsonl')).write_text('\n'.join(json.dumps(dict(r,run_id=run,sequence=i)) for i,r in enumerate(rows,1)))
            with contextlib.redirect_stdout(io.StringIO()):self.assertEqual(analyze(root),0)
            report = json.loads((root/'timing-analysis.json').read_text())
            self.assertEqual(report['ffi_entry_gap_ms'],220)
            self.assertEqual(report['audio_reference']['maximum_ffi_gap_ms'],20)
            self.assertEqual(report['pcm_fail_intervals'][0]['ffi_stream_handles'],[88])


if __name__ == '__main__':unittest.main()
