// ============================================================================
// Remote GPU Offload - Milestone 1 Transport Layer Adversarial Stress Test Suite
// File: tests/test_transport_stress.cpp
// Standard: C++17
// Author: Challenger 1 (Empirical Verification & Stress Testing)
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

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
#include <future>
#include <atomic>
#include <random>
#include <algorithm>

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
    suite_func(); \
    auto t_end = std::chrono::high_resolution_clock::now(); \
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count(); \
    if (g_stress_failed.load() == start_fails) { \
        std::cout << "[PASSED  ] " #suite_func " (" << elapsed_ms << " ms)" << std::endl; \
    } else { \
        std::cout << "[FAILED  ] " #suite_func " (" << elapsed_ms << " ms) - " \
                  << (g_stress_failed.load() - start_fails) << " assertion failure(s)!" << std::endl; \
    } \
} while (0)

// Helper to get ephemeral loopback listener
static bool CreateEphemeralListener(RemoteGPU::NetworkSocket& listener, uint16_t& out_port) {
    if (!listener.BindAndListen(0, "127.0.0.1")) {
        return false;
    }
    out_port = listener.GetLocalPort();
    return out_port != 0;
}

// ----------------------------------------------------------------------------
// Suite 1: Rapid Back-to-Back Connection & Disconnection Churn
// ----------------------------------------------------------------------------
void Stress_ConnectionChurn_And_AbruptResets() {
    // 1.1: 100 rapid connect / immediate disconnect cycles without data
    {
        RemoteGPU::NetworkSocket listener;
        uint16_t port = 0;
        STRESS_ASSERT(CreateEphemeralListener(listener, port));

        std::atomic<bool> server_running{true};
        std::atomic<int> accepted_count{0};

        std::thread server_thread([&]() {
            while (server_running.load()) {
                auto client_conn = listener.Accept(100);
                if (client_conn) {
                    accepted_count.fetch_add(1);
                    // Server attempts to recv packet, client closes immediately
                    RemoteGPU::PacketHeader hdr{};
                    std::vector<uint8_t> payload;
                    bool ok = client_conn->RecvPacket(hdr, payload);
                    // RecvPacket must fail gracefully (clean EOF)
                    STRESS_ASSERT(!ok);
                    client_conn->Close();
                }
            }
        });

        for (int i = 0; i < 100; ++i) {
            RemoteGPU::NetworkSocket client;
            bool ok = client.Connect("127.0.0.1", port, 2000);
            STRESS_ASSERT(ok);
            STRESS_ASSERT(client.IsValid());
            client.Close();
            STRESS_ASSERT(!client.IsValid());
        }

        // Wait a moment for server to finish accepting all
        for (int w = 0; w < 50 && accepted_count.load() < 100; ++w) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        server_running.store(false);
        server_thread.join();

        STRESS_ASSERT_MSG(accepted_count.load() == 100, "Server accepted all 100 rapid connect/disconnect cycles");
        listener.Close();
    }

    // 1.2: 50 cycles of hard socket resets (TCP RST via SO_LINGER) mid-header transmission
    {
        RemoteGPU::NetworkSocket listener;
        uint16_t port = 0;
        STRESS_ASSERT(CreateEphemeralListener(listener, port));

        std::atomic<bool> server_running{true};
        std::atomic<int> rst_handled{0};

        std::thread server_thread([&]() {
            while (server_running.load()) {
                auto client_conn = listener.Accept(100);
                if (client_conn) {
                    rst_handled.fetch_add(1);
                    RemoteGPU::PacketHeader hdr{};
                    std::vector<uint8_t> payload;
                    bool ok = client_conn->RecvPacket(hdr, payload);
                    STRESS_ASSERT(!ok);
                    client_conn->Close();
                }
            }
        });

        for (int i = 0; i < 50; ++i) {
            RemoteGPU::NetworkSocket client;
            bool ok = client.Connect("127.0.0.1", port, 2000);
            STRESS_ASSERT(ok);

            // Send partial header (varying between 1 and 15 bytes)
            size_t partial_len = 1 + (i % 15);
            char partial_bytes[16] = { 'R', 'G', 'P', 'U', 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
            send(client.GetSocket(), partial_bytes, static_cast<int>(partial_len), 0);

            // Force hard RST teardown via SO_LINGER
            struct linger sl{};
            sl.l_onoff = 1;
            sl.l_linger = 0;
            setsockopt(client.GetSocket(), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&sl), sizeof(sl));

            client.Close();
        }

        for (int w = 0; w < 50 && rst_handled.load() < 50; ++w) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        server_running.store(false);
        server_thread.join();

        STRESS_ASSERT_MSG(rst_handled.load() == 50, "Server survived 50 hard TCP RST partial header aborts");
        listener.Close();
    }

    // 1.3: 50 cycles of header sent with non-zero payload length, followed by abrupt payload truncation
    {
        RemoteGPU::NetworkSocket listener;
        uint16_t port = 0;
        STRESS_ASSERT(CreateEphemeralListener(listener, port));

        std::atomic<bool> server_running{true};
        std::atomic<int> truncated_handled{0};

        std::thread server_thread([&]() {
            while (server_running.load()) {
                auto client_conn = listener.Accept(100);
                if (client_conn) {
                    truncated_handled.fetch_add(1);
                    RemoteGPU::PacketHeader hdr{};
                    std::vector<uint8_t> payload;
                    bool ok = client_conn->RecvPacket(hdr, payload);
                    // Must detect incomplete payload transfer and return false cleanly
                    STRESS_ASSERT(!ok);
                    client_conn->Close();
                }
            }
        });

        for (int i = 0; i < 50; ++i) {
            RemoteGPU::NetworkSocket client;
            bool ok = client.Connect("127.0.0.1", port, 2000);
            STRESS_ASSERT(ok);

            // Header declaring 8192 bytes of payload
            RemoteGPU::PacketHeader hdr{};
            hdr.magic = RemoteGPU::RGPU_MAGIC;
            hdr.opcode = RemoteGPU::OP_UPDATE_SUBRESOURCE;
            hdr.flags = RemoteGPU::FLAG_NONE;
            hdr.sequence_id = static_cast<uint32_t>(i + 1);
            hdr.payload_length = 8192;

            send(client.GetSocket(), reinterpret_cast<const char*>(&hdr), sizeof(hdr), 0);

            // Send only 128 bytes of payload (less than the advertised 8192)
            char partial_payload[128]{};
            send(client.GetSocket(), partial_payload, sizeof(partial_payload), 0);

            // Abrupt close
            client.Close();
        }

        for (int w = 0; w < 50 && truncated_handled.load() < 50; ++w) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        server_running.store(false);
        server_thread.join();

        STRESS_ASSERT_MSG(truncated_handled.load() == 50, "Server survived 50 truncated payload drops");
        listener.Close();
    }

    // 1.4: Server accepts connection, immediately closes; Client attempts sends and recvs
    {
        RemoteGPU::NetworkSocket listener;
        uint16_t port = 0;
        STRESS_ASSERT(CreateEphemeralListener(listener, port));

        std::thread server_thread([&]() {
            for (int i = 0; i < 20; ++i) {
                auto client_conn = listener.Accept(2000);
                if (client_conn) {
                    client_conn->Close(); // immediate server-side abort
                }
            }
        });

        for (int i = 0; i < 20; ++i) {
            RemoteGPU::NetworkSocket client;
            bool ok = client.Connect("127.0.0.1", port, 2000);
            STRESS_ASSERT(ok);

            // Wait a tiny moment for server to close
            std::this_thread::sleep_for(std::chrono::milliseconds(5));

            RemoteGPU::PacketHeader hdr{};
            hdr.magic = RemoteGPU::RGPU_MAGIC;
            hdr.opcode = RemoteGPU::OP_PRESENT;
            hdr.sequence_id = i;
            hdr.payload_length = 0;

            // RecvPacket must return false
            RemoteGPU::PacketHeader recv_hdr{};
            std::vector<uint8_t> recv_payload;
            bool r_ok = client.RecvPacket(recv_hdr, recv_payload);
            STRESS_ASSERT(!r_ok);

            client.Close();
        }

        server_thread.join();
        listener.Close();
    }
}

