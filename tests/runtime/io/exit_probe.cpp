#include "src/telemetry/diagnostic_file_sink.h"
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <stdexcept>
using namespace livekit::diagnostic;
void require(bool b){if(!b)throw std::runtime_error("experiment precondition failed");}
namespace livekit::diagnostic { struct DiagnosticFileSinkTestAccess {
 static inline HANDLE ready, release;
 static bool Block(void* h,const char* p,std::uint32_t n,std::uint32_t& written) noexcept {
  SetEvent(ready);if(WaitForSingleObject(release,60000)!=WAIT_OBJECT_0)return false;
  return DiagnosticFileSink::NativeWrite(h,p,n,written);
 }
 static void Install(DiagnosticFileSink& s,HANDLE a,HANDLE z){ready=a;release=z;s.write_=&Block;}
}; }
int wmain(int argc,wchar_t**argv){try{
 require(argc==5);std::wstring mode=argv[1];std::filesystem::path root=argv[2];
 HANDLE ready=OpenEventW(EVENT_MODIFY_STATE|SYNCHRONIZE,FALSE,argv[3]);
 HANDLE release=OpenEventW(EVENT_MODIFY_STATE|SYNCHRONIZE,FALSE,argv[4]);require(ready&&release);
 if(mode==L"exit"){
  auto sink=std::make_shared<DiagnosticFileSink>(root,std::string(32,'d'));
  auto e=Event::Received(ChatKind::Text,1);e.event_sequence=1;require(sink->Write(e));
  DiagnosticFileSinkTestAccess::Install(*sink,ready,release);
  std::thread([sink,e]()mutable{e.event_sequence=2;sink->Write(e);}).detach();
  require(WaitForSingleObject(ready,5000)==WAIT_OBJECT_0);
  const char marker[]="returning_from_main\n";DWORD n;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),marker,sizeof(marker)-1,&n,nullptr);
  return 0;
 }
 DiagnosticFileSink sink(root,std::string(32,mode==L"holder"?'b':'c'));
 auto e=Event::Received(ChatKind::Text,1);e.event_sequence=1;
 if(mode==L"holder"){
  require(sink.Write(e));
  DiagnosticFileSinkTestAccess::Install(sink,ready,release);
  e.event_sequence=2;bool ok=sink.Write(e);
  std::cout<<"holder_write="<<ok<<" committed="<<sink.last_committed_sequence()<<std::endl;require(ok);return 0;
 }
 require(mode==L"contender");
 for(std::string command;std::getline(std::cin,command);){
  if(command=="quit")return 0;
  auto start=std::chrono::steady_clock::now();
  if(command=="write"){
   bool ok=sink.Write(e);auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
   std::cout<<"write "<<ok<<" "<<FailureReasonName(sink.failure_reason())<<" "<<ms<<" "<<sink.last_committed_sequence()<<std::endl;if(ok)++e.event_sequence;
  }else if(command=="clear"){
   auto r=DiagnosticFileSink::ClearInactiveHistory(root);auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
   std::cout<<"clear "<<r.success<<" "<<r.reason<<" "<<ms<<" "<<r.active_runs_skipped<<std::endl;
  }else require(false);
 }
 return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<std::endl;return 2;}}
