import ctypes as c,json,pathlib,sys,collections
import argparse, os
parser=argparse.ArgumentParser(description='Symbolize retained user-mode handle stacks.')
parser.add_argument('trace',type=pathlib.Path)
parser.add_argument('--type',action='append',dest='types')
parser.add_argument('--debuggers',type=pathlib.Path,default=pathlib.Path(os.environ.get('ProgramFiles(x86)','C:/Program Files (x86)'))/'Windows Kits/10/Debuggers/x64')
parser.add_argument('--symbols',required=True,help='PDB search path')
parser.add_argument('--image-directory',type=pathlib.Path,help='Archived matching images; override same-name module paths when present')
parser.add_argument('--output',type=pathlib.Path,required=True)
args=parser.parse_args()
if args.image_directory and not args.image_directory.is_dir(): raise NotADirectoryError(args.image_directory)
if args.output.exists(): raise FileExistsError(args.output)
if args.output.with_suffix('.lines.json').exists(): raise FileExistsError(args.output.with_suffix('.lines.json'))
path=args.trace;r=json.loads(path.read_text());events=r['events']
selected_types=set(args.types or ['ALPC Port'])
stopped=[s for s in r['samples'] if s['phase'] in ('stopped','stopped_worker_alive')];warm=stopped[0];last=stopped[-1]
indices=set(last['open_event_indices'])-set(warm['open_event_indices'])
groups=collections.defaultdict(list)
for i in indices:
 e=events[i]
 if e['type'] not in selected_types:continue
 stack=tuple(s['address'] for s in e['stack'] if int(s['address'],16)<0x8000000000000000)
 groups[stack].append(i)
chosen=sorted(groups.items(),key=lambda kv:len(kv[1]),reverse=True)[:4]
dbgdir=args.debuggers
import os
dllpath=os.add_dll_directory(str(dbgdir));dbg=c.WinDLL(str(dbgdir/'dbghelp.dll'),use_last_error=True)
h=c.c_void_p(-1);U64=c.c_ulonglong;DWORD=c.c_ulong
dbg.SymSetOptions.argtypes=[DWORD];dbg.SymSetOptions(0x4|0x2|0x200|0x400|0x80000|0x10)
dbg.SymInitializeW.argtypes=[c.c_void_p,c.c_wchar_p,c.c_int];dbg.SymInitializeW.restype=c.c_int
cache=path.parent/'symbols';cache.mkdir(exist_ok=True)
search=args.symbols+';'+str(cache.resolve())
if not dbg.SymInitializeW(h,search,False):raise OSError(c.get_last_error())
dbg.SymLoadModuleExW.argtypes=[c.c_void_p,c.c_void_p,c.c_wchar_p,c.c_wchar_p,U64,DWORD,c.c_void_p,DWORD];dbg.SymLoadModuleExW.restype=U64
class Info(c.Structure):
 _fields_=[('SizeOfStruct',DWORD),('TypeIndex',DWORD),('Reserved',U64*2),('Index',DWORD),('Size',DWORD),('ModBase',U64),('Flags',DWORD),('Value',U64),('Address',U64),('Register',DWORD),('Scope',DWORD),('Tag',DWORD),('NameLen',DWORD),('MaxNameLen',DWORD),('Name',c.c_char*1)]
dbg.SymFromAddr.argtypes=[c.c_void_p,U64,c.POINTER(U64),c.c_void_p];dbg.SymFromAddr.restype=c.c_int
class Line(c.Structure):
 _fields_=[('SizeOfStruct',DWORD),('Key',c.c_void_p),('LineNumber',DWORD),('FileName',c.c_char_p),('Address',U64)]
dbg.SymGetLineFromAddr64.argtypes=[c.c_void_p,U64,c.POINTER(DWORD),c.POINTER(Line)];dbg.SymGetLineFromAddr64.restype=c.c_int
source_lines={}
modules={int(m['base'],16):m for m in r['modules']};loaded=set();symbols={}
def symbol(address):
 if address in symbols:return symbols[address]
 value=int(address,16);module=next((m for b,m in modules.items() if b<=value<int(m['end'],16)),None)
 if module and module['base'] not in loaded:
  image_path=pathlib.Path(module['path'])
  if args.image_directory and (args.image_directory/image_path.name).is_file(): image_path=args.image_directory/image_path.name
  base=int(module['base'],16);dbg.SymLoadModuleExW(h,None,str(image_path),None,base,int(module['end'],16)-base,None,0);loaded.add(module['base'])
 buf=c.create_string_buffer(c.sizeof(Info)+2048);info=Info.from_buffer(buf);info.SizeOfStruct=c.sizeof(Info);info.MaxNameLen=2048;displacement=U64()
 if dbg.SymFromAddr(h,value,c.byref(displacement),buf):
  name=c.string_at(c.addressof(buf)+Info.Name.offset,info.NameLen).decode(errors='replace');result=name+'+0x'+format(displacement.value,'x')
 else:result=(pathlib.Path(module['path']).name+'+0x'+format(value-int(module['base'],16),'x')) if module else address
 line=Line();line.SizeOfStruct=c.sizeof(Line);line_offset=DWORD()
 if dbg.SymGetLineFromAddr64(h,value,c.byref(line_offset),c.byref(line)):
  source_lines[address]={'symbol':result,'file':line.FileName.decode(errors='replace'),'line':line.LineNumber,'displacement':line_offset.value}
 symbols[address]=result;return result
output=[]
for stack,items in chosen:
 example=events[items[0]];closes=[]
 for i in items:
  e=events[i];close=next((x for x in events[i+1:] if x['kind']=='HandleClose' and x['handle']==e['handle'] and x['object']==e['object']),None)
  closes.append({'create_event_index':i,'handle':e['handle'],'create_epoch_ms':r['trace_start_epoch_ms']+e['time_us']/1000,'close_epoch_ms':r['trace_start_epoch_ms']+close['time_us']/1000 if close else None,'close_stack':[symbol(x['address']) for x in close['stack'] if int(x['address'],16)<0x8000000000000000] if close else None})
 output.append({'count':len(items),'types':dict(collections.Counter(events[i]['type'] for i in items)),'create_stack':[symbol(a) for a in stack],'lifetimes':closes})
target=args.output;target.write_text(json.dumps(output,indent=2))
target.with_suffix('.lines.json').write_text(json.dumps(source_lines,indent=2))
print(json.dumps([{'count':g['count'],'create_stack':g['create_stack'],'close_stack_example':next((x['close_stack'] for x in g['lifetimes'] if x['close_stack']),None)} for g in output],indent=2))
