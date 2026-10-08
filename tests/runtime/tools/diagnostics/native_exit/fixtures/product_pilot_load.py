"""Fixed PILOT publishers in a separate process, with fail-closed lifecycle IPC.

Native rooms and sources stay in this process. Credentials are read from the
server config; only paths and synthetic test identities cross the boundary.
"""
from __future__ import annotations
import argparse
import asyncio
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import sys
import time
from product_pilot_scheduler import (configure_receiver_priority, process_scheduler, validate_no_realtime,
    realtime_restriction_state, validate_realtime_restriction, load_scheduler_policy,
    scheduler_policy_readback, validate_scheduler_readback, SCHEDULER_POLICY)
from product_pilot_timing import process_cpu_sample


LOAD = dict(publishers=10, video_width=160, video_height=90, video_fps=5,
    video_bps_each=40000, video_codec="VP8", simulcast=False,
    audio_tracks=1, audio_rate=48000, audio_channels=1, audio_samples=960,
    audio_hz=440, audio_amplitude=2000, audio_bps=24000, dtx=False, red=False)


def identities(run_id):
    if not re.fullmatch(r"[a-f0-9]{32}", run_id):
        raise ValueError("invalid_load_run_id")
    return [f"pilot-{run_id[:8]}-load-{i:02d}" for i in range(10)]


def cgroup_fingerprint():
    # exec children inherit the receiver's run-specific net_cls cgroup.
    return hashlib.sha256(Path("/proc/self/cgroup").read_bytes()).hexdigest()


def commit(path, value):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, separators=(",", ":")) + "\n")
    temporary.replace(path)


def validate_ready(value, run_id, pid, cgroup, scheduler_policy=None):
    expected = dict(schema=1, run_id=run_id, state="READY", pid=pid,
        cgroup_sha256=cgroup, identities=identities(run_id), load=LOAD)
    allowed = set(expected) | {"sdk"}
    if "publisher_scheduler" in value:
        allowed.add("publisher_scheduler")
        if json.dumps(value["publisher_scheduler"], sort_keys=True) != json.dumps(dict(nice=0, policy=0), sort_keys=True):
            raise ValueError("load_publisher_scheduler_mismatch")
    if 'publisher_realtime_restriction' in value:
        allowed.add('publisher_realtime_restriction')
        validate_realtime_restriction(value['publisher_realtime_restriction'])
    if 'publisher_scheduler_policy' in value:
        allowed.add('publisher_scheduler_policy')
        # Generic lifecycle review validates shape; plan-bound review checks the hash.
        proof = value['publisher_scheduler_policy']
        validate_scheduler_readback(proof, dict(policy=SCHEDULER_POLICY, sha256=proof['policy_sha256']))
    if scheduler_policy is not None:
        validate_scheduler_readback(value.get('publisher_scheduler_policy'), scheduler_policy)
    if set(value) != allowed:
        raise ValueError("load_ready_schema_mismatch")
    for name, wanted in expected.items():
        # JSON bool must not pass as integer 1, including nested load fields.
        if json.dumps(value[name], sort_keys=True) != json.dumps(wanted, sort_keys=True):
            raise ValueError("load_ready_mismatch_" + name)
    if not isinstance(value["sdk"], str) or not value["sdk"]:
        raise ValueError("load_ready_sdk_missing")
    return value


def review_lifecycle(ready_events, stopped_events, run_id):
    """Independently verify the publisher process in the recorded evidence."""
    try:
        if len(ready_events) != 1 or len(stopped_events) != 1:
            return False
        ready, stopped = ready_events[0], stopped_events[0]
        child = ready["publisher_process"]
        parent = ready["receiver_pid"]
        pid = child["pid"]
        if type(parent) is not int or type(pid) is not int or min(parent, pid) <= 0 or parent == pid:
            return False
        validate_ready(child, run_id, pid, ready["receiver_cgroup_sha256"])
        result = stopped["result"]
        return (ready["run_id"] == stopped["run_id"] == run_id
            and stopped["monotonic_s"] > ready["monotonic_s"]
            and stopped["status"] == result["status"] == "COMPLETE"
            and stopped["forced"] is False and type(stopped["exit_code"]) is int
            and stopped["exit_code"] == 0 and stopped["pid"] == result["pid"] == pid
            and result["schema"] == 1 and result["run_id"] == run_id
            and result["capture_tasks_drained"] is True
            and type(result["rooms_disconnected"]) is int and result["rooms_disconnected"] == 10
            and result["errors"] == [] and len(result["video_captures"]) == 10
            and all(type(n) is int and n > 0 for n in result["video_captures"])
            and type(result["audio_captures"]) is int and result["audio_captures"] > 0)
    except (KeyError, TypeError, ValueError):
        return False


