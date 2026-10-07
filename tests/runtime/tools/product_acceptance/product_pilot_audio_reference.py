"""Diagnostic-only reception of the existing fixed tone, on its own connection.

No new publisher or product media policy. This observer cannot contribute to
product RTP/PCM acceptance; it reports an independent simultaneous control.
"""
from __future__ import annotations
import asyncio
import time


def reference_evidence(remote, timing):
    """Keep control handles separate; never borrow their RTP/PCM for product."""
    starts = [r for r in remote if r['event'] == 'diagnostic.audio_reference_started']
    stops = [r for r in remote if r['event'] == 'diagnostic.audio_reference_stopped']
    streams = [r for r in remote if r['event'] == 'diagnostic.audio_reference_stream_started']
    closed = [r for r in remote if r['event'] == 'diagnostic.audio_reference_stream_closed']
    samples = [r for r in remote if r['event'] == 'diagnostic.audio_reference_sample']
    handles = {r['ffi_stream_handle'] for r in streams
               if type(r.get('ffi_stream_handle')) is int and r['ffi_stream_handle'] > 0}
    audio = [r for r in timing if r['event'] == 'audio_delivery' and r['stream'] in handles]
    product = [r for r in remote if r['event'] == 'receiver.sample' and r.get('kind') == 'audio']
    complete = (len(starts) == len(stops) == len(streams) == len(closed) == len(handles) == 1
        and stops[0].get('status') == 'COMPLETE' and stops[0].get('streams_closed') is True
        and starts[0].get('release_eligible') is False and stops[0].get('release_eligible') is False
        and streams[0]['sid'] == closed[0]['sid']
        and streams[0]['ffi_stream_handle'] == closed[0]['ffi_stream_handle']
        and closed[0]['active'] is False and closed[0]['frames'] > 0
        and len(samples) >= 2 and bool(audio) and bool(product)
        and samples[0]['monotonic_s'] <= product[0]['monotonic_s'] + 2
        and samples[-1]['monotonic_s'] >= product[-1]['monotonic_s'] - 2
        and all(0 < b['monotonic_s']-a['monotonic_s'] <= 3
                and b['frames'] >= a['frames'] and b['samples'] >= a['samples']
                for a, b in zip(samples, samples[1:])))
    windows = []
    previous = {}
    for row in product:
        begin = previous.get(row['sid'], row['monotonic_s']-1.3)
        previous[row['sid']] = row['monotonic_s']
        gap = max(row['window_max_gap_ms'], row.get('silence_ms') or 0)
        if gap <= 200:
            continue
        nearby = [r for r in audio if begin-.1 <= r['monotonic_s'] <= row['monotonic_s']+.1]
        reference_samples = [r for r in samples
            if begin-1.1 <= r['monotonic_s'] <= row['monotonic_s']+1.1]
        windows.append(dict(product_sample_utc=row['utc'], product_pcm_gap_ms=gap,
            reference_deliveries=len(nearby),
            reference_ffi_gap_ms=max((r['ffi_gap_ms'] for r in nearby if r.get('ffi_gap_ms') is not None), default=None),
            reference_python_gap_ms=max((r['python_gap_ms'] for r in nearby if r.get('python_gap_ms') is not None), default=None),
            reference_pcm_window_gap_ms=max((max(r['window_max_gap_ms'], r.get('silence_ms') or 0)
                for r in reference_samples), default=None),
            scope='Same process monotonic interval, independent source/connection; correlation does not prove causation'))
    return dict(verdict='REFERENCE_EVIDENCE_COMPLETE' if complete else 'REFERENCE_EVIDENCE_INCOMPLETE',
        diagnostic_only=True, release_eligible=False, ffi_stream_handles=sorted(handles),
        samples=len(samples), frames=closed[0].get('frames') if len(closed) == 1 else None,
        maximum_pcm_gap_ms=max((max(r['window_max_gap_ms'], r.get('silence_ms') or 0) for r in samples), default=None),
        maximum_ffi_gap_ms=max((r['ffi_gap_ms'] for r in audio if r.get('ffi_gap_ms') is not None), default=None),
        maximum_python_gap_ms=max((r['python_gap_ms'] for r in audio if r.get('python_gap_ms') is not None), default=None),
        product_failure_windows=windows,
        outcome='PRODUCT_FAILURE_WINDOWS_OBSERVED' if windows else 'NO_PRODUCT_PCM_FAILURE_OBSERVED',
        scope='Existing synthetic tone in separate PeerConnection, same process; not physical microphone confirmation or acceptance')


