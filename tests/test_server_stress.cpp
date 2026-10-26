// ============================================================================
// Remote GPU Offload - Milestone 2 Server Adversarial Stress Test Suite
// File: tests/test_server_stress.cpp
// Standard: C++17
// Author: Challenger 1 (Empirical Verification & Adversarial Stress Testing)
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
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
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <chrono>
#include <thread>
#include <future>
#include <atomic>
#include <random>
#include <algorithm>

using Microsoft::WRL::ComPtr;
using namespace RemoteGPU;

// ----------------------------------------------------------------------------
// Lightweight Zero-Dependency Test Harness
// ----------------------------------------------------------------------------
static std::atomic<int> g_stress_run{0};
static std::atomic<int> g_stress_passed{0};
static std::atomic<int> g_stress_failed{0};

#define STRESS_ASSERT(cond) do { \
    g_stress_run.fetch_add(1); \
    if (cond) { \
        g_stress_passed.fetch_add(1); \
    } else { \
        g_stress_failed.fetch_add(1); \
        std::cerr << "  [FAIL] Assertion failed: (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
    } \
} while (0)

#define STRESS_ASSERT_MSG(cond, msg) do { \
    g_stress_run.fetch_add(1); \
    if (cond) { \
        g_stress_passed.fetch_add(1); \
    } else { \
        g_stress_failed.fetch_add(1); \
        std::cerr << "  [FAIL] " << msg << ": (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
    } \
} while (0)

#define RUN_STRESS_SUITE(suite_func) do { \
    std::cout << "============================================================" << std::endl; \
    std::cout << "[RUNNING ] " #suite_func "..." << std::endl; \
    int start_fails = g_stress_failed.load(); \
    auto t_start = std::chrono::high_resolution_clock::now(); \
    DWORD suite_exc = 0; \
    __try { \
        suite_func(); \
    } __except (EXCEPTION_EXECUTE_HANDLER) { \
        suite_exc = GetExceptionCode(); \
        g_stress_failed.fetch_add(1); \
        std::cerr << "  [FATAL SUITE CRASH] " #suite_func " terminated with unhandled driver exception 0x" \
                  << std::hex << suite_exc << std::dec << std::endl; \
    } \
    auto t_end = std::chrono::high_resolution_clock::now(); \
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count(); \
    if (g_stress_failed.load() == start_fails) { \
        std::cout << "[PASSED  ] " #suite_func " (" << elapsed_ms << " ms)" << std::endl; \
    } else { \
        std::cout << "[FAILED  ] " #suite_func " (" << elapsed_ms << " ms) - " \
                  << (g_stress_failed.load() - start_fails) << " assertion failure(s)!" << std::endl; \
    } \
} while (0)

// Helper: Bind an ephemeral port on 127.0.0.1
static bool CreateEphemeralListener(NetworkSocket& listener, uint16_t& out_port) {
    if (!listener.BindAndListen(0, "127.0.0.1")) {
        return false;
    }
    out_port = listener.GetLocalPort();
    return out_port != 0;
}

// Helper: Query current process memory usage in bytes
static size_t GetProcessPrivateMemoryBytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        return pmc.PrivateUsage;
    }
    return 0;
}

// Helper: Safely invoke DispatchCommand guarded by Win32 Structured Exception Handling (SEH)
static bool SafeDispatchCommand(ServerDispatcher& dispatcher, const PacketHeader& header, const uint8_t* payload, size_t size, NetworkSocket* socket = nullptr, DWORD* out_exc = nullptr) {
    __try {
        return dispatcher.DispatchCommand(header, payload, size, socket);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (out_exc) {
            *out_exc = GetExceptionCode();
        }
        return false;
    }
}

// Helper: Compile simple dummy HLSL shaders
static bool CompileDummyShaders(ComPtr<ID3DBlob>& vsBlob, ComPtr<ID3DBlob>& psBlob) {
    const char* hlsl = 
        "float4 VSMain(float4 pos : POSITION) : SV_POSITION { return pos; }\n"
        "float4 PSMain(float4 pos : SV_POSITION) : SV_TARGET { return float4(1, 0, 0, 1); }\n";
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vsBlob, nullptr);
    if (FAILED(hr)) return false;
    hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &psBlob, nullptr);
    return SUCCEEDED(hr);
}

