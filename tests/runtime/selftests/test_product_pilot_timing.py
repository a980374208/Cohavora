"""Diagnostic hook transparency and restoration, without an installed native SDK."""
import asyncio
import contextlib
import io
import json
from pathlib import Path
import sys
import time
from tempfile import TemporaryDirectory
from types import SimpleNamespace as NS
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools/product_acceptance"))
from product_pilot_timing import install, AudioArrivalWitness, thread_scheduling_sample, ProcTimingSampler, process_cpu_sample
from analyze_product_timing import analyze, task_cpu_summary, realtime_restriction_review
from product_pilot_scheduler import (SchedulingStatsLease, configure_receiver_priority, validate_receiver_priority,
    validate_no_realtime, realtime_child_command, realtime_restriction_state, validate_realtime_restriction,restrict_realtime_limit)


class TimingHook(unittest.IsolatedAsyncioTestCase):
    async def test_dispose_keeps_return_exception_and_restores_native_method(self):
        calls=[]
        class Handle:
            def dispose(self):
                calls.append(self)
                # Windows Python 3.11 monotonic ticks can be ~15.6 ms. Ensure
                # the synthetic slow call spans the observer's 10 ms boundary.
                time.sleep(.040)
                if getattr(self,"fail",False): raise RuntimeError("synthetic")
                return 17
        class Queue:
            def put(self,item): return item
        class Client:
            def request(self,item): return item
        class Audio:
            @staticmethod
            def _from_owned_info(info):return info
        class Video:
            @staticmethod
            def _from_owned_info(info):return info
        ffi=NS(FfiHandle=Handle,FfiQueue=Queue,FfiClient=Client)
        rtc=NS(_ffi_client=ffi,audio_frame=NS(AudioFrame=Audio),video_frame=NS(VideoFrame=Video))
        methods=(Handle.dispose,Queue.put,Client.request,Audio._from_owned_info,Video._from_owned_info)
        modules={"livekit":NS(rtc=rtc),"livekit.rtc":rtc,
            "resource":NS(RUSAGE_SELF=0,getrusage=lambda _:NS(ru_utime=0,ru_stime=0,ru_maxrss=0))}
        with TemporaryDirectory() as directory, patch.dict(sys.modules,modules):
            task=install(Path(directory),"a"*32)
            await asyncio.sleep(0)
            self.assertEqual(Handle().dispose(),17)
            broken=Handle();broken.fail=True
            with self.assertRaisesRegex(RuntimeError,"synthetic"):broken.dispose()
            self.assertEqual(len(calls),2)
            task.cancel();await asyncio.gather(task,return_exceptions=True)
            self.assertEqual(methods,(Handle.dispose,Queue.put,Client.request,Audio._from_owned_info,Video._from_owned_info))
            rows=[json.loads(s) for s in (Path(directory)/"timing.jsonl").read_text().splitlines()]
            release=[r for r in rows if r["event"]=="ffi_handle_dispose_slow"]
            self.assertEqual([r["failed"] for r in release],[False,True])
            self.assertTrue(all(r["event_loop_thread"] and r["duration_ms"]>=10 for r in release))
            self.assertEqual(rows[-1]["event"],"timing.stopped")
            self.assertEqual(rows[-1]["lost"],0)
            self.assertTrue(rows[-1]['proc_sampler_closed'])
            self.assertNotIn("handle",release[0])


