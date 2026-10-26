// ============================================================================
// Remote GPU Offload - Milestone 1 Independent Empirical Stress & Benchmark Suite
// File: tests/test_transport_benchmark.cpp
// Standard: C++17
// Author: Challenger 2 (Empirical Transport Stress & Throughput Specialist)
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

#include "protocol.h"
#include "transport.h"

#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <chrono>
#include <thread>
#include <future>
#include <atomic>
#include <algorithm>
#include <numeric>

// ----------------------------------------------------------------------------
// Lightweight Zero-Dependency Test Assertions & Metrics
// ----------------------------------------------------------------------------
static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define STRESS_ASSERT(cond) do { \
    g_tests_run++; \
    if (cond) { \
        g_tests_passed++; \
    } else { \
        g_tests_failed++; \
        std::cerr << "  [FAIL] Assertion failed: (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
    } \
} while (0)

#define STRESS_ASSERT_MSG(cond, msg) do { \
    g_tests_run++; \
    if (cond) { \
        g_tests_passed++; \
    } else { \
        g_tests_failed++; \
        std::cerr << "  [FAIL] " << msg << ": (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
    } \
} while (0)

#define RUN_BENCHMARK_SUITE(suite_func) do { \
    std::cout << "============================================================" << std::endl; \
    std::cout << "[BENCHMARK-RUN ] " #suite_func "..." << std::endl; \
    int start_fails = g_tests_failed; \
    auto t_start = std::chrono::high_resolution_clock::now(); \
    suite_func(); \
    auto t_end = std::chrono::high_resolution_clock::now(); \
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count(); \
    if (g_tests_failed == start_fails) { \
        std::cout << "[BENCHMARK-PASS] " #suite_func " (" << elapsed_ms << " ms)" << std::endl; \
    } else { \
        std::cout << "[BENCHMARK-FAIL] " #suite_func " (" << elapsed_ms << " ms) - " \
                  << (g_tests_failed - start_fails) << " assertion failure(s)!" << std::endl; \
    } \
} while (0)

// Fast 64-bit pseudo-random number generator for deterministic data validation
static inline uint64_t XorShift64(uint64_t& state) {
    uint64_t x = state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return state = x;
}

// ----------------------------------------------------------------------------
// Helper: Start loopback server and return client and server socket pair
// ----------------------------------------------------------------------------
struct SocketPair {
    RemoteGPU::NetworkSocket client;
    std::unique_ptr<RemoteGPU::NetworkSocket> server;
};

static SocketPair EstablishLoopbackPair() {
    SocketPair pair;
    std::promise<uint16_t> port_promise;
    auto port_future = port_promise.get_future();

    std::thread srv_th([&]() {
        RemoteGPU::NetworkSocket listener;
        if (!listener.BindAndListen(0, "127.0.0.1")) {
            port_promise.set_value(0);
            return;
        }
        port_promise.set_value(listener.GetLocalPort());
        pair.server = listener.Accept(5000);
    });

    uint16_t port = port_future.get();
    if (port == 0) {
        if (srv_th.joinable()) srv_th.join();
        return pair;
    }

    bool conn_ok = pair.client.Connect("127.0.0.1", port, 5000);
    if (srv_th.joinable()) srv_th.join();

    if (!conn_ok || !pair.server) {
        pair.client.Close();
        if (pair.server) pair.server->Close();
    }
    return pair;
}

