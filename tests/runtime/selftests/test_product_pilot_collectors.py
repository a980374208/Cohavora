"""Deterministic safety checks for collectors; no runtime PASS is inferred."""
import unittest
import hashlib
import json
import itertools
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import Mock, patch

# Support direct execution and unittest discovery from any working directory.
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/product_acceptance"))

from product_pilot_checkpoints import read_committed, collect
from product_pilot_archive import encode_segment, read_segment
from product_pilot_performance import window_p95
from product_pilot_context import validate_context
from product_pilot_diagnostics import safe_event
from verify_product_external import archive_session, cleanup_release
from product_pilot_local_route import LocalSfuRoute
from product_pilot_timing import AudioArrivalWitness
from release_product_acceptance import evaluate
from product_pilot_audio import review_outbound_audio
from copy import deepcopy
from datetime import datetime, timezone
from argparse import Namespace


class AudioTimingContracts(unittest.TestCase):
    def test_broadcast_duplicate_preserves_first_arrival_and_true_native_gap(self):
        witness=AudioArrivalWitness()
        witness.arrival(1,7,1.0)
        witness.arrival(1,7,1.1)
        self.assertEqual(witness.copied(1,1.15)["ffi_arrival_s"],1.0)
        witness.arrival(1,7,1.19)
        self.assertNotIn(1,witness.pending)
        witness.arrival(2,7,1.02)
        witness.arrival(2,7,1.21)
        proof=witness.copied(2,1.3)
        self.assertAlmostEqual(proof["ffi_gap_ms"],20)
        self.assertAlmostEqual(proof["ffi_to_python_ms"],280)
        self.assertAlmostEqual(proof["python_gap_ms"],150)

    def test_unmatched_or_cross_stream_frame_cannot_fabricate_timing(self):
        witness=AudioArrivalWitness()
        self.assertIsNone(witness.copied(999,1.0))
        witness.arrival(1,1,1.0);witness.arrival(2,2,1.2)
        self.assertIsNone(witness.copied(2,1.3)["ffi_gap_ms"])
        self.assertIsNone(witness.copied(1,1.4)["python_gap_ms"])

    def test_witness_overflow_remains_explicit_loss(self):
        witness=AudioArrivalWitness()
        for i in range(4097):witness.arrival(i,1,i/50)
        self.assertEqual(witness.lost,1)
        self.assertIsNone(witness.copied(4096,99))

    def test_timing_diagnostic_cannot_become_formal_release_evidence(self):
        with TemporaryDirectory() as directory:
            roots=[Path(directory)/str(i) for i in range(3)]
            for root in roots:root.mkdir()
            (roots[0]/"diagnostic-debugger.json").write_text(json.dumps(dict(kind="audio_timing")))
            with self.assertRaisesRegex(ValueError,"diagnostic_run_not_release_eligible"):
                evaluate(roots,{})


class LocalSfuRouteContracts(unittest.TestCase):
    def target(self):
        return dict(collector_media_route=dict(public_ip="123.56.225.164",
            private_ip="172.17.54.189",udp_port=17882,tcp_port=17881))

    def test_rules_cannot_redirect_shared_processes_or_other_sfu_ports(self):
        route=LocalSfuRoute(self.target(),"b"*32,Path("result.json"))
        for protocol,port in route.ports:
            rule=route.rule(protocol,port)
            self.assertEqual(rule[rule.index("--cgroup")+1],str(route.classid))
            self.assertEqual(rule[rule.index("-d")+1],"123.56.225.164")
            self.assertEqual(rule[rule.index("--dport")+1],str(port))
            self.assertEqual(rule[rule.index("--to-destination")+1],f"172.17.54.189:{port}")
            self.assertEqual(route.command("-I",rule)[:7],["iptables","-w","5","-t","nat","-I","OUTPUT"])

    def test_partial_install_failure_removes_only_successfully_inserted_rule(self):
        import subprocess
        with TemporaryDirectory() as directory:
            root=Path(directory)
            route=LocalSfuRoute(self.target(),"b"*32,root/"proof.json")
            route.mount=root;route.group=root/"group"
            results=[Mock(returncode=0),subprocess.CalledProcessError(1,["iptables"]),Mock(returncode=0)]
            with patch("product_pilot_local_route.subprocess.run",side_effect=results) as run, \
                 patch.object(Path,"rmdir") as remove:
                with self.assertRaises(subprocess.CalledProcessError):
                    route.__enter__()
                self.assertEqual(run.call_count,3)
                inserted=run.call_args_list[0].args[0]
                removed=run.call_args_list[2].args[0]
                self.assertEqual(removed,inserted[:5]+["-D"]+inserted[6:])
                remove.assert_called_once()
            self.assertTrue(json.loads((root/"proof.json").read_text())["cleanup_complete"])

    def test_collision_and_invalid_target_fail_before_network_mutation(self):
        with TemporaryDirectory() as directory:
            root=Path(directory)
            route=LocalSfuRoute(self.target(),"b"*32,root/"proof.json")
            route.mount=root
            (root/"net_cls.classid").write_text(str(route.classid))
            with patch("product_pilot_local_route.subprocess.run") as run:
                with self.assertRaisesRegex(ValueError,"collision"):
                    route.__enter__()
                run.assert_not_called()
        target=self.target();target["collector_media_route"]["private_ip"]="8.8.8.8"
        with self.assertRaisesRegex(ValueError,"addresses"):
            LocalSfuRoute(target,"b"*32,Path("result.json"))


