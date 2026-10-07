"""Opt-in diagnostic timing, without PCM/pixels or edits to the external SDK."""
from __future__ import annotations
import asyncio
from collections import deque
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import threading
import time


def thread_scheduling_sample(thread_id=None):
    """Linux runnable wait, not blocked time or native audio production time."""
    began = time.monotonic()
    tid = threading.get_native_id() if thread_id is None else thread_id
    result = dict(available=False, thread_id=tid)
    try:
        if Path("/proc/sys/kernel/sched_schedstats").read_text().strip() != "1":
            raise ValueError("scheduler_statistics_not_enabled")
        directory = Path('/proc/thread-self') if thread_id is None else Path(f'/proc/self/task/{tid}')
        counters = [int(x) for x in (directory/'schedstat').read_text().split()]
        fields = (directory/'stat').read_text().rsplit(")", 1)[1].split()
        started = int(fields[19])  # field 22 after the parenthesized comm field
        if len(counters) != 3 or min(*counters, started) < 0:
            raise ValueError("invalid_scheduler_counters")
        result.update(available=True, thread_start_ticks=started,
                      run_ns=counters[0], wait_ns=counters[1], slices=counters[2],
                      nice=int(fields[16]), policy=int(fields[38]))
    except (OSError, ValueError, IndexError) as error:
        result["error_type"] = type(error).__name__
    result["probe_ms"] = 1000 * (time.monotonic() - began)
    return result


def process_cpu_sample(directory=Path('/proc/self')):
    """Task-owned process CPU; no shared service inspection or command lines."""
    began = time.monotonic()
    result = dict(available=False)
    try:
        fields = (directory/'stat').read_text().rsplit(')',1)[1].split()
        hz = os.sysconf('SC_CLK_TCK')
        if hz <= 0:
            raise ValueError('invalid_clock_frequency')
        result.update(available=True, cpu_user_s=int(fields[11])/hz,
            cpu_system_s=int(fields[12])/hz, thread_start_ticks=int(fields[19]),
            threads=int(fields[17]), nice=int(fields[16]), policy=int(fields[38]))
    except (OSError, ValueError, AttributeError, IndexError) as error:
        result['error_type'] = type(error).__name__
    result.update(sampled_at_s=time.monotonic(), probe_ms=1000*(time.monotonic()-began))
    return result


class ProcTimingSampler:
    """Read procfs on a dedicated worker, never inside the media callback lock."""
    def __init__(self, interval=.05, maximum_age=.25, reader=thread_scheduling_sample):
        self.interval, self.maximum_age, self.reader = interval, maximum_age, reader
        self.lock = threading.Lock()
        self.requested, self.cache = set(), {}
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, name='b14-proc-observer', daemon=True)
        self.cpu = dict(available=False, error_type='SampleNotReady')
        self.samples = self.failures = 0
        self.max_read_ms = 0.0

    def get(self):
        began = time.monotonic()
        tid = threading.get_native_id()
        with self.lock:
            if tid not in self.requested and len(self.requested) >= 64:
                sample = dict(available=False,thread_id=tid,error_type='ObserverTidBudgetExceeded')
            else:
                self.requested.add(tid)
                sample = dict(self.cache.get(tid, dict(available=False, thread_id=tid, error_type='SampleNotReady')))
        age = began-sample.get('sampled_at_s',began)
        if sample.get('available') and (age < 0 or age > self.maximum_age):
            sample.update(available=False,error_type='StaleSample')
        sample.update(sample_age_ms=1000*age, sample_read_ms=sample.pop('probe_ms',None),
            probe_ms=1000*(time.monotonic()-began), scope='external_worker_cached_proc_tid',
            maximum_age_ms=self.maximum_age*1000)
        return sample

    def sample_once(self):
        with self.lock:
            requested = tuple(self.requested)
        # File reads and all slow work happen outside both callback/cache locks.
        for tid in requested:
            sample = self.reader(tid)
            sample['sampled_at_s'] = time.monotonic()
            with self.lock:
                self.cache[tid] = sample
                self.samples += 1
                self.failures += int(not sample['available'])
                self.max_read_ms = max(self.max_read_ms,sample['probe_ms'])

    def collect_cpu(self):
        began = time.monotonic()
        result = process_cpu_sample()
        result['thread_cpu'] = {}
        try:
            directories = list(Path('/proc/self/task').iterdir())
            if len(directories)>512:
                raise ValueError('task_thread_observer_budget_exceeded')
            for directory in directories:
                if directory.name.isdigit():
                    result['thread_cpu'][directory.name] = process_cpu_sample(directory)
            result['thread_observer_complete'] = all(s['available'] for s in result['thread_cpu'].values())
        except (OSError,ValueError) as error:
            result.update(thread_observer_complete=False,thread_observer_error=type(error).__name__)
        result['collection_ms'] = 1000*(time.monotonic()-began)
        with self.lock:
            self.cpu = result

    def snapshot(self):
        with self.lock:
            return dict(samples=self.samples,failures=self.failures,
                maximum_background_read_ms=self.max_read_ms,tracked_tids=sorted(self.requested),
                worker_tid=self.thread.native_id,worker_live=self.thread.is_alive(),task_cpu=self.cpu)

    def run(self):
        last_cpu = 0.0
        try:
            while not self.stop.is_set():
                self.sample_once()
                if time.monotonic()-last_cpu >= 1:
                    self.collect_cpu()
                    last_cpu = time.monotonic()
                self.stop.wait(self.interval)
        except Exception as error:
            with self.lock:
                self.failures += 1
                self.cpu = dict(available=False,error_type=type(error).__name__,worker_failed=True)

    def close(self):
        self.stop.set()
        self.thread.join(timeout=5)
        if self.thread.is_alive():
            raise RuntimeError('proc_timing_worker_did_not_stop')


