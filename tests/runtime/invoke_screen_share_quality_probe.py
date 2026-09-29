"""Bounded same-track SFU probe; tokens stay in memory and child environment."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time
import uuid
from datetime import datetime, timezone
from product_aliyun_transport import execute

SERVER_PORT = 17880
SERVER_CONTAINER = 'livekit'
CONFIG_PATH = '/root/livekit.yaml'

REMOTE_AUTH = """
import base64, hashlib, hmac, json, pathlib, time, yaml
config = yaml.safe_load(pathlib.Path(CONFIG_PATH).read_text())
key, secret = next(iter(config['keys'].items()))
def token(identity, grants):
    encode = lambda x: base64.urlsafe_b64encode(json.dumps(x,separators=(',',':')).encode()).decode().rstrip('=')
    body = encode({'alg':'HS256','typ':'JWT'}) + '.' + encode(dict(iss=key,sub=identity,nbf=int(time.time())-10,exp=int(time.time())+1200,video=grants))
    return body + '.' + base64.urlsafe_b64encode(hmac.new(secret.encode(),body.encode(),hashlib.sha256).digest()).decode().rstrip('=')
"""

def remote_python(code):
    # Read-only room queries can hit a transient Workbench transport failure.
    for attempt in range(3):
        try:
            return json.loads(execute("python3 - <<'PY'\nCONFIG_PATH=" + repr(CONFIG_PATH) + "\n" + code + "\nPY", timeout=55))
        except RuntimeError:
            if attempt == 2:
                raise
            time.sleep(attempt + 1)

def credentials(room):
    return remote_python(REMOTE_AUTH + f"""
room = {room!r}
print(json.dumps({{name:token(name,dict(roomJoin=True,room=room,canPublish=True,canSubscribe=True)) for name in ['publisher','receiver','late']}}))
""")

def metadata(room):
    return remote_python(REMOTE_AUTH + f"""
import urllib.request
room = {room!r}
request = urllib.request.Request('http://127.0.0.1:{SERVER_PORT}/twirp/livekit.RoomService/ListParticipants',
    data=json.dumps(dict(room=room)).encode(),headers={{'Authorization':'Bearer '+token('quality-admin',dict(roomAdmin=True,room=room)),'Content-Type':'application/json'}})
data=json.load(urllib.request.urlopen(request,timeout=10))
allowed = ['sid','type','source','width','height','layers','codecs','mimeType','simulcast']
tracks=[{{k:v for k,v in t.items() if k in allowed}} for p in data.get('participants',[]) if p.get('identity')=='publisher' for t in p.get('tracks',[])]
print(json.dumps(dict(tracks=tracks)))
""")

def bandwidth_evidence(room):
    return remote_python(f"""
import json, subprocess
result=subprocess.run(['docker','logs','--since','5m','{SERVER_CONTAINER}'],capture_output=True,text=True)
values=[]
for line in (result.stdout+result.stderr).splitlines():
    if {room!r} not in line or 'simulating subscriber bandwidth' not in line: continue
    try: fields=json.loads(line.split(chr(9))[-1])
    except ValueError: continue
    if fields.get('participant')=='receiver': values.append(fields.get('bandwidth',0))
