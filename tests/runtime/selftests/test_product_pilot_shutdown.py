import contextlib
import hashlib
import io
import json
import shlex
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
from types import SimpleNamespace as NS
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/product_acceptance'))
import product_aliyun_transport as transport
import product_pilot_shutdown as shutdown
from product_aliyun_transport import AliyunRemoteCommandError
from product_pilot_shutdown import (STOP_SCRIPT, LAUNCH_COMMON, LAUNCH_INSPECT_SCRIPT,
    final_complete, finish, launch_absent, recover_failed_launch, stop_and_observe, transfer)
from product_pilot_local_route import LocalSfuRoute

RUN='a'*32
CFG=dict(remote_root='/root/fixture',instance_id='i-fixture',region='fixture')

def error(code=0,session=False):
    return AliyunRemoteCommandError(NS(returncode=124,stdout='Forbidden.SessionLimit' if session else '',stderr=''),True,code)

def state():
    return dict(run_id=RUN,stop_present=True,stop_mtime_ns=123,terminal=True,
        last_event=dict(run_id=RUN,event='collector.stopped',status='COMPLETE'),
        route=dict(run_id=RUN,status='COMPLETE',child_exit_code=0,cleanup_complete=True),
        actual_cleanup=dict(rule_check_codes=[1,1],cgroup_present=False))


def launch_state(owners=None, rules=None, group=False):
    return dict(schema=1,run_id=RUN,scan_complete=True,owners=owners or [],errors=[],
        rule_check_codes=[1,1] if rules is None else rules,cgroup_present=group)


