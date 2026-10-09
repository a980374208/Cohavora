"""Offline high-resolution target, credential lifetime and exact-owner checks."""
from __future__ import annotations

import ast
import hashlib
import json
from pathlib import Path
import signal
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

TOOLS = Path(__file__).resolve().parents[1] / 'tools' / 'meeting'
sys.path.insert(0,str(TOOLS))
try:
    import b11_highres_run as runner
finally:
    sys.path.remove(str(TOOLS))


def session():
    task_id='a'*32
    return {'identity':{'host':'43.138.244.33','user':'ubuntu','config_path':'fixture'},
        'target':{'instance':'ins-fixture','service_url':'ws://43.138.244.33:17880',
            'local_service_url':'http://127.0.0.1:17880','config_path':'/home/ubuntu/livekit.yml',
            'sfu_container':'livekit'},
        'task':{'task_id':task_id,'room':'b11-highres-'+task_id},
        'remote_directory':'/tmp/b11-highres-'+task_id,
        'target_identity':'b11highres_fixture_source','expected_remote':None}


def credential_values():
    role={'LIVEKIT_URL':'ws://43.138.244.33:17880','LIVEKIT_SOAK_ALLOW_INSECURE':'1',
        'LIVEKIT_SOAK_TOKEN':'eyJmaXh0dXJl.eyJmaXh0dXJl.c2lnbmF0dXJl'}
    return {'publisher':dict(role),'observer':dict(role)}


class HighresRunTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='b11-highres-run-selftest-')
        self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)

    def test_remote_program_compiles_and_never_interpolates_credentials(self):
        request=session()
        for operation in ('prepare','remove_credentials','background','snapshot','room_delete','cleanup'):
            background={'cli_path':'/home/ubuntu/.local/bin/lk','cli_sha256':'b'*64} if operation=='background' else None
            command=runner.remote_command(operation,request,background=background).decode()
            program=command.split('\n',1)[1].rsplit('\nB11_HIGHRES_PY\n',1)[0]
            ast.parse(program)
            self.assertNotIn(credential_values()['publisher']['LIVEKIT_SOAK_TOKEN'],command)
            self.assertNotIn('--token-only',command)
            # Credentials are written into the short-lived transfer file. The only
            # stdout statement emits the safe response rather than that file data.
            prints=[node for node in ast.walk(ast.parse(program)) if isinstance(node,ast.Call)
                and isinstance(node.func,ast.Name) and node.func.id=='print']
            self.assertEqual(len(prints),1)
            self.assertEqual(ast.unparse(prints[0].args[0]),'json.dumps(response)')

    def test_remote_target_and_task_binding_reject_drift_before_ssh(self):
        for mutate in (lambda value:value['identity'].update(host='81.71.85.246'),
                       lambda value:value['identity'].update(user='root'),
                       lambda value:value.update(remote_directory='/tmp/b11-highres-foreign'),
                       lambda value:value['target'].update(service_url='ws://43.138.244.33:7880'),
                       lambda value:value.update(target_identity='foreign')):
            value=session()
            mutate(value)
            with self.subTest(value=value),patch.object(runner.remote,'execute') as execute:
                with self.assertRaises(ValueError):
                    runner.remote_command('prepare',value)
                execute.assert_not_called()

    def test_background_fills_exactly_fourteen_of_fifteen_remote_grid_seats(self):
        command = runner.remote_command('background', session(), background={
            'cli_path':'/home/ubuntu/.local/bin/lk','cli_sha256':'b'*64}).decode()
        program = command.split('\n', 1)[1].rsplit('\nB11_HIGHRES_PY\n', 1)[0]
        payload = next(ast.literal_eval(node.value) for node in ast.parse(program).body
            if isinstance(node, ast.Assign) and any(isinstance(target, ast.Name)
                and target.id == 'PAYLOAD' for target in node.targets))
        argv = payload['background']['argv']
        self.assertEqual(argv[argv.index('--video-publishers') + 1], '14')
        self.assertEqual(argv[argv.index('--subscribers') + 1], '0')
        self.assertEqual(argv[argv.index('--video-resolution') + 1], 'low')
        self.assertEqual(argv[argv.index('--video-codec') + 1], 'vp8')

    def test_prepare_credentials_removes_local_and_remote_file_before_return(self):
        received=[]
        def download(config,path,local):
            received.append(local)
            local.write_text(json.dumps(credential_values()),encoding='utf-8')
        observed={'config_sha256':'b'*64,'sfu_image':'sha256:'+'c'*64}
        operations=[]
        def call(operation,value,**kwargs):
            operations.append(operation)
            return {'ok':True,'remote_inputs':observed} if operation=='prepare' else {'ok':True,'credential_removed':True}
        with patch.object(runner.remote,'snapshot_identity',return_value={'host':'43.138.244.33','user':'ubuntu'}), \
                patch.object(runner.shared,'_pinned_config',return_value={}), \
                patch.object(runner.remote,'download',side_effect=download),patch.object(runner,'call',side_effect=call):
            value=runner.prepare_credentials(self.root/'ssh.json',session()['target'],output=self.root/'prepared',
                task_id='a'*32,target_identity='b11highres_fixture_source')
        self.assertEqual(operations,['prepare','remove_credentials'])
        self.assertEqual(value['credentials'],credential_values())
        self.assertTrue(received)
        self.assertTrue(all(not path.exists() and not path.parent.exists() for path in received))
        persisted=(self.root/'prepared'/'remote-prepared.json').read_text()
        self.assertNotIn('LIVEKIT_SOAK_TOKEN',persisted)
        self.assertNotIn(credential_values()['publisher']['LIVEKIT_SOAK_TOKEN'],persisted)

    def test_invalid_download_still_deletes_credentials_and_attempts_cleanup(self):
        received=[]
        def download(config,path,local):
            received.append(local)
            local.write_text('{"publisher":{"LIVEKIT_SOAK_TOKEN":"bad"}}')
        observed={'config_sha256':'b'*64,'sfu_image':'sha256:'+'c'*64}
        operations=[]
        def call(operation,value,**kwargs):
            operations.append(operation)
            return {'ok':True,'remote_inputs':observed} if operation=='prepare' else \
                {'ok':True,'credential_removed':True,'all_stopped':True,'room_removed':True}
        with patch.object(runner.remote,'snapshot_identity',return_value={'host':'43.138.244.33','user':'ubuntu'}), \
                patch.object(runner.shared,'_pinned_config',return_value={}), \
                patch.object(runner.remote,'download',side_effect=download),patch.object(runner,'call',side_effect=call):
            with self.assertRaisesRegex(ValueError,'invalid_highres_credentials'):
                runner.prepare_credentials(self.root/'ssh.json',session()['target'],output=self.root/'prepared',task_id='a'*32)
        self.assertEqual(operations,['prepare','remove_credentials','cleanup'])
        self.assertTrue(all(not path.exists() for path in received))

    def test_native_credentials_use_child_environment_and_fixed_stop_ready_paths(self):
        value=session()
        value['credentials']=credential_values()
        conflicting={'LIVEKIT_TEST_CAPTURE_BACKEND':'gdi-window','LIVEKIT_TEST_WGC_SCREEN':'1',
            'LIVEKIT_TEST_GDI_WINDOW':'1','LIVEKIT_TEST_FIRST_FRAME_ONLY':'1'}
        with patch.dict(runner.os.environ,conflicting):
            before=dict(runner.os.environ)
            environment=runner.native_publisher_environment(value,scenario='screen',
                ready_file=self.root/'ready.json',stop_file=self.root/'stop.flag')
            self.assertEqual(dict(runner.os.environ),before)
        self.assertEqual(environment['LIVEKIT_TOKEN'],value['credentials']['publisher']['LIVEKIT_SOAK_TOKEN'])
        self.assertEqual(environment['POLICY_SOURCE_KIND'],'screen')
        self.assertEqual(environment['LIVEKIT_TEST_CAPTURE_BACKEND'],'wgc-window')
        for name in ('LIVEKIT_TEST_WGC_SCREEN','LIVEKIT_TEST_GDI_WINDOW','LIVEKIT_TEST_FIRST_FRAME_ONLY'):
            self.assertNotIn(name,environment)
        self.assertEqual(environment['POLICY_SOURCE_READY_FILE'],str((self.root/'ready.json').resolve()))
        self.assertEqual(environment['POLICY_SOURCE_STOP_FILE'],str((self.root/'stop.flag').resolve()))

    def test_four_k_child_has_native_wgc_dimensions_and_task_scope(self):
        value=session()
        value['credentials']=credential_values()
        for scenario,kind in (('camera4k','camera'),('screen4k','screen')):
            environment=runner.native_publisher_environment(value,scenario=scenario,
                ready_file=self.root/'ready.json',stop_file=self.root/'stop.flag')
            self.assertEqual(environment['POLICY_SOURCE_KIND'],kind)
            self.assertEqual(environment['POLICY_SOURCE_CAPTURE'],'wgc_window')
            self.assertEqual(environment['POLICY_SOURCE_WIDTH'],'3840')
            self.assertEqual(environment['POLICY_SOURCE_HEIGHT'],'2160')
            self.assertEqual(environment['LIVEKIT_TEST_CAPTURE_BACKEND'],'wgc-window')
            request=session()
            request['scope']=runner.freeze.scenario_scope(scenario)
            command=runner.remote_command('prepare',request).decode()
            self.assertIn("SCOPE = 'B11_4K_OWNED_WINDOW_LAYER_GPU_DIAGNOSTIC_V1'",command)
        request=session()
        request['scope']='wrong_scope'
        with self.assertRaisesRegex(ValueError,'invalid_highres_task_scope'):
            runner.remote_command('prepare',request)

    def test_owner_uid_namespace_or_argv_drift_never_receives_signal(self):
        owner={'pid':123,'start_ticks':'55','argv':['lk','load-test'],'uid':1000,'namespace_inode':42}
        for field,value in (('uid',0),('namespace_inode',43),('argv',['lk','foreign'])):
            with self.subTest(field=field),patch.object(runner,'process_identity',return_value={**owner,field:value}), \
                    patch.object(runner.os,'kill') as kill:
                result=runner.stop_owned(owner)
            kill.assert_not_called()
            self.assertFalse(result['stopped'])
        with patch.object(runner,'process_identity',return_value={**owner,'start_ticks':'56'}), \
                patch.object(runner.os,'kill') as kill:
            self.assertTrue(runner.stop_owned(owner)['stopped'])
        kill.assert_not_called()

    def test_publisher_probe_export_excludes_unstructured_output_and_removes_raw(self):
        raw,output=self.root/'raw.tmp',self.root/'probes.jsonl'
        raw.write_text('unstructured native detail\nSOURCE_PROBE {"event":"sample","captured_frames":30}\n')
        runner._export_publisher_probes(raw,output)
        self.assertFalse(raw.exists())
        self.assertEqual(json.loads(output.read_text()),{'event':'sample','captured_frames':30})


if __name__=='__main__':
    unittest.main()
