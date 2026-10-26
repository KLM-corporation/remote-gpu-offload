#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include "../explorer_m1_2/draft_transport.h"
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

// Include the protocol types definition from explorer_m1_1
namespace RemoteGPU {

struct HandshakeRequestPayload {
    uint32_t protocol_version;
    uint32_t client_flags;
    uint32_t client_width;
    uint32_t client_height;
    uint32_t preferred_format;
};
using HandshakeRequest = HandshakeRequestPayload;

struct HandshakeResponsePayload {
    uint32_t protocol_version;
    uint32_t status_code;
    uint32_t feature_level;
    uint32_t frame_width;
    uint32_t frame_height;
    uint32_t frame_format;
    uint64_t dedicated_vram;
    char     adapter_desc[128];
};
using HandshakeResponse = HandshakeResponsePayload;

struct CreateBufferPayload {
    uint32_t res_id;
    uint32_t byte_width;
    uint32_t usage;
    uint32_t bind_flags;
    uint32_t cpu_access_flags;
    uint32_t misc_flags;
    uint32_t structure_byte_stride;
    uint32_t has_initial_data;
    uint32_t initial_data_size;
};

struct ClearRtvPayload {
    uint32_t res_id;
    float    color[4];
};

struct DrawIndexedPayload {
    uint32_t index_count;
    uint32_t start_index_location;
    int32_t  base_vertex_location;
};

struct PresentPayload {
    uint32_t swapchain_id;
    uint32_t sync_interval;
    uint32_t flags;
    uint64_t frame_number;
};

struct FrameDataPayload {
    uint32_t swapchain_id;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t row_pitch;
    uint32_t data_size;
    uint64_t frame_number;
};

enum RgpuOpcode : uint16_t {
    OP_HANDSHAKE_REQUEST    = 0x0001,
    OP_HANDSHAKE_RESPONSE   = 0x0002,
    OP_SHUTDOWN             = 0x0003,
    OP_CREATE_BUFFER        = 0x0010,
    OP_DRAW_INDEXED         = 0x0055,
    OP_PRESENT              = 0x0070,
    OP_FRAME_DATA           = 0x0071
};

} // namespace RemoteGPU

// Test framework
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

void Test_PacketHeader_PackingAndConstants() {
    static_assert(sizeof(RemoteGPU::PacketHeader) == 16, "PacketHeader must be exactly 16 bytes");
    TEST_ASSERT_MSG(sizeof(RemoteGPU::PacketHeader) == 16, "PacketHeader size == 16");

    TEST_ASSERT(offsetof(RemoteGPU::PacketHeader, magic) == 0);
    TEST_ASSERT(offsetof(RemoteGPU::PacketHeader, opcode) == 4);
    TEST_ASSERT(offsetof(RemoteGPU::PacketHeader, flags) == 6);
    TEST_ASSERT(offsetof(RemoteGPU::PacketHeader, sequence_id) == 8);
    TEST_ASSERT(offsetof(RemoteGPU::PacketHeader, payload_length) == 12);

    TEST_ASSERT(sizeof(RemoteGPU::PacketHeader::magic) == 4);
    TEST_ASSERT(sizeof(RemoteGPU::PacketHeader::opcode) == 2);
    TEST_ASSERT(sizeof(RemoteGPU::PacketHeader::flags) == 2);
    TEST_ASSERT(sizeof(RemoteGPU::PacketHeader::sequence_id) == 4);
    TEST_ASSERT(sizeof(RemoteGPU::PacketHeader::payload_length) == 4);

    TEST_ASSERT_MSG(RemoteGPU::RGPU_MAGIC == 0x52475055, "RGPU_MAGIC == 0x52475055");
    uint32_t magic_val = RemoteGPU::RGPU_MAGIC;
    // On x86-64 little-endian, integer 0x52475055 has LSB 0x55 ('U') at index 0 and MSB 0x52 ('R') at index 3
    const uint8_t* magic_bytes = reinterpret_cast<const uint8_t*>(&magic_val);
    TEST_ASSERT(magic_bytes[0] == 0x55); // 'U'
    TEST_ASSERT(magic_bytes[1] == 0x50); // 'P'
    TEST_ASSERT(magic_bytes[2] == 0x47); // 'G'
    TEST_ASSERT(magic_bytes[3] == 0x52); // 'R'
    TEST_ASSERT(((magic_val >> 24) & 0xFF) == 'R');
    TEST_ASSERT(((magic_val >> 16) & 0xFF) == 'G');
    TEST_ASSERT(((magic_val >> 8) & 0xFF) == 'P');
    TEST_ASSERT((magic_val & 0xFF) == 'U');

    TEST_ASSERT(RemoteGPU::RGPU_PROTOCOL_VERSION == 1);
    TEST_ASSERT(RemoteGPU::RGPU_DEFAULT_PORT == 48888);

    TEST_ASSERT(RemoteGPU::FLAG_NONE == 0x0000);
    TEST_ASSERT((RemoteGPU::FLAG_SYNC & RemoteGPU::FLAG_RESPONSE_REQUIRED) == 0);
    TEST_ASSERT((RemoteGPU::FLAG_COMPRESSED & RemoteGPU::FLAG_ONEWAY) == 0);
    TEST_ASSERT((RemoteGPU::FLAG_END_OF_FRAME & RemoteGPU::FLAG_HEARTBEAT) == 0);
}

