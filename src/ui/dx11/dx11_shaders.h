#pragma once

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <string>
#include <iostream>

namespace livekit {
namespace dx11 {

using Microsoft::WRL::ComPtr;

// ----------------------------------------------------
// HLSL 源码常量
// ----------------------------------------------------

inline const char* kVertexShaderSource = R"(
struct VS_INPUT {
    float3 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

cbuffer FrameTransform : register(b1) {
    int g_rotation;
    float3 g_transform_padding;
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

PS_INPUT main(VS_INPUT input) {
    PS_INPUT output;
    output.pos = float4(input.pos, 1.0f);
    if (g_rotation == 90) {
        output.uv = float2(input.uv.y, 1.0f - input.uv.x);
    } else if (g_rotation == 180) {
        output.uv = float2(1.0f - input.uv.x, 1.0f - input.uv.y);
    } else if (g_rotation == 270) {
        output.uv = float2(1.0f - input.uv.y, input.uv.x);
    } else {
        output.uv = input.uv;
    }
    return output;
}
)";

// I420 (YUV420P: 3 张单通道灰度 R8 纹理)
inline const char* kPixelShaderI420Source = R"(
Texture2D    g_texY       : register(t0);
Texture2D    g_texU       : register(t1);
Texture2D    g_texV       : register(t2);
SamplerState g_sampler    : register(s0);

cbuffer YuvConversion : register(b0) {
    float4 g_yuv_to_rgb_row0;
    float4 g_yuv_to_rgb_row1;
    float4 g_yuv_to_rgb_row2;
    float4 g_yuv_offset;
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_TARGET {
    float3 yuv = float3(g_texY.Sample(g_sampler, input.uv).r,
                         g_texU.Sample(g_sampler, input.uv).r,
                         g_texV.Sample(g_sampler, input.uv).r) + g_yuv_offset.xyz;
    float3 rgb = float3(dot(g_yuv_to_rgb_row0.xyz, yuv),
                         dot(g_yuv_to_rgb_row1.xyz, yuv),
                         dot(g_yuv_to_rgb_row2.xyz, yuv));
    return float4(saturate(rgb), 1.0f);
}
)";

// NV12 (Y 为 R8, UV 为交织 R8G8)
inline const char* kPixelShaderNV12Source = R"(
Texture2D    g_texY       : register(t0);
Texture2D    g_texUV      : register(t1);
SamplerState g_sampler    : register(s0);

cbuffer YuvConversion : register(b0) {
    float4 g_yuv_to_rgb_row0;
    float4 g_yuv_to_rgb_row1;
    float4 g_yuv_to_rgb_row2;
    float4 g_yuv_offset;
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_TARGET {
    float2 uv = g_texUV.Sample(g_sampler, input.uv).rg;
    float3 yuv = float3(g_texY.Sample(g_sampler, input.uv).r, uv.x, uv.y) + g_yuv_offset.xyz;
    float3 rgb = float3(dot(g_yuv_to_rgb_row0.xyz, yuv),
                         dot(g_yuv_to_rgb_row1.xyz, yuv),
                         dot(g_yuv_to_rgb_row2.xyz, yuv));
    return float4(saturate(rgb), 1.0f);
}
)";

// RGBA 单纹理直采
inline const char* kPixelShaderRGBASource = R"(
Texture2D    g_texRGBA    : register(t0);
SamplerState g_sampler    : register(s0);

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_TARGET {
    return g_texRGBA.Sample(g_sampler, input.uv);
}
)";

// Tile chrome stays inside the native DX11 surface so Qt widgets never need
// to overlap the child HWND (the Windows airspace constraint).
inline const char* kPixelShaderSolidColorSource = R"(
cbuffer TileColor : register(b2) {
    float4 g_tile_color;
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_TARGET {
    return g_tile_color;
}
)";

// ----------------------------------------------------
// 编译辅助函数
// ----------------------------------------------------
inline bool CompileShader(const char* source, const char* entryPoint, const char* target, ID3DBlob** blobOut) {
    ComPtr<ID3DBlob> errorBlob;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    HRESULT hr = D3DCompile(
        source,
        strlen(source),
        nullptr,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entryPoint,
        target,
        flags,
        0,
        blobOut,
        errorBlob.GetAddressOf()
    );

    if (FAILED(hr)) {
        if (errorBlob) {
            std::cerr << "[DX11 Shader Error] " << static_cast<const char*>(errorBlob->GetBufferPointer()) << std::endl;
        }
        return false;
    }
    return true;
}

} // namespace dx11
} // namespace livekit