class LoadProcess:
    def __init__(self, args):
        self.args = args
        self.root = args.output / "load-process"
        self.process = None
        self.log = None

    async def start(self):
        self.root.mkdir(exist_ok=False)
        self.log = (self.root / "stderr.log").open("xb")
        command = [sys.executable, str(Path(__file__).resolve()),
            "--dependencies", str(self.args.dependencies), "--config", str(self.args.config),
            "--output", str(self.root), "--run-id", self.args.run_id,
            "--room", self.args.room, "--url", self.args.url,
            "--seconds", str(self.args.seconds + 180)]
        if getattr(self.args, "timing_diagnostic", False):
            command.append("--scheduler-diagnostic")
        no_realtime = validate_no_realtime(getattr(self.args,'diagnostic_no_realtime',False),
                                          getattr(self.args,'timing_diagnostic',False))
        if no_realtime:
            command.append('--diagnostic-no-realtime')
        policy_path = getattr(self.args, 'scheduler_policy', None)
        policy = load_scheduler_policy(policy_path, getattr(self.args, 'timing_diagnostic', False) or no_realtime)
        if policy is not None:
            command.extend(['--scheduler-policy', str(policy_path)])
        self.process = await asyncio.create_subprocess_exec(*command,
            stdin=asyncio.subprocess.DEVNULL, stdout=self.log, stderr=self.log)
        deadline = asyncio.get_running_loop().time() + 90
        path = self.root / "ready.json"
        while asyncio.get_running_loop().time() < deadline:
            if self.process.returncode is not None:
                raise RuntimeError("load_process_exited_before_ready")
            if path.exists():
                ready = validate_ready(json.loads(path.read_text()), self.args.run_id,
                                       self.process.pid, cgroup_fingerprint(), policy)
                if no_realtime:
                    validate_realtime_restriction(ready.get('publisher_realtime_restriction'))
                return ready
            await asyncio.sleep(.1)
        raise TimeoutError("load_process_ready_timeout")

    async def watch(self):
        code = await self.process.wait()
        raise RuntimeError("load_process_unexpected_exit_" + str(code))

    async def close(self):
        forced = False
        try:
            if self.process is None:
                return dict(status="FAILED", reason="load_process_not_started")
            commit(self.root / "stop.json", dict(run_id=self.args.run_id, action="stop"))
            try:
                await asyncio.wait_for(self.process.wait(), 40)
            except asyncio.TimeoutError:
                forced = True
                self.process.terminate()
                try:
                    await asyncio.wait_for(self.process.wait(), 10)
                except asyncio.TimeoutError:
                    self.process.kill()
                    await asyncio.wait_for(self.process.wait(), 10)
            path = self.root / "result.json"
            result = json.loads(path.read_text()) if path.exists() else {}
            complete = (not forced and self.process.returncode == 0
                and result.get("schema") == 1 and result.get("run_id") == self.args.run_id
                and result.get("pid") == self.process.pid and result.get("status") == "COMPLETE"
                and result.get("capture_tasks_drained") is True
                and result.get("rooms_disconnected") == 10 and not result.get("errors"))
            return dict(status="COMPLETE" if complete else "FAILED", pid=self.process.pid,
                exit_code=self.process.returncode, forced=forced, result=result)
        finally:
            if self.log is not None:
                self.log.close()