void Test_Payload_SerializationRoundTrip() {
    {
        RemoteGPU::HandshakeRequest src{};
        src.protocol_version = RemoteGPU::RGPU_PROTOCOL_VERSION;
        src.client_flags = RemoteGPU::FLAG_NONE;
        src.client_width = 1920;
        src.client_height = 1080;
        src.preferred_format = 28;

        std::vector<uint8_t> buffer(sizeof(src));
        std::memcpy(buffer.data(), &src, sizeof(src));

        RemoteGPU::HandshakeRequest dst{};
        std::memcpy(&dst, buffer.data(), sizeof(dst));

        TEST_ASSERT(dst.protocol_version == src.protocol_version);
        TEST_ASSERT(dst.client_flags == src.client_flags);
        TEST_ASSERT(dst.client_width == src.client_width);
        TEST_ASSERT(dst.client_height == src.client_height);
        TEST_ASSERT(dst.preferred_format == src.preferred_format);
    }

    {
        RemoteGPU::HandshakeResponse src{};
        src.protocol_version = RemoteGPU::RGPU_PROTOCOL_VERSION;
        src.status_code = 0;
        src.feature_level = 0xb000;
        src.frame_width = 1920;
        src.frame_height = 1080;
        src.frame_format = 28;
        src.dedicated_vram = 8589934592ULL;
        const char* desc = "Intel(R) UHD Graphics 620";
        std::strncpy(src.adapter_desc, desc, sizeof(src.adapter_desc) - 1);

        std::vector<uint8_t> buffer(sizeof(src));
        std::memcpy(buffer.data(), &src, sizeof(src));

        RemoteGPU::HandshakeResponse dst{};
        std::memcpy(&dst, buffer.data(), sizeof(dst));

        TEST_ASSERT(dst.protocol_version == src.protocol_version);
        TEST_ASSERT(dst.status_code == src.status_code);
        TEST_ASSERT(dst.feature_level == src.feature_level);
        TEST_ASSERT(dst.frame_width == src.frame_width);
        TEST_ASSERT(dst.frame_height == src.frame_height);
        TEST_ASSERT(dst.dedicated_vram == src.dedicated_vram);
        TEST_ASSERT(std::strcmp(dst.adapter_desc, desc) == 0);
    }

    {
        RemoteGPU::CreateBufferPayload payload{};
        payload.res_id = 42;
        payload.byte_width = 64;
        payload.usage = 0;
        payload.bind_flags = 1;
        payload.cpu_access_flags = 0;
        payload.misc_flags = 0;
        payload.structure_byte_stride = 16;
        payload.has_initial_data = 1;
        payload.initial_data_size = 64;

        float vertex_data[16];
        for (int i = 0; i < 16; ++i) vertex_data[i] = static_cast<float>(i) * 1.5f;

        std::vector<uint8_t> packet_buf(sizeof(payload) + sizeof(vertex_data));
        std::memcpy(packet_buf.data(), &payload, sizeof(payload));
        std::memcpy(packet_buf.data() + sizeof(payload), vertex_data, sizeof(vertex_data));

        RemoteGPU::CreateBufferPayload recvd_payload{};
        std::memcpy(&recvd_payload, packet_buf.data(), sizeof(recvd_payload));
        float recvd_vertices[16];
        std::memcpy(recvd_vertices, packet_buf.data() + sizeof(recvd_payload), sizeof(recvd_vertices));

        TEST_ASSERT(recvd_payload.res_id == 42);
        TEST_ASSERT(recvd_payload.byte_width == 64);
        TEST_ASSERT(recvd_payload.has_initial_data == 1);
        TEST_ASSERT(std::memcmp(vertex_data, recvd_vertices, sizeof(vertex_data)) == 0);
    }
}