// ----------------------------------------------------------------------------
// Suite 2: High-Concurrency Multi-Threaded Packet Bursting
// ----------------------------------------------------------------------------
void Stress_HighConcurrency_PacketBursting() {
    // 2.1: 4 concurrent client threads bursting 1,000 packets each (4,000 total)
    {
        RemoteGPU::NetworkSocket listener;
        uint16_t port = 0;
        STRESS_ASSERT(CreateEphemeralListener(listener, port));

        constexpr int NUM_CLIENT_THREADS = 4;
        constexpr int PACKETS_PER_CLIENT = 1000;
        constexpr int TOTAL_PACKETS = NUM_CLIENT_THREADS * PACKETS_PER_CLIENT;

        std::atomic<int> total_received{0};
        std::atomic<int> verification_errors{0};

        std::vector<std::thread> server_workers;
        std::thread listener_thread([&]() {
            for (int c = 0; c < NUM_CLIENT_THREADS; ++c) {
                auto conn = listener.Accept(5000);
                STRESS_ASSERT(conn != nullptr);
                if (!conn) continue;

                server_workers.emplace_back([client_sock = std::move(conn), &total_received, &verification_errors]() mutable {
                    std::vector<uint8_t> payload;
                    RemoteGPU::PacketHeader hdr{};
                    while (total_received.load() < TOTAL_PACKETS) {
                        bool ok = client_sock->RecvPacket(hdr, payload);
                        if (!ok) break;

                        // Verify packet header
                        if (hdr.magic != RemoteGPU::RGPU_MAGIC) {
                            verification_errors.fetch_add(1);
                        }
                        // Verify payload content matches pattern
                        if (hdr.payload_length > 0) {
                            uint8_t expected_val = static_cast<uint8_t>(hdr.sequence_id & 0xFF);
                            bool match = true;
                            for (size_t i = 0; i < payload.size(); ++i) {
                                if (payload[i] != expected_val) {
                                    match = false;
                                    break;
                                }
                            }
                            if (!match) {
                                verification_errors.fetch_add(1);
                            }
                        }
                        total_received.fetch_add(1);
                    }
                    client_sock->Close();
                });
            }
        });

        std::vector<std::thread> client_threads;
        for (int t = 0; t < NUM_CLIENT_THREADS; ++t) {
            client_threads.emplace_back([port, t]() {
                RemoteGPU::NetworkSocket client;
                bool ok = client.Connect("127.0.0.1", port, 5000);
                STRESS_ASSERT(ok);

                for (int p = 0; p < PACKETS_PER_CLIENT; ++p) {
                    uint32_t seq = static_cast<uint32_t>((t + 1) * 100000 + p);
                    size_t p_size = (p % 4 == 0) ? 0 : (p % 4 == 1) ? 64 : (p % 4 == 2) ? 1024 : 8192;

                    RemoteGPU::PacketHeader hdr{};
                    hdr.magic = RemoteGPU::RGPU_MAGIC;
                    hdr.opcode = static_cast<uint16_t>(RemoteGPU::OP_UPDATE_SUBRESOURCE);
                    hdr.flags = RemoteGPU::FLAG_NONE;
                    hdr.sequence_id = seq;
                    hdr.payload_length = static_cast<uint32_t>(p_size);

                    std::vector<uint8_t> payload_data(p_size, static_cast<uint8_t>(seq & 0xFF));
                    bool s_ok = client.SendPacket(hdr, p_size > 0 ? payload_data.data() : nullptr);
                    if (!s_ok) {
                        STRESS_ASSERT_MSG(false, "Client SendPacket failed during concurrent burst");
                        break;
                    }
                }
                client.Close();
            });
        }

        for (auto& ct : client_threads) ct.join();
        listener_thread.join();
        for (auto& st : server_workers) st.join();
        listener.Close();

        STRESS_ASSERT_MSG(total_received.load() == TOTAL_PACKETS, "Received all 4,000 multi-threaded burst packets");
        STRESS_ASSERT_MSG(verification_errors.load() == 0, "Zero verification errors across 4,000 burst packets");
    }

    // 2.2: Extreme throughput burst: 10 back-to-back 16 MB frame payloads (160 MB total)
    {
        RemoteGPU::NetworkSocket listener;
        uint16_t port = 0;
        STRESS_ASSERT(CreateEphemeralListener(listener, port));

        constexpr size_t FRAME_SIZE = 16 * 1024 * 1024; // 16 MB
        constexpr int NUM_FRAMES = 10;
        constexpr size_t TOTAL_BYTES = FRAME_SIZE * NUM_FRAMES;

        std::promise<bool> server_done;
        auto server_future = server_done.get_future();

        std::thread server_thread([&]() {
            auto conn = listener.Accept(5000);
            if (!conn) {
                server_done.set_value(false);
                return;
            }

            std::vector<uint8_t> recv_buf(FRAME_SIZE);
            RemoteGPU::PacketHeader hdr{};
            bool all_ok = true;

            for (int f = 0; f < NUM_FRAMES; ++f) {
                bool ok = conn->RecvPacket(hdr, recv_buf.data(), recv_buf.size());
                if (!ok || hdr.payload_length != FRAME_SIZE || hdr.opcode != RemoteGPU::OP_FRAME_DATA) {
                    all_ok = false;
                    break;
                }
                // Verify byte pattern at start, middle, and end
                uint8_t expected_seed = static_cast<uint8_t>(f * 17);
                if (recv_buf[0] != expected_seed || 
                    recv_buf[FRAME_SIZE / 2] != static_cast<uint8_t>(expected_seed + 1) ||
                    recv_buf[FRAME_SIZE - 1] != static_cast<uint8_t>(expected_seed + 2)) {
                    all_ok = false;
                    break;
                }
            }
            conn->Close();
            server_done.set_value(all_ok);
        });

        RemoteGPU::NetworkSocket client;
        STRESS_ASSERT(client.Connect("127.0.0.1", port, 5000));

        std::vector<uint8_t> frame_buf(FRAME_SIZE);
        auto t_start = std::chrono::high_resolution_clock::now();

        for (int f = 0; f < NUM_FRAMES; ++f) {
            uint8_t expected_seed = static_cast<uint8_t>(f * 17);
            frame_buf[0] = expected_seed;
            frame_buf[FRAME_SIZE / 2] = static_cast<uint8_t>(expected_seed + 1);
            frame_buf[FRAME_SIZE - 1] = static_cast<uint8_t>(expected_seed + 2);

            RemoteGPU::PacketHeader hdr{};
            hdr.magic = RemoteGPU::RGPU_MAGIC;
            hdr.opcode = RemoteGPU::OP_FRAME_DATA;
            hdr.flags = RemoteGPU::FLAG_NONE;
            hdr.sequence_id = static_cast<uint32_t>(f + 1);
            hdr.payload_length = static_cast<uint32_t>(FRAME_SIZE);

            bool ok = client.SendPacket(hdr, frame_buf.data());
            STRESS_ASSERT_MSG(ok, "SendPacket succeeded for 16 MB frame");
        }

        auto t_end = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
        double speed_mb_s = (static_cast<double>(TOTAL_BYTES) / (1024.0 * 1024.0)) / (static_cast<double>(ms) / 1000.0);
        std::cout << "  [PERF] 160 MB (10 x 16 MB frames) transferred in " << ms << " ms (~" 
                  << static_cast<int>(speed_mb_s) << " MB/s)" << std::endl;

        client.Close();
        server_thread.join();
        listener.Close();

        STRESS_ASSERT_MSG(server_future.get(), "All 10 x 16 MB frames verified byte-exact on server");
    }

    // 2.3: Zero-payload high-frequency burst: 20,000 packets
    {
        RemoteGPU::NetworkSocket listener;
        uint16_t port = 0;
        STRESS_ASSERT(CreateEphemeralListener(listener, port));

        constexpr int BURST_COUNT = 20000;
        std::promise<int> server_count_prom;
        auto server_count_fut = server_count_prom.get_future();

        std::thread server_thread([&]() {
            auto conn = listener.Accept(5000);
            if (!conn) {
                server_count_prom.set_value(-1);
                return;
            }

            int count = 0;
            RemoteGPU::PacketHeader hdr{};
            std::vector<uint8_t> payload;

            while (count < BURST_COUNT) {
                if (!conn->RecvPacket(hdr, payload)) {
                    break;
                }
                if (hdr.magic == RemoteGPU::RGPU_MAGIC && hdr.sequence_id == static_cast<uint32_t>(count)) {
                    count++;
                } else {
                    break;
                }
            }
            conn->Close();
            server_count_prom.set_value(count);
        });

        RemoteGPU::NetworkSocket client;
        STRESS_ASSERT(client.Connect("127.0.0.1", port, 5000));

        RemoteGPU::PacketHeader hdr{};
        hdr.magic = RemoteGPU::RGPU_MAGIC;
        hdr.opcode = RemoteGPU::OP_DRAW;
        hdr.flags = RemoteGPU::FLAG_NONE;
        hdr.payload_length = 0;

        auto t0 = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < BURST_COUNT; ++i) {
            hdr.sequence_id = static_cast<uint32_t>(i);
            bool ok = client.SendPacket(hdr, nullptr);
            if (!ok) break;
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        auto burst_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::cout << "  [PERF] 20,000 zero-payload packets sent in " << burst_ms << " ms (~" 
                  << (BURST_COUNT * 1000 / (burst_ms > 0 ? burst_ms : 1)) << " packets/sec)" << std::endl;

        client.Close();
        server_thread.join();
        listener.Close();

        STRESS_ASSERT_MSG(server_count_fut.get() == BURST_COUNT, "Server received all 20,000 zero-payload packets");
    }

    // 2.4: Full-duplex bidirectional simultaneous streaming (500 ping-pong / interleaved exchanges)
    {
        RemoteGPU::NetworkSocket listener;
        uint16_t port = 0;
        STRESS_ASSERT(CreateEphemeralListener(listener, port));

        constexpr int DUPLEX_COUNT = 500;
        std::promise<bool> server_res;
        auto server_res_fut = server_res.get_future();

        std::thread server_thread([&]() {
            auto conn = listener.Accept(5000);
            if (!conn) {
                server_res.set_value(false);
                return;
            }

            // Server reads commands and simultaneously echoes frame response
            bool ok = true;
            for (int i = 0; i < DUPLEX_COUNT; ++i) {
                RemoteGPU::PacketHeader cmd_hdr{};
                std::vector<uint8_t> cmd_payload;
                if (!conn->RecvPacket(cmd_hdr, cmd_payload)) {
                    ok = false;
                    break;
                }

                RemoteGPU::PacketHeader resp_hdr{};
                resp_hdr.magic = RemoteGPU::RGPU_MAGIC;
                resp_hdr.opcode = RemoteGPU::OP_FRAME_DATA;
                resp_hdr.flags = RemoteGPU::FLAG_NONE;
                resp_hdr.sequence_id = cmd_hdr.sequence_id;
                resp_hdr.payload_length = 256;

                uint8_t echo_data[256];
                std::memset(echo_data, 0xEE, sizeof(echo_data));
                if (!conn->SendPacket(resp_hdr, echo_data)) {
                    ok = false;
                    break;
                }
            }
            conn->Close();
            server_res.set_value(ok);
        });

        RemoteGPU::NetworkSocket client;
        STRESS_ASSERT(client.Connect("127.0.0.1", port, 5000));

        bool client_ok = true;
        for (int i = 0; i < DUPLEX_COUNT; ++i) {
            RemoteGPU::PacketHeader cmd_hdr{};
            cmd_hdr.magic = RemoteGPU::RGPU_MAGIC;
            cmd_hdr.opcode = RemoteGPU::OP_DRAW_INDEXED;
            cmd_hdr.flags = RemoteGPU::FLAG_NONE;
            cmd_hdr.sequence_id = static_cast<uint32_t>(i);
            cmd_hdr.payload_length = 128;

            uint8_t client_data[128];
            std::memset(client_data, 0xCC, sizeof(client_data));
            if (!client.SendPacket(cmd_hdr, client_data)) {
                client_ok = false;
                break;
            }

            RemoteGPU::PacketHeader resp_hdr{};
            std::vector<uint8_t> resp_payload;
            if (!client.RecvPacket(resp_hdr, resp_payload)) {
                client_ok = false;
                break;
            }
            if (resp_hdr.sequence_id != static_cast<uint32_t>(i) || resp_payload.size() != 256) {
                client_ok = false;
                break;
            }
        }

        client.Close();
        server_thread.join();
        listener.Close();

        STRESS_ASSERT_MSG(client_ok, "Client completed all 500 full-duplex exchanges");
        STRESS_ASSERT_MSG(server_res_fut.get(), "Server completed all 500 full-duplex exchanges");
    }
}

