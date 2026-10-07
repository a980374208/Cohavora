"""Reconstruct process-owned scheduler packets from lossless DxgKrnl events."""
from collections import Counter

EVENT_IDS = {27, 28, 29, 30, 31, 32, 175, 176, 177, 178, 179, 180, 244, 245, 361, 450, 451}
SUPPORTED = {(27, 2), (28, 2), (29, 2), (30, 0), (31, 0), (32, 0),
             (175, 1), (176, 0), (177, 1), (178, 1), (179, 0), (180, 1),
             (244, 1), (244, 2), (245, 2), (245, 3), (361, 0)}
LAYOUT = {27:(19,1), 28:(19,2), 29:(19,3), 30:(20,1), 31:(20,2), 32:(20,3),
          175:(8,1), 176:(8,2), 177:(8,0), 178:(9,1), 179:(9,0), 180:(9,2),
          244:(9,1), 245:(9,1), 361:(202,0), 450:(8,0), 451:(8,0)}


def number(value):
    return type(value) is int and 0 <= value <= (1 << 64) - 1


def validate_capture(state, start, stop, decoded):
    if (state.get("trace_started") is not True or state.get("trace_stopped") is not True
            or any(state.get(k) != 0 for k in ("trace_start_exit", "trace_query_exit", "trace_stop_exit", "decode_exit"))
            or state.get("clock") != "perf" or set(state.get("event_id_filter", ())) != EVENT_IDS
            or start.get("start_error") != 0 or start.get("enable_error") != 0
            or set(start.get("event_id_filter", ())) != EVENT_IDS
            or any(stop.get(k) != 0 for k in ("query_error", "stop_error", "events_lost", "log_buffers_lost", "realtime_buffers_lost"))
            or any(decoded.get(k) != 0 for k in ("open_error", "process_error", "close_error", "events_outside_filter",
                                                 "metadata_errors", "property_errors", "events_lost", "buffers_lost"))
            or decoded.get("write_failed") is not False
            or decoded.get("timestamp_mode") != "ProcessTrace normalized FILETIME; RAW_TIMESTAMP disabled"
            or not number(decoded.get("events_kept")) or decoded["events_kept"] < 1
            or not number(decoded.get("trace_start_filetime_100ns"))
            or not number(decoded.get("trace_end_filetime_100ns"))
            or decoded["trace_start_filetime_100ns"] >= decoded["trace_end_filetime_100ns"]):
        raise ValueError("gpu_etw_capture_incomplete_or_lossy")