class ProcFixture:
    """Execute real remote helper source with proc/cgroup and pidfd syscall fixtures."""
    def __init__(self, directory):
        self.root=Path(directory).resolve();self.proc=self.root/'proc';self.proc.mkdir()
        self.group=self.root/('cohavora-b14-'+RUN)
        self.cfg=dict(remote_root=str(self.root),collector_media_route=dict(
            public_ip='203.0.113.1',private_ip='172.17.0.1',udp_port=17882,tcp_port=17881))
        (self.root/'product_aliyun_target.json').write_text(json.dumps(self.cfg))
        self.ns={};exec(compile(LAUNCH_COMMON,'launch-common-real-source','exec'),self.ns)
        self.seconds=0.;self.opened={};self.signals=[];self.commands=[]
        self.rules={'udp':True,'tcp':True};self.delete_fail=set();self.ignore_term=set()
        self.reuse_at_open=set();self.pidfd_errors=set()
        self.ns['time']=NS(monotonic=lambda:self.seconds,sleep=self.sleep)
        self.ns['os']=NS(pidfd_open=self.open_pidfd,close=lambda fd:self.opened.pop(fd))
        self.ns['signal']=NS(SIGTERM=15,SIGKILL=9,pidfd_send_signal=self.send_pidfd)
        self.ns['subprocess']=NS(run=self.command)
        self.ns['launch_route']=self.route

    def sleep(self, seconds):self.seconds+=seconds

    def route(self, root, run):
        route=LocalSfuRoute(self.cfg,run,root/'unused-launch-readonly.json')
        route.group=self.group
        return route

    def add(self, pid, role, run=RUN, ticks=111, ambiguous=False):
        path=self.proc/str(pid);path.mkdir(exist_ok=True)
        argv=[str(self.root/'venv/bin/python'),str(self.root/('product_pilot_'+('local_route' if role=='route' else role)+'.py')),
            '--run-id',run]
        dest=self.root/('pilot-'+run[:8])
        if role=='route':argv+=['--target-config',str(self.root/'product_aliyun_target.json'),
            '--result',str(self.root/('pilot-'+run[:8]+'-route.json')),'--',str(self.root/'venv/bin/python'),
            str(self.root/'product_pilot_remote.py'),'--run-id',run]
        else:argv+=['--output',str(dest/'load-process' if role=='load' else dest)]
        if ambiguous:argv+=['--run-id',run]
        (path/'cmdline').write_bytes(b'\0'.join(arg.encode() for arg in argv)+b'\0')
        self.set_ticks(pid,ticks)
        return dict(pid=pid,start_ticks=ticks,role=role)

    def set_ticks(self,pid,ticks):
        (self.proc/str(pid)/'stat').write_text(str(pid)+' (owned stub) '+' '.join(['S']+['0']*18+[str(ticks)]))

    def ticks(self,pid):return int((self.proc/str(pid)/'stat').read_text().split()[-1])

    def remove(self,pid):
        path=self.proc/str(pid)
        for file in path.iterdir():file.unlink()
        path.rmdir()

    def open_pidfd(self,pid,flags):
        if pid in self.pidfd_errors:raise PermissionError('fixture pidfd denied')
        if not (self.proc/str(pid)).exists():raise ProcessLookupError()
        fd=10000+len(self.opened);self.opened[fd]=(pid,self.ticks(pid))
        if pid in self.reuse_at_open:self.set_ticks(pid,self.ticks(pid)+1)
        return fd

    def send_pidfd(self,fd,number,info,flags):
        pid,ticks=self.opened[fd]
        # A fixture pidfd remains bound to the captured incarnation, like Linux.
        if not (self.proc/str(pid)).exists() or self.ticks(pid)!=ticks:raise ProcessLookupError()
        self.signals.append((pid,ticks,number))
        if number==9 or pid not in self.ignore_term:self.remove(pid)

    def command(self,argv,**kwargs):
        self.commands.append(argv)
        self.assert_exact(argv)
        protocol=argv[argv.index('-p')+1];action=argv[5]
        if action=='-C':return NS(returncode=0 if self.rules[protocol] else 1)
        if action=='-D':
            if protocol in self.delete_fail:return NS(returncode=2)
            self.rules[protocol]=False;return NS(returncode=0)
        raise AssertionError('unexpected_mutation')

    def assert_exact(self,argv):
        route=self.route(self.root,RUN)
        protocol=argv[argv.index('-p')+1];port=dict(route.ports)[protocol]
        if argv!=route.command(argv[5],route.rule(protocol,port)):
            raise AssertionError('exact_full_run_rule_required')
        if argv[argv.index('--comment')+1]!='cohavora-b14-'+RUN:
            raise AssertionError('full_run_comment_required')

    def inspect(self):return self.ns['inspect_launch'](self.root,RUN,self.proc)

    def cleanup(self,initial):return self.ns['cleanup_launch'](self.root,RUN,initial,self.proc)


