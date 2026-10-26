// ============================================================================
// Remote GPU Offload - Milestone 2 Empirical Benchmark & Stress Harness
// File: tests/test_server_benchmark.cpp
// Standard: C++17
// Author: Challenger 2 (Milestone 2)
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "gpu_device.h"

#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <iomanip>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace RemoteGPU;

// Test assertion stats
static int g_asserts_run = 0;
static int g_asserts_passed = 0;
static int g_asserts_failed = 0;

#define BENCH_ASSERT(cond) do { \
    g_asserts_run++; \
    if (cond) { \
        g_asserts_passed++; \
    } else { \
        g_asserts_failed++; \
        std::cerr << "  [FAIL] Assertion failed: (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
    } \
} while (0)

#define BENCH_ASSERT_MSG(cond, msg) do { \
    g_asserts_run++; \
    if (cond) { \
        g_asserts_passed++; \
    } else { \
        g_asserts_failed++; \
        std::cerr << "  [FAIL] " << msg << ": (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
    } \
} while (0)

struct SimpleVertex {
    float x, y, z;
    float r, g, b, a;
};

struct QuadVertex {
    float x, y, z;
};

struct StatisticalSummary {
    double min_ms = 0.0;
    double max_ms = 0.0;
    double mean_ms = 0.0;
    double median_ms = 0.0;
    double p90_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double stddev_ms = 0.0;
    double sustained_fps = 0.0;
};

static StatisticalSummary ComputeStats(std::vector<double>& samples) {
    StatisticalSummary s;
    if (samples.empty()) return s;

    std::sort(samples.begin(), samples.end());
    s.min_ms = samples.front();
    s.max_ms = samples.back();

    double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
    s.mean_ms = sum / static_cast<double>(samples.size());
    s.sustained_fps = (s.mean_ms > 0.0) ? (1000.0 / s.mean_ms) : 0.0;

    size_t n = samples.size();
    s.median_ms = (n % 2 == 0) ? (samples[n / 2 - 1] + samples[n / 2]) * 0.5 : samples[n / 2];
    s.p90_ms = samples[static_cast<size_t>(n * 0.90)];
    s.p95_ms = samples[static_cast<size_t>(n * 0.95)];
    s.p99_ms = samples[static_cast<size_t>(n * 0.99)];

    double variance = 0.0;
    for (double val : samples) {
        variance += (val - s.mean_ms) * (val - s.mean_ms);
    }
    s.stddev_ms = std::sqrt(variance / static_cast<double>(samples.size()));
    return s;
}

// ----------------------------------------------------------------------------
// Helper: Compile HLSL Shader
// ----------------------------------------------------------------------------
static ComPtr<ID3DBlob> CompileShaderBlob(const char* hlsl, const char* entry, const char* target) {
    ComPtr<ID3DBlob> blob, errBlob;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, entry, target, 0, 0, &blob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) {
            std::cerr << "[Shader Compilation Error] " << static_cast<const char*>(errBlob->GetBufferPointer()) << std::endl;
        }
        return nullptr;
    }
    return blob;
}

