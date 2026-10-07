"""Publisher isolation contracts; these tests do not grant runtime acceptance."""
import asyncio
from copy import deepcopy
import json
import math
from pathlib import Path
import struct
import sys
from tempfile import TemporaryDirectory
from types import SimpleNamespace as NS
import unittest
from unittest.mock import AsyncMock, Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/product_acceptance"))
from product_pilot_load import LOAD, LoadProcess, commit, identities, publish, validate_ready, review_lifecycle


class LoadReady(unittest.TestCase):
    def ready(self):
        return dict(schema=1, run_id="a"*32, state="READY", pid=10,
            cgroup_sha256="b"*64, identities=identities("a"*32), load=deepcopy(LOAD), sdk="1.0")

    def test_identity_configuration_and_cgroup_must_match(self):
        value = self.ready()
        self.assertEqual(validate_ready(value, "a"*32, 10, "b"*64), value)
        for key, bad in (("run_id", "c"*32), ("pid", 11), ("schema", True),
                         ("state", "FAILED"), ("cgroup_sha256", "d"*64),
                         ("identities", identities("a"*32)[:-1])):
            with self.subTest(key=key), self.assertRaises(ValueError):
                validate_ready(dict(value, **{key: bad}), "a"*32, 10, "b"*64)
        for key, bad in (("video_fps", 4), ("simulcast", 0), ("audio_tracks", True)):
            changed = deepcopy(value); changed["load"][key] = bad
            with self.subTest(key=key), self.assertRaises(ValueError):
                validate_ready(changed, "a"*32, 10, "b"*64)
        with self.assertRaises(ValueError):
            validate_ready(dict(value, token="secret"), "a"*32, 10, "b"*64)

    def test_diagnostic_publishers_cannot_inherit_receiver_boost(self):
        value=dict(self.ready(),publisher_scheduler=dict(nice=0,policy=0))
        self.assertEqual(validate_ready(value,"a"*32,10,"b"*64),value)
        for scheduler in (dict(nice=-5,policy=0),dict(nice=0,policy=1),dict(nice=False,policy=0)):
            with self.subTest(scheduler=scheduler), self.assertRaises(ValueError):
                validate_ready(dict(value,publisher_scheduler=scheduler),"a"*32,10,"b"*64)

    def test_publisher_capability_proof_is_independently_validated(self):
        state=dict(cap_sys_nice_effective=False,cap_sys_nice_permitted=False,cap_sys_nice_bounding=False,
            cap_sys_nice_inheritable=False,cap_sys_nice_ambient=False,no_new_privs=True,rtprio_limit=[0,0])
        state.update({f'cap_sys_resource_{field}':False for field in ('effective','permitted','bounding','inheritable','ambient')})
        value=dict(self.ready(),publisher_realtime_restriction=state)
        self.assertEqual(validate_ready(value,'a'*32,10,'b'*64),value)
        state['cap_sys_nice_effective']=True
        with self.assertRaises(ValueError):validate_ready(value,'a'*32,10,'b'*64)

    def test_independent_lifecycle_review_rejects_missing_forced_or_cross_process_evidence(self):
        ready = dict(run_id="a"*32, monotonic_s=1, publisher_process=self.ready(),
            receiver_pid=9, receiver_cgroup_sha256="b"*64)
        stopped = dict(run_id="a"*32, monotonic_s=2, status="COMPLETE", forced=False,
            pid=10, exit_code=0, result=dict(schema=1, run_id="a"*32, pid=10,
            status="COMPLETE", capture_tasks_drained=True, rooms_disconnected=10,
            errors=[], video_captures=[1]*10, audio_captures=1))
        self.assertTrue(review_lifecycle([ready], [stopped], "a"*32))
        self.assertFalse(review_lifecycle([], [stopped], "a"*32))
        self.assertFalse(review_lifecycle([ready,ready], [stopped], "a"*32))
        self.assertFalse(review_lifecycle([dict(ready,receiver_pid=10)], [stopped], "a"*32))
        self.assertFalse(review_lifecycle([dict(ready,receiver_cgroup_sha256="c"*64)], [stopped], "a"*32))
        for field,bad in (("forced",True),("exit_code",True),("pid",11),("monotonic_s",0)):
            self.assertFalse(review_lifecycle([ready], [dict(stopped,**{field:bad})], "a"*32))
        changed=deepcopy(stopped); changed["result"]["video_captures"][0]=0
        self.assertFalse(review_lifecycle([ready], [changed], "a"*32))


