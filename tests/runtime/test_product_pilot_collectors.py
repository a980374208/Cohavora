"""Deterministic safety checks for collectors; no runtime PASS is inferred."""
import unittest
import hashlib
import json
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import Mock, patch

from product_pilot_checkpoints import read_committed
from product_pilot_performance import window_p95
from product_pilot_context import validate_context
from product_pilot_diagnostics import safe_event
from verify_product_external import archive_session
from release_product_acceptance import evaluate
from product_pilot_audio import review_outbound_audio
from copy import deepcopy
from datetime import datetime, timezone


class OutboundAudioContracts(unittest.TestCase):
    def setUp(self):
        self.run="a"*32
        self.devices=dict(run_id=self.run,collector="independent_windows_mmdevice",
            enumeration_hresult="00000000",default_capture_hresult="00000000",active_capture_endpoints=1)
        def utc(second): return datetime.fromtimestamp(second,timezone.utc).isoformat()
        self.actions=[dict(run_id=self.run,cycle=2,cycle_id="b"*32,pid=42,process_run_id="c"*32,
            anonymous_session_id="d"*32,participant_sha256=hashlib.sha256((self.run+":product").encode()).hexdigest(),
            operation_id="join",action="join",phase="uia_observed",utc=utc(100))]
        self.actions.append(dict(self.actions[0],operation_id="leave",action="leave",phase="requested",utc=utc(110)))
        def row(event,second,**values):
            return dict(run_id=self.run,event=event,utc=utc(second),
                observer_context={k:v for k,v in self.actions[second>=110].items() if k!="utc"},**values)
        self.remote=[row("receiver.participant_joined",99,participant="product"),
            row("receiver.track_subscribed",99,participant="product",sid="mic-2",source=2,kind="audio")]
        for i in range(10):
            self.remote += [row("receiver.sample",100+i,participant="product",sid="mic-2",source=2,kind="audio",
                active=True,window_frames=50,frames=50*(i+1),samples=48000*(i+1),window_max_gap_ms=25,silence_ms=10),
                row("receiver.connection_sample",100+i,active_track_sids=["mic-2"],
                    inbound=[dict(rtc=dict(id="audio-stream-2"),stream=dict(kind="audio",codec_id="opus"),received=dict(packets_received=20*i+1))],
                    codecs=[dict(rtc=dict(id="opus"),codec=dict(mime_type="audio/opus"))])]
        self.remote.append(row("receiver.track_unpublished",110,participant="product",sid="mic-2",source=2))

    def review(self,remote=None,outcomes=None):
        return review_outbound_audio(self.run,self.actions,self.remote if remote is None else remote,self.devices,outcomes or [],200)

    def test_audio_packets_and_pcm_cover_correlated_rejoin(self):
        report=self.review()
        self.assertTrue(all(c["status"]=="PASS" for c in report.values()))
        self.assertEqual(report["receiver_audio_rtp_stats"]["detail"]["packets_received_delta_by_stream"],{"audio-stream-2":180})

    def test_stale_prior_cycle_audio_cannot_pass(self):
        rows=deepcopy(self.remote)
        for r in rows: r["observer_context"]["cycle_id"]="old-cycle"
        self.assertTrue(all(c["status"]=="FAIL" for c in self.review(rows).values()))

    def test_stale_packet_counter_cannot_pass_with_pcm_callbacks(self):
        rows=deepcopy(self.remote)
        for r in rows:
            if r["event"]=="receiver.connection_sample": r["inbound"][0]["received"]["packets_received"]=9999
        self.assertEqual(self.review(rows)["receiver_audio_rtp_stats"]["status"],"FAIL")

    def test_decode_gap_or_stalled_samples_fails(self):
        for field,value in (("window_max_gap_ms",201),("silence_ms",None),("samples",0)):
            rows=deepcopy(self.remote)
            rows[6][field]=value
            self.assertEqual(self.review(rows)["outbound_audio_pcm_continuity"]["status"],"FAIL")

    def test_missing_tail_or_release_fails(self):
        self.assertTrue(all(c["status"]=="FAIL" for c in self.review(self.remote[:-1]).values()))
        rows=[r for r in self.remote if not (r["event"]=="receiver.sample" and r["utc"]>=self.remote[14]["utc"])]
        self.assertEqual(self.review(rows)["outbound_audio_pcm_continuity"]["status"],"FAIL")

    def test_deferred_requires_os_and_same_cycle_uia_no_device(self):
        outcome=dict(run_id=self.run,cycle=2,device="microphone",result="DEFERRED")
        self.assertTrue(all(c["status"]=="FAIL" for c in self.review(outcomes=[outcome]).values()))
        self.devices.update(active_capture_endpoints=0,default_capture_hresult="80070490")
        self.assertTrue(all(c["status"]=="FAIL" for c in self.review().values()))
        self.assertTrue(all(c["status"]=="DEFERRED" for c in self.review(outcomes=[outcome]).values()))
        outcome["cycle"]=1
        self.assertTrue(all(c["status"]=="FAIL" for c in self.review(outcomes=[outcome]).values()))


