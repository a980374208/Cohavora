"""Offline full-exit admission and verdict checks; no SDK or service."""
import importlib.util
import json
from pathlib import Path
import signal
import sys
import tempfile
import types
import unittest
from unittest.mock import patch
PATH=Path(__file__).parents[1]/'tools/diagnostics/native_exit/run_full_publisher_exit_capture.py'
if sys.platform=='win32': sys.modules.setdefault('resource',types.ModuleType('resource'))
spec=importlib.util.spec_from_file_location('full_exit_capture',PATH)
driver=importlib.util.module_from_spec(spec); spec.loader.exec_module(driver)
api_spec=importlib.util.spec_from_file_location('exit_api',PATH.with_name('publisher_exit_api.py'))
api=importlib.util.module_from_spec(api_spec); api_spec.loader.exec_module(api)
FROZEN_PUBLISHER=PATH.parent/'fixtures/product_pilot_load.py'
CURRENT_PUBLISHER=PATH.parents[2]/'product_acceptance/product_pilot_load.py'
COMPANIONS=('product_pilot_scheduler.py','product_pilot_timing.py',
            'product_pilot_video_counter.py')
SUBSCRIBER_COMPANIONS=('product_pilot_scheduler.py','product_pilot_video_counter.py')

def write_plan(directory,publisher_source=None):
    root=Path(directory)
    publisher=root/'product_pilot_load.py'
    publisher.write_bytes(FROZEN_PUBLISHER.read_bytes() if publisher_source is None else publisher_source)
    names=[*COMPANIONS,'wrapper.py','api.py','scheduler_policy.json',
           'launcher.py','observer.so','subscriber_wrapper.py',
           *[f'sdk-{i}.py' for i in range(5)],'liblivekit_ffi.so']
    for name in names:
        (root/name).write_bytes(('offline placeholder '+name).encode())
    subscriber_root=root/'subscriber-bundle'
    subscriber_root.mkdir()
    for name in ['subscriber.py',*SUBSCRIBER_COMPANIONS]:
        (subscriber_root/name).write_bytes(('offline subscriber placeholder '+name).encode())
    run_id='a'*32
    output=str(Path('/root/livekit-product-acceptance')/('native-exit-'+run_id)/'full-exit-capture')
    plan=dict(run_id=run_id,room='native-exit-'+run_id,room_name='native-exit-'+run_id,
              output=output,bundle_root=output,remote_root='/root/livekit-product-acceptance',
              publisher_path=str(publisher),wrapper_path=str(root/'wrapper.py'),
              subscriber_path=str(subscriber_root/'subscriber.py'),api_helper_path=str(root/'api.py'),
              scheduler_policy=str(root/'scheduler_policy.json'),
              publisher_launcher_path=str(root/'launcher.py'),observer_library=str(root/'observer.so'),
              subscriber_wrapper_path=str(root/'subscriber_wrapper.py'),
              diagnostic_profile='full-exit-all-660-two-240',hard_sdk_seconds=660,cleanup_seconds=30,
              sdk_files=[str(root/name) for name in names[-6:]])
    plan['files']={str(root/name):driver.digest(root/name) for name in ['product_pilot_load.py',*names]}
    plan['files'].update({str(subscriber_root/name):driver.digest(subscriber_root/name)
                          for name in ['subscriber.py',*SUBSCRIBER_COMPANIONS]})
    path=root/'plan.json'
    path.write_text(json.dumps(plan),encoding='utf-8')
    return plan,path

