"""Two distinct product Coordinator processes: file upload, cancellation, retry."""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import time
import uuid

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'screen_capture'))
import invoke_screen_share_quality_probe as service
from run_b_acceptance_recovery import snapshot


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def events(root):
    path=root/'events.jsonl'
    if not path.exists(): return []
    rows=[]
    for line in path.read_text(encoding='utf-8').splitlines():
        try: rows.append(json.loads(line))
        except json.JSONDecodeError: pass  # a writer may still be appending
    return rows


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--input-file',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=False)
    binary=args.binary.resolve()
    verifier=Path(__file__).resolve().parents[1]/'diagnostics/verify_runtime_binary.ps1'
    checked=subprocess.run(['pwsh','-NoProfile','-File',str(verifier),'-Executable',str(binary),
        '-ExpectedExecutableName','uia_entry_fixture.exe','-Configuration','RelWithDebInfo'],
        capture_output=True,text=True,creationflags=subprocess.CREATE_NO_WINDOW)
    if checked.returncode: parser.exit(2,'RelWithDebInfo verification failed\n')
    identity=json.loads(checked.stdout)
    service.SERVER_PORT=17980
    service.SERVER_CONTAINER='cohavora-b-acceptance-20261001'
    service.CONFIG_PATH='/root/livekit-b-acceptance-20261001/livekit.yaml'
    room='b-files-'+uuid.uuid4().hex
    inputs=('src/core/meeting_coordinator.cpp','src/core/meeting_session_runtime.h',
        'src/core/room.cpp','tests/uia/entry_fixture.cpp','tests/runtime/probes/b_file_product_runtime.h')
    result=dict(status='RUNNING',room=room,configuration='RelWithDebInfo',binary_identity=identity,
        started_utc=datetime.now(timezone.utc).isoformat(),runner_sha256=sha(Path(__file__)),
        head=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
        inputs={p:sha(Path(p)) for p in inputs},cases={},
        scope='Two separate native product Coordinator processes; local Qt owner and session strand; real ECS reliable data transport. No physical durability assertion.')
    manifest=args.output/'result.json'
    def save(): manifest.write_text(json.dumps(result,indent=2),encoding='utf-8')
    save()
    processes={}
    roots={name:args.output/name for name in ('publisher','receiver')}
    sequences=dict.fromkeys(roots,0)
    def send(peer,action,**fields):
        sequences[peer]+=1
        row=dict(sequence=sequences[peer],action=action,**fields)
        temp=roots[peer]/'control.tmp'
        temp.write_text(json.dumps(row),encoding='utf-8')
        os.replace(temp,roots[peer]/'control.json')
        return sequences[peer]
    def wait(name,predicate,description,seconds=30):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            rows=events(roots[name])
            found=predicate(rows)
            if found: return found
            if processes[name].poll() is not None:
                raise RuntimeError(description+':process_exited')
            if any(x['event'] in ('coordinator_error','fixture_timeout','payload_path_rejected',
                                  'payload_unreadable','snapshot_failed','unknown_command') for x in rows):
                raise RuntimeError(description+':native_failure')
            time.sleep(.1)
        raise RuntimeError(description+':timeout')
    def observed(event,**fields):
        return lambda rows: next((x for x in rows if x['event']==event and all(x.get(k)==v for k,v in fields.items())),None)
    def counts(name):
        sequence=send(name,'snapshot')
        value=wait(name,observed('file_state',sequence=sequence),'file_state')
        if any(value[k]!=0 for k in ('send_queue','inbound_pending','assemblies')):
            raise RuntimeError('file_state_not_clear:'+name)
        return value
    try:
        result['server_before']=snapshot(room)
        if result['server_before']['health_status']!=200 or result['server_before']['room'].get('enabled_codecs'):
            raise RuntimeError('file_case_requires_healthy_normal_codec_service')
        auth=service.credentials(room)
        payloads=args.output/'payloads'; payloads.mkdir()
        initial=payloads/'initial.jpg'; shutil.copyfile(args.input_file,initial)
        retry=payloads/'retry.dat'; retry.write_bytes(random.Random(0xBA02).randbytes(2*1024*1024))
        result['payloads']={p.name:dict(bytes=p.stat().st_size,sha256=sha(p)) for p in (initial,retry)}
        for name,root in roots.items():
            root.mkdir()
            env={k:v for k,v in os.environ.items() if not k.startswith(('LIVEKIT_TEST_','E2EE_','B_FILE_'))}
            env.update(B_FILE_ROOT=str(root.resolve()),B_FILE_IDENTITY=name,B_FILE_ROOM=room,
                LIVEKIT_URL='ws://123.56.225.164:17980',LIVEKIT_TOKEN=auth[name],
                LIVEKIT_TEST_ALLOW_INSECURE='1',QT_QPA_PLATFORM='windows')
            processes[name]=subprocess.Popen([str(binary),'--b-file-runtime'],env=env,
                stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,stdin=subprocess.DEVNULL,
                creationflags=subprocess.CREATE_NO_WINDOW)
            wait(name,observed('started'),'started')
        # The actual product rejects the original nonempty bytes before admission.
        send('publisher','send',message='upload-offline',name='upload-offline.jpg',path=str(initial.resolve()))
        wait('publisher',observed('send_failed',message='upload-offline'),'offline_rejection')
        result['cases']['offline-upload-rejection']=dict(status='PASS',state=counts('publisher'))
        for name in ('receiver','publisher'):
            send(name,'join'); wait(name,observed('joined',join=1),'first_join',45)
        send('publisher','send',message='upload-initial',name='upload-initial.jpg',path=str(initial.resolve()))
        received=wait('receiver',observed('receive_completed',name='upload-initial.jpg'),'initial_upload')
        wait('publisher',observed('send_success',message='upload-initial'),'initial_sender_success')
        target=roots['receiver']/'received/upload-initial.jpg'
        if not received['saved'] or received['sha256']!=sha(initial) or sha(target)!=sha(initial):
            raise RuntimeError('initial_content_mismatch')
        result['cases']['remote-upload-readback']=dict(status='PASS',received=received,
            publisher=counts('publisher'),receiver=counts('receiver'))
        save()
        send('publisher','send',message='upload-interrupted',name='upload-interrupted.dat',path=str(retry.resolve()))
        partial=wait('receiver',lambda rows:next((x for x in rows if x['event']=='receive_progress' and
            x['name']=='upload-interrupted.dat' and 0<x['percent']<100),None),'actual_partial_reception')
        send('publisher','leave')
        wait('publisher',observed('left'),'interrupted_sender_left')
        cancelled=wait('publisher',observed('send_failed',message='upload-interrupted'),'actual_send_failure')
        remote_failure=wait('receiver',observed('receive_failed',name='upload-interrupted.dat'),'actual_receive_failure')
        result['cases']['inflight-upload-failure']=dict(status='PASS',partial=partial,
            sender_failure=cancelled,receiver_failure=remote_failure,
            publisher=counts('publisher'),receiver=counts('receiver'))
        save()
        send('publisher','join')
        wait('publisher',observed('joined',join=2),'sender_readmission',45)
        send('publisher','send',message='upload-retry',name='upload-retry.dat',path=str(retry.resolve()))
        received=wait('receiver',observed('receive_completed',name='upload-retry.dat'),'retry_upload',45)
        wait('publisher',observed('send_success',message='upload-retry'),'retry_sender_success')
        target=roots['receiver']/'received/upload-retry.dat'
        if not received['saved'] or received['sha256']!=sha(retry) or sha(target)!=sha(retry):
            raise RuntimeError('retry_content_mismatch')
        if any(x['event']=='receive_completed' and x['name']=='upload-interrupted.dat' for x in events(roots['receiver'])):
            raise RuntimeError('cancelled_transfer_completed_late')
        result['cases']['upload-retry-recovery']=dict(status='PASS',received=received,
            publisher=counts('publisher'),receiver=counts('receiver'),old_transfer_never_completed=True)
        result['lifecycle']={}
        for name in roots:
            old_left=sum(x['event']=='left' for x in events(roots[name]))
            send(name,'leave')
            wait(name,lambda rows:sum(x['event']=='left' for x in rows)>old_left,'final_leave')
            counts(name)
            send(name,'finish')
            retired=wait(name,observed('retired'),'retirement')
            if retired['rooms_observed']<1 or retired['rooms_alive'] or retired['cleanup_pending']:
                raise RuntimeError('native_objects_retained:'+name)
            processes[name].wait(timeout=10)
            if processes[name].returncode: raise RuntimeError('native_exit_failed:'+name)
            result['lifecycle'][name]=dict(retirement=retired,exit_code=processes[name].returncode)
        result['server_after']=snapshot(room)
        if result['server_after']['participants_remaining'] or result['server_after']['publications_remaining']:
            raise RuntimeError('room_not_empty')
        if any(result['server_before'][k]!=result['server_after'][k] for k in ('config_sha256','image','binary')):
            raise RuntimeError('service_changed')
        if sha(binary)!=identity['binary_sha256']: raise RuntimeError('binary_changed')
        if sha(Path(__file__))!=result['runner_sha256']: raise RuntimeError('runner_changed')
        result['status']='PASS'
    except Exception as error:
        result.update(status='FAIL',failure=type(error).__name__+':'+str(error))
    finally:
        for child in processes.values():
            if child.poll() is None: child.kill(); child.wait(timeout=10)
        result['finished_utc']=datetime.now(timezone.utc).isoformat()
        result['events_sha256']={name:sha(root/'events.jsonl') for name,root in roots.items() if (root/'events.jsonl').exists()}
        save()
    print(result['status'],flush=True)
    return 0 if result['status']=='PASS' else 1


if __name__=='__main__':
    raise SystemExit(main())