// ============================================================================
// Suite 1: Boundary Conditions - Zero-Length Payload Transfers
// ============================================================================
void Benchmark_ZeroLengthPayload_Transfers() {
    std::cout << "--- [1.1] Zero-length payload: null pointer ---" << std::endl;
    {
        auto pair = EstablishLoopbackPair();
        STRESS_ASSERT(pair.client.IsValid());
        STRESS_ASSERT(pair.server && pair.server->IsValid());

        RemoteGPU::PacketHeader hdr{};
        hdr.magic = RemoteGPU::RGPU_MAGIC;
        hdr.opcode = RemoteGPU::OP_PRESENT;
        hdr.flags = RemoteGPU::FLAG_END_OF_FRAME;
        hdr.sequence_id = 101;
        hdr.payload_length = 0;

        // Send with payload == nullptr
        bool send_ok = pair.client.SendPacket(hdr, nullptr);
        STRESS_ASSERT_MSG(send_ok, "SendPacket with payload_length=0 and nullptr succeeds");

        // Receive into std::vector<uint8_t>
        RemoteGPU::PacketHeader rcv_hdr{};
        std::vector<uint8_t> rcv_payload;
        rcv_payload.resize(64); // pre-populate to ensure RecvPacket clears it
        bool recv_ok = pair.server->RecvPacket(rcv_hdr, rcv_payload);

        STRESS_ASSERT_MSG(recv_ok, "RecvPacket succeeds for zero-length payload");
        STRESS_ASSERT(rcv_hdr.magic == RemoteGPU::RGPU_MAGIC);
        STRESS_ASSERT(rcv_hdr.opcode == RemoteGPU::OP_PRESENT);
        STRESS_ASSERT(rcv_hdr.flags == RemoteGPU::FLAG_END_OF_FRAME);
        STRESS_ASSERT(rcv_hdr.sequence_id == 101);
        STRESS_ASSERT(rcv_hdr.payload_length == 0);
        STRESS_ASSERT_MSG(rcv_payload.empty(), "RecvPacket cleared vector for zero-length payload");
    }

    std::cout << "--- [1.2] Zero-length payload: non-null dummy pointer ---" << std::endl;
    {
        auto pair = EstablishLoopbackPair();
        STRESS_ASSERT(pair.client.IsValid() && pair.server && pair.server->IsValid());

        RemoteGPU::PacketHeader hdr{};
        hdr.magic = RemoteGPU::RGPU_MAGIC;
        hdr.opcode = RemoteGPU::OP_SHUTDOWN;
        hdr.flags = RemoteGPU::FLAG_NONE;
        hdr.sequence_id = 102;
        hdr.payload_length = 0;

        uint8_t dummy_buf[16] = { 0xFF };
        bool send_ok = pair.client.SendPacket(hdr, dummy_buf);
        STRESS_ASSERT_MSG(send_ok, "SendPacket with payload_length=0 and non-null dummy ptr succeeds");

        // Receive into raw buffer overload with max_payload_len = 0 and out_payload = nullptr
        RemoteGPU::PacketHeader rcv_hdr{};
        bool recv_ok = pair.server->RecvPacket(rcv_hdr, nullptr, 0);
        STRESS_ASSERT_MSG(recv_ok, "RecvPacket raw buffer overload succeeds with nullptr and max_len=0");
        STRESS_ASSERT(rcv_hdr.opcode == RemoteGPU::OP_SHUTDOWN);
        STRESS_ASSERT(rcv_hdr.sequence_id == 102);
        STRESS_ASSERT(rcv_hdr.payload_length == 0);
    }

    std::cout << "--- [1.3] Zero-length SendExact and RecvExact ---" << std::endl;
    {
        auto pair = EstablishLoopbackPair();
        STRESS_ASSERT(pair.client.IsValid() && pair.server && pair.server->IsValid());

        // Zero length send and recv should return true immediately without performing I/O
        STRESS_ASSERT(pair.client.SendExact(nullptr, 0));
        STRESS_ASSERT(pair.server->RecvExact(nullptr, 0));
        STRESS_ASSERT(pair.client.SendExact("dummy", 0));
        char dummy_buf[8] = {0};
        STRESS_ASSERT(pair.server->RecvExact(dummy_buf, 0));
    }

    std::cout << "--- [1.4] High-frequency burst: 20,000 zero-length packets ---" << std::endl;
    {
        auto pair = EstablishLoopbackPair();
        STRESS_ASSERT(pair.client.IsValid() && pair.server && pair.server->IsValid());

        const int kPacketCount = 20000;
        std::atomic<bool> thread_error{false};

        auto t_start = std::chrono::high_resolution_clock::now();

        // Server receiver thread
        std::thread recv_thread([&]() {
            RemoteGPU::PacketHeader rcv_hdr{};
            std::vector<uint8_t> payload;
            for (int i = 0; i < kPacketCount; ++i) {
                if (!pair.server->RecvPacket(rcv_hdr, payload)) {
                    thread_error = true;
                    break;
                }
                if (rcv_hdr.sequence_id != static_cast<uint32_t>(i + 1) || rcv_hdr.payload_length != 0) {
                    thread_error = true;
                    break;
                }
            }
        });

        // Client sender thread
        for (int i = 0; i < kPacketCount; ++i) {
            RemoteGPU::PacketHeader hdr{};
            hdr.magic = RemoteGPU::RGPU_MAGIC;
            hdr.opcode = RemoteGPU::OP_PRESENT;
            hdr.flags = RemoteGPU::FLAG_NONE;
            hdr.sequence_id = static_cast<uint32_t>(i + 1);
            hdr.payload_length = 0;

            if (!pair.client.SendPacket(hdr, nullptr)) {
                thread_error = true;
                break;
            }
        }

        recv_thread.join();
        auto t_end = std::chrono::high_resolution_clock::now();
        auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count();
        double duration_sec = static_cast<double>(duration_us) / 1000000.0;
        double packets_per_sec = static_cast<double>(kPacketCount) / (duration_sec > 0.0 ? duration_sec : 0.0001);

        std::cout << "  [PERF] 20,000 zero-length packets transferred in "
                  << (duration_us / 1000) << " ms (~"
                  << static_cast<int>(packets_per_sec) << " packets/sec)" << std::endl;

        STRESS_ASSERT_MSG(!thread_error, "20,000 zero-length packets transferred with 100% sequence accuracy");
        STRESS_ASSERT_MSG(packets_per_sec > 10000.0, "Packet rate exceeds 10,000 packets/sec on loopback");
    }

    std::cout << "--- [1.5] Alternating zero-length and small payload packets (1,000 iterations) ---" << std::endl;
    {
        auto pair = EstablishLoopbackPair();
        STRESS_ASSERT(pair.client.IsValid() && pair.server && pair.server->IsValid());

        const int kIterations = 1000;
        std::atomic<bool> interleaved_error{false};

        std::thread recv_thread([&]() {
            RemoteGPU::PacketHeader rcv_hdr{};
            std::vector<uint8_t> payload;
            for (int i = 0; i < kIterations; ++i) {
                // Odd sequence: zero-length packet
                if (!pair.server->RecvPacket(rcv_hdr, payload)) {
                    interleaved_error = true;
                    return;
                }
                if (rcv_hdr.sequence_id != static_cast<uint32_t>(i * 2 + 1) || rcv_hdr.payload_length != 0) {
                    interleaved_error = true;
                    return;
                }

                // Even sequence: 16-byte payload packet
                if (!pair.server->RecvPacket(rcv_hdr, payload)) {
                    interleaved_error = true;
                    return;
                }
                if (rcv_hdr.sequence_id != static_cast<uint32_t>(i * 2 + 2) || rcv_hdr.payload_length != 16) {
                    interleaved_error = true;
                    return;
                }
                uint32_t expected_val = static_cast<uint32_t>(i * 2 + 2);
                uint32_t actual_val = 0;
                std::memcpy(&actual_val, payload.data(), sizeof(actual_val));
                if (actual_val != expected_val) {
                    interleaved_error = true;
                    return;
                }
            }
        });

        for (int i = 0; i < kIterations; ++i) {
            // Odd sequence: zero-length packet
            RemoteGPU::PacketHeader hdr0{};
            hdr0.magic = RemoteGPU::RGPU_MAGIC;
            hdr0.opcode = RemoteGPU::OP_PRESENT;
            hdr0.flags = RemoteGPU::FLAG_NONE;
            hdr0.sequence_id = static_cast<uint32_t>(i * 2 + 1);
            hdr0.payload_length = 0;
            if (!pair.client.SendPacket(hdr0, nullptr)) {
                interleaved_error = true;
                break;
            }

            // Even sequence: 16-byte payload packet
            RemoteGPU::PacketHeader hdr1{};
            hdr1.magic = RemoteGPU::RGPU_MAGIC;
            hdr1.opcode = RemoteGPU::OP_CLEAR_RTV;
            hdr1.flags = RemoteGPU::FLAG_NONE;
            hdr1.sequence_id = static_cast<uint32_t>(i * 2 + 2);
            hdr1.payload_length = 16;
            uint8_t payload_bytes[16]{};
            uint32_t val = static_cast<uint32_t>(i * 2 + 2);
            std::memcpy(payload_bytes, &val, sizeof(val));
            if (!pair.client.SendPacket(hdr1, payload_bytes)) {
                interleaved_error = true;
                break;
            }
        }

        recv_thread.join();
        STRESS_ASSERT_MSG(!interleaved_error, "Alternating zero/payload packets maintained strict byte alignment");
    }
}