class TimingReview(unittest.TestCase):
    def test_handle_release_is_bound_to_pcm_failure_interval(self):
        run="a"*32
        rows=[dict(event="timing.started",monotonic_s=1),
              dict(event="audio_delivery",monotonic_s=1.1,stream=1,ffi_gap_ms=20,ffi_to_python_ms=220,python_gap_ms=240,copy_ms=1,
                   ffi_thread_scheduling=dict(available=True, thread_id=8, thread_start_ticks=10, status="MEASURED",
                       run_delta_ms=8, runnable_wait_delta_ms=210, slices_delta=3, probe_ms=.1)),
              dict(event="ffi_handle_dispose_slow",monotonic_s=1.2,duration_ms=230,failed=False,event_loop_thread=True),
              dict(event="timing.stopped",monotonic_s=2,lost=0)]
        with TemporaryDirectory() as directory:
            root=Path(directory)
            (root/"plan.json").write_text(json.dumps(dict(run_id=run,diagnostic_only=True,release_eligible=False)))
            (root/"timing.jsonl").write_text("\n".join(json.dumps(dict(r,run_id=run,sequence=i)) for i,r in enumerate(rows,1)))
            (root/"remote.jsonl").write_text(json.dumps(dict(run_id=run,event="receiver.sample",kind="audio",sid="track",monotonic_s=1.4,utc="2026-10-02T00:00:00Z",window_max_gap_ms=240,silence_ms=0,observer_context=dict(cycle=1))))
            with contextlib.redirect_stdout(io.StringIO()):self.assertEqual(analyze(root),0)
            report=json.loads((root/"timing-analysis.json").read_text())
            self.assertEqual(report["ffi_handle_dispose_ms"],230)
            self.assertEqual(report["pcm_fail_intervals"][0]["slow_ffi_handle_releases"],[dict(duration_ms=230,failed=False,event_loop_thread=True)])
            self.assertFalse(report["release_eligible"])
            scheduling=report["pcm_fail_intervals"][0]["ffi_thread_scheduling"]
            self.assertEqual(len(scheduling),1)
            self.assertEqual(scheduling[0]["runnable_wait_delta_ms"],210)


class SchedulingWitness(unittest.TestCase):
    @staticmethod
    def sample(wait=0, run=0, slices=1, tid=8, started=10):
        return dict(available=True,thread_id=tid,thread_start_ticks=started,
                    wait_ns=wait,run_ns=run,slices=slices,probe_ms=.1)

    def test_duplicate_broadcast_does_not_advance_scheduler_interval(self):
        samples=iter([self.sample(),self.sample(wait=210_000_000,run=8_000_000,slices=4)])
        witness=AudioArrivalWitness(lambda:next(samples))
        witness.arrival(1,67,1)
        witness.arrival(1,67,1.001)
        self.assertEqual(witness.copied(1,1.005)["ffi_thread_scheduling"]["status"],"FIRST_SAMPLE")
        witness.arrival(2,67,1.24)
        result=witness.copied(2,1.245)
        self.assertAlmostEqual(result["ffi_gap_ms"],240)
        self.assertAlmostEqual(result["ffi_to_python_ms"],5)
        self.assertEqual(result["ffi_thread_scheduling"]["runnable_wait_delta_ms"],210)
        self.assertEqual(result["ffi_thread_scheduling"]["run_delta_ms"],8)

    def test_tid_reuse_reset_and_unavailable_are_not_zero_wait(self):
        samples=iter([self.sample(wait=100),self.sample(wait=200,started=11),
                      self.sample(wait=50,started=11),
                      dict(available=False,thread_id=8,probe_ms=.1,error_type="OSError")])
        witness=AudioArrivalWitness(lambda:next(samples))
        observed=[]
        for frame in range(4):
            witness.arrival(frame,67,frame+1)
            observed.append(witness.copied(frame,frame+1.01)["ffi_thread_scheduling"])
        self.assertEqual([s["status"] for s in observed],
                         ["FIRST_SAMPLE","THREAD_CHANGED","COUNTER_RESET","UNAVAILABLE"])
        self.assertTrue(all(s["runnable_wait_delta_ms"] is None for s in observed))

    def test_malformed_procfs_keeps_scheduling_unknown(self):
        with patch.object(Path,"read_text",return_value="not counters"):
            sample=thread_scheduling_sample()
        self.assertFalse(sample["available"])
        self.assertNotIn("wait_ns",sample)

    def test_parenthesized_thread_name_does_not_shift_start_time(self):
        stat="8 (audio worker) " + " ".join(["S"]+["0"]*18+["123"]+["0"]*20)
        with patch.object(Path,"read_text",side_effect=["1","10 20 3",stat]):
            sample=thread_scheduling_sample()
        self.assertTrue(sample["available"])
        self.assertEqual(sample["thread_start_ticks"],123)
        self.assertEqual(sample["wait_ns"],20)
        self.assertEqual(sample["nice"],0)
        self.assertEqual(sample["policy"],0)

    def test_disabled_schedstats_are_unavailable_even_if_counters_exist(self):
        with patch.object(Path,"read_text",return_value="0"):
            sample=thread_scheduling_sample()
        self.assertFalse(sample["available"])
        self.assertNotIn("wait_ns",sample)