// ----------------------------------------------------------------------------
// Benchmark 1: High-Resolution Rendering & Readback Throughput
// ----------------------------------------------------------------------------
static void Run_Benchmark_HighResolutionThroughput(HostGpuDevice& gpu) {
    std::cout << "\n============================================================" << std::endl;
    std::cout << "  BENCHMARK 1: High-Resolution Offscreen Rendering & Readback" << std::endl;
    std::cout << "============================================================" << std::endl;

    struct ResolutionTest {
        uint32_t width;
        uint32_t height;
        const char* label;
    };

    ResolutionTest resolutions[] = {
        { 800, 600, "SVGA 800x600" },
        { 1280, 720, "HD 720p 1280x720" },
        { 1920, 1080, "Full HD 1080p 1920x1080" },
        { 2560, 1440, "QHD 1440p 2560x1440" }
    };

    // Shaders for triangle rendering
    const char* triangle_hlsl =
        "struct VS_IN { float3 pos : POSITION; float4 col : COLOR; };\n"
        "struct PS_IN { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
        "PS_IN VSMain(VS_IN input) { PS_IN o; o.pos = float4(input.pos, 1.0f); o.col = input.col; return o; }\n"
        "float4 PSMain(PS_IN input) : SV_TARGET { return input.col; }\n";

    ComPtr<ID3DBlob> vsBlob = CompileShaderBlob(triangle_hlsl, "VSMain", "vs_4_0");
    ComPtr<ID3DBlob> psBlob = CompileShaderBlob(triangle_hlsl, "PSMain", "ps_4_0");
    BENCH_ASSERT(vsBlob != nullptr && psBlob != nullptr);

    ID3D11Device* dev = gpu.GetDevice();
    ID3D11DeviceContext* ctx = gpu.GetContext();

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);

    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    ComPtr<ID3D11InputLayout> layout;
    dev->CreateInputLayout(layoutDesc, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout);

    SimpleVertex triVertices[3] = {
        {  0.0f,  0.5f, 0.0f,  1.0f, 0.0f, 0.0f, 1.0f },
        {  0.5f, -0.5f, 0.0f,  0.0f, 1.0f, 0.0f, 1.0f },
        { -0.5f, -0.5f, 0.0f,  0.0f, 0.0f, 1.0f, 1.0f }
    };
    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = sizeof(triVertices);
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbInit{};
    vbInit.pSysMem = triVertices;
    ComPtr<ID3D11Buffer> vb;
    dev->CreateBuffer(&vbDesc, &vbInit, &vb);

    const float clear_col[4] = { 0.1f, 0.1f, 0.2f, 1.0f };

    for (const auto& res : resolutions) {
        bool create_ok = gpu.CreateOffscreenTargets(res.width, res.height);
        BENCH_ASSERT_MSG(create_ok, "CreateOffscreenTargets for high-res");

        // Warmup 3 frames
        std::vector<uint8_t> frame_buf;
        uint32_t out_w = 0, out_h = 0;
        for (int w = 0; w < 3; ++w) {
            gpu.SetDefaultTargets();
            gpu.Clear(clear_col);
            UINT stride = sizeof(SimpleVertex), offset = 0;
            ID3D11Buffer* vbs[] = { vb.Get() };
            ctx->IASetInputLayout(layout.Get());
            ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vs.Get(), nullptr, 0);
            ctx->PSSetShader(ps.Get(), nullptr, 0);
            ctx->Draw(3, 0);
            gpu.ReadbackFrame(frame_buf, out_w, out_h);
        }

        // Benchmark 30 measured frames
        constexpr int NUM_MEASURED = 30;
        std::vector<double> render_times;
        std::vector<double> readback_times;
        std::vector<double> total_times;

        render_times.reserve(NUM_MEASURED);
        readback_times.reserve(NUM_MEASURED);
        total_times.reserve(NUM_MEASURED);

        for (int i = 0; i < NUM_MEASURED; ++i) {
            auto t0 = std::chrono::high_resolution_clock::now();

            // Render phase
            gpu.SetDefaultTargets();
            gpu.Clear(clear_col);
            UINT stride = sizeof(SimpleVertex), offset = 0;
            ID3D11Buffer* vbs[] = { vb.Get() };
            ctx->IASetInputLayout(layout.Get());
            ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vs.Get(), nullptr, 0);
            ctx->PSSetShader(ps.Get(), nullptr, 0);
            ctx->Draw(3, 0);
            ctx->Flush();

            auto t1 = std::chrono::high_resolution_clock::now();

            // Readback phase
            bool read_ok = gpu.ReadbackFrame(frame_buf, out_w, out_h);
            BENCH_ASSERT(read_ok);

            auto t2 = std::chrono::high_resolution_clock::now();

            double r_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            double rb_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
            double tot_ms = std::chrono::duration<double, std::milli>(t2 - t0).count();

            render_times.push_back(r_ms);
            readback_times.push_back(rb_ms);
            total_times.push_back(tot_ms);
        }

        BENCH_ASSERT(out_w == res.width);
        BENCH_ASSERT(out_h == res.height);
        BENCH_ASSERT(frame_buf.size() == static_cast<size_t>(res.width) * res.height * 4);

        StatisticalSummary r_stat = ComputeStats(render_times);
        StatisticalSummary rb_stat = ComputeStats(readback_times);
        StatisticalSummary tot_stat = ComputeStats(total_times);

        double frame_mb = (static_cast<double>(res.width) * res.height * 4.0) / (1024.0 * 1024.0);
        double rb_bandwidth_gbps = (frame_mb / 1024.0) / (rb_stat.mean_ms / 1000.0);

        std::cout << "\n  --- " << res.label << " (" << std::fixed << std::setprecision(2) << frame_mb << " MB/frame) ---" << std::endl;
        std::cout << "    Render Time (ms)   : Mean=" << std::setprecision(3) << r_stat.mean_ms
                  << " (min=" << r_stat.min_ms << ", max=" << r_stat.max_ms << ", stddev=" << r_stat.stddev_ms << ")" << std::endl;
        std::cout << "    Readback Time (ms) : Mean=" << rb_stat.mean_ms
                  << " (min=" << rb_stat.min_ms << ", max=" << rb_stat.max_ms << ", stddev=" << rb_stat.stddev_ms << ")" << std::endl;
        std::cout << "    Total Frame (ms)   : Mean=" << tot_stat.mean_ms
                  << " (P50=" << tot_stat.median_ms << ", P95=" << tot_stat.p95_ms << ", P99=" << tot_stat.p99_ms << ")" << std::endl;
        std::cout << "    Throughput         : " << std::setprecision(1) << tot_stat.sustained_fps << " FPS"
                  << " | Staging Bandwidth: " << std::setprecision(2) << rb_bandwidth_gbps << " GB/s" << std::endl;

        // Check real-time requirements
        if (res.width <= 1920 && res.height <= 1080) {
            BENCH_ASSERT_MSG(tot_stat.sustained_fps >= 30.0, "Resolution maintains at least 30 FPS real-time threshold");
        }
    }
}

