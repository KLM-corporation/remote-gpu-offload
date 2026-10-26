// ============================================================================
// Remote GPU Offload - Milestone 2 Iteration 2 Server Dispatcher Benchmark
// File: tests/test_server_dispatcher_benchmark.cpp
// Standard: C++17
// Author: Challenger 2 (Empirical Verification & Remediation)
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <psapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "protocol.h"
#include "transport.h"
#include "gpu_device.h"
#include "server_dispatcher.h"

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

// Global assertion statistics
static int g_asserts_run = 0;
static int g_asserts_passed = 0;
static int g_asserts_failed = 0;

#define DISP_BENCH_ASSERT(cond) do { \
    g_asserts_run++; \
    if (cond) { \
        g_asserts_passed++; \
    } else { \
        g_asserts_failed++; \
        std::cerr << "  [FAIL] Assertion failed: (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
    } \
} while (0)

#define DISP_BENCH_ASSERT_MSG(cond, msg) do { \
    g_asserts_run++; \
    if (cond) { \
        g_asserts_passed++; \
    } else { \
        g_asserts_failed++; \
        std::cerr << "  [FAIL] " << msg << ": (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
    } \
} while (0)

struct StatisticalSummary {
    double min_ms = 0.0;
    double max_ms = 0.0;
    double mean_ms = 0.0;
    double median_ms = 0.0;
    double p90_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double stddev_ms = 0.0;
    double sustained_rate = 0.0;
};

static StatisticalSummary ComputeStats(std::vector<double>& samples) {
    StatisticalSummary s;
    if (samples.empty()) return s;

    std::sort(samples.begin(), samples.end());
    s.min_ms = samples.front();
    s.max_ms = samples.back();

    double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
    s.mean_ms = sum / static_cast<double>(samples.size());
    s.sustained_rate = (s.mean_ms > 0.0) ? (1000.0 / s.mean_ms) : 0.0;

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

static size_t GetProcessPrivateMemoryBytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        return pmc.PrivateUsage;
    }
    return 0;
}

static bool CompileBasicShaders(ComPtr<ID3DBlob>& vsBlob, ComPtr<ID3DBlob>& psBlob) {
    const char* hlsl =
        "struct VS_IN { float3 pos : POSITION; float4 col : COLOR; };\n"
        "struct PS_IN { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
        "PS_IN VSMain(VS_IN input) { PS_IN o; o.pos = float4(input.pos, 1.0f); o.col = input.col; return o; }\n"
        "float4 PSMain(PS_IN input) : SV_TARGET { return input.col; }\n";

    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vsBlob, &err);
    if (FAILED(hr)) return false;
    hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &psBlob, &err);
    return SUCCEEDED(hr);
}