class AsyncProcSampler(unittest.TestCase):
    def test_callback_reader_performs_no_procfs_reads(self):
        sampler = ProcTimingSampler()
        with patch.object(Path,'read_text',side_effect=AssertionError('callback_file_io')):
            sample = sampler.get()
        self.assertFalse(sample['available'])
        self.assertEqual(sample['error_type'],'SampleNotReady')
        self.assertIn(sample['thread_id'],sampler.requested)

    def test_worker_reads_outside_cache_lock_and_stale_data_stays_unknown(self):
        def reader(tid):
            self.assertFalse(sampler.lock.locked())
            value = SchedulingWitness.sample()
            value['thread_id'] = tid
            return value
        sampler = ProcTimingSampler(reader=reader)
        tid = sampler.get()['thread_id']
        sampler.sample_once()
        self.assertTrue(sampler.get()['available'])
        sampler.cache[tid]['sampled_at_s'] = time.monotonic()-1
        stale = sampler.get()
        self.assertFalse(stale['available'])
        self.assertEqual(stale['error_type'],'StaleSample')
        self.assertEqual(stale['wait_ns'],0)

    def test_repeated_cached_sample_is_not_measured_zero_wait(self):
        samples = iter([dict(SchedulingWitness.sample(),sampled_at_s=1),
            dict(SchedulingWitness.sample(),sampled_at_s=1),
            dict(SchedulingWitness.sample(wait=20_000_000),sampled_at_s=1.05)])
        witness = AudioArrivalWitness(lambda:next(samples))
        result = []
        for frame in range(3):
            witness.arrival(frame,67,frame+.1)
            result.append(witness.copied(frame,frame+.11)['ffi_thread_scheduling'])
        self.assertEqual([r['status'] for r in result],['FIRST_SAMPLE','UNCHANGED_SAMPLE','MEASURED'])
        self.assertIsNone(result[1]['runnable_wait_delta_ms'])
        self.assertAlmostEqual(result[2]['counter_interval_ms'],50)
        self.assertAlmostEqual(result[2]['runnable_wait_delta_ms'],20)

    def test_requested_tid_is_the_procfs_target_not_worker_tid(self):
        stat='8 (audio worker) '+' '.join(['S']+['0']*18+['123']+['0']*20)
        with patch.object(Path,'read_text',side_effect=['1','10 20 3',stat]) as read:
            sample = thread_scheduling_sample(12345)
        self.assertEqual(sample['thread_id'],12345)
        self.assertTrue(sample['available'])
        self.assertEqual(read.call_count,3)

    def test_cpu_ticks_and_parenthesized_name_are_parsed_without_command_line(self):
        fields=['S']+['0']*40
        fields[11],fields[12],fields[17],fields[19] = '100','50','3','123'
        with patch.object(Path,'read_text',return_value='8 (audio ) worker) '+' '.join(fields)), \
                patch('product_pilot_timing.os.sysconf',return_value=100,create=True):
            sample = process_cpu_sample()
        self.assertTrue(sample['available'])
        self.assertEqual((sample['cpu_user_s'],sample['cpu_system_s'],sample['threads']),(1,.5,3))
        self.assertEqual(sample['thread_start_ticks'],123)

    def test_cpu_counter_reset_and_tid_reuse_cannot_fabricate_attribution(self):
        def sample(at, cpu, started=10):
            return dict(available=True,sampled_at_s=at,cpu_user_s=cpu,cpu_system_s=0,
                thread_start_ticks=started,threads=1,nice=0,policy=0)
        a,b = sample(1,1),sample(2,1.5)
        a['thread_cpu']={'8':sample(1,1)}
        b['thread_cpu']={'8':sample(2,2,started=11)}
        proof=task_cpu_summary([a,b])
        self.assertEqual(proof['peak_cpu_percent_of_one_core'],50)
        self.assertEqual(proof['top_observed_thread_cpu_seconds'],[])
        b['cpu_user_s']=.5
        self.assertEqual(task_cpu_summary([a,b])['verdict'],'CPU_OBSERVATION_INCOMPLETE')

    def test_worker_shutdown_is_joined_and_nonblocking_for_callback(self):
        sampler = ProcTimingSampler(interval=.005)
        with patch.object(sampler,'collect_cpu'),patch.object(sampler,'sample_once'):
            sampler.thread.start()
            sampler.close()
        self.assertFalse(sampler.thread.is_alive())