// ============================================================================
// Suite 2: Extreme Payload Scaling (16 MB Uncompressed Framebuffers & Beyond)
// ============================================================================
void Benchmark_LargePayload_Transfers() {
    std::cout << "--- [2.1] Single 16 MB Uncompressed Framebuffer Transfer ---" << std::endl;
    {
        auto pair = EstablishLoopbackPair();
        STRESS_ASSERT(pair.client.IsValid() && pair.server && pair.server->IsValid());

        const size_t k16MB = 16 * 1024 * 1024; // 16,777,216 bytes
        std::vector<uint8_t> send_buf(k16MB);

        // Fill buffer with deterministic pseudorandom data
        uint64_t prng_state = 0xFEEDFACECAFEBABEULL;
        uint64_t expected_hash = 0;
        uint64_t* ptr64 = reinterpret_cast<uint64_t*>(send_buf.data());
        size_t u64_count = k16MB / sizeof(uint64_t);
        for (size_t i = 0; i < u64_count; ++i) {
            uint64_t val = XorShift64(prng_state);
            ptr64[i] = val;
            expected_hash ^= val;
        }

        RemoteGPU::PacketHeader hdr{};
        hdr.magic = RemoteGPU::RGPU_MAGIC;
        hdr.opcode = RemoteGPU::OP_FRAME_DATA;
        hdr.flags = RemoteGPU::FLAG_NONE;
        hdr.sequence_id = 5001;
        hdr.payload_length = static_cast<uint32_t>(k16MB);

        auto t0 = std::chrono::high_resolution_clock::now();

        // Asynchronous send and receive
        std::vector<uint8_t> recv_buf;
        RemoteGPU::PacketHeader rcv_hdr{};
        std::atomic<bool> recv_ok{false};

        std::thread recv_thread([&]() {
            recv_ok = pair.server->RecvPacket(rcv_hdr, recv_buf);
        });

        bool send_ok = pair.client.SendPacket(hdr, send_buf.data());
        STRESS_ASSERT_MSG(send_ok, "SendPacket for 16 MB buffer succeeded");

        recv_thread.join();
        auto t1 = std::chrono::high_resolution_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        STRESS_ASSERT_MSG(recv_ok.load(), "RecvPacket for 16 MB buffer succeeded");
        STRESS_ASSERT(rcv_hdr.sequence_id == 5001);
        STRESS_ASSERT(rcv_hdr.payload_length == k16MB);
        STRESS_ASSERT(recv_buf.size() == k16MB);

        // Verify hash integrity
        uint64_t actual_hash = 0;
        const uint64_t* rcv_ptr64 = reinterpret_cast<const uint64_t*>(recv_buf.data());
        for (size_t i = 0; i < u64_count; ++i) {
            actual_hash ^= rcv_ptr64[i];
        }

        STRESS_ASSERT_MSG(actual_hash == expected_hash, "16 MB payload 64-bit checksum matches bit-for-bit");
        int memcmp_res = std::memcmp(send_buf.data(), recv_buf.data(), k16MB);
        STRESS_ASSERT_MSG(memcmp_res == 0, "16 MB memcmp matches 100%");

        double speed_mb_s = (16.0) / ((elapsed_ms > 0 ? elapsed_ms : 1) / 1000.0);
        std::cout << "  [PERF] 16 MB framebuffer transfer completed in " << elapsed_ms
                  << " ms (~" << static_cast<int>(speed_mb_s) << " MB/s)" << std::endl;
    }

    std::cout << "--- [2.2] Streaming 30 Consecutive 16 MB Framebuffers (480 MB Total) ---" << std::endl;
    {
        auto pair = EstablishLoopbackPair();
        STRESS_ASSERT(pair.client.IsValid() && pair.server && pair.server->IsValid());

        const int kFrameCount = 30;
        const size_t k16MB = 16 * 1024 * 1024;
        std::vector<uint8_t> frame_buf(k16MB);
        for (size_t i = 0; i < k16MB; i += 4096) {
            frame_buf[i] = static_cast<uint8_t>(i & 0xFF);
        }

        std::atomic<bool> stream_error{false};
        std::atomic<size_t> frames_received{0};

        auto t0 = std::chrono::high_resolution_clock::now();

        std::thread srv_thread([&]() {
            RemoteGPU::PacketHeader rcv_hdr{};
            std::vector<uint8_t> rcv_payload;
            for (int f = 0; f < kFrameCount; ++f) {
                if (!pair.server->RecvPacket(rcv_hdr, rcv_payload)) {
                    stream_error = true;
                    return;
                }
                if (rcv_hdr.sequence_id != static_cast<uint32_t>(f + 1) ||
                    rcv_hdr.payload_length != k16MB ||
                    rcv_payload.size() != k16MB) {
                    stream_error = true;
                    return;
                }
                // Quick sample check
                if (rcv_payload[4096] != static_cast<uint8_t>(4096 & 0xFF)) {
                    stream_error = true;
                    return;
                }
                frames_received++;
            }
        });

        for (int f = 0; f < kFrameCount; ++f) {
            RemoteGPU::PacketHeader hdr{};
            hdr.magic = RemoteGPU::RGPU_MAGIC;
            hdr.opcode = RemoteGPU::OP_FRAME_DATA;
            hdr.flags = (f == kFrameCount - 1) ? RemoteGPU::FLAG_END_OF_FRAME : RemoteGPU::FLAG_NONE;
            hdr.sequence_id = static_cast<uint32_t>(f + 1);
            hdr.payload_length = static_cast<uint32_t>(k16MB);

            if (!pair.client.SendPacket(hdr, frame_buf.data())) {
                stream_error = true;
                break;
            }
        }

        srv_thread.join();
        auto t1 = std::chrono::high_resolution_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        double total_mb = static_cast<double>(kFrameCount * 16);
        double total_sec = (elapsed_ms > 0 ? elapsed_ms : 1) / 1000.0;
        double throughput_mb_s = total_mb / total_sec;
        double fps = static_cast<double>(kFrameCount) / total_sec;

        std::cout << "  [PERF] 30 x 16 MB frames (480 MB) transferred in " << elapsed_ms
                  << " ms (~" << static_cast<int>(throughput_mb_s) << " MB/s, "
                  << std::fixed << std::setprecision(1) << fps << " FPS)" << std::endl;

        STRESS_ASSERT_MSG(!stream_error, "All 30 frames streamed without error or frame drop");
        STRESS_ASSERT(frames_received == kFrameCount);
        STRESS_ASSERT_MSG(throughput_mb_s >= 250.0, "Throughput exceeds 250 MB/s baseline requirement");
    }

    std::cout << "--- [2.3] Extreme Payloads: 32 MB and 64 MB Single Packets ---" << std::endl;
    {
        auto pair = EstablishLoopbackPair();
        STRESS_ASSERT(pair.client.IsValid() && pair.server && pair.server->IsValid());

        // Test 32 MB
        const size_t k32MB = 32 * 1024 * 1024;
        std::vector<uint8_t> buf32(k32MB, 0x5A);

        RemoteGPU::PacketHeader hdr32{};
        hdr32.magic = RemoteGPU::RGPU_MAGIC;
        hdr32.opcode = RemoteGPU::OP_FRAME_DATA;
        hdr32.sequence_id = 9001;
        hdr32.payload_length = static_cast<uint32_t>(k32MB);

        std::thread recv32_thread([&]() {
            RemoteGPU::PacketHeader rcv_hdr{};
            std::vector<uint8_t> rcv_payload;
            bool ok = pair.server->RecvPacket(rcv_hdr, rcv_payload);
            STRESS_ASSERT_MSG(ok, "32 MB RecvPacket succeeded");
            STRESS_ASSERT(rcv_payload.size() == k32MB);
            STRESS_ASSERT(rcv_payload[0] == 0x5A && rcv_payload[k32MB - 1] == 0x5A);
        });

        bool send32_ok = pair.client.SendPacket(hdr32, buf32.data());
        STRESS_ASSERT_MSG(send32_ok, "32 MB SendPacket succeeded");
        recv32_thread.join();

        // Test 64 MB (Exact SendExact/RecvExact chunk threshold)
        const size_t k64MB = 64 * 1024 * 1024;
        std::vector<uint8_t> buf64(k64MB, 0xA5);

        RemoteGPU::PacketHeader hdr64{};
        hdr64.magic = RemoteGPU::RGPU_MAGIC;
        hdr64.opcode = RemoteGPU::OP_FRAME_DATA;
        hdr64.sequence_id = 9002;
        hdr64.payload_length = static_cast<uint32_t>(k64MB);

        std::thread recv64_thread([&]() {
            RemoteGPU::PacketHeader rcv_hdr{};
            std::vector<uint8_t> rcv_payload;
            bool ok = pair.server->RecvPacket(rcv_hdr, rcv_payload);
            STRESS_ASSERT_MSG(ok, "64 MB RecvPacket succeeded");
            STRESS_ASSERT(rcv_payload.size() == k64MB);
            STRESS_ASSERT(rcv_payload[0] == 0xA5 && rcv_payload[k64MB - 1] == 0xA5);
        });

        bool send64_ok = pair.client.SendPacket(hdr64, buf64.data());
        STRESS_ASSERT_MSG(send64_ok, "64 MB SendPacket succeeded");
        recv64_thread.join();
    }

    std::cout << "--- [2.4] Safety Rejection: Payload Exceeding 256 MB Limit ---" << std::endl;
    {
        auto pair = EstablishLoopbackPair();
        STRESS_ASSERT(pair.client.IsValid() && pair.server && pair.server->IsValid());

        RemoteGPU::PacketHeader bad_hdr{};
        bad_hdr.magic = RemoteGPU::RGPU_MAGIC;
        bad_hdr.opcode = RemoteGPU::OP_FRAME_DATA;
        bad_hdr.sequence_id = 9999;
        bad_hdr.payload_length = 256 * 1024 * 1024 + 1; // 256 MB + 1 byte

        // Send bad header directly
        send(pair.client.GetSocket(), reinterpret_cast<const char*>(&bad_hdr), sizeof(bad_hdr), 0);

        RemoteGPU::PacketHeader rcv_hdr{};
        std::vector<uint8_t> rcv_payload;
        bool ok = pair.server->RecvPacket(rcv_hdr, rcv_payload);
        STRESS_ASSERT_MSG(!ok, "RecvPacket safely rejected packet claiming > 256 MB payload");
    }

    std::cout << "--- [2.5] Safety Protection: Fixed-Size Buffer Capacity Overflow ---" << std::endl;
    {
        auto pair = EstablishLoopbackPair();
        STRESS_ASSERT(pair.client.IsValid() && pair.server && pair.server->IsValid());

        const size_t kPayloadSize = 65536; // 64 KB
        std::vector<uint8_t> payload(kPayloadSize, 0x42);

        RemoteGPU::PacketHeader hdr{};
        hdr.magic = RemoteGPU::RGPU_MAGIC;
        hdr.opcode = RemoteGPU::OP_UPDATE_SUBRESOURCE;
        hdr.sequence_id = 9998;
        hdr.payload_length = static_cast<uint32_t>(kPayloadSize);

        pair.client.SendPacket(hdr, payload.data());

        // Receiver only allocates 1024 bytes buffer
        uint8_t small_buf[1024]{};
        RemoteGPU::PacketHeader rcv_hdr{};
        bool ok = pair.server->RecvPacket(rcv_hdr, small_buf, sizeof(small_buf));
        STRESS_ASSERT_MSG(!ok, "RecvPacket raw buffer overload rejected oversized payload without overrun");
    }
}