// ----------------------------------------------------------------------------
// Benchmark 1: High-Frequency Command Dispatch Throughput (> 50,000 cmds/sec)
// ----------------------------------------------------------------------------
static void Run_Benchmark_CommandDispatchThroughput(HostGpuDevice& gpu) {
    std::cout << "\n============================================================" << std::endl;
    std::cout << "  BENCHMARK 1: ServerDispatcher Command Dispatch Throughput" << std::endl;
    std::cout << "============================================================" << std::endl;

    ServerDispatcher dispatcher(gpu);

    // Initial handshake
    HandshakeRequestPayload hs_req = { RGPU_PROTOCOL_VERSION, 256, 256, 0 };
    PacketHeader hs_hdr = { RGPU_MAGIC, OP_HANDSHAKE_REQUEST, FLAG_NONE, 1, sizeof(hs_req) };
    bool hs_ok = dispatcher.DispatchCommand(hs_hdr, reinterpret_cast<const uint8_t*>(&hs_req), sizeof(hs_req));
    DISP_BENCH_ASSERT(hs_ok);
    DISP_BENCH_ASSERT(dispatcher.IsHandshakeCompleted());

    // Setup resources (Vertex Shader, Pixel Shader, Vertex Buffer, Input Layout, Constant Buffer)
    ComPtr<ID3DBlob> vsBlob, psBlob;
    DISP_BENCH_ASSERT(CompileBasicShaders(vsBlob, psBlob));

    // Register shaders
    CreateShaderPayload vsPayload = { 101, static_cast<uint32_t>(vsBlob->GetBufferSize()), 0 };
    std::vector<uint8_t> vsBuf(sizeof(vsPayload) + vsBlob->GetBufferSize());
    std::memcpy(vsBuf.data(), &vsPayload, sizeof(vsPayload));
    std::memcpy(vsBuf.data() + sizeof(vsPayload), vsBlob->GetBufferPointer(), vsBlob->GetBufferSize());
    PacketHeader vsHdr = { RGPU_MAGIC, OP_CREATE_VERTEX_SHADER, FLAG_NONE, 2, static_cast<uint32_t>(vsBuf.size()) };
    DISP_BENCH_ASSERT(dispatcher.DispatchCommand(vsHdr, vsBuf.data(), vsBuf.size()));

    CreateShaderPayload psPayload = { 102, static_cast<uint32_t>(psBlob->GetBufferSize()), 0 };
    std::vector<uint8_t> psBuf(sizeof(psPayload) + psBlob->GetBufferSize());
    std::memcpy(psBuf.data(), &psPayload, sizeof(psPayload));
    std::memcpy(psBuf.data() + sizeof(psPayload), psBlob->GetBufferPointer(), psBlob->GetBufferSize());
    PacketHeader psHdr = { RGPU_MAGIC, OP_CREATE_PIXEL_SHADER, FLAG_NONE, 3, static_cast<uint32_t>(psBuf.size()) };
    DISP_BENCH_ASSERT(dispatcher.DispatchCommand(psHdr, psBuf.data(), psBuf.size()));

    // Vertex buffer
    CreateBufferPayload vbPayload = { 201, 1024, 0, 1, 0, 0 }; // DEFAULT, VERTEX_BUFFER
    PacketHeader vbHdr = { RGPU_MAGIC, OP_CREATE_BUFFER, FLAG_NONE, 4, sizeof(vbPayload) };
    DISP_BENCH_ASSERT(dispatcher.DispatchCommand(vbHdr, reinterpret_cast<const uint8_t*>(&vbPayload), sizeof(vbPayload)));

    // Constant buffer
    CreateBufferPayload cbPayload = { 202, 256, 0, 4, 0, 0 }; // DEFAULT, CONSTANT_BUFFER
    PacketHeader cbHdr = { RGPU_MAGIC, OP_CREATE_BUFFER, FLAG_NONE, 5, sizeof(cbPayload) };
    DISP_BENCH_ASSERT(dispatcher.DispatchCommand(cbHdr, reinterpret_cast<const uint8_t*>(&cbPayload), sizeof(cbPayload)));

    // Prepare command packet sequence for benchmark loop
    // Sequence consists of 8 distinct commands covering validation for viewports, topologies, clear,
    // shaders, vertex buffers, constant buffers, and draw calls:
    struct BenchmarkCommand {
        PacketHeader header;
        std::vector<uint8_t> payload;
    };

    std::vector<BenchmarkCommand> cmdSequence;

    // 1. Clear RTV
    {
        ClearRtvPayload p = { 0, { 0.1f, 0.2f, 0.3f, 1.0f } };
        PacketHeader h = { RGPU_MAGIC, OP_CLEAR_RTV, FLAG_NONE, 10, sizeof(p) };
        BenchmarkCommand c; c.header = h; c.payload.resize(sizeof(p));
        std::memcpy(c.payload.data(), &p, sizeof(p));
        cmdSequence.push_back(std::move(c));
    }
    // 2. RS Set Viewport
    {
        RSSetViewportPayload p = { 0.0f, 0.0f, 256.0f, 256.0f, 0.0f, 1.0f };
        PacketHeader h = { RGPU_MAGIC, OP_RS_SET_VIEWPORT, FLAG_NONE, 11, sizeof(p) };
        BenchmarkCommand c; c.header = h; c.payload.resize(sizeof(p));
        std::memcpy(c.payload.data(), &p, sizeof(p));
        cmdSequence.push_back(std::move(c));
    }
    // 3. IA Set Topology
    {
        IASetTopologyPayload p = { D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST };
        PacketHeader h = { RGPU_MAGIC, OP_IA_SET_TOPOLOGY, FLAG_NONE, 12, sizeof(p) };
        BenchmarkCommand c; c.header = h; c.payload.resize(sizeof(p));
        std::memcpy(c.payload.data(), &p, sizeof(p));
        cmdSequence.push_back(std::move(c));
    }
    // 4. VS Set Shader
    {
        VSSetShaderPayload p = { 101 };
        PacketHeader h = { RGPU_MAGIC, OP_VS_SET_SHADER, FLAG_NONE, 13, sizeof(p) };
        BenchmarkCommand c; c.header = h; c.payload.resize(sizeof(p));
        std::memcpy(c.payload.data(), &p, sizeof(p));
        cmdSequence.push_back(std::move(c));
    }
    // 5. PS Set Shader
    {
        PSSetShaderPayload p = { 102 };
        PacketHeader h = { RGPU_MAGIC, OP_PS_SET_SHADER, FLAG_NONE, 14, sizeof(p) };
        BenchmarkCommand c; c.header = h; c.payload.resize(sizeof(p));
        std::memcpy(c.payload.data(), &p, sizeof(p));
        cmdSequence.push_back(std::move(c));
    }
    // 6. IA Set Vertex Buffer
    {
        IASetVertexBufferPayload p = { 0, 201, 28, 0 };
        PacketHeader h = { RGPU_MAGIC, OP_IA_SET_VERTEX_BUFFER, FLAG_NONE, 15, sizeof(p) };
        BenchmarkCommand c; c.header = h; c.payload.resize(sizeof(p));
        std::memcpy(c.payload.data(), &p, sizeof(p));
        cmdSequence.push_back(std::move(c));
    }
    // 7. VS Set Constant Buffer
    {
        VSSetConstantBufferPayload p = { 0, 202 };
        PacketHeader h = { RGPU_MAGIC, OP_VS_SET_CB, FLAG_NONE, 16, sizeof(p) };
        BenchmarkCommand c; c.header = h; c.payload.resize(sizeof(p));
        std::memcpy(c.payload.data(), &p, sizeof(p));
        cmdSequence.push_back(std::move(c));
    }
    // 8. Draw Call
    {
        DrawPayload p = { 3, 0 };
        PacketHeader h = { RGPU_MAGIC, OP_DRAW, FLAG_NONE, 17, sizeof(p) };
        BenchmarkCommand c; c.header = h; c.payload.resize(sizeof(p));
        std::memcpy(c.payload.data(), &p, sizeof(p));
        cmdSequence.push_back(std::move(c));
    }

    constexpr int TOTAL_COMMANDS = 100000;
    const size_t seq_size = cmdSequence.size();

    // Warmup 1000 commands
    for (int i = 0; i < 1000; ++i) {
        const auto& cmd = cmdSequence[i % seq_size];
        dispatcher.DispatchCommand(cmd.header, cmd.payload.data(), cmd.payload.size());
    }

    // Benchmark 100,000 commands
    auto t0 = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < TOTAL_COMMANDS; ++i) {
        const auto& cmd = cmdSequence[i % seq_size];
        dispatcher.DispatchCommand(cmd.header, cmd.payload.data(), cmd.payload.size());
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double total_sec = std::chrono::duration<double>(t1 - t0).count();
    double commands_per_sec = static_cast<double>(TOTAL_COMMANDS) / total_sec;
    double avg_nanosec_per_cmd = (total_sec * 1e9) / static_cast<double>(TOTAL_COMMANDS);

    std::cout << "  Dispatched " << TOTAL_COMMANDS << " validated commands across 8 API opcode classes:" << std::endl;
    std::cout << "    Total Dispatch Time : " << std::fixed << std::setprecision(2) << (total_sec * 1000.0) << " ms" << std::endl;
    std::cout << "    Mean Latency/Command: " << std::setprecision(2) << avg_nanosec_per_cmd << " ns (" 
              << (avg_nanosec_per_cmd / 1000.0) << " us)" << std::endl;
    std::cout << "    Dispatch Throughput : " << std::setprecision(1) << commands_per_sec << " commands/sec" << std::endl;

    DISP_BENCH_ASSERT_MSG(commands_per_sec > 50000.0, "Command dispatch throughput exceeds 50,000 commands/sec threshold");
}

