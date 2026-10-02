"""B15 separate UIA interference and same-process residual/resource cycles."""
from __future__ import annotations
import argparse
from datetime import datetime,timezone
import json
from pathlib import Path
import statistics
import subprocess
import time
import uuid
from run_b_acceptance_telemetry import TelemetryPeers,samples
from run_b_acceptance_files import events,sha
from run_b_acceptance_recovery import snapshot
import invoke_screen_share_quality_probe as service


def resource_delta(first,last):
    return {key:last[key]-first[key] for key in ('private_bytes','working_set_bytes','handles','threads')}


def retire(peers):
    end=time.monotonic()+30
    while time.monotonic()<end:
        sequence=peers.send('receiver','retirement_observe')
        value=peers.wait('receiver',lambda rows:next((x for x in rows if x['event']=='retirement_observation'
            and x['sequence']==sequence),None),'native_retirement_observation')
        if not value['rooms_alive'] and not value['cleanup_pending']:return value
        time.sleep(.25)
    raise RuntimeError('same_process_native_release_timeout')


def finish_receiver(peers,count):
    peers.send('receiver','telemetry_finalize')
    closed=peers.wait('receiver',lambda rows:next((x for x in rows if x['event']=='telemetry_store_closed'),None),'persistence_close')
    if not closed['history_completed'] or closed['disk_unknown'] or not closed['diagnostic_completed']:
        raise RuntimeError('persistence_drain_failed')
    peers.send('receiver','finish')
    retired=peers.wait('receiver',lambda rows:next((x for x in rows if x['event']=='retired'),None),'retired')
    child=peers.processes['receiver'];child.wait(timeout=10)
    if child.returncode or retired['rooms_observed']!=count or retired['rooms_alive'] or retired['cleanup_pending']:
        raise RuntimeError('residual_native_retirement_failed')
    peers.lifecycle['receiver'].update(retirement=retired,exit_code=child.returncode,persistence_close=closed)