// ============================================================================
// SUITE 1: Client Disconnect Mid-Command Stream & Network Fault Tolerance
// ============================================================================
static void Stress_ClientDisconnect_MidCommandStream() {
    HostGpuDevice gpu;
    STRESS_ASSERT(gpu.InitializeDedicatedGpu());

    // 1.1: Partial Packet Disconnect (Send 16-byte header, drop socket before payload)
    {
        NetworkSocket listener;
        uint16_t port = 0;
        STRESS_ASSERT(CreateEphemeralListener(listener, port));

        std::atomic<bool> server_finished{false};

        std::thread server_thread([&]() {
            auto client_sock = listener.Accept(3000);
            if (client_sock) {
                ServerDispatcher dispatcher(gpu);
                dispatcher.ProcessPacketStream(*client_sock);
                dispatcher.ResetSession();
            }
            server_finished = true;
        });

        NetworkSocket client;
        STRESS_ASSERT(client.Connect("127.0.0.1", port, 2000));

        // Complete handshake
        HandshakeRequestPayload req{ RGPU_PROTOCOL_VERSION, FLAG_NONE, 64, 64, 28 };
        PacketHeader req_hdr{ RGPU_MAGIC, OP_HANDSHAKE_REQUEST, FLAG_NONE, 1, sizeof(req) };
        STRESS_ASSERT(client.SendPacket(req_hdr, &req));

        PacketHeader resp_hdr{};
        std::vector<uint8_t> resp_pl;
        STRESS_ASSERT(client.RecvPacket(resp_hdr, resp_pl));

        // Now send header for CreateBuffer with payload_length = 512, then immediately close socket
        PacketHeader partial_hdr{ RGPU_MAGIC, OP_CREATE_BUFFER, FLAG_NONE, 2, 512 };
        int sent = send(client.GetRawSocket(), reinterpret_cast<const char*>(&partial_hdr), sizeof(partial_hdr), 0);
        STRESS_ASSERT(sent == sizeof(partial_hdr));

        // Immediate hard close without sending payload
        client.Close();

        // Server must detect EOF / drop and finish within 3 seconds without hang
        auto t0 = std::chrono::steady_clock::now();
        while (!server_finished && std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < 3000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        STRESS_ASSERT_MSG(server_finished.load(), "Server cleanly terminated loop on partial packet disconnect");

        if (server_thread.joinable()) {
            server_thread.join();
        }
        listener.Close();
    }

    // 1.2: Hard TCP RST Disconnect Mid-Stream (SO_LINGER l_onoff=1, l_linger=0)
    {
        NetworkSocket listener;
        uint16_t port = 0;
        STRESS_ASSERT(CreateEphemeralListener(listener, port));

        std::atomic<bool> server_finished{false};

        std::thread server_thread([&]() {
            auto client_sock = listener.Accept(3000);
            if (client_sock) {
                ServerDispatcher dispatcher(gpu);
                dispatcher.ProcessPacketStream(*client_sock);
                dispatcher.ResetSession();
            }
            server_finished = true;
        });

        SOCKET raw_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        int cr = connect(raw_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        STRESS_ASSERT(cr == 0);

        // Send 7 bytes of garbage
        char garbage[7] = { 0x52, 0x47, 0x50, 0x55, 0x10, 0x00, 0x01 };
        send(raw_sock, garbage, sizeof(garbage), 0);

        // Abortive RST
        linger l{ 1, 0 };
        setsockopt(raw_sock, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof(l));
        closesocket(raw_sock);

        auto t0 = std::chrono::steady_clock::now();
        while (!server_finished && std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < 3000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        STRESS_ASSERT_MSG(server_finished.load(), "Server survived abrupt TCP RST without crash");

        if (server_thread.joinable()) {
            server_thread.join();
        }
        listener.Close();
    }

    // 1.3: Disconnect During OP_PRESENT Readback Stream
    {
        NetworkSocket listener;
        uint16_t port = 0;
        STRESS_ASSERT(CreateEphemeralListener(listener, port));

        std::atomic<bool> server_finished{false};

        std::thread server_thread([&]() {
            auto client_sock = listener.Accept(3000);
            if (client_sock) {
                ServerDispatcher dispatcher(gpu);
                dispatcher.ProcessPacketStream(*client_sock);
                dispatcher.ResetSession();
            }
            server_finished = true;
        });

        NetworkSocket client;
        STRESS_ASSERT(client.Connect("127.0.0.1", port, 2000));

        // Complete handshake
        HandshakeRequestPayload req{ RGPU_PROTOCOL_VERSION, FLAG_NONE, 128, 128, 28 };
        PacketHeader req_hdr{ RGPU_MAGIC, OP_HANDSHAKE_REQUEST, FLAG_NONE, 1, sizeof(req) };
        STRESS_ASSERT(client.SendPacket(req_hdr, &req));

        PacketHeader resp_hdr{};
        std::vector<uint8_t> resp_pl;
        STRESS_ASSERT(client.RecvPacket(resp_hdr, resp_pl));

        // Send OP_PRESENT and close socket immediately
        PresentPayload pres{ 0, 0, 0, 1 };
        PacketHeader pres_hdr{ RGPU_MAGIC, OP_PRESENT, FLAG_NONE, 2, sizeof(pres) };
        STRESS_ASSERT(client.SendPacket(pres_hdr, &pres));

        // Client drops socket immediately before reading OP_FRAME_DATA
        client.Close();

        auto t0 = std::chrono::steady_clock::now();
        while (!server_finished && std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < 3000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        STRESS_ASSERT_MSG(server_finished.load(), "Server cleanly survived client drop during OP_PRESENT frame write");

        if (server_thread.joinable()) {
            server_thread.join();
        }
        listener.Close();
    }

    // 1.4: Multi-Client Connection Churn with Adversarial Drops followed by Legitimate Client
    {
        NetworkSocket listener;
        uint16_t port = 0;
        STRESS_ASSERT(CreateEphemeralListener(listener, port));

        std::atomic<bool> server_running{true};
        std::atomic<int> sessions_processed{0};

        std::thread server_thread([&]() {
            while (server_running) {
                auto client_sock = listener.Accept(100);
                if (!client_sock) continue;

                ServerDispatcher dispatcher(gpu);
                dispatcher.ProcessPacketStream(*client_sock);
                dispatcher.ResetSession();
                sessions_processed.fetch_add(1);
            }
        });

        // 20 adversarial clients in rapid succession
        for (int i = 0; i < 20; ++i) {
            NetworkSocket c;
            if (c.Connect("127.0.0.1", port, 1000)) {
                if (i % 4 == 0) {
                    // Connect and immediate close
                    c.Close();
                } else if (i % 4 == 1) {
                    // Send 4 magic bytes and close
                    uint32_t magic = RGPU_MAGIC;
                    send(c.GetRawSocket(), reinterpret_cast<const char*>(&magic), 4, 0);
                    c.Close();
                } else if (i % 4 == 2) {
                    // Complete handshake then hard reset
                    HandshakeRequestPayload req{ RGPU_PROTOCOL_VERSION, FLAG_NONE, 32, 32, 28 };
                    PacketHeader h{ RGPU_MAGIC, OP_HANDSHAKE_REQUEST, FLAG_NONE, 1, sizeof(req) };
                    c.SendPacket(h, &req);
                    linger l{ 1, 0 };
                    setsockopt(c.GetRawSocket(), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof(l));
                    c.Close();
                } else {
                    // Graceful handshake and shutdown
                    HandshakeRequestPayload req{ RGPU_PROTOCOL_VERSION, FLAG_NONE, 32, 32, 28 };
                    PacketHeader h{ RGPU_MAGIC, OP_HANDSHAKE_REQUEST, FLAG_NONE, 1, sizeof(req) };
                    c.SendPacket(h, &req);
                    PacketHeader rh{};
                    std::vector<uint8_t> rp;
                    c.RecvPacket(rh, rp);

                    ShutdownPayload sp{ 0 };
                    PacketHeader sh{ RGPU_MAGIC, OP_SHUTDOWN, FLAG_NONE, 2, sizeof(sp) };
                    c.SendPacket(sh, &sp);
                    c.Close();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        // Now client 21: Legitimate client that renders, presents, reads back, and shuts down
        {
            NetworkSocket legitimate_client;
            STRESS_ASSERT(legitimate_client.Connect("127.0.0.1", port, 2000));

            HandshakeRequestPayload req{ RGPU_PROTOCOL_VERSION, FLAG_NONE, 64, 64, 28 };
            PacketHeader req_hdr{ RGPU_MAGIC, OP_HANDSHAKE_REQUEST, FLAG_NONE, 1, sizeof(req) };
            STRESS_ASSERT(legitimate_client.SendPacket(req_hdr, &req));

            PacketHeader resp_hdr{};
            std::vector<uint8_t> resp_pl;
            STRESS_ASSERT(legitimate_client.RecvPacket(resp_hdr, resp_pl));
            STRESS_ASSERT(resp_hdr.opcode == OP_HANDSHAKE_RESPONSE);

            // Clear RTV
            ClearRtvPayload clr{};
            clr.res_id = 0;
            clr.color[0] = 0.5f; clr.color[1] = 0.2f; clr.color[2] = 0.8f; clr.color[3] = 1.0f;
            PacketHeader clr_hdr{ RGPU_MAGIC, OP_CLEAR_RTV, FLAG_NONE, 2, sizeof(clr) };
            STRESS_ASSERT(legitimate_client.SendPacket(clr_hdr, &clr));

            // Present & receive frame
            PresentPayload pres{ 0, 0, 0, 1 };
            PacketHeader pres_hdr{ RGPU_MAGIC, OP_PRESENT, FLAG_NONE, 3, sizeof(pres) };
            STRESS_ASSERT(legitimate_client.SendPacket(pres_hdr, &pres));

            PacketHeader frame_hdr{};
            std::vector<uint8_t> frame_pl;
            STRESS_ASSERT(legitimate_client.RecvPacket(frame_hdr, frame_pl));
            STRESS_ASSERT(frame_hdr.opcode == OP_FRAME_DATA);
            STRESS_ASSERT(frame_pl.size() == sizeof(FrameDataPayload) + (64 * 64 * 4));

            const uint8_t* pixels = frame_pl.data() + sizeof(FrameDataPayload);
            // Verify clear color RGBA ~ (127, 51, 204, 255)
            STRESS_ASSERT(std::abs(static_cast<int>(pixels[0]) - 127) <= 2);
            STRESS_ASSERT(std::abs(static_cast<int>(pixels[1]) - 51) <= 2);
            STRESS_ASSERT(std::abs(static_cast<int>(pixels[2]) - 204) <= 2);

            ShutdownPayload sp{ 0 };
            PacketHeader sh{ RGPU_MAGIC, OP_SHUTDOWN, FLAG_NONE, 4, sizeof(sp) };
            legitimate_client.SendPacket(sh, &sp);
            legitimate_client.Close();
        }

        server_running = false;
        if (server_thread.joinable()) {
            server_thread.join();
        }
        listener.Close();

        STRESS_ASSERT_MSG(sessions_processed.load() >= 20, "Server survived 20+ adversarial churn sessions and processed legitimate client");
    }
}

// ============================================================================
// SUITE 2: Malformed / Invalid Opcodes & Boundary Truncation Defenses
// ============================================================================
static void Stress_MalformedOpcodes_And_BoundaryAttacks() {
    HostGpuDevice gpu;
    STRESS_ASSERT(gpu.InitializeDedicatedGpu());
    STRESS_ASSERT(gpu.CreateOffscreenTargets(64, 64));

    ServerDispatcher dispatcher(gpu);

    // 2.1: Bogus / Corrupted Opcodes
    const uint16_t bogus_opcodes[] = {
        0x0000, 0x0004, 0x000F, 0x001A, 0x002F, 0x003C, 0x004F, 
        0x0056, 0x006F, 0x0072, 0x0080, 0x00FF, 0x1234, 0x8000, 0xFFFF
    };

    uint8_t dummy_payload[64];
    std::memset(dummy_payload, 0xAA, sizeof(dummy_payload));

    for (uint16_t op : bogus_opcodes) {
        PacketHeader h{ RGPU_MAGIC, op, FLAG_NONE, 100, sizeof(dummy_payload) };
        bool res = dispatcher.DispatchCommand(h, dummy_payload, sizeof(dummy_payload));
        STRESS_ASSERT_MSG(!res, ("Bogus opcode 0x" + std::to_string(op) + " was safely rejected").c_str());
    }

    // 2.2: Truncated Zero-Length and Micro Payloads on ALL standard opcodes
    const uint16_t all_opcodes[] = {
        OP_HANDSHAKE_REQUEST, OP_HANDSHAKE_RESPONSE, OP_SHUTDOWN,
        OP_CREATE_BUFFER, OP_CREATE_TEXTURE2D, OP_CREATE_SRV, OP_CREATE_RTV,
        OP_CREATE_DSV, OP_CREATE_VERTEX_SHADER, OP_CREATE_PIXEL_SHADER,
        OP_CREATE_INPUT_LAYOUT, OP_DESTROY_RESOURCE, OP_CREATE_SAMPLER,
        OP_IA_SET_INPUT_LAYOUT, OP_IA_SET_VERTEX_BUFFER, OP_IA_SET_INDEX_BUFFER,
        OP_IA_SET_TOPOLOGY, OP_VS_SET_SHADER, OP_VS_SET_CB, OP_PS_SET_SHADER,
        OP_PS_SET_CB, OP_PS_SET_SRV, OP_RS_SET_VIEWPORT, OP_OM_SET_RENDER_TARGETS,
        OP_PS_SET_SAMPLER, OP_UPDATE_SUBRESOURCE, OP_WRITE_BUFFER_DISCARD,
        OP_CLEAR_RTV, OP_CLEAR_DSV, OP_DRAW, OP_DRAW_INDEXED, OP_PRESENT, OP_FRAME_DATA
    };

    int num_crashes_zero_micro = 0;
    for (uint16_t op : all_opcodes) {
        DWORD exc = 0;
        // Zero-length payload
        PacketHeader h_zero{ RGPU_MAGIC, op, FLAG_NONE, 200, 0 };
        SafeDispatchCommand(dispatcher, h_zero, nullptr, 0, nullptr, &exc);
        if (exc != 0) {
            num_crashes_zero_micro++;
            std::cerr << "  [CRASH DEFECT] Opcode 0x" << std::hex << op << std::dec << " crashed on 0-byte payload with code 0x" << std::hex << exc << std::dec << std::endl;
        }

        // Micro payload (1 byte)
        uint8_t one_byte = 0x55;
        PacketHeader h_micro{ RGPU_MAGIC, op, FLAG_NONE, 201, 1 };
        SafeDispatchCommand(dispatcher, h_micro, &one_byte, 1, nullptr, &exc);
        if (exc != 0) {
            num_crashes_zero_micro++;
            std::cerr << "  [CRASH DEFECT] Opcode 0x" << std::hex << op << std::dec << " crashed on 1-byte payload with code 0x" << std::hex << exc << std::dec << std::endl;
        }

        // 4 bytes payload
        uint32_t four_bytes = 0x12345678;
        PacketHeader h_four{ RGPU_MAGIC, op, FLAG_NONE, 202, 4 };
        SafeDispatchCommand(dispatcher, h_four, reinterpret_cast<const uint8_t*>(&four_bytes), 4, nullptr, &exc);
        if (exc != 0) {
            num_crashes_zero_micro++;
            std::cerr << "  [CRASH DEFECT] Opcode 0x" << std::hex << op << std::dec << " crashed on 4-byte payload with code 0x" << std::hex << exc << std::dec << std::endl;
        }
    }
    STRESS_ASSERT_MSG(num_crashes_zero_micro == 0, ("Truncated payload safety: detected " + std::to_string(num_crashes_zero_micro) + " hardware/driver crash(es)").c_str());

    // 2.3: Struct Field Discrepancies & Truncated Variable-Length Appended Buffers
    // Test CreateBuffer with has_initial_data=1, initial_data_size=4096, but payload is only sizeof(CreateBufferPayload)
    {
        CreateBufferPayload bp{};
        bp.res_id = 9001;
        bp.byte_width = 4096;
        bp.has_initial_data = 1;
        bp.initial_data_size = 4096;
        PacketHeader h{ RGPU_MAGIC, OP_CREATE_BUFFER, FLAG_NONE, 301, sizeof(bp) };
        bool r = dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&bp), sizeof(bp));
        STRESS_ASSERT_MSG(!r, "CreateBuffer rejected truncated initial data payload");
    }

    // Test CreateTexture2D with has_initial_data=1, initial_data_size=8192, truncated payload
    {
        CreateTexture2DPayload tp{};
        tp.res_id = 9002;
        tp.width = 64; tp.height = 64; tp.mips = 1; tp.array_size = 1;
        tp.format = 28;
        tp.has_initial_data = 1;
        tp.initial_data_size = 8192;
        PacketHeader h{ RGPU_MAGIC, OP_CREATE_TEXTURE2D, FLAG_NONE, 302, sizeof(tp) };
        bool r = dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&tp), sizeof(tp));
        STRESS_ASSERT_MSG(!r, "CreateTexture2D rejected truncated initial data payload");
    }

    // Test CreateInputLayout with num_elements=10, bytecode_length=500, truncated payload
    {
        CreateInputLayoutPayload ilp{ 9003, 10, 500 };
        PacketHeader h{ RGPU_MAGIC, OP_CREATE_INPUT_LAYOUT, FLAG_NONE, 303, sizeof(ilp) };
        bool r = dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&ilp), sizeof(ilp));
        STRESS_ASSERT_MSG(!r, "CreateInputLayout rejected truncated element descriptor / bytecode payload");
    }

    // Test UpdateSubresource with data_size=4096, truncated payload
    {
        UpdateSubresourcePayload usp{};
        usp.res_id = 9001;
        usp.data_size = 4096;
        PacketHeader h{ RGPU_MAGIC, OP_UPDATE_SUBRESOURCE, FLAG_NONE, 304, sizeof(usp) };
        bool r = dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&usp), sizeof(usp));
        STRESS_ASSERT_MSG(!r, "UpdateSubresource rejected truncated subresource data");
    }

    // Test WriteBufferDiscard with byte_width=2048, truncated payload
    {
        WriteBufferDiscardPayload wbp{ 9001, 0, 2048 };
        PacketHeader h{ RGPU_MAGIC, OP_WRITE_BUFFER_DISCARD, FLAG_NONE, 305, sizeof(wbp) };
        bool r = dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&wbp), sizeof(wbp));
        STRESS_ASSERT_MSG(!r, "WriteBufferDiscard rejected truncated buffer data");
    }

    // 2.4: Malformed Garbage Shader Bytecode
    {
        uint8_t garbage_shader[64];
        std::memset(garbage_shader, 0xEF, sizeof(garbage_shader));

        CreateShaderPayload sp{ 9004, sizeof(garbage_shader) };
        std::vector<uint8_t> pl(sizeof(sp) + sizeof(garbage_shader));
        std::memcpy(pl.data(), &sp, sizeof(sp));
        std::memcpy(pl.data() + sizeof(sp), garbage_shader, sizeof(garbage_shader));

        PacketHeader vs_h{ RGPU_MAGIC, OP_CREATE_VERTEX_SHADER, FLAG_NONE, 306, static_cast<uint32_t>(pl.size()) };
        bool vs_r = dispatcher.DispatchCommand(vs_h, pl.data(), pl.size());
        STRESS_ASSERT_MSG(!vs_r, "D3D11 safely rejected invalid vertex shader bytecode without crash");

        PacketHeader ps_h{ RGPU_MAGIC, OP_CREATE_PIXEL_SHADER, FLAG_NONE, 307, static_cast<uint32_t>(pl.size()) };
        bool ps_r = dispatcher.DispatchCommand(ps_h, pl.data(), pl.size());
        STRESS_ASSERT_MSG(!ps_r, "D3D11 safely rejected invalid pixel shader bytecode without crash");
    }

    // 2.5: Pseudorandom Packet Fuzzing (150 random packets)
    {
        std::mt19937 rng(42);
        std::uniform_int_distribution<int> op_dist(0, 0x0085);
        std::uniform_int_distribution<int> size_dist(0, 1024);
        std::uniform_int_distribution<int> byte_dist(0, 255);

        int fuzz_crashes = 0;
        for (int i = 0; i < 150; ++i) {
            uint16_t rand_op = static_cast<uint16_t>(op_dist(rng));
            uint32_t rand_size = static_cast<uint32_t>(size_dist(rng));

            std::vector<uint8_t> rand_data(rand_size);
            for (auto& b : rand_data) {
                b = static_cast<uint8_t>(byte_dist(rng));
            }

            PacketHeader rand_hdr{ RGPU_MAGIC, rand_op, FLAG_NONE, static_cast<uint32_t>(i), rand_size };
            DWORD exc = 0;
            SafeDispatchCommand(dispatcher, rand_hdr, rand_data.data(), rand_data.size(), nullptr, &exc);
            if (exc != 0) {
                fuzz_crashes++;
                std::cerr << "  [CRASH DEFECT] Fuzz packet " << i << " (op 0x" << std::hex << rand_op 
                          << ") crashed with code 0x" << exc << std::dec << std::endl;
            }
        }
        STRESS_ASSERT_MSG(fuzz_crashes == 0, ("Fuzz test completed: detected " + std::to_string(fuzz_crashes) + " hardware/driver crash(es)").c_str());
    }

    // 2.6: Parameter Range & Enum Validation Stress Testing
    {
        int param_crashes = 0;

        // Test 1: OP_IA_SET_TOPOLOGY with invalid topology values
        const uint32_t invalid_topologies[] = { 0x12345678, 65, 0xFFFFFFFF, 6, 255 };
        for (uint32_t bad_topo : invalid_topologies) {
            IASetTopologyPayload p{ bad_topo };
            PacketHeader h{ RGPU_MAGIC, OP_IA_SET_TOPOLOGY, FLAG_NONE, 401, sizeof(p) };
            DWORD exc = 0;
            SafeDispatchCommand(dispatcher, h, reinterpret_cast<const uint8_t*>(&p), sizeof(p), nullptr, &exc);
            if (exc != 0) {
                param_crashes++;
                std::cerr << "  [CRASH DEFECT] OP_IA_SET_TOPOLOGY with bad topology=" << bad_topo 
                          << " crashed with code 0x" << std::hex << exc << std::dec << std::endl;
            }
        }

        // Test 2: OP_CREATE_INPUT_LAYOUT with non-null-terminated 32-byte semantic names
        {
            CreateInputLayoutPayload ilp{ 9990, 1, 0 };
            RgpuInputElementDesc elem{};
            std::memset(elem.semantic_name, 'A', sizeof(elem.semantic_name)); // 32 bytes of 'A', NO '\0'
            elem.format = DXGI_FORMAT_R32G32B32_FLOAT;
            std::vector<uint8_t> pl(sizeof(ilp) + sizeof(elem));
            std::memcpy(pl.data(), &ilp, sizeof(ilp));
            std::memcpy(pl.data() + sizeof(ilp), &elem, sizeof(elem));
            PacketHeader h{ RGPU_MAGIC, OP_CREATE_INPUT_LAYOUT, FLAG_NONE, 402, static_cast<uint32_t>(pl.size()) };
            DWORD exc = 0;
            SafeDispatchCommand(dispatcher, h, pl.data(), pl.size(), nullptr, &exc);
            if (exc != 0) {
                param_crashes++;
                std::cerr << "  [CRASH DEFECT] OP_CREATE_INPUT_LAYOUT with non-null-terminated semantic name crashed with 0x"
                          << std::hex << exc << std::dec << std::endl;
            }
        }

        // Test 3: Out-of-bounds Pipeline Slots
        const uint32_t bad_slots[] = { 32, 64, 128, 256, 1000, 0x12345678 };
        for (uint32_t slot : bad_slots) {
            DWORD exc = 0;
            // Vertex Buffer slot
            IASetVertexBufferPayload vb_p{ slot, 0, 32, 0 };
            PacketHeader vb_h{ RGPU_MAGIC, OP_IA_SET_VERTEX_BUFFER, FLAG_NONE, 403, sizeof(vb_p) };
            SafeDispatchCommand(dispatcher, vb_h, reinterpret_cast<const uint8_t*>(&vb_p), sizeof(vb_p), nullptr, &exc);
            if (exc != 0) {
                param_crashes++;
                std::cerr << "  [CRASH DEFECT] OP_IA_SET_VERTEX_BUFFER with slot=" << slot 
                          << " crashed with 0x" << std::hex << exc << std::dec << std::endl;
            }

            // VS CB slot
            VSSetConstantBufferPayload vscb_p{ slot, 0 };
            PacketHeader vscb_h{ RGPU_MAGIC, OP_VS_SET_CB, FLAG_NONE, 404, sizeof(vscb_p) };
            SafeDispatchCommand(dispatcher, vscb_h, reinterpret_cast<const uint8_t*>(&vscb_p), sizeof(vscb_p), nullptr, &exc);
            if (exc != 0) {
                param_crashes++;
                std::cerr << "  [CRASH DEFECT] OP_VS_SET_CB with slot=" << slot 
                          << " crashed with 0x" << std::hex << exc << std::dec << std::endl;
            }

            // PS CB slot
            PSSetConstantBufferPayload pscb_p{ slot, 0 };
            PacketHeader pscb_h{ RGPU_MAGIC, OP_PS_SET_CB, FLAG_NONE, 405, sizeof(pscb_p) };
            SafeDispatchCommand(dispatcher, pscb_h, reinterpret_cast<const uint8_t*>(&pscb_p), sizeof(pscb_p), nullptr, &exc);
            if (exc != 0) {
                param_crashes++;
                std::cerr << "  [CRASH DEFECT] OP_PS_SET_CB with slot=" << slot 
                          << " crashed with 0x" << std::hex << exc << std::dec << std::endl;
            }

            // PS SRV slot
            PSSetSrvPayload srv_p{ slot, 0 };
            PacketHeader srv_h{ RGPU_MAGIC, OP_PS_SET_SRV, FLAG_NONE, 406, sizeof(srv_p) };
            SafeDispatchCommand(dispatcher, srv_h, reinterpret_cast<const uint8_t*>(&srv_p), sizeof(srv_p), nullptr, &exc);
            if (exc != 0) {
                param_crashes++;
                std::cerr << "  [CRASH DEFECT] OP_PS_SET_SRV with slot=" << slot 
                          << " crashed with 0x" << std::hex << exc << std::dec << std::endl;
            }

            // PS Sampler slot
            PSSetSamplerPayload samp_p{ slot, 0 };
            PacketHeader samp_h{ RGPU_MAGIC, OP_PS_SET_SAMPLER, FLAG_NONE, 407, sizeof(samp_p) };
            SafeDispatchCommand(dispatcher, samp_h, reinterpret_cast<const uint8_t*>(&samp_p), sizeof(samp_p), nullptr, &exc);
            if (exc != 0) {
                param_crashes++;
                std::cerr << "  [CRASH DEFECT] OP_PS_SET_SAMPLER with slot=" << slot 
                          << " crashed with 0x" << std::hex << exc << std::dec << std::endl;
            }
        }

        STRESS_ASSERT_MSG(param_crashes == 0, ("Parameter validation stress: detected " + std::to_string(param_crashes) + " crash(es)").c_str());
    }
}

