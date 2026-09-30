// Manual diagnostic only. Default invocation enumerates attached display
// adapters. A reset requires the explicit option AND the exact displayed LUID.
// Never registered with CTest: a TDR can interrupt every app on that adapter.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

std::string Id(LUID id) {
    std::ostringstream out;
    out<<std::hex<<std::setfill('0')<<std::setw(8)<<static_cast<unsigned long>(id.HighPart)
       <<":"<<std::setw(8)<<id.LowPart;
    return out.str();
}
int main(int argc,char** argv) {
    const bool reset=argc==3&&std::string(argv[1])=="--force-adapter-tdr";
    if(argc!=1&&!reset) {
        std::cerr<<"Usage: driver_reset_control [--force-adapter-tdr exact-high:low-LUID]\n";return 2;
    }
    const auto gdi=GetModuleHandleW(L"gdi32.dll");
    const auto open=reinterpret_cast<PFND3DKMT_OPENADAPTERFROMGDIDISPLAYNAME>(GetProcAddress(gdi,"D3DKMTOpenAdapterFromGdiDisplayName"));
    const auto close=reinterpret_cast<PFND3DKMT_CLOSEADAPTER>(GetProcAddress(gdi,"D3DKMTCloseAdapter"));
    const auto escape=reinterpret_cast<PFND3DKMT_ESCAPE>(GetProcAddress(gdi,"D3DKMTEscape"));
    if(!open||!close||!escape) {std::cerr<<"KMT entry unavailable\n";return 3;}
    for(DWORD index=0;;++index) {
        DISPLAY_DEVICEW display{};display.cb=sizeof(display);
        if(!EnumDisplayDevicesW(nullptr,index,&display,0)) break;
        if(!(display.StateFlags&DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME adapter{};
        wcscpy_s(adapter.DeviceName,display.DeviceName);
        const auto opened=open(&adapter);
        if(opened<0) {std::cerr<<"open status=0x"<<std::hex<<static_cast<unsigned long>(opened)<<"\n";continue;}
        const auto id=Id(adapter.AdapterLuid);
        std::wcout<<L"DISPLAY="<<display.DeviceName<<L" ADAPTER="<<display.DeviceString<<L"\n";
        std::cout<<"LUID="<<id<<" primary="<<bool(display.StateFlags&DISPLAY_DEVICE_PRIMARY_DEVICE)<<std::endl;
        D3DKMT_CLOSEADAPTER closed{adapter.hAdapter};
        if(reset&&id==argv[2]) {
            // No unconditional override, scheduler stall, registry change,
            // driver disable, repeated reset, or stress shader is used.
            D3DKMT_TDRDBGCTRLTYPE type=D3DKMT_TDRDBGCTRLTYPE_FORCETDR;
            D3DKMT_ESCAPE request{};request.hAdapter=adapter.hAdapter;
            request.Type=D3DKMT_ESCAPE_TDRDBGCTRL;
            request.pPrivateDriverData=&type;request.PrivateDriverDataSize=sizeof(type);
            std::cout<<"REQUESTING_ONE_ADAPTER_TDR "<<id<<std::endl;
            const auto status=escape(&request);
            std::cout<<"TDR_REQUEST_NTSTATUS=0x"<<std::hex<<static_cast<unsigned long>(status)<<std::endl;
            close(&closed);
            // Success only means accepted. Separate observer must prove driver
            // loss and native CPU fallback; this tool never claims that verdict.
            return status<0?4:0;
        }
        close(&closed);
    }
    if(reset) {std::cerr<<"No attached adapter matches requested LUID; no reset issued\n";return 5;}
    std::cout<<"READ_ONLY: no reset requested\n";
}
