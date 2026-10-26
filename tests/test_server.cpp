// ============================================================================
// Remote GPU Offload - Milestone 2 Server Unit & Integration Test Suite
// File: tests/test_server.cpp
// Standard: C++17
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

#include "protocol.h"
#include "transport.h"
#include "gpu_device.h"
#include "server_dispatcher.h"

#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <cmath>
#include <atomic>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace RemoteGPU;

// Global test assertion counters
static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond) do { \
    g_tests_run++; \
    if (cond) { \
        g_tests_passed++; \
    } else { \
        g_tests_failed++; \
        std::cerr << "  [FAIL] Assertion failed: (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
    } \
} while (0)

#define TEST_ASSERT_MSG(cond, msg) do { \
    g_tests_run++; \
    if (cond) { \
        g_tests_passed++; \
    } else { \
        g_tests_failed++; \
        std::cerr << "  [FAIL] " << msg << ": (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
    } \
} while (0)

#define RUN_TEST_SUITE(suite_func) do { \
    std::cout << "============================================================" << std::endl; \
    std::cout << "[RUNNING ] " #suite_func "..." << std::endl; \
    int start_fails = g_tests_failed; \
    auto t_start = std::chrono::high_resolution_clock::now(); \
    suite_func(); \
    auto t_end = std::chrono::high_resolution_clock::now(); \
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count(); \
    if (g_tests_failed == start_fails) { \
        std::cout << "[PASSED  ] " #suite_func " (" << elapsed_ms << " ms)" << std::endl; \
    } else { \
        std::cout << "[FAILED  ] " #suite_func " (" << elapsed_ms << " ms) - " \
                  << (g_tests_failed - start_fails) << " assertion failure(s)!" << std::endl; \
    } \
} while (0)

struct ColoredVertex {
    float x, y, z;
    float r, g, b, a;
};

