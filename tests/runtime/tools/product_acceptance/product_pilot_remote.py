"""Bounded PILOT load + independent SFU/decoded-media evidence.

Run in an isolated Python environment with livekit, livekit-api and PyYAML.
Credentials are read on the server, never written to evidence or argv.
This intentionally does not produce a formal eight-hour acceptance verdict.
"""
from __future__ import annotations

import argparse
import asyncio
import gc
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import signal
import sys
import time
from product_pilot_context import validate_context
from product_pilot_video_counter import create_video_counter
from product_pilot_load import LoadProcess, cgroup_fingerprint
from product_pilot_scheduler import (validate_receiver_priority, process_scheduler, validate_no_realtime,
    realtime_restriction_state, load_scheduler_policy, scheduler_policy_readback)


async def run(args):
    requested = validate_receiver_priority(getattr(args, "diagnostic_receiver_nice", 0), args.timing_diagnostic)
    no_realtime = validate_no_realtime(getattr(args,'diagnostic_no_realtime',False),args.timing_diagnostic,requested)
    realtime_state = realtime_restriction_state() if no_realtime else None
    policy = load_scheduler_policy(getattr(args, 'scheduler_policy', None), args.timing_diagnostic or no_realtime, requested)
    policy_proof = scheduler_policy_readback(policy) if policy is not None else None
    scheduler = process_scheduler() if args.timing_diagnostic else None
    if scheduler is not None and scheduler != dict(nice=requested, policy=0):
        raise RuntimeError("receiver_scheduler_not_applied")
    sys.path.insert(0, str(args.dependencies))
    from livekit import api, rtc
    from google.protobuf.json_format import MessageToDict
    import yaml

    args.output.mkdir(parents=True, exist_ok=False)
    config = yaml.safe_load(args.config.read_text())
    key, secret = next(iter(config["keys"].items()))
    client = api.LiveKitAPI(args.url, key, secret)
    stop = asyncio.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        asyncio.get_running_loop().add_signal_handler(sig, stop.set)
    sequence = 0
    context = None
    stream = (args.output / "remote.jsonl").open("x", buffering=1)

    def emit(event, **values):
        nonlocal sequence
        sequence += 1
        stream.write(json.dumps(dict(schema=1, run_id=args.run_id,
            sequence=sequence, utc=datetime.now(timezone.utc).isoformat(),
            monotonic_s=time.monotonic(), collector="remote", event=event,
            observer_context=context,
            **values), separators=(",", ":")) + "\n")

    def token(identity):
        return api.AccessToken(key, secret).with_identity(identity).with_grants(
            api.VideoGrants(room_join=True, room=args.room)).to_jwt()

    if scheduler is not None:
        emit("receiver.scheduler", pid=os.getpid(), scheduler=scheduler, diagnostic_only=True)
    if realtime_state is not None:
        emit('receiver.realtime_restriction',state=realtime_state,diagnostic_only=True,release_eligible=False)
    if policy_proof is not None:
        emit('receiver.scheduler_policy', proof=policy_proof)

    tasks, rooms, tracks = [], [], {}
    if args.timing_diagnostic:
        from product_pilot_timing import install
        tasks.append(install(args.output, args.run_id))
    stream_tasks = {}
    prefix = "pilot-" + args.run_id[:8]
    receiver = rtc.Room()
    rooms.append(receiver)

    def product(participant):
        return not participant.identity.startswith(prefix)

    def test_cycle(participant):
        # The product submits this synthetic name through the actual join UI.
        # Never persist an arbitrary participant name.
        import re
        match = re.fullmatch("U-" + args.run_id[:8] + r"-([0-9]{4})", participant.name)
        return int(match[1]) if match else None

    def subscribe(publication, participant):
        if product(participant):
            publication.set_subscribed(True)
            emit("receiver.subscribe_requested", participant=participant.identity,
                 sid=publication.sid, source=int(publication.source))

    @receiver.on("participant_connected")
    def connected(participant):
        if product(participant):
            emit("receiver.participant_joined", participant=participant.identity,
                 test_cycle=test_cycle(participant))
            for publication in participant.track_publications.values():
                subscribe(publication, participant)

    @receiver.on("participant_disconnected")
    def disconnected(participant):
        if product(participant):
            emit("receiver.participant_left", participant=participant.identity)

    receiver.on("track_published", subscribe)

    async def consume(track, publication, participant):
        audio = track.kind == rtc.TrackKind.KIND_AUDIO
        media = (rtc.AudioStream(track, capacity=200, sample_rate=48000,
                                num_channels=1, frame_size_ms=20) if audio
                 else create_video_counter(track, capacity=4))
        state = dict(track=track, sid=publication.sid, participant=participant.identity,
                     source=int(publication.source), kind="audio" if audio else "video",
                     frames=0, samples=0, max_gap_ms=0.0, last=None, first=None,
                     active=True, window_frames=0, window_max_gap_ms=0.0)
        tracks[publication.sid] = state
        if not audio:
            state["video_counter"] = dict(scope="native decoded-frame events; no Python pixel copy",
                frames_observed=0, buffers_released=0, width=None, height=None, format=None)
        emit("receiver.track_subscribed", sid=publication.sid,
             participant=participant.identity, source=state["source"], kind=state["kind"])
        try:
            async for frame in media:
                now = time.monotonic()
                if state["last"] is not None:
                    gap = 1000 * (now - state["last"])
                    state["max_gap_ms"] = max(state["max_gap_ms"], gap)
                    state["window_max_gap_ms"] = max(state["window_max_gap_ms"], gap)
                else:
                    state["first"] = now
                state["last"] = now
                state["frames"] += 1
                state["window_frames"] += 1
                if audio:
                    state["samples"] += frame.frame.samples_per_channel
                else:
                    state["video_counter"] = dict(scope="native decoded-frame events; no Python pixel copy",
                        frames_observed=media.frames_observed, buffers_released=media.buffers_released,
                        width=frame.width, height=frame.height, format=frame.format)
                if stop.is_set():
                    break
        except asyncio.CancelledError:
            pass
        finally:
            await media.aclose()
            if not audio:
                state["video_counter"].update(frames_observed=media.frames_observed,
                    buffers_released=media.buffers_released)
                if media.frames_observed != media.buffers_released:
                    raise ValueError("decoded_video_buffer_release_mismatch")
            state["active"] = False
            emit("receiver.stream_closed", **{k: v for k, v in state.items()
                 if k != "track"})

    @receiver.on("track_subscribed")
    def subscribed(track, publication, participant):
        previous = stream_tasks.get(publication.sid)
        if previous is not None:
            previous.cancel()
        task = asyncio.create_task(consume(track, publication, participant))
        stream_tasks[publication.sid] = task
        tasks.append(task)
        def completed(done):
            if stream_tasks.get(publication.sid) is done:
                stream_tasks.pop(publication.sid, None)
        task.add_done_callback(completed)

    @receiver.on("track_unsubscribed")
    def unsubscribed(track, publication, participant):
        if publication.sid in tracks:
            tracks[publication.sid]["active"] = False
        task = stream_tasks.get(publication.sid)
        if task is not None:
            task.cancel()
        emit("receiver.track_unsubscribed", sid=publication.sid,
             participant=participant.identity, source=int(publication.source))

    @receiver.on("track_unpublished")
    def unpublished(publication, participant):
        if product(participant):
            emit("receiver.track_unpublished", sid=publication.sid,
                 participant=participant.identity, source=int(publication.source))

    def network():
        counters = {}
        for line in Path("/proc/net/dev").read_text().splitlines()[2:]:
            name, values = line.split(":")
            name = name.strip()
            if name.startswith(("eth", "ens", "enp")):
                fields = values.split()
                counters[name] = {"rx_bytes": int(fields[0]), "tx_bytes": int(fields[8])}
        return counters

    previous_network, previous_time, previous_cpu = network(), time.monotonic(), None

    async def sample():
        nonlocal previous_network, previous_time, previous_cpu, context
        while not stop.is_set():
            began = time.monotonic()
            context_path = args.output / "context.json"
            if context_path.exists():
                updated = validate_context(json.loads(context_path.read_text()), args.run_id)
                if updated != context:
                    context = updated
                    emit("context.accepted")
                    ack = args.output / "context-ack.tmp"
                    ack.write_text(json.dumps(dict(context=context, sequence=sequence)))
                    ack.replace(args.output / "context-ack.json")
            try:
                listing = await asyncio.wait_for(client.room.list_participants(
                    api.ListParticipantsRequest(room=args.room)), 5)
                emit("sfu.snapshot", participants=[dict(identity=p.identity, sid=p.sid,
                    tracks=[dict(sid=t.sid, source=int(t.source), kind=int(t.type),
                                 muted=t.muted) for t in p.tracks])
                    for p in listing.participants])
            except Exception as error:
                emit("collector.error", stage="sfu", error_type=type(error).__name__)
            # Track-level audio stats may be empty in this SDK. Collect the
            # receiver's public PeerConnection stats independently, without
            # assigning a stream to a track SID by guessing its identifier.
            try:
                connection = await asyncio.wait_for(receiver.get_rtc_stats(), 5)
                # Single-PeerConnection rooms carry received RTP on the
                # publisher transport. Inspect both public stats collections;
                # media direction comes from inbound_rtp, not the PC name.
                inbound = []
                codecs = []
                for side in ("publisher_stats", "subscriber_stats"):
                    for stat in getattr(connection, side):
                        if stat.HasField("inbound_rtp"):
                            values = MessageToDict(stat.inbound_rtp, preserving_proto_field_name=True)
                            inbound.append(dict(values, transport_stats_collection=side))
                        if stat.HasField("codec"):
                            codec = MessageToDict(stat.codec, preserving_proto_field_name=True)
                            values = codec.get("codec", {})
                            codecs.append({"rtc": codec.get("rtc", {}), "codec":
                                {k: values[k] for k in ("payload_type", "mime_type", "clock_rate", "channels")
                                 if k in values}, "transport_stats_collection": side})
                emit("receiver.connection_sample", inbound=inbound, codecs=codecs,
                     active_track_sids=[sid for sid, state in tracks.items() if state["active"]])
            except Exception as error:
                emit("collector.error", stage="receiver_connection_stats", error_type=type(error).__name__)
            for sid, state in list(tracks.items()):
                if not state["active"]:
                    continue
                try:
                    stats = await asyncio.wait_for(state["track"].get_stats(), 5)
                    # Whitelist RTP data; omit candidates, addresses and credentials.
                    inbound = [MessageToDict(s.inbound_rtp, preserving_proto_field_name=True)
                               for s in stats if s.HasField("inbound_rtp")]
                    emit("receiver.sample", **{k: v for k, v in state.items()
                         if k != "track"}, stats=inbound,
                         silence_ms=None if state["last"] is None else
                         1000 * (time.monotonic() - state["last"]))
                    state["window_frames"] = 0
                    state["window_max_gap_ms"] = 0.0
                except Exception as error:
                    emit("collector.error", stage="rtp", sid=sid,
                         error_type=type(error).__name__)
            now, current = time.monotonic(), network()
            elapsed = now - previous_time
            bandwidth = {name: {direction + "_bps":
                8 * (value[direction + "_bytes"] - previous_network[name][direction + "_bytes"]) / elapsed
                for direction in ("rx", "tx")} for name, value in current.items()
                if name in previous_network}
            cpu = list(map(int, Path("/proc/stat").read_text().splitlines()[0].split()[1:]))
            cpu_pct = None
            if previous_cpu:
                total = sum(cpu[:8]) - sum(previous_cpu[:8])
                idle = cpu[3] + cpu[4] - previous_cpu[3] - previous_cpu[4]
                cpu_pct = 100 * (total - idle) / max(1, total)
            memory = {line.split(":")[0]: int(line.split()[1]) * 1024
                      for line in Path("/proc/meminfo").read_text().splitlines()
                      if line.startswith(("MemTotal:", "MemAvailable:", "SwapFree:"))}
            emit("server.resource", network=bandwidth, cpu_pct=cpu_pct, memory=memory)
            previous_network, previous_time, previous_cpu = current, now, cpu
            if (args.output / "stop").exists():
                stop.set()
            await asyncio.sleep(max(.05, 1 - (time.monotonic() - began)))

    async def task_failure():
        while True:
            for task in tasks:
                if task.done() and not task.cancelled() and task.exception() is not None:
                    raise task.exception()
            await asyncio.sleep(.2)

    load = LoadProcess(args)
    waiters = []
    exit_status = "FAILED"
    try:
        await receiver.connect(args.url, token(prefix + "-receiver"),
                               rtc.RoomOptions(auto_subscribe=False))
        for participant in receiver.remote_participants.values():
            connected(participant)
        tasks.append(asyncio.create_task(sample()))
        ready = await load.start()
        if args.timing_diagnostic:
            from product_pilot_audio_reference import AudioReference
            reference = AudioReference(rtc, args.url, token(prefix + '-audio-reference'),
                                       args.run_id, emit, stop)
            tasks.append(asyncio.create_task(reference.run()))
        emit("load.ready", publishers=10, video_width=160, video_height=90,
             video_fps=5, video_bps_each=40000, video_codec="VP8", simulcast=False,
             audio_bps=24000, duration_limit_s=args.seconds, sdk=rtc.__version__,
             publisher_process=ready,
             receiver_pid=__import__("os").getpid(), receiver_cgroup_sha256=cgroup_fingerprint(),
             receiver_video_observer="native decoded-frame counter; pixel quality unmeasured")
        (args.output / "ready.json").write_text(json.dumps({"run_id": args.run_id,
              "pid": __import__("os").getpid(), "state": "READY"}))
        waiters = [asyncio.create_task(stop.wait()), asyncio.create_task(load.watch()),
                   asyncio.create_task(task_failure())]
        done, _ = await asyncio.wait(waiters, timeout=args.seconds,
                                     return_when=asyncio.FIRST_COMPLETED)
        for task in done:
            task.result()
        exit_status = "COMPLETE"
    except Exception as error:
        emit("collector.error", stage="main", error_type=type(error).__name__)
        raise
    finally:
        stop.set()
        for waiter in waiters:
            waiter.cancel()
        await asyncio.gather(*waiters, return_exceptions=True)
        try:
            cleanup = await load.close()
            emit("load.stopped", **cleanup)
            if cleanup["status"] != "COMPLETE":
                emit("collector.error", stage="load_cleanup", error_type="LoadCleanupFailed")
                exit_status = "FAILED"
        except Exception as error:
            emit("collector.error", stage="load_cleanup", error_type=type(error).__name__)
            exit_status = "FAILED"
        for task in tasks:
            task.cancel()
        results = await asyncio.gather(*tasks, return_exceptions=True)
        for result in results:
            if isinstance(result, Exception) and not isinstance(result, asyncio.CancelledError):
                emit("collector.error", stage="task", error_type=type(result).__name__)
                exit_status = "FAILED"
        for room in reversed(rooms):
            await room.disconnect()
        await client.aclose()
        emit("collector.stopped", status=exit_status)
        stream.close()
    if exit_status != "COMPLETE":
        raise RuntimeError("remote_collector_failed")