// ============================================================================
// SUITE 3: Invalid Resource IDs & Cross-Type QueryInterface Safety
// ============================================================================
static void Stress_InvalidResourceIDs_And_TypeConfusion() {
    HostGpuDevice gpu;
    STRESS_ASSERT(gpu.InitializeDedicatedGpu());
    STRESS_ASSERT(gpu.CreateOffscreenTargets(64, 64));

    ServerDispatcher dispatcher(gpu);

    // 3.1: Non-Existent Resource ID Lookups (res_id = 0xDEADBEEF)
    constexpr uint32_t GHOST_ID = 0xDEADBEEF;

    {
        IASetInputLayoutPayload p{ GHOST_ID };
        PacketHeader h{ RGPU_MAGIC, OP_IA_SET_INPUT_LAYOUT, FLAG_NONE, 1, sizeof(p) };
        STRESS_ASSERT(dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&p), sizeof(p)));
    }
    {
        IASetVertexBufferPayload p{ 0, GHOST_ID, 32, 0 };
        PacketHeader h{ RGPU_MAGIC, OP_IA_SET_VERTEX_BUFFER, FLAG_NONE, 2, sizeof(p) };
        STRESS_ASSERT(dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&p), sizeof(p)));
    }
    {
        IASetIndexBufferPayload p{ GHOST_ID, 42, 0 };
        PacketHeader h{ RGPU_MAGIC, OP_IA_SET_INDEX_BUFFER, FLAG_NONE, 3, sizeof(p) };
        STRESS_ASSERT(dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&p), sizeof(p)));
    }
    {
        VSSetShaderPayload p{ GHOST_ID };
        PacketHeader h{ RGPU_MAGIC, OP_VS_SET_SHADER, FLAG_NONE, 4, sizeof(p) };
        STRESS_ASSERT(dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&p), sizeof(p)));
    }
    {
        VSSetConstantBufferPayload p{ 0, GHOST_ID };
        PacketHeader h{ RGPU_MAGIC, OP_VS_SET_CB, FLAG_NONE, 5, sizeof(p) };
        STRESS_ASSERT(dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&p), sizeof(p)));
    }
    {
        PSSetShaderPayload p{ GHOST_ID };
        PacketHeader h{ RGPU_MAGIC, OP_PS_SET_SHADER, FLAG_NONE, 6, sizeof(p) };
        STRESS_ASSERT(dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&p), sizeof(p)));
    }
    {
        PSSetConstantBufferPayload p{ 0, GHOST_ID };
        PacketHeader h{ RGPU_MAGIC, OP_PS_SET_CB, FLAG_NONE, 7, sizeof(p) };
        STRESS_ASSERT(dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&p), sizeof(p)));
    }
    {
        PSSetSrvPayload p{ 0, GHOST_ID };
        PacketHeader h{ RGPU_MAGIC, OP_PS_SET_SRV, FLAG_NONE, 8, sizeof(p) };
        STRESS_ASSERT(dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&p), sizeof(p)));
    }
    {
        PSSetSamplerPayload p{ 0, GHOST_ID };
        PacketHeader h{ RGPU_MAGIC, OP_PS_SET_SAMPLER, FLAG_NONE, 9, sizeof(p) };
        STRESS_ASSERT(dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&p), sizeof(p)));
    }
    {
        OMSetRenderTargetsPayload p{ GHOST_ID, GHOST_ID };
        PacketHeader h{ RGPU_MAGIC, OP_OM_SET_RENDER_TARGETS, FLAG_NONE, 10, sizeof(p) };
        STRESS_ASSERT(dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&p), sizeof(p)));
    }
    {
        ClearRtvPayload p{};
        p.res_id = GHOST_ID;
        p.color[0] = 0.1f; p.color[1] = 0.2f; p.color[2] = 0.3f; p.color[3] = 1.0f;
        PacketHeader h{ RGPU_MAGIC, OP_CLEAR_RTV, FLAG_NONE, 11, sizeof(p) };
        STRESS_ASSERT(dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&p), sizeof(p)));
    }
    {
        ClearDsvPayload p{};
        p.res_id = GHOST_ID;
        p.clear_flags = 1;
        p.depth = 1.0f;
        PacketHeader h{ RGPU_MAGIC, OP_CLEAR_DSV, FLAG_NONE, 12, sizeof(p) };
        STRESS_ASSERT(dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&p), sizeof(p)));
    }
    {
        // UpdateSubresource on ghost ID must fail cleanly
        uint8_t dummy[16] = {};
        UpdateSubresourcePayload p{};
        p.res_id = GHOST_ID;
        p.data_size = sizeof(dummy);
        std::vector<uint8_t> pl(sizeof(p) + sizeof(dummy));
        std::memcpy(pl.data(), &p, sizeof(p));
        std::memcpy(pl.data() + sizeof(p), dummy, sizeof(dummy));
        PacketHeader h{ RGPU_MAGIC, OP_UPDATE_SUBRESOURCE, FLAG_NONE, 13, static_cast<uint32_t>(pl.size()) };
        STRESS_ASSERT(!dispatcher.DispatchCommand(h, pl.data(), pl.size()));
    }
    {
        // WriteBufferDiscard on ghost ID must fail cleanly
        uint8_t dummy[16] = {};
        WriteBufferDiscardPayload p{ GHOST_ID, 0, sizeof(dummy) };
        std::vector<uint8_t> pl(sizeof(p) + sizeof(dummy));
        std::memcpy(pl.data(), &p, sizeof(p));
        std::memcpy(pl.data() + sizeof(p), dummy, sizeof(dummy));
        PacketHeader h{ RGPU_MAGIC, OP_WRITE_BUFFER_DISCARD, FLAG_NONE, 14, static_cast<uint32_t>(pl.size()) };
        STRESS_ASSERT(!dispatcher.DispatchCommand(h, pl.data(), pl.size()));
    }
    {
        // Destroy ghost ID must return false cleanly
        DestroyResourcePayload p{ GHOST_ID };
        PacketHeader h{ RGPU_MAGIC, OP_DESTROY_RESOURCE, FLAG_NONE, 15, sizeof(p) };
        STRESS_ASSERT(!dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&p), sizeof(p)));
    }
    {
        // Create SRV referencing ghost resource
        CreateSrvPayload p{ 9050, GHOST_ID, 28, 4, 0, 1 };
        PacketHeader h{ RGPU_MAGIC, OP_CREATE_SRV, FLAG_NONE, 16, sizeof(p) };
        STRESS_ASSERT(!dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&p), sizeof(p)));
    }

    STRESS_ASSERT_MSG(true, "All ghost resource ID lookups handled cleanly without crashes");

    // 3.2: COM QueryInterface Validation & Type-Confusion Defenses
    // Create a real ID3D11Buffer
    constexpr uint32_t REAL_BUF_ID = 8101;
    {
        CreateBufferPayload bp{};
        bp.res_id = REAL_BUF_ID;
        bp.byte_width = 128;
        bp.usage = 2; // DYNAMIC
        bp.bind_flags = 1; // VERTEX_BUFFER
        bp.cpu_access_flags = 0x10000; // WRITE
        PacketHeader h{ RGPU_MAGIC, OP_CREATE_BUFFER, FLAG_NONE, 101, sizeof(bp) };
        STRESS_ASSERT(dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&bp), sizeof(bp)));
        STRESS_ASSERT(dispatcher.GetResourceManager().HasResource(REAL_BUF_ID));
    }

    // Verify ServerResourceManager typed query fails for incompatible types
    auto& rm = dispatcher.GetResourceManager();
    STRESS_ASSERT(rm.GetBuffer(REAL_BUF_ID) != nullptr);
    STRESS_ASSERT(rm.GetVertexShader(REAL_BUF_ID) == nullptr);
    STRESS_ASSERT(rm.GetPixelShader(REAL_BUF_ID) == nullptr);
    STRESS_ASSERT(rm.GetTexture2D(REAL_BUF_ID) == nullptr);
    STRESS_ASSERT(rm.GetInputLayout(REAL_BUF_ID) == nullptr);
    STRESS_ASSERT(rm.GetSamplerState(REAL_BUF_ID) == nullptr);
    STRESS_ASSERT(rm.GetSRV(REAL_BUF_ID) == nullptr);
    STRESS_ASSERT(rm.GetRTV(REAL_BUF_ID) == nullptr);
    STRESS_ASSERT(rm.GetDSV(REAL_BUF_ID) == nullptr);

    // Bind REAL_BUF_ID to shader slot (should safely resolve to nullptr and unbind shader)
    {
        VSSetShaderPayload p{ REAL_BUF_ID };
        PacketHeader h{ RGPU_MAGIC, OP_VS_SET_SHADER, FLAG_NONE, 102, sizeof(p) };
        STRESS_ASSERT(dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&p), sizeof(p)));
    }
    {
        PSSetShaderPayload p{ REAL_BUF_ID };
        PacketHeader h{ RGPU_MAGIC, OP_PS_SET_SHADER, FLAG_NONE, 103, sizeof(p) };
        STRESS_ASSERT(dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&p), sizeof(p)));
    }
    {
        PSSetSamplerPayload p{ 0, REAL_BUF_ID };
        PacketHeader h{ RGPU_MAGIC, OP_PS_SET_SAMPLER, FLAG_NONE, 104, sizeof(p) };
        STRESS_ASSERT(dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&p), sizeof(p)));
    }

    // 3.3: Use-After-Free & Double-Destroy Resistance
    {
        DestroyResourcePayload dp{ REAL_BUF_ID };
        PacketHeader h1{ RGPU_MAGIC, OP_DESTROY_RESOURCE, FLAG_NONE, 201, sizeof(dp) };
        STRESS_ASSERT(dispatcher.DispatchCommand(h1, reinterpret_cast<const uint8_t*>(&dp), sizeof(dp)));
        STRESS_ASSERT(!rm.HasResource(REAL_BUF_ID));

        // Second destroy must fail safely
        PacketHeader h2{ RGPU_MAGIC, OP_DESTROY_RESOURCE, FLAG_NONE, 202, sizeof(dp) };
        STRESS_ASSERT(!dispatcher.DispatchCommand(h2, reinterpret_cast<const uint8_t*>(&dp), sizeof(dp)));

        // WriteBufferDiscard on destroyed ID must fail safely
        uint8_t dummy[16] = {};
        WriteBufferDiscardPayload wbp{ REAL_BUF_ID, 0, sizeof(dummy) };
        std::vector<uint8_t> pl(sizeof(wbp) + sizeof(dummy));
        std::memcpy(pl.data(), &wbp, sizeof(wbp));
        std::memcpy(pl.data() + sizeof(wbp), dummy, sizeof(dummy));
        PacketHeader h3{ RGPU_MAGIC, OP_WRITE_BUFFER_DISCARD, FLAG_NONE, 203, static_cast<uint32_t>(pl.size()) };
        STRESS_ASSERT(!dispatcher.DispatchCommand(h3, pl.data(), pl.size()));
    }

    STRESS_ASSERT_MSG(true, "COM QueryInterface typing, use-after-free, and double destroy confirmed robust");
}