class AudioReference:
    def __init__(self, rtc, url, token, run_id, emit, stop, *, ready_timeout=30, sample_seconds=1):
        self.rtc, self.url, self.token, self.emit = rtc, url, token, emit
        self.identity = 'pilot-' + run_id[:8] + '-load-00'
        self.room = rtc.Room()
        self.tasks = {}
        self.states = {}
        self.ready_timeout, self.sample_seconds = ready_timeout, sample_seconds
        self.began = None
        self.stop = stop
        self.failure = None

    def subscribe(self, publication, participant):
        if (participant.identity == self.identity
                and publication.kind == self.rtc.TrackKind.KIND_AUDIO
                and publication.source == self.rtc.TrackSource.SOURCE_MICROPHONE):
            publication.set_subscribed(True)

    async def consume(self, track, publication, participant):
        media = self.rtc.AudioStream(track, capacity=200, sample_rate=48000,
                                    num_channels=1, frame_size_ms=20)
        state = dict(sid=publication.sid, participant=participant.identity, active=True,
                     frames=0, samples=0, first=None, last=None, window_frames=0,
                     window_max_gap_ms=0.0, max_gap_ms=0.0)
        try:
            # Pinned SDK ownership handle, never inferred from RTP stats/SID.
            state['ffi_stream_handle'] = media._ffi_handle.handle
            if type(state['ffi_stream_handle']) is not int or state['ffi_stream_handle'] <= 0:
                raise ValueError('reference_audio_stream_handle_unavailable')
            self.states[publication.sid] = state
            self.emit('diagnostic.audio_reference_stream_started', **state)
            async for frame in media:
                now = time.monotonic()
                if state['last'] is None:
                    state['first'] = now
                else:
                    gap = 1000 * (now - state['last'])
                    state['window_max_gap_ms'] = max(state['window_max_gap_ms'], gap)
                    state['max_gap_ms'] = max(state['max_gap_ms'], gap)
                state['last'] = now
                state['frames'] += 1
                state['window_frames'] += 1
                state['samples'] += frame.frame.samples_per_channel
        finally:
            await media.aclose()
            state['active'] = False
            self.emit('diagnostic.audio_reference_stream_closed', **state)

    def subscribed(self, track, publication, participant):
        if (participant.identity != self.identity or track.kind != self.rtc.TrackKind.KIND_AUDIO
                or publication.source != self.rtc.TrackSource.SOURCE_MICROPHONE):
            self.failure = 'unexpected_reference_subscription'
            raise ValueError('unexpected_reference_subscription')
        previous = self.tasks.get(publication.sid)
        if previous is not None:
            # Do not hide the result of a replaced consumer.
            self.failure = 'duplicate_reference_subscription'
            raise ValueError('duplicate_reference_subscription')
        self.tasks[publication.sid] = asyncio.create_task(self.consume(track, publication, participant))

    def unsubscribed(self, track, publication, participant):
        task = self.tasks.get(publication.sid)
        if task is not None:
            task.cancel()

    async def run(self):
        self.room.on('track_published', self.subscribe)
        self.room.on('track_subscribed', self.subscribed)
        self.room.on('track_unsubscribed', self.unsubscribed)
        self.began = time.monotonic()
        self.emit('diagnostic.audio_reference_started', participant=self.identity,
                  connection_scope='separate_peer_connection_same_receiver_process',
                  load_scope='existing_fixed_publisher_audio_only_no_new_publisher',
                  diagnostic_only=True, release_eligible=False)
        status = 'FAILED'
        try:
            await asyncio.wait_for(self.room.connect(self.url, self.token,
                self.rtc.RoomOptions(auto_subscribe=False)), self.ready_timeout)
            for participant in self.room.remote_participants.values():
                for publication in participant.track_publications.values():
                    self.subscribe(publication, participant)
            while not self.stop.is_set():
                if self.failure:
                    raise ValueError(self.failure)
                for task in self.tasks.values():
                    if task.done():
                        if not task.cancelled() and task.exception() is not None:
                            raise task.exception()
                        raise RuntimeError('reference_audio_consumer_ended')
                if (time.monotonic()-self.began > self.ready_timeout
                        and not any(s['frames'] for s in self.states.values())):
                    raise TimeoutError('reference_audio_not_received')
                for state in self.states.values():
                    self.emit('diagnostic.audio_reference_sample', **state,
                        silence_ms=None if state['last'] is None else 1000*(time.monotonic()-state['last']))
                    state['window_frames'] = 0
                    state['window_max_gap_ms'] = 0.0
                await asyncio.sleep(self.sample_seconds)
            status = 'COMPLETE'
        except asyncio.CancelledError:
            status = 'COMPLETE'
            raise
        finally:
            for task in self.tasks.values():
                task.cancel()
            results = await asyncio.gather(*self.tasks.values(), return_exceptions=True)
            for result in results:
                if isinstance(result, Exception) and not isinstance(result, asyncio.CancelledError):
                    status = 'FAILED'
            await self.room.disconnect()
            self.emit('diagnostic.audio_reference_stopped', status=status,
                      streams=len(self.states), frames=sum(s['frames'] for s in self.states.values()),
                      streams_closed=all(not s['active'] for s in self.states.values()),
                      diagnostic_only=True, release_eligible=False)
            if status != 'COMPLETE':
                raise RuntimeError('reference_audio_observer_failed')
