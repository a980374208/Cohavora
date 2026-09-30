import ctypes as C, ctypes.wintypes as W, subprocess as S, tempfile, pathlib, time, json, threading, queue, shutil, uuid
import argparse
parser=argparse.ArgumentParser(description='Controlled sink exit and root lock regression; not physical disk stall.')
parser.add_argument('--binary',type=pathlib.Path,required=True)
parser.add_argument('--output',type=pathlib.Path,required=True)
args=parser.parse_args()
exe=args.binary.resolve(strict=True)
base=args.output.resolve();base.mkdir(parents=True,exist_ok=False)
k=C.WinDLL('kernel32',use_last_error=True)
k.CreateEventW.argtypes=[C.c_void_p,W.BOOL,W.BOOL,W.LPCWSTR];k.CreateEventW.restype=W.HANDLE
k.SetEvent.argtypes=[W.HANDLE];k.CloseHandle.argtypes=[W.HANDLE]
k.WaitForSingleObject.argtypes=[W.HANDLE,W.DWORD];k.WaitForSingleObject.restype=W.DWORD
results=[]
def log(**r):results.append(r);print(json.dumps(r),flush=True);(base/'results.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
class Trial:
 def __init__(self):
  self.root=pathlib.Path(tempfile.mkdtemp(prefix='cohavora-exit-root-')).resolve();self.procs=[];self.events=[]
  ident=uuid.uuid4().hex;self.names=['Local\\CodexProbeReady'+ident,'Local\\CodexProbeRelease'+ident]
  for n in self.names:self.events.append(k.CreateEventW(None,True,False,n));assert self.events[-1]
 def child(self,mode):
  p=S.Popen([str(exe),mode,str(self.root),*self.names],stdin=S.PIPE,stdout=S.PIPE,stderr=S.STDOUT,text=True,creationflags=S.CREATE_NO_WINDOW);q=queue.Queue()
  def read():
   for line in p.stdout:q.put(line.strip())
   q.put(None)
  threading.Thread(target=read,daemon=True).start();p.lines=q;self.procs.append(p);return p
 def line(self,p):
  x=p.lines.get(timeout=10);assert x is not None,'unexpected child exit';return x
 def command(self,p,c):p.stdin.write(c+'\n');p.stdin.flush();return self.line(p)
 def cleanup(self):
  k.SetEvent(self.events[1])
  for p in self.procs:
   if p.poll() is None:
    try:p.communicate('quit\n',timeout=3)
    except S.TimeoutExpired:p.kill();p.wait(timeout=5)
  for h in self.events:k.CloseHandle(h)
  assert self.root.parent==pathlib.Path(tempfile.gettempdir()).resolve() and self.root.name.startswith('cohavora-exit-root-')
  shutil.rmtree(self.root)
  log(cleanup_root=str(self.root),removed=not self.root.exists())
for mode in ['exit']:
 for attempt in range(3):
  t=Trial()
  try:
   p=t.child(mode);assert k.WaitForSingleObject(t.events[0],5000)==0;marker=t.line(p);assert marker=='returning_from_main'
   start=time.monotonic()
   try:p.wait(timeout=2);pending=False
   except S.TimeoutExpired:pending=True
   observation_ms=round((time.monotonic()-start)*1000,3)
   assert not pending
   released=time.monotonic();k.SetEvent(t.events[1]);assert p.wait(timeout=5)==0
   log(test='normal_exit',mode=mode,attempt=attempt,marker=marker,pending_after_observation=pending,observation_ms=observation_ms,after_release_exit_ms=round((time.monotonic()-released)*1000,3),exit_code=p.returncode)
  finally:t.cleanup()
for termination in [False,True]:
 t=Trial()
 try:
  a=t.child('holder');assert k.WaitForSingleObject(t.events[0],5000)==0
  b=t.child('contender')
  blocked=[]
  for command in ['write','write','clear']:
   line=t.command(b,command);parts=line.split();assert parts[1]=='1';assert int(parts[3])<2000
   blocked.append(line);log(test='root_lock_blocked',terminate_owner=termination,result=line)
  if termination:a.kill();a.wait(timeout=5)
  else:
   k.SetEvent(t.events[1]);assert t.line(a)=='holder_write=1 committed=2';assert a.wait(timeout=5)==0
  write=t.command(b,'write');assert write.split()[1]=='1' and int(write.split()[3])<2000
  clear=t.command(b,'clear');assert clear.split()[1]=='1' and clear.split()[4]=='1'
  log(test='root_lock_recovery',terminate_owner=termination,write=write,clear=clear,owner_exit=a.returncode)
  b.stdin.write('quit\n');b.stdin.flush();assert b.wait(timeout=5)==0
 finally:t.cleanup()
log(verdict='PASS',scope='current native sink exit and data-write injection; no physical disk stall claimed')
