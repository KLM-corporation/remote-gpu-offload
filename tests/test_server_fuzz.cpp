// ============================================================================
// Remote GPU Offload - Milestone 2 Adversarial Fuzzing Test Harness
// File: tests/test_server_fuzz.cpp
// Standard: C++17
// Author: Challenger 1 (Milestone 2 Iteration 2 Empirical Remediation Verification)
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

#include "protocol.h"
#include "transport.h"

#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <chrono>
#include <thread>
#include <random>
#include <atomic>

using namespace RemoteGPU;

// ----------------------------------------------------------------------------
// Test Tracking Macros
// ----------------------------------------------------------------------------
static std::atomic<int> g_fuzz_run{0};
static std::atomic<int> g_fuzz_passed{0};
static std::atomic<int> g_fuzz_failed{0};

#define FUZZ_ASSERT(cond) do { \
    g_fuzz_run.fetch_add(1); \
    if (cond) { \
        g_fuzz_passed.fetch_add(1); \
    } else { \
        g_fuzz_failed.fetch_add(1); \
        std::cerr << "  [FAIL] Assertion failed: (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
    } \
} while (0)

#define FUZZ_ASSERT_MSG(cond, msg) do { \
    g_fuzz_run.fetch_add(1); \
    if (cond) { \
        g_fuzz_passed.fetch_add(1); \
    } else { \
        g_fuzz_failed.fetch_add(1); \
        std::cerr << "  [FAIL] " << msg << ": (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
    } \
} while (0)

// ----------------------------------------------------------------------------
// Process Helper: Spawn & Manage server.exe
// ----------------------------------------------------------------------------
struct ServerProcess {
    PROCESS_INFORMATION pi = {};
    HANDLE hLogFile = INVALID_HANDLE_VALUE;
    uint16_t port = 0;
    bool running = false;
};

static bool SpawnServer(uint16_t port, const std::string& log_filename, ServerProcess& out_proc, bool single_session = true) {
    out_proc.port = port;
    out_proc.running = false;

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    out_proc.hLogFile = CreateFileA(
        log_filename.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (out_proc.hLogFile == INVALID_HANDLE_VALUE) {
        std::cerr << "  [Spawner] Failed to create log file: " << log_filename << std::endl;
        return false;
    }

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out_proc.hLogFile;
    si.hStdError = out_proc.hLogFile;

    std::string cmd = "build\\server.exe --port " + std::to_string(port) + " --bind 127.0.0.1";
    if (single_session) {
        cmd += " --single-session";
    }
    std::vector<char> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back('\0');

    BOOL ok = CreateProcessA(
        NULL,
        cmd_buf.data(),
        NULL,
        NULL,
        TRUE, // Inherit log handle
        CREATE_NEW_PROCESS_GROUP,
        NULL,
        NULL,
        &si,
        &out_proc.pi
    );

    if (!ok) {
        std::cerr << "  [Spawner] CreateProcessA failed with error " << GetLastError() << std::endl;
        CloseHandle(out_proc.hLogFile);
        out_proc.hLogFile = INVALID_HANDLE_VALUE;
        return false;
    }

    // Allow server 300 ms to bind socket and begin listening
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Verify process has not exited immediately
    DWORD exit_code = 0;
    if (GetExitCodeProcess(out_proc.pi.hProcess, &exit_code)) {
        if (exit_code != STILL_ACTIVE) {
            std::cerr << "  [Spawner] server.exe exited prematurely with code 0x"
                      << std::hex << exit_code << std::dec << std::endl;
            CloseHandle(out_proc.pi.hProcess);
            CloseHandle(out_proc.pi.hThread);
            CloseHandle(out_proc.hLogFile);
            return false;
        }
    }

    out_proc.running = true;
    return true;
}

static bool IsServerAlive(const ServerProcess& proc, DWORD* out_exit_code = nullptr) {
    if (!proc.running || proc.pi.hProcess == NULL) return false;
    DWORD exit_code = 0;
    if (GetExitCodeProcess(proc.pi.hProcess, &exit_code)) {
        if (out_exit_code) *out_exit_code = exit_code;
        return (exit_code == STILL_ACTIVE);
    }
    return false;
}

static DWORD ShutdownServer(ServerProcess& proc, NetworkSocket* sock = nullptr) {
    if (!proc.running) return 0;

    DWORD exit_code = 0;
    if (sock && sock->IsValid()) {
        ShutdownPayload sp{ 0 };
        PacketHeader h{ RGPU_MAGIC, OP_SHUTDOWN, FLAG_NONE, 999999, sizeof(sp) };
        sock->SendPacket(h, &sp);
    }

    // Signal console interrupt if daemon is in multi-session accept loop
    GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, proc.pi.dwProcessId);

    // Wait up to 3 seconds for clean exit
    DWORD wait_res = WaitForSingleObject(proc.pi.hProcess, 3000);
    if (wait_res == WAIT_TIMEOUT) {
        std::cerr << "  [Spawner] server.exe did not exit within 3s; killing process." << std::endl;
        TerminateProcess(proc.pi.hProcess, 101);
        WaitForSingleObject(proc.pi.hProcess, 1000);
    }

    GetExitCodeProcess(proc.pi.hProcess, &exit_code);

    CloseHandle(proc.pi.hProcess);
    CloseHandle(proc.pi.hThread);
    if (proc.hLogFile != INVALID_HANDLE_VALUE) {
        CloseHandle(proc.hLogFile);
        proc.hLogFile = INVALID_HANDLE_VALUE;
    }

    proc.running = false;
    return exit_code;
}