def cpu_pressure_sample():
    result = dict(available=False, scope="whole_server_cpu_some_pressure")
    try:
        line = next(x for x in Path("/proc/pressure/cpu").read_text().splitlines()
                    if x.startswith("some "))
        fields = dict(x.split("=", 1) for x in line.split()[1:])
        result.update(available=True, total_us=int(fields["total"]), avg10=float(fields["avg10"]))
    except (OSError, ValueError, KeyError, StopIteration) as error:
        result["error_type"] = type(error).__name__
    return result


class AudioArrivalWitness:
    """Deduplicate a frame broadcast to multiple SDK queues before timing it."""
    def __init__(self, scheduling_reader=thread_scheduling_sample):
        self.pending = {}
        self.native_last = {}
        self.python_last = {}
        self.scheduling_reader = scheduling_reader
        self.scheduling_last = {}
        self.seen = set()
        self.recent = deque()
        self.lost = 0
        self.lock = threading.Lock()

    def arrival(self, frame, stream, now):
        with self.lock:
            if frame in self.seen:
                return
            if len(self.recent) == 8192:
                self.seen.remove(self.recent.popleft())
            self.seen.add(frame)
            self.recent.append(frame)
            if len(self.pending) >= 4096:
                self.lost += 1
                return
            prior = self.native_last.get(stream)
            self.native_last[stream] = now
            sample = self.scheduling_reader()
            previous = self.scheduling_last.get(stream)
            scheduling = dict(available=sample["available"], thread_id=sample["thread_id"],
                probe_ms=sample["probe_ms"], status="UNAVAILABLE",
                run_delta_ms=None, runnable_wait_delta_ms=None, slices_delta=None,
                nice=sample.get("nice"), policy=sample.get("policy"))
            for field in ('sampled_at_s','sample_age_ms','sample_read_ms','scope','maximum_age_ms'):
                if field in sample:
                    scheduling[field] = sample[field]
            if sample["available"]:
                scheduling["thread_start_ticks"] = sample["thread_start_ticks"]
                scheduling["status"] = "FIRST_SAMPLE"
                if previous is not None and previous["available"]:
                    identity = ("thread_id", "thread_start_ticks")
                    counters = ("run_ns", "wait_ns", "slices")
                    if any(sample[k] != previous[k] for k in identity):
                        scheduling["status"] = "THREAD_CHANGED"
                    elif (sample.get('sampled_at_s') is not None
                            and sample['sampled_at_s'] == previous.get('sampled_at_s')):
                        scheduling['status'] = 'UNCHANGED_SAMPLE'
                    elif any(sample[k] < previous[k] for k in counters):
                        scheduling["status"] = "COUNTER_RESET"
                    else:
                        scheduling.update(status="MEASURED",
                            run_delta_ms=(sample["run_ns"]-previous["run_ns"])/1_000_000,
                            runnable_wait_delta_ms=(sample["wait_ns"]-previous["wait_ns"])/1_000_000,
                            slices_delta=sample["slices"]-previous["slices"])
                        if 'sampled_at_s' in sample and 'sampled_at_s' in previous:
                            scheduling['counter_interval_ms'] = 1000*(sample['sampled_at_s']-previous['sampled_at_s'])
            else:
                scheduling["error_type"] = sample.get("error_type", "Unknown")
            self.scheduling_last[stream] = sample
            self.pending[frame] = (now, stream, None if prior is None else 1000*(now-prior), scheduling)

    def copied(self, frame, now):
        with self.lock:
            arrival = self.pending.pop(frame, None)
            if arrival is None:
                return None
            arrived, stream, native_gap, scheduling = arrival
            previous = self.python_last.get(stream)
            self.python_last[stream] = now
            return dict(stream=stream, ffi_arrival_s=arrived, ffi_gap_ms=native_gap,
                ffi_to_python_ms=1000*(now-arrived),
                python_gap_ms=None if previous is None else 1000*(now-previous),
                ffi_thread_scheduling=scheduling)


