"""B10 isolated SFU + two independent product peers and real receiver UI."""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
import uuid

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'meeting'))
from run_b_acceptance_whiteboard import Peers
from run_b_acceptance_files import events, sha
from run_b_acceptance_recovery import snapshot
from configure_b_acceptance_fault_network import configure
from configure_b_acceptance_turn import configure_turn
import invoke_screen_share_quality_probe as service

CASES=('baseline','packet-loss','jitter','bandwidth','network-loss','service-restart',
       'signal-reconnect','full-reconnect','auth-recovery','path-switch','turn-relay')


def authentication_case(room):
    # Correct signature, expired lifetime. Keys/tokens never enter evidence or argv.
    return service.remote_python('ROOM='+repr(room)+'\n'+service.REMOTE_AUTH+'''
import urllib.request,urllib.error
encode=lambda x:base64.urlsafe_b64encode(json.dumps(x,separators=(',',':')).encode()).decode().rstrip('=')
body=encode(dict(alg='HS256',typ='JWT'))+'.'+encode(dict(iss=key,sub='receiver',
    nbf=int(time.time())-600,exp=int(time.time())-300,
    video=dict(roomJoin=True,room=ROOM,canPublish=True,canSubscribe=True)))
expired=body+'.'+base64.urlsafe_b64encode(hmac.new(secret.encode(),body.encode(),hashlib.sha256).digest()).decode().rstrip('=')
valid=token('receiver',dict(roomJoin=True,room=ROOM,canPublish=True,canSubscribe=True))
def validate(value):
    request=urllib.request.Request('http://127.0.0.1:17980/rtc/validate',
        headers={'Authorization':'Bearer '+value})
    try:return urllib.request.urlopen(request,timeout=5).status
    except urllib.error.HTTPError as error:return error.code
print(json.dumps(dict(expired=expired,expired_http=validate(expired),valid_http=validate(valid))))
''')


class NetworkPeers(Peers):
    def __init__(self,root,binary,room,auth,proxy_url=None,rejected_token=None):
        super().__init__(root,binary,room,auth)
        self.proxy_url=proxy_url
        self.rejected_token=rejected_token

    def launch(self,role):
        root=self.roots[role]; root.mkdir()
        env={k:v for k,v in os.environ.items() if not k.startswith(('LIVEKIT_TEST_','E2EE_','B_FILE_','B_BOARD_','B_NETWORK_','B_GENERATED_','B_AUTH_'))}
        env.update(B_FILE_ROOT=str(root),B_FILE_IDENTITY=role,B_FILE_ROOM=self.room,B_NETWORK_MODE='1',
            B_NETWORK_SHOW_UI='1' if role=='receiver' and not self.rejected_token else '0',B_GENERATED_VIDEO='1' if role=='publisher' else '0',
            LIVEKIT_URL=self.proxy_url if role=='receiver' and self.proxy_url else 'ws://123.56.225.164:17980',
            LIVEKIT_TOKEN=self.auth[role],LIVEKIT_TEST_ALLOW_INSECURE='1',QT_QPA_PLATFORM='windows')
        if role=='receiver' and self.rejected_token:env['B_AUTH_REJECTED_TOKEN']=self.rejected_token
        child=subprocess.Popen([str(self.binary),'--b-file-runtime'],env=env,stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW)
        self.processes[role]=child
        started=self.wait(role,lambda rows:next((x for x in rows if x['event']=='started'),None),'started')
        self.lifecycle[role]=dict(pid=child.pid,started=started)
        if role=='receiver' and self.rejected_token:
            self.send(role,'join_rejected')
            rejected=self.wait(role,lambda rows:next((x for x in rows if x['event']=='auth_rejected'),None),'expired_auth_rejected',45)
            if not any(x['event']=='state' and x['state']==8 for x in events(root)):
                raise RuntimeError('expired_auth_failed_state_missing')
            self.lifecycle[role]['authentication_rejected']=rejected
            # A failed admission retires its meeting window. Open the real
            # receiving window only for the subsequent valid admission.
            self.send(role,'network_open_ui')
        self.send(role,'join')
        self.wait(role,lambda rows:next((x for x in rows if x['event']=='joined'),None),'joined',45)

    def finish(self,role,already_left=False):
        # A rejected admission owns a separate retired Room. Both must drain.
        if not (role=='receiver' and self.rejected_token):return super().finish(role,already_left)
        if not already_left:
            self.send(role,'leave')
            self.wait(role,lambda rows:next((x for x in rows if x['event']=='left'),None),'leave')
        self.send(role,'finish')
        retired=self.wait(role,lambda rows:next((x for x in rows if x['event']=='retired'),None),'retirement')
        child=self.processes[role];child.wait(timeout=10)
        if child.returncode or retired['rooms_observed']!=2 or retired['rooms_alive'] or retired['cleanup_pending']:
            raise RuntimeError(role+':native_auth_retirement_failed')
        self.lifecycle[role].update(retirement=retired,exit_code=child.returncode)

    def observe_network(self,role='receiver'):
        sequence=self.send(role,'network_observe')
        return self.wait(role,lambda rows:next((x for x in rows if x['event']=='network_observation'
            and x['sequence']==sequence),None),'network_observation')

    def advancing_media(self,baseline=0,seconds=35):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            value=self.observe_network()
            if value['state']==5 and value['decoded_frames']>baseline+10 and value.get('ui',{}).get('render_delivered',0)>0:
                if not any(t['attached'] and t['frames']>0 and t['width']>0 and t['height']>0 for t in value['tracks']):
                    raise RuntimeError('no_live_native_video_binding')
                return value
            time.sleep(.2)
        raise RuntimeError('actual_product_decode_render_recovery_timeout')


