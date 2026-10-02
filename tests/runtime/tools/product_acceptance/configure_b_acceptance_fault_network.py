"""Task-container-only netem. Never change the host/shared-service qdisc."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'screen_capture'))
import invoke_screen_share_quality_probe as service

NETWORK='cohavora-b-fault-20261001'
CONTAINER='cohavora-b-acceptance-20261001'
LABEL='b-acceptance-20261001'
FAULTS={'loss':['loss','2%'], 'jitter':['delay','100ms','20ms'],
        'bandwidth':['rate','600kbit'], 'outage':['loss','100%']}


def configure(operation):
    if operation not in ('prepare','clear','inspect','restart','restore-host',*FAULTS):
        raise ValueError('unknown_operation')
    service.CONFIG_PATH='/root/livekit-b-acceptance-20261001/livekit.yaml'
    code='OP='+repr(operation)+'\nNETWORK='+repr(NETWORK)+'\nCONTAINER='+repr(CONTAINER)+'\nLABEL='+repr(LABEL)+'\nFAULTS='+repr(FAULTS)+'\n'+service.REMOTE_AUTH+'''
import os,subprocess,urllib.request
def inspect(name):return json.loads(subprocess.check_output(['docker','inspect',name]))[0]
shared=inspect('livekit')
original=inspect(CONTAINER)
assert original['Config']['Labels'].get('codex.task')==LABEL
image=original['Image']
original_hash=hashlib.sha256(pathlib.Path(CONFIG_PATH).read_bytes()).hexdigest()
def empty_rooms():
    request=urllib.request.Request('http://127.0.0.1:17980/twirp/livekit.RoomService/ListRooms',
        data=b'{}',headers={'Authorization':'Bearer '+token('b10-admin',dict(roomList=True)),
        'Content-Type':'application/json'})
    rooms=json.load(urllib.request.urlopen(request,timeout=5)).get('rooms',[])
    if any(r.get('numParticipants',0) or r.get('num_participants',0) for r in rooms):
        raise RuntimeError('active_room_blocks_network_reconfiguration')
if OP in ('prepare','restore-host'):
    empty_rooms()
    target=NETWORK if OP=='prepare' else 'host'
    if target==NETWORK:
        found=subprocess.run(['docker','network','inspect',NETWORK],capture_output=True,text=True)
        if found.returncode:
            subprocess.run(['docker','network','create','--driver','bridge','--label','codex.task='+LABEL,
                NETWORK],check=True,capture_output=True)
        else:assert json.loads(found.stdout)[0]['Labels'].get('codex.task')==LABEL
    if original['HostConfig']['NetworkMode']!=target:
        subprocess.run(['docker','rm','-f',CONTAINER],check=True,capture_output=True)
        command=['docker','run','--detach','--name',CONTAINER,'--network',target,'--label',
            'codex.task='+LABEL,'--memory','700m','--cpus','1.5','--volume',CONFIG_PATH+':/config.yaml:ro']
        if target==NETWORK:command+=['-p','17980:17980/tcp','-p','17981:17981/tcp','-p','17982:17982/udp']
        command+=[image,'--config','/config.yaml']
        subprocess.run(command,check=True,capture_output=True)
    if OP=='restore-host':
        found=subprocess.run(['docker','network','inspect',NETWORK],capture_output=True,text=True)
        if found.returncode==0:
            network=json.loads(found.stdout)[0]
            assert network['Labels'].get('codex.task')==LABEL and not network['Containers']
            subprocess.run(['docker','network','rm',NETWORK],check=True,capture_output=True)
elif OP=='restart':
    assert original['HostConfig']['NetworkMode']==NETWORK
    subprocess.run(['docker','restart','--time','2',CONTAINER],check=True,capture_output=True)
current=inspect(CONTAINER)
pid=current['State']['Pid']
assert pid and current['State']['Running']
shared_namespace=os.readlink('/proc/'+str(shared['State']['Pid'])+'/ns/net')
container_namespace=os.readlink('/proc/'+str(pid)+'/ns/net')
qdisc=None
if OP not in ('restore-host',):
    assert current['HostConfig']['NetworkMode']==NETWORK and container_namespace!=shared_namespace
    prefix=['nsenter','--target',str(pid),'--net','tc']
    previous=json.loads(subprocess.check_output(prefix+['-s','-j','qdisc','show','dev','eth0']))
    if any(x.get('kind') not in ('noqueue','netem') for x in previous):
        raise RuntimeError('foreign_qdisc_blocks_mutation')
    if OP in FAULTS:
        subprocess.run(prefix+['qdisc','replace','dev','eth0','root','netem']+FAULTS[OP],check=True,capture_output=True)
    elif OP=='clear' and any(x.get('kind')=='netem' for x in previous):
        subprocess.run(prefix+['qdisc','del','dev','eth0','root'],check=True,capture_output=True)
    qdisc=json.loads(subprocess.check_output(prefix+['-s','-j','qdisc','show','dev','eth0']))
    if OP=='clear' and any(x.get('kind')=='netem' for x in qdisc):raise RuntimeError('netem_not_cleared')
health=None
if OP not in ('outage','inspect'):
    for attempt in range(20):
        try:
            health=urllib.request.urlopen('http://127.0.0.1:17980',timeout=1).status
            break
        except Exception:time.sleep(.2)
    if health!=200:raise RuntimeError('task_health_failed')
after=inspect('livekit')
assert after['Id']==shared['Id'] and after['State']['Running']
assert hashlib.sha256(pathlib.Path(CONFIG_PATH).read_bytes()).hexdigest()==original_hash
assert current['Image']==image
print(json.dumps(dict(operation=OP,network=current['HostConfig']['NetworkMode'],pid=pid,
    container_id=current['Id'],namespace=container_namespace,shared_namespace=shared_namespace,
    shared_service_unchanged=True,config_sha256=original_hash,image=image,qdisc=qdisc,health_status=health)))
'''
    return service.remote_python(code)


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--operation',choices=('prepare','clear','inspect','restart','restore-host',*FAULTS),required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=False)
    try:
        result=configure(args.operation)
    except Exception as error:
        result=dict(status='FAIL',operation=args.operation,failure=type(error).__name__+':'+str(error))
        (args.output/'network.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
        raise
    (args.output/'network.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result))


if __name__=='__main__':main()
