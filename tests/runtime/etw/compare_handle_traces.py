import collections
import csv
import json
from pathlib import Path

import argparse
parser=argparse.ArgumentParser(description='Compare ALPC attribution with bounded process teardown.')
parser.add_argument('--input-directory',type=Path,required=True)
parser.add_argument('--capture',required=True,help='Prefix of -handles.json and -processes.csv')
parser.add_argument('--published',required=True)
parser.add_argument('--external')
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
if args.output.exists(): raise FileExistsError(args.output)
root=args.input_directory

def analyze(tag):
    data = json.loads((root / (tag + '-handles.json')).read_text())
    events = data['events']
    modules = data['modules']
    rows = list(csv.reader((root / (tag + '-processes.csv')).read_text().splitlines()))
    process = next(row for row in rows[1:] if f"({data['pid']})" in row[4] and 'test_' in row[4])
    start_us, end_us = int(process[0]), int(process[1])
    # ETW process End precedes its final handle-table teardown by ~1 ms.
    # Bound the teardown interval so a later reuse of this PID cannot match.
    scoped = [e for e in events if start_us <= e['time_us'] <= end_us + 100000]
    def normalize(stack):
        result = []
        for frame in stack:
            address = int(frame['address'], 16)
            if address >= 0x8000000000000000:
                continue
            module = next((m for m in modules if int(m['base'],16) <= address < int(m['end'],16)), None)
            result.append((Path(module['path']).name + '+0x' + format(address-int(module['base'],16),'x')) if module else frame['address'])
        return result
    stopped = [s for s in data['samples'] if s['phase'] == 'stopped']
    first, last = stopped[0], stopped[-1]
    groups = collections.defaultdict(list)
    closes = collections.defaultdict(list)
    for index, event in enumerate(events):
        if event['kind'] == 'HandleClose' and start_us <= event['time_us'] <= end_us + 100000:
            closes[(event['handle'],event['object'])].append(index)
    for index in sorted(set(last['open_event_indices'])-set(first['open_event_indices'])):
        event = events[index]
        if event['type'] != 'ALPC Port':
            continue
        close_index = next((j for j in closes[(event['handle'],event['object'])] if j > index), None)
        close = events[close_index] if close_index is not None else None
        groups[tuple(normalize(event['stack']))].append({
            'create_event_index':index, 'close_event_index':close_index,
            'close_after_last_stopped_ms':data['trace_start_epoch_ms']+close['time_us']/1000-last['epoch_ms'] if close else None,
            'close_user_stack':normalize(close['stack']) if close else None,
            'close_kernel_frames':sum(int(f['address'],16)>=0x8000000000000000 for f in close['stack']) if close else None,
            'close_after_process_end_ms':(close['time_us']-end_us)/1000 if close else None,
        })
    return {
        'pid':data['pid'], 'events_lost':data['events_lost'],
        'process_start_us':start_us, 'process_end_us':end_us,
        'excluded_events_outside_process_lifetime_and_100ms_teardown':len(events)-len(scoped),
        'creates':sum(e['kind']=='HandleCreate' for e in scoped),'closes':sum(e['kind']=='HandleClose' for e in scoped),
        'events_with_any_stack':sum(bool(e['stack']) for e in scoped),
        'duplicate_events_not_applied_full_pid_trace':data['duplicate_events_not_applied'],
        'samples':[{k:s[k] for k in ('cycle','phase','epoch_ms','handles','private_bytes','threads')} | {'alpc_count_from_create_close':s['type_counts'].get('ALPC Port',0)} for s in data['samples']],
        'alpc_groups':[{'count':len(items),'create_module_stack':list(stack),'lifetimes':items} for stack,items in sorted(groups.items(),key=lambda x:len(x[1]),reverse=True)],
    }

capture = analyze(args.capture)
published = analyze(args.published)
# Compare system call paths, excluding the distinct test harness executables.
def system_path(group):
    return tuple(s.lower() for s in group['create_module_stack'] if not s.lower().startswith('test_'))
matches = []
for a in capture['alpc_groups']:
    for b in published['alpc_groups']:
        if system_path(a) == system_path(b):
            matches.append({'capture_count':a['count'],'published_count':b['count'],'system_module_stack':list(system_path(a))})
report = {'capture':capture,'published':published,'matching_system_create_paths':matches,
          'limitations':['HandleDuplicate is not applied; reconstructed overall handle totals are not authoritative.',
                         'Module offsets are authoritative; exact system internal function ownership is not resolved.',
                         'Same-process target windows; external-target behavior is not established.']}
if args.external:
    external = analyze(args.external)
    report['external'] = external
    report['external_matching_system_create_paths'] = [
        {'capture_count':a['count'],'external_count':b['count'],'system_module_stack':list(system_path(a))}
        for a in capture['alpc_groups'] for b in external['alpc_groups'] if system_path(a)==system_path(b)]
    report['limitations'][-1] = 'External result uses an independent controlled target process; it is not a long-soak or arbitrary-application acceptance.'
output = args.output
output.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'output':str(output),'matching_groups':[(m['capture_count'],m['published_count']) for m in matches],
                  'capture_alpc_groups':[g['count'] for g in capture['alpc_groups']],
                  'published_alpc_groups':[g['count'] for g in published['alpc_groups']]},indent=2))