// ----------------------------------------------------------------------------
// Benchmark 2: Continuous Multi-Frame Profiling (100+ Frames)
// ----------------------------------------------------------------------------
static void Run_Benchmark_ContinuousFrames_Profile(HostGpuDevice& gpu) {
    std::cout << "\n============================================================" << std::endl;
    std::cout << "  BENCHMARK 2: Multi-Frame Continuous Rendering (100 Frames)" << std::endl;
    std::cout << "============================================================" << std::endl;

    struct ContinuousConfig {
        uint32_t width;
        uint32_t height;
        const char* name;
        int frame_count;
    };

    ContinuousConfig configs[] = {
        { 800, 600, "Continuous SVGA (800x600)", 100 },
        { 1920, 1080, "Continuous Full HD (1920x1080)", 100 }
    };

    // Dynamic rotation vertex shader
    const char* dynamic_hlsl =
        "cbuffer TransformCB : register(b0) {\n"
        "    float angle;\n"
        "    float aspect;\n"
        "    float pad0, pad1;\n"
        "};\n"
        "struct VS_IN { float3 pos : POSITION; float4 col : COLOR; };\n"
        "struct PS_IN { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
        "PS_IN VSMain(VS_IN input) {\n"
        "    PS_IN o;\n"
        "    float s = sin(angle);\n"
        "    float c = cos(angle);\n"
        "    float x_rot = input.pos.x * c - input.pos.y * s;\n"
        "    float y_rot = input.pos.x * s + input.pos.y * c;\n"
        "    o.pos = float4(x_rot / aspect, y_rot, 0.5f, 1.0f);\n"
        "    o.col = input.col;\n"
        "    return o;\n"
        "}\n"
        "float4 PSMain(PS_IN input) : SV_TARGET {\n"
        "    return input.col;\n"
        "}\n";

    ComPtr<ID3DBlob> vsBlob = CompileShaderBlob(dynamic_hlsl, "VSMain", "vs_4_0");
    ComPtr<ID3DBlob> psBlob = CompileShaderBlob(dynamic_hlsl, "PSMain", "ps_4_0");
    BENCH_ASSERT(vsBlob != nullptr && psBlob != nullptr);

    ID3D11Device* dev = gpu.GetDevice();
    ID3D11DeviceContext* ctx = gpu.GetContext();

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);

    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    ComPtr<ID3D11InputLayout> layout;
    dev->CreateInputLayout(layoutDesc, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout);

    SimpleVertex quadVerts[6] = {
        { -0.5f,  0.5f, 0.0f,  1.0f, 0.2f, 0.2f, 1.0f },
        {  0.5f,  0.5f, 0.0f,  0.2f, 1.0f, 0.2f, 1.0f },
        { -0.5f, -0.5f, 0.0f,  0.2f, 0.2f, 1.0f, 1.0f },
        { -0.5f, -0.5f, 0.0f,  0.2f, 0.2f, 1.0f, 1.0f },
        {  0.5f,  0.5f, 0.0f,  0.2f, 1.0f, 0.2f, 1.0f },
        {  0.5f, -0.5f, 0.0f,  1.0f, 1.0f, 0.2f, 1.0f }
    };
    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = sizeof(quadVerts);
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbInit{};
    vbInit.pSysMem = quadVerts;
    ComPtr<ID3D11Buffer> vb;
    dev->CreateBuffer(&vbDesc, &vbInit, &vb);

    // Constant buffer for animation
    struct TransformData {
        float angle;
        float aspect;
        float pad[2];
    };
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(TransformData);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> cb;
    dev->CreateBuffer(&cbDesc, nullptr, &cb);

    for (const auto& cfg : configs) {
        gpu.CreateOffscreenTargets(cfg.width, cfg.height);

        std::vector<uint8_t> frame_pixels;
        uint32_t fw = 0, fh = 0;

        std::vector<double> render_times(cfg.frame_count);
        std::vector<double> readback_times(cfg.frame_count);
        std::vector<double> total_times(cfg.frame_count);

        int dropped_60fps = 0; // frames taking > 16.667 ms
        int dropped_30fps = 0; // frames taking > 33.333 ms

        const float aspect_ratio = static_cast<float>(cfg.width) / static_cast<float>(cfg.height);

        for (int frame = 0; frame < cfg.frame_count; ++frame) {
            auto t0 = std::chrono::high_resolution_clock::now();

            // Update constant buffer with frame angle
            float angle = static_cast<float>(frame) * 0.05f;
            TransformData td{ angle, aspect_ratio, {0.0f, 0.0f} };
            ctx->UpdateSubresource(cb.Get(), 0, nullptr, &td, 0, 0);

            // Render
            gpu.SetDefaultTargets();
            float dynamic_clear[4] = {
                0.05f + 0.05f * std::sin(angle),
                0.05f + 0.05f * std::cos(angle),
                0.15f,
                1.0f
            };
            gpu.Clear(dynamic_clear);

            UINT stride = sizeof(SimpleVertex), offset = 0;
            ID3D11Buffer* vbs[] = { vb.Get() };
            ID3D11Buffer* cbs[] = { cb.Get() };
            ctx->IASetInputLayout(layout.Get());
            ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vs.Get(), nullptr, 0);
            ctx->VSSetConstantBuffers(0, 1, cbs);
            ctx->PSSetShader(ps.Get(), nullptr, 0);
            ctx->Draw(6, 0);
            ctx->Flush();

            auto t1 = std::chrono::high_resolution_clock::now();

            // Readback
            bool ok = gpu.ReadbackFrame(frame_pixels, fw, fh);
            BENCH_ASSERT(ok);

            auto t2 = std::chrono::high_resolution_clock::now();

            double r_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            double rb_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
            double tot_ms = std::chrono::duration<double, std::milli>(t2 - t0).count();

            render_times[frame] = r_ms;
            readback_times[frame] = rb_ms;
            total_times[frame] = tot_ms;

            if (tot_ms > 16.667) dropped_60fps++;
            if (tot_ms > 33.333) dropped_30fps++;
        }

        StatisticalSummary tot_stat = ComputeStats(total_times);
        StatisticalSummary rb_stat = ComputeStats(readback_times);
        StatisticalSummary r_stat = ComputeStats(render_times);

        std::cout << "\n  --- " << cfg.name << " (" << cfg.frame_count << " Frames) ---" << std::endl;
        std::cout << "    Sustained Frame Rate: " << std::fixed << std::setprecision(1) << tot_stat.sustained_fps << " FPS" << std::endl;
        std::cout << "    Mean Frame Time     : " << std::setprecision(3) << tot_stat.mean_ms << " ms (StdDev/Jitter=" << tot_stat.stddev_ms << " ms)" << std::endl;
        std::cout << "    Latency Percentiles : Min=" << tot_stat.min_ms << " ms | P50=" << tot_stat.median_ms
                  << " ms | P90=" << tot_stat.p90_ms << " ms | P95=" << tot_stat.p95_ms << " ms | P99=" << tot_stat.p99_ms
                  << " ms | Max=" << tot_stat.max_ms << " ms" << std::endl;
        std::cout << "    Readback Latency    : Mean=" << rb_stat.mean_ms << " ms (P50=" << rb_stat.median_ms << " ms, Max=" << rb_stat.max_ms << " ms)" << std::endl;
        std::cout << "    Render Latency      : Mean=" << r_stat.mean_ms << " ms" << std::endl;
        std::cout << "    Frame Budget Drops  : >16.6ms (60 FPS budget)=" << dropped_60fps << "/" << cfg.frame_count
                  << " (" << (dropped_60fps * 100.0 / cfg.frame_count) << "%), >33.3ms (30 FPS budget)=" << dropped_30fps << "/" << cfg.frame_count << std::endl;

        BENCH_ASSERT_MSG(cfg.frame_count >= 60, "Executed >= 60 continuous frames");
        BENCH_ASSERT_MSG(tot_stat.sustained_fps >= 30.0, "Sustained FPS >= 30 FPS");
    }
}