// ----------------------------------------------------------------------------
// Benchmark 2: Sustained Frame Rendering & Presentation Throughput (> 120 FPS)
// ----------------------------------------------------------------------------
static void Run_Benchmark_FrameRenderingRate(HostGpuDevice& gpu) {
    std::cout << "\n============================================================" << std::endl;
    std::cout << "  BENCHMARK 2: ServerDispatcher Sustained Frame Rendering Rate" << std::endl;
    std::cout << "============================================================" << std::endl;

    struct FrameResolutionTest {
        uint32_t width;
        uint32_t height;
        const char* name;
        int frame_count;
        double min_target_fps;
    };

    FrameResolutionTest tests[] = {
        { 256, 256, "256x256 (Render & Readback)", 500, 120.0 },
        { 800, 600, "800x600 (SVGA Render & Readback)", 200, 120.0 }
    };

    for (const auto& t : tests) {
        ServerDispatcher dispatcher(gpu);

        // Handshake to set dimensions
        HandshakeRequestPayload hs_req = { RGPU_PROTOCOL_VERSION, t.width, t.height, 0 };
        PacketHeader hs_hdr = { RGPU_MAGIC, OP_HANDSHAKE_REQUEST, FLAG_NONE, 1, sizeof(hs_req) };
        DISP_BENCH_ASSERT(dispatcher.DispatchCommand(hs_hdr, reinterpret_cast<const uint8_t*>(&hs_req), sizeof(hs_req)));

        // Setup triangle shaders
        ComPtr<ID3DBlob> vsBlob, psBlob;
        DISP_BENCH_ASSERT(CompileBasicShaders(vsBlob, psBlob));

        CreateShaderPayload vsPayload = { 1, static_cast<uint32_t>(vsBlob->GetBufferSize()), 0 };
        std::vector<uint8_t> vsBuf(sizeof(vsPayload) + vsBlob->GetBufferSize());
        std::memcpy(vsBuf.data(), &vsPayload, sizeof(vsPayload));
        std::memcpy(vsBuf.data() + sizeof(vsPayload), vsBlob->GetBufferPointer(), vsBlob->GetBufferSize());
        PacketHeader vsHdr = { RGPU_MAGIC, OP_CREATE_VERTEX_SHADER, FLAG_NONE, 2, static_cast<uint32_t>(vsBuf.size()) };
        DISP_BENCH_ASSERT(dispatcher.DispatchCommand(vsHdr, vsBuf.data(), vsBuf.size()));

        CreateShaderPayload psPayload = { 2, static_cast<uint32_t>(psBlob->GetBufferSize()), 0 };
        std::vector<uint8_t> psBuf(sizeof(psPayload) + psBlob->GetBufferSize());
        std::memcpy(psBuf.data(), &psPayload, sizeof(psPayload));
        std::memcpy(psBuf.data() + sizeof(psPayload), psBlob->GetBufferPointer(), psBlob->GetBufferSize());
        PacketHeader psHdr = { RGPU_MAGIC, OP_CREATE_PIXEL_SHADER, FLAG_NONE, 3, static_cast<uint32_t>(psBuf.size()) };
        DISP_BENCH_ASSERT(dispatcher.DispatchCommand(psHdr, psBuf.data(), psBuf.size()));

        // Vertex buffer
        float triVerts[21] = {
             0.0f,  0.5f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
             0.5f, -0.5f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f,
            -0.5f, -0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f
        };
        CreateBufferPayload vbPayload = { 3, sizeof(triVerts), 0, 1, 0, 0 };
        std::vector<uint8_t> vbBuf(sizeof(vbPayload) + sizeof(triVerts));
        std::memcpy(vbBuf.data(), &vbPayload, sizeof(vbPayload));
        std::memcpy(vbBuf.data() + sizeof(vbPayload), triVerts, sizeof(triVerts));
        PacketHeader vbHdr = { RGPU_MAGIC, OP_CREATE_BUFFER, FLAG_NONE, 4, static_cast<uint32_t>(vbBuf.size()) };
        DISP_BENCH_ASSERT(dispatcher.DispatchCommand(vbHdr, vbBuf.data(), vbBuf.size()));

        // Bind pipeline
        VSSetShaderPayload vsBind = { 1 };
        PacketHeader vsBindHdr = { RGPU_MAGIC, OP_VS_SET_SHADER, FLAG_NONE, 5, sizeof(vsBind) };
        DISP_BENCH_ASSERT(dispatcher.DispatchCommand(vsBindHdr, reinterpret_cast<const uint8_t*>(&vsBind), sizeof(vsBind)));

        PSSetShaderPayload psBind = { 2 };
        PacketHeader psBindHdr = { RGPU_MAGIC, OP_PS_SET_SHADER, FLAG_NONE, 6, sizeof(psBind) };
        DISP_BENCH_ASSERT(dispatcher.DispatchCommand(psBindHdr, reinterpret_cast<const uint8_t*>(&psBind), sizeof(psBind)));

        IASetTopologyPayload topoBind = { D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST };
        PacketHeader topoBindHdr = { RGPU_MAGIC, OP_IA_SET_TOPOLOGY, FLAG_NONE, 7, sizeof(topoBind) };
        DISP_BENCH_ASSERT(dispatcher.DispatchCommand(topoBindHdr, reinterpret_cast<const uint8_t*>(&topoBind), sizeof(topoBind)));

        IASetVertexBufferPayload vbBind = { 0, 3, 28, 0 };
        PacketHeader vbBindHdr = { RGPU_MAGIC, OP_IA_SET_VERTEX_BUFFER, FLAG_NONE, 8, sizeof(vbBind) };
        DISP_BENCH_ASSERT(dispatcher.DispatchCommand(vbBindHdr, reinterpret_cast<const uint8_t*>(&vbBind), sizeof(vbBind)));

        RSSetViewportPayload vpBind = { 0.0f, 0.0f, static_cast<float>(t.width), static_cast<float>(t.height), 0.0f, 1.0f };
        PacketHeader vpHdr = { RGPU_MAGIC, OP_RS_SET_VIEWPORT, FLAG_NONE, 9, sizeof(vpBind) };
        DISP_BENCH_ASSERT(dispatcher.DispatchCommand(vpHdr, reinterpret_cast<const uint8_t*>(&vpBind), sizeof(vpBind)));

        // Warmup 5 frames
        for (int w = 0; w < 5; ++w) {
            ClearRtvPayload clr = { 0, { 0.1f, 0.1f, 0.2f, 1.0f } };
            PacketHeader clrHdr = { RGPU_MAGIC, OP_CLEAR_RTV, FLAG_NONE, 10, sizeof(clr) };
            dispatcher.DispatchCommand(clrHdr, reinterpret_cast<const uint8_t*>(&clr), sizeof(clr));

            DrawPayload drw = { 3, 0 };
            PacketHeader drwHdr = { RGPU_MAGIC, OP_DRAW, FLAG_NONE, 11, sizeof(drw) };
            dispatcher.DispatchCommand(drwHdr, reinterpret_cast<const uint8_t*>(&drw), sizeof(drw));

            PresentPayload prs = { 0, static_cast<uint64_t>(w), 0 };
            PacketHeader prsHdr = { RGPU_MAGIC, OP_PRESENT, FLAG_NONE, 12, sizeof(prs) };
            dispatcher.DispatchCommand(prsHdr, reinterpret_cast<const uint8_t*>(&prs), sizeof(prs));
        }

        // Benchmark t.frame_count frames
        std::vector<double> frameTimes;
        frameTimes.reserve(t.frame_count);

        for (int f = 0; f < t.frame_count; ++f) {
            auto t0 = std::chrono::high_resolution_clock::now();

            ClearRtvPayload clr = { 0, { 0.1f + 0.05f * std::sin(static_cast<float>(f) * 0.1f), 0.1f, 0.2f, 1.0f } };
            PacketHeader clrHdr = { RGPU_MAGIC, OP_CLEAR_RTV, FLAG_NONE, 10, sizeof(clr) };
            dispatcher.DispatchCommand(clrHdr, reinterpret_cast<const uint8_t*>(&clr), sizeof(clr));

            DrawPayload drw = { 3, 0 };
            PacketHeader drwHdr = { RGPU_MAGIC, OP_DRAW, FLAG_NONE, 11, sizeof(drw) };
            dispatcher.DispatchCommand(drwHdr, reinterpret_cast<const uint8_t*>(&drw), sizeof(drw));

            PresentPayload prs = { 0, static_cast<uint64_t>(f), 0 };
            PacketHeader prsHdr = { RGPU_MAGIC, OP_PRESENT, FLAG_NONE, 12, sizeof(prs) };
            dispatcher.DispatchCommand(prsHdr, reinterpret_cast<const uint8_t*>(&prs), sizeof(prs));

            auto t1 = std::chrono::high_resolution_clock::now();
            double f_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            frameTimes.push_back(f_ms);
        }

        DISP_BENCH_ASSERT(dispatcher.GetTotalFramesPresented() >= static_cast<uint64_t>(t.frame_count));
        DISP_BENCH_ASSERT(dispatcher.GetLastReadbackBuffer().size() == static_cast<size_t>(t.width) * t.height * 4);

        StatisticalSummary stats = ComputeStats(frameTimes);

        std::cout << "\n  --- " << t.name << " (" << t.frame_count << " Frames) ---" << std::endl;
        std::cout << "    Sustained Frame Rate: " << std::fixed << std::setprecision(1) << stats.sustained_rate << " FPS" << std::endl;
        std::cout << "    Mean Frame Latency  : " << std::setprecision(3) << stats.mean_ms << " ms (StdDev=" << stats.stddev_ms << " ms)" << std::endl;
        std::cout << "    Percentiles         : Min=" << stats.min_ms << " ms | P50=" << stats.median_ms 
                  << " ms | P90=" << stats.p90_ms << " ms | P95=" << stats.p95_ms << " ms | P99=" << stats.p99_ms 
                  << " ms | Max=" << stats.max_ms << " ms" << std::endl;

        DISP_BENCH_ASSERT_MSG(stats.sustained_rate > t.min_target_fps, "Frame rendering rate exceeds target FPS");
    }
}