// ----------------------------------------------------------------------------
// Suite 3: Malformed Packets & Adversarial Fuzzing
// ----------------------------------------------------------------------------
void Stress_MalformedPackets_And_Fuzzing() {
    // 3.1: Invalid Magic Rejections (50 variations)
    {
        RemoteGPU::NetworkSocket listener;
        uint16_t port = 0;
        STRESS_ASSERT(CreateEphemeralListener(listener, port));

        std::thread server_thread([&]() {
            for (int i = 0; i < 50; ++i) {
                auto conn = listener.Accept(2000);
                if (conn) {
                    RemoteGPU::PacketHeader hdr{};
                    std::vector<uint8_t> payload;
                    bool ok = conn->RecvPacket(hdr, payload);
                    // Must reject bad magic
                    STRESS_ASSERT(!ok);
                    conn->Close();
                }
            }
        });

        std::mt19937 rng(1337);
        for (int i = 0; i < 50; ++i) {
            RemoteGPU::NetworkSocket client;
            STRESS_ASSERT(client.Connect("127.0.0.1", port, 2000));

            uint32_t bad_magic = static_cast<uint32_t>(rng());
            if (bad_magic == RemoteGPU::RGPU_MAGIC) bad_magic++;

            RemoteGPU::PacketHeader hdr{};
            hdr.magic = bad_magic;
            hdr.opcode = RemoteGPU::OP_HANDSHAKE_REQUEST;
            hdr.flags = 0;
            hdr.sequence_id = i;
            hdr.payload_length = 0;

            send(client.GetSocket(), reinterpret_cast<const char*>(&hdr), sizeof(hdr), 0);
            client.Close();
        }

        server_thread.join();
        listener.Close();
    }

    // 3.2: Malformed Payload Lengths: 4 GB, 2 GB, and 300 MB (> 256 MB safety limit)
    {
        RemoteGPU::NetworkSocket listener;
        uint16_t port = 0;
        STRESS_ASSERT(CreateEphemeralListener(listener, port));

        std::thread server_thread([&]() {
            // Three tests
            for (int i = 0; i < 3; ++i) {
                auto conn = listener.Accept(2000);
                if (conn) {
                    RemoteGPU::PacketHeader hdr{};
                    std::vector<uint8_t> payload;
                    bool ok = conn->RecvPacket(hdr, payload);
                    // Must reject payload length exceeding safety limit without OOM or crash
                    STRESS_ASSERT(!ok);
                    conn->Close();
                }
            }
        });

        const uint32_t bad_lengths[3] = {
            0xFFFFFFFF,                 // 4 GB - 1
            0x80000000,                 // 2 GB
            300 * 1024 * 1024           // 300 MB (> 256 MB limit)
        };

        for (int i = 0; i < 3; ++i) {
            RemoteGPU::NetworkSocket client;
            STRESS_ASSERT(client.Connect("127.0.0.1", port, 2000));

            RemoteGPU::PacketHeader hdr{};
            hdr.magic = RemoteGPU::RGPU_MAGIC;
            hdr.opcode = RemoteGPU::OP_UPDATE_SUBRESOURCE;
            hdr.flags = 0;
            hdr.sequence_id = i;
            hdr.payload_length = bad_lengths[i];

            send(client.GetSocket(), reinterpret_cast<const char*>(&hdr), sizeof(hdr), 0);
            client.Close();
        }

        server_thread.join();
        listener.Close();
    }

    // 3.3: Fixed-Buffer RecvPacket: Buffer Overflow Prevention
    {
        RemoteGPU::NetworkSocket listener;
        uint16_t port = 0;
        STRESS_ASSERT(CreateEphemeralListener(listener, port));

        std::promise<bool> server_prom;
        auto server_fut = server_prom.get_future();

        std::thread server_thread([&]() {
            auto conn = listener.Accept(2000);
            if (!conn) {
                server_prom.set_value(false);
                return;
            }

            RemoteGPU::PacketHeader hdr{};
            // Pre-allocated buffer of only 512 bytes, flanked by guard canary bytes
            uint8_t canary_before[32];
            uint8_t target_buf[512];
            uint8_t canary_after[32];
            std::memset(canary_before, 0xAA, sizeof(canary_before));
            std::memset(target_buf, 0x00, sizeof(target_buf));
            std::memset(canary_after, 0xBB, sizeof(canary_after));

            // Client will send 2048 bytes payload. Server must reject and NOT overflow target_buf
            bool ok = conn->RecvPacket(hdr, target_buf, sizeof(target_buf));
            STRESS_ASSERT(!ok);

            // Verify canary values intact
            bool canaries_intact = true;
            for (size_t i = 0; i < sizeof(canary_before); ++i) {
                if (canary_before[i] != 0xAA) canaries_intact = false;
            }
            for (size_t i = 0; i < sizeof(canary_after); ++i) {
                if (canary_after[i] != 0xBB) canaries_intact = false;
            }
            STRESS_ASSERT(canaries_intact);

            conn->Close();
            server_prom.set_value(!ok && canaries_intact);
        });

        RemoteGPU::NetworkSocket client;
        STRESS_ASSERT(client.Connect("127.0.0.1", port, 2000));

        RemoteGPU::PacketHeader hdr{};
        hdr.magic = RemoteGPU::RGPU_MAGIC;
        hdr.opcode = RemoteGPU::OP_UPDATE_SUBRESOURCE;
        hdr.flags = 0;
        hdr.sequence_id = 1;
        hdr.payload_length = 2048; // larger than server target_buf

        std::vector<uint8_t> send_payload(2048, 0x77);
        client.SendPacket(hdr, send_payload.data());
        client.Close();

        server_thread.join();
        listener.Close();

        STRESS_ASSERT_MSG(server_fut.get(), "Fixed buffer RecvPacket rejected oversize payload and preserved canaries");
    }

    // 3.4: Raw Stream Random Byte Fuzzing (64 KB stream of pseudo-random noise)
    {
        RemoteGPU::NetworkSocket listener;
        uint16_t port = 0;
        STRESS_ASSERT(CreateEphemeralListener(listener, port));

        std::promise<bool> server_fuzz_prom;
        auto server_fuzz_fut = server_fuzz_prom.get_future();

        std::thread server_thread([&]() {
            auto conn = listener.Accept(2000);
            if (!conn) {
                server_fuzz_prom.set_value(false);
                return;
            }

            // Server attempts to read packets in a loop from random stream
            bool survived = true;
            RemoteGPU::PacketHeader hdr{};
            std::vector<uint8_t> payload;

            // RecvPacket should quickly reject desynchronized stream
            bool ok = conn->RecvPacket(hdr, payload);
            if (ok) {
                // If it happened to match magic by random 1 in 4 billion chance, check next
                conn->RecvPacket(hdr, payload);
            }
            conn->Close();
            server_fuzz_prom.set_value(survived);
        });

        RemoteGPU::NetworkSocket client;
        STRESS_ASSERT(client.Connect("127.0.0.1", port, 2000));

        std::vector<uint8_t> random_noise(64 * 1024);
        std::mt19937 rng(42);
        for (auto& b : random_noise) {
            b = static_cast<uint8_t>(rng() & 0xFF);
        }

        // Blast random noise
        send(client.GetSocket(), reinterpret_cast<const char*>(random_noise.data()), static_cast<int>(random_noise.size()), 0);
        client.Close();

        server_thread.join();
        listener.Close();

        STRESS_ASSERT_MSG(server_fuzz_fut.get(), "Server cleanly survived 64 KB random fuzz stream without crashing");
    }

    // 3.5: Null Pointers and Boundary Invariants
    {
        RemoteGPU::NetworkSocket invalid_sock;
        STRESS_ASSERT(!invalid_sock.IsValid());
        STRESS_ASSERT(invalid_sock.GetSocket() == INVALID_SOCKET);

        char dummy[16];
        RemoteGPU::PacketHeader hdr{};
        std::vector<uint8_t> payload;

        // Operations on invalid socket must all safely return false / empty
        STRESS_ASSERT(!invalid_sock.SendExact(dummy, sizeof(dummy)));
        STRESS_ASSERT(!invalid_sock.RecvExact(dummy, sizeof(dummy)));
        STRESS_ASSERT(!invalid_sock.SendPacket(hdr, dummy));
        STRESS_ASSERT(!invalid_sock.RecvPacket(hdr, payload));
        STRESS_ASSERT(!invalid_sock.RecvPacket(hdr, dummy, sizeof(dummy)));
        STRESS_ASSERT(!invalid_sock.SetTimeouts(100, 100));
        STRESS_ASSERT(!invalid_sock.SetNonBlocking(true));
        STRESS_ASSERT(invalid_sock.GetPeerAddress().empty());
        STRESS_ASSERT(invalid_sock.GetPeerPort() == 0);
        STRESS_ASSERT(invalid_sock.GetLocalAddress().empty());
        STRESS_ASSERT(invalid_sock.GetLocalPort() == 0);

        // Valid socket null-pointer arguments
        RemoteGPU::NetworkSocket listener;
        uint16_t port = 0;
        STRESS_ASSERT(CreateEphemeralListener(listener, port));

        RemoteGPU::NetworkSocket client;
        STRESS_ASSERT(client.Connect("127.0.0.1", port, 2000));

        STRESS_ASSERT(!client.SendExact(nullptr, 100));
        STRESS_ASSERT(!client.RecvExact(nullptr, 100));

        hdr.magic = RemoteGPU::RGPU_MAGIC;
        hdr.payload_length = 100;
        STRESS_ASSERT(!client.SendPacket(hdr, nullptr)); // Non-zero payload length with null ptr

        STRESS_ASSERT(!client.RecvPacket(hdr, nullptr, 100)); // Null output buffer

        // Null host connect
        RemoteGPU::NetworkSocket null_host_sock;
        STRESS_ASSERT(!null_host_sock.Connect(nullptr, 80));

        // 0-length send/recv on valid connected socket must return true immediately
        STRESS_ASSERT(client.SendExact(dummy, 0));
        STRESS_ASSERT(client.RecvExact(dummy, 0));
        STRESS_ASSERT(client.SendExact(nullptr, 0));
        STRESS_ASSERT(client.RecvExact(nullptr, 0));

        client.Close();
        listener.Close();
    }
}