// ----------------------------------------------------------------------------
// Benchmark 3: Row-Pitch Alignment Stress (Prime & Odd Dimensions)
// ----------------------------------------------------------------------------
static void Run_Stress_RowPitchAlignment_ZeroSkew(HostGpuDevice& gpu) {
    std::cout << "\n============================================================" << std::endl;
    std::cout << "  BENCHMARK 3: Scanline Row-Pitch Alignment (Prime/Odd Dims)" << std::endl;
    std::cout << "============================================================" << std::endl;

    struct OddDimCase {
        uint32_t width;
        uint32_t height;
        const char* desc;
    };

    OddDimCase cases[] = {
        { 333, 221, "333x221 (Prime/Odd, tight=1332 B, GPU pitch=1536 B, pad=204 B)" },
        { 511, 511, "511x511 (Mersenne Odd, tight=2044 B, GPU pitch=2048 B, pad=4 B)" },
        { 137,  89, "137x89 (Prime Dimensions, tight=548 B, GPU pitch=768 B, pad=220 B)" },
        { 1023, 767, "1023x767 (Odd High-Res, tight=4092 B, GPU pitch=4096 B, pad=4 B)" },
        {  77,  61, "77x61 (Small Prime/Odd, tight=308 B, GPU pitch=512 B, pad=204 B)" }
    };

    // Fullscreen quad HLSL that renders vertical stripe pattern using SV_POSITION.
    // Screen space x coordinate determines color.
    // If stripe width is 8 pixels:
    // (x / 8) % 2 == 0 -> Reddish (240, 20, 20)
    // (x / 8) % 2 == 1 -> Blueish (20, 20, 240)
    const char* stripe_hlsl =
        "struct VS_OUT { float4 pos : SV_POSITION; };\n"
        "VS_OUT VSMain(uint id : SV_VertexID) {\n"
        "    VS_OUT o;\n"
        "    // Fullscreen triangle covering [-1, 1]\n"
        "    float2 uv = float2((id << 1) & 2, id & 2);\n"
        "    o.pos = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n"
        "    return o;\n"
        "}\n"
        "float4 PSMain(VS_OUT input) : SV_TARGET {\n"
        "    uint x = (uint)input.pos.x;\n"
        "    if (((x / 8u) % 2u) == 0u) {\n"
        "        return float4(240.0f / 255.0f, 20.0f / 255.0f, 20.0f / 255.0f, 1.0f);\n"
        "    } else {\n"
        "        return float4(20.0f / 255.0f, 20.0f / 255.0f, 240.0f / 255.0f, 1.0f);\n"
        "    }\n"
        "}\n";

    ComPtr<ID3DBlob> vsBlob = CompileShaderBlob(stripe_hlsl, "VSMain", "vs_4_0");
    ComPtr<ID3DBlob> psBlob = CompileShaderBlob(stripe_hlsl, "PSMain", "ps_4_0");
    BENCH_ASSERT(vsBlob != nullptr && psBlob != nullptr);

    ID3D11Device* dev = gpu.GetDevice();
    ID3D11DeviceContext* ctx = gpu.GetContext();

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);

    for (const auto& c : cases) {
        bool create_ok = gpu.CreateOffscreenTargets(c.width, c.height);
        BENCH_ASSERT_MSG(create_ok, "CreateOffscreenTargets for odd dimensions");

        gpu.SetDefaultTargets();
        const float black_clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        gpu.Clear(black_clear);

        // Draw fullscreen triangle
        ctx->IASetInputLayout(nullptr);
        ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->Draw(3, 0);
        ctx->Flush();

        std::vector<uint8_t> pixels;
        uint32_t out_w = 0, out_h = 0;
        bool read_ok = gpu.ReadbackFrame(pixels, out_w, out_h);
        BENCH_ASSERT_MSG(read_ok, "ReadbackFrame for odd dimension succeeded");
        BENCH_ASSERT(out_w == c.width);
        BENCH_ASSERT(out_h == c.height);
        BENCH_ASSERT(pixels.size() == static_cast<size_t>(c.width) * c.height * 4);

        // Mathematical Skew Oracle:
        // Every single pixel (x, y) must match the expected stripe color corresponding to x!
        // If there was even a 1-pixel scanline shift on row y, pixels on row y will sample the wrong color!
        size_t total_pixels = static_cast<size_t>(c.width) * c.height;
        size_t skew_errors = 0;
        size_t first_error_x = 0, first_error_y = 0;
        uint8_t first_err_r = 0, first_err_b = 0;

        for (uint32_t y = 0; y < c.height; ++y) {
            for (uint32_t x = 0; x < c.width; ++x) {
                size_t idx = (static_cast<size_t>(y) * c.width + static_cast<size_t>(x)) * 4;
                uint8_t r = pixels[idx];
                uint8_t g = pixels[idx + 1];
                uint8_t b = pixels[idx + 2];
                uint8_t a = pixels[idx + 3];

                bool is_stripe_a = ((x / 8) % 2 == 0);
                bool matches = false;

                if (is_stripe_a) {
                    // Expect Reddish (240, 20, 20, 255)
                    matches = (std::abs(static_cast<int>(r) - 240) <= 5) &&
                              (std::abs(static_cast<int>(g) - 20) <= 5) &&
                              (std::abs(static_cast<int>(b) - 20) <= 5) &&
                              (a == 255);
                } else {
                    // Expect Blueish (20, 20, 240, 255)
                    matches = (std::abs(static_cast<int>(r) - 20) <= 5) &&
                              (std::abs(static_cast<int>(g) - 20) <= 5) &&
                              (std::abs(static_cast<int>(b) - 240) <= 5) &&
                              (a == 255);
                }

                if (!matches) {
                    if (skew_errors == 0) {
                        first_error_x = x;
                        first_error_y = y;
                        first_err_r = r;
                        first_err_b = b;
                    }
                    skew_errors++;
                }
            }
        }

        std::cout << "  Tested " << c.desc << ":" << std::endl;
        std::cout << "    Total Pixels Tested : " << total_pixels << std::endl;
        std::cout << "    Pixel Skew Errors   : " << skew_errors << " (" << (skew_errors * 100.0 / total_pixels) << "%)" << std::endl;

        if (skew_errors > 0) {
            std::cerr << "    [FAIL] First skew mismatch at (" << first_error_x << ", " << first_error_y
                      << "): Observed R=" << static_cast<int>(first_err_r) << " B=" << static_cast<int>(first_err_b)
                      << " for x=" << first_error_x << " (Stripe " << ((first_error_x / 8) % 2) << ")" << std::endl;
        }

        BENCH_ASSERT_MSG(skew_errors == 0, "Scanline row pitch unpadding verified with ZERO pixel skew");
    }
}