// ----------------------------------------------------------------------------
// Suite 1: Direct D3D11 Headless Pipeline, Colored Triangle & Staging Readback
// ----------------------------------------------------------------------------
static void Test_HeadlessPipeline_ColoredTriangleAndReadback() {
    // 1. Initialize Host GPU Device
    HostGpuDevice gpu;
    bool init_ok = gpu.InitializeDedicatedGpu();
    TEST_ASSERT_MSG(init_ok, "InitializeDedicatedGpu should succeed on host");
    TEST_ASSERT(!gpu.GetAdapterDescription().empty());

    std::cout << "  Active GPU: " << gpu.GetAdapterDescription()
              << " (" << (gpu.GetDedicatedVram() / (1024 * 1024)) << " MB VRAM)" << std::endl;

    // 2. Create Offscreen Render Targets (256x256)
    constexpr uint32_t WIDTH = 256;
    constexpr uint32_t HEIGHT = 256;
    bool target_ok = gpu.CreateOffscreenTargets(WIDTH, HEIGHT);
    TEST_ASSERT_MSG(target_ok, "CreateOffscreenTargets 256x256 succeeded");

    ID3D11Device* device = gpu.GetDevice();
    ID3D11DeviceContext* context = gpu.GetContext();
    TEST_ASSERT(device != nullptr && context != nullptr);

    // 3. Compile Shaders via D3DCompile
    const char* hlsl = 
        "struct VS_IN { float3 pos : POSITION; float4 col : COLOR; };\n"
        "struct PS_IN { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
        "PS_IN VSMain(VS_IN input) {\n"
        "    PS_IN o;\n"
        "    o.pos = float4(input.pos, 1.0f);\n"
        "    o.col = input.col;\n"
        "    return o;\n"
        "}\n"
        "float4 PSMain(PS_IN input) : SV_TARGET {\n"
        "    return input.col;\n"
        "}\n";

    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vsBlob, &errBlob);
    TEST_ASSERT_MSG(SUCCEEDED(hr), "D3DCompile VSMain succeeded");

    hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &psBlob, &errBlob);
    TEST_ASSERT_MSG(SUCCEEDED(hr), "D3DCompile PSMain succeeded");

    ComPtr<ID3D11VertexShader> vs;
    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
    TEST_ASSERT(SUCCEEDED(hr));

    ComPtr<ID3D11PixelShader> ps;
    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);
    TEST_ASSERT(SUCCEEDED(hr));

    // 4. Create Input Layout
    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    ComPtr<ID3D11InputLayout> layout;
    hr = device->CreateInputLayout(layoutDesc, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout);
    TEST_ASSERT(SUCCEEDED(hr));

    // 5. Create Vertex Buffer (Colored Triangle in Normalized Device Coordinates)
    // Triangle: Apex (0, 0.5) Red, Bottom Right (0.5, -0.5) Green, Bottom Left (-0.5, -0.5) Blue
    ColoredVertex vertices[3] = {
        {  0.0f,  0.5f, 0.0f,  1.0f, 0.0f, 0.0f, 1.0f }, // Red Apex
        {  0.5f, -0.5f, 0.0f,  0.0f, 1.0f, 0.0f, 1.0f }, // Green Bottom Right
        { -0.5f, -0.5f, 0.0f,  0.0f, 0.0f, 1.0f, 1.0f }  // Blue Bottom Left
    };

    D3D11_BUFFER_DESC bufDesc{};
    bufDesc.ByteWidth = sizeof(vertices);
    bufDesc.Usage = D3D11_USAGE_DEFAULT;
    bufDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA initData{};
    initData.pSysMem = vertices;

    ComPtr<ID3D11Buffer> vb;
    hr = device->CreateBuffer(&bufDesc, &initData, &vb);
    TEST_ASSERT(SUCCEEDED(hr));

    // 6. Bind Pipeline State
    gpu.SetDefaultTargets();

    // Clear background to Dark Blue: RGBA(25, 25, 80, 255) -> (0.098f, 0.098f, 0.314f, 1.0f)
    const float clear_color[4] = { 0.098039f, 0.098039f, 0.313725f, 1.0f };
    gpu.Clear(clear_color);

    UINT stride = sizeof(ColoredVertex);
    UINT offset = 0;
    ID3D11Buffer* vbs[] = { vb.Get() };
    context->IASetInputLayout(layout.Get());
    context->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);

    // 7. Issue Draw Call
    context->Draw(3, 0);

    // 8. Execute Staging Readback
    std::vector<uint8_t> pixels;
    uint32_t out_w = 0, out_h = 0;
    bool read_ok = gpu.ReadbackFrame(pixels, out_w, out_h);
    TEST_ASSERT_MSG(read_ok, "ReadbackFrame succeeded");
    TEST_ASSERT(out_w == WIDTH);
    TEST_ASSERT(out_h == HEIGHT);
    TEST_ASSERT(pixels.size() == WIDTH * HEIGHT * 4);

    // 9. Verify Pixel Buffer Output
    // a) Verify background clear color on corners
    auto CheckClearColor = [&](int x, int y) -> bool {
        size_t idx = (static_cast<size_t>(y) * WIDTH + static_cast<size_t>(x)) * 4;
        uint8_t r = pixels[idx];
        uint8_t g = pixels[idx + 1];
        uint8_t b = pixels[idx + 2];
        uint8_t a = pixels[idx + 3];
        return std::abs(static_cast<int>(r) - 25) <= 2 &&
               std::abs(static_cast<int>(g) - 25) <= 2 &&
               std::abs(static_cast<int>(b) - 80) <= 2 &&
               a == 255;
    };

    TEST_ASSERT_MSG(CheckClearColor(0, 0), "Corner (0,0) matches clear color");
    TEST_ASSERT_MSG(CheckClearColor(WIDTH - 1, 0), "Corner (255,0) matches clear color");
    TEST_ASSERT_MSG(CheckClearColor(0, HEIGHT - 1), "Corner (0,255) matches clear color");
    TEST_ASSERT_MSG(CheckClearColor(WIDTH - 1, HEIGHT - 1), "Corner (255,255) matches clear color");

    // b) Verify rasterized triangle interior pixel (128, 140)
    size_t center_idx = (140 * WIDTH + 128) * 4;
    uint8_t cr = pixels[center_idx];
    uint8_t cg = pixels[center_idx + 1];
    uint8_t cb = pixels[center_idx + 2];
    double center_lum = 0.2126 * cr + 0.7152 * cg + 0.0722 * cb;
    TEST_ASSERT_MSG(center_lum > 20.0, "Center pixel has non-zero luminance");
    TEST_ASSERT_MSG(!CheckClearColor(128, 140), "Center pixel is distinctly rasterized geometry");

    // c) Verify geometric pixel coverage and vertex color interpolation
    int non_clear_count = 0;
    bool has_red = false;
    bool has_green = false;
    bool has_blue = false;

    for (size_t i = 0; i < pixels.size(); i += 4) {
        uint8_t r = pixels[i];
        uint8_t g = pixels[i + 1];
        uint8_t b = pixels[i + 2];
        int diff = std::abs(static_cast<int>(r) - 25) +
                   std::abs(static_cast<int>(g) - 25) +
                   std::abs(static_cast<int>(b) - 80);
        if (diff > 30) {
            non_clear_count++;
            if (r > g + 40 && r > b + 40) has_red = true;
            if (g > r + 40 && g > b + 40) has_green = true;
            if (b > r + 40 && b > g + 40) has_blue = true;
        }
    }

    std::cout << "  Rasterized triangle pixels: " << non_clear_count 
              << " / " << (WIDTH * HEIGHT) << " (" 
              << (non_clear_count * 100.0 / (WIDTH * HEIGHT)) << "%)" << std::endl;

    TEST_ASSERT_MSG(non_clear_count >= 5000 && non_clear_count <= 12000, 
                    "Rasterized pixel count within geometric bounds (~8,192 pixels)");
    TEST_ASSERT_MSG(has_red, "Red vertex region confirmed");
    TEST_ASSERT_MSG(has_green, "Green vertex region confirmed");
    TEST_ASSERT_MSG(has_blue, "Blue vertex region confirmed");
}