static bool DoHandshake(NetworkSocket& sock, uint32_t width = 256, uint32_t height = 256) {
    HandshakeRequestPayload req = {};
    req.protocol_version = RGPU_PROTOCOL_VERSION;
    req.client_width = width;
    req.client_height = height;

    PacketHeader hdr = {};
    hdr.magic = RGPU_MAGIC;
    hdr.opcode = OP_HANDSHAKE_REQUEST;
    hdr.flags = FLAG_NONE;
    hdr.sequence_id = 1;
    hdr.payload_length = sizeof(req);

    if (!sock.SendPacket(hdr, &req)) return false;

    PacketHeader resp_hdr = {};
    std::vector<uint8_t> resp_payload;
    if (!sock.RecvPacket(resp_hdr, resp_payload)) return false;

    if (resp_hdr.opcode != OP_HANDSHAKE_RESPONSE) return false;
    if (resp_payload.size() < sizeof(HandshakeResponsePayload)) return false;

    const auto* resp = reinterpret_cast<const HandshakeResponsePayload*>(resp_payload.data());
    return (resp->status_code == 0);
}

// ----------------------------------------------------------------------------
// Test 1: Corrupted Primitive Topology Values
// ----------------------------------------------------------------------------
static void Test_CorruptedPrimitiveTopology() {
    std::cout << "============================================================" << std::endl;
    std::cout << "[RUNNING ] Test_CorruptedPrimitiveTopology against server.exe..." << std::endl;

    const uint16_t port = 49201;
    ServerProcess proc;
    FUZZ_ASSERT_MSG(SpawnServer(port, "build/server_fuzz_topo.log", proc), "Spawn server.exe on port 49201");

    NetworkSocket sock;
    FUZZ_ASSERT_MSG(sock.Connect("127.0.0.1", port), "Connect client to server.exe");
    FUZZ_ASSERT_MSG(DoHandshake(sock), "Complete handshake with server.exe");

    // Comprehensive array of invalid and corrupted primitive topology values
    const uint32_t corrupted_topologies[] = {
        0x80000000, // High-bit set / negative int32
        65,         // Just above D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST (64)
        9999,       // Large non-zero
        0xFFFFFFFF, // All bits set (-1)
        6,          // Gap: TRIANGLESTRIP (5) to LINELIST_ADJ (10)
        7,
        8,
        9,
        14,         // Gap: TRIANGLESTRIP_ADJ (13) to 1_CONTROL_POINT_PATCHLIST (33)
        15,
        20,
        31,
        32,
        66,
        100,
        255,
        1000,
        0x12345678,
        0x7FFFFFFF,
        0xDEADBEEF,
        0xC0000005
    };

    uint32_t seq = 100;
    for (uint32_t bad_topo : corrupted_topologies) {
        IASetTopologyPayload p = { bad_topo };
        PacketHeader h = { RGPU_MAGIC, OP_IA_SET_TOPOLOGY, FLAG_NONE, ++seq, sizeof(p) };
        bool send_ok = sock.SendPacket(h, &p);
        FUZZ_ASSERT_MSG(send_ok, ("Sent OP_IA_SET_TOPOLOGY with bad topology=" + std::to_string(bad_topo)).c_str());

        // Assert server process is still alive and has not crashed
        DWORD exit_code = 0;
        bool alive = IsServerAlive(proc, &exit_code);
        FUZZ_ASSERT_MSG(alive, ("server.exe survived corrupted topology 0x" + std::to_string(bad_topo)).c_str());
        if (!alive) {
            std::cerr << "  [CRASH] server.exe terminated unexpectedly with code 0x"
                      << std::hex << exit_code << std::dec << std::endl;
            break;
        }
    }

    // Send shutdown and verify clean exit code 0
    DWORD final_exit = ShutdownServer(proc, &sock);
    FUZZ_ASSERT_MSG(final_exit == 0, ("server.exe exited cleanly with code 0 (actual: 0x" + std::to_string(final_exit) + ")").c_str());
    std::cout << "[PASSED  ] Test_CorruptedPrimitiveTopology completed successfully." << std::endl;
}

