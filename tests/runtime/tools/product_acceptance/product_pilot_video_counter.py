"""Count real native decoded frames without copying unused pixels into Python.

This observer uses the pinned SDK's VideoStream ownership/filtering contract.
The same native stream and decoder remain active; every delivered buffer is
released before its metadata enters the same bounded Python queue. Pixel
quality remains outside this counter's claims.
"""
import asyncio
from dataclasses import dataclass


@dataclass(frozen=True)
class DecodedFrameObservation:
    width: int
    height: int
    format: int
    timestamp_us: int
    rotation: int


def counter_type(video_stream_type, ffi_client_type, ffi_handle_type):
    class DecodedVideoCounter(video_stream_type):
        def __init__(self, *args, **kwargs):
            self.frames_observed = 0
            self.buffers_released = 0
            self.last_observation = None
            super().__init__(*args, **kwargs)

        async def _run(self):
            try:
                while True:
                    event = await self._ffi_queue.wait_for(self._is_event)
                    video = event.video_stream_event
                    if video.HasField("frame_received"):
                        received = video.frame_received
                        owned = received.buffer
                        try:
                            info = owned.info
                            observation = DecodedFrameObservation(info.width, info.height,
                                info.type, received.timestamp_us, received.rotation)
                            if not 0 < observation.width <= 16384 or not 0 < observation.height <= 16384:
                                raise ValueError("invalid_decoded_frame_dimensions")
                            self.frames_observed += 1
                            self.last_observation = observation
                        finally:
                            ffi_handle_type(owned.handle.id).dispose()
                            self.buffers_released += 1
                        self._queue.put(observation)
                        # A ready native queue may not suspend; let PCM work run
                        # after releasing this frame's native ownership.
                        await asyncio.sleep(0)
                    elif video.HasField("eos"):
                        break
            finally:
                ffi_client_type.instance.queue.unsubscribe(self._ffi_queue)
    return DecodedVideoCounter


def create_video_counter(track, capacity=4):
    from livekit import rtc
    from livekit.rtc._ffi_client import FfiClient, FfiHandle
    return counter_type(rtc.VideoStream, FfiClient, FfiHandle)(track, capacity=capacity)