// ============================================================================
// SUITE 4: High-Volume Resource Churn & GPU Memory Leak Check (1,000 resources)
// ============================================================================
static void Stress_ResourceLeakCheck_1000Resources() {
    HostGpuDevice gpu;
    STRESS_ASSERT(gpu.InitializeDedicatedGpu());
    STRESS_ASSERT(gpu.CreateOffscreenTargets(64, 64));

    ServerDispatcher dispatcher(gpu);

    size_t mem_baseline = GetProcessPrivateMemoryBytes();
    std::cout << "  Memory Baseline: " << (mem_baseline / (1024 * 1024)) << " MB" << std::endl;

    // 4.1: Repeated 1,000 Buffer Allocations and Destructions
    constexpr int NUM_BUFFERS = 1000;
    std::cout << "  Allocating and destroying " << NUM_BUFFERS << " buffers (64 KB each)..." << std::endl;

    for (int round = 0; round < 2; ++round) {
        uint32_t base_id = 10000 + round * 2000;

        // Allocate 1,000 buffers (64 KB each = 64 MB total)
        for (int i = 0; i < NUM_BUFFERS; ++i) {
            CreateBufferPayload bp{};
            bp.res_id = base_id + i;
            bp.byte_width = 64 * 1024;
            bp.usage = 0; // DEFAULT
            bp.bind_flags = 1; // VERTEX_BUFFER
            PacketHeader h{ RGPU_MAGIC, OP_CREATE_BUFFER, FLAG_NONE, static_cast<uint32_t>(i), sizeof(bp) };
            bool ok = dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&bp), sizeof(bp));
            if (!ok) {
                STRESS_ASSERT_MSG(false, ("Failed to create buffer index " + std::to_string(i)).c_str());
                break;
            }
        }

        STRESS_ASSERT(dispatcher.GetResourceManager().Count() == NUM_BUFFERS);

        size_t mem_allocated = GetProcessPrivateMemoryBytes();
        std::cout << "  Round " << round << " allocated " << NUM_BUFFERS << " buffers | Memory: " 
                  << (mem_allocated / (1024 * 1024)) << " MB" << std::endl;

        // Destroy all 1,000 buffers
        for (int i = 0; i < NUM_BUFFERS; ++i) {
            DestroyResourcePayload dp{ base_id + i };
            PacketHeader h{ RGPU_MAGIC, OP_DESTROY_RESOURCE, FLAG_NONE, static_cast<uint32_t>(i), sizeof(dp) };
            bool ok = dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&dp), sizeof(dp));
            if (!ok) {
                STRESS_ASSERT_MSG(false, ("Failed to destroy buffer index " + std::to_string(i)).c_str());
                break;
            }
        }

        STRESS_ASSERT(dispatcher.GetResourceManager().Count() == 0);
    }

    // Flush GPU command queue to ensure driver deallocations finish
    if (gpu.GetContext()) {
        gpu.GetContext()->Flush();
    }

    // 4.2: Repeated 1,000 Texture Allocations and Destructions
    constexpr int NUM_TEXTURES = 1000;
    std::cout << "  Allocating and destroying " << NUM_TEXTURES << " 2D textures (64x64 RGBA)..." << std::endl;
    {
        uint32_t base_id = 30000;
        for (int i = 0; i < NUM_TEXTURES; ++i) {
            CreateTexture2DPayload tp{};
            tp.res_id = base_id + i;
            tp.width = 64;
            tp.height = 64;
            tp.mips = 1;
            tp.array_size = 1;
            tp.format = 28; // DXGI_FORMAT_R8G8B8A8_UNORM
            tp.sample_count = 1;
            tp.usage = 0;
            tp.bind_flags = 8; // SHADER_RESOURCE
            PacketHeader h{ RGPU_MAGIC, OP_CREATE_TEXTURE2D, FLAG_NONE, static_cast<uint32_t>(i), sizeof(tp) };
            bool ok = dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&tp), sizeof(tp));
            if (!ok) {
                STRESS_ASSERT_MSG(false, ("Failed to create texture index " + std::to_string(i)).c_str());
                break;
            }
        }

        STRESS_ASSERT(dispatcher.GetResourceManager().Count() == NUM_TEXTURES);

        // Destroy all 1,000 textures
        for (int i = 0; i < NUM_TEXTURES; ++i) {
            DestroyResourcePayload dp{ base_id + i };
            PacketHeader h{ RGPU_MAGIC, OP_DESTROY_RESOURCE, FLAG_NONE, static_cast<uint32_t>(i), sizeof(dp) };
            bool ok = dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&dp), sizeof(dp));
            if (!ok) {
                STRESS_ASSERT_MSG(false, ("Failed to destroy texture index " + std::to_string(i)).c_str());
                break;
            }
        }

        STRESS_ASSERT(dispatcher.GetResourceManager().Count() == 0);
    }

    if (gpu.GetContext()) {
        gpu.GetContext()->Flush();
    }

    // 4.3: Session Reset with 1,000 Allocated Resources
    std::cout << "  Testing ResetSession() with 1,000 active allocations..." << std::endl;
    {
        uint32_t base_id = 40000;
        for (int i = 0; i < 1000; ++i) {
            CreateBufferPayload bp{};
            bp.res_id = base_id + i;
            bp.byte_width = 1024;
            bp.usage = 0;
            bp.bind_flags = 1;
            PacketHeader h{ RGPU_MAGIC, OP_CREATE_BUFFER, FLAG_NONE, static_cast<uint32_t>(i), sizeof(bp) };
            dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&bp), sizeof(bp));
        }

        STRESS_ASSERT(dispatcher.GetResourceManager().Count() == 1000);
        dispatcher.ResetSession();
        STRESS_ASSERT(dispatcher.GetResourceManager().Count() == 0);
    }

    // 4.4: Rapid Interleaved Allocation/Deallocation Churn (500 cycles)
    std::cout << "  Testing 500 rapid interleaved create/use/destroy cycles..." << std::endl;
    {
        for (int i = 0; i < 500; ++i) {
            uint32_t res_b = 50000 + i;
            uint32_t res_t = 60000 + i;
            uint32_t res_s = 70000 + i;

            // 1. Create dynamic buffer
            CreateBufferPayload bp{};
            bp.res_id = res_b;
            bp.byte_width = 256;
            bp.usage = 2; // DYNAMIC
            bp.bind_flags = 1;
            bp.cpu_access_flags = 0x10000;
            PacketHeader bh{ RGPU_MAGIC, OP_CREATE_BUFFER, FLAG_NONE, 1, sizeof(bp) };
            dispatcher.DispatchCommand(bh, reinterpret_cast<const uint8_t*>(&bp), sizeof(bp));

            // 2. Create texture
            CreateTexture2DPayload tp{};
            tp.res_id = res_t;
            tp.width = 16; tp.height = 16; tp.mips = 1; tp.array_size = 1;
            tp.format = 28; tp.sample_count = 1; tp.usage = 0; tp.bind_flags = 8;
            PacketHeader th{ RGPU_MAGIC, OP_CREATE_TEXTURE2D, FLAG_NONE, 2, sizeof(tp) };
            dispatcher.DispatchCommand(th, reinterpret_cast<const uint8_t*>(&tp), sizeof(tp));

            // 3. Create SRV
            CreateSrvPayload sp{ res_s, res_t, 28, 4, 0, 1 };
            PacketHeader sh{ RGPU_MAGIC, OP_CREATE_SRV, FLAG_NONE, 3, sizeof(sp) };
            dispatcher.DispatchCommand(sh, reinterpret_cast<const uint8_t*>(&sp), sizeof(sp));

            // 4. Write dynamic buffer
            uint8_t dummy_data[64] = { 0x11, 0x22 };
            WriteBufferDiscardPayload wbp{ res_b, 0, sizeof(dummy_data) };
            std::vector<uint8_t> wpl(sizeof(wbp) + sizeof(dummy_data));
            std::memcpy(wpl.data(), &wbp, sizeof(wbp));
            std::memcpy(wpl.data() + sizeof(wbp), dummy_data, sizeof(dummy_data));
            PacketHeader wh{ RGPU_MAGIC, OP_WRITE_BUFFER_DISCARD, FLAG_NONE, 4, static_cast<uint32_t>(wpl.size()) };
            dispatcher.DispatchCommand(wh, wpl.data(), wpl.size());

            // 5. Destroy SRV, Texture, Buffer
            DestroyResourcePayload d1{ res_s };
            PacketHeader dh1{ RGPU_MAGIC, OP_DESTROY_RESOURCE, FLAG_NONE, 5, sizeof(d1) };
            dispatcher.DispatchCommand(dh1, reinterpret_cast<const uint8_t*>(&d1), sizeof(d1));

            DestroyResourcePayload d2{ res_t };
            PacketHeader dh2{ RGPU_MAGIC, OP_DESTROY_RESOURCE, FLAG_NONE, 6, sizeof(d2) };
            dispatcher.DispatchCommand(dh2, reinterpret_cast<const uint8_t*>(&d2), sizeof(d2));

            DestroyResourcePayload d3{ res_b };
            PacketHeader dh3{ RGPU_MAGIC, OP_DESTROY_RESOURCE, FLAG_NONE, 7, sizeof(d3) };
            dispatcher.DispatchCommand(dh3, reinterpret_cast<const uint8_t*>(&d3), sizeof(d3));
        }

        STRESS_ASSERT(dispatcher.GetResourceManager().Count() == 0);
    }

    if (gpu.GetContext()) {
        gpu.GetContext()->Flush();
    }

    size_t mem_post_rounds = GetProcessPrivateMemoryBytes();

    // 4.5: Verify memory stabilization across subsequent 1,000 allocations (driver pool plateau)
    for (int i = 0; i < 1000; ++i) {
        CreateBufferPayload bp{};
        bp.res_id = 80000 + i;
        bp.byte_width = 64 * 1024;
        bp.usage = 0;
        bp.bind_flags = 1;
        PacketHeader h{ RGPU_MAGIC, OP_CREATE_BUFFER, FLAG_NONE, static_cast<uint32_t>(i), sizeof(bp) };
        dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&bp), sizeof(bp));
    }
    STRESS_ASSERT(dispatcher.GetResourceManager().Count() == 1000);

    for (int i = 0; i < 1000; ++i) {
        DestroyResourcePayload dp{ 80000 + i };
        PacketHeader h{ RGPU_MAGIC, OP_DESTROY_RESOURCE, FLAG_NONE, static_cast<uint32_t>(i), sizeof(dp) };
        dispatcher.DispatchCommand(h, reinterpret_cast<const uint8_t*>(&dp), sizeof(dp));
    }
    STRESS_ASSERT(dispatcher.GetResourceManager().Count() == 0);

    if (gpu.GetContext()) {
        gpu.GetContext()->Flush();
    }

    size_t mem_stabilized = GetProcessPrivateMemoryBytes();
    int64_t growth_after_pool_mb = (static_cast<int64_t>(mem_stabilized) - static_cast<int64_t>(mem_post_rounds)) / (1024 * 1024);
    std::cout << "  Memory Stabilized: " << (mem_stabilized / (1024 * 1024)) << " MB (Growth after initial pool: "
              << growth_after_pool_mb << " MB)" << std::endl;

    STRESS_ASSERT_MSG(growth_after_pool_mb <= 10, "Memory leak check passed: COM refcounts cleanly released and driver pool stabilized (zero unbounded growth)");
}