// ----------------------------------------------------------------------------
// Benchmark 3: Zero Memory Leaks Across 10,000 Rapid Command Dispatch Cycles
// ----------------------------------------------------------------------------
static void Run_Benchmark_MemoryLeakCheck_10kCycles(HostGpuDevice& gpu) {
    std::cout << "\n============================================================" << std::endl;
    std::cout << "  BENCHMARK 3: Memory Leak Check Across 10,000 Command Dispatch Cycles" << std::endl;
    std::cout << "============================================================" << std::endl;

    ServerDispatcher dispatcher(gpu);

    HandshakeRequestPayload hs_req = { RGPU_PROTOCOL_VERSION, 256, 256, 0 };
    PacketHeader hs_hdr = { RGPU_MAGIC, OP_HANDSHAKE_REQUEST, FLAG_NONE, 1, sizeof(hs_req) };
    DISP_BENCH_ASSERT(dispatcher.DispatchCommand(hs_hdr, reinterpret_cast<const uint8_t*>(&hs_req), sizeof(hs_req)));

    // Driver Warmup (500 cycles) to initialize internal driver heaps and staging resources
    for (int i = 0; i < 500; ++i) {
        uint32_t res_id = 900000 + i;
        CreateBufferPayload bp = { res_id, 256, 0, 1, 0, 0 };
        PacketHeader bh = { RGPU_MAGIC, OP_CREATE_BUFFER, FLAG_NONE, 1, sizeof(bp) };
        dispatcher.DispatchCommand(bh, reinterpret_cast<const uint8_t*>(&bp), sizeof(bp));

        ClearRtvPayload clr = { 0, { 0.2f, 0.2f, 0.2f, 1.0f } };
        PacketHeader ch = { RGPU_MAGIC, OP_CLEAR_RTV, FLAG_NONE, 2, sizeof(clr) };
        dispatcher.DispatchCommand(ch, reinterpret_cast<const uint8_t*>(&clr), sizeof(clr));

        DestroyResourcePayload dp = { res_id };
        PacketHeader dh = { RGPU_MAGIC, OP_DESTROY_RESOURCE, FLAG_NONE, 3, sizeof(dp) };
        dispatcher.DispatchCommand(dh, reinterpret_cast<const uint8_t*>(&dp), sizeof(dp));
    }

    if (gpu.GetContext()) {
        gpu.GetContext()->Flush();
    }

    size_t mem_warmup = GetProcessPrivateMemoryBytes();
    std::cout << "  Post-warmup Baseline Private Working Set: " << (mem_warmup / (1024 * 1024)) << " MB" << std::endl;

    constexpr int CYCLES = 10000;
    auto t_start = std::chrono::high_resolution_clock::now();

    size_t mem_at_2500 = 0;
    size_t mem_at_5000 = 0;
    size_t mem_at_7500 = 0;

    for (int i = 1; i <= CYCLES; ++i) {
        uint32_t buf_id = 100000 + (i % 500); // Recycle IDs to stress dynamic map and discard

        // 1. Create dynamic vertex buffer
        CreateBufferPayload bp = { buf_id, 512, 0x10000 /* DYNAMIC */, 1 /* VERTEX_BUFFER */, 0x10000 /* CPU_ACCESS_WRITE */, 0 };
        PacketHeader bh = { RGPU_MAGIC, OP_CREATE_BUFFER, FLAG_NONE, static_cast<uint32_t>(i * 6 + 1), sizeof(bp) };
        dispatcher.DispatchCommand(bh, reinterpret_cast<const uint8_t*>(&bp), sizeof(bp));

        // 2. Write buffer discard
        uint8_t dummy_verts[128] = { 0xAA, 0xBB, 0xCC, 0xDD };
        WriteBufferDiscardPayload wbp = { buf_id, 0, sizeof(dummy_verts) };
        std::vector<uint8_t> wpl(sizeof(wbp) + sizeof(dummy_verts));
        std::memcpy(wpl.data(), &wbp, sizeof(wbp));
        std::memcpy(wpl.data() + sizeof(wbp), dummy_verts, sizeof(dummy_verts));
        PacketHeader wh = { RGPU_MAGIC, OP_WRITE_BUFFER_DISCARD, FLAG_NONE, static_cast<uint32_t>(i * 6 + 2), static_cast<uint32_t>(wpl.size()) };
        dispatcher.DispatchCommand(wh, wpl.data(), wpl.size());

        // 3. Clear RTV
        ClearRtvPayload clr = { 0, { 0.05f, 0.05f, 0.05f, 1.0f } };
        PacketHeader ch = { RGPU_MAGIC, OP_CLEAR_RTV, FLAG_NONE, static_cast<uint32_t>(i * 6 + 3), sizeof(clr) };
        dispatcher.DispatchCommand(ch, reinterpret_cast<const uint8_t*>(&clr), sizeof(clr));

        // 4. Set Viewport
        RSSetViewportPayload vp = { 0.0f, 0.0f, 256.0f, 256.0f, 0.0f, 1.0f };
        PacketHeader vh = { RGPU_MAGIC, OP_RS_SET_VIEWPORT, FLAG_NONE, static_cast<uint32_t>(i * 6 + 4), sizeof(vp) };
        dispatcher.DispatchCommand(vh, reinterpret_cast<const uint8_t*>(&vp), sizeof(vp));

        // 5. Draw
        DrawPayload dp = { 3, 0 };
        PacketHeader dh = { RGPU_MAGIC, OP_DRAW, FLAG_NONE, static_cast<uint32_t>(i * 6 + 5), sizeof(dp) };
        dispatcher.DispatchCommand(dh, reinterpret_cast<const uint8_t*>(&dp), sizeof(dp));

        // 6. Destroy dynamic buffer
        DestroyResourcePayload drp = { buf_id };
        PacketHeader drh = { RGPU_MAGIC, OP_DESTROY_RESOURCE, FLAG_NONE, static_cast<uint32_t>(i * 6 + 6), sizeof(drp) };
        dispatcher.DispatchCommand(drh, reinterpret_cast<const uint8_t*>(&drp), sizeof(drp));

        if (i == 2500) mem_at_2500 = GetProcessPrivateMemoryBytes();
        if (i == 5000) mem_at_5000 = GetProcessPrivateMemoryBytes();
        if (i == 7500) mem_at_7500 = GetProcessPrivateMemoryBytes();
    }

    if (gpu.GetContext()) {
        gpu.GetContext()->Flush();
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();

    size_t mem_final = GetProcessPrivateMemoryBytes();
    int64_t net_growth_bytes = static_cast<int64_t>(mem_final) - static_cast<int64_t>(mem_warmup);
    double net_growth_mb = static_cast<double>(net_growth_bytes) / (1024.0 * 1024.0);

    std::cout << "  Executed " << CYCLES << " command dispatch cycles in " << std::fixed << std::setprecision(1) << total_ms 
              << " ms (" << (CYCLES / (total_ms / 1000.0)) << " cycles/sec):" << std::endl;
    std::cout << "    Memory at 2,500 cycles : " << (mem_at_2500 / (1024 * 1024)) << " MB" << std::endl;
    std::cout << "    Memory at 5,000 cycles : " << (mem_at_5000 / (1024 * 1024)) << " MB" << std::endl;
    std::cout << "    Memory at 7,500 cycles : " << (mem_at_7500 / (1024 * 1024)) << " MB" << std::endl;
    std::cout << "    Memory at 10,000 cycles: " << (mem_final / (1024 * 1024)) << " MB" << std::endl;
    std::cout << "    Net Memory Delta       : " << std::setprecision(3) << net_growth_mb << " MB" << std::endl;
    std::cout << "    Tracked Resources Left : " << dispatcher.GetResourceManager().Count() << std::endl;

    DISP_BENCH_ASSERT_MSG(dispatcher.GetResourceManager().Count() == 0, "All dynamically created resources were cleanly destroyed");
    DISP_BENCH_ASSERT_MSG(std::abs(net_growth_mb) < 5.0, "Zero memory leak verified: Net memory growth across 10,000 cycles is under 5.0 MB");
}

// ----------------------------------------------------------------------------
// Benchmark 4: Adversarial Validation Overhead & Rapid Rejection
// ----------------------------------------------------------------------------
static void Run_Benchmark_AdversarialRejectionPerformance(HostGpuDevice& gpu) {
    std::cout << "\n============================================================" << std::endl;
    std::cout << "  BENCHMARK 4: Adversarial Rejection Throughput & Stability" << std::endl;
    std::cout << "============================================================" << std::endl;

    ServerDispatcher dispatcher(gpu);

    HandshakeRequestPayload hs_req = { RGPU_PROTOCOL_VERSION, 256, 256, 0 };
    PacketHeader hs_hdr = { RGPU_MAGIC, OP_HANDSHAKE_REQUEST, FLAG_NONE, 1, sizeof(hs_req) };
    DISP_BENCH_ASSERT(dispatcher.DispatchCommand(hs_hdr, reinterpret_cast<const uint8_t*>(&hs_req), sizeof(hs_req)));

    // Prepare adversarial invalid commands
    std::vector<std::pair<PacketHeader, std::vector<uint8_t>>> badCommands;

    // 1. Invalid viewport dimensions (negative width)
    {
        RSSetViewportPayload vp = { 0.0f, 0.0f, -100.0f, 256.0f, 0.0f, 1.0f };
        PacketHeader h = { RGPU_MAGIC, OP_RS_SET_VIEWPORT, FLAG_NONE, 1, sizeof(vp) };
        std::vector<uint8_t> b(sizeof(vp)); std::memcpy(b.data(), &vp, sizeof(vp));
        badCommands.push_back({ h, b });
    }
    // 2. Out-of-bounds constant buffer slot (slot 32 >= 14)
    {
        VSSetConstantBufferPayload cb = { 32, 100 };
        PacketHeader h = { RGPU_MAGIC, OP_VS_SET_CB, FLAG_NONE, 2, sizeof(cb) };
        std::vector<uint8_t> b(sizeof(cb)); std::memcpy(b.data(), &cb, sizeof(cb));
        badCommands.push_back({ h, b });
    }
    // 3. Invalid topology (out of enum bounds)
    {
        IASetTopologyPayload topo = { 9999 };
        PacketHeader h = { RGPU_MAGIC, OP_IA_SET_TOPOLOGY, FLAG_NONE, 3, sizeof(topo) };
        std::vector<uint8_t> b(sizeof(topo)); std::memcpy(b.data(), &topo, sizeof(topo));
        badCommands.push_back({ h, b });
    }
    // 4. Non-finite color in Clear RTV (NaN)
    {
        ClearRtvPayload clr = { 0, { NAN, 0.0f, 0.0f, 1.0f } };
        PacketHeader h = { RGPU_MAGIC, OP_CLEAR_RTV, FLAG_NONE, 4, sizeof(clr) };
        std::vector<uint8_t> b(sizeof(clr)); std::memcpy(b.data(), &clr, sizeof(clr));
        badCommands.push_back({ h, b });
    }
    // 5. Truncated payload
    {
        PacketHeader h = { RGPU_MAGIC, OP_DRAW, FLAG_NONE, 5, 2 }; // sizeof(DrawPayload) is 8
        std::vector<uint8_t> b = { 0x01, 0x02 };
        badCommands.push_back({ h, b });
    }

    constexpr int NUM_REJECTIONS = 50000;
    size_t count = badCommands.size();

    auto t0 = std::chrono::high_resolution_clock::now();
    int rejected_count = 0;

    for (int i = 0; i < NUM_REJECTIONS; ++i) {
        const auto& c = badCommands[i % count];
        bool ok = dispatcher.DispatchCommand(c.first, c.second.data(), c.second.size());
        if (!ok) {
            rejected_count++;
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double total_sec = std::chrono::duration<double>(t1 - t0).count();
    double reject_rate = static_cast<double>(NUM_REJECTIONS) / total_sec;
    double ns_per_reject = (total_sec * 1e9) / static_cast<double>(NUM_REJECTIONS);

    std::cout << "  Tested " << NUM_REJECTIONS << " adversarial malformed commands:" << std::endl;
    std::cout << "    Rejected Count       : " << rejected_count << "/" << NUM_REJECTIONS << " (100.0%)" << std::endl;
    std::cout << "    Rejection Throughput : " << std::fixed << std::setprecision(1) << reject_rate << " rejects/sec" << std::endl;
    std::cout << "    Mean Rejection Time  : " << std::setprecision(2) << ns_per_reject << " ns" << std::endl;

    DISP_BENCH_ASSERT(rejected_count == NUM_REJECTIONS);
    DISP_BENCH_ASSERT_MSG(reject_rate > 50000.0, "Adversarial rejection throughput exceeds 50,000 rejects/sec");
}

// ----------------------------------------------------------------------------
// Main Entrypoint
// ----------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    std::cout << "============================================================" << std::endl;
    std::cout << "  ServerDispatcher Performance & Remediation Benchmark Suite" << std::endl;
    std::cout << "  Milestone 2 Iteration 2 (Challenger 2)" << std::endl;
    std::cout << "============================================================" << std::endl;

    HostGpuDevice gpu;
    bool init_ok = gpu.InitializeDedicatedGpu();
    if (!init_ok) {
        std::cerr << "[FATAL] Failed to initialize Host GPU device!" << std::endl;
        return 1;
    }

    std::cout << "\nHost GPU Device:" << std::endl;
    std::cout << "  Description   : " << gpu.GetAdapterDescription() << std::endl;
    std::cout << "  Feature Level : 0x" << std::hex << gpu.GetFeatureLevel() << std::dec << std::endl;
    std::cout << "  Dedicated VRAM: " << (gpu.GetDedicatedVram() / (1024 * 1024)) << " MB" << std::endl;

    auto t_start = std::chrono::high_resolution_clock::now();

    Run_Benchmark_CommandDispatchThroughput(gpu);
    Run_Benchmark_FrameRenderingRate(gpu);
    Run_Benchmark_MemoryLeakCheck_10kCycles(gpu);
    Run_Benchmark_AdversarialRejectionPerformance(gpu);

    auto t_end = std::chrono::high_resolution_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();

    std::cout << "\n============================================================" << std::endl;
    std::cout << "  BENCHMARK & STRESS SUMMARY" << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << "  Total Assertions Run : " << g_asserts_run << std::endl;
    std::cout << "  Passed               : " << g_asserts_passed << std::endl;
    std::cout << "  Failed               : " << g_asserts_failed << std::endl;
    std::cout << "  Total Elapsed Time   : " << total_ms << " ms" << std::endl;
    std::cout << "============================================================" << std::endl;

    if (g_asserts_failed == 0) {
        std::cout << "  FINAL VERDICT: APPROVE (NO PERFORMANCE REGRESSION DETECTED)" << std::endl;
        std::cout << "============================================================" << std::endl;
        return 0;
    } else {
        std::cerr << "  FINAL VERDICT: REQUEST_CHANGES (" << g_asserts_failed << " failure(s))" << std::endl;
        std::cout << "============================================================" << std::endl;
        return 1;
    }
}
