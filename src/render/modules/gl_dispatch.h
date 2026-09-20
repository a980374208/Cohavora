#pragma once
#include "render/api/render_backend_api.h"
#include <cstring>

// Minimal GL 3.3 / ES 3.0 dispatch. No platform GL loader or Qt dependency is
// linked into the module; every address belongs to the host's current context.
namespace livekit::render::gl {
using Enum = unsigned int;
using UInt = unsigned int;
using Int = int;
using Size = int;
using Bool = unsigned char;
#define LK_GL_FUNCTIONS(F) \
 F(const unsigned char*, GetString, (Enum)) \
 F(const unsigned char*, GetStringi, (Enum, UInt)) \
 F(void, GetIntegerv, (Enum, Int*)) \
 F(void, GetFloatv, (Enum, float*)) \
 F(Enum, GetError, ()) \
 F(Bool, IsEnabled, (Enum)) \
 F(void, Enable, (Enum)) \
 F(void, Disable, (Enum)) \
 F(void, GenTextures, (Size, UInt*)) \
 F(void, DeleteTextures, (Size, const UInt*)) \
 F(void, ActiveTexture, (Enum)) \
 F(void, BindTexture, (Enum, UInt)) \
 F(void, BindSampler, (UInt, UInt)) \
 F(void, TexParameteri, (Enum, Enum, Int)) \
 F(void, TexImage2D, (Enum, Int, Int, Size, Size, Int, Enum, Enum, const void*)) \
 F(void, TexSubImage2D, (Enum, Int, Int, Int, Size, Size, Enum, Enum, const void*)) \
 F(void, PixelStorei, (Enum, Int)) \
 F(void, BindBuffer, (Enum, UInt)) \
 F(UInt, CreateShader, (Enum)) \
 F(void, ShaderSource, (UInt, Size, const char* const*, const Int*)) \
 F(void, CompileShader, (UInt)) \
 F(void, GetShaderiv, (UInt, Enum, Int*)) \
 F(void, DeleteShader, (UInt)) \
 F(UInt, CreateProgram, ()) \
 F(void, AttachShader, (UInt, UInt)) \
 F(void, LinkProgram, (UInt)) \
 F(void, GetProgramiv, (UInt, Enum, Int*)) \
 F(void, DeleteProgram, (UInt)) \
 F(void, UseProgram, (UInt)) \
 F(Int, GetUniformLocation, (UInt, const char*)) \
 F(void, Uniform1i, (Int, Int)) \
 F(void, Uniform4fv, (Int, Size, const float*)) \
 F(void, GenVertexArrays, (Size, UInt*)) \
 F(void, DeleteVertexArrays, (Size, const UInt*)) \
 F(void, BindVertexArray, (UInt)) \
 F(void, BindFramebuffer, (Enum, UInt)) \
 F(Enum, CheckFramebufferStatus, (Enum)) \
 F(void, Viewport, (Int, Int, Size, Size)) \
 F(void, Scissor, (Int, Int, Size, Size)) \
 F(void, BlendFuncSeparate, (Enum, Enum, Enum, Enum)) \
 F(void, BlendEquationSeparate, (Enum, Enum)) \
 F(void, ColorMask, (Bool, Bool, Bool, Bool)) \
 F(void, ClearColor, (float, float, float, float)) \
 F(void, Clear, (Enum)) \
 F(void, DrawArrays, (Enum, Int, Size))
struct Dispatch {
#define LK_DECLARE(ret, name, args) ret (LK_RENDER_GL_CALL *name) args = nullptr;
    LK_GL_FUNCTIONS(LK_DECLARE)
#undef LK_DECLARE
    Enum (LK_RENDER_GL_CALL *GetGraphicsResetStatus)() = nullptr;
    bool contextLost = false;
    void LoadRobustness(const lk_render_gl_binding& b) {
        Int count = 0;
        GetIntegerv(0x821D /* GL_NUM_EXTENSIONS */, &count);
        for (Int i = 0; i < count; ++i) {
            const auto* ext = reinterpret_cast<const char*>(GetStringi(0x1F03, UInt(i)));
            if (!ext) continue;
            const char* name = nullptr;
            if (!std::strcmp(ext, "GL_KHR_robustness")) {
                const auto* version = reinterpret_cast<const char*>(GetString(0x1F02));
                name = version && std::strstr(version, "OpenGL ES") ? "glGetGraphicsResetStatusKHR" : "glGetGraphicsResetStatus";
            } else if (!std::strcmp(ext, "GL_ARB_robustness")) name = "glGetGraphicsResetStatusARB";
            else if (!std::strcmp(ext, "GL_EXT_robustness")) name = "glGetGraphicsResetStatusEXT";
            if (name) {
                GetGraphicsResetStatus = reinterpret_cast<decltype(GetGraphicsResetStatus)>(b.get_proc(b.host_user, name));
                if (GetGraphicsResetStatus) break;
            }
        }
    }
    bool ResetDetected() {
        if (!contextLost && GetGraphicsResetStatus) contextLost = GetGraphicsResetStatus() != 0;
        return contextLost;
    }
    bool Load(const lk_render_gl_binding& b) {
#define LK_LOAD(ret, name, args) name = reinterpret_cast<decltype(name)>(b.get_proc(b.host_user, "gl" #name)); if (!name) return false;
        LK_GL_FUNCTIONS(LK_LOAD)
#undef LK_LOAD
        return true;
    }
};
#undef LK_GL_FUNCTIONS
// Enumerants shared by core desktop GL and ES.
constexpr Enum TEXTURE_2D=0x0DE1, TEXTURE0=0x84C0, ACTIVE_TEXTURE=0x84E0,
 TEXTURE_BINDING_2D=0x8069, SAMPLER_BINDING=0x8919, TEXTURE_MIN_FILTER=0x2801,
 TEXTURE_MAG_FILTER=0x2800, TEXTURE_WRAP_S=0x2802, TEXTURE_WRAP_T=0x2803,
 LINEAR=0x2601, CLAMP_TO_EDGE=0x812F, R8=0x8229, RG8=0x822B, RGBA8=0x8058,
 RED=0x1903, RG=0x8227, RGBA=0x1908, UNSIGNED_BYTE=0x1401,
 UNPACK_ALIGNMENT=0x0CF5, UNPACK_ROW_LENGTH=0x0CF2, UNPACK_SKIP_ROWS=0x0CF3,
 UNPACK_SKIP_PIXELS=0x0CF4, PIXEL_UNPACK_BUFFER=0x88EC, PIXEL_UNPACK_BUFFER_BINDING=0x88EF,
 VERSION=0x1F02, MAJOR_VERSION=0x821B, MINOR_VERSION=0x821C, MAX_TEXTURE_SIZE=0x0D33,
 VERTEX_SHADER=0x8B31, FRAGMENT_SHADER=0x8B30, COMPILE_STATUS=0x8B81, LINK_STATUS=0x8B82,
 CURRENT_PROGRAM=0x8B8D, VERTEX_ARRAY_BINDING=0x85B5, DRAW_FRAMEBUFFER=0x8CA9,
 DRAW_FRAMEBUFFER_BINDING=0x8CA6, FRAMEBUFFER_COMPLETE=0x8CD5, VIEWPORT=0x0BA2,
 SCISSOR_BOX=0x0C10, SCISSOR_TEST=0x0C11, BLEND=0x0BE2, DEPTH_TEST=0x0B71,
 STENCIL_TEST=0x0B90, CULL_FACE=0x0B44, RASTERIZER_DISCARD=0x8C89,
 BLEND_SRC_RGB=0x80C9, BLEND_DST_RGB=0x80C8, BLEND_SRC_ALPHA=0x80CB,
 BLEND_DST_ALPHA=0x80CA, BLEND_EQUATION_RGB=0x8009, BLEND_EQUATION_ALPHA=0x883D,
 FUNC_ADD=0x8006, ONE=1, ONE_MINUS_SRC_ALPHA=0x0303, COLOR_WRITEMASK=0x0C23,
 COLOR_CLEAR_VALUE=0x0C22, COLOR_BUFFER_BIT=0x4000, TRIANGLES=4;

// This surface is owned by the host. Restore all state changed by module calls,
// including pixel unpack and sampler state (Qt may use the same context).
struct State {
    Dispatch& g;
    Int active, tex[3], sampler[3], unpack[5], program, vao, fbo, viewport[4], scissor[4], mask[4], blend[6];
    float clear[4];
    static constexpr Enum caps[]{BLEND, DEPTH_TEST, STENCIL_TEST, CULL_FACE, SCISSOR_TEST, RASTERIZER_DISCARD};
    static constexpr Enum stores[]{UNPACK_ALIGNMENT, UNPACK_ROW_LENGTH, UNPACK_SKIP_ROWS, UNPACK_SKIP_PIXELS};
    static constexpr Enum blends[]{BLEND_SRC_RGB, BLEND_DST_RGB, BLEND_SRC_ALPHA, BLEND_DST_ALPHA, BLEND_EQUATION_RGB, BLEND_EQUATION_ALPHA};
    Bool enabled[6];
    explicit State(Dispatch& gl) : g(gl) {
        g.GetIntegerv(ACTIVE_TEXTURE,&active);
        for (int i=0;i<3;++i) { g.ActiveTexture(TEXTURE0+i); g.GetIntegerv(TEXTURE_BINDING_2D,&tex[i]); g.GetIntegerv(SAMPLER_BINDING,&sampler[i]); }
        for (int i=0;i<4;++i) g.GetIntegerv(stores[i],&unpack[i]);
        g.GetIntegerv(PIXEL_UNPACK_BUFFER_BINDING,&unpack[4]);
        g.GetIntegerv(CURRENT_PROGRAM,&program); g.GetIntegerv(VERTEX_ARRAY_BINDING,&vao);
        g.GetIntegerv(DRAW_FRAMEBUFFER_BINDING,&fbo); g.GetIntegerv(VIEWPORT,viewport);
        g.GetIntegerv(SCISSOR_BOX,scissor); g.GetIntegerv(COLOR_WRITEMASK,mask);
        g.GetFloatv(COLOR_CLEAR_VALUE,clear);
        for (int i=0;i<6;++i) { enabled[i]=g.IsEnabled(caps[i]); g.GetIntegerv(blends[i],&blend[i]); }
    }
    ~State() {
        if (g.contextLost) return; // Lost contexts must not receive restore calls.
        for (int i=0;i<3;++i) { g.ActiveTexture(TEXTURE0+i); g.BindTexture(TEXTURE_2D,tex[i]); g.BindSampler(i,sampler[i]); }
        g.ActiveTexture(active);
        for (int i=0;i<4;++i) g.PixelStorei(stores[i],unpack[i]);
        g.BindBuffer(PIXEL_UNPACK_BUFFER,unpack[4]); g.UseProgram(program); g.BindVertexArray(vao);
        g.BindFramebuffer(DRAW_FRAMEBUFFER,fbo); g.Viewport(viewport[0],viewport[1],viewport[2],viewport[3]);
        g.Scissor(scissor[0],scissor[1],scissor[2],scissor[3]);
        g.ColorMask(Bool(mask[0]),Bool(mask[1]),Bool(mask[2]),Bool(mask[3]));
        g.ClearColor(clear[0],clear[1],clear[2],clear[3]);
        g.BlendFuncSeparate(blend[0],blend[1],blend[2],blend[3]); g.BlendEquationSeparate(blend[4],blend[5]);
        for (int i=0;i<6;++i) { if(enabled[i]) g.Enable(caps[i]); else g.Disable(caps[i]); }
    }
};
} // namespace livekit::render::gl