class ShutdownContracts(unittest.TestCase):
    def observe(self,replies):
        executor=Mock(side_effect=replies)
        attempts=[]
        with patch('product_pilot_shutdown.time.sleep'):
            result=stop_and_observe(CFG,RUN,False,attempts,executor)
        return result,executor,attempts

    def test_lost_stop_reply_and_cli_timeout_remote_zero_recover_without_rewriting(self):
        value,executor,attempts=self.observe([error(),error(),json.dumps(state())])
        self.assertTrue(final_complete(value,RUN))
        self.assertEqual([a['stage'] for a in attempts],['request_stop','readonly_state','readonly_state'])
        self.assertEqual(sum(not a['readonly'] for a in attempts),1)
        self.assertEqual(executor.call_count,3)

    def test_remote_rejection_never_retries_or_downloads(self):
        executor=Mock(side_effect=error(1))
        with self.assertRaises(AliyunRemoteCommandError):stop_and_observe(CFG,RUN,False,[],executor)
        self.assertEqual(executor.call_count,1)

    def test_wrong_run_or_missing_stop_receipt_is_terminal(self):
        for change in (dict(run_id='b'*32),dict(stop_present=False),dict(stop_mtime_ns=True)):
            bad=state();bad.update(change)
            executor=Mock(side_effect=[json.dumps(state()),json.dumps(bad)])
            with self.assertRaises(ValueError):stop_and_observe(CFG,RUN,False,[],executor)
            self.assertEqual(executor.call_count,2)

    def test_session_limit_retries_once_then_remains_failed(self):
        executor=Mock(side_effect=[error(session=True),error(session=True),json.dumps(state())])
        with self.assertRaises(AliyunRemoteCommandError):stop_and_observe(CFG,RUN,False,[],executor)
        self.assertEqual(executor.call_count,2)

    def test_legal_delayed_close_uses_full_monotonic_budget(self):
        for terminal_at in (12.,40.,44.6):
            clock=NS(seconds=0.);observations=[];attempts=[]
            def sleep(seconds):clock.seconds+=seconds
            def execute(command,timeout):
                is_state='import hashlib,json,subprocess,sys' in command
                if is_state:
                    self.assertGreater(timeout,0)
                    self.assertLessEqual(timeout,45.05-clock.seconds+1e-9)
                    observations.append(clock.seconds)
                clock.seconds+=min(.05,timeout)
                value=state();value['terminal']=is_state and clock.seconds>=terminal_at
                return json.dumps(value)
            with patch.object(shutdown,'time',NS(monotonic=lambda:clock.seconds,sleep=sleep)):
                value=stop_and_observe(CFG,RUN,False,attempts,execute)
            self.assertTrue(final_complete(value,RUN))
            self.assertGreater(len(observations),20)
            self.assertLess(clock.seconds,45.05)
            self.assertEqual(sum(item['stage']=='request_stop' for item in attempts),1)

    def test_missing_or_late_terminal_cannot_exceed_or_pass_45_second_budget(self):
        for late_terminal in (False,True):
            clock=NS(seconds=0.);attempts=[]
            def sleep(seconds):clock.seconds+=seconds
            def execute(command,timeout):
                is_state='import hashlib,json,subprocess,sys' in command
                self.assertGreater(timeout,0)
                if is_state:self.assertLessEqual(timeout,45.05-clock.seconds+1e-9)
                # The late-reply variant deliberately violates the transport
                # timeout. Even its claimed COMPLETE must fail closed.
                clock.seconds+=(46. if late_terminal and is_state else min(.05,timeout))
                value=state();value['terminal']=late_terminal and is_state
                return json.dumps(value)
            with patch.object(shutdown,'time',NS(monotonic=lambda:clock.seconds,sleep=sleep)):
                with self.assertRaisesRegex(RuntimeError,'receipt_budget_exhausted'):
                    stop_and_observe(CFG,RUN,False,attempts,execute)
            if not late_terminal:self.assertAlmostEqual(clock.seconds,45.05)
            self.assertEqual(sum(item['stage']=='request_stop' for item in attempts),1)

    def test_transport_last_seconds_keep_positive_integer_remote_and_exact_local_timeout(self):
        for seconds in (15,5.5,.25):
            def run(argv,**kwargs):
                self.assertEqual(kwargs['timeout'],seconds)
                remote_timeout=argv[argv.index('--timeout')+1]
                self.assertEqual(int(remote_timeout),max(1,int(seconds-5)))
                self.assertTrue(remote_timeout.isdecimal())
                wrapped=argv[argv.index('-c')+1]
                marker=transport.re.search(r'PRODUCT_EXIT_[a-f0-9]+:',wrapped).group()
                payload=dict(instance_id=CFG['instance_id'],command=wrapped,exit_code=0,
                             stdout='ok\n'+marker+'0\n')
                return NS(returncode=0,stdout=json.dumps(payload),stderr='')
            with patch.object(transport,'target',return_value=CFG),patch.object(transport.subprocess,'run',side_effect=run):
                self.assertEqual(transport.execute('readonly fixture',timeout=seconds),'ok')

    def test_abort_or_residual_resources_cannot_be_complete(self):
        for change in (dict(child_exit_code=-6),dict(child_exit_code=False),dict(status='FAILED'),dict(cleanup_complete=False)):
            bad=state();bad['route'].update(change);self.assertFalse(final_complete(bad,RUN))
        for change in (dict(rule_check_codes=[0,1]),dict(rule_check_codes=[True,True]),dict(cgroup_present=True)):
            bad=state();bad['actual_cleanup'].update(change);self.assertFalse(final_complete(bad,RUN))

    def test_existing_stop_original_timestamp_is_preserved(self):
        with TemporaryDirectory() as directory:
            root=Path(directory);dest=root/('pilot-'+RUN[:8]);dest.mkdir()
            (dest/'ready.json').write_text(json.dumps(dict(run_id=RUN)))
            stop=dest/'stop';stop.write_bytes(b'');stamp=stop.stat().st_mtime_ns
            with patch.object(sys,'argv',['stop',str(root),RUN]),contextlib.redirect_stdout(io.StringIO()):
                exec(compile(STOP_SCRIPT,'stop-script','exec'),{})
            self.assertEqual(stop.stat().st_mtime_ns,stamp)

    def test_cli_transfer_failure_can_only_recover_exact_immutable_bytes(self):
        raw=b'exact original bytes\n'
        meta=dict(size=len(raw),sha256=hashlib.sha256(raw).hexdigest())
        def download(argv,**kw):
            Path(argv[3]).write_bytes(raw)
            return NS(returncode=124,stdout='',stderr='transport timeout')
        with TemporaryDirectory() as directory:
            local=Path(directory)/'remote.jsonl';attempts=[]
            transfer(CFG,'/root/fixture/remote.jsonl',local,meta,attempts,download)
            self.assertEqual(local.read_bytes(),raw)
            self.assertEqual(attempts[0]['cli_returncode'],124)
            self.assertTrue(attempts[0]['hash_verified'])

    def test_mismatched_transfer_bytes_remain_failed_and_are_preserved(self):
        def download(argv,**kw):
            Path(argv[3]).write_bytes(b'wrong')
            return NS(returncode=0,stdout='',stderr='')
        with TemporaryDirectory() as directory:
            local=Path(directory)/'remote.jsonl';attempts=[]
            with self.assertRaises(RuntimeError):
                transfer(CFG,'/root/fixture/remote.jsonl',local,dict(size=5,sha256='a'*64),attempts,download)
            self.assertFalse(local.exists())
            self.assertEqual(len(attempts),2)
            self.assertTrue(all(not a['hash_verified'] for a in attempts))
            self.assertEqual(len(list(Path(directory).glob('*.incoming-*'))),2)