// ----------------------------------------------------------------------------
// Suite 4: Timeout Resilience & Anti-Hang Verification
// ----------------------------------------------------------------------------
void Stress_TimeoutResilience_And_AntiHang() {
    // 4.1: Recv Timeout: Client connects, sends 0 bytes. Server socket has 300 ms timeout.
    {
        RemoteGPU::NetworkSocket listener;
        uint16_t port = 0;
        STRESS_ASSERT(CreateEphemeralListener(listener, port));

        std::promise<int64_t> server_ms_prom;
        auto server_ms_fut = server_ms_prom.get_future();

        std::thread server_thread([&]() {
            auto conn = listener.Accept(2000);
            if (!conn) {
                server_ms_prom.set_value(-1);
                return;
            }

            // Set receive timeout to 300 ms
            conn->SetTimeouts(300, 300);

            RemoteGPU::PacketHeader hdr{};
            std::vector<uint8_t> payload;

            auto t0 = std::chrono::high_resolution_clock::now();
            bool ok = conn->RecvPacket(hdr, payload);
            auto t1 = std::chrono::high_resolution_clock::now();

            STRESS_ASSERT(!ok);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            conn->Close();
            server_ms_prom.set_value(elapsed);
        });

        RemoteGPU::NetworkSocket client;
        STRESS_ASSERT(client.Connect("127.0.0.1", port, 2000));
        // Client sleeps without sending anything
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        client.Close();

        server_thread.join();
        listener.Close();

        int64_t elapsed_ms = server_ms_fut.get();
        STRESS_ASSERT_MSG(elapsed_ms >= 250 && elapsed_ms <= 1500, 
                          "RecvPacket timeout triggered in expected range (~300ms)");
    }

    // 4.2: Accept Timeout: Server calls Accept(timeout_ms = 250) with no client connecting
    {
        RemoteGPU::NetworkSocket listener;
        uint16_t port = 0;
        STRESS_ASSERT(CreateEphemeralListener(listener, port));

        auto t0 = std::chrono::high_resolution_clock::now();
        auto conn = listener.Accept(250);
        auto t1 = std::chrono::high_resolution_clock::now();

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        STRESS_ASSERT(conn == nullptr);
        STRESS_ASSERT_MSG(elapsed >= 200 && elapsed <= 800, "Accept timeout expired in expected range (~250ms)");

        listener.Close();
    }

    // 4.3: Partial Read Timeout: Client sends 8 bytes of 16-byte header, then stops.
    {
        RemoteGPU::NetworkSocket listener;
        uint16_t port = 0;
        STRESS_ASSERT(CreateEphemeralListener(listener, port));

        std::promise<bool> server_prom;
        auto server_fut = server_prom.get_future();

        std::thread server_thread([&]() {
            auto conn = listener.Accept(2000);
            if (!conn) {
                server_prom.set_value(false);
                return;
            }

            conn->SetTimeouts(300, 300);

            RemoteGPU::PacketHeader hdr{};
            std::vector<uint8_t> payload;

            auto t0 = std::chrono::high_resolution_clock::now();
            bool ok = conn->RecvPacket(hdr, payload);
            auto t1 = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

            STRESS_ASSERT(!ok);
            bool timed_out_properly = (elapsed >= 250 && elapsed <= 1500);
            conn->Close();
            server_prom.set_value(!ok && timed_out_properly);
        });

        RemoteGPU::NetworkSocket client;
        STRESS_ASSERT(client.Connect("127.0.0.1", port, 2000));

        // Send half a header
        char half_hdr[8] = { 'R', 'G', 'P', 'U', 0x01, 0x00, 0x00, 0x00 };
        send(client.GetSocket(), half_hdr, sizeof(half_hdr), 0);

        // Hold connection open without sending more
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        client.Close();

        server_thread.join();
        listener.Close();

        STRESS_ASSERT_MSG(server_fut.get(), "RecvPacket timed out properly on stalled partial header");
    }
}