class CleanupReleaseContracts(unittest.TestCase):
    def setUp(self):
        def at(seconds, **values):
            return dict(utc_ms=seconds*1000, **values)
        self.actions = {
            ("join", "uia_observed"): at(0, anonymous_session_id="old"),
            ("leave", "uia_observed"): at(100),
            ("export", "uia_observed"): at(116)}
        self.probe = [at(i, anonymous_session_id="old", session_complete=True,
                         native_cleanup_pending=0) for i in range(100, 119)]
        self.next_join = at(118)

    def test_next_join_global_work_cannot_be_attributed_to_old_sid(self):
        self.probe[-1]["native_cleanup_pending"] = 1
        passed, proof = cleanup_release(self.probe, self.actions, self.next_join)
        self.assertTrue(passed)
        self.assertEqual(proof["samples"], 11)
        self.assertEqual(proof["maximum_pending"], 0)

    def test_pending_work_in_release_window_fails_even_with_zero_last_sample(self):
        self.probe[8]["native_cleanup_pending"] = 1
        self.assertFalse(cleanup_release(self.probe, self.actions, self.next_join)[0])

    def test_missing_short_stale_and_gapped_release_evidence_fail(self):
        for rows in ([], self.probe[:9], self.probe[12:],
                     self.probe[:8]+self.probe[12:]):
            self.assertFalse(cleanup_release(rows, self.actions, self.next_join)[0])
        for row in self.probe:
            row["anonymous_session_id"] = "different"
        self.assertFalse(cleanup_release(self.probe, self.actions, self.next_join)[0])

    def test_overlapping_next_join_limits_release_window_and_last_cycle_is_bounded(self):
        self.assertFalse(cleanup_release(self.probe, self.actions, dict(utc_ms=108000))[0])
        self.probe[-1]["native_cleanup_pending"] = 1
        self.assertTrue(cleanup_release(self.probe, self.actions)[0])