void Test_Socket_LoopbackConnection() {
    RemoteGPU::NetworkSocket listener;
    TEST_ASSERT(listener.BindAndListen(0, "127.0.0.1"));
    uint16_t port = listener.GetLocalPort();
    TEST_ASSERT(port > 0);

    std::unique_ptr<RemoteGPU::NetworkSocket> server_conn;
    std::thread server_thread([&]() {
        server_conn = listener.Accept(5000);
    });

    RemoteGPU::NetworkSocket client;
    bool connected = client.Connect("127.0.0.1", port, 5000);
    TEST_ASSERT_MSG(connected, "Client connected to loopback port");
    TEST_ASSERT(client.IsValid());

    server_thread.join();
    TEST_ASSERT(server_conn != nullptr);
    if (server_conn) {
        TEST_ASSERT(server_conn->IsValid());

        int nodelay = 0;
        int optlen = sizeof(nodelay);
        int opt_res = getsockopt(client.GetRawSocket(), IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, &optlen);
        TEST_ASSERT(opt_res == 0);
        TEST_ASSERT_MSG(nodelay == 1, "TCP_NODELAY is active on client socket");

        server_conn->Close();
    }
    client.Close();
    listener.Close();
}

void Test_Socket_DataTransfer() {
    RemoteGPU::NetworkSocket listener;
    TEST_ASSERT(listener.BindAndListen(0, "127.0.0.1"));
    uint16_t port = listener.GetLocalPort();

    std::unique_ptr<RemoteGPU::NetworkSocket> server_conn;
    std::thread server_thread([&]() {
        server_conn = listener.Accept(5000);
    });

    RemoteGPU::NetworkSocket client;
    TEST_ASSERT(client.Connect("127.0.0.1", port, 5000));
    server_thread.join();
    TEST_ASSERT(server_conn != nullptr);
    if (!server_conn) return;

    // Small command test
    {
        RemoteGPU::PacketHeader hdr{};
        hdr.magic = RemoteGPU::RGPU_MAGIC;
        hdr.opcode = RemoteGPU::OP_HANDSHAKE_REQUEST;
        hdr.flags = RemoteGPU::FLAG_NONE;
        hdr.sequence_id = 1;
        hdr.payload_length = sizeof(RemoteGPU::HandshakeRequest);

        RemoteGPU::HandshakeRequest req{};
        req.protocol_version = RemoteGPU::RGPU_PROTOCOL_VERSION;
        req.client_width = 800;
        req.client_height = 600;
        req.preferred_format = 28;

        TEST_ASSERT(client.SendPacket(hdr, &req));

        RemoteGPU::PacketHeader rcv_hdr{};
        std::vector<uint8_t> rcv_payload;
        TEST_ASSERT(server_conn->RecvPacket(rcv_hdr, rcv_payload));
        TEST_ASSERT(rcv_hdr.magic == RemoteGPU::RGPU_MAGIC);
        TEST_ASSERT(rcv_hdr.opcode == RemoteGPU::OP_HANDSHAKE_REQUEST);
        TEST_ASSERT(rcv_hdr.sequence_id == 1);
        TEST_ASSERT(rcv_hdr.payload_length == sizeof(RemoteGPU::HandshakeRequest));
        TEST_ASSERT(rcv_payload.size() == sizeof(RemoteGPU::HandshakeRequest));

        const auto* recvd_req = reinterpret_cast<const RemoteGPU::HandshakeRequest*>(rcv_payload.data());
        TEST_ASSERT(recvd_req->client_width == 800);
        TEST_ASSERT(recvd_req->client_height == 600);
    }

    // 4 MB Multi-Megabyte transfer
    {
        const size_t payload_size = 4 * 1024 * 1024; // 4 MB
        std::vector<uint8_t> large_payload(payload_size);
        for (size_t i = 0; i < payload_size; ++i) {
            large_payload[i] = static_cast<uint8_t>((i * 109 + 37) ^ (i >> 5));
        }

        RemoteGPU::PacketHeader hdr{};
        hdr.magic = RemoteGPU::RGPU_MAGIC;
        hdr.opcode = RemoteGPU::OP_FRAME_DATA;
        hdr.flags = RemoteGPU::FLAG_NONE;
        hdr.sequence_id = 2;
        hdr.payload_length = static_cast<uint32_t>(payload_size);

        auto t0 = std::chrono::high_resolution_clock::now();
        TEST_ASSERT(client.SendPacket(hdr, large_payload.data()));

        RemoteGPU::PacketHeader rcv_hdr{};
        std::vector<uint8_t> rcv_payload;
        TEST_ASSERT(server_conn->RecvPacket(rcv_hdr, rcv_payload));
        auto t1 = std::chrono::high_resolution_clock::now();

        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        double mb = static_cast<double>(payload_size) / (1024.0 * 1024.0);
        double sec = (elapsed_ms > 0) ? (static_cast<double>(elapsed_ms) / 1000.0) : 0.001;
        double speed_mb_s = mb / sec;

        std::cout << "  [PERF] 4 MB loopback transfer completed in " << elapsed_ms
                  << " ms (~" << static_cast<int>(speed_mb_s) << " MB/s)" << std::endl;

        TEST_ASSERT(rcv_hdr.magic == RemoteGPU::RGPU_MAGIC);
        TEST_ASSERT(rcv_hdr.opcode == RemoteGPU::OP_FRAME_DATA);
        TEST_ASSERT(rcv_hdr.sequence_id == 2);
        TEST_ASSERT(rcv_hdr.payload_length == payload_size);
        TEST_ASSERT(rcv_payload.size() == payload_size);

        int cmp_res = std::memcmp(large_payload.data(), rcv_payload.data(), payload_size);
        TEST_ASSERT_MSG(cmp_res == 0, "4 MB payload byte-by-byte integrity matches 100%");
    }

    server_conn->Close();
    client.Close();
    listener.Close();
}

