"""Frozen collector startup evidence and ordinary/formal admission boundaries."""
import asyncio
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
from types import SimpleNamespace as NS
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools/product_acceptance'))
from product_pilot_scheduler import (SCHEDULER_POLICY, load_scheduler_policy,
    validate_scheduler_policy, expected_realtime_restriction, scheduler_policy_readback,
    validate_scheduler_readback, review_scheduler_policy)
from product_pilot_load import validate_ready, LOAD, identities, LoadProcess, commit
from product_pilot_remote import run
from release_product_acceptance import frozen_scheduler_release

PROFILE=Path(__file__).resolve().parents[1]/'tools/product_acceptance/product_pilot_scheduler_policy.json'


class FrozenScheduler(unittest.TestCase):
    def setUp(self):
        self.policy=load_scheduler_policy(PROFILE)
        self.proof=dict(policy_sha256=self.policy['sha256'], stage='pre_sdk_import',
            scheduler=dict(nice=0,policy=0), restriction=expected_realtime_restriction())
        self.ready=dict(schema=1, run_id='a'*32, state='READY', pid=10,
            cgroup_sha256='b'*64, identities=identities('a'*32), load=LOAD, sdk='fixture',
            publisher_scheduler_policy=deepcopy(self.proof))
        self.plan=dict(run_id='a'*32,collector_scheduler_policy=self.policy)
        self.remote=[dict(run_id='a'*32,event='receiver.scheduler_policy',proof=self.proof),
            dict(run_id='a'*32,event='load.ready',publisher_process=self.ready)]
        self.route=dict(scheduler_policy=self.policy)

    def test_frozen_file_is_hashed_as_executed_bytes(self):
        self.assertEqual(self.policy['sha256'],hashlib.sha256(PROFILE.read_bytes()).hexdigest())
        self.assertEqual(self.policy['policy'],SCHEDULER_POLICY)
        self.assertIsNone(load_scheduler_policy(None))

    def test_diagnostic_or_boosted_entry_cannot_use_ordinary_profile(self):
        for diagnostic,nice in ((True,0),(False,-5),(False,False)):
            with self.subTest(diagnostic=diagnostic,nice=nice),self.assertRaises(ValueError):
                load_scheduler_policy(PROFILE,diagnostic,nice)

    def test_missing_unknown_relaxed_and_bool_numeric_policy_fail_closed(self):
        for mutate in (lambda p:p.pop('rtprio_limit'),lambda p:p.update(extra=True),
                lambda p:p.update(rtprio_limit=[0,1]),lambda p:p.update(schema=True),
                lambda p:p['scheduler'].update(policy=False),lambda p:p.update(diagnostic_only=True)):
            policy=deepcopy(SCHEDULER_POLICY);mutate(policy)
            with self.assertRaises(ValueError):validate_scheduler_policy(policy)

    def test_pre_sdk_readback_requires_actual_limits_caps_and_baseline_priority(self):
        with patch('product_pilot_scheduler.process_scheduler',return_value=dict(nice=0,policy=0)), \
                patch('product_pilot_scheduler.realtime_restriction_state',return_value=expected_realtime_restriction()):
            self.assertEqual(scheduler_policy_readback(self.policy),self.proof)
        for scheduler in (dict(nice=-5,policy=0),dict(nice=0,policy=1),dict(nice=False,policy=0)):
            with patch('product_pilot_scheduler.process_scheduler',return_value=scheduler),self.assertRaises(ValueError):
                scheduler_policy_readback(self.policy)

    def test_receiver_unapplied_policy_fails_before_sdk_or_media_is_imported(self):
        args=NS(diagnostic_receiver_nice=0,timing_diagnostic=False,diagnostic_no_realtime=False,scheduler_policy=PROFILE)
        with patch('product_pilot_scheduler.process_scheduler',return_value=dict(nice=0,policy=1)), \
                patch('product_pilot_remote.create_video_counter',side_effect=AssertionError('media_started')), \
                self.assertRaisesRegex(ValueError,'not_applied'):
            asyncio.run(run(args))

    def test_readback_rejects_wrong_hash_missing_caps_and_bool_limit(self):
        for mutate in (lambda p:p.update(policy_sha256='b'*64),lambda p:p['restriction'].pop('cap_sys_resource_bounding'),
                lambda p:p['restriction'].update(rtprio_limit=[False,0]),lambda p:p.update(stage='post_sdk_import')):
            proof=deepcopy(self.proof);mutate(proof)
            with self.assertRaises(ValueError):validate_scheduler_readback(proof,self.policy)

    def test_publisher_ready_requires_plan_bound_readback(self):
        self.assertEqual(validate_ready(self.ready,'a'*32,10,'b'*64,self.policy),self.ready)
        for ready in (dict(self.ready,publisher_scheduler_policy=None),
                {k:v for k,v in self.ready.items() if k!='publisher_scheduler_policy'}):
            with self.assertRaises((ValueError,TypeError)):validate_ready(ready,'a'*32,10,'b'*64,self.policy)

    def test_independent_streaming_review_requires_both_processes_once(self):
        self.assertTrue(review_scheduler_policy(self.plan,iter(self.remote),self.route)['passed'])
        for rows in ([],self.remote[:1],self.remote[1:],self.remote+self.remote[:1]):
            self.assertFalse(review_scheduler_policy(self.plan,iter(rows),self.route)['passed'])

    def test_wrong_run_and_route_policy_cannot_pass_independent_review(self):
        rows=deepcopy(self.remote);rows[1]['run_id']='b'*32
        self.assertFalse(review_scheduler_policy(self.plan,iter(rows),self.route)['passed'])
        route=deepcopy(self.route);route['scheduler_policy']['policy']['schema']=True
        self.assertFalse(review_scheduler_policy(self.plan,iter(self.remote),route)['passed'])

    def test_release_requires_executed_profile_snapshot_and_complete_evidence(self):
        external=dict(final_checks=dict(collector_scheduler_policy_complete=True),
            collector_scheduler_policy=dict(passed=True,policy_sha256=self.policy['sha256']))
        inputs={str(PROFILE):self.policy['sha256']}
        with TemporaryDirectory() as directory:
            root=Path(directory);(root/'collector-scheduler-policy.json').write_bytes(PROFILE.read_bytes())
            self.assertEqual(frozen_scheduler_release(root,self.plan,external,inputs),self.policy)
            for bad_external,bad_inputs in (({},inputs),(external,{}),
                    (dict(external,final_checks=dict(collector_scheduler_policy_complete=False)),inputs)):
                with self.assertRaises(ValueError):frozen_scheduler_release(root,self.plan,bad_external,bad_inputs)
            (root/'collector-scheduler-policy.json').write_text(json.dumps(SCHEDULER_POLICY))
            with self.assertRaises(ValueError):frozen_scheduler_release(root,self.plan,external,inputs)


class FrozenPublisher(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        FrozenScheduler.setUp(self)

    async def test_publisher_inherits_profile_without_diagnostic_hooks(self):
        with TemporaryDirectory() as directory:
            args=NS(output=Path(directory),dependencies=Path('deps'),config=Path('config.yml'),
                run_id='a'*32,room='000000238',url='ws://127.0.0.1:17880',seconds=900,scheduler_policy=PROFILE)
            load=LoadProcess(args)
            async def spawn(*argv,**kwargs):
                self.assertEqual(argv[argv.index('--scheduler-policy')+1],str(PROFILE))
                self.assertNotIn('--scheduler-diagnostic',argv)
                self.assertNotIn('--diagnostic-no-realtime',argv)
                self.assertNotIn('preexec_fn',kwargs)
                commit(load.root/'ready.json',self.ready)
                return NS(pid=10,returncode=None)
            try:
                with patch('product_pilot_load.asyncio.create_subprocess_exec',side_effect=spawn), \
                        patch('product_pilot_load.cgroup_fingerprint',return_value='b'*64):
                    self.assertEqual((await load.start())['publisher_scheduler_policy'],self.proof)
            finally:
                if load.log:load.log.close()


if __name__=='__main__':unittest.main()