class IsolationSupervisor(unittest.IsolatedAsyncioTestCase):
    def args(self, root):
        return NS(output=Path(root), dependencies=Path("deps"), config=Path("config.yml"),
            run_id="a"*32, room="000000233", url="ws://127.0.0.1:17880", seconds=900)

    async def test_subprocess_argv_contains_paths_without_native_objects_or_credentials(self):
        with TemporaryDirectory() as root:
            load = LoadProcess(self.args(root))
            child = NS(pid=10, returncode=None)
            async def spawn(*args, **kwargs):
                self.assertEqual(args[0], sys.executable)
                self.assertNotIn("--key", args); self.assertNotIn("--secret", args)
                self.assertNotIn("preexec_fn", kwargs)  # inherit task cgroup
                commit(load.root / "ready.json", LoadReady().ready())
                return child
            with patch("product_pilot_load.asyncio.create_subprocess_exec", side_effect=spawn), \
                 patch("product_pilot_load.cgroup_fingerprint", return_value="b"*64):
                self.assertEqual((await load.start())["pid"], 10)
            load.log.close()

    async def test_timing_run_requests_independent_publisher_scheduler_measurement(self):
        with TemporaryDirectory() as root:
            args=self.args(root);args.timing_diagnostic=True
            load=LoadProcess(args)
            async def spawn(*argv,**kwargs):
                self.assertIn("--scheduler-diagnostic",argv)
                ready=dict(LoadReady().ready(),publisher_scheduler=dict(nice=0,policy=0))
                commit(load.root/"ready.json",ready)
                return NS(pid=10,returncode=None)
            with patch("product_pilot_load.asyncio.create_subprocess_exec",side_effect=spawn), \
                 patch("product_pilot_load.cgroup_fingerprint",return_value="b"*64):
                self.assertEqual((await load.start())["publisher_scheduler"],dict(nice=0,policy=0))
            load.log.close()

    async def test_unexpected_child_exit_propagates_even_with_zero_exit(self):
        load = LoadProcess(self.args("unused"))
        load.process = NS(wait=AsyncMock(return_value=0))
        with self.assertRaisesRegex(RuntimeError, "unexpected_exit_0"):
            await load.watch()

    async def test_cleanup_requires_drained_captures_and_all_ten_rooms(self):
        with TemporaryDirectory() as root:
            load = LoadProcess(self.args(root)); load.root.mkdir()
            load.process = NS(pid=10, returncode=0, wait=AsyncMock(return_value=0))
            result = dict(schema=1, run_id="a"*32, pid=10, status="COMPLETE",
                capture_tasks_drained=True, rooms_disconnected=10, errors=[])
            commit(load.root / "result.json", result)
            self.assertEqual((await load.close())["status"], "COMPLETE")
            for field, bad in (("capture_tasks_drained", False), ("rooms_disconnected", 9),
                               ("run_id", "b"*32), ("errors", [{"error_type": "Error"}])):
                commit(load.root / "result.json", dict(result, **{field: bad}))
                self.assertEqual((await load.close())["status"], "FAILED")

    async def test_forced_cleanup_never_passes(self):
        with TemporaryDirectory() as root:
            load = LoadProcess(self.args(root)); load.root.mkdir()
            load.process = NS(pid=10, returncode=0,
                wait=AsyncMock(side_effect=[asyncio.TimeoutError(), 0]), terminate=Mock())
            commit(load.root / "result.json", dict(schema=1, run_id="a"*32, pid=10,
                status="COMPLETE", capture_tasks_drained=True, rooms_disconnected=10, errors=[]))
            result = await load.close()
            self.assertTrue(result["forced"]); self.assertEqual(result["status"], "FAILED")
            load.process.terminate.assert_called_once()