// ----------------------------------------------------------------------------
// Suite 5: Resource & Socket Descriptor Leak Verification
// ----------------------------------------------------------------------------
void Stress_ResourceLeaks_And_Concurrency() {
    // 5.1: Windows Process Handle Count Stability
    {
        HANDLE hProcess = GetCurrentProcess();
        DWORD handle_before = 0;
        BOOL ok1 = GetProcessHandleCount(hProcess, &handle_before);
        STRESS_ASSERT(ok1);

        RemoteGPU::NetworkSocket listener;
        uint16_t port = 0;
        STRESS_ASSERT(CreateEphemeralListener(listener, port));

        std::atomic<bool> server_running{true};
        std::thread server_thread([&]() {
            while (server_running.load()) {
                auto conn = listener.Accept(50);
                if (conn) {
                    RemoteGPU::PacketHeader hdr{};
                    std::vector<uint8_t> payload;
                    conn->RecvPacket(hdr, payload);
                    conn->Close();
                }
            }
        });

        // Execute 200 connection and packet transmission cycles
        for (int i = 0; i < 200; ++i) {
            RemoteGPU::NetworkSocket client;
            if (client.Connect("127.0.0.1", port, 2000)) {
                RemoteGPU::PacketHeader hdr{};
                hdr.magic = RemoteGPU::RGPU_MAGIC;
                hdr.opcode = RemoteGPU::OP_PRESENT;
                hdr.sequence_id = i;
                hdr.payload_length = 0;
                client.SendPacket(hdr, nullptr);
                client.Close();
            }
        }

        server_running.store(false);
        server_thread.join();
        listener.Close();

        // Allow Windows socket cleanup threads to settle
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        DWORD handle_after = 0;
        BOOL ok2 = GetProcessHandleCount(hProcess, &handle_after);
        STRESS_ASSERT(ok2);

        int handle_diff = static_cast<int>(handle_after) - static_cast<int>(handle_before);
        std::cout << "  [RESOURCE] Handles Before: " << handle_before 
                  << ", Handles After: " << handle_after 
                  << " (Delta: " << handle_diff << " handles after 200 socket cycles)" << std::endl;

        // An unchecked socket leak would increase handles by at least 200 to 400.
        // Operating system background worker threads can fluctuate slightly (+/- 15),
        // but delta should be strictly less than 30 handles.
        STRESS_ASSERT_MSG(std::abs(handle_diff) < 30, "Process handles remained stable; zero socket handle leaks");
    }

    // 5.2: Multi-Threaded InitializeNetwork() & ShutdownNetwork() Concurrency
    {
        constexpr int NUM_INIT_THREADS = 8;
        constexpr int ITERS_PER_THREAD = 100;

        std::vector<std::thread> init_threads;
        for (int t = 0; t < NUM_INIT_THREADS; ++t) {
            init_threads.emplace_back([]() {
                for (int i = 0; i < ITERS_PER_THREAD; ++i) {
                    bool init_ok = RemoteGPU::NetworkSocket::InitializeNetwork();
                    STRESS_ASSERT(init_ok);
                    RemoteGPU::NetworkSocket::ShutdownNetwork();
                }
            });
        }

        for (auto& t : init_threads) {
            t.join();
        }

        // Re-verify network is fully operational after concurrent init/shutdown
        STRESS_ASSERT(RemoteGPU::NetworkSocket::InitializeNetwork());
        RemoteGPU::NetworkSocket test_sock;
        uint16_t test_port = 0;
        STRESS_ASSERT(CreateEphemeralListener(test_sock, test_port));
        test_sock.Close();
    }
}