def reconstruct(events, pid, decoded, windows=None):
    """Header PID is never used as device/context ownership evidence."""
    if type(pid) is not int or pid <= 0:
        raise ValueError("gpu_etw_identity_or_record_count")
    devices, contexts, retired, generations, adapters = {}, {}, set(), Counter(), {}
    counts, pending, nodes, transitions = Counter(), {}, Counter(), []
    peak, previous_time, seen = 0, None, 0
    first, last = decoded["trace_start_filetime_100ns"], decoded["trace_end_filetime_100ns"]
    initial=dict(filetime_100ns=first,pending=0,maximum_pending_per_node=0,
        contexts=0,devices=0,queue_submits=0,queue_completes=0)
    transitions.append(initial)
    observer=_WindowObserver(windows,first,last,initial) if windows is not None else None

    def require(fields, *keys):
        if any(not number(fields.get(k)) for k in keys):
            raise ValueError("gpu_etw_required_scalar_unknown")

    def snapshot(row):
        nonlocal peak
        value = max(nodes.values(), default=0)
        peak = max(peak, value)
        state=dict(filetime_100ns=row["filetime_100ns"], pending=len(pending),
            maximum_pending_per_node=value, contexts=len(contexts), devices=len(devices),
            queue_submits=counts["queue_submits"], queue_completes=counts["queue_completes"])
        if observer is None: transitions.append(state)
        else: observer.observe(state)

    for index, row in enumerate(events, 1):
        seen = index
        if (row.get("sequence") != index or row.get("metadata_error") != 0
                or not number(row.get("filetime_100ns")) or row.get("event_id") not in EVENT_IDS
                or not isinstance(row.get("fields"), dict)
                or not first <= row["filetime_100ns"] <= last
                or row.get("utc_ms") != row["filetime_100ns"] // 10000 - 11644473600000):
            raise ValueError("gpu_etw_event_sequence_or_clock")
        now = row["filetime_100ns"]
        if previous_time is not None and now < previous_time:
            raise ValueError("gpu_etw_event_time_reordered")
        previous_time = now
        eid, fields = row["event_id"], row["fields"]
        if (row.get("task"), row.get("opcode")) != LAYOUT[eid]:
            raise ValueError("gpu_etw_descriptor_changed")
        if eid in (450,451):
            raise ValueError("gpu_etw_hardware_queue_schema_unhandled")
        if eid in (27, 28, 29):
            require(fields, "hProcessId", "hDevice", "pDxgAdapter")
            if eid == 29 and fields["hProcessId"] == pid:
                raise ValueError("gpu_etw_owned_device_rundown_only")
            if eid == 27 and fields["hProcessId"] == pid:
                if (eid,row.get("version")) not in SUPPORTED:
                    raise ValueError("gpu_etw_owned_device_schema_unsupported")
                if fields["hDevice"] in devices or fields["hDevice"] == 0 or fields["pDxgAdapter"] == 0:
                    raise ValueError("gpu_etw_duplicate_or_unknown_device")
                adapter = adapters.setdefault(fields["pDxgAdapter"], len(adapters))
                devices[fields["hDevice"]] = adapter
                counts["device_creates"] += 1
                snapshot(row)
            elif eid == 28 and fields["hDevice"] in devices:
                if (eid,row.get("version")) not in SUPPORTED:
                    raise ValueError("gpu_etw_owned_device_schema_unsupported")
                if fields["hProcessId"] != pid or any(c["device"] == fields["hDevice"] for c in contexts.values()):
                    raise ValueError("gpu_etw_device_destroy_with_live_context_or_owner_change")
                del devices[fields["hDevice"]]
                counts["device_destroys"] += 1
                snapshot(row)
            continue
        if eid in (30, 31, 32):
            require(fields, "hDevice", "hContext")
            handle = fields["hContext"]
            if eid == 32 and fields["hDevice"] in devices:
                raise ValueError("gpu_etw_owned_context_rundown_only")
            if eid == 30:
                if handle in contexts:
                    raise ValueError("gpu_etw_live_context_handle_reused")
                # A foreign birth retires attribution to an older pointer generation.
                retired.discard(handle)
                if fields["hDevice"] in devices:
                    generations[handle] += 1
                    if (eid,row.get("version")) not in SUPPORTED:
                        raise ValueError("gpu_etw_owned_context_schema_unsupported")
                    require(fields, "NodeOrdinal")
                    if handle == 0:
                        raise ValueError("gpu_etw_context_handle_unknown")
                    contexts[handle] = dict(device=fields["hDevice"], generation=generations[handle],
                        node=(devices[fields["hDevice"]], fields["NodeOrdinal"]))
                    counts["context_creates"] += 1
                    snapshot(row)
            elif eid == 31 and handle in contexts:
                if (eid,row.get("version")) not in SUPPORTED:
                    raise ValueError("gpu_etw_owned_context_schema_unsupported")
                context = contexts[handle]
                if fields["hDevice"] != context["device"] or any(key[:2] == (handle, context["generation"]) for key in pending):
                    raise ValueError("gpu_etw_context_destroy_with_pending_or_owner_change")
                del contexts[handle]
                retired.add(handle)
                counts["context_destroys"] += 1
                snapshot(row)
            continue
        handle = fields.get("hContext")
        if handle in retired:
            raise ValueError("gpu_etw_packet_after_owned_context_destroy")
        if handle not in contexts:
            continue
        if (eid, row.get("version")) not in SUPPORTED:
            raise ValueError("gpu_etw_owned_packet_schema_unsupported")
        context = contexts[handle]
        if eid == 179:
            # The observed v0 progress schema has no pQueuePacket field. It
            # does not retire work; require a unique pending context/sequence.
            require(fields, "SubmitSequence")
            prefix = (handle, context["generation"], fields["SubmitSequence"])
            if sum(key[:3] == prefix for key in pending) != 1:
                raise ValueError("gpu_etw_queue_progress_without_unique_submit")
            counts["queue_progress"] += 1
        elif eid in (178, 244, 245, 180):
            require(fields, "SubmitSequence", "pQueuePacket")
            key = (handle, context["generation"], fields["SubmitSequence"], fields["pQueuePacket"])
            if fields["pQueuePacket"] == 0:
                raise ValueError("gpu_etw_queue_packet_identity_unknown")
            if eid in (178, 244, 245):
                if any(other[:3] == key[:3] for other in pending):
                    raise ValueError("gpu_etw_duplicate_queue_submit")
                pending[key] = context["node"]
                nodes[context["node"]] += 1
                counts["queue_submits"] += 1
                snapshot(row)
            elif eid == 180:
                require(fields, "bPreempted", "bTimeouted")
                if fields["bPreempted"] not in (0, 1) or fields["bTimeouted"] not in (0, 1):
                    raise ValueError("gpu_etw_packet_terminal_flags_unknown")
                if key not in pending:
                    raise ValueError("gpu_etw_queue_complete_without_submit")
                if fields["bTimeouted"]:
                    raise ValueError("gpu_etw_queue_packet_timeout")
                if fields["bPreempted"]:
                    counts["queue_preemptions"] += 1
                else:
                    nodes[pending.pop(key)] -= 1
                    counts["queue_completes"] += 1
                    snapshot(row)
        elif eid == 177:
            require(fields, "InterruptType", "FaultedVirtualAddress", "PageFaultFlags", "FaultedProcessHandle")
            # Preserve all interrupt variants for review; do not infer unknown variants as healthy.
            counts["dma_interrupt_" + str(fields["InterruptType"])] += 1
            if fields["InterruptType"] not in (1, 2) or any(fields[k] for k in ("FaultedVirtualAddress", "PageFaultFlags", "FaultedProcessHandle")):
                raise ValueError("gpu_etw_dma_fault_or_unknown_interrupt")
        elif eid == 361:
            raise ValueError("gpu_etw_owned_queue_abort")
        else:
            counts["owned_dma_event_" + str(eid)] += 1
    if seen != decoded["events_kept"]:
        raise ValueError("gpu_etw_identity_or_record_count")
    if not counts["queue_submits"] or not counts["device_creates"] or not counts["context_creates"]:
        raise ValueError("gpu_etw_actual_process_gpu_work_unproven")
    result=dict(scope="all observed process-owned devices, contexts and scheduler queue packets",
        ownership="device payload hProcessId -> device -> context -> packet; header PID is not ownership",
        counts=dict(counts), physical_adapter_aliases=len(adapters), maximum_pending_per_node=peak,
        final_pending=len(pending), final_contexts=len(contexts), final_devices=len(devices), transitions=transitions,
        trace_start_filetime_100ns=first, trace_end_filetime_100ns=last,
        release_eligible=False, limitations=["kernel scheduler queue packets; not a hardware queue depth",
            "lossless complete capture and pre-process start boundary required", "schema variants fail closed"])
    if observer is not None:
        result.pop("transitions")
        result["window_observations"]=observer.finish()
        result["reconstruction_storage"]="live ownership/pending state and predeclared disjoint windows; no event-sized transition list"
    return result


class _WindowObserver:
    """Carry terminal state across silent windows without retaining transitions."""
    def __init__(self, windows, first, last, initial):
        self.windows=sorted(set(windows));self.last=last;self.previous=initial
        self.index=0;self.current=None;self.results={}
        for i,(start,end) in enumerate(self.windows):
            if (not number(start) or not number(end) or not first<=start<end<=last
                    or (i and self.windows[i-1][1]>=start)):
                raise ValueError("gpu_etw_predeclared_windows_invalid_or_overlapping")

    def open(self, baseline):
        start,end=self.windows[self.index]
        self.current=dict(maximum_pending_per_node=baseline["maximum_pending_per_node"],
            maximum_contexts=baseline["contexts"],maximum_devices=baseline["devices"],window_seconds=(end-start)/10000000,
            absence_evidence="matched device/context destroys and queue completes in lossless continuous ETW",release_eligible=False,
            queue_submits_baseline=baseline["queue_submits"],queue_completes_baseline=baseline["queue_completes"])

    def close(self):
        start,end=self.windows[self.index]
        if self.current is None:self.open(self.previous)
        self.current["queue_submits"]=self.previous["queue_submits"]-self.current.pop("queue_submits_baseline")
        self.current["queue_completes"]=self.previous["queue_completes"]-self.current.pop("queue_completes_baseline")
        self.results[f"{start}:{end}"]=self.current
        self.index+=1;self.current=None

    def observe(self, state):
        now=state["filetime_100ns"]
        while self.index<len(self.windows) and self.windows[self.index][1]<now:self.close()
        if self.index<len(self.windows) and self.windows[self.index][0]<=now:
            if now==self.windows[self.index][0]:self.open(state)
            elif self.current is None:self.open(self.previous)
            for key,source in (("maximum_pending_per_node","maximum_pending_per_node"),("maximum_contexts","contexts"),("maximum_devices","devices")):
                self.current[key]=max(self.current[key],state[source])
        self.previous=state

    def finish(self):
        while self.index<len(self.windows):self.close()
        return self.results


def release_observation(proof, start_filetime, end_filetime):
    """Retain state through an event-silent interval after resource destruction."""
    if (not number(start_filetime) or not number(end_filetime)
            or not proof["trace_start_filetime_100ns"] <= start_filetime < end_filetime <= proof["trace_end_filetime_100ns"]):
        raise ValueError("gpu_etw_release_window_invalid")
    if "window_observations" in proof:
        key=f"{start_filetime}:{end_filetime}"
        if key not in proof["window_observations"]:raise ValueError("gpu_etw_window_not_predeclared")
        return {k:v for k,v in proof["window_observations"][key].items() if k not in ("queue_submits","queue_completes")}
    before = [x for x in proof["transitions"] if x["filetime_100ns"] <= start_filetime]
    if not before:
        raise ValueError("gpu_etw_release_baseline_missing")
    states = [before[-1]] + [x for x in proof["transitions"] if start_filetime < x["filetime_100ns"] <= end_filetime]
    return dict(maximum_pending_per_node=max(x["maximum_pending_per_node"] for x in states),
        maximum_contexts=max(x["contexts"] for x in states), maximum_devices=max(x["devices"] for x in states),
        window_seconds=(end_filetime-start_filetime)/10000000,
        absence_evidence="matched device/context destroys and queue completes in lossless continuous ETW",
        release_eligible=False)


def validate_live_capture(ready, summary, identity, maximum_bytes):
    """A stopped, drained real-time capture is needed before absence is proof."""
    zero = ("start_error", "open_error", "enable_error", "query_error", "stop_error", "process_error", "close_error",
            "events_outside_filter", "metadata_errors", "property_errors", "events_lost", "log_buffers_lost",
            "buffers_lost", "realtime_buffers_lost")
    pid, ticks = identity.get("pid"), identity.get("start_ticks")
    if (any(summary.get(key) != 0 for key in zero) or any(ready.get(key) != 0 for key in ("start_error", "open_error", "enable_error"))
            or summary.get("complete") is not True or summary.get("stop_requested") is not True
            or summary.get("write_failed") is not False or summary.get("collector_failed") is not False
            or set(ready.get("event_id_filter", ())) != EVENT_IDS
            or summary.get("mode") != "lossless realtime owner-filtered JSONL" or ready.get("mode") != summary["mode"]
            or summary.get("timestamp_mode") != "ProcessTrace normalized FILETIME; RAW_TIMESTAMP disabled"
            or not number(pid) or not pid or summary.get("target_pid") != pid
            or not number(ticks) or not number(summary.get("trace_start_filetime_100ns"))
            or not number(summary.get("capture_ready_filetime_100ns")) or not number(summary.get("trace_end_filetime_100ns"))
            or ready.get("capture_ready_filetime_100ns") != summary["capture_ready_filetime_100ns"]
            or not summary["trace_start_filetime_100ns"] <= summary["capture_ready_filetime_100ns"] < ticks-504911232000000000 < summary["trace_end_filetime_100ns"]
            or any(not number(summary.get(key)) for key in ("events_seen", "events_kept", "foreign_packets_filtered", "prebind_events_kept", "written_bytes"))
            or not 0 < summary["events_kept"] <= summary["events_seen"]
            or summary["events_seen"] != summary["events_kept"] + summary["foreign_packets_filtered"]
            or not 0 < summary["written_bytes"] <= maximum_bytes or summary.get("maximum_bytes") != maximum_bytes
            or ready.get("maximum_bytes") != maximum_bytes):
        raise ValueError("gpu_etw_live_capture_incomplete_or_identity_changed")


def interval_observation(proof, start_filetime, end_filetime):
    result = release_observation(proof, start_filetime, end_filetime)
    if "window_observations" in proof:
        result.update({k:v for k,v in proof["window_observations"][f"{start_filetime}:{end_filetime}"].items() if k in ("queue_submits","queue_completes")})
        return result
    before = [x for x in proof["transitions"] if x["filetime_100ns"] <= start_filetime]
    during = [x for x in proof["transitions"] if start_filetime < x["filetime_100ns"] <= end_filetime]
    last = during[-1] if during else before[-1]
    result.update(queue_submits=last["queue_submits"]-before[-1]["queue_submits"],
                  queue_completes=last["queue_completes"]-before[-1]["queue_completes"])
    return result


def validate_frozen_limits(policy):
    from product_gpu_queue import SCOPE
    names=("maximum_observed_pending_packets_per_scheduler_node","maximum_room_release_pending_packets_per_scheduler_node",
        "maximum_dma_faults_delta","etw_maximum_pending_packets_per_scheduler_node",
        "etw_maximum_room_release_pending_packets_per_scheduler_node","maximum_room_release_contexts","maximum_room_release_devices",
        "etw_maximum_bytes","minimum_free_evidence_disk_bytes","evidence_disk_reserve_bytes")
    if (policy.get("schema")!=1 or policy.get("status")!="FROZEN_B14_TEST" or policy.get("diagnostic_only") is not False
            or policy.get("scope")!=SCOPE or policy.get("etw_scope")!="process_owned_device_context_scheduler_packet_lifecycle"
            or any(not number(policy.get(k)) for k in names) or policy["etw_maximum_bytes"]<=0
            or policy["minimum_free_evidence_disk_bytes"]<policy["etw_maximum_bytes"]+34359738368+policy["evidence_disk_reserve_bytes"]
            or policy["maximum_dma_faults_delta"]!=0 or policy["maximum_room_release_contexts"]!=0 or policy["maximum_room_release_devices"]!=0):
        raise ValueError("gpu_queue_frozen_test_limits_invalid")


def review_hybrid_cycle(proof, sampled, actions, policy, frozen=False):
    """Complement valid active snapshots with actual continuous release events."""
    keys=("maximum_observed_pending_packets_per_scheduler_node", "maximum_room_release_pending_packets_per_scheduler_node",
          "maximum_dma_faults_delta", "etw_maximum_pending_packets_per_scheduler_node",
          "etw_maximum_room_release_pending_packets_per_scheduler_node")
    if frozen:
        validate_frozen_limits(policy)
    elif (policy.get("status") != "PROVISIONAL_USER_REQUESTED_DIAGNOSTIC" or policy.get("diagnostic_only") is not True
            or policy.get("release_eligible") is not False or policy.get("etw_scope") != "process_owned_device_context_scheduler_packet_lifecycle"
            or any(not number(policy.get(k)) for k in keys)):
        raise ValueError("gpu_etw_hybrid_policy_or_active_samples_invalid")
    if sampled.get("window") != "active_only":raise ValueError("gpu_etw_hybrid_policy_or_active_samples_invalid")
    from analyze_product_gpu_budget import stamp
    def endpoint(action, phase):
        found=[a for a in actions if a.get("action")==action and a.get("phase")==phase]
        if len(found)!=1: raise ValueError("gpu_etw_cycle_endpoint_missing_or_duplicate")
        return int(stamp(found[0])*10000)+116444736000000000
    start,end=endpoint("join","uia_observed"),endpoint("leave","requested")
    settled_start,settled_end=endpoint("leave","uia_observed")+50000000,endpoint("export","requested")
    active=interval_observation(proof,start,end)
    release=release_observation(proof,settled_start,settled_end)
    if not active["queue_submits"] or not active["queue_completes"]:
        raise ValueError("gpu_etw_cycle_actual_packet_progress_missing")
    sample_peak=max(n["maximum_observed_submitted_minus_completed"] for a in sampled["adapters"].values() for n in a.values())
    faults=max(sum(p["faulted_delta"] for p in n["dma_packets"].values()) for a in sampled["adapters"].values() for n in a.values())
    checks=dict(sampled_pending=sample_peak<=policy[keys[0]], dma_faults=faults<=policy[keys[2]],
        continuous_active_pending=active["maximum_pending_per_node"]<=policy[keys[3]],
        continuous_release_pending=release["maximum_pending_per_node"]<=policy[keys[4]])
    if frozen:
        checks.update(global_continuous_pending=proof["maximum_pending_per_node"]<=policy[keys[3]],
            release_contexts=release["maximum_contexts"]<=policy["maximum_room_release_contexts"],
            release_devices=release["maximum_devices"]<=policy["maximum_room_release_devices"])
    return dict(passed=all(checks.values()),checks=checks,active_snapshots=sampled,continuous_active=active,continuous_release=release,
        maximum_dma_faults_delta_per_node=faults,policy=policy,diagnostic_only=not frozen,
        gpu_measurement_complete=frozen and all(checks.values()),release_eligible=False,qualification_credit=0)
