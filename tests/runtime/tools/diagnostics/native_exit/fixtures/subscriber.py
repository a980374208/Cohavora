"""Diagnostic subscriber; no product UI and no media-quality acceptance claim."""
import argparse
import asyncio
from datetime import datetime, timezone
import gc
import json
import os
from pathlib import Path
import sys
import time
import weakref


def atomic(path, value):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, separators=(",", ":")) + "\n")
    temporary.replace(path)


async def run(args):
    sys.path.insert(0, str(args.dependencies))
    from livekit import api, rtc
    import yaml
    from product_pilot_video_counter import create_video_counter
    config = yaml.safe_load(args.config.read_text())
    key, secret = next(iter(config["keys"].items()))
    identity = "native-exit-" + args.run_id[:8] + "-subscriber"
    expected = {f"pilot-{args.run_id[:8]}-load-{i:02d}" for i in range(10)}
    token = api.AccessToken(key, secret).with_identity(identity).with_grants(
        api.VideoGrants(room_join=True, room=args.room)).to_jwt()
    args.retirement_refs = {"objects": [], "handles": [], "kinds": {}}
    observed_objects, observed_handles = set(), set()
    def observe_owned(obj, kind):
        if id(obj) not in observed_objects:
            observed_objects.add(id(obj))
            args.retirement_refs["kinds"][kind] = args.retirement_refs["kinds"].get(kind, 0) + 1
            args.retirement_refs["objects"].append(weakref.ref(obj))
        handle = getattr(obj, "_ffi_handle", None)
        if handle is not None and id(handle) not in observed_handles:
            observed_handles.add(id(handle))
            args.retirement_refs["handles"].append(weakref.ref(handle))
    room = rtc.Room()
    observe_owned(room, "room")
    tasks, counts, errors = {}, {}, []
    stop_requested = False
    disconnected = False
    shutting_down = False

    async def consume(track, publication, participant):
        audio = track.kind == rtc.TrackKind.KIND_AUDIO
        media = (rtc.AudioStream(track, capacity=200, sample_rate=48000,
                                num_channels=1, frame_size_ms=20) if audio
                 else create_video_counter(track, capacity=4))
        observe_owned(media, "stream")
        tag = participant.identity + ("/audio" if audio else "/video")
        if tag in counts:
            raise ValueError("duplicate_media_track")
        state = counts[tag] = dict(kind="audio" if audio else "video", frames=0,
                                  closed=False, width=None, height=None)
        try:
            async for frame in media:
                state["frames"] += 1
                if not audio:
                    state["width"], state["height"] = frame.width, frame.height
        except asyncio.CancelledError:
            pass
        finally:
            await media.aclose()
            state["closed"] = True
            if not audio and media.frames_observed != media.buffers_released:
                errors.append({"stage": "video_release", "error_type": "BufferCountMismatch"})
            frame = media = track = publication = participant = None

    @room.on("track_subscribed")
    def subscribed(track, publication, participant):
        if shutting_down:
            return
        observe_owned(track, "track")
        observe_owned(participant, "remote_participant")
        if participant.identity not in expected or publication.sid in tasks:
            errors.append({"stage": "subscribe", "error_type": "UnexpectedTrack"})
            return
        tasks[publication.sid] = asyncio.create_task(consume(track, publication, participant))

    def snapshot():
        video = [v for v in counts.values() if v["kind"] == "video"]
        audio = [v for v in counts.values() if v["kind"] == "audio"]
        return dict(video_tracks=len(video), audio_tracks=len(audio),
                    minimum_video_frames=min((v["frames"] for v in video), default=0),
                    audio_frames=sum(v["frames"] for v in audio),
                    all_video_160x90=all((v["width"], v["height"]) == (160, 90) for v in video),
                    video_track_numeric_geometry=[dict(track_index=i, width=v["width"], height=v["height"], frames=v["frames"])
                                                  for i, v in enumerate(video)],
                    all_streams_closed=all(v["closed"] for v in counts.values()))

    try:
        await asyncio.wait_for(room.connect(args.url, token, rtc.RoomOptions(auto_subscribe=True)), 15)
        observe_owned(room, "room")
        observe_owned(room.local_participant, "local_participant")
        atomic(args.output / "ready.json", dict(run_id=args.run_id, pid=os.getpid(),
                                               status="CONNECTED", monotonic_s=time.monotonic()))
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            for task in tasks.values():
                if task.done() and not task.cancelled():
                    error = task.exception()
                    raise error or RuntimeError("subscriber_stream_ended_early")
            if errors:
                raise RuntimeError("subscriber_observation_failed")
            atomic(args.output / "decoded.json", dict(run_id=args.run_id, pid=os.getpid(),
                monotonic_s=time.monotonic(), **snapshot()))
            path = args.output / "stop.json"
            if path.exists():
                if json.loads(path.read_text()) != dict(run_id=args.run_id, action="stop"):
                    raise ValueError("subscriber_stop_context")
                stop_requested = True
                break
            await asyncio.sleep(.1)
        if not stop_requested:
            raise TimeoutError("subscriber_parent_stop_missing")
    except Exception as error:
        errors.append(dict(stage="run", error_type=type(error).__name__))
    finally:
        shutting_down = True
        room.off("track_subscribed", subscribed)
        for task in tasks.values():
            task.cancel()
        results = await asyncio.gather(*tasks.values(), return_exceptions=True)
        for result in results:
            if isinstance(result, Exception):
                errors.append(dict(stage="stream", error_type=type(result).__name__))
        try:
            await asyncio.wait_for(room.disconnect(), 5)
            disconnected = True
        except Exception as error:
            errors.append(dict(stage="disconnect", error_type=type(error).__name__))
        observed = snapshot()
        complete = (stop_requested and disconnected and not errors and
                    observed["video_tracks"] == 10 and observed["audio_tracks"] == 1 and
                    observed["minimum_video_frames"] >= 50 and observed["audio_frames"] >= 500 and
                    observed["all_streams_closed"])
        atomic(args.output / "result.json", dict(schema=1, run_id=args.run_id, pid=os.getpid(),
            status=("EXIT_DIAGNOSTIC_COMPLETE_GEOMETRY_DIFFERENCE_RECORDED" if not observed["all_video_160x90"] else "EXIT_DIAGNOSTIC_COMPLETE_GEOMETRY_MATCH_OBSERVED") if complete else "FAILED",
            geometry_status="MATCH_OBSERVED" if observed["all_video_160x90"] else "DIFFERENCE_OBSERVED",
            media_quality_verdict="NOT_ASSESSED", cli_zero_means="NATURAL_EXIT_AND_CLEANUP_DIAGNOSTIC_ONLY", stop_requested=stop_requested,
            disconnected=disconnected, errors=errors, **observed,
            finished_monotonic_s=time.monotonic(), finished_utc=datetime.now(timezone.utc).isoformat(),
            qualification_credit=0, formal_credit=0, release_eligible=False))
        tasks.clear()
        results.clear()
        task = result = None
        room = consume = subscribed = None
    return 0 if complete else 1