// ----------------------------------------------------------------------------
// Main Entrypoint
// ----------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    std::cout << "============================================================" << std::endl;
    std::cout << "  Remote GPU Offloading - Milestone 1 Transport STRESS Suite" << std::endl;
    std::cout << "  Empirical Adversarial Verification" << std::endl;
    std::cout << "============================================================" << std::endl;

    if (!RemoteGPU::NetworkSocket::InitializeNetwork()) {
        std::cerr << "[FATAL] Failed to initialize Winsock2 networking!" << std::endl;
        return 1;
    }

    auto total_start = std::chrono::high_resolution_clock::now();

    RUN_STRESS_SUITE(Stress_ConnectionChurn_And_AbruptResets);
    RUN_STRESS_SUITE(Stress_HighConcurrency_PacketBursting);
    RUN_STRESS_SUITE(Stress_MalformedPackets_And_Fuzzing);
    RUN_STRESS_SUITE(Stress_TimeoutResilience_And_AntiHang);
    RUN_STRESS_SUITE(Stress_ResourceLeaks_And_Concurrency);

    auto total_end = std::chrono::high_resolution_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(total_end - total_start).count();

    RemoteGPU::NetworkSocket::ShutdownNetwork();

    std::cout << "============================================================" << std::endl;
    std::cout << "  STRESS TEST SUMMARY" << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << "  Total Assertions Run : " << g_stress_run.load() << std::endl;
    std::cout << "  Passed               : " << g_stress_passed.load() << std::endl;
    std::cout << "  Failed               : " << g_stress_failed.load() << std::endl;
    std::cout << "  Total Duration       : " << total_ms << " ms" << std::endl;
    std::cout << "============================================================" << std::endl;

    if (g_stress_failed.load() == 0) {
        std::cout << "  OVERALL STATUS: ALL STRESS TESTS PASSED (EXIT CODE 0)" << std::endl;
        std::cout << "============================================================" << std::endl;
        return 0;
    } else {
        std::cerr << "  OVERALL STATUS: STRESS TESTS FAILED (" << g_stress_failed.load() << " failures)" << std::endl;
        std::cout << "============================================================" << std::endl;
        return 1;
    }
}