class RealtimeRestriction(unittest.TestCase):
    @staticmethod
    def state():
        state = dict(cap_sys_nice_effective=False,cap_sys_nice_permitted=False,cap_sys_nice_bounding=False,
            cap_sys_nice_inheritable=False,cap_sys_nice_ambient=False,no_new_privs=True,rtprio_limit=[0,0])
        state.update({f'cap_sys_resource_{field}':False for field in ('effective','permitted','bounding','inheritable','ambient')})
        return state

    def test_restriction_cannot_apply_to_ordinary_or_boosted_diagnostic(self):
        self.assertFalse(validate_no_realtime(False,False))
        self.assertTrue(validate_no_realtime(True,True,0))
        for enabled,diagnostic,nice in ((True,False,0),(True,True,-5),(1,True,0)):
            with self.assertRaises(ValueError):validate_no_realtime(enabled,diagnostic,nice)

    def test_child_wrapper_only_removes_nice_capability_and_preserves_argv(self):
        command=['/task/venv/python','/task/remote.py','--room','000000238']
        result=realtime_child_command(command)
        self.assertEqual(result[:6],['/usr/bin/setpriv','--bounding-set=-sys_nice,-sys_resource',
            '--inh-caps=-sys_nice,-sys_resource','--ambient-caps=-sys_nice,-sys_resource','--no-new-privs','--'])
        self.assertEqual(result[6:],command)
        self.assertEqual(command[0],'/task/venv/python')

    def test_missing_capability_proof_or_bool_limit_cannot_pass(self):
        state=self.state()
        self.assertEqual(validate_realtime_restriction(state),state)
        for name,bad in (('cap_sys_nice_bounding',True),('cap_sys_nice_effective',True),
                ('no_new_privs',False),('rtprio_limit',[False,False])):
            changed=dict(state,**{name:bad})
            with self.assertRaises(ValueError):validate_realtime_restriction(changed)
        with self.assertRaises(ValueError):validate_realtime_restriction(None)

    def test_readback_checks_all_capability_sets_and_rlimit(self):
        text='\n'.join(f'{name}:0000000000000000' for name in ('CapEff','CapPrm','CapBnd','CapInh','CapAmb'))+'\nNoNewPrivs:1\n'
        resource=NS(RLIMIT_RTPRIO=14,getrlimit=lambda _: (0,0))
        with patch.dict(sys.modules,{'resource':resource}),patch.object(Path,'read_text',return_value=text):
            self.assertEqual(realtime_restriction_state(),self.state())
        with patch.dict(sys.modules,{'resource':resource}),patch.object(Path,'read_text',return_value=text.replace('CapBnd:0000000000000000','CapBnd:0000000000800000')):
            with self.assertRaises(ValueError):realtime_restriction_state()

    def test_limit_mutation_is_scoped_to_calling_child_and_not_global_sysctl(self):
        setrlimit=unittest.mock.Mock()
        with patch.dict(sys.modules,{'resource':NS(RLIMIT_RTPRIO=14,setrlimit=setrlimit)}), \
                patch.object(Path,'write_text',side_effect=AssertionError('global_write')):
            restrict_realtime_limit()
        setrlimit.assert_called_once_with(14,(0,0))

    def test_realtime_thread_or_missing_publisher_proof_cannot_pass_review(self):
        remote=[dict(event='receiver.realtime_restriction',state=self.state(),release_eligible=False),
            dict(event='load.ready',publisher_process=dict(publisher_realtime_restriction=self.state()))]
        cpu=[dict(proc_sampler=dict(task_cpu=dict(thread_cpu={'8':dict(available=True,policy=0)})))]
        self.assertEqual(realtime_restriction_review(remote,cpu)['verdict'],'REALTIME_RESTRICTION_EVIDENCE_COMPLETE')
        cpu[0]['proc_sampler']['task_cpu']['thread_cpu']['8']['policy']=1
        self.assertEqual(realtime_restriction_review(remote,cpu)['verdict'],'REALTIME_RESTRICTION_EVIDENCE_INCOMPLETE')
        self.assertEqual(realtime_restriction_review(remote[:1],[])['verdict'],'REALTIME_RESTRICTION_EVIDENCE_INCOMPLETE')