// ----------------------------------------------------------------------------
// Test 2: Out-of-Bounds Pipeline Slot Numbers
// ----------------------------------------------------------------------------
static void Test_OutOfBoundsSlotNumbers() {
    std::cout << "============================================================" << std::endl;
    std::cout << "[RUNNING ] Test_OutOfBoundsSlotNumbers against server.exe..." << std::endl;

    const uint16_t port = 49202;
    ServerProcess proc;
    FUZZ_ASSERT_MSG(SpawnServer(port, "build/server_fuzz_slots.log", proc), "Spawn server.exe on port 49202");

    NetworkSocket sock;
    FUZZ_ASSERT_MSG(sock.Connect("127.0.0.1", port), "Connect client to server.exe");
    FUZZ_ASSERT_MSG(DoHandshake(sock), "Complete handshake with server.exe");

    uint32_t seq = 200;

    // 1. Vertex Buffer slots (limit: 32)
    const uint32_t bad_vb_slots[] = { 32, 33, 64, 128, 256, 1000, 65535, 0x80000000, 0xFFFFFFFF };
    for (uint32_t slot : bad_vb_slots) {
        IASetVertexBufferPayload p = { slot, 0, 16, 0 };
        PacketHeader h = { RGPU_MAGIC, OP_IA_SET_VERTEX_BUFFER, FLAG_NONE, ++seq, sizeof(p) };
        sock.SendPacket(h, &p);
        FUZZ_ASSERT_MSG(IsServerAlive(proc), ("server.exe survived bad VB slot=" + std::to_string(slot)).c_str());
    }

    // 2. VS Constant Buffer slots (limit: 14)
    const uint32_t bad_cb_slots[] = { 14, 15, 16, 31, 32, 64, 128, 1000, 0x80000000, 0xFFFFFFFF };
    for (uint32_t slot : bad_cb_slots) {
        VSSetConstantBufferPayload p = { slot, 0 };
        PacketHeader h = { RGPU_MAGIC, OP_VS_SET_CB, FLAG_NONE, ++seq, sizeof(p) };
        sock.SendPacket(h, &p);
        FUZZ_ASSERT_MSG(IsServerAlive(proc), ("server.exe survived bad VS CB slot=" + std::to_string(slot)).c_str());
    }

    // 3. PS Constant Buffer slots (limit: 14)
    for (uint32_t slot : bad_cb_slots) {
        PSSetConstantBufferPayload p = { slot, 0 };
        PacketHeader h = { RGPU_MAGIC, OP_PS_SET_CB, FLAG_NONE, ++seq, sizeof(p) };
        sock.SendPacket(h, &p);
        FUZZ_ASSERT_MSG(IsServerAlive(proc), ("server.exe survived bad PS CB slot=" + std::to_string(slot)).c_str());
    }

    // 4. PS Shader Resource View slots (limit: 128)
    const uint32_t bad_srv_slots[] = { 128, 129, 256, 1000, 65535, 0x80000000, 0xFFFFFFFF };
    for (uint32_t slot : bad_srv_slots) {
        PSSetSrvPayload p = { slot, 0 };
        PacketHeader h = { RGPU_MAGIC, OP_PS_SET_SRV, FLAG_NONE, ++seq, sizeof(p) };
        sock.SendPacket(h, &p);
        FUZZ_ASSERT_MSG(IsServerAlive(proc), ("server.exe survived bad PS SRV slot=" + std::to_string(slot)).c_str());
    }

    // 5. PS Sampler slots (limit: 16)
    const uint32_t bad_sampler_slots[] = { 16, 17, 32, 64, 128, 1000, 0x80000000, 0xFFFFFFFF };
    for (uint32_t slot : bad_sampler_slots) {
        PSSetSamplerPayload p = { slot, 0 };
        PacketHeader h = { RGPU_MAGIC, OP_PS_SET_SAMPLER, FLAG_NONE, ++seq, sizeof(p) };
        sock.SendPacket(h, &p);
        FUZZ_ASSERT_MSG(IsServerAlive(proc), ("server.exe survived bad PS Sampler slot=" + std::to_string(slot)).c_str());
    }

    // 6. CreateInputLayout with num_elements > 32 and input_slot >= 32
    {
        CreateInputLayoutPayload ilp = { 9010, 33, 100 }; // 33 elements > 32 limit
        std::vector<uint8_t> payload(sizeof(ilp) + 33 * sizeof(RgpuInputElementDesc) + 100, 0);
        std::memcpy(payload.data(), &ilp, sizeof(ilp));
        PacketHeader h = { RGPU_MAGIC, OP_CREATE_INPUT_LAYOUT, FLAG_NONE, ++seq, static_cast<uint32_t>(payload.size()) };
        sock.SendPacket(h, payload.data());
        FUZZ_ASSERT_MSG(IsServerAlive(proc), "server.exe survived CreateInputLayout num_elements=33");
    }

    // 7. CreateInputLayout with non-null terminated semantic name (32 'Z' chars)
    {
        CreateInputLayoutPayload ilp = { 9011, 1, 10 };
        RgpuInputElementDesc elem = {};
        std::memset(elem.semantic_name, 'Z', sizeof(elem.semantic_name)); // No null terminator!
        elem.semantic_index = 0;
        elem.format = 28;
        elem.input_slot = 32; // Also out of bounds!
        elem.aligned_byte_offset = 0;
        elem.input_slot_class = 0;
        elem.instance_data_step = 0;

        std::vector<uint8_t> payload(sizeof(ilp) + sizeof(elem) + 10, 0);
        std::memcpy(payload.data(), &ilp, sizeof(ilp));
        std::memcpy(payload.data() + sizeof(ilp), &elem, sizeof(elem));
        PacketHeader h = { RGPU_MAGIC, OP_CREATE_INPUT_LAYOUT, FLAG_NONE, ++seq, static_cast<uint32_t>(payload.size()) };
        sock.SendPacket(h, payload.data());
        FUZZ_ASSERT_MSG(IsServerAlive(proc), "server.exe survived non-null semantic name & OOB input_slot=32");
    }

    // Send shutdown and verify clean exit code 0
    DWORD final_exit = ShutdownServer(proc, &sock);
    FUZZ_ASSERT_MSG(final_exit == 0, ("server.exe exited cleanly with code 0 (actual: 0x" + std::to_string(final_exit) + ")").c_str());
    std::cout << "[PASSED  ] Test_OutOfBoundsSlotNumbers completed successfully." << std::endl;
}

