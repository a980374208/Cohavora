import copy
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools/product_acceptance"))
from product_gpu_etw import EVENT_IDS, LAYOUT, reconstruct, release_observation, interval_observation, validate_capture, validate_live_capture, review_hybrid_cycle, validate_frozen_limits

BASE = 134354900000000000
PID = 501


def event(eid, offset, fields, version=None, header_pid=0):
    if version is None:
        version = {27:2,28:2,178:1,180:1}.get(eid,0)
    return dict(sequence=0, filetime_100ns=BASE+offset, utc_ms=(BASE+offset)//10000-11644473600000,
                metadata_error=0,event_id=eid,version=version,task=LAYOUT[eid][0],opcode=LAYOUT[eid][1],
                header_pid=header_pid,fields=fields)


def fixture():
    return [event(27,1,dict(hProcessId=PID,hDevice=1000,pDxgAdapter=5000)),
            event(30,2,dict(hDevice=1000,hContext=2000,NodeOrdinal=1)),
            event(178,3,dict(hContext=2000,SubmitSequence=1,pQueuePacket=3000,PacketType=0)),
            event(180,4,dict(hContext=2000,SubmitSequence=1,pQueuePacket=3000,bPreempted=0,bTimeouted=0)),
            event(31,5,dict(hDevice=1000,hContext=2000)),
            event(28,6,dict(hProcessId=PID,hDevice=1000,pDxgAdapter=5000))]


def decoded(rows):
    for i,row in enumerate(rows,1):row["sequence"]=i
    return dict(events_kept=len(rows),trace_start_filetime_100ns=BASE,trace_end_filetime_100ns=BASE+100000000)


