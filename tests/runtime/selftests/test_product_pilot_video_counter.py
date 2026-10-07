"""Native buffer ownership and real-frame identity for the metadata observer."""
import asyncio
from collections import deque
from pathlib import Path
from types import SimpleNamespace as NS
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools/product_acceptance"))
from product_pilot_video_counter import counter_type


class PoisonPixelInfo:
    width=2560; height=1440; type=5
    @property
    def data_ptr(self):
        raise AssertionError("pixel data must never be read by a frame counter")


def frame(handle,stream=22,info=None):
    received=NS(buffer=NS(handle=NS(id=handle),info=info or PoisonPixelInfo()),timestamp_us=handle*1000,rotation=0)
    video=NS(stream_handle=stream,frame_received=received,HasField=lambda name:name=="frame_received")
    return NS(video_stream_event=video)


def eos():
    return NS(video_stream_event=NS(stream_handle=22,HasField=lambda name:name=="eos"))


class VideoCounterContracts(unittest.IsolatedAsyncioTestCase):
    def fixture(self,capacity=4):
        native=asyncio.Queue();released=[];unsubscribed=[]
        class Queue:
            async def wait_for(self,predicate):
                while True:
                    item=await native.get()
                    if predicate(item):return item
        class Bounded:
            def __init__(self):self.rows=deque()
            def put(self,row):
                if len(self.rows)==capacity:self.rows.popleft()
                self.rows.append(row)
        class Base:
            def __init__(self,*args,**kwargs):
                self._ffi_queue=Queue();self._queue=Bounded()
            def _is_event(self,event):return event.video_stream_event.stream_handle==22
        class Handle:
            def __init__(self,value):self.value=value
            def dispose(self):released.append(self.value)
        client=NS(instance=NS(queue=NS(unsubscribe=lambda q:unsubscribed.append(q))))
        counter=counter_type(Base,client,Handle)()
        return counter,native,released,unsubscribed

    async def test_real_frame_metadata_is_preserved_without_pixel_readback(self):
        counter,native,released,unsubscribed=self.fixture()
        await native.put(frame(101));await native.put(frame(102));await native.put(eos())
        await counter._run()
        self.assertEqual(released,[101,102])
        self.assertEqual(counter.frames_observed,counter.buffers_released)
        self.assertEqual(counter.frames_observed,2)
        self.assertEqual([r.timestamp_us for r in counter._queue.rows],[101000,102000])
        self.assertEqual(counter.last_observation.width,2560)
        self.assertEqual(counter.last_observation.height,1440)
        self.assertEqual(len(unsubscribed),1)

    async def test_other_streams_buffers_are_not_consumed_or_released(self):
        counter,native,released,unsubscribed=self.fixture()
        await native.put(frame(999,stream=33));await native.put(frame(101));await native.put(eos())
        await counter._run()
        self.assertEqual(released,[101]);self.assertEqual(counter.frames_observed,1)

    async def test_queue_backpressure_releases_every_buffer_and_remains_bounded(self):
        counter,native,released,unsubscribed=self.fixture(capacity=2)
        for handle in range(5):await native.put(frame(handle))
        await native.put(eos());await counter._run()
        self.assertEqual(released,list(range(5)))
        self.assertEqual(counter.buffers_released,5)
        self.assertEqual([r.timestamp_us for r in counter._queue.rows],[3000,4000])

    async def test_invalid_metadata_releases_buffer_before_reporting_failure(self):
        counter,native,released,unsubscribed=self.fixture()
        await native.put(frame(101,info=NS(width=0,height=1440,type=5)))
        with self.assertRaisesRegex(ValueError,"invalid_decoded_frame_dimensions"):
            await counter._run()
        self.assertEqual(released,[101]);self.assertEqual(len(unsubscribed),1)
        self.assertFalse(counter._queue.rows)

    async def test_cancel_while_waiting_unsubscribes_without_fabricated_frames(self):
        counter,native,released,unsubscribed=self.fixture()
        task=asyncio.create_task(counter._run());await asyncio.sleep(0)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):await task
        self.assertEqual(len(unsubscribed),1)
        self.assertFalse(released);self.assertEqual(counter.frames_observed,0)

    async def test_video_backlog_does_not_starve_ready_audio_work(self):
        counter,native,released,unsubscribed=self.fixture()
        for handle in range(20):await native.put(frame(handle))
        await native.put(eos())
        seen=[]
        async def audio_work():
            # Audio work is already ready when the video backlog begins draining.
            seen.append(len(released))
        await asyncio.gather(counter._run(),audio_work())
        self.assertLessEqual(seen[0],1)
        self.assertEqual(released,list(range(20)))
        self.assertEqual(counter.frames_observed,counter.buffers_released)
        self.assertEqual(len(unsubscribed),1)

    async def test_cancel_during_backlog_releases_current_buffer_and_unsubscribes(self):
        counter,native,released,unsubscribed=self.fixture()
        for handle in range(20):await native.put(frame(handle))
        task=asyncio.create_task(counter._run())
        await asyncio.sleep(0)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):await task
        self.assertEqual(released,[0])
        self.assertEqual(counter.frames_observed,counter.buffers_released)
        self.assertEqual(len(unsubscribed),1)


if __name__=="__main__":unittest.main()