void Test_Socket_DisconnectionAndErrors() {
    // 5A: Clean Disconnect
    {
        RemoteGPU::NetworkSocket listener;
        TEST_ASSERT(listener.BindAndListen(0, "127.0.0.1"));
        uint16_t port = listener.GetLocalPort();

        std::unique_ptr<RemoteGPU::NetworkSocket> server_conn;
        std::thread server_thread([&]() {
            server_conn = listener.Accept(5000);
        });

        RemoteGPU::NetworkSocket client;
        TEST_ASSERT(client.Connect("127.0.0.1", port, 5000));
        server_thread.join();
        TEST_ASSERT(server_conn != nullptr);

        client.Close();

        RemoteGPU::PacketHeader hdr{};
        std::vector<uint8_t> payload;
        bool recv_ok = server_conn->RecvPacket(hdr, payload);
        TEST_ASSERT_MSG(!recv_ok, "Server detected peer closure as clean EOF");

        server_conn->Close();
        listener.Close();
    }

    // 5B: Connect to closed port
    {
        RemoteGPU::NetworkSocket client;
        auto t0 = std::chrono::high_resolution_clock::now();
        bool connected = client.Connect("127.0.0.1", 48895, 500);
        auto t1 = std::chrono::high_resolution_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        TEST_ASSERT_MSG(!connected, "Connect to closed port returned false");
        TEST_ASSERT_MSG(!client.IsValid(), "Socket remains invalid after failed connect");
        TEST_ASSERT_MSG(elapsed_ms < 3000, "Connect failure returned promptly");
    }

    // 5C: Mid-stream disconnect
    {
        RemoteGPU::NetworkSocket listener;
        TEST_ASSERT(listener.BindAndListen(0, "127.0.0.1"));
        uint16_t port = listener.GetLocalPort();

        std::unique_ptr<RemoteGPU::NetworkSocket> server_conn;
        std::thread server_thread([&]() {
            server_conn = listener.Accept(5000);
        });

        RemoteGPU::NetworkSocket client;
        TEST_ASSERT(client.Connect("127.0.0.1", port, 5000));
        server_thread.join();
        TEST_ASSERT(server_conn != nullptr);

        uint8_t partial_hdr[8] = { 'R', 'G', 'P', 'U', 0x01, 0x00, 0x00, 0x00 };
        send(client.GetRawSocket(), reinterpret_cast<const char*>(partial_hdr), sizeof(partial_hdr), 0);
        client.Close();

        RemoteGPU::PacketHeader hdr{};
        std::vector<uint8_t> payload;
        bool recv_ok = server_conn->RecvPacket(hdr, payload);
        TEST_ASSERT_MSG(!recv_ok, "Server detected mid-stream disconnect on truncated header");

        server_conn->Close();
        listener.Close();
    }

    // 5D: Corrupted Magic
    {
        RemoteGPU::NetworkSocket listener;
        TEST_ASSERT(listener.BindAndListen(0, "127.0.0.1"));
        uint16_t port = listener.GetLocalPort();

        std::unique_ptr<RemoteGPU::NetworkSocket> server_conn;
        std::thread server_thread([&]() {
            server_conn = listener.Accept(5000);
        });

        RemoteGPU::NetworkSocket client;
        TEST_ASSERT(client.Connect("127.0.0.1", port, 5000));
        server_thread.join();
        TEST_ASSERT(server_conn != nullptr);

        RemoteGPU::PacketHeader bad_hdr{};
        bad_hdr.magic = 0xDEADBEEF;
        bad_hdr.opcode = 0x0005;
        bad_hdr.flags = RemoteGPU::FLAG_NONE;
        bad_hdr.sequence_id = 1;
        bad_hdr.payload_length = 0;

        send(client.GetRawSocket(), reinterpret_cast<const char*>(&bad_hdr), sizeof(bad_hdr), 0);

        RemoteGPU::PacketHeader hdr{};
        std::vector<uint8_t> payload;
        bool recv_ok = server_conn->RecvPacket(hdr, payload);
        TEST_ASSERT_MSG(!recv_ok, "Server rejected packet with invalid magic");

        server_conn->Close();
        client.Close();
        listener.Close();
    }
}

int main() {
    std::cout << "============================================================" << std::endl;
    std::cout << "  Remote GPU Offloading - Milestone 1 Transport Test Suite" << std::endl;
    std::cout << "============================================================" << std::endl;

    if (!RemoteGPU::NetworkSocket::InitializeNetwork()) {
        std::cerr << "[FATAL] Failed to initialize Winsock2 networking!" << std::endl;
        return 1;
    }

    auto total_start = std::chrono::high_resolution_clock::now();

    RUN_TEST_SUITE(Test_PacketHeader_PackingAndConstants);
    RUN_TEST_SUITE(Test_Payload_SerializationRoundTrip);
    RUN_TEST_SUITE(Test_Socket_LoopbackConnection);
    RUN_TEST_SUITE(Test_Socket_DataTransfer);
    RUN_TEST_SUITE(Test_Socket_DisconnectionAndErrors);

    auto total_end = std::chrono::high_resolution_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(total_end - total_start).count();

    RemoteGPU::NetworkSocket::ShutdownNetwork();

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