// ----------------------------------------------------------------------------
// Suite 2: Row Pitch Packing Verification with Non-Aligned Odd Dimensions
// ----------------------------------------------------------------------------
static void Test_RowPitchPacking_OddDimensions() {
    HostGpuDevice gpu;
    TEST_ASSERT(gpu.InitializeDedicatedGpu());

    // Non-power-of-two, non-multiple-of-256 dimensions (e.g. 137x89)
    constexpr uint32_t ODD_W = 137;
    constexpr uint32_t ODD_H = 89;
    TEST_ASSERT(gpu.CreateOffscreenTargets(ODD_W, ODD_H));

    // Clear to Emerald Green: RGBA(10, 200, 80, 255)
    const float emerald[4] = { 10.0f / 255.0f, 200.0f / 255.0f, 80.0f / 255.0f, 1.0f };
    gpu.Clear(emerald);

    std::vector<uint8_t> pixels;
    uint32_t w = 0, h = 0;
    TEST_ASSERT(gpu.ReadbackFrame(pixels, w, h));
    TEST_ASSERT(w == ODD_W);
    TEST_ASSERT(h == ODD_H);
    TEST_ASSERT(pixels.size() == ODD_W * ODD_H * 4);

    bool row_pitch_perfect = true;
    for (uint32_t y = 0; y < ODD_H; ++y) {
        for (uint32_t x = 0; x < ODD_W; ++x) {
            size_t idx = (static_cast<size_t>(y) * ODD_W + static_cast<size_t>(x)) * 4;
            if (pixels[idx] != 10 || pixels[idx + 1] != 200 || pixels[idx + 2] != 80 || pixels[idx + 3] != 255) {
                row_pitch_perfect = false;
                break;
            }
        }
        if (!row_pitch_perfect) break;
    }
    TEST_ASSERT_MSG(row_pitch_perfect, "Odd dimension row pitch unpadding verified with zero scanline distortion");
}