class CollectorContracts(unittest.TestCase):
    def test_archive_proves_real_metric_schema_without_inventing_session_field(self):
        with TemporaryDirectory() as directory:
            root=Path(directory); session="a"*32
            (root/session).mkdir()
            payload=b''.join((json.dumps(dict(revision=i,session_generation=7,key="queue.depth",value=0))+"\n").encode()
                             for i in (1,2,2,3))
            name="segment-00000000000000000003.jsonl"
            (root/session/name).write_bytes(payload)
            (root/session/"manifest.json").write_text(json.dumps(dict(anonymous_session_id=session,
                last_committed_revision=3,session_generation=7,session_complete=True,pruned_records=0)))
            entry=dict(file=name,first_revision=1,last_revision=3,size_bytes=len(payload),
                       sha256=hashlib.sha256(payload).hexdigest())
            self.assertEqual(archive_session(root,session,[entry])["missing_revisions"],0)
            (root/session/name).write_bytes(payload.replace(b'"revision": 1',b'"revision": 2'))
            with self.assertRaisesRegex(ValueError,"hash_or_size"):
                archive_session(root,session,[entry])

    def test_formal_gate_rejects_single_pair(self):
        with self.assertRaisesRegex(ValueError,"three_distinct"):
            evaluate([Path("single-pilot")],{})

    def test_diagnostic_witness_keeps_native_ids_without_business_text(self):
        run = "a"*32
        event = dict(schema_version=1, process_run_id=run, event_sequence=1,
                     occurred_at_utc_ms=2, monotonic_us=3, pid=4,
                     event_name="room.connected", severity="info",
                     operation_id="connect:1:3", attributes={"bytes": 10})
        witness = safe_event(event, run)
        self.assertNotIn("attributes", witness)
        self.assertEqual(witness["operation_id"], "connect:1:3")
        with self.assertRaisesRegex(ValueError, "privacy"):
            safe_event(dict(event, attributes={"body":"private"}),run)
        with self.assertRaisesRegex(ValueError, "privacy"):
            safe_event(dict(event, operation_id="https://private.example"), run)

    def test_context_rejects_cross_run_or_unbounded_fields(self):
        run = "a" * 32
        value = dict(run_id=run, cycle=1, cycle_id="b"*32, operation_id="c"*32,
                     action="join", phase="requested", pid=10,
                     process_run_id="d"*32, anonymous_session_id=None, participant_sha256=None)
        self.assertEqual(validate_context(value, run), value)
        with self.assertRaisesRegex(ValueError, "run"):
            validate_context(value, "e"*32)
        with self.assertRaisesRegex(ValueError, "schema"):
            validate_context(dict(value, token="must_not_be_carried"), run)

    def test_atomic_replace_access_conflict_retries_without_skipping(self):
        path = Mock()
        path.read_bytes.side_effect = [PermissionError(), b"committed"]
        with patch("product_pilot_checkpoints.time.sleep"):
            self.assertEqual(read_committed(path), b"committed")
        self.assertEqual(path.read_bytes.call_count, 2)

    def test_persistent_access_denial_remains_failure(self):
        path = Mock()
        path.read_bytes.side_effect = PermissionError()
        with patch("product_pilot_checkpoints.time.sleep"):
            with self.assertRaises(PermissionError):
                read_committed(path)
        self.assertEqual(path.read_bytes.call_count, 21)

    def test_percentile_uses_only_new_intervals(self):
        before = {"render_interval_histogram_ms": {"900": 10000}}
        after = {"render_interval_histogram_ms": {"900": 10000, "201": 200}}
        self.assertEqual(window_p95(before, after),
                         dict(samples=200, lower_ms=200, upper_ms=201))

    def test_retired_binding_cannot_look_like_zero_work(self):
        with self.assertRaisesRegex(ValueError, "counter_reset"):
            window_p95({"render_interval_histogram_ms": {"200": 10}},
                       {"render_interval_histogram_ms": {"201": 200}})

    def test_overflow_cannot_be_fabricated_as_1000_ms(self):
        with self.assertRaisesRegex(ValueError, "unbounded"):
            window_p95({"render_interval_histogram_ms": {}},
                       {"render_interval_histogram_ms": {"1000": 200}})


if __name__ == "__main__":
    unittest.main()