def run(case,root,binary):
    root.mkdir();room='b15-'+uuid.uuid4().hex
    result=dict(status='RUNNING',case=case,room=room,cycles=[])
    manifest=root/'result.json'
    def save():manifest.write_text(json.dumps(result,indent=2),encoding='utf-8')
    save();peers=None
    try:
        result['server_before']=snapshot(room)
        peers=TelemetryPeers(root,binary,room,service.credentials(room),'on',1000)
        peers.launch('receiver');peers.launch('publisher')
        result['media_baseline']=peers.advancing_media();peers.wait_duration(20)
        if case=='uia-interference':
            peers.send('receiver','telemetry_open');peers.wait_duration(10)
            peers.send('receiver','telemetry_begin',phase='before-uia')
            peers.wait_duration(30);result['before']=peers.observe_performance()
            peers.send('receiver','telemetry_begin',phase='uia-interference')
            script=Path(__file__).resolve().parents[1]/'desktop/run_b_telemetry_uia_interference.ps1'
            process=subprocess.run(['pwsh','-NoProfile','-File',str(script),'-ProductProcessId',
                str(peers.processes['receiver'].pid),'-Output',str(root/'uia.json')],capture_output=True,
                text=True,timeout=45,creationflags=subprocess.CREATE_NO_WINDOW)
            if process.returncode:raise RuntimeError('bounded_UIA_client_failed')
            result['uia']=json.loads((root/'uia.json').read_text(encoding='utf-8-sig'))
            if result['uia']['rounds']!=20 or result['uia']['pid']!=peers.processes['receiver'].pid:
                raise RuntimeError('UIA_process_or_round_count_mismatch')
            result['during']=peers.observe_performance()
            peers.send('receiver','telemetry_begin',phase='after-uia');peers.wait_duration(60)
            result['after']=peers.observe_performance()
            result['media_after']=peers.advancing_media(result['media_baseline']['decoded_frames'])
            result['settled_UIA_delta']=resource_delta(result['before'],result['after'])
            if result['settled_UIA_delta']['private_bytes']>64*1024**2:
                raise RuntimeError('settled_UIA_private_growth_exceeded_64MiB')
            peers.send('receiver','telemetry_close');peers.send('receiver','leave')
            peers.wait('receiver',lambda rows:next((x for x in rows if x['event']=='left'),None),'left')
            result['retirement']=retire(peers);count=1
        else:
            count=5
            for cycle in range(count):
                before=peers.advancing_media()
                peers.send('receiver','telemetry_open')
                peers.send('receiver','telemetry_begin',phase='cycle-active-'+str(cycle+1))
                peers.wait_duration(40)
                after=peers.advancing_media(before['decoded_frames'])
                peers.send('receiver','telemetry_close')
                cutoff=len(events(peers.roots['receiver']))
                peers.send('receiver','leave')
                peers.wait('receiver',lambda rows:next((x for x in rows[cutoff:] if x['event']=='left'),None),'cycle_left')
                retirement=retire(peers)
                if retirement['rooms_observed']!=cycle+1:raise RuntimeError('retired_room_epoch_count_mismatch')
                peers.send('receiver','telemetry_begin',phase='cycle-settled-'+str(cycle+1))
                peers.wait_duration(15)
                released=peers.observe_performance()
                result['cycles'].append(dict(cycle=cycle+1,media_before=before,media_after=after,
                    native_retirement=retirement,released=released))
                print('residual cycle '+str(cycle+1)+' released',flush=True);save()
                if cycle+1<count:
                    peers.send('receiver','network_reopen_ui');peers.send('receiver','join')
                    peers.wait('receiver',lambda rows:next((x for x in rows if x['event']=='joined'
                        and x['join']==cycle+2),None),'cycle_join',45)
            result['residual_delta']=resource_delta(result['cycles'][0]['released'],result['cycles'][-1]['released'])
            if result['residual_delta']['private_bytes']>64*1024**2 or result['residual_delta']['handles']>64 or result['residual_delta']['threads']>8:
                raise RuntimeError('same_process_residual_resource_gate_failed')
        finish_receiver(peers,count);peers.finish('publisher');result['lifecycle']=peers.lifecycle
        result['server_after']=snapshot(room)
        if result['server_after']['participants_remaining'] or result['server_after']['publications_remaining']:
            raise RuntimeError('telemetry_room_not_empty')
        if any(result['server_before'][key]!=result['server_after'][key] for key in ('config_sha256','image','binary')):
            raise RuntimeError('server_fingerprint_changed')
        result['status']='PASS'
    except Exception as error:result.update(status='FAIL',failure=type(error).__name__+':'+str(error))
    finally:
        if peers:peers.cleanup()
        result['finished_utc']=datetime.now(timezone.utc).isoformat();save()
    return result


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--binary',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--case',choices=('uia-interference','residual-growth'),action='append')
    args=parser.parse_args();root=args.output.resolve();binary=args.binary.resolve();root.mkdir(parents=True,exist_ok=False)
    service.SERVER_PORT=17980;service.SERVER_CONTAINER='cohavora-b-acceptance-20261001';service.CONFIG_PATH='/root/livekit-b-acceptance-20261001/livekit.yaml'
    verify=Path(__file__).resolve().parents[1]/'diagnostics/verify_runtime_binary.ps1'
    checked=subprocess.run(['pwsh','-NoProfile','-File',str(verify),'-Executable',str(binary),
        '-ExpectedExecutableName','uia_entry_fixture.exe','-Configuration','RelWithDebInfo'],capture_output=True,text=True)
    if checked.returncode:parser.exit(2,'RelWithDebInfo verification failed\n')
    inputs=[str(Path(__file__)),str(Path(__file__).with_name('run_b_acceptance_telemetry.py')),
        str(Path(__file__).resolve().parents[1]/'desktop/run_b_telemetry_uia_interference.ps1'),
        'tests/runtime/probes/b_telemetry_product_runtime.h','tests/runtime/probes/b_file_product_runtime.h',
        'tests/runtime/probes/b_network_product_runtime.h','src/core/room.cpp','src/core/room.h',
        'src/render/owned_i420_frame.cpp','src/render/owned_i420_frame.h',
        'src/core/meeting_coordinator.cpp','src/core/meeting_coordinator.h']
    index=dict(status='RUNNING',binary_identity=json.loads(checked.stdout),configuration='RelWithDebInfo',
        inputs={p:sha(Path(p)) for p in inputs},cases=[])
    path=root/'index.json'
    def save():path.write_text(json.dumps(index,indent=2),encoding='utf-8')
    save()
    for case in args.case or ('uia-interference','residual-growth'):
        value=run(case,root/case,binary)
        index['cases'].append(dict(case=case,status=value['status'],evidence=case+'/result.json',sha256=sha(root/case/'result.json')))
        print(case+' '+value['status'],flush=True);save()
        if value['status']!='PASS':break
    index['status']='PASS' if len(index['cases'])==len(args.case or ('uia-interference','residual-growth')) and \
        all(x['status']=='PASS' for x in index['cases']) and all(sha(Path(p))==h for p,h in index['inputs'].items()) and \
        sha(binary)==index['binary_identity']['binary_sha256'] else 'FAIL'
    save();return 0 if index['status']=='PASS' else 1


if __name__=='__main__':raise SystemExit(main())