// ----------------------------------------------------------------------------
// Suite 3: Server Dispatcher Loopback Handling with Synthetic Packet Sequence
// ----------------------------------------------------------------------------
static void Test_ServerDispatcher_LoopbackSyntheticPackets() {
    HostGpuDevice gpu;
    TEST_ASSERT(gpu.InitializeDedicatedGpu());

    NetworkSocket listen_sock;
    constexpr uint16_t TEST_PORT = 49154;
    bool bind_ok = listen_sock.BindAndListen(TEST_PORT, "127.0.0.1");
    TEST_ASSERT_MSG(bind_ok, "Server socket BindAndListen on 127.0.0.1:49154");

    std::atomic<bool> server_ready{false};

    // Compile HLSL shaders for synthetic packet payloads
    const char* hlsl = 
        "struct VS_IN { float3 pos : POSITION; float4 col : COLOR; };\n"
        "struct PS_IN { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
        "PS_IN VSMain(VS_IN input) { PS_IN o; o.pos = float4(input.pos, 1.0f); o.col = input.col; return o; }\n"
        "float4 PSMain(PS_IN input) : SV_TARGET { return input.col; }\n";

    ComPtr<ID3DBlob> vsBlob, psBlob;
    D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vsBlob, nullptr);
    D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &psBlob, nullptr);

    // Launch Server Thread executing ServerDispatcher
    std::thread server_thread([&]() {
        server_ready = true;
        auto client_sock = listen_sock.Accept(3000);
        if (!client_sock) {
            std::cerr << "[Server Thread] Accept timed out!" << std::endl;
            return;
        }

        ServerDispatcher dispatcher(gpu);
        // Process packet stream until client disconnects or sends OP_SHUTDOWN
        dispatcher.ProcessPacketStream(*client_sock);
        dispatcher.ResetSession();
    });

    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Client connects over loopback
    NetworkSocket client;
    bool conn_ok = client.Connect("127.0.0.1", TEST_PORT, 2000);
    TEST_ASSERT_MSG(conn_ok, "Client connected to loopback server");

    // 1. Send OP_HANDSHAKE_REQUEST
    HandshakeRequestPayload req{};
    req.protocol_version = RGPU_PROTOCOL_VERSION;
    req.client_flags = FLAG_NONE;
    req.client_width = 256;
    req.client_height = 256;
    req.preferred_format = 28; // DXGI_FORMAT_R8G8B8A8_UNORM

    PacketHeader req_hdr{};
    req_hdr.magic = RGPU_MAGIC;
    req_hdr.opcode = OP_HANDSHAKE_REQUEST;
    req_hdr.flags = FLAG_NONE;
    req_hdr.sequence_id = 1;
    req_hdr.payload_length = sizeof(req);
    TEST_ASSERT(client.SendPacket(req_hdr, &req));

    // Receive OP_HANDSHAKE_RESPONSE
    PacketHeader resp_hdr{};
    std::vector<uint8_t> resp_payload;
    bool recv_ok = client.RecvPacket(resp_hdr, resp_payload);
    TEST_ASSERT_MSG(recv_ok, "Client received handshake response");
    TEST_ASSERT(resp_hdr.opcode == OP_HANDSHAKE_RESPONSE);
    TEST_ASSERT(resp_payload.size() >= sizeof(HandshakeResponsePayload));

    const auto* resp = reinterpret_cast<const HandshakeResponsePayload*>(resp_payload.data());
    TEST_ASSERT(resp->protocol_version == RGPU_PROTOCOL_VERSION);
    TEST_ASSERT(resp->status_code == 0);
    TEST_ASSERT(resp->frame_width == 256);
    TEST_ASSERT(resp->frame_height == 256);

    // 2. Send Synthetic D3D11 Pipeline Packets
    // Packet 2: Vertex Shader (res_id 301)
    {
        CreateShaderPayload p{ 301, static_cast<uint32_t>(vsBlob->GetBufferSize()) };
        std::vector<uint8_t> pl(sizeof(p) + p.bytecode_length);
        std::memcpy(pl.data(), &p, sizeof(p));
        std::memcpy(pl.data() + sizeof(p), vsBlob->GetBufferPointer(), p.bytecode_length);
        PacketHeader h{ RGPU_MAGIC, OP_CREATE_VERTEX_SHADER, FLAG_NONE, 2, static_cast<uint32_t>(pl.size()) };
        TEST_ASSERT(client.SendPacket(h, pl.data()));
    }

    // Packet 3: Pixel Shader (res_id 302)
    {
        CreateShaderPayload p{ 302, static_cast<uint32_t>(psBlob->GetBufferSize()) };
        std::vector<uint8_t> pl(sizeof(p) + p.bytecode_length);
        std::memcpy(pl.data(), &p, sizeof(p));
        std::memcpy(pl.data() + sizeof(p), psBlob->GetBufferPointer(), p.bytecode_length);
        PacketHeader h{ RGPU_MAGIC, OP_CREATE_PIXEL_SHADER, FLAG_NONE, 3, static_cast<uint32_t>(pl.size()) };
        TEST_ASSERT(client.SendPacket(h, pl.data()));
    }

    // Packet 4: Input Layout (res_id 303)
    {
        CreateInputLayoutPayload p{ 303, 2, static_cast<uint32_t>(vsBlob->GetBufferSize()) };
        RgpuInputElementDesc elems[2]{};
        std::strncpy(elems[0].semantic_name, "POSITION", sizeof(elems[0].semantic_name));
        elems[0].format = DXGI_FORMAT_R32G32B32_FLOAT;
        elems[0].aligned_byte_offset = 0;

        std::strncpy(elems[1].semantic_name, "COLOR", sizeof(elems[1].semantic_name));
        elems[1].format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        elems[1].aligned_byte_offset = 12;

        std::vector<uint8_t> pl(sizeof(p) + sizeof(elems) + p.bytecode_length);
        std::memcpy(pl.data(), &p, sizeof(p));
        std::memcpy(pl.data() + sizeof(p), elems, sizeof(elems));
        std::memcpy(pl.data() + sizeof(p) + sizeof(elems), vsBlob->GetBufferPointer(), p.bytecode_length);
        PacketHeader h{ RGPU_MAGIC, OP_CREATE_INPUT_LAYOUT, FLAG_NONE, 4, static_cast<uint32_t>(pl.size()) };
        TEST_ASSERT(client.SendPacket(h, pl.data()));
    }

    // Packet 5: Vertex Buffer (res_id 304)
    {
        ColoredVertex verts[3] = {
            {  0.0f,  0.5f, 0.0f,  1.0f, 0.0f, 0.0f, 1.0f },
            {  0.5f, -0.5f, 0.0f,  0.0f, 1.0f, 0.0f, 1.0f },
            { -0.5f, -0.5f, 0.0f,  0.0f, 0.0f, 1.0f, 1.0f }
        };
        CreateBufferPayload p{};
        p.res_id = 304;
        p.byte_width = sizeof(verts);
        p.bind_flags = 1; // D3D11_BIND_VERTEX_BUFFER
        p.has_initial_data = 1;
        p.initial_data_size = sizeof(verts);

        std::vector<uint8_t> pl(sizeof(p) + sizeof(verts));
        std::memcpy(pl.data(), &p, sizeof(p));
        std::memcpy(pl.data() + sizeof(p), verts, sizeof(verts));
        PacketHeader h{ RGPU_MAGIC, OP_CREATE_BUFFER, FLAG_NONE, 5, static_cast<uint32_t>(pl.size()) };
        TEST_ASSERT(client.SendPacket(h, pl.data()));
    }

    // Pipeline bindings
    {
        IASetInputLayoutPayload p{ 303 };
        PacketHeader h{ RGPU_MAGIC, OP_IA_SET_INPUT_LAYOUT, FLAG_NONE, 6, sizeof(p) };
        TEST_ASSERT(client.SendPacket(h, &p));
    }
    {
        IASetVertexBufferPayload p{ 0, 304, sizeof(ColoredVertex), 0 };
        PacketHeader h{ RGPU_MAGIC, OP_IA_SET_VERTEX_BUFFER, FLAG_NONE, 7, sizeof(p) };
        TEST_ASSERT(client.SendPacket(h, &p));
    }
    {
        IASetTopologyPayload p{ D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST };
        PacketHeader h{ RGPU_MAGIC, OP_IA_SET_TOPOLOGY, FLAG_NONE, 8, sizeof(p) };
        TEST_ASSERT(client.SendPacket(h, &p));
    }
    {
        VSSetShaderPayload p{ 301 };
        PacketHeader h{ RGPU_MAGIC, OP_VS_SET_SHADER, FLAG_NONE, 9, sizeof(p) };
        TEST_ASSERT(client.SendPacket(h, &p));
    }
    {
        PSSetShaderPayload p{ 302 };
        PacketHeader h{ RGPU_MAGIC, OP_PS_SET_SHADER, FLAG_NONE, 10, sizeof(p) };
        TEST_ASSERT(client.SendPacket(h, &p));
    }
    {
        RSSetViewportPayload p{ 0.0f, 0.0f, 256.0f, 256.0f, 0.0f, 1.0f };
        PacketHeader h{ RGPU_MAGIC, OP_RS_SET_VIEWPORT, FLAG_NONE, 11, sizeof(p) };
        TEST_ASSERT(client.SendPacket(h, &p));
    }
    {
        ClearRtvPayload p{};
        p.res_id = 0; // Default offscreen backbuffer RTV
        p.color[0] = 0.2f; p.color[1] = 0.1f; p.color[2] = 0.3f; p.color[3] = 1.0f; // Purple
        PacketHeader h{ RGPU_MAGIC, OP_CLEAR_RTV, FLAG_NONE, 12, sizeof(p) };
        TEST_ASSERT(client.SendPacket(h, &p));
    }
    {
        DrawPayload p{ 3, 0 };
        PacketHeader h{ RGPU_MAGIC, OP_DRAW, FLAG_NONE, 13, sizeof(p) };
        TEST_ASSERT(client.SendPacket(h, &p));
    }

    // 3. Present & Receive Streamed OP_FRAME_DATA
    {
        PresentPayload p{ 0, 0, 0, 1 };
        PacketHeader h{ RGPU_MAGIC, OP_PRESENT, FLAG_NONE, 14, sizeof(p) };
        TEST_ASSERT(client.SendPacket(h, &p));
    }

    PacketHeader frame_hdr{};
    std::vector<uint8_t> frame_payload;
    bool frame_recv_ok = client.RecvPacket(frame_hdr, frame_payload);
    TEST_ASSERT_MSG(frame_recv_ok, "Client received OP_FRAME_DATA from ServerDispatcher");
    TEST_ASSERT(frame_hdr.opcode == OP_FRAME_DATA);
    TEST_ASSERT(frame_payload.size() >= sizeof(FrameDataPayload));

    const auto* frame_meta = reinterpret_cast<const FrameDataPayload*>(frame_payload.data());
    TEST_ASSERT(frame_meta->width == 256);
    TEST_ASSERT(frame_meta->height == 256);
    TEST_ASSERT(frame_meta->data_size == 256 * 256 * 4);

    const uint8_t* frame_pixels = frame_payload.data() + sizeof(FrameDataPayload);
    // Verify corner clear color (51, 25, 76, 255)
    TEST_ASSERT(std::abs(static_cast<int>(frame_pixels[0]) - 51) <= 2);
    TEST_ASSERT(std::abs(static_cast<int>(frame_pixels[1]) - 25) <= 2);
    TEST_ASSERT(std::abs(static_cast<int>(frame_pixels[2]) - 76) <= 2);

    // Verify center pixel non-zero luminance
    size_t c_idx = (140 * 256 + 128) * 4;
    double c_lum = 0.2126 * frame_pixels[c_idx] + 0.7152 * frame_pixels[c_idx+1] + 0.0722 * frame_pixels[c_idx+2];
    TEST_ASSERT_MSG(c_lum > 20.0, "Center pixel over loopback has non-zero luminance");

    // 4. Shutdown & Disconnect
    ShutdownPayload shut{ 0 };
    PacketHeader shut_hdr{ RGPU_MAGIC, OP_SHUTDOWN, FLAG_NONE, 15, sizeof(shut) };
    TEST_ASSERT(client.SendPacket(shut_hdr, &shut));

    client.Close();
    if (server_thread.joinable()) {
        server_thread.join();
    }
    listen_sock.Close();
}

