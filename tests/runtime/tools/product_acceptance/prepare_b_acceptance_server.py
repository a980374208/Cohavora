"""Prepare the task-owned ECS SFU without changing the shared livekit service."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "screen_capture"))
from invoke_screen_share_quality_probe import remote_python


REMOTE_ROOT = "/root/livekit-b-acceptance-20261001"
CONTAINER = "cohavora-b-acceptance-20261001"
PORT = 17980


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", choices=("primary", "regression"), default="primary")
    parser.add_argument("--codec-profile", choices=("all", "vp8-only", "no-video"), default="all")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    code = """
import hashlib,json,os,pathlib,secrets,socket,subprocess,time,urllib.request,yaml
root=pathlib.Path(ROOT)
root.mkdir(mode=0o700,exist_ok=True)
path=root/'livekit.yaml'
label='b-acceptance-20261001'
shared=json.loads(subprocess.check_output(['docker','inspect','livekit']))[0]
original_id=shared['Id']
image=shared['Image']
inspect=subprocess.run(['docker','inspect',CONTAINER],capture_output=True,text=True)
existing=json.loads(inspect.stdout)[0] if inspect.returncode==0 else None
if existing and existing['Config']['Labels'].get('codex.task')!=label:
    raise RuntimeError('container_not_owned')
threshold=1 if PROFILE=='primary' else 0
if path.exists():
    config=yaml.safe_load(path.read_text())
else:
    config=dict(port=PORT,bind_addresses=['0.0.0.0'],
        rtc=dict(tcp_port=PORT+1,udp_port=PORT+2,node_ip='123.56.225.164',use_external_ip=False),
        room=dict(empty_timeout=60,departure_timeout=20),
        keys={'b-acceptance':secrets.token_urlsafe(32)},logging=dict(level='info'))
config['video']=dict(codec_regression_threshold=threshold)
config['room'].pop('enabled_codecs',None)
if CODEC_PROFILE!='all':
    codecs=[dict(mime='audio/opus')]
    if CODEC_PROFILE=='vp8-only':codecs += [dict(mime='video/VP8'),dict(mime='video/rtx')]
    config['room']['enabled_codecs']=codecs
content=yaml.safe_dump(config,sort_keys=True)
changed=not path.exists() or path.read_text()!=content
if changed and existing:
    subprocess.run(['docker','rm','-f',CONTAINER],check=True,capture_output=True)
    existing=None
if changed:
    temporary=root/'livekit.yaml.tmp'
    fd=os.open(temporary,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600)
    with os.fdopen(fd,'w') as stream:stream.write(content)
    os.replace(temporary,path)
if not existing:
    for port,kind in [(PORT,socket.SOCK_STREAM),(PORT+1,socket.SOCK_STREAM),(PORT+2,socket.SOCK_DGRAM)]:
        with socket.socket(socket.AF_INET,kind) as test:
            # Live clients leave TCP TIME_WAIT sockets after the old service
            # exits. Match Go's listener reuse semantics while still rejecting
            # a live listener; UDP ownership remains exclusive.
            if kind==socket.SOCK_STREAM:test.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
            test.bind(('0.0.0.0',port))
    subprocess.run(['docker','run','--detach','--name',CONTAINER,'--network','host',
        '--label','codex.task='+label,'--memory','700m','--cpus','1.5',
        '--volume',str(path)+':/config.yaml:ro',image,'--config','/config.yaml'],
        check=True,capture_output=True)
elif not existing['State']['Running']:
    subprocess.run(['docker','start',CONTAINER],check=True,capture_output=True)
for attempt in range(20):
    try:
        status=urllib.request.urlopen('http://127.0.0.1:'+str(PORT),timeout=1).status
        break
    except Exception:time.sleep(.5)
else:raise RuntimeError('isolated_health_failed')
after=json.loads(subprocess.check_output(['docker','inspect','livekit']))[0]
assert after['Id']==original_id and after['State']['Running']
print(json.dumps(dict(status='READY',profile=PROFILE,codec_profile=CODEC_PROFILE,container=CONTAINER,port=PORT,
    config_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
    video=config['video'],room=config['room'],rtc=config['rtc'],image=image,
    binary=subprocess.check_output(['docker','exec',CONTAINER,'sha256sum','/livekit-server'],text=True).split()[0],
    health_status=status,shared_service_unchanged=True)))
"""
    result = remote_python("ROOT=" + repr(REMOTE_ROOT) + "\nCONTAINER=" + repr(CONTAINER)
                           + "\nPORT=" + str(PORT) + "\nPROFILE=" + repr(args.profile)
                           + "\nCODEC_PROFILE=" + repr(args.codec_profile) + "\n" + code)
    (args.output / "server.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