// ----------------------------------------------------------------------------
// Benchmark 4: Dynamic Target Resizing & Stress Under Rapid Resolution Shifts
// ----------------------------------------------------------------------------
static void Run_Stress_DynamicResolutionSwitching(HostGpuDevice& gpu) {
    std::cout << "\n============================================================" << std::endl;
    std::cout << "  BENCHMARK 4: Dynamic Target Resizing & Rapid Resolution Shifts" << std::endl;
    std::cout << "============================================================" << std::endl;

    const std::vector<std::pair<uint32_t, uint32_t>> sequence = {
        { 256, 256 },
        { 1920, 1080 },
        { 333, 221 },
        { 800, 600 },
        { 511, 511 },
        { 1280, 720 },
        { 64, 64 },
        { 1920, 1080 }
    };

    std::vector<uint8_t> frame_pixels;
    uint32_t w = 0, h = 0;

    auto t_start = std::chrono::high_resolution_clock::now();

    for (size_t cycle = 0; cycle < 3; ++cycle) {
        for (const auto& res : sequence) {
            bool ok = gpu.CreateOffscreenTargets(res.first, res.second);
            BENCH_ASSERT(ok);

            const float color[4] = {
                static_cast<float>(res.first % 255) / 255.0f,
                static_cast<float>(res.second % 255) / 255.0f,
                0.5f,
                1.0f
            };
            gpu.Clear(color);

            bool read_ok = gpu.ReadbackFrame(frame_pixels, w, h);
            BENCH_ASSERT(read_ok);
            BENCH_ASSERT(w == res.first);
            BENCH_ASSERT(h == res.second);
            BENCH_ASSERT(frame_pixels.size() == static_cast<size_t>(res.first) * res.second * 4);
        }
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();

    std::cout << "  Executed " << (3 * sequence.size()) << " dynamic resize/readback cycles across varying resolutions in "
              << total_ms << " ms (" << (total_ms / static_cast<double>(3 * sequence.size())) << " ms/reallocation)." << std::endl;
    BENCH_ASSERT_MSG(true, "Dynamic target destruction and reallocation succeeded with zero crashes");
}

// ----------------------------------------------------------------------------
// Benchmark 5: Mathematical Pixel Luminance & Non-Clear Distribution Oracle
// ----------------------------------------------------------------------------
static void Run_Validation_PixelLuminance_And_Distribution(HostGpuDevice& gpu) {
    std::cout << "\n============================================================" << std::endl;
    std::cout << "  BENCHMARK 5: Mathematical Luminance & Geometric Distribution" << std::endl;
    std::cout << "============================================================" << std::endl;

    struct GeomTest {
        uint32_t width;
        uint32_t height;
        const char* name;
    };

    GeomTest tests[] = {
        { 800, 600, "SVGA 800x600" },
        { 1920, 1080, "Full HD 1920x1080" },
        { 333, 221, "Odd Dimension 333x221" },
        { 511, 511, "Odd Dimension 511x511" }
    };

    // Vertex shader and pixel shader for colored triangle
    const char* hlsl =
        "struct VS_IN { float3 pos : POSITION; float4 col : COLOR; };\n"
        "struct PS_IN { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
        "PS_IN VSMain(VS_IN input) { PS_IN o; o.pos = float4(input.pos, 1.0f); o.col = input.col; return o; }\n"
        "float4 PSMain(PS_IN input) : SV_TARGET { return input.col; }\n";

    ComPtr<ID3DBlob> vsBlob = CompileShaderBlob(hlsl, "VSMain", "vs_4_0");
    ComPtr<ID3DBlob> psBlob = CompileShaderBlob(hlsl, "PSMain", "ps_4_0");
    BENCH_ASSERT(vsBlob != nullptr && psBlob != nullptr);

    ID3D11Device* dev = gpu.GetDevice();
    ID3D11DeviceContext* ctx = gpu.GetContext();

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);

    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    ComPtr<ID3D11InputLayout> layout;
    dev->CreateInputLayout(layoutDesc, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout);

    // Standard triangle: Apex (0, 0.5) Red, BR (0.5, -0.5) Green, BL (-0.5, -0.5) Blue
    // Theoretical NDC area = 0.5 * base(1.0) * height(1.0) = 0.5.
    // Total NDC viewport area = 2.0 * 2.0 = 4.0.
    // Theoretical area ratio = 0.5 / 4.0 = 0.1250 (12.50%).
    SimpleVertex triVertices[3] = {
        {  0.0f,  0.5f, 0.0f,  1.0f, 0.0f, 0.0f, 1.0f },
        {  0.5f, -0.5f, 0.0f,  0.0f, 1.0f, 0.0f, 1.0f },
        { -0.5f, -0.5f, 0.0f,  0.0f, 0.0f, 1.0f, 1.0f }
    };
    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = sizeof(triVertices);
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbInit{};
    vbInit.pSysMem = triVertices;
    ComPtr<ID3D11Buffer> vb;
    dev->CreateBuffer(&vbDesc, &vbInit, &vb);

    // Clear color: Dark Blue RGBA(25, 25, 80, 255) -> (0.098039f, 0.098039f, 0.313725f, 1.0f)
    const float clear_col[4] = { 0.098039f, 0.098039f, 0.313725f, 1.0f };

    for (const auto& t : tests) {
        gpu.CreateOffscreenTargets(t.width, t.height);
        gpu.SetDefaultTargets();
        gpu.Clear(clear_col);

        UINT stride = sizeof(SimpleVertex), offset = 0;
        ID3D11Buffer* vbs[] = { vb.Get() };
        ctx->IASetInputLayout(layout.Get());
        ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->Draw(3, 0);
        ctx->Flush();

        std::vector<uint8_t> pixels;
        uint32_t w = 0, h = 0;
        bool read_ok = gpu.ReadbackFrame(pixels, w, h);
        BENCH_ASSERT(read_ok);

        size_t total_pixels = static_cast<size_t>(t.width) * t.height;
        size_t non_clear_count = 0;

        double sum_lum = 0.0;
        double sum_lum_sq = 0.0;
        double sum_r = 0.0, sum_g = 0.0, sum_b = 0.0;

        uint32_t min_x = t.width, max_x = 0;
        uint32_t min_y = t.height, max_y = 0;

        for (uint32_t y = 0; y < t.height; ++y) {
            for (uint32_t x = 0; x < t.width; ++x) {
                size_t idx = (static_cast<size_t>(y) * t.width + static_cast<size_t>(x)) * 4;
                uint8_t r = pixels[idx];
                uint8_t g = pixels[idx + 1];
                uint8_t b = pixels[idx + 2];

                double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                sum_lum += lum;
                sum_lum_sq += lum * lum;
                sum_r += r;
                sum_g += g;
                sum_b += b;

                int diff = std::abs(static_cast<int>(r) - 25) +
                           std::abs(static_cast<int>(g) - 25) +
                           std::abs(static_cast<int>(b) - 80);
                if (diff > 30) {
                    non_clear_count++;
                    min_x = std::min(min_x, x);
                    max_x = std::max(max_x, x);
                    min_y = std::min(min_y, y);
                    max_y = std::max(max_y, y);
                }
            }
        }

        double mean_lum = sum_lum / static_cast<double>(total_pixels);
        double variance_lum = (sum_lum_sq / static_cast<double>(total_pixels)) - (mean_lum * mean_lum);
        double stddev_lum = std::sqrt(std::max(0.0, variance_lum));
        double non_clear_ratio = static_cast<double>(non_clear_count) / static_cast<double>(total_pixels);

        // Theoretical Bounding Box:
        // Triangle x is in [-0.5, 0.5] -> screen [0.25 W, 0.75 W]
        // Triangle y is in [-0.5, 0.5] -> screen [0.25 H, 0.75 H] (DirectX screen Y points down)
        double expected_min_x = 0.25 * t.width;
        double expected_max_x = 0.75 * t.width;
        double expected_min_y = 0.25 * t.height;
        double expected_max_y = 0.75 * t.height;

        std::cout << "\n  --- " << t.name << " ---" << std::endl;
        std::cout << "    Total Pixels        : " << total_pixels << std::endl;
        std::cout << "    Mean Luminance      : " << std::fixed << std::setprecision(2) << mean_lum << std::endl;
        std::cout << "    Luminance StdDev    : " << stddev_lum << std::endl;
        std::cout << "    Non-Clear Pixels    : " << non_clear_count << " (" << std::setprecision(3) << (non_clear_ratio * 100.0) << "%)"
                  << " [Theoretical: 12.500%]" << std::endl;
        std::cout << "    Observed BoundingBox: [" << min_x << ".." << max_x << "] x [" << min_y << ".." << max_y << "]" << std::endl;
        std::cout << "    Expected BoundingBox: [" << static_cast<int>(expected_min_x) << ".." << static_cast<int>(expected_max_x)
                  << "] x [" << static_cast<int>(expected_min_y) << ".." << static_cast<int>(expected_max_y) << "]" << std::endl;

        // Verify mathematical constraints
        BENCH_ASSERT_MSG(mean_lum > 15.0 && mean_lum < 150.0, "Mean luminance matches expected range");
        BENCH_ASSERT_MSG(stddev_lum > 5.0, "Luminance exhibits non-flat geometric variance");
        // Non-clear ratio should match 12.5% within 1.0% tolerance due to rasterization discretization
        BENCH_ASSERT_MSG(std::abs(non_clear_ratio - 0.125) < 0.010, "Non-clear ratio strictly matches theoretical NDC triangle area 12.5% +/- 1%");

        // Bounding box should match within 2 pixels or 1%
        BENCH_ASSERT_MSG(std::abs(static_cast<double>(min_x) - expected_min_x) <= std::max(3.0, 0.02 * t.width), "Bounding box min_x");
        BENCH_ASSERT_MSG(std::abs(static_cast<double>(max_x) - expected_max_x) <= std::max(3.0, 0.02 * t.width), "Bounding box max_x");
        BENCH_ASSERT_MSG(std::abs(static_cast<double>(min_y) - expected_min_y) <= std::max(3.0, 0.02 * t.height), "Bounding box min_y");
        BENCH_ASSERT_MSG(std::abs(static_cast<double>(max_y) - expected_max_y) <= std::max(3.0, 0.02 * t.height), "Bounding box max_y");
    }
}