class Numbers(list):
    def __add__(self, x): return Numbers(v+x for v in self)
    def __truediv__(self, x): return Numbers(v/x for v in self)
    def __mul__(self, x): return Numbers(v*x for v in self)
    def astype(self, dtype): return self
    def tobytes(self): return struct.pack("<"+"h"*len(self), *(int(v) for v in self))


class PublisherLifecycle(unittest.IsolatedAsyncioTestCase):
    async def exercise(self, *, capture_error=False, connect_error=False,
                       disconnect_error=False, wrong_stop=False, drain_mode=None):
        rooms, frames, options, pixels = [], [], [], []
        sources, events = [], []
        inflight, release = asyncio.Event(), asyncio.Event()
        wait_requests = []
        real_wait = asyncio.wait
        class Pixels:
            def __setitem__(self, selection, value): pixels.append((selection, value))
            def tobytes(self): return b"rgba"
        self_outer = self
        class Source:
            def __init__(self, *shape):
                self.shape = shape; self.closed = False; sources.append(self)
            async def aclose(self):
                self.closed = True; events.append("source_close")
            def capture_frame(self, frame):
                self_outer.assertFalse(self.closed, "capture after source close")
                if capture_error: raise RuntimeError("synthetic capture failure")
                frames.append((self.shape, frame))
        class Audio(Source):
            async def capture_frame(self, frame):
                self_outer.assertFalse(self.closed, "capture after source close")
                frames.append((self.shape, frame))
                if drain_mode and sum(shape == (48000, 1) for shape, _ in frames) >= 2:
                    inflight.set(); events.append("capture_started")
                    try:
                        await release.wait(); events.append("capture_completed")
                    except asyncio.CancelledError:
                        events.append("capture_cancelled"); raise
                else:
                    await asyncio.sleep(.02)
            def clear_queue(self):
                self_outer.assertFalse(self.closed)
                events.append("audio_clear_queue")
        class Room:
            def __init__(self):
                self.closed=False; rooms.append(self)
                self.local_participant=NS(publish_track=self.publish_track)
            async def connect(self, url, token, opts):
                if connect_error: raise RuntimeError("synthetic connect failure")
                self.identity=token.identity; await asyncio.sleep(0)
            async def publish_track(self, track, opts):
                options.append(opts); await asyncio.sleep(0)
            async def disconnect(self):
                self.closed=True; events.append("room_disconnect")
                if disconnect_error: raise RuntimeError("synthetic disconnect failure")
        class Token:
            def __init__(self, *args): pass
            def with_identity(self, identity): self.identity=identity; return self
            def with_grants(self, grants): return self
            def to_jwt(self): return self
        def opts(**kw): return NS(**kw, video_encoding=NS(), audio_encoding=NS())
        rtc=NS(Room=Room, RoomOptions=lambda **kw:kw, VideoSource=Source, AudioSource=Audio,
            VideoFrame=lambda *args:args, AudioFrame=lambda *args:args,
            VideoBufferType=NS(RGBA=1), TrackSource=NS(SOURCE_CAMERA=2,SOURCE_MICROPHONE=1),
            VideoCodec=NS(VP8=1), TrackPublishOptions=opts, __version__="fixture",
            LocalVideoTrack=NS(create_video_track=lambda *args:args),
            LocalAudioTrack=NS(create_audio_track=lambda *args:args))
        np=NS(zeros=lambda shape,dtype:Pixels(), uint8=1, int16=2,
            arange=lambda n:Numbers(range(n)), sin=lambda values:Numbers(math.sin(v) for v in values))
        modules={"livekit":NS(api=NS(AccessToken=Token,VideoGrants=lambda **kw:kw), rtc=rtc),
            "numpy":np, "yaml":NS(safe_load=json.loads)}
        with TemporaryDirectory() as root:
            root=Path(root); config=root/"config.yml"
            config.write_text(json.dumps({"keys":{"fixture":"fixture"}}))
            args=NS(output=root, config=config, dependencies=root, run_id="a"*32,
                room="000000233", url="ws://fixture", seconds=10)
            async def stop_when_ready():
                for _ in range(300):
                    if (root/"ready.json").exists():
                        if drain_mode: await inflight.wait()
                        commit(root/"stop.json", dict(run_id="b"*32 if wrong_stop else "a"*32, action="stop"))
                        return
                    await asyncio.sleep(.01)
            async def bounded_wait(tasks, timeout):
                wait_requests.append(timeout)
                self.assertEqual(timeout, 5)
                if drain_mode == "natural": release.set()
                return await real_wait(tasks, timeout=.03 if drain_mode == "timeout" else timeout)
            with patch.dict(sys.modules, modules), \
                 patch("product_pilot_load.cgroup_fingerprint", return_value="b"*64), \
                 patch.object(asyncio.get_running_loop(), "add_signal_handler"), \
                 patch("product_pilot_load.asyncio.wait", side_effect=bounded_wait):
                stopper=asyncio.create_task(stop_when_ready())
                code=await publish(args)
                stopper.cancel(); await asyncio.gather(stopper, return_exceptions=True)
            result=json.loads((root/"result.json").read_text())
            ready=json.loads((root/"ready.json").read_text()) if (root/"ready.json").exists() else None
            before=len(frames); await asyncio.sleep(.21)
            self.assertEqual(len(frames), before)  # no captures after teardown
            self.assertTrue(all(room.closed for room in rooms))
            self.assertTrue(all(source.closed for source in sources))
            if sources:
                self.assertLess(events.index("room_disconnect"), events.index("source_close"))
            if any(source.shape == (48000, 1) for source in sources):
                self.assertLess(events.index("audio_clear_queue"), events.index("room_disconnect"))
            self.last_events, self.last_wait_requests = events, wait_requests
            return code,result,ready,rooms,frames,options,pixels

    async def test_frozen_publish_configuration_signal_and_normal_cleanup(self):
        code,result,ready,rooms,frames,options,pixels=await self.exercise()
        self.assertEqual(code,0); self.assertEqual(result["status"],"COMPLETE")
        self.assertEqual([room.identity for room in rooms],identities("a"*32))
        self.assertEqual(ready["load"],LOAD)
        video=[o for o in options if o.source==2]; audio=[o for o in options if o.source==1]
        self.assertEqual(len(video),10); self.assertEqual(len(audio),1)
        self.assertTrue(all(o.video_encoding.max_bitrate==40000 and
            o.video_encoding.max_framerate==5 and o.video_codec==1 and o.simulcast is False for o in video))
        self.assertEqual(audio[0].audio_encoding.max_bitrate,24000)
        self.assertFalse(audio[0].dtx); self.assertFalse(audio[0].red)
        self.assertTrue(all(shape==(160,90) and frame[:3]==(160,90,1)
            for shape,frame in frames if shape==(160,90)))
        pcm=next(frame for shape,frame in frames if shape==(48000,1))
        self.assertEqual(pcm[1:],(48000,1,960))
        expected=struct.pack("<960h",*(int(math.sin(i/48000*2*math.pi*440)*2000) for i in range(960)))
        self.assertEqual(pcm[0],expected)
        self.assertEqual([v for selection,v in pixels[:5]],[255,0,40,60,180])
        self.assertEqual(result["rooms_disconnected"],10)
        self.assertTrue(result["capture_tasks_drained"])

    async def test_capture_failure_propagates_and_cleans_up(self):
        code,result,ready,*_=await self.exercise(capture_error=True)
        self.assertEqual(code,1); self.assertEqual(result["status"],"FAILED")
        self.assertIsNone(ready)

    async def test_partial_startup_failure_cleans_up(self):
        code,result,ready,*_=await self.exercise(connect_error=True)
        self.assertEqual(code,1); self.assertEqual(result["rooms_disconnected"],1)
        self.assertIsNone(ready)

    async def test_disconnect_failure_prevents_complete(self):
        code,result,*_=await self.exercise(disconnect_error=True)
        self.assertEqual(code,1); self.assertEqual(result["rooms_disconnected"],0)

    async def test_wrong_stop_identity_fails(self):
        code,result,*_=await self.exercise(wrong_stop=True)
        self.assertEqual(code,1); self.assertEqual(result["status"],"FAILED")




if __name__ == "__main__": unittest.main()
