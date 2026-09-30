import csv,json,pathlib,re,subprocess,sys,collections
import argparse, os
parser=argparse.ArgumentParser(description='Decode WPR handle traces; duplicate handles are not reconstructed.')
parser.add_argument('etl', type=pathlib.Path)
parser.add_argument('log', type=pathlib.Path)
parser.add_argument('--tag')
parser.add_argument('--xperf', default=str(pathlib.Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)'))/'Windows Kits/10/Windows Performance Toolkit/xperf.exe'))
parser.add_argument('--range', nargs=2, dest='time_range')
args=parser.parse_args()
etl,log=args.etl,args.log; tag=args.tag or etl.stem
if pathlib.Path(tag).name != tag or not re.fullmatch(r'[A-Za-z0-9_.-]+',tag):
 raise ValueError('tag must be a filename component')
for suffix in ('-decode.log','-handles.json'):
 if etl.with_name(tag+suffix).exists(): raise FileExistsError(etl.with_name(tag+suffix))
samples=[]
log_bytes=log.read_bytes()
log_text=log_bytes.decode('utf-16' if log_bytes.startswith((b'\xff\xfe',b'\xfe\xff')) else 'utf-8-sig',errors='replace')
for line in log_text.splitlines():
 if line.startswith(('[LIFECYCLE] ','[CAPTURE_LIFECYCLE] ')):samples.append(json.loads(line.split('] ',1)[1]))
if not samples: raise ValueError('No lifecycle samples in log')
pid=samples[0]['pid'];target=f'({pid})'
if any(s['pid'] != pid for s in samples): raise ValueError('Mixed lifecycle process IDs')
xperf=args.xperf
errors=etl.with_name(tag+'-decode.log').open('w')
range_args=['-range',*args.time_range] if args.time_range else []
process=subprocess.Popen([xperf,'-i',str(etl),'-a','dumper','-stacktimeshifting',*range_args],stdout=subprocess.PIPE,stderr=errors,text=True,encoding='utf-8',errors='replace')
events=[];modules=[];module_events=[];pending=None;origin=None;lost=None;duplicates=0
header_path=etl.with_name(etl.stem+'-header.csv')
if header_path.exists():
 for line in header_path.read_text(errors='replace').splitlines():
  if line.startswith('OS Version:'):
   origin=(int(re.search(r'Trace Start: (\d+)',line)[1])-116444736000000000)/10000
   lost=int(re.search(r'Events Lost: (\d+)',line)[1])
for line in process.stdout:
 if line.startswith('OS Version:'):
  origin=(int(re.search(r'Trace Start: (\d+)',line)[1])-116444736000000000)/10000
  lost=int(re.search(r'Events Lost: (\d+)',line)[1])
 is_stack=line.lstrip().startswith('Stack,')
 if is_stack and pending is None:continue
 if not is_stack and target not in line:
  pending=None
  continue
 fields=[x.strip() for x in next(csv.reader([line]))]
 if not fields:continue
 kind=fields[0]
 if kind=='Stack':
  if pending and len(fields)>5 and fields[1]==str(pending['time_us']):
   pending['stack'].append({'address':fields[4],'image_symbol':fields[5]})
  continue
 pending=None
 if len(fields)<3 or target not in fields[2]:continue
 if kind in ('I-Start','I-DCStart'):
  modules.append({'base':fields[3],'end':fields[4],'path':fields[8].strip('"')})
 if kind in ('I-Start','I-End','I-DCStart','I-DCEnd'):
  module_events.append({'kind':kind,'time_us':int(fields[1]),'base':fields[3],'end':fields[4],'path':fields[8].strip('"')})
 if kind in ('HandleCreate','HandleClose'):
  if int(fields[5],16)&0x80000000:continue
  pending={'kind':kind,'time_us':int(fields[1]),'tid':int(fields[3]),'object':fields[4],'handle':fields[5],'type':fields[6],'stack':[]}
  events.append(pending)
 if kind=='HandleDuplicate':duplicates+=1
status=process.wait();errors.close()
if status:raise RuntimeError(f'xperf failed {status}')
if origin is None: raise ValueError('Missing ETW trace start header')
events.sort(key=lambda e:e['time_us'])
active={};checkpoints=[];index=0;unmatched_close=0;replaced_open=0
for sample in samples:
 bound=(sample['epoch_ms']-origin)*1000
 while index<len(events) and events[index]['time_us']<=bound:
  e=events[index];key=e['handle']
  if e['kind']=='HandleCreate':
   if key in active:replaced_open+=1
   active[key]=index
  else:
   if key not in active:unmatched_close+=1
   active.pop(key,None)
  index+=1
 checkpoints.append({**sample,'trace_open_count':len(active),'open_event_indices':list(active.values()),'type_counts':dict(collections.Counter(events[i]['type'] for i in active.values()))})
warm=next(s for s in checkpoints if s['cycle']==1 and s['phase'] in ('stopped','stopped_worker_alive'))
settled=checkpoints[-1]
retained=sorted(set(settled['open_event_indices'])-set(warm['open_event_indices']))
retired=sorted(set(warm['open_event_indices'])-set(settled['open_event_indices']))
result={'pid':pid,'trace_start_epoch_ms':origin,'events_lost':lost,'create_events':sum(e['kind']=='HandleCreate' for e in events),'close_events':sum(e['kind']=='HandleClose' for e in events),'events_with_stack':sum(bool(e['stack']) for e in events),'duplicate_events_not_applied':duplicates,'unmatched_close_before_final_checkpoint':unmatched_close,'replaced_open_before_final_checkpoint':replaced_open,'modules':modules,'module_events':module_events,'samples':checkpoints,'retained_after_warmup':retained,'retired_after_warmup':retired,'events':events}
path=etl.with_name(tag+'-handles.json');path.write_text(json.dumps(result,separators=(',',':')))
print(json.dumps({'file':str(path),'pid':pid,'lost':lost,'creates':result['create_events'],'closes':result['close_events'],'stacked_events':result['events_with_stack'],'retained_after_warmup':len(retained),'retired_after_warmup':len(retired),'warm_handle_count':warm['handles'],'settled_handle_count':settled['handles'],'warm_trace_count':warm['trace_open_count'],'settled_trace_count':settled['trace_open_count'],'retained_types':dict(collections.Counter(events[i]['type'] for i in retained))},indent=2))