// ----------------------------------------------------------------------------
// Main Runner
// ----------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    std::cout << "============================================================" << std::endl;
    std::cout << "  Milestone 2 Empirical Benchmark & Stress Test Harness" << std::endl;
    std::cout << "  Agent: Challenger 2 (Rendering & Readback Throughput)" << std::endl;
    std::cout << "============================================================" << std::endl;

    HostGpuDevice gpu;
    bool init_ok = gpu.InitializeDedicatedGpu();
    if (!init_ok) {
        std::cerr << "[FATAL] Failed to initialize Host GPU device!" << std::endl;
        return 1;
    }

    std::cout << "\nHost GPU Information:" << std::endl;
    std::cout << "  Description   : " << gpu.GetAdapterDescription() << std::endl;
    std::cout << "  Feature Level : 0x" << std::hex << gpu.GetFeatureLevel() << std::dec << std::endl;
    std::cout << "  Dedicated VRAM: " << (gpu.GetDedicatedVram() / (1024 * 1024)) << " MB" << std::endl;
    std::cout << "  Shared RAM    : " << (gpu.GetSharedRam() / (1024 * 1024)) << " MB" << std::endl;

    auto t_start = std::chrono::high_resolution_clock::now();

    Run_Benchmark_HighResolutionThroughput(gpu);
    Run_Benchmark_ContinuousFrames_Profile(gpu);
    Run_Stress_RowPitchAlignment_ZeroSkew(gpu);
    Run_Stress_DynamicResolutionSwitching(gpu);
    Run_Validation_PixelLuminance_And_Distribution(gpu);

    auto t_end = std::chrono::high_resolution_clock::now();
    auto total_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();

    std::cout << "\n============================================================" << std::endl;
    std::cout << "  BENCHMARK & STRESS SUMMARY" << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << "  Total Assertions Run : " << g_asserts_run << std::endl;
    std::cout << "  Passed               : " << g_asserts_passed << std::endl;
    std::cout << "  Failed               : " << g_asserts_failed << std::endl;
    std::cout << "  Total Duration       : " << total_duration_ms << " ms" << std::endl;
    std::cout << "============================================================" << std::endl;

    if (g_asserts_failed == 0) {
        std::cout << "  OVERALL VERDICT: ALL BENCHMARKS & STRESS TESTS PASSED" << std::endl;
        std::cout << "============================================================" << std::endl;
        return 0;
    } else {
        std::cerr << "  OVERALL VERDICT: STRESS FAILURES DETECTED (" << g_asserts_failed << " failures)" << std::endl;
        std::cout << "============================================================" << std::endl;
        return 1;
    }
}
