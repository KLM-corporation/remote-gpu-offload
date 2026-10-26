#pragma once
#ifndef REMOTE_GPU_SAMPLE_SHADERS_H
#define REMOTE_GPU_SAMPLE_SHADERS_H

namespace Sample {

// Compiled at runtime with D3DCompile (a CPU-only library, so the client still needs no GPU).
// The matrix is uploaded transposed, matching HLSL's default column-major packing.
inline constexpr const char kCubeShaderSource[] = R"(
cbuffer Transform : register(b0) {
    float4x4 world_view_proj;
};

Texture2D    checker_tex : register(t0);
SamplerState checker_smp : register(s0);

struct VS_IN {
    float3 pos   : POSITION;
    float3 color : COLOR;
    float2 uv    : TEXCOORD;
};

struct PS_IN {
    float4 pos   : SV_POSITION;
    float3 color : COLOR;
    float2 uv    : TEXCOORD;
};

PS_IN VSMain(VS_IN i) {
    PS_IN o;
    o.pos = mul(float4(i.pos, 1.0f), world_view_proj);
    o.color = i.color;
    o.uv = i.uv;
    return o;
}

float4 PSMain(PS_IN i) : SV_TARGET {
    float3 texel = checker_tex.Sample(checker_smp, i.uv).rgb;
    return float4(i.color * texel, 1.0f);
}
)";

} // namespace Sample

#endif // REMOTE_GPU_SAMPLE_SHADERS_H