class FullCaptureTest(unittest.TestCase):
    def test_complete_historical_manifest_and_missing_import_dependencies(self):
        with tempfile.TemporaryDirectory() as directory,patch.dict(driver.os.environ,LD_PRELOAD='',LIVEKIT_LIB_PATH=''):
            plan,path=write_plan(directory)
            self.assertEqual(driver.load_plan(path,driver.digest(path)),plan)
            short_spec=importlib.util.spec_from_file_location('frozen_short_capture',PATH.with_name('run_publisher_exit_capture.py'))
            short=importlib.util.module_from_spec(short_spec); short_spec.loader.exec_module(short)
            self.assertEqual(plan['files'][plan['publisher_path']],driver.PUBLISHER_SHA)
            self.assertEqual(short.PUBLISHER_SHA,driver.PUBLISHER_SHA)
            required=[*(Path(directory)/name for name in COMPANIONS),
                      *(Path(directory)/'subscriber-bundle'/name for name in SUBSCRIBER_COMPANIONS)]
            for dependency in required:
                changed=dict(plan,files=dict(plan['files']))
                del changed['files'][str(dependency)]
                path.write_text(json.dumps(changed),encoding='utf-8')
                with self.subTest(missing=str(dependency)),self.assertRaisesRegex(ValueError,'manifest_incomplete'):
                    driver.load_plan(path,driver.digest(path))

    def test_current_publisher_is_not_historical_reproduction_input(self):
        with tempfile.TemporaryDirectory() as directory,patch.dict(driver.os.environ,LD_PRELOAD='',LIVEKIT_LIB_PATH=''):
            plan,path=write_plan(directory,CURRENT_PUBLISHER.read_bytes())
            self.assertNotEqual(plan['files'][plan['publisher_path']],driver.PUBLISHER_SHA)
            with self.assertRaisesRegex(ValueError,'publisher_changed'):
                driver.load_plan(path,driver.digest(path))

    def test_deadline_admission_reserves_both_full_holds(self):
        self.assertEqual(driver.lifecycle_budget(2),635)
        self.assertEqual(driver.lifecycle_budget(1),340)
        with self.assertRaises(ValueError): driver.lifecycle_budget(0)

    def test_plan_layout_and_deadline_fail_closed(self):
        plan=dict(files={'launcher':'a','observer':'b'},publisher_launcher_path='launcher',observer_library='observer',
                  diagnostic_profile='full-exit-660-two-240',output='/owned/full-exit-capture',
                  bundle_root='/owned/full-exit-capture',remote_root='/root/livekit-product-acceptance',
                  room='owned',room_name='owned',hard_sdk_seconds=660,cleanup_seconds=30)
        driver.validate_full_plan(plan)
        root=Path('/root/livekit-product-acceptance'); run_id='a'*32
        full=root/('native-exit-'+run_id)/'full-exit-capture'
        api.validate_bundle(dict(output=str(full),diagnostic_profile='full-exit-660-two-240'),root,full,run_id)
        api.validate_bundle(dict(output=str(full),diagnostic_profile='full-exit-all-660-two-240'),root,full,run_id)
        with self.assertRaises(api.GateError): api.validate_bundle(dict(output=str(full),diagnostic_profile='unknown'),root,full,run_id)
        api.validate_bundle({},root,full.with_name('exit-capture'),run_id)
        with self.assertRaises(api.GateError): api.validate_bundle({},root,full,run_id)
        with self.assertRaises(api.GateError): api.validate_bundle({},root,full.with_name('arbitrary'),run_id)
        for field,value in [('hard_sdk_seconds',120),('bundle_root','/other'),('diagnostic_profile','short'),('observer_library','missing')]:
            with self.subTest(field=field), self.assertRaises(ValueError):
                driver.validate_full_plan(dict(plan,**{field:value}))

    def test_abort_requires_exact_signal_panic_and_python_terminal(self):
        self.assertEqual(driver.classify_exit(-signal.SIGABRT,b'panicked at',True,False),'ABORT_REPRODUCED')
        for code,raw,terminal in [(1,b'panicked at',True),(-signal.SIGABRT,b'',True),(-signal.SIGABRT,b'panicked at',False)]:
            self.assertEqual(driver.classify_exit(code,raw,terminal,False),'MEASUREMENT_FAILED')

    def test_natural_exit_requires_complete_phase_identity_and_sequence(self):
        events=[('cleanup','begin')]
        for phase in ('capture_drain','audio_clear','room_disconnect','source_close','result_commit'):
            events.extend(((phase,'begin'),(phase,'end')))
        events.extend((('publish_return','end'),('asyncio_run_return','end'),
                       ('python_atexit','begin'),('native_ffi_dispose','begin'),
                       ('native_drop_handle','begin'),('native_drop_handle','end'),
                       ('native_ffi_dispose','end'),('integrity','end')))
        def rows(events):
            return [dict(phase=p,kind=k,success=True,pid=12,tid=22,sequence=i+1,
                         dropped=0,monotonic_ns=i+1) for i,(p,k) in enumerate(events)]
        records=rows(events)
        self.assertTrue(driver.phases_complete(records,12))
        self.assertFalse(driver.phases_complete(rows([('integrity','end')]),12))
        for index in range(len(events)):
            self.assertFalse(driver.phases_complete(rows(events[:index]+events[index+1:]),12))
        for key,value in [('pid',13),('sequence',2),('dropped',1),('success',1),
                          ('kind','unknown'),('monotonic_ns',0),('pid',True),('tid',True)]:
            changed=[dict(row) for row in records]; changed[0][key]=value
            self.assertFalse(driver.phases_complete(changed,12),key)
        for key,value in [('monotonic_ns',1),('success',False),('kind','begin')]:
            changed=[dict(row) for row in records]; changed[-1][key]=value
            self.assertFalse(driver.phases_complete(changed,12),key)
        changed=list(events); changed[1],changed[3]=changed[3],changed[1]
        self.assertFalse(driver.phases_complete(rows(changed),12))
        changed=rows(events); changed[16]['tid']=23
        self.assertFalse(driver.phases_complete(changed,12))
        repeated=events[:17]+[('native_drop_handle','begin'),('native_drop_handle','end')]+events[17:]
        self.assertTrue(driver.phases_complete(rows(repeated),12))
        self.assertEqual(driver.classify_exit(0,b'',True,False),'MEASUREMENT_FAILED')

    def test_all_worker_command_and_old_profile_compatibility(self):
        plan=dict(diagnostic_profile='full-exit-660-two-240',subscriber_path='receiver.py',
                  publisher_launcher_path='launch.py',observer_library='observer.so',
                  subscriber_wrapper_path='receiver-wrapper.py',files={'observer.so':'a'*64,'receiver.py':'b'*64})
        directory=Path('/owned/subscriber-1')
        old=driver.subscriber_command(plan,['--room','owned'],directory)
        self.assertEqual(old[:2],[sys.executable,'receiver.py'])
        self.assertNotIn('--observer-library',old)
        new=driver.subscriber_command(dict(plan,diagnostic_profile='full-exit-all-660-two-240'),['--room','owned'],directory)
        self.assertEqual(new[new.index('--')+1],'receiver-wrapper.py')
        self.assertEqual(new[new.index('--trace-directory')+1],str(directory/'pthread-exit-trace'))
        self.assertIn('--source-sha256',new)
        self.assertEqual(new[-2:],['--seconds','660'])

    def test_worker_abort_evidence_is_independent(self):
        import tempfile
        with tempfile.TemporaryDirectory() as folder:
            directory=Path(folder)
            (directory/'stderr.log').write_bytes(b'panicked at\nstack backtrace:')
            (directory/'phases.jsonl').write_text('')
            child=types.SimpleNamespace(proc=types.SimpleNamespace(pid=12,returncode=-signal.SIGABRT,poll=lambda:-signal.SIGABRT),ticks=34,overflow=False)
            evidence=driver.exit_evidence(child,directory,directory/'phases.jsonl')
            self.assertTrue(evidence['native_abort'])
            self.assertTrue(evidence['panic_observed'])
            self.assertFalse(evidence['root_cause_established'])
            child.proc.poll=lambda:-9
            self.assertFalse(driver.exit_evidence(child,directory,directory/'phases.jsonl')['native_abort'])

    def test_subscriber_phase_integrity_is_distinct(self):
        events=[('cleanup','begin')]
        for phase in ('subscription_cancel_gather','room_disconnect','result_commit','reference_clear'):
            events.extend(((phase,'begin'),(phase,'end')))
        events.extend((('subscriber_run_return','end'),('gc_weakref_retirement','begin'),
                       ('gc_weakref_retirement','end'),('subscriber_retirement_return','end'),
                       ('asyncio_run_return','end'),('python_atexit','begin'),
                       ('native_ffi_dispose','begin'),('native_ffi_dispose','end'),('integrity','end')))
        records=[dict(phase=p,kind=k,pid=12,tid=22,monotonic_ns=i+1,sequence=i+1,dropped=0,success=True)
                 for i,(p,k) in enumerate(events)]
        self.assertTrue(driver.phases_complete(records,12,subscriber=True))
        self.assertFalse(driver.phases_complete(records,12))
        self.assertFalse(driver.phases_complete(records[:-1],12,subscriber=True))

    def test_frozen_plan_hash_precedes_parsing(self):
        self.assertEqual(driver.failure_code(ValueError('phase_integrity_failed')),'phase_integrity_failed')
        self.assertEqual(driver.failure_code(ValueError('secret credential')),'measurement_failed')
        self.assertEqual(driver.failure_code(RuntimeError('phase_integrity_failed')),'measurement_failed')
        with self.assertRaisesRegex(ValueError,'plan_hash_mismatch'): driver.load_plan(PATH,'0'*64)

if __name__=='__main__': unittest.main()