class LaunchCleanupContracts(unittest.TestCase):
    def recover(self, replies):
        executor=Mock(side_effect=replies)
        with TemporaryDirectory() as directory:
            root=Path(directory)
            result=recover_failed_launch(root,CFG,RUN,executor)
            saved=json.loads((root/'launch-cleanup.json').read_text())
        self.assertEqual(saved,result)
        return result,executor

    def test_configured_collector_interpreter_for_all_fallback_requests(self):
        empty=launch_state()
        result,executor=self.recover([json.dumps(empty)]*3)
        self.assertEqual(result['status'],'COMPLETE')
        self.assertEqual([shlex.split(call.args[0])[0] for call in executor.call_args_list],
                         [CFG['remote_root']+'/venv/bin/python']*3)
        owned=launch_state([dict(pid=42,start_ticks=100,role='remote')])
        cleaned=dict(schema=1,run_id=RUN,errors=[],operations=[],final_observation=empty)
        result,executor=self.recover([json.dumps(owned),json.dumps(cleaned),json.dumps(empty)])
        self.assertEqual(result['status'],'COMPLETE')
        self.assertEqual([shlex.split(call.args[0])[0] for call in executor.call_args_list],
                         [CFG['remote_root']+'/venv/bin/python']*3)
        normal=Mock(side_effect=[json.dumps(state()),json.dumps(state())])
        stop_and_observe(CFG,RUN,False,[],normal)
        self.assertEqual([shlex.split(call.args[0])[0] for call in normal.call_args_list],['python3']*2)

    def test_pending_ready_owned_start_then_transport_timeout_keeps_original_failure(self):
        owners=[dict(pid=42,start_ticks=100,role='remote')]
        initial=launch_state(owners,[0,0],True);empty=launch_state()
        cleaned=dict(schema=1,run_id=RUN,errors=[],operations=[dict(stage='pidfd_signal')],final_observation=empty)
        executor=Mock(side_effect=[error(1),json.dumps(initial),json.dumps(cleaned),json.dumps(empty)])
        with TemporaryDirectory() as directory,patch('product_pilot_shutdown.target',return_value=CFG):
            root=Path(directory);(root/'plan.json').write_text(json.dumps(dict(run_id=RUN)))
            result=finish(root,RUN,launch_attempted=True,executor=executor)
            self.assertEqual(result['status'],'FAILED')
            self.assertEqual(json.loads((root/'remote-shutdown.json').read_text())['status'],'FAILED')
            cleanup=json.loads((root/'launch-cleanup.json').read_text())
            self.assertEqual(cleanup['status'],'COMPLETE')
            self.assertEqual([item['stage'] for item in cleanup['attempts']],
                ['launch_readonly_initial','launch_cleanup','launch_readonly_final'])
            self.assertFalse(cleanup['attempts'][1]['readonly'])

    def test_receipt_open_failure_still_runs_actual_owned_launch_fallback(self):
        empty=json.dumps(launch_state())
        executor=Mock(side_effect=[error(1),empty,empty,empty])
        original_open=Path.open
        with TemporaryDirectory() as directory,patch.object(shutdown,'target',return_value=CFG):
            root=Path(directory);(root/'plan.json').write_text(json.dumps(dict(run_id=RUN)))
            def open_receipt(path,*args,**kwargs):
                if path==root/'remote-shutdown.json':raise PermissionError('fixture receipt denied')
                return original_open(path,*args,**kwargs)
            with patch.object(Path,'open',open_receipt):
                result=finish(root,RUN,launch_attempted=True,executor=executor)
            self.assertEqual(result['status'],'FAILED')
            self.assertEqual(result['error_type'],'AliyunRemoteCommandError')
            self.assertEqual(result['receipt_error_type'],'PermissionError')
            self.assertEqual(result['launch_cleanup_status'],'COMPLETE')
            self.assertEqual(json.loads((root/'launch-cleanup.json').read_text())['status'],'COMPLETE')
            self.assertEqual(executor.call_count,4)
            self.assertFalse((root/'remote-shutdown.json').exists())

    def test_receipt_write_and_fallback_exception_preserve_both_failures_and_original_cause(self):
        for failure_stage in ('fallback_receipt','fallback_command'):
            empty=json.dumps(launch_state())
            executor=Mock(side_effect=[error(1),empty,empty,empty])
            original_dump=json.dump
            def dump_receipt(value,stream,*args,**kwargs):
                name=Path(stream.name).name
                if name=='remote-shutdown.json':raise OSError('fixture disk full')
                if name=='launch-cleanup.json' and failure_stage=='fallback_receipt':
                    raise PermissionError('fixture fallback receipt denied')
                return original_dump(value,stream,*args,**kwargs)
            with TemporaryDirectory() as directory,patch.object(shutdown,'target',return_value=CFG):
                root=Path(directory);(root/'plan.json').write_text(json.dumps(dict(run_id=RUN)))
                with patch.object(shutdown.json,'dump',side_effect=dump_receipt):
                    if failure_stage=='fallback_command':
                        with patch.object(shutdown,'recover_failed_launch',side_effect=LookupError('fixture')) as fallback:
                            result=finish(root,RUN,launch_attempted=True,executor=executor)
                            fallback.assert_called_once_with(root,CFG,RUN,executor)
                    else:
                        result=finish(root,RUN,launch_attempted=True,executor=executor)
                        self.assertEqual(executor.call_count,4)
                self.assertEqual(result['status'],'FAILED')
                self.assertEqual(result['error_type'],'AliyunRemoteCommandError')
                self.assertEqual(result['receipt_error_type'],'OSError')
                self.assertEqual(result['launch_cleanup_error_type'],
                                 'PermissionError' if failure_stage=='fallback_receipt' else 'LookupError')

    def test_missing_ready_no_start_requires_actual_proc_route_queries(self):
        empty=json.dumps(launch_state())
        result,executor=self.recover([empty,empty,empty])
        self.assertEqual(result['status'],'COMPLETE')
        self.assertTrue(all(item['readonly'] for item in result['attempts']))
        self.assertEqual(executor.call_count,3)

    def test_late_launch_after_empty_first_query_is_cleaned_once(self):
        initial=launch_state([dict(pid=42,start_ticks=100,role='remote')]);empty=launch_state()
        cleaned=dict(schema=1,run_id=RUN,errors=[],operations=[],final_observation=empty)
        result,executor=self.recover([json.dumps(empty),json.dumps(initial),json.dumps(cleaned),json.dumps(empty)])
        self.assertEqual(result['status'],'COMPLETE')
        self.assertEqual(sum(not item['readonly'] for item in result['attempts']),1)
        self.assertEqual(executor.call_count,4)

    def test_lost_cleanup_reply_still_queries_residual_and_remains_failed(self):
        initial=launch_state([dict(pid=42,start_ticks=100,role='remote')])
        result,executor=self.recover([json.dumps(initial),error(),json.dumps(launch_state())])
        self.assertEqual(result['status'],'FAILED')
        self.assertIn('final_observation',result)
        self.assertEqual(executor.call_count,3)

    def test_incomplete_or_foreign_scan_never_signals(self):
        for changes in (dict(run_id='b'*32),dict(scan_complete=False),dict(errors=[dict(stage='proc')],scan_complete=False),
                dict(errors=[dict(stage='proc')]),dict(rule_check_codes=[True,True]),dict(owners=[dict(pid=True,start_ticks=1,role='remote')])):
            initial=launch_state();initial.update(changes)
            result,executor=self.recover([json.dumps(initial)])
            self.assertEqual(result['status'],'FAILED')
            self.assertEqual(executor.call_count,1)

    def test_cleanup_error_or_residual_cannot_pass(self):
        initial=launch_state([dict(pid=42,start_ticks=100,role='remote')]);empty=launch_state()
        for errors,final in (([dict(stage='pidfd_signal')],empty),([],initial)):
            cleaned=dict(schema=1,run_id=RUN,errors=errors,operations=[],final_observation=final)
            result,_=self.recover([json.dumps(initial),json.dumps(cleaned),json.dumps(final)])
            self.assertEqual(result['status'],'FAILED')

    def test_historical_normal_failed_bytes_preserved_and_complete_never_recovered(self):
        for status in ('FAILED','COMPLETE'):
            with TemporaryDirectory() as directory,patch('product_pilot_shutdown.target',return_value=CFG):
                root=Path(directory);(root/'plan.json').write_text(json.dumps(dict(run_id=RUN)))
                raw=json.dumps(dict(schema=1,run_id=RUN,status=status,original='historical failure')).encode()
                path=root/'remote-shutdown.json';path.write_bytes(raw)
                empty=json.dumps(launch_state());executor=Mock(side_effect=[empty,empty,empty])
                self.assertEqual(finish(root,RUN,launch_attempted=True,executor=executor)['status'],status)
                self.assertEqual(path.read_bytes(),raw)
                self.assertEqual(executor.call_count,3 if status=='FAILED' else 0)
                self.assertEqual((root/'launch-cleanup.json').exists(),status=='FAILED')

    def test_existing_cleanup_receipt_prevents_repeated_mutation(self):
        with TemporaryDirectory() as directory:
            root=Path(directory);(root/'launch-cleanup.json').write_text('original')
            executor=Mock()
            with self.assertRaisesRegex(ValueError,'already_recorded'):
                recover_failed_launch(root,CFG,RUN,executor)
            executor.assert_not_called()
            self.assertEqual((root/'launch-cleanup.json').read_text(),'original')

    def test_actual_script_pending_ready_terminates_only_exact_owned_processes(self):
        with TemporaryDirectory() as directory:
            fixture=ProcFixture(directory)
            fixture.add(42,'remote');fixture.add(43,'load');fixture.add(44,'route')
            fixture.add(45,'remote',run='b'*32)
            dest=fixture.root/('pilot-'+RUN[:8]);dest.mkdir()
            self.assertFalse((dest/'ready.json').exists())
            initial=fixture.inspect()
            result=fixture.cleanup(initial)
            self.assertEqual(result['errors'],[])
            self.assertTrue((dest/'stop').exists())
            self.assertTrue(launch_absent(result['final_observation'],RUN))
            self.assertEqual({pid for pid,_,_ in fixture.signals},{42,43,44})
            self.assertTrue((fixture.proc/'45').exists())
            self.assertEqual(fixture.signals[-1][0],44)  # wrapper last
            self.assertEqual(fixture.opened,{})

    def test_actual_script_pid_reuse_between_pidfd_and_stat_never_signals_replacement(self):
        with TemporaryDirectory() as directory:
            fixture=ProcFixture(directory);fixture.add(42,'remote')
            initial=fixture.inspect();fixture.reuse_at_open.add(42)
            result=fixture.cleanup(initial)
            self.assertTrue(result['errors'])
            self.assertEqual(fixture.signals,[])
            self.assertTrue((fixture.proc/'42').exists())
            self.assertTrue(all(argv[5]=='-C' for argv in fixture.commands))
            self.assertEqual(fixture.opened,{})

    def test_actual_script_term_timeout_uses_bound_pidfd_kill(self):
        with TemporaryDirectory() as directory:
            fixture=ProcFixture(directory);fixture.add(42,'remote');fixture.ignore_term.add(42)
            result=fixture.cleanup(fixture.inspect())
            self.assertEqual(fixture.signals,[(42,111,15),(42,111,9)])
            self.assertTrue(launch_absent(result['final_observation'],RUN))
            self.assertLess(fixture.seconds,12)

    def test_actual_script_one_pidfd_exception_does_not_skip_other_owned_cleanup(self):
        with TemporaryDirectory() as directory:
            fixture=ProcFixture(directory);fixture.add(42,'remote');fixture.add(43,'load')
            fixture.pidfd_errors.add(42)
            result=fixture.cleanup(fixture.inspect())
            self.assertTrue(result['errors'])
            self.assertEqual({pid for pid,_,_ in fixture.signals},{43})
            self.assertTrue((fixture.proc/'42').exists())
            self.assertFalse((fixture.proc/'43').exists())

    def test_actual_script_existing_stop_timestamp_is_preserved(self):
        with TemporaryDirectory() as directory:
            fixture=ProcFixture(directory);fixture.add(42,'remote')
            dest=fixture.root/('pilot-'+RUN[:8]);dest.mkdir()
            stop=dest/'stop';stop.write_bytes(b'original request');stamp=stop.stat().st_mtime_ns
            result=fixture.cleanup(fixture.inspect())
            self.assertEqual(result['errors'],[])
            self.assertEqual(stop.read_bytes(),b'original request')
            self.assertEqual(stop.stat().st_mtime_ns,stamp)

    def test_actual_script_ambiguous_owned_argv_or_same_prefix_foreign_receipt_is_unproven(self):
        with TemporaryDirectory() as directory:
            fixture=ProcFixture(directory);fixture.add(42,'remote',ambiguous=True)
            initial=fixture.inspect()
            self.assertFalse(initial['scan_complete'])
            with self.assertRaises(ValueError):fixture.cleanup(initial)
            self.assertFalse(fixture.signals)
        with TemporaryDirectory() as directory:
            fixture=ProcFixture(directory);dest=fixture.root/('pilot-'+RUN[:8]);dest.mkdir()
            (dest/'ready.json').write_text(json.dumps(dict(run_id=RUN[:8]+'b'*24)))
            self.assertFalse(fixture.inspect()['scan_complete'])

    def test_actual_script_rule_failure_continues_other_exact_rule_and_keeps_fail(self):
        with TemporaryDirectory() as directory:
            fixture=ProcFixture(directory);fixture.add(42,'remote');fixture.delete_fail.add('udp')
            result=fixture.cleanup(fixture.inspect())
            self.assertTrue(result['errors'])
            self.assertEqual(fixture.rules,dict(udp=True,tcp=False))
            self.assertFalse(launch_absent(result['final_observation'],RUN))
            self.assertEqual({argv[argv.index('-p')+1] for argv in fixture.commands if argv[5]=='-D'},{'udp','tcp'})

    def test_actual_script_nonempty_cgroup_refuses_rule_and_group_removal(self):
        with TemporaryDirectory() as directory:
            fixture=ProcFixture(directory);fixture.group.mkdir()
            route=fixture.route(fixture.root,RUN)
            (fixture.group/'net_cls.classid').write_text(str(route.classid))
            (fixture.group/'cgroup.procs').write_text('45\n')
            result=fixture.cleanup(fixture.inspect())
            self.assertTrue(result['errors'])
            self.assertTrue(fixture.group.exists())
            self.assertTrue(all(argv[5]=='-C' for argv in fixture.commands))

    def test_actual_script_wrong_classid_or_stat_failure_is_not_absence(self):
        with TemporaryDirectory() as directory:
            fixture=ProcFixture(directory);fixture.add(42,'remote')
            (fixture.proc/'42'/'stat').write_text('not a valid stat frame')
            initial=fixture.inspect()
            self.assertFalse(initial['scan_complete'])
            with self.assertRaises(ValueError):fixture.cleanup(initial)
            self.assertFalse(fixture.signals)
        with TemporaryDirectory() as directory:
            fixture=ProcFixture(directory);fixture.group.mkdir()
            (fixture.group/'net_cls.classid').write_text('1')
            (fixture.group/'cgroup.procs').write_text('')
            result=fixture.cleanup(fixture.inspect())
            self.assertTrue(result['errors'])
            self.assertTrue(fixture.group.exists())
            self.assertTrue(all(argv[5]=='-C' for argv in fixture.commands))

    def test_actual_script_empty_exact_cgroup_is_removed(self):
        with TemporaryDirectory() as directory:
            fixture=ProcFixture(directory);fixture.group.mkdir()
            route=fixture.route(fixture.root,RUN)
            (fixture.group/'net_cls.classid').write_text(str(route.classid))
            (fixture.group/'cgroup.procs').write_text('')
            original_rmdir=Path.rmdir
            def kernel_rmdir(path):
                if path==fixture.group:
                    # Linux cgroup virtual control files do not prevent rmdir.
                    for name in ('net_cls.classid','cgroup.procs'):(path/name).unlink()
                return original_rmdir(path)
            with patch.object(Path,'rmdir',kernel_rmdir):result=fixture.cleanup(fixture.inspect())
            self.assertEqual(result['errors'],[])
            self.assertTrue(launch_absent(result['final_observation'],RUN))
            self.assertFalse(fixture.group.exists())

    def test_inline_script_guard_refuses_outside_root_before_any_proc_or_rule_access(self):
        with TemporaryDirectory() as directory,patch.object(sys,'argv',['inspect',directory,RUN]):
            with self.assertRaisesRegex(ValueError,'identity_invalid'):
                exec(compile(LAUNCH_INSPECT_SCRIPT,'actual-launch-inspect-script','exec'),{})

if __name__=='__main__':unittest.main()