def install(output, run_id):
    import resource
    from livekit.rtc import _ffi_client, audio_frame, video_frame
    sampler = ProcTimingSampler()
    witness = AudioArrivalWitness(sampler.get)
    rows = deque(maxlen=8192)
    lost = 0
    formats = set()
    original_put = _ffi_client.FfiQueue.put
    original_audio = audio_frame.AudioFrame._from_owned_info
    original_video = video_frame.VideoFrame._from_owned_info
    original_request = _ffi_client.FfiClient.request
    original_dispose = _ffi_client.FfiHandle.dispose
    event_loop_thread = threading.get_ident()

    def record(kind, **values):
        nonlocal lost
        if len(rows) == rows.maxlen:
            lost += 1
        rows.append(dict(event=kind, monotonic_s=time.monotonic(), **values))

    def put(self, item):
        if item.HasField("audio_stream_event"):
            event = item.audio_stream_event
            if event.HasField("frame_received"):
                witness.arrival(event.frame_received.frame.handle.id, event.stream_handle, time.monotonic())
        return original_put(self, item)

    def audio(info):
        began = time.monotonic()
        arrival = witness.copied(info.handle.id, began)
        result = original_audio(info)
        if arrival is not None:
            record("audio_delivery", **arrival, samples=result.samples_per_channel,
                   copy_ms=1000*(time.monotonic()-began))
        else:
            record("audio_unmatched")
        return result

    def video(info):
        began = time.monotonic()
        result = original_video(info)
        elapsed = 1000*(time.monotonic()-began)
        shape = (info.info.width, info.info.height, info.info.type)
        if shape not in formats:
            formats.add(shape)
            record("video_format", width=shape[0], height=shape[1], format=shape[2],
                   python_buffer_bytes=len(result.data))
        if elapsed >= 10:
            record("video_copy_slow", duration_ms=elapsed, width=shape[0], height=shape[1], format=shape[2])
        return result

    def request(self, req):
        began = time.monotonic()
        result = original_request(self, req)
        elapsed = 1000*(time.monotonic()-began)
        if elapsed >= 10:
            record("ffi_request_slow", operation=req.WhichOneof("message"), duration_ms=elapsed)
        return result

    def dispose(self):
        began = time.monotonic()
        failed = True
        try:
            result = original_dispose(self)
            failed = False
            return result
        finally:
            elapsed = 1000*(time.monotonic()-began)
            if elapsed >= 10:
                record("ffi_handle_dispose_slow", duration_ms=elapsed, failed=failed,
                       event_loop_thread=threading.get_ident() == event_loop_thread)

    _ffi_client.FfiQueue.put = put
    audio_frame.AudioFrame._from_owned_info = staticmethod(audio)
    video_frame.VideoFrame._from_owned_info = staticmethod(video)
    _ffi_client.FfiClient.request = request
    _ffi_client.FfiHandle.dispose = dispose
    stream = (output/"timing.jsonl").open("x", buffering=1)
    sampler.thread.start()
    sequence = 0

    def flush():
        nonlocal sequence
        while rows:
            row = rows.popleft()
            sequence += 1
            stream.write(json.dumps(dict(run_id=run_id, sequence=sequence,
                utc=datetime.now(timezone.utc).isoformat(), **row), separators=(",", ":"))+"\n")

    async def monitor():
        last_flush = time.monotonic()
        record("timing.started", diagnostic_only=True, release_eligible=False,
            scope="first Python FFI queue entry to PCM construction; native audio production before callback is unmeasured",
            scheduling_scope="same stream/TID/start-time externally sampled counters; intervals are sampler timestamps, not exact callback boundaries; blocked/native-ready time unmeasured",
            scheduling_probe="worker_cached_proc_tid_v2",sampler_interval_ms=50,maximum_cached_age_ms=250)
        try:
            while True:
                if not sampler.thread.is_alive():
                    raise RuntimeError('proc_timing_worker_failed')
                expected = time.monotonic()+.01
                await asyncio.sleep(.01)
                now = time.monotonic()
                if now-expected >= .02:
                    record("event_loop_lag", lag_ms=1000*(now-expected))
                if now-last_flush >= 1:
                    usage = resource.getrusage(resource.RUSAGE_SELF)
                    record("timing.sample", lost=lost+witness.lost, pending_audio=len(witness.pending),
                        cpu_user_s=usage.ru_utime, cpu_system_s=usage.ru_stime, max_rss_kib=usage.ru_maxrss,
                        cpu_pressure=cpu_pressure_sample(),proc_sampler=sampler.snapshot())
                    flush()
                    last_flush = now
        finally:
            _ffi_client.FfiQueue.put = original_put
            audio_frame.AudioFrame._from_owned_info = staticmethod(original_audio)
            video_frame.VideoFrame._from_owned_info = staticmethod(original_video)
            _ffi_client.FfiClient.request = original_request
            _ffi_client.FfiHandle.dispose = original_dispose
            await asyncio.to_thread(sampler.close)
            record("timing.stopped", lost=lost+witness.lost, pending_audio=len(witness.pending),
                   proc_sampler=sampler.snapshot(),proc_sampler_closed=not sampler.thread.is_alive())
            flush()
            stream.close()
    return asyncio.create_task(monitor())
