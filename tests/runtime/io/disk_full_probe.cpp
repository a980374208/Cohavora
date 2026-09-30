#include "src/telemetry/diagnostic_file_sink.h"
#include <windows.h>
#include <winioctl.h>
#include <filesystem>
#include <vector>
#include <thread>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <chrono>
using namespace livekit::diagnostic;
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
void check(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
struct Handle { HANDLE h=INVALID_HANDLE_VALUE; ~Handle(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);} void close(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);h=INVALID_HANDLE_VALUE;} };
uint64_t freebytes(){ULARGE_INTEGER a{},b{},c{};check(GetDiskFreeSpaceExW(L"G:\\",&a,&b,&c),"free space query");check(b.QuadPart>64*1024*1024 && b.QuadPart<128*1024*1024,"unexpected G volume capacity");return c.QuadPart;}
uint64_t total(const fs::path& p){uint64_t n=0;for(auto& e:fs::recursive_directory_iterator(p))if(e.path().extension()==L".jsonl")n+=fs::file_size(e.path());return n;}
void full(const fs::path& root){
 DiagnosticFileSink sink(root/L"logs",std::string(32,'b'));
 auto ev=Event::Received(ChatKind::Text,1);ev.event_sequence=1;
 check(sink.Write(ev),"initial sink write");
 Handle filler{CreateFileW((root/L"filler.bin").c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_DELETE_ON_CLOSE,nullptr)};
 check(filler.h!=INVALID_HANDLE_VALUE,"filler create");
 std::vector<char> data(1024*1024,'x');uint64_t filled=0;DWORD error=0;
 for(DWORD chunk:{DWORD(data.size()),DWORD(4096),DWORD(1)}){
  while(true){DWORD n=0;BOOL ok=WriteFile(filler.h,data.data(),chunk,&n,nullptr);error=ok?0:GetLastError();filled+=n;if(!ok){check(error==ERROR_DISK_FULL||error==ERROR_HANDLE_DISK_FULL,"unexpected filler error");break;}check(n==chunk,"filler short write");check(filled<128*1024*1024,"filler bound");}
 }
 std::cout<<"disk_full_win32_error="<<error<<" filler_bytes="<<filled<<" free="<<freebytes()<<std::endl;
 bool failed=false;uint64_t last=1;
 for(unsigned seq=2;seq<256;++seq){ev.event_sequence=seq;if(!sink.Write(ev)){failed=true;break;}last=seq;}
 std::cout<<"sink_failure="<<FailureReasonName(sink.failure_reason())<<" committed="<<sink.last_committed_sequence()<<" expected="<<last<<" residue_bytes="<<total(root/L"logs")<<std::endl;
 check(failed,"sink must encounter real full volume");check(sink.failure_reason()==FailureReason::WriteFailed||sink.failure_reason()==FailureReason::FlushFailed,"sink I/O failure expected");check(sink.last_committed_sequence()==last,"failed write advances committed");
 filler.close();ev.event_sequence=1000;check(sink.Write(ev),"same sink recovery");check(sink.last_committed_sequence()==1000,"recovery commit");sink.Close();
 DiagnosticFileSink reopened(root/L"logs",std::string(32,'c'));ev.event_sequence=1001;check(reopened.Write(ev),"fresh sink recovery");
 std::cout<<"disk_full_recovery=PASS committed="<<reopened.last_committed_sequence()<<" free="<<freebytes()<<" residue_and_recovery_bytes="<<total(root/L"logs")<<std::endl;
}
void cancel(const fs::path& root){
 constexpr DWORD bytes=16*1024*1024;
 void* buffer=VirtualAlloc(nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);check(buffer!=nullptr,"aligned allocation");
 struct Release{void*p;~Release(){VirtualFree(p,0,MEM_RELEASE);}} release{buffer};memset(buffer,0x35,bytes);
 Handle file{CreateFileW((root/L"cancel.bin").c_str(),GENERIC_WRITE|GENERIC_READ,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH|FILE_FLAG_DELETE_ON_CLOSE,nullptr)};
 check(file.h!=INVALID_HANDLE_VALUE,"cancel file open");unsigned aborted=0,successful=0,notfound=0;
 for(unsigned attempt=0;attempt<20 && aborted<3;++attempt){
  LARGE_INTEGER zero{};check(SetFilePointerEx(file.h,zero,nullptr,FILE_BEGIN),"rewind");
  std::atomic<bool> started=false,done=false;DWORD writeError=0,written=0;BOOL writeOK=FALSE;
  auto start=Clock::now();
  std::thread worker([&]{started=true;writeOK=WriteFile(file.h,buffer,bytes,&written,nullptr);writeError=writeOK?0:GetLastError();done=true;});
  while(!started.load())SwitchToThread();
  DWORD cancelError=ERROR_NOT_FOUND;unsigned accepted=0;
  while(!done.load()){
   if(CancelSynchronousIo(worker.native_handle())){++accepted;++successful;}else {cancelError=GetLastError();if(cancelError==ERROR_NOT_FOUND)++notfound;}
   Sleep(1);
  }
  worker.join();
  auto ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
  LARGE_INTEGER actual{};check(GetFileSizeEx(file.h,&actual),"cancel residue query");
  std::cout<<"cancel_attempt="<<attempt<<" accepted="<<accepted<<" last_cancel_error="<<cancelError<<" write_ok="<<writeOK<<" write_error="<<writeError<<" bytes_reported="<<written<<" file_size="<<actual.QuadPart<<" elapsed_ms="<<ms<<std::endl;
  if(!writeOK && writeError==ERROR_OPERATION_ABORTED)++aborted;
  else check(writeOK,"unexpected write failure during cancellation");
 }
 LARGE_INTEGER zero{};check(SetFilePointerEx(file.h,zero,nullptr,FILE_BEGIN),"recovery rewind");DWORD written=0;check(WriteFile(file.h,buffer,4096,&written,nullptr)&&written==4096,"write after cancel");check(FlushFileBuffers(file.h),"flush after cancel");
 std::cout<<"synchronous_disk_cancel_aborted="<<aborted<<" accepted_calls="<<successful<<" no_request_calls="<<notfound<<" recovery=PASS"<<std::endl;
 check(aborted>=3,"real synchronous disk cancellation not reproduced three times");
}
void oplockCancel(const fs::path& root){
 for(int attempt=0;attempt<3;++attempt){
  auto path=root/(L"oplock-"+std::to_wstring(attempt)+L".bin");
  Handle holder{CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_FLAG_OVERLAPPED,nullptr)};
  check(holder.h!=INVALID_HANDLE_VALUE,"oplock holder");
  Handle event{CreateEventW(nullptr,TRUE,FALSE,nullptr)};check(event.h!=nullptr,"oplock event");
  OVERLAPPED ov{};ov.hEvent=event.h;DWORD returned=0;
  BOOL requested=DeviceIoControl(holder.h,FSCTL_REQUEST_OPLOCK_LEVEL_1,nullptr,0,nullptr,0,&returned,&ov);
  check(!requested && GetLastError()==ERROR_IO_PENDING,"pending oplock");
  std::atomic<bool> done=false;DWORD openError=0;bool opened=false;
  std::thread worker([&]{HANDLE h=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);openError=h==INVALID_HANDLE_VALUE?GetLastError():0;opened=h!=INVALID_HANDLE_VALUE;if(opened)CloseHandle(h);done=true;});
  auto breakWait=WaitForSingleObject(event.h,5000);
  bool pending=breakWait==WAIT_OBJECT_0 && !done.load();
  auto start=Clock::now();BOOL accepted=FALSE;DWORD cancelError=0;
  if(pending){accepted=CancelSynchronousIo(worker.native_handle());cancelError=accepted?0:GetLastError();}
  auto deadline=Clock::now()+std::chrono::seconds(5);
  while(!done.load() && Clock::now()<deadline)Sleep(1);
  bool exitedBeforeRelease=done.load();
  holder.close(); // Always release the oplock before join, including failed cancellation.
  worker.join();
  double elapsed=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
  std::cout<<"oplock_attempt="<<attempt<<" break_observed="<<(breakWait==WAIT_OBJECT_0)<<" pending_before_cancel="<<pending<<" cancel_accepted="<<accepted<<" cancel_error="<<cancelError<<" open_error="<<openError<<" exited_before_release="<<exitedBeforeRelease<<" elapsed_ms="<<elapsed<<std::endl;
  check(pending && accepted && !opened && openError==ERROR_OPERATION_ABORTED && exitedBeforeRelease,"oplock blocked file open must abort");
  Handle recovered{CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};check(recovered.h!=INVALID_HANDLE_VALUE,"oplock recovery open");DWORD written=0;check(WriteFile(recovered.h,"recovered",9,&written,nullptr)&&written==9&&FlushFileBuffers(recovered.h),"oplock recovery write");
 }
 std::cout<<"oplock_blocked_file_open_cancel=PASS recovery=PASS"<<std::endl;
}
int wmain(int argc,wchar_t**argv){try{check(argc==2||argc==3,"root argument");fs::path root=argv[1];check(root.parent_path()==fs::path(L"G:\\")&&root.filename().wstring().starts_with(L"codex-quota-io-"),"root guard");check(fs::is_directory(root),"runner must create root");std::cout<<"start_free="<<freebytes()<<std::endl;if(argc==3){oplockCancel(root);}else{full(root);}std::cout<<"runtime_gate=PASS"<<std::endl;return 0;}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