// ============================================================================
// Suite 3: Loopback Transfer Speed Benchmarks & Socket Options Verification
// ============================================================================
void Benchmark_Loopback_Optimizations() {
    std::cout << "--- [3.1] Socket Options & Fast Path Verification ---" << std::endl;
    {
        auto pair = EstablishLoopbackPair();
        STRESS_ASSERT(pair.client.IsValid() && pair.server && pair.server->IsValid());

        // 1. Verify TCP_NODELAY on both client and accepted server sockets
        int client_nodelay = 0, server_nodelay = 0;
        int optlen = sizeof(int);
        getsockopt(pair.client.GetSocket(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&client_nodelay), &optlen);
        getsockopt(pair.server->GetSocket(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&server_nodelay), &optlen);
        STRESS_ASSERT_MSG(client_nodelay == 1, "TCP_NODELAY is active (1) on client socket");
        STRESS_ASSERT_MSG(server_nodelay == 1, "TCP_NODELAY is active (1) on server socket");

        // 2. Verify SO_SNDBUF and SO_RCVBUF
        int client_snd = 0, client_rcv = 0, server_snd = 0, server_rcv = 0;
        optlen = sizeof(int);
        getsockopt(pair.client.GetSocket(), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&client_snd), &optlen);
        getsockopt(pair.client.GetSocket(), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&client_rcv), &optlen);
        getsockopt(pair.server->GetSocket(), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&server_snd), &optlen);
        getsockopt(pair.server->GetSocket(), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&server_rcv), &optlen);

        std::cout << "  [INFO] Socket buffer sizes: Client SND=" << (client_snd / 1024)
                  << " KB, RCV=" << (client_rcv / 1024)
                  << " KB | Server SND=" << (server_snd / 1024)
                  << " KB, RCV=" << (server_rcv / 1024) << " KB" << std::endl;

        STRESS_ASSERT_MSG(client_snd >= 1048576, "Client send buffer >= 1 MB");
        STRESS_ASSERT_MSG(client_rcv >= 1048576, "Client recv buffer >= 1 MB");
        STRESS_ASSERT_MSG(server_snd >= 1048576, "Server send buffer >= 1 MB");
        STRESS_ASSERT_MSG(server_rcv >= 1048576, "Server recv buffer >= 1 MB");

        // 3. Verify SIO_LOOPBACK_FAST_PATH ioctl behavior on fresh socket before connect
        SOCKET pre_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        DWORD fast_path = 1;
        DWORD bytes_ret = 0;
        int ioctl_res = WSAIoctl(pre_sock, SIO_LOOPBACK_FAST_PATH, &fast_path, sizeof(fast_path),
                                 nullptr, 0, &bytes_ret, nullptr, nullptr);
        STRESS_ASSERT_MSG(ioctl_res == 0, "SIO_LOOPBACK_FAST_PATH ioctl succeeds on pre-connect socket");
        closesocket(pre_sock);

        // 4. Verify Send/Recv Timeouts
        int client_snd_to = 0, client_rcv_to = 0;
        optlen = sizeof(int);
        getsockopt(pair.client.GetSocket(), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&client_snd_to), &optlen);
        getsockopt(pair.client.GetSocket(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&client_rcv_to), &optlen);
        STRESS_ASSERT_MSG(client_snd_to > 0, "Send timeout configured (> 0 ms)");
        STRESS_ASSERT_MSG(client_rcv_to > 0, "Recv timeout configured (> 0 ms)");
    }

    std::cout << "--- [3.2] Sustained Loopback Throughput Benchmark (1000 MB Total) ---" << std::endl;
    {
        auto pair = EstablishLoopbackPair();
        STRESS_ASSERT(pair.client.IsValid() && pair.server && pair.server->IsValid());

        const size_t kChunkSize = 16 * 1024 * 1024; // 16 MB chunks
        const int kChunkCount = 64; // 64 * 16 MB = 1024 MB (1.0 GB)
        std::vector<uint8_t> chunk_buf(kChunkSize, 0x7E);

        std::atomic<bool> throughput_error{false};
        std::atomic<size_t> bytes_transferred{0};

        auto t0 = std::chrono::high_resolution_clock::now();

        std::thread srv_thread([&]() {
            RemoteGPU::PacketHeader rcv_hdr{};
            std::vector<uint8_t> rcv_buf;
            for (int i = 0; i < kChunkCount; ++i) {
                if (!pair.server->RecvPacket(rcv_hdr, rcv_buf)) {
                    throughput_error = true;
                    return;
                }
                bytes_transferred += rcv_buf.size();
            }
        });

        for (int i = 0; i < kChunkCount; ++i) {
            RemoteGPU::PacketHeader hdr{};
            hdr.magic = RemoteGPU::RGPU_MAGIC;
            hdr.opcode = RemoteGPU::OP_FRAME_DATA;
            hdr.sequence_id = static_cast<uint32_t>(i + 1);
            hdr.payload_length = static_cast<uint32_t>(kChunkSize);

            if (!pair.client.SendPacket(hdr, chunk_buf.data())) {
                throughput_error = true;
                break;
            }
        }

        srv_thread.join();
        auto t1 = std::chrono::high_resolution_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        double gigabytes = static_cast<double>(bytes_transferred.load()) / (1024.0 * 1024.0 * 1024.0);
        double seconds = static_cast<double>(elapsed_ms > 0 ? elapsed_ms : 1) / 1000.0;
        double throughput_mb_s = (gigabytes * 1024.0) / seconds;
        double throughput_gb_s = gigabytes / seconds;

        std::cout << "  [PERF] 1024 MB Sustained Transfer: "
                  << gigabytes << " GB in " << elapsed_ms << " ms (~"
                  << static_cast<int>(throughput_mb_s) << " MB/s | "
                  << std::fixed << std::setprecision(2) << throughput_gb_s << " GB/s)" << std::endl;

        STRESS_ASSERT_MSG(!throughput_error, "1024 MB sustained transfer completed without error");
        STRESS_ASSERT(bytes_transferred == static_cast<size_t>(kChunkCount) * kChunkSize);
        STRESS_ASSERT_MSG(throughput_mb_s >= 300.0, "Sustained throughput meets >= 300 MB/s high-perf bar");
    }

    std::cout << "--- [3.3] Command Latency / RTT Round-Trip Benchmark (2,000 Ping-Pongs) ---" << std::endl;
    {
        auto pair = EstablishLoopbackPair();
        STRESS_ASSERT(pair.client.IsValid() && pair.server && pair.server->IsValid());

        const int kRttCount = 2000;
        std::atomic<bool> rtt_error{false};

        // Server echo thread
        std::thread echo_thread([&]() {
            RemoteGPU::PacketHeader rcv_hdr{};
            std::vector<uint8_t> payload;
            for (int i = 0; i < kRttCount; ++i) {
                if (!pair.server->RecvPacket(rcv_hdr, payload)) {
                    rtt_error = true;
                    return;
                }
                rcv_hdr.flags |= RemoteGPU::FLAG_RESPONSE_REQUIRED;
                if (!pair.server->SendPacket(rcv_hdr, payload.data())) {
                    rtt_error = true;
                    return;
                }
            }
        });

        std::vector<double> rtt_microseconds;
        rtt_microseconds.reserve(kRttCount);

        for (int i = 0; i < kRttCount; ++i) {
            RemoteGPU::PacketHeader hdr{};
            hdr.magic = RemoteGPU::RGPU_MAGIC;
            hdr.opcode = RemoteGPU::OP_HANDSHAKE_REQUEST;
            hdr.sequence_id = static_cast<uint32_t>(i + 1);
            hdr.payload_length = 32;
            uint8_t dummy[32]{};

            auto t_send = std::chrono::high_resolution_clock::now();
            if (!pair.client.SendPacket(hdr, dummy)) {
                rtt_error = true;
                break;
            }

            RemoteGPU::PacketHeader echo_hdr{};
            std::vector<uint8_t> echo_payload;
            if (!pair.client.RecvPacket(echo_hdr, echo_payload)) {
                rtt_error = true;
                break;
            }
            auto t_recv = std::chrono::high_resolution_clock::now();

            auto us = std::chrono::duration_cast<std::chrono::microseconds>(t_recv - t_send).count();
            rtt_microseconds.push_back(static_cast<double>(us));
        }

        echo_thread.join();
        STRESS_ASSERT_MSG(!rtt_error, "2,000 round-trip exchanges completed without failure");

        if (!rtt_microseconds.empty()) {
            std::sort(rtt_microseconds.begin(), rtt_microseconds.end());
            double min_rtt = rtt_microseconds.front();
            double max_rtt = rtt_microseconds.back();
            double median_rtt = rtt_microseconds[rtt_microseconds.size() / 2];
            double p99_rtt = rtt_microseconds[static_cast<size_t>(rtt_microseconds.size() * 0.99)];
            double avg_rtt = std::accumulate(rtt_microseconds.begin(), rtt_microseconds.end(), 0.0) / rtt_microseconds.size();

            std::cout << "  [PERF] RTT Stats (2000 msgs): Min=" << min_rtt << " us, Median=" << median_rtt
                      << " us, Avg=" << avg_rtt << " us, p99=" << p99_rtt << " us, Max=" << max_rtt << " us" << std::endl;

            // Confirm Nagle delay (40ms-200ms) did NOT occur
            STRESS_ASSERT_MSG(median_rtt < 1000.0, "Median RTT < 1 ms (Nagle algorithm disabled)");
            STRESS_ASSERT_MSG(p99_rtt < 5000.0, "99th percentile RTT < 5 ms");
        }
    }
}