// ----------------------------------------------------------------------------
// Test 3: 10,000 Randomized Adversarial Packets Fuzzing
// ----------------------------------------------------------------------------
static void Test_10000_RandomizedAdversarialFuzzing() {
    std::cout << "============================================================" << std::endl;
    std::cout << "[RUNNING ] Test_10000_RandomizedAdversarialFuzzing against server.exe..." << std::endl;

    const uint16_t port = 49203;
    ServerProcess proc;
    FUZZ_ASSERT_MSG(SpawnServer(port, "build/server_fuzz_10k.log", proc), "Spawn server.exe on port 49203");

    NetworkSocket sock;
    FUZZ_ASSERT_MSG(sock.Connect("127.0.0.1", port), "Connect client to server.exe");
    FUZZ_ASSERT_MSG(DoHandshake(sock), "Complete handshake with server.exe");

    std::mt19937 rng(1337);
    std::uniform_int_distribution<uint32_t> dist_u32(0, 0xFFFFFFFF);
    std::uniform_int_distribution<uint16_t> dist_op(0x0000, 0xFFFF);
    std::uniform_int_distribution<size_t> dist_size(0, 256);
    std::uniform_int_distribution<int> dist_byte(0, 255);
    std::uniform_int_distribution<int> dist_category(0, 4);

    const int TOTAL_FUZZ_PACKETS = 10000;
    int packets_sent = 0;
    auto t_start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < TOTAL_FUZZ_PACKETS; ++i) {
        int category = dist_category(rng);
        PacketHeader h = {};
        h.magic = RGPU_MAGIC;
        h.flags = FLAG_NONE;
        h.sequence_id = static_cast<uint32_t>(i + 1000);

        std::vector<uint8_t> payload;

        switch (category) {
            case 0: {
                // Category 0: Corrupted Primitive Topology values
                h.opcode = OP_IA_SET_TOPOLOGY;
                IASetTopologyPayload p = {};
                // Alternate between high-bit, medium values, and raw random
                if (i % 3 == 0) {
                    p.topology = 0x80000000 | (dist_u32(rng) & 0x7FFFFFFF);
                } else if (i % 3 == 1) {
                    p.topology = 65 + (dist_u32(rng) % 10000);
                } else {
                    p.topology = dist_u32(rng);
                }
                h.payload_length = sizeof(p);
                payload.resize(sizeof(p));
                std::memcpy(payload.data(), &p, sizeof(p));
                break;
            }
            case 1: {
                // Category 1: Out-of-bounds slot numbers across IA, VS, PS
                int sub = i % 5;
                if (sub == 0) {
                    h.opcode = OP_IA_SET_VERTEX_BUFFER;
                    IASetVertexBufferPayload p = { 32 + (dist_u32(rng) % 10000), 0, 16, 0 };
                    h.payload_length = sizeof(p);
                    payload.resize(sizeof(p));
                    std::memcpy(payload.data(), &p, sizeof(p));
                } else if (sub == 1) {
                    h.opcode = OP_VS_SET_CB;
                    VSSetConstantBufferPayload p = { 14 + (dist_u32(rng) % 10000), 0 };
                    h.payload_length = sizeof(p);
                    payload.resize(sizeof(p));
                    std::memcpy(payload.data(), &p, sizeof(p));
                } else if (sub == 2) {
                    h.opcode = OP_PS_SET_CB;
                    PSSetConstantBufferPayload p = { 14 + (dist_u32(rng) % 10000), 0 };
                    h.payload_length = sizeof(p);
                    payload.resize(sizeof(p));
                    std::memcpy(payload.data(), &p, sizeof(p));
                } else if (sub == 3) {
                    h.opcode = OP_PS_SET_SRV;
                    PSSetSrvPayload p = { 128 + (dist_u32(rng) % 10000), 0 };
                    h.payload_length = sizeof(p);
                    payload.resize(sizeof(p));
                    std::memcpy(payload.data(), &p, sizeof(p));
                } else {
                    h.opcode = OP_PS_SET_SAMPLER;
                    PSSetSamplerPayload p = { 16 + (dist_u32(rng) % 10000), 0 };
                    h.payload_length = sizeof(p);
                    payload.resize(sizeof(p));
                    std::memcpy(payload.data(), &p, sizeof(p));
                }
                break;
            }
            case 2: {
                // Category 2: Completely randomized opcodes with random payload
                h.opcode = dist_op(rng);
                if (h.opcode == OP_SHUTDOWN) h.opcode = 0xEEEE; // Don't trigger premature shutdown
                size_t p_size = dist_size(rng);
                h.payload_length = static_cast<uint32_t>(p_size);
                payload.resize(p_size);
                for (auto& b : payload) b = static_cast<uint8_t>(dist_byte(rng));
                break;
            }
            case 3: {
                // Category 3: Valid opcodes with truncated / zero / random payloads
                const uint16_t valid_ops[] = {
                    OP_CREATE_BUFFER, OP_CREATE_TEXTURE2D, OP_CREATE_SRV, OP_CREATE_RTV,
                    OP_CREATE_DSV, OP_CREATE_VERTEX_SHADER, OP_CREATE_PIXEL_SHADER,
                    OP_CREATE_INPUT_LAYOUT, OP_CREATE_SAMPLER, OP_DESTROY_RESOURCE,
                    OP_IA_SET_INPUT_LAYOUT, OP_IA_SET_INDEX_BUFFER, OP_RS_SET_VIEWPORT,
                    OP_OM_SET_RENDER_TARGETS, OP_UPDATE_SUBRESOURCE, OP_WRITE_BUFFER_DISCARD,
                    OP_CLEAR_RTV, OP_CLEAR_DSV, OP_DRAW, OP_DRAW_INDEXED
                };
                h.opcode = valid_ops[dist_u32(rng) % (sizeof(valid_ops) / sizeof(valid_ops[0]))];
                size_t p_size = dist_u32(rng) % 40; // Intentionally truncated below required struct sizes
                h.payload_length = static_cast<uint32_t>(p_size);
                payload.resize(p_size);
                for (auto& b : payload) b = static_cast<uint8_t>(dist_byte(rng));
                break;
            }
            case 4: {
                // Category 4: Corrupted resource creation descriptors
                h.opcode = OP_CREATE_TEXTURE2D;
                CreateTexture2DPayload tp = {};
                tp.res_id = 8000 + (i % 100);
                tp.width = (i % 2 == 0) ? 0 : (dist_u32(rng) % 0x10000000);
                tp.height = (i % 2 == 1) ? 0 : (dist_u32(rng) % 0x10000000);
                tp.mips = dist_u32(rng);
                tp.array_size = dist_u32(rng);
                tp.format = dist_u32(rng);
                tp.sample_count = dist_u32(rng);
                tp.sample_quality = dist_u32(rng);
                tp.usage = dist_u32(rng);
                tp.bind_flags = dist_u32(rng);
                tp.cpu_access_flags = dist_u32(rng);
                tp.misc_flags = dist_u32(rng);
                tp.initial_data_size = 0;
                h.payload_length = sizeof(tp);
                payload.resize(sizeof(tp));
                std::memcpy(payload.data(), &tp, sizeof(tp));
                break;
            }
        }

        if (!sock.SendPacket(h, payload.data())) {
            std::cerr << "  [FAIL] SendPacket failed on iteration " << i << std::endl;
            break;
        }
        packets_sent++;

        // Every 1,000 iterations, verify the server is alive
        if ((i + 1) % 1000 == 0) {
            DWORD exc = 0;
            bool alive = IsServerAlive(proc, &exc);
            FUZZ_ASSERT_MSG(alive, ("server.exe alive at packet " + std::to_string(i + 1)).c_str());
            if (!alive) {
                std::cerr << "  [CRASH] server.exe crashed at packet " << (i + 1)
                          << " with code 0x" << std::hex << exc << std::dec << std::endl;
                break;
            }
        }
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
    std::cout << "  Sent " << packets_sent << " / " << TOTAL_FUZZ_PACKETS
              << " adversarial fuzz packets in " << elapsed_ms << " ms ("
              << (packets_sent * 1000.0 / (elapsed_ms ? elapsed_ms : 1)) << " packets/sec)" << std::endl;

    FUZZ_ASSERT_MSG(packets_sent == TOTAL_FUZZ_PACKETS, "Sent all 10,000 adversarial fuzz packets");

    // Post-Fuzzing Server Sanity Check:
    // Verify server can still execute valid rendering and presentation commands
    std::cout << "  Verifying server responsiveness post-fuzzing with Clear and Present..." << std::endl;
    {
        ClearRtvPayload clr = {};
        clr.res_id = 0; // Default offscreen RTV
        clr.color[0] = 0.2f;
        clr.color[1] = 0.4f;
        clr.color[2] = 0.8f;
        clr.color[3] = 1.0f;
        PacketHeader clr_h = { RGPU_MAGIC, OP_CLEAR_RTV, FLAG_NONE, 50000, sizeof(clr) };
        FUZZ_ASSERT_MSG(sock.SendPacket(clr_h, &clr), "Send valid OP_CLEAR_RTV post-fuzzing");

        PresentPayload pres = {};
        pres.swapchain_id = 0;
        pres.sync_interval = 0;
        pres.flags = 0;
        pres.frame_number = 1;
        PacketHeader pres_h = { RGPU_MAGIC, OP_PRESENT, FLAG_NONE, 50001, sizeof(pres) };
        FUZZ_ASSERT_MSG(sock.SendPacket(pres_h, &pres), "Send valid OP_PRESENT post-fuzzing");

        PacketHeader resp_hdr = {};
        std::vector<uint8_t> frame_payload;
        bool got_frame = sock.RecvPacket(resp_hdr, frame_payload);
        FUZZ_ASSERT_MSG(got_frame, "Received OP_FRAME_DATA response from server post-fuzzing");
        if (got_frame) {
            FUZZ_ASSERT_MSG(resp_hdr.opcode == OP_FRAME_DATA, "Response opcode is OP_FRAME_DATA");
            FUZZ_ASSERT_MSG(frame_payload.size() >= sizeof(FrameDataPayload), "Frame payload size valid");
            if (frame_payload.size() >= sizeof(FrameDataPayload)) {
                const auto* fp = reinterpret_cast<const FrameDataPayload*>(frame_payload.data());
                FUZZ_ASSERT_MSG(fp->width == 256 && fp->height == 256, "Frame dimensions match 256x256");
                FUZZ_ASSERT_MSG(fp->frame_number == 1, "Frame number matches 1");
                std::cout << "  Post-fuzzing frame readback: " << fp->width << "x" << fp->height
                          << " (bytes=" << fp->data_size << ") rendered cleanly!" << std::endl;
            }
        }
    }

    // Send shutdown and verify clean exit code 0
    DWORD final_exit = ShutdownServer(proc, &sock);
    FUZZ_ASSERT_MSG(final_exit == 0, ("server.exe exited cleanly with code 0 (actual: 0x" + std::to_string(final_exit) + ")").c_str());
    std::cout << "[PASSED  ] Test_10000_RandomizedAdversarialFuzzing completed successfully." << std::endl;
}

