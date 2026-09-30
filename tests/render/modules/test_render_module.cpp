#include "render/backend_module.h"
#include "render/api/render_backend_dx11_native.h"
#include "render/api/render_backend_module_info.h"
#include "tests/support/test_check.h"
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <iostream>
#include <thread>

using namespace livekit::render;
using Microsoft::WRL::ComPtr;
namespace {
struct Window {
    HWND handle = CreateWindowExW(0, L"STATIC", L"P2 isolated module test", WS_POPUP,
        0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Window() { TEST_CHECK(handle); }
    ~Window() { DestroyWindow(handle); }
};
lk_render_frame_view Frame(const uint8_t* data, uint32_t alpha) {
    lk_render_frame_view f{};
    f.struct_size=sizeof(f); f.format=LK_RENDER_RGBA8; f.width=f.height=f.plane_count=1;
    f.alpha_mode=alpha; f.planes[0]={data,4,4,1,1,0};
    return f;
}
std::array<uint8_t,4> Pixel(BackendDevice& instance, int x, int y) {
    lk_render_dx11_native_v1 native{};
    TEST_CHECK(instance.QueryExtension(LK_RENDER_EXT_DX11_NATIVE,LK_RENDER_DX11_NATIVE_V1,sizeof(native),&native)==LK_RENDER_OK);
    auto device=reinterpret_cast<ID3D11Device*>(native.device);
    auto context=reinterpret_cast<ID3D11DeviceContext*>(native.context);
    ComPtr<ID3D11RenderTargetView> view; context->OMGetRenderTargets(1,view.GetAddressOf(),nullptr);
    TEST_CHECK(view);
    ComPtr<ID3D11Resource> resource; view->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> texture; TEST_CHECK(SUCCEEDED(resource.As(&texture)));
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ; desc.MiscFlags=0;
    ComPtr<ID3D11Texture2D> copy; TEST_CHECK(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,copy.GetAddressOf())));
    context->CopyResource(copy.Get(),texture.Get());
    D3D11_MAPPED_SUBRESOURCE map{}; TEST_CHECK(SUCCEEDED(context->Map(copy.Get(),0,D3D11_MAP_READ,0,&map)));
    auto pixel=static_cast<const uint8_t*>(map.pData)+y*map.RowPitch+x*4;
    std::array<uint8_t,4> result{pixel[0],pixel[1],pixel[2],pixel[3]};
    context->Unmap(copy.Get(),0); return result;
}
void Near(const std::array<uint8_t,4>& pixel, int r,int g,int b) {
    TEST_CHECK(std::abs(int(pixel[0])-r)<=2 && std::abs(int(pixel[1])-g)<=2 && std::abs(int(pixel[2])-b)<=2);
}
}
int main(int argc,char** argv) {
    TEST_CHECK(argc==6);
    const auto path=std::filesystem::absolute(argv[1]);
    ModuleLoadError error;
    TEST_CHECK(!BackendModule::Load("relative.dll",LK_RENDER_BACKEND_DX11,error) && error==ModuleLoadError::InvalidPath);
    TEST_CHECK(!BackendModule::Load(path.parent_path()/"absent-renderer.dll",LK_RENDER_BACKEND_DX11,error) && error==ModuleLoadError::OpenFailed);
    const ModuleLoadError expected[]{ModuleLoadError::EntryMissing,ModuleLoadError::InvalidAbi,ModuleLoadError::InvalidAbi,ModuleLoadError::InvalidCapabilities};
    for(int i=2;i<6;++i) {
        const auto fixture=std::filesystem::absolute(argv[i]);
        TEST_CHECK(!BackendModule::Load(fixture,LK_RENDER_BACKEND_DX11,error) && error==expected[i-2]);
        TEST_CHECK(!GetModuleHandleW(fixture.c_str()));
    }
    TEST_CHECK(!BackendModule::Load(path,LK_RENDER_BACKEND_OPENGL,error) && error==ModuleLoadError::InvalidCapabilities);
    TEST_CHECK(!GetModuleHandleW(path.c_str()));
    auto module=BackendModule::Load(path,LK_RENDER_BACKEND_DX11,error);
    TEST_CHECK(module && error==ModuleLoadError::None);
    TEST_CHECK(BackendModule::Load(path,LK_RENDER_BACKEND_DX11,error)==module);
    TEST_CHECK(!BackendModule::Load(std::filesystem::absolute(argv[2]),LK_RENDER_BACKEND_DX11,error) && error==ModuleLoadError::SelectionLocked);
    std::weak_ptr<BackendModule> retained=module;
    Window first,second;
    lk_render_win32_binding binding{sizeof(binding),0,reinterpret_cast<uintptr_t>(first.handle)};
    lk_render_create_info info{sizeof(info),LK_RENDER_SURFACE_WIN32,&binding,sizeof(binding),2,10};
    lk_render_result status;
    auto malformed=info; malformed.binding=nullptr;
    TEST_CHECK(!module->Create(malformed,status) && status==LK_RENDER_INVALID_ARGUMENT);
    auto one=module->Create(info,status); TEST_CHECK(one && status==LK_RENDER_OK);
    lk_render_module_info_v1 moduleInfo{};
    TEST_CHECK(one->QueryExtension(LK_RENDER_EXT_MODULE_INFO,LK_RENDER_MODULE_INFO_V1,
        sizeof(moduleInfo),&moduleInfo)==LK_RENDER_OK);
    TEST_CHECK(moduleInfo.struct_size==sizeof(moduleInfo) && moduleInfo.module_version==LK_RENDER_MODULE_VERSION &&
        moduleInfo.abi_version==LK_RENDER_ABI_V1 && moduleInfo.reserved==0);
    binding.hwnd=reinterpret_cast<uintptr_t>(second.handle); info.surface_generation=20;
    auto two=module->Create(info,status); TEST_CHECK(two && status==LK_RENDER_OK);
    module.reset(); TEST_CHECK(!retained.expired() && GetModuleHandleW(path.c_str()));

    lk_render_frame_target target{sizeof(target),64,64,0,10,0};
    auto stale=target; stale.surface_generation=11;
    TEST_CHECK(one->Resize(stale)==LK_RENDER_SURFACE_LOST);
    auto suspended=target; suspended.pixel_width=0;
    TEST_CHECK(one->Resize(suspended)==LK_RENDER_NOT_READY);
    std::thread wrongOwner([&] { TEST_CHECK(one->Resize(target)==LK_RENDER_INVALID_ARGUMENT); }); wrongOwner.join();
    const uint8_t red[]{255,0,0,0}; // OPAQUE video ignores producer alpha.
    const uint8_t green[]{0,128,0,128};
    const auto video=Frame(red,LK_RENDER_ALPHA_OPAQUE), overlay=Frame(green,LK_RENDER_ALPHA_PREMULTIPLIED);
    TEST_CHECK(one->Upload({1,1},video)==LK_RENDER_OK);
    TEST_CHECK(one->Upload({2,1},overlay,true)==LK_RENDER_OK);
    TEST_CHECK(one->Upload({3,1},video)==LK_RENDER_OUT_OF_MEMORY);
    auto truncated=video; truncated.planes[0].byte_count=3;
    TEST_CHECK(one->Upload({1,1},truncated)==LK_RENDER_INVALID_ARGUMENT);
    TEST_CHECK(one->Upload({1,2},video)==LK_RENDER_INVALID_ARGUMENT);
    one->Remove({1,99}); // Wrong generation cannot remove current frame.
    const lk_render_draw_command commands[]{
        {{0,0},{0,0,64,64},{0,0,64,64},{0,0,1,1}},
        {{1,1},{0,0,64,64},{0,0,32,64},{0.5f,0.5f,0.5f,0.5f}},
        {{2,1},{0,0,64,64},{0,0,16,64},{1,1,1,1}}};
    const lk_render_scene_view scene{sizeof(scene),3,commands,{0,0,0,1}};
    TEST_CHECK(one->Render(target,scene)==LK_RENDER_OK);
    Near(Pixel(*one,8,8),64,128,64); Near(Pixel(*one,24,8),128,0,128); Near(Pixel(*one,50,8),0,0,255);
    lk_render_dx11_native_v1 untouched{123,456,789,101};
    TEST_CHECK(one->QueryExtension(LK_RENDER_EXT_GL_NATIVE,1,sizeof(untouched),&untouched)==LK_RENDER_UNSUPPORTED);
    TEST_CHECK(untouched.struct_size==123 && untouched.context==101);
    one->Remove({1,1}); one->Remove({1,1});
    TEST_CHECK(one->Render(target,scene)==LK_RENDER_INVALID_ARGUMENT);
    TEST_CHECK(one->Upload({1,2},video)==LK_RENDER_OK);
    one.reset(); TEST_CHECK(!retained.expired() && GetModuleHandleW(path.c_str()));
    two->Reset(); TEST_CHECK(retained.expired() && !GetModuleHandleW(path.c_str()));
    TEST_CHECK(two->Resize(target)==LK_RENDER_INVALID_ARGUMENT); two.reset();
    // Selection stays fixed, but reopening a meeting can reload the same DLL.
    TEST_CHECK(!BackendModule::Load(std::filesystem::absolute(argv[2]),LK_RENDER_BACKEND_DX11,error) && error==ModuleLoadError::SelectionLocked);
    module=BackendModule::Load(path,LK_RENDER_BACKEND_DX11,error); TEST_CHECK(module);
    module.reset(); TEST_CHECK(!GetModuleHandleW(path.c_str()));
    std::cout<<"P2_MODULE PASS: validation, selection, bounds, generations, owner, atomic upload, clip/modulation/alpha, two-device retention, unload/reload\n";
}