async def run_cli(args):
    try:
        return await run(args)
    finally:
        gc.collect()
        await asyncio.sleep(0)
        gc.collect()
        await asyncio.sleep(0)
        refs = getattr(args, "retirement_refs", {"objects": [], "handles": []})
        objects_alive = sum(ref() is not None for ref in refs["objects"])
        handles_alive = sum(ref() is not None for ref in refs["handles"])
        undisposed = sum(ref() is not None and not ref().disposed for ref in refs["handles"])
        proof = dict(schema=1, status="PYTHON_WRAPPER_RETIREMENT_OBSERVED", objects_observed=len(refs["objects"]),
            handles_observed=len(refs["handles"]), owner_kinds_observed=refs.get("kinds", {}), objects_alive=objects_alive,
            handles_alive=handles_alive, handles_undisposed=undisposed,
            public_stream_close_required=True, private_dispose_called=False,
            native_destructor_completion="UNKNOWN", native_exit_zero_is_independent_gate=True,
            formal_credit=0, qualification_credit=0)
        atomic(args.output / "retirement.json", proof)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for name in ("dependencies", "config", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    for name in ("run-id", "room", "url"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--seconds", type=int, required=True)
    parser.add_argument("--scheduler-policy", type=Path)
    args = parser.parse_args()
    if args.scheduler_policy:
        from product_pilot_scheduler import load_scheduler_policy, scheduler_policy_readback
        scheduler_policy_readback(load_scheduler_policy(args.scheduler_policy))
    try:
        raise SystemExit(asyncio.run(run_cli(args)))
    except Exception as error:
        print(json.dumps(dict(status="FAILED", error_type=type(error).__name__)))
        raise SystemExit(1)