class SchedulerLease(unittest.TestCase):
    def test_success_and_child_exception_restore_original_setting(self):
        for initial in ("0","1"):
            for fail in (False,True):
                with self.subTest(initial=initial,fail=fail), TemporaryDirectory() as directory:
                    root=Path(directory);setting=root/"setting";setting.write_text(initial)
                    proof={};flock=[]
                    fcntl=NS(LOCK_EX=1,LOCK_NB=4,LOCK_UN=8,flock=lambda file,operation:flock.append(operation))
                    with patch.dict(sys.modules,{"fcntl":fcntl}):
                        try:
                            with SchedulingStatsLease(proof,setting,root/"lock"):
                                self.assertEqual(setting.read_text().strip(),"1")
                                if fail:raise RuntimeError("child_failed")
                        except RuntimeError as error:
                            self.assertTrue(fail);self.assertEqual(str(error),"child_failed")
                    self.assertEqual(setting.read_text().strip(),initial)
                    self.assertEqual(flock,[5,8])
                    self.assertTrue(proof["scheduler_statistics"]["cleanup_complete"])
                    self.assertFalse(proof["scheduler_statistics"]["release_eligible"])

    def test_lock_contention_does_not_change_shared_setting(self):
        with TemporaryDirectory() as directory:
            root=Path(directory);setting=root/"setting";setting.write_text("0")
            def busy(file,operation):raise BlockingIOError("busy")
            fcntl=NS(LOCK_EX=1,LOCK_NB=4,LOCK_UN=8,flock=busy)
            lease=SchedulingStatsLease({},setting,root/"lock")
            with patch.dict(sys.modules,{"fcntl":fcntl}), self.assertRaises(BlockingIOError):
                with lease:pass
            self.assertEqual(setting.read_text(),"0")
            self.assertIsNone(lease.lock)


class ReceiverPriority(unittest.TestCase):
    def test_nonzero_priority_and_invalid_values_require_diagnostic(self):
        self.assertEqual(validate_receiver_priority(-5,True),-5)
        self.assertEqual(validate_receiver_priority(0,False),0)
        for nice, diagnostic in ((-5,False),(-11,True),(1,True),(False,True)):
            with self.subTest(nice=nice,diagnostic=diagnostic), self.assertRaises(ValueError):
                validate_receiver_priority(nice,diagnostic)

    def test_only_current_child_and_sched_other_are_changed(self):
        with patch("product_pilot_scheduler.os.SCHED_OTHER",0,create=True), \
             patch("product_pilot_scheduler.os.PRIO_PROCESS",0,create=True), \
             patch("product_pilot_scheduler.os.sched_getscheduler",return_value=0,create=True), \
             patch("product_pilot_scheduler.os.setpriority",create=True) as setter, \
             patch("product_pilot_scheduler.os.getpriority",return_value=-5,create=True):
            configure_receiver_priority(0,False)
            setter.assert_not_called()
            configure_receiver_priority(-5,True)
            setter.assert_called_once_with(0,0,-5)

    def test_unapplied_priority_or_realtime_policy_cannot_silently_pass(self):
        with patch("product_pilot_scheduler.os.SCHED_OTHER",0,create=True), \
             patch("product_pilot_scheduler.os.PRIO_PROCESS",0,create=True), \
             patch("product_pilot_scheduler.os.sched_getscheduler",return_value=1,create=True) as policy, \
             patch("product_pilot_scheduler.os.setpriority",create=True) as setter, \
             patch("product_pilot_scheduler.os.getpriority",return_value=0,create=True):
            with self.assertRaisesRegex(RuntimeError,"requires_sched_other"):
                configure_receiver_priority(-5,True)
            setter.assert_not_called()
            policy.return_value=0
            with self.assertRaisesRegex(RuntimeError,"not_applied"):
                configure_receiver_priority(-5,True)

    def test_failed_restore_is_explicit_and_releases_lock(self):
        with TemporaryDirectory() as directory:
            root=Path(directory);setting=root/"setting";setting.write_text("0")
            proof={};fcntl=NS(LOCK_EX=1,LOCK_NB=4,LOCK_UN=8,flock=lambda file,operation:None)
            lease=SchedulingStatsLease(proof,setting,root/"lock")
            with patch.dict(sys.modules,{"fcntl":fcntl}):
                lease.__enter__()
                with patch.object(Path,"write_text",side_effect=PermissionError("restore_failed")), \
                        self.assertRaises(PermissionError):
                    lease.close()
            self.assertFalse(proof["scheduler_statistics"]["cleanup_complete"])
            self.assertIsNone(lease.lock)


if __name__=="__main__":unittest.main()