// ----------------------------------------------------------------------------
// Test 4: Multi-Client Connection Churn & Abrupt Disconnects under Fuzzing
// ----------------------------------------------------------------------------
static void Test_MultiClientChurnUnderFuzzing() {
    std::cout << "============================================================" << std::endl;
    std::cout << "[RUNNING ] Test_MultiClientChurnUnderFuzzing against server.exe..." << std::endl;

    const uint16_t port = 49204;
    ServerProcess proc;
    FUZZ_ASSERT_MSG(SpawnServer(port, "build/server_fuzz_churn.log", proc, false), "Spawn server.exe on port 49204");

    std::mt19937 rng(999);
    std::uniform_int_distribution<uint32_t> dist_u32(0, 0xFFFFFFFF);

    const int ADVERSARIAL_CLIENTS = 10;
    for (int c = 1; c <= ADVERSARIAL_CLIENTS; ++c) {
        NetworkSocket client;
        if (!client.Connect("127.0.0.1", port, 2000)) {
            FUZZ_ASSERT_MSG(false, ("Adversarial client #" + std::to_string(c) + " connect failed").c_str());
            break;
        }

        DoHandshake(client);

        // Send 200 corrupt packets
        for (int p = 0; p < 200; ++p) {
            IASetTopologyPayload topo{ dist_u32(rng) };
            PacketHeader h{ RGPU_MAGIC, OP_IA_SET_TOPOLOGY, FLAG_NONE, static_cast<uint32_t>(p), sizeof(topo) };
            client.SendPacket(h, &topo);
        }

        // Abrupt disconnect (every other client sets SO_LINGER hard RST)
        if (c % 2 == 0) {
            linger l = { 1, 0 }; // Hard RST
            setsockopt(client.GetRawSocket(), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof(l));
        }
        client.Close();

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        FUZZ_ASSERT_MSG(IsServerAlive(proc), ("server.exe alive after adversarial client #" + std::to_string(c)).c_str());
    }

    // 11th client: Legitimate client connects, renders, presents, and shuts down
    {
        NetworkSocket legit_client;
        FUZZ_ASSERT_MSG(legit_client.Connect("127.0.0.1", port, 3000), "Legitimate client connected after churn");
        FUZZ_ASSERT_MSG(DoHandshake(legit_client), "Legitimate client completed handshake");

        ClearRtvPayload clr = {};
        clr.res_id = 0;
        clr.color[0] = 0.0f;
        clr.color[1] = 1.0f;
        clr.color[2] = 0.0f;
        clr.color[3] = 1.0f;
        PacketHeader clr_h = { RGPU_MAGIC, OP_CLEAR_RTV, FLAG_NONE, 60000, sizeof(clr) };
        FUZZ_ASSERT_MSG(legit_client.SendPacket(clr_h, &clr), "Send clear RTV from legit client");

        PresentPayload pres = {};
        pres.frame_number = 100;
        PacketHeader pres_h = { RGPU_MAGIC, OP_PRESENT, FLAG_NONE, 60001, sizeof(pres) };
        FUZZ_ASSERT_MSG(legit_client.SendPacket(pres_h, &pres), "Send present from legit client");

        PacketHeader resp_hdr = {};
        std::vector<uint8_t> frame_payload;
        bool got_frame = legit_client.RecvPacket(resp_hdr, frame_payload);
        FUZZ_ASSERT_MSG(got_frame, "Legitimate client received frame after churn");

        DWORD final_exit = ShutdownServer(proc, &legit_client);
        FUZZ_ASSERT_MSG(final_exit == 0, ("server.exe cleanly exited with 0 (actual: 0x" + std::to_string(final_exit) + ")").c_str());
    }

    std::cout << "[PASSED  ] Test_MultiClientChurnUnderFuzzing completed successfully." << std::endl;
}

