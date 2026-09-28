import ast
import asyncio
from pathlib import Path
from types import SimpleNamespace as NS
import time
import unittest


class StreamLifetime(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.streams=[]
        self.events=[]
        streams=self.streams
        class Stream:
            def __init__(self,*args,**kwargs):
                self.queue=asyncio.Queue(); self.closed=False; streams.append(self)
            def __aiter__(self):return self
            async def __anext__(self):return await self.queue.get()
            async def aclose(self):self.closed=True
        path=Path(__file__).with_name('product_pilot_remote.py')
        tree=ast.parse(path.read_text(encoding='utf-8-sig'))
        run=next(n for n in tree.body if isinstance(n,ast.AsyncFunctionDef) and n.name=='run')
        definitions=[n for n in run.body if isinstance(n,(ast.FunctionDef,ast.AsyncFunctionDef)) and n.name in ('consume','subscribed','unsubscribed')]
        self.ns=dict(asyncio=asyncio,time=time,rtc=NS(TrackKind=NS(KIND_AUDIO=1),AudioStream=Stream,VideoStream=Stream),
            stop=asyncio.Event(),tracks={},tasks=[],stream_tasks={},receiver=NS(on=lambda _:lambda f:f),
            emit=lambda event,**data:self.events.append((event,data)))
        exec(compile(ast.Module(body=definitions,type_ignores=[]),str(path),'exec'),self.ns)
        self.pub=NS(sid='same-sid',source=2);self.peer=NS(identity='peer')

    async def asyncTearDown(self):
        for task in self.ns['tasks']:task.cancel()
        await asyncio.gather(*self.ns['tasks'],return_exceptions=True)

    async def drain(self):
        for _ in range(3):await asyncio.sleep(0)

    async def test_unsubscribe_closes_audio_and_video_and_prevents_late_frames(self):
        for kind in (1,2):
            track=NS(kind=kind)
            self.ns['subscribed'](track,self.pub,self.peer)
            await self.drain()
            media=self.streams[-1]
            await media.queue.put(NS(frame=NS(samples_per_channel=960)))
            await self.drain()
            self.ns['unsubscribed'](track,self.pub,self.peer)
            await self.drain()
            before=self.ns['tracks'][self.pub.sid]['frames']
            await media.queue.put(NS(frame=NS(samples_per_channel=960)))
            await self.drain()
            self.assertTrue(media.closed)
            self.assertFalse(self.ns['tracks'][self.pub.sid]['active'])
            self.assertEqual(self.ns['tracks'][self.pub.sid]['frames'],before)
            self.assertNotIn(self.pub.sid,self.ns['stream_tasks'])

    async def test_replacement_survives_old_task_completion(self):
        track=NS(kind=1)
        self.ns['subscribed'](track,self.pub,self.peer)
        await self.drain()
        old=self.streams[-1]
        self.ns['subscribed'](track,self.pub,self.peer)
        replacement=self.ns['stream_tasks'][self.pub.sid]
        await self.drain()
        self.assertTrue(old.closed)
        self.assertFalse(self.streams[-1].closed)
        self.assertIs(self.ns['stream_tasks'][self.pub.sid],replacement)
        self.assertFalse(replacement.done())

    async def test_unsubscribe_before_task_starts_allocates_no_stream(self):
        track=NS(kind=1)
        self.ns['subscribed'](track,self.pub,self.peer)
        self.ns['unsubscribed'](track,self.pub,self.peer)
        await self.drain()
        self.assertEqual(self.streams,[])
        self.assertFalse(self.ns['stream_tasks'])


if __name__=='__main__':unittest.main()