// ----------------------------------------------------------------------------
// Suite 4: Dynamic Buffer Discard (Map WRITE_DISCARD) & Resource Lifecycle
// ----------------------------------------------------------------------------
static void Test_ServerDispatcher_DynamicBufferDiscard_And_Lifecycle() {
    HostGpuDevice gpu;
    TEST_ASSERT(gpu.InitializeDedicatedGpu());
    TEST_ASSERT(gpu.CreateOffscreenTargets(64, 64));

    ServerDispatcher dispatcher(gpu);

    // 1. Create a dynamic vertex buffer (Usage = 2 (DYNAMIC), CPUAccess = 0x10000 (WRITE))
    float init_coords[6] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f };
    CreateBufferPayload bp{};
    bp.res_id = 501;
    bp.byte_width = sizeof(init_coords);
    bp.usage = 2; // D3D11_USAGE_DYNAMIC
    bp.bind_flags = 1; // D3D11_BIND_VERTEX_BUFFER
    bp.cpu_access_flags = 0x10000; // D3D11_CPU_ACCESS_WRITE
    bp.has_initial_data = 1;
    bp.initial_data_size = sizeof(init_coords);

    std::vector<uint8_t> bpl(sizeof(bp) + sizeof(init_coords));
    std::memcpy(bpl.data(), &bp, sizeof(bp));
    std::memcpy(bpl.data() + sizeof(bp), init_coords, sizeof(init_coords));

    PacketHeader bh{ RGPU_MAGIC, OP_CREATE_BUFFER, FLAG_NONE, 1, static_cast<uint32_t>(bpl.size()) };
    bool create_ok = dispatcher.DispatchCommand(bh, bpl.data(), bpl.size());
    TEST_ASSERT_MSG(create_ok, "Create dynamic buffer via dispatcher");
    TEST_ASSERT(dispatcher.GetResourceManager().HasResource(501));

    // 2. Perform OP_WRITE_BUFFER_DISCARD
    float new_coords[6] = { 1.1f, 2.2f, 3.3f, 4.4f, 5.5f, 6.6f };
    WriteBufferDiscardPayload wbd{};
    wbd.res_id = 501;
    wbd.byte_offset = 0;
    wbd.byte_width = sizeof(new_coords);

    std::vector<uint8_t> wbd_pl(sizeof(wbd) + sizeof(new_coords));
    std::memcpy(wbd_pl.data(), &wbd, sizeof(wbd));
    std::memcpy(wbd_pl.data() + sizeof(wbd), new_coords, sizeof(new_coords));

    PacketHeader wbd_hdr{ RGPU_MAGIC, OP_WRITE_BUFFER_DISCARD, FLAG_NONE, 2, static_cast<uint32_t>(wbd_pl.size()) };
    bool discard_ok = dispatcher.DispatchCommand(wbd_hdr, wbd_pl.data(), wbd_pl.size());
    TEST_ASSERT_MSG(discard_ok, "OP_WRITE_BUFFER_DISCARD succeeded");

    // 3. Test Sampler Creation & Destruction Lifecycle
    CreateSamplerPayload samp{};
    samp.res_id = 601;
    samp.filter = 0x15; // MIN_MAG_MIP_LINEAR
    samp.address_u = 1; // D3D11_TEXTURE_ADDRESS_WRAP
    samp.address_v = 1;
    samp.address_w = 1;
    samp.min_lod = 0.0f;
    samp.max_lod = 1000.0f;

    PacketHeader samp_hdr{ RGPU_MAGIC, OP_CREATE_SAMPLER, FLAG_NONE, 3, sizeof(samp) };
    bool samp_ok = dispatcher.DispatchCommand(samp_hdr, reinterpret_cast<const uint8_t*>(&samp), sizeof(samp));
    TEST_ASSERT_MSG(samp_ok, "Create Sampler succeeded");
    TEST_ASSERT(dispatcher.GetResourceManager().HasResource(601));
    TEST_ASSERT(dispatcher.GetResourceManager().GetSamplerState(601) != nullptr);

    // 4. Test Texture2D and SRV Creation
    CreateTexture2DPayload tex_p{};
    tex_p.res_id = 701;
    tex_p.width = 16;
    tex_p.height = 16;
    tex_p.mips = 1;
    tex_p.array_size = 1;
    tex_p.format = 28; // DXGI_FORMAT_R8G8B8A8_UNORM
    tex_p.sample_count = 1;
    tex_p.usage = 0; // D3D11_USAGE_DEFAULT
    tex_p.bind_flags = 8; // D3D11_BIND_SHADER_RESOURCE

    PacketHeader tex_hdr{ RGPU_MAGIC, OP_CREATE_TEXTURE2D, FLAG_NONE, 4, sizeof(tex_p) };
    bool tex_ok = dispatcher.DispatchCommand(tex_hdr, reinterpret_cast<const uint8_t*>(&tex_p), sizeof(tex_p));
    TEST_ASSERT_MSG(tex_ok, "Create Texture2D succeeded");
    TEST_ASSERT(dispatcher.GetResourceManager().HasResource(701));

    CreateSrvPayload srv_p{};
    srv_p.res_id = 702;
    srv_p.resource_id = 701;
    srv_p.format = 28;
    srv_p.view_dimension = 4; // D3D11_SRV_DIMENSION_TEXTURE2D
    srv_p.most_detailed_mip = 0;
    srv_p.mip_levels = 1;

    PacketHeader srv_hdr{ RGPU_MAGIC, OP_CREATE_SRV, FLAG_NONE, 5, sizeof(srv_p) };
    bool srv_ok = dispatcher.DispatchCommand(srv_hdr, reinterpret_cast<const uint8_t*>(&srv_p), sizeof(srv_p));
    TEST_ASSERT_MSG(srv_ok, "Create SRV succeeded");
    TEST_ASSERT(dispatcher.GetResourceManager().HasResource(702));

    // 5. Test Destroy Resource
    DestroyResourcePayload dest_p{ 601 };
    PacketHeader dest_hdr{ RGPU_MAGIC, OP_DESTROY_RESOURCE, FLAG_NONE, 6, sizeof(dest_p) };
    bool dest_ok = dispatcher.DispatchCommand(dest_hdr, reinterpret_cast<const uint8_t*>(&dest_p), sizeof(dest_p));
    TEST_ASSERT_MSG(dest_ok, "Destroy resource 601 succeeded");
    TEST_ASSERT(!dispatcher.GetResourceManager().HasResource(601));

    dispatcher.ResetSession();
    TEST_ASSERT(dispatcher.GetResourceManager().Count() == 0);
}