class LosslessArchiveContracts(unittest.TestCase):
    def fixture(self, root):
        session="a"*32
        run="b"*32
        process="c"*32
        native=root/"native"/"cohavora-telemetry-v2-current"
        native.mkdir(parents=True)
        payload=b''.join((json.dumps(dict(revision=i,session_generation=7,
            key="queue.depth",value=0,padding="x"*8192))+"\n").encode() for i in (1,2,3))
        name="segment-00000000000000000003.jsonl"
        entry=dict(file=name,first_revision=1,last_revision=3,size_bytes=len(payload),
            sha256=hashlib.sha256(payload).hexdigest())
        (native/name).write_bytes(payload)
        (native/"manifest.json").write_text(json.dumps(dict(process_run_id=process,
            anonymous_session_id=session,last_committed_revision=3,
            session_generation=7,session_complete=True,pruned_records=0,segments=[entry])))
        probe=root/"probe.jsonl"
        probe.write_text(json.dumps(dict(run_id=run,process_run_id=process,
            history_root=str(native.parent)))+"\n")
        result=root/"uia"/"uia-result.json"
        result.parent.mkdir()
        result.write_text("{}")
        stored, storage=encode_segment(name,payload)
        args=Namespace(probe=probe,result=result,output=root/"archive",run_id=run,
            seconds=30,maximum_bytes=len(stored))
        return session,payload,entry,stored,storage,args

    def collect(self, args):
        ticks=itertools.count()
        with patch("product_pilot_checkpoints.time.monotonic",side_effect=lambda:next(ticks)), \
             patch("product_pilot_checkpoints.time.sleep"):
            return collect(args)

    def test_compressed_budget_preserves_all_native_bytes_and_revision_proof(self):
        with TemporaryDirectory() as directory:
            session,payload,entry,stored,storage,args=self.fixture(Path(directory))
            self.assertGreater(len(payload),args.maximum_bytes)
            self.assertEqual(self.collect(args),0)
            events=[json.loads(line) for line in (args.output/"collector.jsonl").read_text().splitlines()]
            archived=next(row for row in events if row["event"]=="segment.archived")
            self.assertEqual(read_segment(args.output/session,archived),payload)
            proof=archive_session(args.output,session,[archived])
            self.assertEqual(proof["missing_revisions"],0)
            self.assertEqual(proof["archived_bytes"],len(payload))
            self.assertEqual(proof["stored_segment_bytes"],len(stored))
            self.assertEqual(events[-1]["bytes"],args.maximum_bytes)
            self.assertEqual(events[-1]["raw_bytes"],len(payload))

    def test_one_byte_over_storage_budget_still_fails_before_segment_write(self):
        with TemporaryDirectory() as directory:
            session,payload,entry,stored,storage,args=self.fixture(Path(directory))
            args.maximum_bytes-=1
            self.assertEqual(self.collect(args),1)
            self.assertFalse((args.output/session/storage["archive_file"]).exists())
            event=json.loads((args.output/"collector.jsonl").read_text().splitlines()[-1])
            self.assertEqual(event["reason"],"external_archive_budget_exceeded")

    def test_storage_tampering_and_native_hash_tampering_both_fail(self):
        with TemporaryDirectory() as directory:
            session,payload,entry,stored,storage,args=self.fixture(Path(directory))
            path=Path(directory)/storage["archive_file"]
            path.write_bytes(stored+b"changed")
            with self.assertRaisesRegex(ValueError,"storage_hash_or_size"):
                read_segment(Path(directory),dict(entry,**storage))
            changed,changed_storage=encode_segment(entry["file"],payload.replace(b'"revision": 1',b'"revision": 2'))
            path.write_bytes(changed)
            with self.assertRaisesRegex(ValueError,"archive_hash_or_size"):
                read_segment(Path(directory),dict(entry,**changed_storage))

    def test_declared_decoded_size_and_storage_path_are_enforced(self):
        with TemporaryDirectory() as directory:
            session,payload,entry,stored,storage,args=self.fixture(Path(directory))
            (Path(directory)/storage["archive_file"]).write_bytes(stored)
            with self.assertRaisesRegex(ValueError,"archive_hash_or_size"):
                read_segment(Path(directory),dict(entry,**storage,size_bytes=1))
            with self.assertRaisesRegex(ValueError,"storage_path"):
                read_segment(Path(directory),dict(entry,**dict(storage,archive_file="../escape.gz")))
            with self.assertRaisesRegex(ValueError,"encoding"):
                read_segment(Path(directory),dict(entry,**dict(storage,archive_encoding="unknown")))

    def test_truncated_compressed_stream_fails_even_with_matching_storage_hash(self):
        with TemporaryDirectory() as directory:
            session,payload,entry,stored,storage,args=self.fixture(Path(directory))
            truncated=stored[:-8]
            (Path(directory)/storage["archive_file"]).write_bytes(truncated)
            storage.update(stored_bytes=len(truncated),stored_sha256=hashlib.sha256(truncated).hexdigest())
            with self.assertRaisesRegex(ValueError,"compressed_payload_invalid"):
                read_segment(Path(directory),dict(entry,**storage))

    def test_valid_compression_and_hashes_cannot_hide_a_missing_revision(self):
        with TemporaryDirectory() as directory:
            session,payload,entry,stored,storage,args=self.fixture(Path(directory))
            self.assertEqual(self.collect(args),0)
            missing=b''.join(line+b"\n" for line in payload.splitlines()
                             if json.loads(line)["revision"]!=2)
            stored,storage=encode_segment(entry["file"],missing)
            (args.output/session/storage["archive_file"]).write_bytes(stored)
            entry.update(size_bytes=len(missing),sha256=hashlib.sha256(missing).hexdigest())
            with self.assertRaisesRegex(ValueError,"revision_gap"):
                archive_session(args.output,session,[dict(entry,**storage)])


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

    def test_current_writer_fields_keep_types_and_reject_raw_payloads(self):
        run="a"*32
        event=dict(schema_version=1,process_run_id=run,event_sequence=1,
                   occurred_at_utc_ms=2,monotonic_us=3,source_monotonic_us=4,
                   pid=5,event_name="rtc.sdp_step",severity="info",
                   attributes=dict(description_type="offer",action="set_local",
                       phase="completed",pc_role="publisher",round_sequence=1,
                       signaling_before="stable",signaling_after="have_local_offer",
                       after_terminal=False,ice_restart=True,cache_hit=False,
                       device_count=2,binding_epoch=1,boundary="end",begin_us=3,
                       end_us=4,threshold_us=1))
        self.assertEqual(safe_event(event,run)["event_sequence"],1)
        for value in (-1,True,2**64,"https://private.example"):
            with self.assertRaisesRegex(ValueError,"privacy_unsigned"):
                safe_event(dict(event,source_monotonic_us=value),run)
        with self.assertRaisesRegex(ValueError,"privacy_boolean"):
            safe_event(dict(event,attributes=dict(event["attributes"],cache_hit=1)),run)
        with self.assertRaisesRegex(ValueError,"privacy_unbounded"):
            safe_event(dict(event,attributes=dict(event["attributes"],description_type="https://private.example")),run)
        with self.assertRaisesRegex(ValueError,"privacy_attribute"):
            safe_event(dict(event,attributes=dict(event["attributes"],sdp="raw SDP")),run)

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
