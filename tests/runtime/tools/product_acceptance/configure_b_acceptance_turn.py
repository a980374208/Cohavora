"""Temporary UDP TURN and direct-media blocking inside the task namespace."""
from __future__ import annotations
import json
from pathlib import Path
import sys

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'screen_capture'))
import invoke_screen_share_quality_probe as service
from configure_b_acceptance_network import api, INSTANCE

LABEL='codex-b-turn-20261001'
SOURCE='115.61.243.139/32'


def security_group(enable):
    instance=api('DescribeInstances',InstanceIds=json.dumps([INSTANCE]))['Instances']['Instance'][0]
    groups=instance['SecurityGroupIds']['SecurityGroupId']
    changes=[]
    for group in groups:
        rules=api('DescribeSecurityGroupAttribute',SecurityGroupId=group)['Permissions']['Permission']
        owned=[x for x in rules if x.get('Description')==LABEL]
        for rule in owned:
            if rule['IpProtocol'].lower()!='udp' or rule['PortRange']!='17983/17983' or rule['SourceCidrIp']!=SOURCE:
                raise RuntimeError('TURN_rule_scope_mismatch')
        if not enable:
            for rule in owned:
                api('RevokeSecurityGroup',SecurityGroupId=group,**{'SecurityGroupRuleId.1':rule['SecurityGroupRuleId']})
                changes.append(dict(group=group,removed=rule['SecurityGroupRuleId']))
        elif not owned:
            api('AuthorizeSecurityGroup',SecurityGroupId=group,IpProtocol='udp',PortRange='17983/17983',
                SourceCidrIp=SOURCE,Policy='Accept',Description=LABEL)
            changes.append(dict(group=group,port=17983,source=SOURCE))
    return changes


def configure_turn(operation):
    if operation not in ('prepare','direct-block','clear','inspect','restore'):raise ValueError('unknown_operation')
    service.CONFIG_PATH='/root/livekit-b-acceptance-20261001/livekit.yaml'
    if operation=='prepare':rules=security_group(True)
    else:rules=[]
    code='OP='+repr(operation)+'\n'+service.REMOTE_AUTH+'''
import os,subprocess,urllib.request
name='cohavora-b-acceptance-20261001'
label='b-acceptance-20261001'
network='cohavora-b-fault-20261001'
path=pathlib.Path(CONFIG_PATH)
backup=path.with_name('no-turn-original.yaml')
def inspect(name):return json.loads(subprocess.check_output(['docker','inspect',name]))[0]
shared=inspect('livekit');current=inspect(name);image=current['Image']
assert current['Config']['Labels'].get('codex.task')==label and current['HostConfig']['NetworkMode']==network
before_hash=hashlib.sha256(path.read_bytes()).hexdigest()
if OP in ('prepare','restore'):
    request=urllib.request.Request('http://127.0.0.1:17980/twirp/livekit.RoomService/ListRooms',data=b'{}',
        headers={'Authorization':'Bearer '+token('turn-admin',dict(roomList=True)),'Content-Type':'application/json'})
    # ListRooms' participant count can lag a just-completed departure. Query
    # the actual participant roster before any task container reconfiguration.
    for room in json.load(urllib.request.urlopen(request,timeout=5)).get('rooms',[]):
        roster=urllib.request.Request('http://127.0.0.1:17980/twirp/livekit.RoomService/ListParticipants',
            data=json.dumps(dict(room=room['name'])).encode(),headers={
                'Authorization':'Bearer '+token('turn-admin',dict(roomAdmin=True,room=room['name'])),
                'Content-Type':'application/json'})
        assert not json.load(urllib.request.urlopen(roster,timeout=5)).get('participants',[]), 'active_participants_block_reconfiguration'
    if OP=='prepare':
        assert not config.get('turn',{}).get('enabled')
        if backup.exists():assert backup.read_bytes()==path.read_bytes()
        else:
            fd=os.open(backup,os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600)
            with os.fdopen(fd,'wb') as stream:stream.write(path.read_bytes())
        config['turn']=dict(enabled=True,udp_port=17983,tls_port=0,relay_range_start=17984,relay_range_end=17999)
        content=yaml.safe_dump(config,sort_keys=True).encode()
    else:
        assert config.get('turn',{}).get('udp_port')==17983 and backup.exists()
        content=backup.read_bytes()
    subprocess.run(['docker','rm','-f',name],check=True,capture_output=True)
    path.write_bytes(content);os.chmod(path,0o600)
    command=['docker','run','--detach','--name',name,'--network',network,'--label','codex.task='+label,
        '--memory','700m','--cpus','1.5','--volume',str(path)+':/config.yaml:ro',
        '-p','17980:17980/tcp','-p','17981:17981/tcp','-p','17982:17982/udp']
    if OP=='prepare':command+=['-p','17983:17983/udp','-p','17984-17999:17984-17999/udp']
    command+=[image,'--config','/config.yaml']
    subprocess.run(command,check=True,capture_output=True)
    current=inspect(name)
pid=current['State']['Pid'];assert pid and current['State']['Running']
namespace=os.readlink('/proc/'+str(pid)+'/ns/net')
assert namespace!=os.readlink('/proc/'+str(shared['State']['Pid'])+'/ns/net')
prefix=['nsenter','--target',str(pid),'--net','iptables']
for proto,port in [('udp','17982'),('tcp','17981')]:
    rule=['INPUT','-s','115.61.243.139/32','-p',proto,'--dport',port,'-m','comment','--comment','b10-direct-media-block','-j','DROP']
    present=subprocess.run(prefix+['-C']+rule,capture_output=True).returncode==0
    if OP=='direct-block' and not present:subprocess.run(prefix+['-I']+rule,check=True,capture_output=True)
    elif OP=='clear' and present:subprocess.run(prefix+['-D']+rule,check=True,capture_output=True)
rules=subprocess.check_output(prefix+['-S','INPUT'],text=True).splitlines()
health=None
for attempt in range(30):
    try:health=urllib.request.urlopen('http://127.0.0.1:17980',timeout=1).status;break
    except Exception:time.sleep(.2)
assert health==200
after=inspect('livekit');assert after['Id']==shared['Id'] and after['State']['Running']
assert current['Image']==image
print(json.dumps(dict(operation=OP,health_status=health,config_before=before_hash,
    config_after=hashlib.sha256(path.read_bytes()).hexdigest(),image=image,
    namespace=namespace,shared_service_unchanged=True,rules=rules)))
'''
    result=service.remote_python(code)
    if operation=='restore':rules=security_group(False)
    result['security_group_changes']=rules
    return result