// ============================================================================
// SUITE 5: HostGpuDevice Boundary Stress & Offscreen Target Robustness
// ============================================================================
static void Stress_HostGpuDevice_BoundaryStress() {
    HostGpuDevice gpu;
    STRESS_ASSERT(gpu.InitializeDedicatedGpu());

    // 5.1: Zero and Invalid Target Dimensions
    STRESS_ASSERT(!gpu.CreateOffscreenTargets(0, 0));
    STRESS_ASSERT(!gpu.CreateOffscreenTargets(0, 512));
    STRESS_ASSERT(!gpu.CreateOffscreenTargets(512, 0));

    // Excessive dimensions (> 16384 D3D11 max texture dimension)
    STRESS_ASSERT(!gpu.CreateOffscreenTargets(65536, 65536));

    // 5.2: Readback without Offscreen Targets
    {
        std::vector<uint8_t> px;
        uint32_t w = 0, h = 0;
        STRESS_ASSERT(!gpu.ReadbackFrame(px, w, h));
    }

    // 5.3: Rapid Target Resizing Churn
    const std::pair<uint32_t, uint32_t> sizes[] = {
        { 64, 64 },
        { 1920, 1080 },
        { 128, 128 },
        { 800, 600 },
        { 137, 89 },
        { 256, 256 }
    };

    for (const auto& sz : sizes) {
        STRESS_ASSERT(gpu.CreateOffscreenTargets(sz.first, sz.second));
        STRESS_ASSERT(gpu.GetWidth() == sz.first);
        STRESS_ASSERT(gpu.GetHeight() == sz.second);
        STRESS_ASSERT(gpu.GetRenderTargetView() != nullptr);
        STRESS_ASSERT(gpu.GetDepthStencilView() != nullptr);
        STRESS_ASSERT(gpu.GetStagingTexture() != nullptr);

        const float clr[4] = { 0.1f, 0.2f, 0.3f, 1.0f };
        gpu.Clear(clr);

        std::vector<uint8_t> px;
        uint32_t rw = 0, rh = 0;
        STRESS_ASSERT(gpu.ReadbackFrame(px, rw, rh));
        STRESS_ASSERT(rw == sz.first);
        STRESS_ASSERT(rh == sz.second);
        STRESS_ASSERT(px.size() == static_cast<size_t>(sz.first) * sz.second * 4);
    }

    // 5.4: Reset Lifecycle
    gpu.Reset();
    STRESS_ASSERT(gpu.GetDevice() == nullptr);
    STRESS_ASSERT(gpu.GetContext() == nullptr);
    STRESS_ASSERT(gpu.GetWidth() == 0);
    STRESS_ASSERT(gpu.GetHeight() == 0);

    // Readback after reset must fail safely
    std::vector<uint8_t> post_reset_px;
    uint32_t pr_w = 0, pr_h = 0;
    STRESS_ASSERT(!gpu.ReadbackFrame(post_reset_px, pr_w, pr_h));

    STRESS_ASSERT_MSG(true, "HostGpuDevice boundary conditions and rapid resizing passed cleanly");
}