// ============================================================================
// Suite 4: Abrupt Socket Termination During Half-Sent Packets & Error Handling
// ============================================================================
void Benchmark_AbruptDisconnection_Resilience() {
    std::cout << "--- [4.1] Abrupt Termination: Sender closes during half-sent header ---" << std::endl;
    {
        auto pair = EstablishLoopbackPair();
        STRESS_ASSERT(pair.client.IsValid() && pair.server && pair.server->IsValid());

        std::thread sender_thread([&]() {
            // Send only 7 bytes of the 16-byte header
            uint8_t partial[7] = { 'R', 'G', 'P', 'U', 0x01, 0x00, 0x00 };
            send(pair.client.GetSocket(), reinterpret_cast<const char*>(partial), sizeof(partial), 0);
            // Immediately close client socket
            pair.client.Close();
        });

        // Server in RecvPacket must detect EOF and return false cleanly
        RemoteGPU::PacketHeader rcv_hdr{};
        std::vector<uint8_t> rcv_payload;
        bool ok = pair.server->RecvPacket(rcv_hdr, rcv_payload);
        STRESS_ASSERT_MSG(!ok, "RecvPacket returned false upon truncated header");

        sender_thread.join();
    }

    std::cout << "--- [4.2] Abrupt Termination: Sender closes mid-way through 16 MB payload (Graceful FIN) ---" << std::endl;
    {
        auto pair = EstablishLoopbackPair();
        STRESS_ASSERT(pair.client.IsValid() && pair.server && pair.server->IsValid());

        const size_t k16MB = 16 * 1024 * 1024;
        std::atomic<bool> receiver_completed{false};
        std::atomic<bool> receiver_result{true};

        std::thread receiver_thread([&]() {
            RemoteGPU::PacketHeader rcv_hdr{};
            std::vector<uint8_t> rcv_payload;
            receiver_result = pair.server->RecvPacket(rcv_hdr, rcv_payload);
            receiver_completed = true;
        });

        // Sender sends full 16-byte header advertising 16 MB payload
        RemoteGPU::PacketHeader hdr{};
        hdr.magic = RemoteGPU::RGPU_MAGIC;
        hdr.opcode = RemoteGPU::OP_FRAME_DATA;
        hdr.sequence_id = 7001;
        hdr.payload_length = static_cast<uint32_t>(k16MB);
        send(pair.client.GetSocket(), reinterpret_cast<const char*>(&hdr), sizeof(hdr), 0);

        // Sender sends only 32 KB of the advertised 16 MB payload
        std::vector<uint8_t> partial_payload(32768, 0x55);
        send(pair.client.GetSocket(), reinterpret_cast<const char*>(partial_payload.data()), 32768, 0);

        // Abruptly close sender socket
        pair.client.Close();

        receiver_thread.join();
        STRESS_ASSERT_MSG(receiver_completed.load(), "Receiver thread terminated promptly");
        STRESS_ASSERT_MSG(!receiver_result.load(), "RecvPacket safely failed when sender closed mid-payload");
    }

    std::cout << "--- [4.3] Abrupt Termination: Sender hard abort (SO_LINGER RST) mid-16MB payload ---" << std::endl;
    {
        auto pair = EstablishLoopbackPair();
        STRESS_ASSERT(pair.client.IsValid() && pair.server && pair.server->IsValid());

        const size_t k16MB = 16 * 1024 * 1024;
        std::atomic<bool> receiver_completed{false};
        std::atomic<bool> receiver_result{true};

        std::thread receiver_thread([&]() {
            RemoteGPU::PacketHeader rcv_hdr{};
            std::vector<uint8_t> rcv_payload;
            receiver_result = pair.server->RecvPacket(rcv_hdr, rcv_payload);
            receiver_completed = true;
        });

        // Sender sends 16-byte header advertising 16 MB payload
        RemoteGPU::PacketHeader hdr{};
        hdr.magic = RemoteGPU::RGPU_MAGIC;
        hdr.opcode = RemoteGPU::OP_FRAME_DATA;
        hdr.sequence_id = 7002;
        hdr.payload_length = static_cast<uint32_t>(k16MB);
        send(pair.client.GetSocket(), reinterpret_cast<const char*>(&hdr), sizeof(hdr), 0);

        // Send 128 KB
        std::vector<uint8_t> chunk(131072, 0xAA);
        send(pair.client.GetSocket(), reinterpret_cast<const char*>(chunk.data()), 131072, 0);

        // Set SO_LINGER to {1, 0} to force immediate TCP RST
        linger sl{ 1, 0 };
        setsockopt(pair.client.GetSocket(), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&sl), sizeof(sl));
        pair.client.Close();

        receiver_thread.join();
        STRESS_ASSERT_MSG(receiver_completed.load(), "Receiver unblocked promptly on TCP RST");
        STRESS_ASSERT_MSG(!receiver_result.load(), "RecvPacket detected connection reset without hanging");
    }

    std::cout << "--- [4.4] Abrupt Termination: Receiver closes socket with RST while sender is transmitting ---" << std::endl;
    {
        auto pair = EstablishLoopbackPair();
        STRESS_ASSERT(pair.client.IsValid() && pair.server && pair.server->IsValid());

        const size_t k32MB = 32 * 1024 * 1024;
        std::vector<uint8_t> large_buf(k32MB, 0xCC);

        std::atomic<bool> sender_completed{false};
        std::atomic<bool> sender_result{true};

        std::thread sender_thread([&]() {
            RemoteGPU::PacketHeader hdr{};
            hdr.magic = RemoteGPU::RGPU_MAGIC;
            hdr.opcode = RemoteGPU::OP_FRAME_DATA;
            hdr.sequence_id = 7003;
            hdr.payload_length = static_cast<uint32_t>(k32MB);
            sender_result = pair.client.SendPacket(hdr, large_buf.data());
            sender_completed = true;
        });

        // Receiver reads only the header and 64 KB, then terminates with RST
        RemoteGPU::PacketHeader rcv_hdr{};
        pair.server->RecvExact(&rcv_hdr, sizeof(rcv_hdr));
        std::vector<uint8_t> partial(65536);
        pair.server->RecvExact(partial.data(), 65536);

        // Abortively close receiver with RST
        linger sl{ 1, 0 };
        setsockopt(pair.server->GetSocket(), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&sl), sizeof(sl));
        pair.server->Close();

        sender_thread.join();
        STRESS_ASSERT_MSG(sender_completed.load(), "Sender unblocked promptly when receiver RST disconnected");
        STRESS_ASSERT_MSG(!sender_result.load(), "SendPacket returned false upon remote RST");
    }

    std::cout << "--- [4.5] Concurrency Stress & Deadlock Freedom under Abrupt Peer Drop ---" << std::endl;
    {
        auto pair = EstablishLoopbackPair();
        STRESS_ASSERT(pair.client.IsValid() && pair.server && pair.server->IsValid());

        // Bidirectional streaming between two threads
        std::atomic<bool> stop_flag{false};
        std::atomic<bool> client_deadlock_free{false};
        std::atomic<bool> server_deadlock_free{false};

        std::thread server_bidir([&]() {
            std::vector<uint8_t> buf(1024 * 1024, 0x11);
            RemoteGPU::PacketHeader hdr{};
            hdr.magic = RemoteGPU::RGPU_MAGIC;
            hdr.opcode = RemoteGPU::OP_FRAME_DATA;
            hdr.payload_length = 1024 * 1024;

            for (int i = 0; i < 50; ++i) {
                if (stop_flag.load()) break;
                hdr.sequence_id = i;
                pair.server->SendPacket(hdr, buf.data());

                RemoteGPU::PacketHeader rcv{};
                std::vector<uint8_t> rcv_buf;
                pair.server->RecvPacket(rcv, rcv_buf);
            }
            server_deadlock_free = true;
        });

        std::thread client_bidir([&]() {
            std::vector<uint8_t> buf(1024 * 1024, 0x22);
            RemoteGPU::PacketHeader hdr{};
            hdr.magic = RemoteGPU::RGPU_MAGIC;
            hdr.opcode = RemoteGPU::OP_FRAME_DATA;
            hdr.payload_length = 1024 * 1024;

            for (int i = 0; i < 20; ++i) {
                hdr.sequence_id = i;
                pair.client.SendPacket(hdr, buf.data());

                RemoteGPU::PacketHeader rcv{};
                std::vector<uint8_t> rcv_buf;
                pair.client.RecvPacket(rcv, rcv_buf);
            }

            // Abrupt drop by client mid-flight!
            stop_flag = true;
            linger sl{ 1, 0 };
            setsockopt(pair.client.GetSocket(), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&sl), sizeof(sl));
            pair.client.Close();
            client_deadlock_free = true;
        });

        client_bidir.join();
        server_bidir.join();

        STRESS_ASSERT_MSG(client_deadlock_free.load(), "Client completed without deadlock");
        STRESS_ASSERT_MSG(server_deadlock_free.load(), "Server unblocked and completed without deadlock");
    }
}

