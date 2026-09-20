#include "gl_dispatch.h"
#include "render/api/render_backend_module_info.h"
#include "render/render_color_conversion.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

using namespace livekit::render::gl;
namespace {
struct Frame {
    UInt textures[3]{};
    uint32_t width=0, height=0, format=0, rotation=0;
    bool overlay=false;
    livekit::render::YuvColorConversion color{};
};
struct Resource { uint64_t generation=0; Frame active, spare; };
template<class F> lk_render_result Guard(F&& f) noexcept {
    try { return f(); }
    catch (const std::bad_alloc&) { return LK_RENDER_OUT_OF_MEMORY; }
    catch (...) { return LK_RENDER_FATAL; }
}
bool ValidColor(const float* c) {
    for(int i=0;i<4;++i) if(!std::isfinite(c[i]) || c[i]<0 || c[i]>1) return false;
    return true;
}
constexpr uint64_t formats=LK_RENDER_FORMAT_BIT(LK_RENDER_I420)|LK_RENDER_FORMAT_BIT(LK_RENDER_NV12)|LK_RENDER_FORMAT_BIT(LK_RENDER_RGBA8);
constexpr const char* vertex=R"(
out vec2 uv;
void main() {
    vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));
    uv=vec2(p.x,1.0-p.y); gl_Position=vec4(p*2.0-1.0,0.0,1.0);
})";
constexpr const char* fragment=R"(
in vec2 uv; out vec4 pixel;
uniform sampler2D plane0, plane1, plane2;
uniform int format, rotation, overlay;
uniform vec4 modulation, row0, row1, row2, offset;
void main() {
    vec2 p=uv;
    if(rotation==90) p=vec2(uv.y,1.0-uv.x);
    else if(rotation==180) p=vec2(1.0-uv.x,1.0-uv.y);
    else if(rotation==270) p=vec2(1.0-uv.y,uv.x);
    vec4 c=vec4(1.0);
    if(format==3) { c=texture(plane0,p); if(overlay==0) c.a=1.0; }
    else if(format!=0) {
        vec3 yuv;
        yuv.x=texture(plane0,p).r;
        if(format==1) yuv.yz=vec2(texture(plane1,p).r,texture(plane2,p).r);
        else yuv.yz=texture(plane1,p).rg;
        yuv+=offset.xyz;
        c=vec4(clamp(vec3(dot(row0.xyz,yuv),dot(row1.xyz,yuv),dot(row2.xyz,yuv)),0.0,1.0),1.0);
    }
    pixel=c*modulation;
})";
}
struct lk_render_device {
    Dispatch gl;
    std::thread::id owner=std::this_thread::get_id();
    uint64_t surface=0, context=0;
    uint32_t bound=0;
    Int max_texture=0;
    UInt program=0, vao=0;
    Int format=-1, rotation=-1, overlay=-1, modulation=-1, rows[3]{}, offset=-1;
    bool release=true;
    std::map<uint64_t,Resource> resources;
    void Free(Frame& f) { if (!gl.contextLost) gl.DeleteTextures(3,f.textures); f={}; }
    ~lk_render_device() {
        if(!release || gl.contextLost) return; // No GL calls after abandon or a detected reset.
        for(auto& [_,r]:resources) { Free(r.active); Free(r.spare); }
        if(program) gl.DeleteProgram(program);
        if(vao) gl.DeleteVertexArrays(1,&vao);
    }
};
namespace {
bool OnOwner(const lk_render_device* d) { return d && d->owner==std::this_thread::get_id(); }
lk_render_result Error(Dispatch& g) {
    if (g.ResetDetected()) return LK_RENDER_DEVICE_LOST;
    const auto error=g.GetError();
    if (error == 0x0507 /* GL_CONTEXT_LOST */) g.contextLost = true;
    return error==0 ? LK_RENDER_OK : error==0x0505 ? LK_RENDER_OUT_OF_MEMORY : LK_RENDER_DEVICE_LOST;
}
lk_render_result CheckTarget(lk_render_device* d,const lk_render_frame_target* t) {
    if(!OnOwner(d)||!t||t->struct_size!=sizeof(*t)) return LK_RENDER_INVALID_ARGUMENT;
    if(t->surface_generation!=d->surface) return LK_RENDER_SURFACE_LOST;
    if(t->context_generation!=d->context) return LK_RENDER_DEVICE_LOST;
    if(d->gl.ResetDetected()) return LK_RENDER_DEVICE_LOST;
    if(!t->pixel_width||!t->pixel_height) return LK_RENDER_NOT_READY;
    if(t->pixel_width>16384||t->pixel_height>16384) return LK_RENDER_INVALID_ARGUMENT;
    return LK_RENDER_OK;
}
lk_render_result LK_RENDER_CALL Describe(uint32_t size,lk_render_backend_info* out) noexcept {
    if(!out||size<sizeof(*out)) return LK_RENDER_INVALID_ARGUMENT;
    *out={sizeof(*out),LK_RENDER_BACKEND_OPENGL,LK_RENDER_SURFACE_HOST_GL,sizeof(void*),formats,LK_RENDER_FORMAT_BIT(LK_RENDER_RGBA8)};
    return LK_RENDER_OK;
}
bool Compile(Dispatch& g,UInt shader,const char* prefix,const char* source) {
    const char* strings[]{prefix,source};
    g.ShaderSource(shader,2,strings,nullptr); g.CompileShader(shader);
    Int ok=0; g.GetShaderiv(shader,COMPILE_STATUS,&ok); return ok!=0;
}
lk_render_result LK_RENDER_CALL Create(const lk_render_create_info* info,lk_render_device** out) noexcept {
    if(!out) return LK_RENDER_INVALID_ARGUMENT;
    *out=nullptr;
    return Guard([&]() -> lk_render_result {
        if(!info||info->struct_size!=sizeof(*info)||info->surface_kind!=LK_RENDER_SURFACE_HOST_GL||
           !info->binding||info->binding_size!=sizeof(lk_render_gl_binding)||!info->surface_generation||
           !info->max_resources||info->max_resources>4096) return LK_RENDER_INVALID_ARGUMENT;
        const auto& b=*static_cast<const lk_render_gl_binding*>(info->binding);
        if(b.struct_size!=sizeof(b)||b.reserved||!b.context_generation||!b.get_proc) return LK_RENDER_INVALID_ARGUMENT;
        auto d=std::make_unique<lk_render_device>();
        if(!d->gl.Load(b)) return LK_RENDER_UNSUPPORTED;
        auto& g=d->gl;
        const auto version=reinterpret_cast<const char*>(g.GetString(VERSION));
        if(!version) return LK_RENDER_NOT_READY;
        const bool es=std::strstr(version,"OpenGL ES")!=nullptr;
        Int major=0,minor=0; g.GetIntegerv(MAJOR_VERSION,&major); g.GetIntegerv(MINOR_VERSION,&minor);
        if(major<3||(!es&&major==3&&minor<3)) return LK_RENDER_UNSUPPORTED;
        g.LoadRobustness(b);
        if (g.ResetDetected()) return LK_RENDER_DEVICE_LOST;
        State state(g);
        const char* prefix=es ? "#version 300 es\nprecision highp float;\nprecision highp int;\n" : "#version 330 core\n";
        const UInt vs=g.CreateShader(VERTEX_SHADER),fs=g.CreateShader(FRAGMENT_SHADER);
        const bool compiled=Compile(g,vs,prefix,vertex)&&Compile(g,fs,prefix,fragment);
        if(compiled) { d->program=g.CreateProgram(); g.AttachShader(d->program,vs); g.AttachShader(d->program,fs); g.LinkProgram(d->program); }
        g.DeleteShader(vs); g.DeleteShader(fs);
        Int linked=0; if(d->program) g.GetProgramiv(d->program,LINK_STATUS,&linked);
        if(!linked) return LK_RENDER_UNSUPPORTED;
        g.GenVertexArrays(1,&d->vao); g.UseProgram(d->program);
        for(int i=0;i<3;++i) { const char* names[]{"plane0","plane1","plane2"}; g.Uniform1i(g.GetUniformLocation(d->program,names[i]),i); }
        d->format=g.GetUniformLocation(d->program,"format"); d->rotation=g.GetUniformLocation(d->program,"rotation");
        d->overlay=g.GetUniformLocation(d->program,"overlay"); d->modulation=g.GetUniformLocation(d->program,"modulation");
        d->offset=g.GetUniformLocation(d->program,"offset");
        d->rows[0]=g.GetUniformLocation(d->program,"row0"); d->rows[1]=g.GetUniformLocation(d->program,"row1"); d->rows[2]=g.GetUniformLocation(d->program,"row2");
        g.GetIntegerv(MAX_TEXTURE_SIZE,&d->max_texture);
        const auto status=Error(g); if(status!=LK_RENDER_OK) return status;
        d->surface=info->surface_generation; d->context=b.context_generation; d->bound=info->max_resources;
        *out=d.release(); return LK_RENDER_OK;
    });
}
void LK_RENDER_CALL Destroy(lk_render_device* d,uint32_t mode) noexcept {
    if(!d) return;
    if(!OnOwner(d)) std::terminate();
    if (mode != LK_RENDER_DESTROY_ABANDON_GL) d->gl.ResetDetected();
    d->release=mode!=LK_RENDER_DESTROY_ABANDON_GL;
    delete d;
}
lk_render_result LK_RENDER_CALL Resize(lk_render_device* d,const lk_render_frame_target* t) noexcept { return CheckTarget(d,t); }

lk_render_result ValidateFrame(const lk_render_frame_view* f, bool overlay) {
    if (!f || f->struct_size != sizeof(*f) || f->reserved || !f->width || !f->height ||
        f->width > 16384 || f->height > 16384 ||
        (f->rotation_degrees != 0 && f->rotation_degrees != 90 && f->rotation_degrees != 180 && f->rotation_degrees != 270) ||
        f->color_matrix > LK_RENDER_MATRIX_BT2020_NCL || f->color_range > LK_RENDER_RANGE_FULL) return LK_RENDER_INVALID_ARGUMENT;
    if (f->format != LK_RENDER_I420 && f->format != LK_RENDER_NV12 && f->format != LK_RENDER_RGBA8) return LK_RENDER_UNSUPPORTED;
    if (overlay ? (f->format != LK_RENDER_RGBA8 || f->alpha_mode != LK_RENDER_ALPHA_PREMULTIPLIED || f->rotation_degrees)
                : f->alpha_mode != LK_RENDER_ALPHA_OPAQUE) return LK_RENDER_INVALID_ARGUMENT;
    const uint32_t count = f->format == LK_RENDER_I420 ? 3 : f->format == LK_RENDER_NV12 ? 2 : 1;
    if (f->plane_count != count) return LK_RENDER_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < 3; ++i) {
        const auto& p = f->planes[i];
        if (i >= count) {
            if (p.data || p.byte_count || p.stride_bytes || p.width_samples || p.height_samples || p.reserved) return LK_RENDER_INVALID_ARGUMENT;
            continue;
        }
        const uint32_t w = i ? (f->width + 1) / 2 : f->width;
        const uint32_t h = i ? (f->height + 1) / 2 : f->height;
        const uint32_t components = f->format == LK_RENDER_RGBA8 ? 4 : (f->format == LK_RENDER_NV12 && i ? 2 : 1);
        const uint64_t row = uint64_t(w) * components;
        if (!p.data || p.reserved || p.width_samples != w || p.height_samples != h || p.stride_bytes < row ||
            p.byte_count < uint64_t(h - 1) * p.stride_bytes + row) return LK_RENDER_INVALID_ARGUMENT;
    }
    return LK_RENDER_OK;
}
lk_render_result Upload(lk_render_device* d,lk_render_resource_id id,const lk_render_frame_view* f,bool overlay) {
    if(!OnOwner(d)||!id.value||!id.generation) return LK_RENDER_INVALID_ARGUMENT;
    if(d->gl.ResetDetected()) return LK_RENDER_DEVICE_LOST;
    const auto valid=ValidateFrame(f,overlay); if(valid!=LK_RENDER_OK) return valid;
    if(f->width>uint32_t(d->max_texture)||f->height>uint32_t(d->max_texture)) return LK_RENDER_UNSUPPORTED;
    auto it=d->resources.find(id.value);
    if(it!=d->resources.end()&&it->second.generation!=id.generation) return LK_RENDER_INVALID_ARGUMENT;
    if(it==d->resources.end()&&d->resources.size()>=d->bound) return LK_RENDER_OUT_OF_MEMORY;
    // Allocate bookkeeping first; failed upload never changes the active frame.
    const bool inserted=it==d->resources.end();
    auto& r=d->resources[id.value]; r.generation=id.generation;
    auto& g=d->gl;
    State state(g);
    struct Rollback { lk_render_device* d; uint64_t id; bool inserted,committed=false;
        ~Rollback(){if(inserted&&!committed){auto& r=d->resources.at(id);d->Free(r.spare);d->resources.erase(id);}}
    } rollback{d,id.value,inserted};
    auto& next=r.spare;
    const bool allocate=next.width!=f->width||next.height!=f->height||next.format!=f->format;
    if(allocate) { d->Free(next); g.GenTextures(f->plane_count,next.textures); }
    g.ActiveTexture(TEXTURE0); g.BindBuffer(PIXEL_UNPACK_BUFFER,0);
    g.PixelStorei(UNPACK_ALIGNMENT,1); g.PixelStorei(UNPACK_SKIP_ROWS,0); g.PixelStorei(UNPACK_SKIP_PIXELS,0);
    for(uint32_t i=0;i<f->plane_count;++i) {
        const auto& p=f->planes[i];
        const unsigned components=f->format==LK_RENDER_RGBA8?4:(f->format==LK_RENDER_NV12&&i?2:1);
        const Enum external=components==4?RGBA:components==2?RG:RED;
        const Enum internal=components==4?RGBA8:components==2?RG8:R8;
        g.BindTexture(TEXTURE_2D,next.textures[i]);
        if(allocate) {
            g.TexParameteri(TEXTURE_2D,TEXTURE_MIN_FILTER,LINEAR); g.TexParameteri(TEXTURE_2D,TEXTURE_MAG_FILTER,LINEAR);
            g.TexParameteri(TEXTURE_2D,TEXTURE_WRAP_S,CLAMP_TO_EDGE); g.TexParameteri(TEXTURE_2D,TEXTURE_WRAP_T,CLAMP_TO_EDGE);
            g.TexImage2D(TEXTURE_2D,0,internal,p.width_samples,p.height_samples,0,external,UNSIGNED_BYTE,nullptr);
        }
        if(p.stride_bytes%components==0 && p.stride_bytes/components<=uint32_t(INT32_MAX)) {
            g.PixelStorei(UNPACK_ROW_LENGTH,p.stride_bytes/components);
            g.TexSubImage2D(TEXTURE_2D,0,0,0,p.width_samples,p.height_samples,external,UNSIGNED_BYTE,p.data);
        } else {
            g.PixelStorei(UNPACK_ROW_LENGTH,0);
            for(uint32_t y=0;y<p.height_samples;++y) g.TexSubImage2D(TEXTURE_2D,0,0,y,p.width_samples,1,external,UNSIGNED_BYTE,p.data+uint64_t(y)*p.stride_bytes);
        }
    }
    const auto status=Error(g);
    if(status!=LK_RENDER_OK) { d->Free(next); return status; }
    next.width=f->width; next.height=f->height; next.format=f->format; next.rotation=f->rotation_degrees; next.overlay=overlay;
    next.color=livekit::render::MakeYuvColorConversion({static_cast<livekit::render::RenderColorMatrix>(f->color_matrix),static_cast<livekit::render::RenderColorRange>(f->color_range)});
    std::swap(r.active,r.spare); rollback.committed=true;
    return LK_RENDER_OK;
}
lk_render_result LK_RENDER_CALL UploadFrame(lk_render_device* d,lk_render_resource_id id,const lk_render_frame_view* f) noexcept { return Guard([&]{return Upload(d,id,f,false);}); }
lk_render_result LK_RENDER_CALL UploadOverlay(lk_render_device* d,lk_render_resource_id id,const lk_render_frame_view* f) noexcept { return Guard([&]{return Upload(d,id,f,true);}); }
void LK_RENDER_CALL Remove(lk_render_device* d,lk_render_resource_id id) noexcept {
    if(!OnOwner(d)) return;
    if(d->gl.ResetDetected()) return;
    const auto it=d->resources.find(id.value);
    if(it!=d->resources.end()&&it->second.generation==id.generation) {d->Free(it->second.active);d->Free(it->second.spare);d->resources.erase(it);}
}
lk_render_result LK_RENDER_CALL Render(lk_render_device* d,const lk_render_frame_target* t,const lk_render_scene_view* s) noexcept {
    return Guard([&]() -> lk_render_result {
        const auto valid=CheckTarget(d,t); if(valid!=LK_RENDER_OK) return valid;
        if(!s||s->struct_size!=sizeof(*s)||s->command_count>65536||(s->command_count&&!s->commands)||!ValidColor(s->clear_rgba)) return LK_RENDER_INVALID_ARGUMENT;
        for(uint32_t i=0;i<s->command_count;++i) {
            const auto& c=s->commands[i];
            if(!ValidColor(c.color)||c.destination.width>32767||c.destination.height>32767||c.destination.x< -32768||c.destination.y< -32768||
               int64_t(c.destination.x)+c.destination.width>32767||int64_t(c.destination.y)+c.destination.height>32767) return LK_RENDER_INVALID_ARGUMENT;
            if(!c.resource.value) {if(c.resource.generation) return LK_RENDER_INVALID_ARGUMENT;}
            else {const auto it=d->resources.find(c.resource.value);if(it==d->resources.end()||it->second.generation!=c.resource.generation) return LK_RENDER_INVALID_ARGUMENT;}
        }
        auto& g=d->gl; State state(g);
        g.BindFramebuffer(DRAW_FRAMEBUFFER,t->gl_draw_fbo);
        if(g.CheckFramebufferStatus(DRAW_FRAMEBUFFER)!=FRAMEBUFFER_COMPLETE) return LK_RENDER_SURFACE_LOST;
        for(const auto cap:State::caps) g.Disable(cap);
        g.ColorMask(1,1,1,1); g.ClearColor(s->clear_rgba[0],s->clear_rgba[1],s->clear_rgba[2],s->clear_rgba[3]); g.Clear(COLOR_BUFFER_BIT);
        g.Enable(BLEND); g.Enable(SCISSOR_TEST); g.BlendFuncSeparate(ONE,ONE_MINUS_SRC_ALPHA,ONE,ONE_MINUS_SRC_ALPHA); g.BlendEquationSeparate(FUNC_ADD,FUNC_ADD);
        g.UseProgram(d->program); g.BindVertexArray(d->vao);
        for(int i=0;i<3;++i) g.BindSampler(i,0);
        for(uint32_t i=0;i<s->command_count;++i) {
            const auto& c=s->commands[i];
            const auto l=std::clamp<int64_t>(c.clip.x,0,t->pixel_width),r=std::clamp<int64_t>(int64_t(c.clip.x)+c.clip.width,0,t->pixel_width);
            const auto top=std::clamp<int64_t>(c.clip.y,0,t->pixel_height),bottom=std::clamp<int64_t>(int64_t(c.clip.y)+c.clip.height,0,t->pixel_height);
            if(l>=r||top>=bottom||!c.destination.width||!c.destination.height) continue;
            g.Scissor(Int(l),Int(t->pixel_height-bottom),Size(r-l),Size(bottom-top));
            g.Viewport(c.destination.x,Int(int64_t(t->pixel_height)-c.destination.y-c.destination.height),c.destination.width,c.destination.height);
            g.Uniform4fv(d->modulation,1,c.color);
            const Frame* f=c.resource.value?&d->resources.at(c.resource.value).active:nullptr;
            g.Uniform1i(d->format,f?f->format:0); g.Uniform1i(d->rotation,f?f->rotation:0); g.Uniform1i(d->overlay,f&&f->overlay);
            if(f) {
                for(int j=0;j<3;++j) {g.ActiveTexture(TEXTURE0+j);g.BindTexture(TEXTURE_2D,f->textures[j]);g.Uniform4fv(d->rows[j],1,f->color.yuv_to_rgb[j]);}
                g.Uniform4fv(d->offset,1,f->color.yuv_offset);
            }
            g.DrawArrays(TRIANGLES,0,3);
        }
        return Error(g);
    });
}
lk_render_result LK_RENDER_CALL Query(lk_render_device* d,uint32_t id,uint32_t version,uint32_t size,void* out) noexcept {
    if(id!=LK_RENDER_EXT_MODULE_INFO||version!=LK_RENDER_MODULE_INFO_V1) return LK_RENDER_UNSUPPORTED;
    if(!OnOwner(d)||!out||size<sizeof(lk_render_module_info_v1)) return LK_RENDER_INVALID_ARGUMENT;
    *static_cast<lk_render_module_info_v1*>(out)={
        sizeof(lk_render_module_info_v1),LK_RENDER_MODULE_VERSION,LK_RENDER_ABI_V1,0};
    return LK_RENDER_OK;
}
}
extern "C" LK_RENDER_EXPORT lk_render_result LK_RENDER_CALL lk_render_get_api(uint32_t version,uint32_t size,lk_render_api* out) {
    if(version!=LK_RENDER_ABI_V1) return LK_RENDER_ABI_MISMATCH;
    if(!out||size<sizeof(*out)) return LK_RENDER_INVALID_ARGUMENT;
    *out={LK_RENDER_ABI_V1,sizeof(*out),Describe,Create,Destroy,Resize,UploadFrame,UploadOverlay,Remove,Render,Query};
    return LK_RENDER_OK;
}