// ----------------------------------------------------------------------------
// Test Main Entrypoint
// ----------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    std::cout << "============================================================" << std::endl;
    std::cout << "  Remote GPU Offloading - Milestone 2 Server Test Suite" << std::endl;
    std::cout << "============================================================" << std::endl;

    if (!NetworkSocket::InitializeNetwork()) {
        std::cerr << "[FATAL] Failed to initialize Winsock2 networking!" << std::endl;
        return 1;
    }

    auto total_start = std::chrono::high_resolution_clock::now();

    RUN_TEST_SUITE(Test_HeadlessPipeline_ColoredTriangleAndReadback);
    RUN_TEST_SUITE(Test_RowPitchPacking_OddDimensions);
    RUN_TEST_SUITE(Test_ServerDispatcher_LoopbackSyntheticPackets);
    RUN_TEST_SUITE(Test_ServerDispatcher_DynamicBufferDiscard_And_Lifecycle);

    auto total_end = std::chrono::high_resolution_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(total_end - total_start).count();

    NetworkSocket::ShutdownNetwork();

    std::cout << "============================================================" << std::endl;
    std::cout << "  TEST SUMMARY" << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << "  Total Assertions Run : " << g_tests_run << std::endl;
    std::cout << "  Passed               : " << g_tests_passed << std::endl;
    std::cout << "  Failed               : " << g_tests_failed << std::endl;
    std::cout << "  Total Duration       : " << total_ms << " ms" << std::endl;
    std::cout << "============================================================" << std::endl;

    if (g_tests_failed == 0) {
        std::cout << "  OVERALL STATUS: ALL TESTS PASSED (EXIT CODE 0)" << std::endl;
        std::cout << "============================================================" << std::endl;
        return 0;
    } else {
        std::cerr << "  OVERALL STATUS: TESTS FAILED (" << g_tests_failed << " failures)" << std::endl;
        std::cout << "============================================================" << std::endl;
        return 1;
    }
}