async def run_cli(args):
    try:
        await run(args)
    finally:
        # Room listeners can retain the disconnected Room through a closure
        # cycle. Release those handles while the SDK callback loop is alive,
        # before asyncio.run closes it and Python begins interpreter teardown.
        gc.collect()
        await asyncio.sleep(0)
        gc.collect()
        await asyncio.sleep(0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--dependencies", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--room", required=True)
    parser.add_argument("--url", default="ws://127.0.0.1:17880")
    parser.add_argument("--seconds", type=int, default=600)
    parser.add_argument("--formal", action="store_true")
    parser.add_argument("--diagnostic", action="store_true")
    parser.add_argument("--timing-diagnostic", action="store_true")
    parser.add_argument("--diagnostic-receiver-nice", type=int, default=0)
    parser.add_argument('--diagnostic-no-realtime',action='store_true')
    parser.add_argument('--scheduler-policy', type=Path)
    args = parser.parse_args()
    if args.timing_diagnostic and (not args.diagnostic or args.formal):
        parser.error("timing witness is diagnostic-only and never formal")
    validate_receiver_priority(args.diagnostic_receiver_nice, args.timing_diagnostic)
    if args.formal and not 28800 <= args.seconds <= 30000:
        parser.error("formal observer must be bounded to 28800..30000 seconds")
    if args.diagnostic and (args.formal or not 60 <= args.seconds <= 2820):
        parser.error("diagnostic observer must be non-formal and bounded to 60..2820 seconds")
    if not args.formal and not args.diagnostic and not 60 <= args.seconds <= 900:
        parser.error("short PILOT must be bounded to 60..900 seconds")
    asyncio.run(run_cli(args))