async def publish(args):
    scheduling = getattr(args, "scheduler_diagnostic", False)
    no_realtime = validate_no_realtime(getattr(args,'diagnostic_no_realtime',False),scheduling)
    realtime_state = realtime_restriction_state() if no_realtime else None
    policy = load_scheduler_policy(getattr(args, 'scheduler_policy', None), scheduling or no_realtime)
    policy_proof = scheduler_policy_readback(policy) if policy is not None else None
    if scheduling:
        # Reset before SDK imports/threads; the receiver's boost must not
        # change the independent fixed publishers' baseline priority.
        configure_receiver_priority(0, True)
    sys.path.insert(0, str(args.dependencies))
    from livekit import api, rtc
    import numpy as np
    import yaml
    peers = identities(args.run_id)
    config = yaml.safe_load(args.config.read_text())
    key, secret = next(iter(config["keys"].items()))
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)
    tasks, rooms, errors = [], [], []
    sources = []
    sources_closed = 0
    audio_queues_cleared = 0
    stop_requested = False
    disconnected = 0
    captures = [0] * 10
    audio_captures = 0
    cadence = (args.output/"cadence.jsonl").open("x", buffering=1) if scheduling else None
    cadence_sequence = 0

    def record_cadence(event):
        nonlocal cadence_sequence
        if cadence is None:return
        cadence_sequence += 1
        cadence.write(json.dumps(dict(event=event, run_id=args.run_id, sequence=cadence_sequence,
            pid=os.getpid(), monotonic_s=time.monotonic(), video_captures=captures,
            audio_captures=audio_captures, scheduler=process_scheduler(),
            process_cpu=process_cpu_sample()), separators=(",", ":"))+"\n")

    async def video_loop(source, index):
        pixels = np.zeros((90, 160, 4), dtype=np.uint8)
        pixels[:, :, 3] = 255
        frame = 0
        while not stop.is_set():
            pixels[:, :, 0] = (index * 21) % 256
            pixels[:, :, 1] = 40
            pixels[:, :, 2] = 60
            pixels[:, (frame * 3) % 140:(frame * 3) % 140 + 20, :3] = 180
            source.capture_frame(rtc.VideoFrame(160, 90, rtc.VideoBufferType.RGBA,
                                               pixels.tobytes()))
            frame += 1
            captures[index] += 1
            await asyncio.sleep(.2)

    async def audio_loop(source):
        nonlocal audio_captures
        samples = 0
        while not stop.is_set():
            t = (np.arange(960) + samples) / 48000
            pcm = (np.sin(t * 2 * math.pi * 440) * 2000).astype(np.int16)
            await source.capture_frame(rtc.AudioFrame(pcm.tobytes(), 48000, 1, 960))
            samples += 960
            audio_captures += 1

    def check_tasks():
        for task in tasks:
            if task.done():
                if task.cancelled():
                    raise RuntimeError("load_capture_cancelled")
                error = task.exception()
                if error is not None:
                    raise error
                raise RuntimeError("load_capture_ended")

    try:
        for index, identity in enumerate(peers):
            room = rtc.Room()
            rooms.append(room)
            token = api.AccessToken(key, secret).with_identity(identity).with_grants(
                api.VideoGrants(room_join=True, room=args.room)).to_jwt()
            await asyncio.wait_for(room.connect(args.url, token,
                rtc.RoomOptions(auto_subscribe=False)), 15)
            source = rtc.VideoSource(160, 90)
            sources.append(("video", source))
            track = rtc.LocalVideoTrack.create_video_track("pilot-low-vp8", source)
            options = rtc.TrackPublishOptions(source=rtc.TrackSource.SOURCE_CAMERA,
                simulcast=False, video_codec=rtc.VideoCodec.VP8)
            options.video_encoding.max_bitrate = 40000
            options.video_encoding.max_framerate = 5
            await asyncio.wait_for(room.local_participant.publish_track(track, options), 15)
            tasks.append(asyncio.create_task(video_loop(source, index)))
            if index == 0:
                audio_source = rtc.AudioSource(48000, 1)
                sources.append(("audio", audio_source))
                audio_track = rtc.LocalAudioTrack.create_audio_track("pilot-tone", audio_source)
                audio_options = rtc.TrackPublishOptions(source=rtc.TrackSource.SOURCE_MICROPHONE,
                                                        dtx=False, red=False)
                audio_options.audio_encoding.max_bitrate = 24000
                await asyncio.wait_for(room.local_participant.publish_track(audio_track, audio_options), 15)
                tasks.append(asyncio.create_task(audio_loop(audio_source)))
            check_tasks()
        # All 11 capture tasks must deliver actual frames before readiness.
        deadline = loop.time() + 10
        while not all(captures) or not audio_captures:
            check_tasks()
            if loop.time() >= deadline:
                raise TimeoutError("load_first_capture_timeout")
            await asyncio.sleep(.05)
        ready = dict(schema=1, run_id=args.run_id,
            pid=os.getpid(), state="READY", cgroup_sha256=cgroup_fingerprint(),
            identities=peers, load=LOAD, sdk=rtc.__version__)
        if scheduling:ready["publisher_scheduler"] = process_scheduler()
        if realtime_state is not None:ready['publisher_realtime_restriction'] = realtime_state
        if policy_proof is not None:ready['publisher_scheduler_policy'] = policy_proof
        commit(args.output / "ready.json", ready)
        record_cadence("publisher.started")
        cadence_time = loop.time()
        deadline = loop.time() + args.seconds
        while not stop.is_set():
            check_tasks()
            if scheduling and loop.time()-cadence_time >= 1:
                record_cadence("publisher.sample")
                cadence_time = loop.time()
            path = args.output / "stop.json"
            if path.exists():
                if json.loads(path.read_text()) != dict(run_id=args.run_id, action="stop"):
                    raise ValueError("load_stop_context_mismatch")
                stop_requested = True
                break
            if loop.time() >= deadline:
                raise TimeoutError("load_parent_stop_missing")
            await asyncio.sleep(.1)
        if not stop_requested:
            raise RuntimeError("load_external_signal")
    except Exception as error:
        errors.append(dict(stage="publish", error_type=type(error).__name__))
    finally:
        stop.set()
        for task in tasks:
            task.cancel()
        results = await asyncio.gather(*tasks, return_exceptions=True)
        for result in results:
            if isinstance(result, Exception):
                errors.append(dict(stage="capture", error_type=type(result).__name__))
        # Capture tasks must be drained before clearing the audio queue.
        # SDK clear_queue releases its waiter; it does not cancel its timer.
        for source_kind, source in sources:
            if source_kind != "audio":
                continue
            try:
                source.clear_queue()
                audio_queues_cleared += 1
            except Exception as error:
                errors.append(dict(stage="audio_clear_queue", error_type=type(error).__name__))
        for room in reversed(rooms):
            try:
                await asyncio.wait_for(room.disconnect(), 3)
                disconnected += 1
            except Exception as error:
                errors.append(dict(stage="disconnect", error_type=type(error).__name__))
        # Register immediately after construction so partial publish failures
        # still release every source. Do not reach into Track private handles.
        for source_kind, source in reversed(sources):
            try:
                await source.aclose()
                sources_closed += 1
            except Exception as error:
                errors.append(dict(stage="source_close", source_kind=source_kind,
                    error_type=type(error).__name__))
        complete = (stop_requested and not errors and disconnected == 10
            and len(sources) == sources_closed == 11 and audio_queues_cleared == 1)
        if cadence is not None:
            try:record_cadence("publisher.stopped")
            finally:cadence.close()
        commit(args.output / "result.json", dict(schema=1, run_id=args.run_id,
            pid=os.getpid(), status="COMPLETE" if complete else "FAILED", errors=errors,
            capture_tasks_drained=all(task.done() for task in tasks),
            rooms_disconnected=disconnected, video_captures=captures,
            sources_created=len(sources), sources_closed=sources_closed,
            audio_queues_cleared=audio_queues_cleared,
            audio_captures=audio_captures, finished_utc=datetime.now(timezone.utc).isoformat()))
    return 0 if complete else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("config", "dependencies", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("run-id", "room", "url"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--seconds", type=int, required=True)
    parser.add_argument("--scheduler-diagnostic", action="store_true")
    parser.add_argument('--diagnostic-no-realtime',action='store_true')
    parser.add_argument('--scheduler-policy', type=Path)
    try:
        raise SystemExit(asyncio.run(publish(parser.parse_args())))
    except Exception as error:
        # Never print a native/API exception message or traceback containing credentials.
        print(json.dumps(dict(status="FAILED", error_type=type(error).__name__)))
        raise SystemExit(1)
