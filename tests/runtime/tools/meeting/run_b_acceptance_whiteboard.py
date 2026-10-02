"""B12: three separate product processes over the task-owned real SFU.

Cross-domain dependencies: product_acceptance file peer fixture and service
snapshot; screen_capture's authenticated Workbench transport. Tokens stay in
memory/child environments. The eight case results remain independent.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'screen_capture'))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'product_acceptance'))
import invoke_screen_share_quality_probe as service
from run_b_acceptance_recovery import snapshot
from run_b_acceptance_files import events, sha

ROLES = ('publisher', 'receiver', 'late')


def prepare_room(room):
    return service.remote_python('ROOM=' + repr(room) + '\nPORT=' + str(service.SERVER_PORT) + '\n' + service.REMOTE_AUTH + '''
import urllib.request
metadata=json.dumps(dict(detail=dict(info=dict(systemGenerated=dict(meetingID=ROOM,
    creatorUserID='publisher',startTime=int(time.time())),creatorDefinedMeeting=dict(
    title='B12 task-owned board',hostUserID='publisher',meetingDuration=3600)),
    setting=dict(disableCameraOnJoin=True,disableMicrophoneOnJoin=True))))
request=urllib.request.Request('http://127.0.0.1:'+str(PORT)+'/twirp/livekit.RoomService/CreateRoom',
    data=json.dumps(dict(name=ROOM,metadata=metadata)).encode(),headers={
    'Authorization':'Bearer '+token('b12-admin',dict(roomCreate=True)), 'Content-Type':'application/json'})
created=json.load(urllib.request.urlopen(request,timeout=5))
assert created['name']==ROOM and created['metadata']==metadata
print(json.dumps(dict(metadata_sha256=hashlib.sha256(metadata.encode()).hexdigest(),room_sid=created['sid'])))
''')


class Peers:
    def __init__(self, root, binary, room, auth):
        self.root, self.binary, self.room, self.auth = root, binary, room, auth
        self.roots = {role: root / role for role in ROLES}
        self.sequences = dict.fromkeys(ROLES, 0)
        self.processes = {}
        self.lifecycle = {}

    def launch(self, role):
        root = self.roots[role]
        root.mkdir()
        env = {k: v for k, v in os.environ.items() if not k.startswith(('LIVEKIT_TEST_', 'E2EE_', 'B_FILE_', 'B_BOARD_'))}
        env.update(B_FILE_ROOT=str(root.resolve()), B_FILE_IDENTITY=role, B_FILE_ROOM=self.room,
            B_BOARD_MODE='1', LIVEKIT_URL='ws://123.56.225.164:17980', LIVEKIT_TOKEN=self.auth[role],
            LIVEKIT_TEST_ALLOW_INSECURE='1', QT_QPA_PLATFORM='windows')
        child = subprocess.Popen([str(self.binary), '--b-file-runtime'], env=env,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL,
            creationflags=subprocess.CREATE_NO_WINDOW)
        self.processes[role] = child
        started = self.wait(role, lambda rows: next((x for x in rows if x['event']=='started'), None), 'started')
        self.lifecycle[role] = dict(pid=child.pid, started=started)
        self.send(role, 'join')
        self.wait(role, lambda rows: next((x for x in rows if x['event']=='joined'), None), 'joined', 45)

    def send(self, role, action, *, ack=True, **fields):
        self.sequences[role] += 1
        row = dict(sequence=self.sequences[role], action=action, **fields)
        root = self.roots[role]
        temporary = root / 'control.tmp'
        temporary.write_text(json.dumps(row), encoding='utf-8')
        os.replace(temporary, root / 'control.json')
        if ack:
            self.wait(role, lambda rows: next((x for x in rows if x['event']=='command'
                and x['sequence']==row['sequence']), None), 'command_admitted')
        return row['sequence']

    def wait(self, role, predicate, description, seconds=30):
        end = time.monotonic()+seconds
        while time.monotonic()<end:
            rows = events(self.roots[role])
            if any(x['event'] in ('coordinator_error','fixture_timeout','board_command_failed',
                                 'snapshot_failed','unknown_command') for x in rows):
                raise RuntimeError(role+':'+description+':native_failure')
            found = predicate(rows)
            if found: return found
            if self.processes[role].poll() is not None:
                raise RuntimeError(role+':'+description+':process_exited')
            time.sleep(.1)
        raise RuntimeError(role+':'+description+':timeout')

    def observe(self, role):
        sequence = self.send(role, 'board_observe')
        return self.wait(role, lambda rows: next((x for x in rows if x['event']=='board_projection'
            and x['command_sequence']==sequence), None), 'observation')

    def convergence(self, roles, objects=(), asset=None, state=1):
        end = time.monotonic()+30
        while time.monotonic()<end:
            values = {role: self.observe(role) for role in roles}
            if all(x.get('state')==state and bool(x.get('can_edit'))==(state==1) for x in values.values()):
                hashes = {x['document_sha256'] for x in values.values()}
                sequences = {x['sequence'] for x in values.values()}
                ids = [{o['id'] for page in x['document']['pages'] for o in page['objects']} for x in values.values()]
                assets_ok = not asset or all(x['asset_hashes'].get(asset)==asset and
                    any(p['backgroundAssetId']==asset for p in x['document']['pages']) for x in values.values())
                if len(hashes)==len(sequences)==1 and all(set(objects)<=x for x in ids) and assets_ok:
                    return values
            time.sleep(.15)
        raise RuntimeError('three_process_document_sequence_asset_convergence_timeout')

    def finish(self, role, already_left=False):
        if not already_left:
            self.send(role,'leave')
            self.wait(role,lambda rows: next((x for x in rows if x['event']=='left'),None),'leave')
        self.send(role,'finish')
        retired = self.wait(role,lambda rows: next((x for x in rows if x['event']=='retired'),None),'retirement')
        child = self.processes[role]
        child.wait(timeout=10)
        if child.returncode or retired['rooms_observed']!=1 or retired['rooms_alive'] or retired['cleanup_pending']:
            raise RuntimeError(role+':native_retirement_failed')
        self.lifecycle[role].update(retirement=retired,exit_code=child.returncode)

    def cleanup(self):
        for role,child in self.processes.items():
            if child.poll() is None:
                child.kill(); child.wait(timeout=10)


def run_room(root, binary, disconnect=False):
    root.mkdir()
    room = 'b12-'+uuid.uuid4().hex
    result = dict(status='RUNNING',room=room,cases={},started_utc=datetime.now(timezone.utc).isoformat())
    manifest = root/'result.json'
    def save(): manifest.write_text(json.dumps(result,indent=2),encoding='utf-8')
    def passed(name, observations, **fields):
        result['cases'][name] = dict(status='PASS',observations=observations,**fields)
        save()
    save()
    peers = None
    try:
        result['server_before']=snapshot(room)
        if result['server_before']['room'].get('enabled_codecs'):
            raise RuntimeError('requires_normal_codec_service')
        result['room_preparation']=prepare_room(room)
        auth=service.credentials(room)
        payloads=root/'payloads'; payloads.mkdir()
        image=Image.new('RGB',(320,180))
        image.putdata([((x*3+y)%256,(x+y*5)%256,(x*7+y*11)%256) for y in range(180) for x in range(320)])
        input_file=payloads/'board-input.png'; image.save(input_file)
        asset=sha(input_file)
        result['input_png']=dict(sha256=asset,width=320,height=180)
        peers=Peers(root,binary,room,auth)
        peers.launch('publisher'); peers.launch('receiver')
        peers.convergence(ROLES[:2])
        at_ms=int(time.time()*1000)+500
        sequences={}
        for role,x,color in [('publisher',20,0xff4411),('receiver',90,0x22aa44)]:
            sequences[role]=peers.send(role,'board_add',id=role+'-simultaneous',x=x,color=color,at_ms=at_ms,ack=False)
        witnesses={role:peers.wait(role,lambda rows,seq=seq: next((x for x in rows if x['event']=='board_proposed'
            and x['sequence']==seq),None),'simultaneous_proposal') for role,seq in sequences.items()}
        skew=abs(witnesses['publisher']['utc_ms']-witnesses['receiver']['utc_ms'])
        if skew>100: raise RuntimeError('simultaneous_submission_skew_exceeds_100ms')
        initial_ids=['object-publisher-simultaneous','object-receiver-simultaneous']
        values=peers.convergence(ROLES[:2],objects=initial_ids)
        if not disconnect:
            passed('simultaneous-write',values,submission_witnesses=witnesses,submission_skew_ms=skew)
            if not values['publisher']['can_admin'] or values['receiver']['can_admin']:
                raise RuntimeError('authority_permission_projection_invalid')
            peers.send('publisher','board_image',path=str(input_file.resolve()),page='b12-image-page')
            values=peers.convergence(ROLES[:2],objects=initial_ids,asset=asset)
            passed('image-import',values)
        peers.launch('late')
        if len({p.pid for p in peers.processes.values()})!=3 or any(p.poll() is not None for p in peers.processes.values()):
            raise RuntimeError('three_distinct_live_processes_required')
        values=peers.convergence(ROLES,objects=initial_ids,asset=asset if not disconnect else None)
        if not disconnect:
            passed('late-join',values,existing_image_page_and_asset_recovered=True)
            peers.send('late','board_add',id='image-annotation',x=100,color=0x22aa44)
            ids=initial_ids+['object-image-annotation']
            values=peers.convergence(ROLES,objects=ids,asset=asset)
            passed('sync',values)
            sequence=peers.send('receiver','board_reconnect')
            witness=peers.wait('receiver',lambda rows: next((x for x in rows if x['event']=='state' and x['state']==6),None),'actual_reconnecting')
            peers.wait('receiver',lambda rows: next((x for x in rows if x['event']=='state' and x['state']==5
                and x['utc_ms']>witness['utc_ms']),None),'actual_recovered')
            values=peers.convergence(ROLES,objects=ids,asset=asset)
            peers.send('receiver','board_add',id='after-reconnect',x=180,color=0xff4411)
            ids.append('object-after-reconnect')
            values=peers.convergence(ROLES,objects=ids,asset=asset)
            passed('reconnect',values,reconnecting=witness,command_sequence=sequence,post_recovery_commit=True)
            exports={}
            pixels=[]
            for role in ROLES:
                sequence=peers.send(role,'board_export')
                value=peers.wait(role,lambda rows,seq=sequence: next((x for x in rows if x['event']=='board_exported'
                    and x['sequence']==seq),None),'native_export')
                if not value['saved']: raise RuntimeError('native_export_failed')
                path=Path(value['path'])
                with Image.open(path) as decoded:
                    decoded=decoded.convert('RGB')
                    if decoded.size!=(320,180): raise RuntimeError('export_size_mismatch')
                    for point in [(0,0),(319,0),(0,179),(319,179),(200,100)]:
                        if decoded.getpixel(point)!=image.getpixel(point): raise RuntimeError('export_background_pixel_mismatch')
                    if decoded.getpixel((100,25))!=(0x22,0xaa,0x44): raise RuntimeError('export_annotation_pixel_missing')
                    pixel_hash=hashlib.sha256(decoded.tobytes()).hexdigest()
                    pixels.append(pixel_hash)
                exports[role]=dict(file=path.relative_to(root).as_posix(),sha256=sha(path),pixel_sha256=pixel_hash)
            if len(set(pixels))!=1: raise RuntimeError('three_export_pixel_mismatch')
            passed('export',values,exports=exports,independent_decoder='Pillow RGB pixel readback')
            peers.send('publisher','leave')
            peers.wait('publisher',lambda rows: next((x for x in rows if x['event']=='left'),None),'authority_leave')
            name='authority-leave'
        else:
            peers.processes['publisher'].kill(); peers.processes['publisher'].wait(timeout=10)
            peers.lifecycle['publisher'].update(expected_process_termination=True,exit_code=peers.processes['publisher'].returncode)
            name='authority-disconnect'
        frozen=peers.convergence(ROLES[1:],objects=initial_ids,state=3)
        for role in ROLES[1:]: peers.send(role,'board_add',id=role+'-forbidden-after-authority',x=150)
        time.sleep(2.2)  # Includes a production heartbeat; no realtime UIA sampling.
        unchanged=peers.convergence(ROLES[1:],objects=initial_ids,state=3)
        if any(unchanged[r]['document_sha256']!=frozen[r]['document_sha256'] or
               unchanged[r]['sequence']!=frozen[r]['sequence'] for r in ROLES[1:]):
            raise RuntimeError('authority_departure_mutated_frozen_document')
        passed(name,unchanged,read_only_proposals_preserved_document=True)
        if not disconnect: peers.finish('publisher',already_left=True)
        for role in ROLES[1:]: peers.finish(role)
        result['lifecycle']=peers.lifecycle
        result['server_after']=snapshot(room)
        if result['server_after']['participants_remaining'] or result['server_after']['publications_remaining']:
            raise RuntimeError('room_not_empty')
        if any(result['server_before'][k]!=result['server_after'][k] for k in ('config_sha256','image','binary')):
            raise RuntimeError('service_changed')
        result['status']='PASS'
    except Exception as error:
        result.update(status='FAIL',failure=type(error).__name__+':'+str(error))
    finally:
        if peers:
            peers.cleanup()
            result['events_sha256']={r:sha(p/'events.jsonl') for r,p in peers.roots.items() if (p/'events.jsonl').exists()}
        result['finished_utc']=datetime.now(timezone.utc).isoformat()
        save()
    return result


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    args.output=args.output.resolve()
    args.output.mkdir(parents=True,exist_ok=False)
    binary=args.binary.resolve()
    verifier=Path(__file__).resolve().parents[1]/'diagnostics/verify_runtime_binary.ps1'
    verified=subprocess.run(['pwsh','-NoProfile','-File',str(verifier),'-Executable',str(binary),
        '-ExpectedExecutableName','uia_entry_fixture.exe','-Configuration','RelWithDebInfo'],capture_output=True,
        text=True,creationflags=subprocess.CREATE_NO_WINDOW)
    if verified.returncode: parser.exit(2,'RelWithDebInfo verification failed\n')
    service.SERVER_PORT=17980
    service.SERVER_CONTAINER='cohavora-b-acceptance-20261001'
    service.CONFIG_PATH='/root/livekit-b-acceptance-20261001/livekit.yaml'
    inputs=['src/core/meeting_coordinator.cpp','src/core/meeting_session_runtime.h','src/core/room.cpp',
        'src/core/whiteboard/whiteboard_runtime.cpp','src/core/whiteboard/whiteboard_document.cpp',
        'src/ui/whiteboard/whiteboard_renderer.cpp','tests/uia/entry_fixture.cpp',
        'tests/runtime/probes/b_file_product_runtime.h','tests/runtime/probes/b_whiteboard_product_runtime.h',str(Path(__file__))]
    result=dict(status='RUNNING',configuration='RelWithDebInfo',binary_identity=json.loads(verified.stdout),
        head=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),inputs={p:sha(Path(p)) for p in inputs},rooms=[],
        scope='Three separate product Coordinator processes. Public commands, real SFU data, immutable Qt projections and independent PNG pixels. Not three physical terminals or subjective visual alignment.')
    manifest=args.output/'index.json'
    def save(): manifest.write_text(json.dumps(result,indent=2),encoding='utf-8')
    save()
    for name,disconnect in [('collaboration-and-leave',False),('authority-disconnect',True)]:
        if sha(binary)!=result['binary_identity']['binary_sha256']: raise RuntimeError('binary_changed')
        value=run_room(args.output/name,binary,disconnect)
        result['rooms'].append(dict(evidence=name+'/result.json',sha256=sha(args.output/name/'result.json'),status=value['status']))
        save()
        print(name+' '+value['status'],flush=True)
        if value['status']!='PASS': break
    result['independent_cases']=sum(len(json.loads((args.output/x['evidence']).read_text())['cases']) for x in result['rooms'])
    result['status']='PASS' if result['independent_cases']==8 and len(result['rooms'])==2 and all(x['status']=='PASS' for x in result['rooms']) and all(
        sha(Path(p))==h for p,h in result['inputs'].items()) and sha(binary)==result['binary_identity']['binary_sha256'] else 'FAIL'
    result['finished_utc']=datetime.now(timezone.utc).isoformat()
    save()
    return 0 if result['status']=='PASS' else 1


if __name__=='__main__':
    raise SystemExit(main())