print(json.dumps(values))
""")

def run(binary, root, codec, simulcast, legacy_layers, camera=False):
    name = codec + ('-simulcast' if simulcast else '-single') + ('-camera' if camera else '')
    directory = root / name
    directory.mkdir()
    room = 'quality-' + uuid.uuid4().hex[:16]
    auth = credentials(room)
    env = os.environ.copy()
    env.update(LIVEKIT_URL=f'ws://123.56.225.164:{SERVER_PORT}', LIVEKIT_TOKEN=auth['publisher'],
        LIVEKIT_PEER_TOKEN=auth['receiver'],LIVEKIT_LATE_TOKEN=auth['late'],
        LIVEKIT_TEST_ALLOW_INSECURE='1',QUALITY_ACK_DIR=str(directory.resolve()))
    env.pop('QUALITY_PROBE_LEGACY_LAYERS',None)
    env.pop('QUALITY_PROBE_CAMERA',None)
    if camera: env['QUALITY_PROBE_CAMERA']='1'
    if legacy_layers: env['QUALITY_PROBE_LEGACY_LAYERS']='1'
    args = [str(binary.resolve()), codec] + (['simulcast'] if simulcast else [])
    child = subprocess.Popen(args, env=env, stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,text=True,encoding='utf-8',errors='replace',creationflags=subprocess.CREATE_NO_WINDOW)
    lines = queue.Queue()
    def read():
        for line in child.stdout:
            if line.startswith('QUALITY_PROBE '):
                try: lines.put(json.loads(line[len('QUALITY_PROBE '):]))
                except ValueError: lines.put(dict(event='failure',code='invalid_probe_json'))
        lines.put(None)
    reader = threading.Thread(target=read,daemon=True)
    reader.start()
    events = []
    status = 'FAIL'
    media_status = 'FAIL'
    layer_status = 'NOT_APPLICABLE'
    metadata_status = 'NOT_APPLICABLE' if camera else 'FAIL'
    deadline = time.monotonic()+240
    try:
        while time.monotonic()<deadline:
            try: record=lines.get(timeout=.5)
            except queue.Empty: continue
            if record is None: break
            record['timestamp_utc']=datetime.now(timezone.utc).isoformat()
            record['run_id']=room
            if record.get('event')=='weak_result':
                record['server_bandwidth_limits']=bandwidth_evidence(room)
                record['matched']=1000 in record['server_bandwidth_limits'] and 0 in record['server_bandwidth_limits']
            if record.get('matched') is False:
                record['server_at_failure']=metadata(room)
            if record.get('event')=='stage':
                state=metadata(room)
                record['server']=state
                tracks=state['tracks']
                record['server_dimensions_verified']=any(t.get('sid')==record['sid'] and
                    t.get('width')==record['width'] and t.get('height')==record['height'] for t in tracks)
                expected={(record['width'],record['height'])}
                if simulcast: expected.add((record['width']//2,record['height']//2))
                published=next((t for t in tracks if t.get('sid')==record['sid']),{})
                primary=published.get('codecs',[{}])[0]
                record['server_layer_dimensions_verified']=all(
                    {(layer.get('width'),layer.get('height')) for layer in layers}==expected
                    for layers in [published.get('layers',[]),primary.get('layers',[])])
                if os.environ.get('QUALITY_PROBE_BACKUP'):
                    backup=next((c for c in published.get('codecs',[]) if c.get('mime_type','').lower()=='video/vp8'),{})
                    record['server_backup_dimensions_verified']={
                        (layer.get('width'),layer.get('height')) for layer in backup.get('layers',[])}==expected
                    record['server_layer_dimensions_verified'] &= record['server_backup_dimensions_verified']
                (directory/(str(record['index'])+'.ack')).write_text('observed\n')
            events.append(record)
            (directory/'events.json').write_text(json.dumps(events,indent=2),encoding='utf-8')
            print(name, json.dumps(record),flush=True)
            # Keep the explicit LOW diagnostic after a dimension-selection failure.
            # The final layer verdict still rejects every unmatched event.
            if (record.get('server_dimensions_verified') is False or
                (not camera and record.get('server_layer_dimensions_verified') is False)):
                child.terminate()
                break
        child.wait(timeout=15)
        stages=[e for e in events if e.get('event')=='stage']
        expected_count=3 if camera else 10
        if child.returncode==0 and len(stages)==expected_count and all(e['server_dimensions_verified'] for e in stages): media_status='PASS'
        if not camera and len(stages)==expected_count and all(e.get('server_layer_dimensions_verified') for e in stages): metadata_status='PASS'
        if simulcast:
            dimensions=[e for e in events if e.get('event')=='layer_dimensions']
            lows=[e for e in events if e.get('event')=='layer_explicit_low']
            layer_status='PASS' if len(dimensions)==expected_count and len(lows)==expected_count and all(e.get('matched') for e in dimensions+lows) and all(e.get('server_layer_dimensions_verified') for e in stages) else 'FAIL'
        weak_ok = not os.environ.get('QUALITY_PROBE_WEAK') or any(
            e.get('event')=='weak_result' and e.get('status')=='PASS' and e.get('matched') for e in events)
        if media_status=='PASS' and layer_status in ['PASS','NOT_APPLICABLE'] and metadata_status in ['PASS','NOT_APPLICABLE'] and weak_ok: status='PASS'
    finally:
        if child.poll() is None: child.terminate(); child.wait(timeout=10)
        child.stdout.close()
    return dict(case=name,status=status,media_switch_status=media_status,
                dimension_layer_selection_status=layer_status,metadata_status=metadata_status,exit_code=child.returncode,events=events)

def main():
    global SERVER_PORT, SERVER_CONTAINER, CONFIG_PATH
    parser=argparse.ArgumentParser()
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--codecs',nargs='+',default=['vp8','h264','vp9','av1'])
    parser.add_argument('--simulcast',action='store_true')
    parser.add_argument('--legacy-layers',action='store_true')
    parser.add_argument('--candidate-server',action='store_true')
    parser.add_argument('--camera',action='store_true')
    parser.add_argument('--production',action='store_true')
    parser.add_argument('--weak',action='store_true')
    parser.add_argument('--backup',action='store_true')
    args=parser.parse_args()
    if args.backup and (not args.simulcast or args.camera or args.codecs != ['vp9']):
        parser.error('--backup requires --simulcast --codecs vp9 and screen-share mode')
    if args.production: os.environ["QUALITY_PROBE_PRODUCTION"]="1"
    else: os.environ.pop("QUALITY_PROBE_PRODUCTION",None)
    if args.weak: os.environ['QUALITY_PROBE_WEAK']='1'
    else: os.environ.pop('QUALITY_PROBE_WEAK',None)
    if args.backup: os.environ['QUALITY_PROBE_BACKUP']='1'
    else: os.environ.pop('QUALITY_PROBE_BACKUP',None)
    if args.candidate_server:
        SERVER_PORT = 17890
        SERVER_CONTAINER = 'livekit-quality-candidate'
        CONFIG_PATH = '/root/livekit-quality-candidate-20260929/candidate.yaml'
    args.output.mkdir(parents=True,exist_ok=False)
    server=execute(f"docker exec {SERVER_CONTAINER} /livekit-server --version && docker inspect --format '{{{{.Image}}}}' {SERVER_CONTAINER} && docker exec {SERVER_CONTAINER} sha256sum /livekit-server")
    inputs=['tests/runtime/test_screen_share_quality_runtime.cpp',
            'tests/runtime/invoke_screen_share_quality_probe.py','tests/cmake/NativeTests.cmake',
            'src/core/room.cpp','src/core/local_video_track.cpp','src/rtc/rtc_video_source.cpp','src/rtc/webrtc_manager.cpp']
    manifest=dict(status='RUNNING',production_api=args.production,weak_bandwidth=args.weak,backup_vp8=args.backup,timestamp_utc=datetime.now(timezone.utc).isoformat(),camera=args.camera,legacy_layers=args.legacy_layers,server=server,binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                  head=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
                  inputs={p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in inputs},cases=[])
    (args.output/'summary.json').write_text(json.dumps(manifest,indent=2))
    for codec in args.codecs:
        if codec not in ['vp8','h264','vp9','av1']: raise ValueError('invalid_codec')
        try:
            manifest['cases'].append(run(args.binary,args.output,codec,args.simulcast,args.legacy_layers,args.camera))
        except Exception as error:
            manifest['cases'].append(dict(case=codec,status='FAIL',error=type(error).__name__))
        (args.output/'summary.json').write_text(json.dumps(manifest,indent=2))
        if manifest['cases'][-1]['status'] != 'PASS': break
    passed=all(c['status']=='PASS' for c in manifest['cases'])
    manifest['status']='PASS' if passed else 'FAIL'
    (args.output/'summary.json').write_text(json.dumps(manifest,indent=2))
    return 0 if passed else 1

if __name__=='__main__':
    try: raise SystemExit(main())
    except Exception as error: print('QUALITY_PROBE_LAUNCH_ERROR',type(error).__name__); raise SystemExit(1)