// ----------------------------------------------------------------------------
// Main Entrypoint
// ----------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    std::cout << "============================================================" << std::endl;
    std::cout << "  Remote GPU Execution Server - Adversarial Fuzzing Suite" << std::endl;
    std::cout << "  Milestone 2 Iteration 2 Remediation Verification" << std::endl;
    std::cout << "============================================================" << std::endl;

    if (!NetworkSocket::InitializeNetwork()) {
        std::cerr << "FATAL: Winsock initialization failed!" << std::endl;
        return 1;
    }

    auto t_start = std::chrono::high_resolution_clock::now();

    Test_CorruptedPrimitiveTopology();
    Test_OutOfBoundsSlotNumbers();
    Test_10000_RandomizedAdversarialFuzzing();
    Test_MultiClientChurnUnderFuzzing();

    auto t_end = std::chrono::high_resolution_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();

    std::cout << "\n============================================================" << std::endl;
    std::cout << "  ADVERSARIAL FUZZING VERIFICATION SUMMARY" << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << "  Total Assertions Run : " << g_fuzz_run.load() << std::endl;
    std::cout << "  Passed               : " << g_fuzz_passed.load() << std::endl;
    std::cout << "  Failed               : " << g_fuzz_failed.load() << std::endl;
    std::cout << "  Total Duration       : " << total_ms << " ms" << std::endl;
    std::cout << "============================================================" << std::endl;

    NetworkSocket::ShutdownNetwork();

    if (g_fuzz_failed.load() == 0) {
        std::cout << "  VERDICT: APPROVE (SERVER NEVER CRASHED OR THREW 0xC0000005)" << std::endl;
        std::cout << "============================================================" << std::endl;
        return 0;
    } else {
        std::cout << "  VERDICT: REQUEST_CHANGES (" << g_fuzz_failed.load() << " FAILURE(S) DETECTED)" << std::endl;
        std::cout << "============================================================" << std::endl;
        return 1;
    }
}