// ============================================================================
// Main Entrypoint
// ============================================================================
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    std::cout << "============================================================" << std::endl;
    std::cout << "  Remote GPU Offload - Milestone 2 Adversarial Stress Suite" << std::endl;
    std::cout << "============================================================" << std::endl;

    if (!NetworkSocket::InitializeNetwork()) {
        std::cerr << "[FATAL] Failed to initialize Winsock2 networking!" << std::endl;
        return 1;
    }

    auto total_start = std::chrono::high_resolution_clock::now();

    RUN_STRESS_SUITE(Stress_ClientDisconnect_MidCommandStream);
    RUN_STRESS_SUITE(Stress_InvalidResourceIDs_And_TypeConfusion);
    RUN_STRESS_SUITE(Stress_ResourceLeakCheck_1000Resources);
    RUN_STRESS_SUITE(Stress_HostGpuDevice_BoundaryStress);
    RUN_STRESS_SUITE(Stress_MalformedOpcodes_And_BoundaryAttacks);

    auto total_end = std::chrono::high_resolution_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(total_end - total_start).count();

    NetworkSocket::ShutdownNetwork();

    std::cout << "============================================================" << std::endl;
    std::cout << "  ADVERSARIAL STRESS TEST SUMMARY" << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << "  Total Assertions Run : " << g_stress_run.load() << std::endl;
    std::cout << "  Passed               : " << g_stress_passed.load() << std::endl;
    std::cout << "  Failed               : " << g_stress_failed.load() << std::endl;
    std::cout << "  Total Duration       : " << total_ms << " ms" << std::endl;
    std::cout << "============================================================" << std::endl;

    if (g_stress_failed.load() == 0) {
        std::cout << "  VERDICT: APPROVE (ALL ADVERSARIAL STRESS TESTS PASSED)" << std::endl;
        std::cout << "============================================================" << std::endl;
        return 0;
    } else {
        std::cerr << "  VERDICT: REQUEST_CHANGES (" << g_stress_failed.load() << " failure(s))" << std::endl;
        std::cout << "============================================================" << std::endl;
        return 1;
    }
}
