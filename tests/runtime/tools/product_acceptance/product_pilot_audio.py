"""Per-lifecycle outbound microphone evidence; no PCM content is persisted."""
from datetime import datetime
import hashlib


def stamp(row):
    return datetime.fromisoformat(row["utc"].replace("Z", "+00:00")).timestamp()


def review_outbound_audio(run, actions, remote, devices, outcomes, max_gap_ms):
    names = ("receiver_audio_rtp_stats", "outbound_audio_pcm_continuity")
    def both(status, reason):
        return {name: dict(status=status, detail=dict(reason=reason)) for name in names}
    joins = [a for a in actions if a["action"] == "join" and a["phase"] == "uia_observed"]
    leaves = [a for a in actions if a["action"] == "leave" and a["phase"] == "requested"]
    if len(joins) != 1 or len(leaves) != 1:
        return both("FAIL", "audio_lifecycle_window_missing")
    joined, left = joins[0], leaves[0]
    start, end = stamp(joined), stamp(left)
    if end <= start or joined["run_id"] != run or left["cycle_id"] != joined["cycle_id"]:
        return both("FAIL", "audio_lifecycle_identity_mismatch")
    device_valid = devices.get("run_id") == run and devices.get("collector") == \
        "independent_windows_mmdevice" and devices.get("enumeration_hresult") == "00000000"
    deferred = any(o.get("run_id") == run and o.get("cycle") == joined["cycle"]
                   and o.get("device") == "microphone" and o.get("result") == "DEFERRED" for o in outcomes)
    if device_valid and devices.get("active_capture_endpoints") == 0 and \
            devices.get("default_capture_hresult") == "80070490" and deferred:
        return both("DEFERRED", "no_active_windows_capture_endpoint")
    if not device_valid or devices.get("active_capture_endpoints", 0) < 1 or \
            devices.get("default_capture_hresult") != "00000000" or deferred:
        return both("FAIL", "microphone_device_evidence_missing_or_conflicting")
    accepted = {(a["operation_id"], a["phase"]): a for a in actions}
    def bound(row):
        context = row.get("observer_context") or {}
        action = accepted.get((context.get("operation_id"), context.get("phase")))
        return row.get("run_id") == run and action is not None and all(
            context.get(k) == action.get(k) for k in ("run_id", "cycle", "cycle_id", "pid",
                "process_run_id", "anonymous_session_id", "participant_sha256", "action"))
    cycle_rows = [r for r in remote if bound(r)]
    peers = {r["participant"] for r in cycle_rows if r["event"] == "receiver.participant_joined"
             and hashlib.sha256((run+":"+r["participant"]).encode()).hexdigest() == joined["participant_sha256"]}
    if len(peers) != 1:
        return both("FAIL", "audio_authenticated_participant_not_correlated")
    subscriptions = [r for r in cycle_rows if r["event"] == "receiver.track_subscribed"
                     and r.get("participant") in peers and r.get("source") == 2 and r.get("kind") == "audio"]
    track_ids = {r["sid"] for r in subscriptions}
    unpublished = {r["sid"] for r in cycle_rows if r["event"] == "receiver.track_unpublished"
                   and r.get("participant") in peers and r.get("source") == 2 and stamp(r) >= end}
    if len(track_ids) != 1 or not track_ids <= unpublished:
        return both("FAIL", "microphone_publish_release_not_correlated")
    audio = [r for r in cycle_rows if r["event"] == "receiver.sample" and r.get("kind") == "audio"
             and r.get("source") == 2 and r.get("participant") in peers and r.get("sid") in track_ids
             and start <= stamp(r) <= end]
    def covered(samples):
        times = [stamp(r) for r in samples]
        return len(times) >= 2 and times[0] <= start+2 and times[-1] >= end-2 and \
            all(0 < b-a <= 10 for a, b in zip(times, times[1:]))
    gaps = [r["window_max_gap_ms"] for r in audio] + [r["silence_ms"] for r in audio if r["silence_ms"] is not None]
    pcm_ok = covered(audio) and all(r["active"] and r["window_frames"] > 0 and r["silence_ms"] is not None for r in audio) \
        and all(b["frames"] > a["frames"] and b["samples"] > a["samples"] for a,b in zip(audio,audio[1:])) \
        and max(gaps, default=float("inf")) <= max_gap_ms
    connection = [r for r in cycle_rows if r["event"] == "receiver.connection_sample"
                  and start <= stamp(r) <= end and track_ids <= set(r.get("active_track_sids", []))]
    streams, codecs = {}, set()
    for row in connection:
        codec_by_id = {c.get("rtc", {}).get("id"): c.get("codec", {}).get("mime_type") for c in row.get("codecs", [])}
        for stat in row.get("inbound", []):
            stream = stat.get("stream", {})
            if stream.get("kind") != "audio":
                continue
            key = stat.get("rtc", {}).get("id")
            if not key:
                continue
            streams.setdefault(key, []).append((stamp(row), int(stat.get("received", {}).get("packets_received", 0))))
            codec = codec_by_id.get(stream.get("codec_id"))
            if codec:
                codecs.add(codec.lower())
    progressing = {key: values[-1][1]-values[0][1] for key,values in streams.items()
                   if len(values) >= 2 and values[0][0] <= start+2 and values[-1][0] >= end-2
                   and all(b[1] >= a[1] for a,b in zip(values,values[1:])) and values[-1][1] > values[0][1]}
    rtp_ok = covered(connection) and bool(progressing) and "audio/opus" in codecs
    common = dict(cycle=joined["cycle"],cycle_id=joined["cycle_id"],
                  anonymous_session_id=joined["anonymous_session_id"],track_sids=sorted(track_ids),
                  start_utc=joined["utc"],end_utc=left["utc"])
    return {
        names[0]: dict(status="PASS" if rtp_ok else "FAIL", detail=dict(common,
            peer_connection_samples=len(connection),packets_received_delta_by_stream=progressing,codecs=sorted(codecs),
            scope="independent receiver subscribed only to correlated product; no inferred RTP-to-track SID mapping")),
        names[1]: dict(status="PASS" if pcm_ok else "FAIL", detail=dict(common,samples=len(audio),
            max_callback_or_silence_ms=max(gaps,default=None),threshold_ms=max_gap_ms,
            scope="decoded PCM callback timing and sample progress; does not prove audible quality"))}