class GpuEtwContracts(unittest.TestCase):

    def test_kernel_header_pid_can_complete_owned_work(self):
        rows=fixture(); proof=reconstruct(rows,PID,decoded(rows))
        self.assertEqual(proof["counts"]["queue_completes"],1)
        self.assertEqual((proof["final_pending"],proof["final_contexts"],proof["final_devices"]),(0,0,0))
        release=release_observation(proof,BASE+7,BASE+90000000)
        self.assertEqual(release["maximum_pending_per_node"],0)
        self.assertGreater(release["window_seconds"],8)

    def test_foreign_packet_with_target_header_pid_is_not_owned(self):
        rows=fixture()
        rows.insert(3,event(178,3,dict(hContext=9999,SubmitSequence=1,pQueuePacket=8000),header_pid=PID))
        proof=reconstruct(rows,PID,decoded(rows))
        self.assertEqual(proof["counts"]["queue_submits"],1)

    def test_missing_complete_cannot_be_cleared_by_context_destroy(self):
        rows=fixture();del rows[3]
        with self.assertRaisesRegex(ValueError,"context_destroy_with_pending"):
            reconstruct(rows,PID,decoded(rows))

    def test_preemption_retains_pending_until_real_completion(self):
        rows=fixture();preempt=copy.deepcopy(rows[3]);preempt["fields"]["bPreempted"]=1
        rows.insert(3,preempt)
        proof=reconstruct(rows,PID,decoded(rows))
        self.assertEqual(proof["counts"]["queue_preemptions"],1)
        self.assertEqual(proof["counts"]["queue_completes"],1)

    def test_real_progress_schema_without_packet_pointer_cannot_retire_work(self):
        rows=fixture();rows.insert(3,event(179,3,dict(hContext=2000,SubmitSequence=1,PacketType=0)))
        proof=reconstruct(rows,PID,decoded(rows))
        self.assertEqual(proof["counts"]["queue_progress"],1)
        self.assertEqual(proof["counts"]["queue_completes"],1)
        rows=fixture();rows[3]=event(179,4,dict(hContext=2000,SubmitSequence=1,PacketType=0))
        with self.assertRaisesRegex(ValueError,"context_destroy_with_pending"):
            reconstruct(rows,PID,decoded(rows))

    def test_timeout_and_abort_are_not_healthy_completion(self):
        for eid in (180,361):
            rows=fixture()
            if eid==180:rows[3]["fields"]["bTimeouted"]=1
            else:rows[3]=event(361,4,dict(hContext=2000))
            with self.assertRaisesRegex(ValueError,"timeout|abort"):
                reconstruct(rows,PID,decoded(rows))

    def test_reused_foreign_context_pointer_invalidates_old_ownership(self):
        rows=fixture();rows.extend([
            event(30,7,dict(hDevice=9000,hContext=2000,NodeOrdinal=1),header_pid=PID),
            event(178,8,dict(hContext=2000,SubmitSequence=1,pQueuePacket=3000),header_pid=PID)])
        proof=reconstruct(rows,PID,decoded(rows))
        self.assertEqual(proof["counts"]["queue_submits"],1)

    def test_owned_late_packet_after_destroy_is_rejected(self):
        rows=fixture();rows.append(event(178,7,dict(hContext=2000,SubmitSequence=2,pQueuePacket=3001)))
        with self.assertRaisesRegex(ValueError,"packet_after_owned_context_destroy"):
            reconstruct(rows,PID,decoded(rows))

    def test_sequence_loss_clock_reordering_and_unknown_scalars_fail(self):
        for kind in ("sequence","clock","scalar"):
            rows=fixture();summary=decoded(rows)
            if kind=="sequence":rows[1]["sequence"]=50
            elif kind=="clock":rows[3]["filetime_100ns"]=BASE+2
            else:rows[2]["fields"]["pQueuePacket"]=None
            with self.assertRaises(ValueError):reconstruct(rows,PID,summary)

    def test_unknown_owned_schema_and_hardware_packets_fail_closed(self):
        for kind in ("owned","hardware"):
            rows=fixture()
            if kind=="owned":rows[2]["version"]=99
            else:rows.insert(3,event(450,3,dict(hContext=2000)))
            with self.assertRaisesRegex(ValueError,"schema"):
                reconstruct(rows,PID,decoded(rows))

    def test_release_window_cannot_extend_past_trace_end(self):
        rows=fixture();proof=reconstruct(rows,PID,decoded(rows))
        with self.assertRaisesRegex(ValueError,"release_window_invalid"):
            release_observation(proof,BASE+7,BASE+288000000000)

    def test_lossy_or_unparsed_capture_cannot_prove_release(self):
        state=dict(trace_started=True,trace_stopped=True,trace_start_exit=0,trace_query_exit=0,
                   trace_stop_exit=0,decode_exit=0,clock="perf",event_id_filter=list(EVENT_IDS))
        start=dict(start_error=0,enable_error=0,event_id_filter=list(EVENT_IDS))
        stop=dict(query_error=0,stop_error=0,events_lost=0,log_buffers_lost=0,realtime_buffers_lost=0)
        summary=dict(open_error=0,process_error=0,close_error=0,events_outside_filter=0,metadata_errors=0,
                     property_errors=0,events_lost=0,buffers_lost=0,write_failed=False,events_kept=6,
                     trace_start_filetime_100ns=BASE,trace_end_filetime_100ns=BASE+100,
                     timestamp_mode="ProcessTrace normalized FILETIME; RAW_TIMESTAMP disabled")
        validate_capture(state,start,stop,summary)
        for source,key in ((stop,"events_lost"),(stop,"realtime_buffers_lost"),(summary,"property_errors")):
            changed=copy.deepcopy(source);changed[key]=1
            with self.assertRaisesRegex(ValueError,"capture_incomplete_or_lossy"):
                validate_capture(state,start,changed if source is stop else stop,changed if source is summary else summary)

    def test_live_capture_requires_pre_process_readiness_identity_drain_and_accounting(self):
        summary=dict.fromkeys(("start_error open_error enable_error query_error stop_error process_error close_error events_outside_filter "
            "metadata_errors property_errors events_lost log_buffers_lost buffers_lost realtime_buffers_lost").split(),0)
        summary.update(complete=True,stop_requested=True,write_failed=False,collector_failed=False,target_pid=PID,
            mode="lossless realtime owner-filtered JSONL",timestamp_mode="ProcessTrace normalized FILETIME; RAW_TIMESTAMP disabled",
            trace_start_filetime_100ns=BASE,capture_ready_filetime_100ns=BASE+1,trace_end_filetime_100ns=BASE+1000,
            events_seen=10,events_kept=6,foreign_packets_filtered=4,prebind_events_kept=2,written_bytes=100,maximum_bytes=10000)
        ready={k:summary[k] for k in ("start_error","open_error","enable_error","capture_ready_filetime_100ns","mode","maximum_bytes")}
        ready["event_id_filter"]=list(EVENT_IDS)
        identity=dict(pid=PID,start_ticks=BASE+10+504911232000000000)
        validate_live_capture(ready,summary,identity,10000)
        for key,value in (("target_pid",PID+1),("stop_requested",False),("complete",False),("events_seen",11),
                          ("events_lost",1),("capture_ready_filetime_100ns",BASE+11),("write_failed",True)):
            changed=copy.deepcopy(summary);changed[key]=value
            with self.assertRaises(ValueError):validate_live_capture(ready,changed,identity,10000)

    def test_stream_truncation_remains_a_failure(self):
        rows=fixture();summary=decoded(rows)
        with self.assertRaisesRegex(ValueError,"record_count"):
            reconstruct(iter(rows[:-1]),PID,summary)

    def test_bounded_windows_match_transitions_including_exact_boundaries_and_silence(self):
        rows=fixture();summary=decoded(rows)
        windows=[(BASE,BASE+4),(BASE+5,BASE+6),(BASE+7,BASE+90000000)]
        complete=reconstruct(iter(rows),PID,summary)
        bounded=reconstruct(iter(rows),PID,summary,windows=windows)
        self.assertNotIn("transitions",bounded)
        for start,end in windows:
            self.assertEqual(interval_observation(complete,start,end),interval_observation(bounded,start,end))
        self.assertEqual(bounded["counts"],complete["counts"])
        with self.assertRaisesRegex(ValueError,"not_predeclared"):release_observation(bounded,BASE+8,BASE+10)
        with self.assertRaisesRegex(ValueError,"overlapping"):
            reconstruct(iter(rows),PID,summary,windows=[(BASE,BASE+5),(BASE+4,BASE+10)])

    def test_equal_timestamp_start_uses_final_baseline_without_inventing_submissions(self):
        rows=fixture();rows[3]["filetime_100ns"]=rows[2]["filetime_100ns"]
        rows[3]["utc_ms"]=rows[2]["utc_ms"]
        summary=decoded(rows);window=(BASE+3,BASE+6)
        complete=reconstruct(iter(rows),PID,summary)
        bounded=reconstruct(iter(rows),PID,summary,windows=[window])
        self.assertEqual(interval_observation(complete,*window),interval_observation(bounded,*window))

    def test_hybrid_diagnostic_requires_real_release_window_and_preserves_noneligibility(self):
        rows=fixture();proof=reconstruct(iter(rows),PID,decoded(rows))
        # Fixture events complete inside 1ms; the release window is seconds later.
        unix_ms=BASE//10000-11644473600000
        actions=[dict(action=a,phase=p,utc_ms=t) for a,p,t in (("join","uia_observed",unix_ms),
            ("leave","requested",unix_ms+1),("leave","uia_observed",unix_ms+2),("export","requested",unix_ms+9000))]
        sampled=dict(window="active_only",adapters={"0":{"0":dict(maximum_observed_submitted_minus_completed=1,
            dma_packets={"0":dict(faulted_delta=0)})}})
        policy=dict(status="PROVISIONAL_USER_REQUESTED_DIAGNOSTIC",diagnostic_only=True,release_eligible=False,
            etw_scope="process_owned_device_context_scheduler_packet_lifecycle",maximum_observed_pending_packets_per_scheduler_node=64,
            maximum_room_release_pending_packets_per_scheduler_node=4,maximum_dma_faults_delta=0,
            etw_maximum_pending_packets_per_scheduler_node=64,etw_maximum_room_release_pending_packets_per_scheduler_node=4)
        result=review_hybrid_cycle(proof,sampled,actions,policy)
        self.assertTrue(result["passed"]);self.assertIs(result["release_eligible"],False)
        actions[-1]["utc_ms"]+=28800000
        with self.assertRaisesRegex(ValueError,"release_window_invalid"):
            review_hybrid_cycle(proof,sampled,actions,policy)

    def test_frozen_test_limits_require_storage_and_exact_release_resources(self):
        policy=json.loads((Path(__file__).resolve().parents[1]/'tools/product_acceptance/product_gpu_queue_limits.json').read_text())
        validate_frozen_limits(policy)
        for key,value in (("status","PROVISIONAL_USER_REQUESTED_DIAGNOSTIC"),("diagnostic_only",True),
                          ("maximum_dma_faults_delta",1),("maximum_room_release_contexts",1),
                          ("minimum_free_evidence_disk_bytes",34359738368),("etw_maximum_bytes",True)):
            changed=copy.deepcopy(policy);changed[key]=value
            with self.assertRaisesRegex(ValueError,'frozen_test_limits_invalid'):validate_frozen_limits(changed)

    def test_frozen_hybrid_retains_packet_and_device_failures(self):
        policy=json.loads((Path(__file__).resolve().parents[1]/'tools/product_acceptance/product_gpu_queue_limits.json').read_text())
        rows=fixture();proof=reconstruct(iter(rows),PID,decoded(rows))
        unix_ms=BASE//10000-11644473600000
        actions=[dict(action=a,phase=p,utc_ms=t) for a,p,t in (("join","uia_observed",unix_ms),
            ("leave","requested",unix_ms+1),("leave","uia_observed",unix_ms+2),("export","requested",unix_ms+9000))]
        sampled=dict(window="active_only",adapters={"0":{"0":dict(maximum_observed_submitted_minus_completed=1,dma_packets={"0":dict(faulted_delta=0)})}})
        result=review_hybrid_cycle(proof,sampled,actions,policy,frozen=True)
        self.assertTrue(result['passed']);self.assertTrue(result['gpu_measurement_complete'])
        for kind in ('active_packet','release_device','release_context','fault'):
            changed=copy.deepcopy(proof);samples=copy.deepcopy(sampled)
            if kind=='active_packet':changed['maximum_pending_per_node']=65
            elif kind=='release_device':changed['transitions'][-1]['devices']=1
            elif kind=='release_context':changed['transitions'][-1]['contexts']=1
            else:samples['adapters']['0']['0']['dma_packets']['0']['faulted_delta']=1
            self.assertFalse(review_hybrid_cycle(changed,samples,actions,policy,frozen=True)['passed'],kind)


if __name__=="__main__":unittest.main()