def unused_ports():
    held=[]
    try:
        for _ in range(2):
            current=socket.socket(socket.AF_INET,socket.SOCK_STREAM)
            current.setsockopt(socket.SOL_SOCKET,socket.SO_EXCLUSIVEADDRUSE,1)
            current.bind(('127.0.0.1',0)); held.append(current)
        return [x.getsockname()[1] for x in held]
    finally:
        for x in held:x.close()


def run_case(case,root,binary):
    root.mkdir()
    room='b10-'+uuid.uuid4().hex
    result=dict(status='RUNNING',case=case,room=room,started_utc=datetime.now(timezone.utc).isoformat())
    manifest=root/'result.json'
    def save():manifest.write_text(json.dumps(result,indent=2),encoding='utf-8')
    save()
    peers=proxy=None
    proxy_stream=None
    turn_prepared=False
    try:
        result['clean_network_before']=configure('clear')
        if case=='turn-relay':
            result['turn_preparation']=configure_turn('prepare');turn_prepared=True
            result['direct_media_block']=configure_turn('direct-block')
        result['server_before']=snapshot(room)
        auth=service.credentials(room)
        rejected_token=None
        if case=='auth-recovery':
            admission=authentication_case(room)
            rejected_token=admission.pop('expired')
            result['authentication_http']=admission
            if admission!={'expired_http':401,'valid_http':200}:
                raise RuntimeError('signed_expired_or_valid_token_precondition_failed')
        proxy_url=None
        if case=='network-loss':
            listen,control=unused_ports()
            result['proxy_ports']=dict(listen=listen,control=control)
            proxy_stream=(root/'proxy.log').open('w',encoding='utf-8')
            script=Path(__file__).resolve().parents[1]/'meeting/livekit_signal_fault_proxy.py'
            proxy=subprocess.Popen([sys.executable,'-B',str(script),'--listen-port',str(listen),'--control-port',str(control),
                '--target-host','123.56.225.164','--target-port','17980'],stdout=proxy_stream,stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NO_WINDOW)
            end=time.monotonic()+10
            while time.monotonic()<end:
                if '[PROXY_READY]' in (root/'proxy.log').read_text():break
                if proxy.poll() is not None:raise RuntimeError('proxy_exited')
                time.sleep(.1)
            else:raise RuntimeError('proxy_not_ready')
            proxy_url='ws://127.0.0.1:'+str(listen)
        peers=NetworkPeers(root,binary,room,auth,proxy_url,rejected_token)
        peers.launch('receiver'); peers.launch('publisher')
        result['baseline']=peers.advancing_media()
        result['sender_baseline']=peers.observe_network('publisher')
        if case=='turn-relay':
            end=time.monotonic()+30
            while time.monotonic()<end:
                value=peers.observe_network()
                if 'relay' in value['telemetry'].get('localCandidateTypes',''):
                    result['actual_relay']=value
                    break
                time.sleep(.5)
            else:raise RuntimeError('actual_TURN_relay_not_observed')
            result['recovered']=peers.advancing_media(value['decoded_frames'])
        if peers.processes['receiver'].pid==peers.processes['publisher'].pid:
            raise RuntimeError('distinct_product_processes_required')
        save()
        if case in ('packet-loss','jitter','bandwidth'):
            operation={'packet-loss':'loss','jitter':'jitter','bandwidth':'bandwidth'}[case]
            result['fault_started']=configure(operation)
            samples=[]
            sender_samples=[]
            for _ in range(10):
                time.sleep(1)
                samples.append(peers.observe_network())
                sender_samples.append(peers.observe_network('publisher'))
            result['fault_samples']=samples
            result['sender_fault_samples']=sender_samples
            result['fault_actual']=configure('inspect')
            qdisc=[x for x in result['fault_actual']['qdisc'] if x['kind']=='netem']
            if len(qdisc)!=1 or qdisc[0]['packets']==0 or qdisc[0]['bytes']==0:
                raise RuntimeError('fault_did_not_carry_real_traffic')
            if case=='packet-loss' and qdisc[0]['drops']==0:
                raise RuntimeError('actual_packet_drop_not_observed')
            result['fault_removed']=configure('clear')
            result['sender_after_fault']=peers.observe_network('publisher')
            save()
            result['recovered']=peers.advancing_media(baseline=samples[-1]['decoded_frames'])
        elif case in ('network-loss','service-restart','signal-reconnect','full-reconnect'):
            cutoff=len(events(peers.roots['receiver']))
            if case=='network-loss':
                with socket.create_connection(('127.0.0.1',control),timeout=5) as channel:
                    channel.sendall(b'fault 3\n')
                    if channel.recv(128).strip()!=b'OK':raise RuntimeError('proxy_rejected_fault')
                result['fault_started']=dict(signaling_tcp_actual_close=True,blocked_seconds=3)
            elif case=='service-restart':result['fault_started']=configure('restart')
            else:
                # A real server-driven disconnect scenario, not a local fake event.
                sequence=peers.send('receiver','network_full_reconnect' if case=='full-reconnect' else 'network_reconnect')
                result['fault_started']=dict(command_sequence=sequence,scenario=case)
            reconnecting=peers.wait('receiver',lambda rows:next((x for x in rows[cutoff:] if x['event']=='state' and x['state']==6),None),'actual_reconnecting',45)
            ui=peers.wait('receiver',lambda rows:next((x for x in rows[cutoff:] if x['event']=='network_ui_state'
                and x['state']==6 and x['visible'] and x['banner_visible'] and x['reconnecting_banner']),None),'actual_reconnecting_UI',10)
            peers.wait('receiver',lambda rows:next((x for x in rows[cutoff:] if x['event']=='state' and x['state']==5
                and x['utc_ms']>=reconnecting['utc_ms']),None),'actual_recovered',60)
            baseline=peers.observe_network()['decoded_frames']
            result.update(reconnecting=reconnecting,reconnecting_ui=ui,recovered=peers.advancing_media(baseline))
        elif case=='path-switch':
            before=peers.observe_network()
            if 'udp' not in before['telemetry'].get('mediaProtocols',[]):
                raise RuntimeError('UDP_media_path_required_before_switch')
            result['fault_started']=dict(command_sequence=peers.send('receiver','network_path_switch'),target='TCP')
            # Apply the SFU's cached candidate preference during real signal
            # resume; an existing nominated UDP pair can otherwise survive
            # the server's ICE restart on this client/server combination.
            time.sleep(1)
            result['fault_started']['resume_sequence']=peers.send('receiver','network_reconnect')
            end=time.monotonic()+60
            while time.monotonic()<end:
                after=peers.observe_network()
                if ('tcp' in after['telemetry'].get('mediaProtocols',[]) and
                    after['telemetry'].get('mediaPathSwitches',0)>before['telemetry'].get('mediaPathSwitches',0)):
                    break
                time.sleep(.5)
            else:raise RuntimeError('actual_TCP_candidate_switch_not_observed')
            result.update(path_before=before,path_after=after,recovered=peers.advancing_media(after['decoded_frames']))
        elif case in ('baseline','auth-recovery'):
            result['recovered']=peers.advancing_media(result['baseline']['decoded_frames'])
        for role in ('publisher','receiver'):peers.finish(role)
        result['lifecycle']=peers.lifecycle
        result['clean_network_after']=configure('clear')
        result['server_after']=snapshot(room)
        if result['server_after']['participants_remaining'] or result['server_after']['publications_remaining']:
            raise RuntimeError('room_not_empty')
        if any(result['server_before'][k]!=result['server_after'][k] for k in ('config_sha256','image','binary')):
            raise RuntimeError('service_configuration_changed')
        result['status']='PASS'
    except Exception as error:
        result.update(status='FAIL',failure=type(error).__name__+':'+str(error))
    finally:
        if peers:
            peers.cleanup()
            result['events_sha256']={role:sha(p/'events.jsonl') for role,p in peers.roots.items() if (p/'events.jsonl').exists()}
        if proxy and proxy.poll() is None:proxy.terminate();proxy.wait(timeout=10)
        if proxy_stream:proxy_stream.close()
        try:result['finally_network_clear']=configure('clear')
        except Exception as error:result.update(status='FAIL',cleanup_failure=type(error).__name__+':'+str(error))
        if turn_prepared:
            try:
                result['finally_turn_clear']=configure_turn('clear')
                result['finally_turn_restore']=configure_turn('restore')
                if result['finally_turn_restore']['config_after']!=result['turn_preparation']['config_before']:
                    raise RuntimeError('original_SFU_config_not_restored')
            except Exception as error:result.update(status='FAIL',turn_cleanup_failure=type(error).__name__+':'+str(error))
        result['finished_utc']=datetime.now(timezone.utc).isoformat()
        save()
    return result


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--case',choices=CASES,action='append')
    args=parser.parse_args();args.output=args.output.resolve()
    args.output.mkdir(parents=True,exist_ok=False)
    binary=args.binary.resolve()
    verifier=Path(__file__).resolve().parents[1]/'diagnostics/verify_runtime_binary.ps1'
    verified=subprocess.run(['pwsh','-NoProfile','-File',str(verifier),'-Executable',str(binary),
        '-ExpectedExecutableName','uia_entry_fixture.exe','-Configuration','RelWithDebInfo'],capture_output=True,
        text=True,creationflags=subprocess.CREATE_NO_WINDOW)
    if verified.returncode:parser.exit(2,'RelWithDebInfo verification failed\n')
    service.SERVER_PORT=17980;service.SERVER_CONTAINER='cohavora-b-acceptance-20261001'
    service.CONFIG_PATH='/root/livekit-b-acceptance-20261001/livekit.yaml'
    inputs=['src/core/meeting_coordinator.cpp','src/core/room.cpp','src/ui/meeting_room_window.cpp',
        'tests/uia/entry_fixture.cpp','tests/runtime/probes/b_file_product_runtime.h',
        'tests/runtime/probes/b_network_product_runtime.h',str(Path(__file__)),
        str(Path(__file__).with_name('configure_b_acceptance_fault_network.py'))]
    inputs.append(str(Path(__file__).with_name('configure_b_acceptance_turn.py')))
    result=dict(status='RUNNING',configuration='RelWithDebInfo',head=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
        binary_identity=json.loads(verified.stdout),inputs={p:sha(Path(p)) for p in inputs},cases=[],
        scope='Two distinct product Coordinator processes; generated 640x360/20FPS media and actual receiving MeetingRoomWindow. Netem only inside task container namespace; no production/shared service faults.')
    manifest=args.output/'index.json'
    def save():manifest.write_text(json.dumps(result,indent=2),encoding='utf-8')
    save()
    for case in args.case or CASES:
        value=run_case(case,args.output/case,binary)
        result['cases'].append(dict(case=case,status=value['status'],evidence=case+'/result.json',sha256=sha(args.output/case/'result.json')))
        print(case+' '+value['status'],flush=True)
        save()
        if value['status']!='PASS':break
    result['status']='PASS' if len(result['cases'])==len(args.case or CASES) and all(x['status']=='PASS' for x in result['cases']) and all(
        sha(Path(p))==h for p,h in result['inputs'].items()) and sha(binary)==result['binary_identity']['binary_sha256'] else 'FAIL'
    result['finished_utc']=datetime.now(timezone.utc).isoformat();save()
    return 0 if result['status']=='PASS' else 1


if __name__=='__main__':raise SystemExit(main())
