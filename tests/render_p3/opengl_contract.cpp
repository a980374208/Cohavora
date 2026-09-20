#include "src/render/api/render_backend_api.h"
#include "src/render/api/render_backend_module_info.h"
#include "src/render/backend_module.h"
#include "src/render/render_color_conversion.h"
#include "tests/support/test_check.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QLibrary>
#include <QtGui/QOffscreenSurface>
#include <QtGui/QOpenGLContext>
#include <QtGui/QOpenGLExtraFunctions>
#include <QtGui/QOpenGLFramebufferObject>
#include <QtGui/QImage>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
QOpenGLExtraFunctions* gl;
int deletes=0, failPlane=0;
bool advertiseReset=false;
unsigned resetStatus=0;
const GLubyte* LK_RENDER_GL_CALL GetStringi(GLenum name,GLuint index) {
    if(advertiseReset && name==GL_EXTENSIONS && index==0) return reinterpret_cast<const GLubyte*>("GL_KHR_robustness");
    return gl->glGetStringi(name,index);
}
unsigned LK_RENDER_GL_CALL GetResetStatus() { return resetStatus; }
void LK_RENDER_GL_CALL DeleteTextures(GLsizei count,const GLuint* names) { ++deletes; gl->glDeleteTextures(count,names); }
void LK_RENDER_GL_CALL DeleteProgram(GLuint name) { ++deletes; gl->glDeleteProgram(name); }
void LK_RENDER_GL_CALL DeleteVertexArrays(GLsizei count,const GLuint* names) { ++deletes; gl->glDeleteVertexArrays(count,names); }
void LK_RENDER_GL_CALL Upload(GLenum target,GLint level,GLint x,GLint y,GLsizei w,GLsizei h,GLenum format,GLenum type,const void* data) {
    if(failPlane && !--failPlane) { gl->glEnable(0xDEAD); return; }
    gl->glTexSubImage2D(target,level,x,y,w,h,format,type,data);
}
lk_render_gl_proc LK_RENDER_CALL Proc(void* user,const char* name) {
    if(!std::strcmp(name,"glGetStringi")) return reinterpret_cast<lk_render_gl_proc>(GetStringi);
    if(advertiseReset && !std::strncmp(name,"glGetGraphicsResetStatus",24)) return reinterpret_cast<lk_render_gl_proc>(GetResetStatus);
    if(!std::strcmp(name,"glDeleteTextures")) return reinterpret_cast<lk_render_gl_proc>(DeleteTextures);
    if(!std::strcmp(name,"glDeleteProgram")) return reinterpret_cast<lk_render_gl_proc>(DeleteProgram);
    if(!std::strcmp(name,"glDeleteVertexArrays")) return reinterpret_cast<lk_render_gl_proc>(DeleteVertexArrays);
    if(!std::strcmp(name,"glTexSubImage2D")) return reinterpret_cast<lk_render_gl_proc>(Upload);
    return reinterpret_cast<lk_render_gl_proc>(static_cast<QOpenGLContext*>(user)->getProcAddress(name));
}
struct Pixels {
    lk_render_frame_view view{};
    std::vector<uint8_t> planes[3];
    explicit Pixels(uint32_t format,uint32_t w=3,uint32_t h=5) {
        view.struct_size=sizeof(view); view.format=format; view.width=w; view.height=h;
        view.plane_count=format==LK_RENDER_I420?3:format==LK_RENDER_NV12?2:1;
        for(uint32_t i=0;i<view.plane_count;++i) {
            const auto pw=i?(w+1)/2:w, ph=i?(h+1)/2:h;
            const auto components=format==LK_RENDER_RGBA8?4:(format==LK_RENDER_NV12&&i?2:1);
            const auto stride=pw*components+3; // includes an odd-byte RG/RGBA stride
            planes[i].resize((ph-1)*stride+pw*components,0xCC); // no padding after final row
            view.planes[i]={planes[i].data(),planes[i].size(),stride,pw,ph,0};
            for(uint32_t y=0;y<ph;++y) for(uint32_t x=0;x<pw;++x) {
                auto* p=planes[i].data()+y*stride+x*components;
                if(format==LK_RENDER_RGBA8) {p[0]=240;p[1]=20;p[2]=10;p[3]=255;}
                else if(!i) *p=81;
                else if(format==LK_RENDER_NV12) {p[0]=90;p[1]=240;}
                else *p=i==1?90:240;
            }
        }
    }
};
bool Near(QColor a,QColor b,int tolerance=4) {
    return std::abs(a.red()-b.red())<=tolerance && std::abs(a.green()-b.green())<=tolerance && std::abs(a.blue()-b.blue())<=tolerance;
}
}
void RunOpenGlContract() {
    QSurfaceFormat format;
    const bool es=QOpenGLContext::openGLModuleType()==QOpenGLContext::LibGLES;
    format.setRenderableType(es?QSurfaceFormat::OpenGLES:QSurfaceFormat::OpenGL);
    format.setVersion(3,es?0:3); if(!es) format.setProfile(QSurfaceFormat::CoreProfile);
    QOpenGLContext context; context.setFormat(format); TEST_CHECK(context.create());
    QOffscreenSurface surface; surface.setFormat(context.format()); surface.create();
    TEST_CHECK(surface.isValid()&&context.makeCurrent(&surface));
    gl=context.extraFunctions(); gl->initializeOpenGLFunctions();
    std::cout<<"P3_GL_DRIVER "<<gl->glGetString(GL_VERSION)<<" / "<<gl->glGetString(GL_RENDERER)<<"\n";
    const auto path=livekit::render::BackendModule::PathForBackend(
        std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString()),LK_RENDER_BACKEND_OPENGL);
    QLibrary library(QString::fromStdWString(path.wstring())); TEST_CHECK(library.load());
    auto entry=reinterpret_cast<lk_render_get_api_fn>(library.resolve(LK_RENDER_ENTRY_POINT)); TEST_CHECK(entry);
    lk_render_api api{}; TEST_CHECK(entry(2,sizeof(api),&api)==LK_RENDER_ABI_MISMATCH);
    TEST_CHECK(entry(1,sizeof(api),&api)==LK_RENDER_OK);
    const lk_render_gl_binding binding{sizeof(binding),0,17,&context,Proc};
    lk_render_create_info info{sizeof(info),LK_RENDER_SURFACE_HOST_GL,&binding,sizeof(binding),2,11};
    lk_render_device* device=nullptr;
    TEST_CHECK(api.create(&info,&device)==LK_RENDER_OK&&device);
    lk_render_module_info_v1 moduleInfo{};
    TEST_CHECK(api.query_extension(device,LK_RENDER_EXT_MODULE_INFO,LK_RENDER_MODULE_INFO_V1,
        sizeof(moduleInfo),&moduleInfo)==LK_RENDER_OK);
    TEST_CHECK(moduleInfo.struct_size==sizeof(moduleInfo)&&moduleInfo.module_version==LK_RENDER_MODULE_VERSION&&
        moduleInfo.abi_version==LK_RENDER_ABI_V1&&!moduleInfo.reserved);
    {
        QOpenGLFramebufferObject first(40,40),second(32,24);
        TEST_CHECK(first.isValid()&&second.isValid()&&first.handle()!=second.handle()&&first.handle());
        lk_render_frame_target target{sizeof(target),40,40,first.handle(),11,17};
        lk_render_draw_command command{{1,1},{0,0,40,40},{0,0,40,40},{1,1,1,1}};
        lk_render_scene_view scene{sizeof(scene),1,&command,{0,0,0,1}};
        const auto draw=[&] {TEST_CHECK(api.render(device,&target,&scene)==LK_RENDER_OK);return first.toImage();};
        for(auto type:{LK_RENDER_I420,LK_RENDER_NV12,LK_RENDER_RGBA8}) {
            Pixels frame(type);
            TEST_CHECK(api.upload_frame(device,{1,1},&frame.view)==LK_RENDER_OK);
            const auto before=draw();
            TEST_CHECK(Near(before.pixelColor(20,20),type==LK_RENDER_RGBA8?QColor(240,20,10):QColor(255,0,0)));
            // An invalid final plane and a driver error on a later plane both keep the active frame.
            --frame.view.planes[frame.view.plane_count-1].byte_count;
            TEST_CHECK(api.upload_frame(device,{1,1},&frame.view)==LK_RENDER_INVALID_ARGUMENT);
            TEST_CHECK(draw()==before);
            ++frame.view.planes[frame.view.plane_count-1].byte_count;
            failPlane=frame.view.plane_count;
            TEST_CHECK(api.upload_frame(device,{1,1},&frame.view)==LK_RENDER_DEVICE_LOST);
            TEST_CHECK(draw()==before);
            TEST_CHECK(api.upload_frame(device,{1,1},&frame.view)==LK_RENDER_OK);
            TEST_CHECK(api.upload_frame(device,{1,2},&frame.view)==LK_RENDER_INVALID_ARGUMENT);
        }
        // YUV matrix/range metadata agrees with the common CPU-side coefficient policy.
        Pixels yuv(LK_RENDER_I420);
        for(uint32_t matrix=0;matrix<=LK_RENDER_MATRIX_BT2020_NCL;++matrix) for(uint32_t range=0;range<=LK_RENDER_RANGE_FULL;++range) {
            yuv.view.color_matrix=matrix; yuv.view.color_range=range;
            TEST_CHECK(api.upload_frame(device,{1,1},&yuv.view)==LK_RENDER_OK);
            const auto c=livekit::render::MakeYuvColorConversion({static_cast<livekit::render::RenderColorMatrix>(matrix),static_cast<livekit::render::RenderColorRange>(range)});
            int rgb[3]{}; const float samples[]{81/255.f,90/255.f,240/255.f};
            for(int i=0;i<3;++i) {float v=0;for(int j=0;j<3;++j)v+=c.yuv_to_rgb[i][j]*(samples[j]+c.yuv_offset[j]);rgb[i]=int(std::round(std::clamp(v,0.f,1.f)*255));}
            TEST_CHECK(Near(draw().pixelColor(20,20),QColor(rgb[0],rgb[1],rgb[2])));
        }
        // Top-left origin and all four clockwise rotations, sampled away from filtered edges.
        Pixels quadrant(LK_RENDER_RGBA8,8,8);
        const QColor colors[]{Qt::red,Qt::green,Qt::blue,Qt::yellow};
        for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) {
            auto* p=quadrant.planes[0].data()+y*quadrant.view.planes[0].stride_bytes+4*x;
            const auto c=colors[(y>=4)*2+(x>=4)];p[0]=c.red();p[1]=c.green();p[2]=c.blue();
        }
        const int expected[]{0,2,3,1};
        for(unsigned rotation=0;rotation<4;++rotation) {
            quadrant.view.rotation_degrees=rotation*90;
            TEST_CHECK(api.upload_frame(device,{1,1},&quadrant.view)==LK_RENDER_OK);
            TEST_CHECK(Near(draw().pixelColor(5,5),colors[expected[rotation]]));
        }
        // Premultiplied overlay and clipping over a red base.
        Pixels red(LK_RENDER_RGBA8),overlay(LK_RENDER_RGBA8);
        overlay.view.alpha_mode=LK_RENDER_ALPHA_PREMULTIPLIED;
        for(unsigned y=0;y<5;++y) for(unsigned x=0;x<3;++x) {
            auto* p=overlay.planes[0].data()+y*overlay.view.planes[0].stride_bytes+4*x;
            p[0]=p[1]=p[2]=128;p[3]=128;
        }
        TEST_CHECK(api.upload_frame(device,{1,1},&red.view)==LK_RENDER_OK);
        TEST_CHECK(api.upload_overlay(device,{2,1},&overlay.view)==LK_RENDER_OK);
        TEST_CHECK(api.upload_frame(device,{3,1},&red.view)==LK_RENDER_OUT_OF_MEMORY);
        auto commands=std::array<lk_render_draw_command,2>{command,{{2,1},{0,0,40,40},{0,0,20,40},{1,1,1,1}}};
        scene.command_count=2;scene.commands=commands.data();
        auto image=draw();
        TEST_CHECK(Near(image.pixelColor(5,5),QColor(248,138,133)));
        TEST_CHECK(Near(image.pixelColor(30,5),QColor(240,20,10)));
        // Complete scene prevalidation leaves even the clear color untouched on an invalid command.
        commands[1].resource.generation=99;
        TEST_CHECK(api.render(device,&target,&scene)==LK_RENDER_INVALID_ARGUMENT);
        TEST_CHECK(first.toImage()==image);
        commands[1].resource.generation=1;
        api.remove_resource(device,{2,9}); // stale removal must not evict live resource
        TEST_CHECK(draw()==image);
        api.remove_resource(device,{2,1}); api.remove_resource(device,{2,1});
        scene.command_count=1;scene.commands=&command;
        // Host FBO changes and owner/generation fences; no FBO cached inside module.
        target.gl_draw_fbo=second.handle();target.pixel_width=32;target.pixel_height=24;
        TEST_CHECK(api.render(device,&target,&scene)==LK_RENDER_OK);
        TEST_CHECK(Near(second.toImage().pixelColor(5,5),QColor(240,20,10)));
        TEST_CHECK(first.toImage()==image);
        ++target.context_generation; TEST_CHECK(api.render(device,&target,&scene)==LK_RENDER_DEVICE_LOST); --target.context_generation;
        ++target.surface_generation; TEST_CHECK(api.resize(device,&target)==LK_RENDER_SURFACE_LOST); --target.surface_generation;
        target.pixel_width=0; TEST_CHECK(api.resize(device,&target)==LK_RENDER_NOT_READY); target.pixel_width=32;
        // Restore host GL state, including nondefault unpack and sampler bindings.
        GLuint sampler=0;gl->glGenSamplers(1,&sampler);gl->glBindSampler(0,sampler);
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT,8);gl->glPixelStorei(GL_UNPACK_ROW_LENGTH,7);
        gl->glEnable(GL_DEPTH_TEST);gl->glViewport(1,2,3,4);gl->glActiveTexture(GL_TEXTURE2);
        TEST_CHECK(api.upload_frame(device,{1,1},&red.view)==LK_RENDER_OK);
        TEST_CHECK(api.render(device,&target,&scene)==LK_RENDER_OK);
        GLint value=0,viewport[4]{};gl->glGetIntegerv(GL_ACTIVE_TEXTURE,&value);TEST_CHECK(value==GL_TEXTURE2);
        gl->glGetIntegerv(GL_UNPACK_ALIGNMENT,&value);TEST_CHECK(value==8);
        gl->glGetIntegerv(GL_UNPACK_ROW_LENGTH,&value);TEST_CHECK(value==7);
        gl->glGetIntegerv(GL_VIEWPORT,viewport);TEST_CHECK(viewport[0]==1&&viewport[3]==4&&gl->glIsEnabled(GL_DEPTH_TEST));
        gl->glActiveTexture(GL_TEXTURE0);gl->glGetIntegerv(GL_SAMPLER_BINDING,&value);TEST_CHECK(GLuint(value)==sampler);
        gl->glBindSampler(0,0);gl->glDeleteSamplers(1,&sampler);
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT,4);gl->glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
        TEST_CHECK(gl->glGetError()==GL_NO_ERROR);
        lk_render_device* other=nullptr;TEST_CHECK(api.create(&info,&other)==LK_RENDER_OK);
        TEST_CHECK(api.render(other,&target,&scene)==LK_RENDER_INVALID_ARGUMENT); // no resource sharing
        const auto beforeDelete=deletes;api.destroy(other,LK_RENDER_DESTROY_RELEASE);TEST_CHECK(deletes>beforeDelete);
        api.destroy(device,LK_RENDER_DESTROY_RELEASE);device=nullptr;
    }
    // Robustness reports loss even if glGetError is clean. Loss is sticky and
    // release/remove must no longer issue GL deletion calls on that device.
    advertiseReset=true;
    TEST_CHECK(api.create(&info,&device)==LK_RENDER_OK);
    Pixels resetFrame(LK_RENDER_RGBA8);
    TEST_CHECK(api.upload_frame(device,{1,1},&resetFrame.view)==LK_RENDER_OK);
    const auto beforeReset=deletes;
    resetStatus=0x8255; // GL_UNKNOWN_CONTEXT_RESET
    const lk_render_frame_target resetTarget{sizeof(resetTarget),32,32,0,11,17};
    TEST_CHECK(api.resize(device,&resetTarget)==LK_RENDER_DEVICE_LOST);
    resetStatus=0; // A later NO_ERROR cannot resurrect a lost device.
    TEST_CHECK(api.upload_frame(device,{1,1},&resetFrame.view)==LK_RENDER_DEVICE_LOST);
    api.remove_resource(device,{1,1});api.destroy(device,LK_RENDER_DESTROY_RELEASE);
    TEST_CHECK(deletes==beforeReset);advertiseReset=false;
    // Abandon after current context has been detached. No deletion callback may run.
    TEST_CHECK(api.create(&info,&device)==LK_RENDER_OK);
    Pixels red(LK_RENDER_RGBA8);TEST_CHECK(api.upload_frame(device,{1,1},&red.view)==LK_RENDER_OK);
    const auto before=deletes;context.doneCurrent();api.destroy(device,LK_RENDER_DESTROY_ABANDON_GL);
    TEST_CHECK(deletes==before);
    TEST_CHECK(library.unload());
    std::cout<<"P3_GL_ABI PASS: formats/stride, matrix/range, rotation, alpha/clip, atomic upload, FBO/generation, bounds, state, multi-device, release/abandon\n";
}