// ============================================================================
// Main Benchmark Runner
// ============================================================================
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    std::cout << "============================================================" << std::endl;
    std::cout << "  Remote GPU Offload - Transport Empirical Benchmark & Stress" << std::endl;
    std::cout << "============================================================" << std::endl;

    if (!RemoteGPU::NetworkSocket::InitializeNetwork()) {
        std::cerr << "[FATAL] Failed to initialize Winsock2!" << std::endl;
        return 1;
    }

    auto total_start = std::chrono::high_resolution_clock::now();

    RUN_BENCHMARK_SUITE(Benchmark_ZeroLengthPayload_Transfers);
    RUN_BENCHMARK_SUITE(Benchmark_LargePayload_Transfers);
    RUN_BENCHMARK_SUITE(Benchmark_Loopback_Optimizations);
    RUN_BENCHMARK_SUITE(Benchmark_AbruptDisconnection_Resilience);

    auto total_end = std::chrono::high_resolution_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(total_end - total_start).count();

    RemoteGPU::NetworkSocket::ShutdownNetwork();

    std::cout << "============================================================" << std::endl;
    std::cout << "  EMPIRICAL BENCHMARK SUMMARY" << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << "  Total Assertions Run : " << g_tests_run << std::endl;
    std::cout << "  Passed               : " << g_tests_passed << std::endl;
    std::cout << "  Failed               : " << g_tests_failed << std::endl;
    std::cout << "  Total Duration       : " << total_ms << " ms" << std::endl;
    std::cout << "============================================================" << std::endl;

    if (g_tests_failed == 0) {
        std::cout << "  OVERALL STATUS: ALL EMPIRICAL BENCHMARKS PASSED (EXIT CODE 0)" << std::endl;
        std::cout << "============================================================" << std::endl;
        return 0;
    } else {
        std::cerr << "  OVERALL STATUS: BENCHMARKS FAILED (" << g_tests_failed << " failures)" << std::endl;
        std::cout << "============================================================" << std::endl;
        return 1;
    }
}
