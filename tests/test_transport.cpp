// ============================================================================
// Remote GPU Offload - Milestone 1 Transport Layer Unit Test Suite
// File: tests/test_transport.cpp
// Standard: C++17
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

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
#include <future>
#include <atomic>

// ----------------------------------------------------------------------------
// Lightweight Zero-Dependency Test Harness
// ----------------------------------------------------------------------------
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

// ----------------------------------------------------------------------------
// Suite 1: Packet Header Packing, Alignment, and Constants
// ----------------------------------------------------------------------------
void Test_PacketHeader_PackingAndConstants() {
    // 1. Fixed size check
    static_assert(sizeof(RemoteGPU::PacketHeader) == 16, "PacketHeader must be exactly 16 bytes");
    TEST_ASSERT_MSG(sizeof(RemoteGPU::PacketHeader) == 16, "PacketHeader size == 16");

    // 2. Member offset checks
    TEST_ASSERT(offsetof(RemoteGPU::PacketHeader, magic) == 0);
    TEST_ASSERT(offsetof(RemoteGPU::PacketHeader, opcode) == 4);
    TEST_ASSERT(offsetof(RemoteGPU::PacketHeader, flags) == 6);
    TEST_ASSERT(offsetof(RemoteGPU::PacketHeader, sequence_id) == 8);
    TEST_ASSERT(offsetof(RemoteGPU::PacketHeader, payload_length) == 12);

    // 3. Member size checks
    TEST_ASSERT(sizeof(RemoteGPU::PacketHeader::magic) == 4);
    TEST_ASSERT(sizeof(RemoteGPU::PacketHeader::opcode) == 2);
    TEST_ASSERT(sizeof(RemoteGPU::PacketHeader::flags) == 2);
    TEST_ASSERT(sizeof(RemoteGPU::PacketHeader::sequence_id) == 4);
    TEST_ASSERT(sizeof(RemoteGPU::PacketHeader::payload_length) == 4);

    // 4. Magic constant check (0x52475055 == 'RGPU')
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

    // 5. Version and default port checks
    TEST_ASSERT(RemoteGPU::RGPU_PROTOCOL_VERSION == 1);
    TEST_ASSERT(RemoteGPU::RGPU_DEFAULT_PORT == 48888);

    // 6. Flags bitwise orthogonality
    TEST_ASSERT(RemoteGPU::FLAG_NONE == 0x0000);
    TEST_ASSERT((RemoteGPU::FLAG_SYNC & RemoteGPU::FLAG_RESPONSE_REQUIRED) == 0);
    TEST_ASSERT((RemoteGPU::FLAG_COMPRESSED & RemoteGPU::FLAG_ONEWAY) == 0);
    TEST_ASSERT((RemoteGPU::FLAG_END_OF_FRAME & RemoteGPU::FLAG_HEARTBEAT) == 0);

    // 7. Opcode enumeration completeness (all 33 distinct opcodes)
    std::vector<uint16_t> opcodes = {
        RemoteGPU::OP_HANDSHAKE_REQUEST,
        RemoteGPU::OP_HANDSHAKE_RESPONSE,
        RemoteGPU::OP_SHUTDOWN,
        RemoteGPU::OP_CREATE_BUFFER,
        RemoteGPU::OP_CREATE_TEXTURE2D,
        RemoteGPU::OP_CREATE_SRV,
        RemoteGPU::OP_CREATE_RTV,
        RemoteGPU::OP_CREATE_DSV,
        RemoteGPU::OP_CREATE_VERTEX_SHADER,
        RemoteGPU::OP_CREATE_PIXEL_SHADER,
        RemoteGPU::OP_CREATE_INPUT_LAYOUT,
        RemoteGPU::OP_DESTROY_RESOURCE,
        RemoteGPU::OP_CREATE_SAMPLER,
        RemoteGPU::OP_IA_SET_INPUT_LAYOUT,
        RemoteGPU::OP_IA_SET_VERTEX_BUFFER,
        RemoteGPU::OP_IA_SET_INDEX_BUFFER,
        RemoteGPU::OP_IA_SET_TOPOLOGY,
        RemoteGPU::OP_VS_SET_SHADER,
        RemoteGPU::OP_VS_SET_CB,
        RemoteGPU::OP_PS_SET_SHADER,
        RemoteGPU::OP_PS_SET_CB,
        RemoteGPU::OP_PS_SET_SRV,
        RemoteGPU::OP_RS_SET_VIEWPORT,
        RemoteGPU::OP_OM_SET_RENDER_TARGETS,
        RemoteGPU::OP_PS_SET_SAMPLER,
        RemoteGPU::OP_UPDATE_SUBRESOURCE,
        RemoteGPU::OP_WRITE_BUFFER_DISCARD,
        RemoteGPU::OP_CLEAR_RTV,
        RemoteGPU::OP_CLEAR_DSV,
        RemoteGPU::OP_DRAW,
        RemoteGPU::OP_DRAW_INDEXED,
        RemoteGPU::OP_PRESENT,
        RemoteGPU::OP_FRAME_DATA
    };
    TEST_ASSERT_MSG(opcodes.size() == 33, "Exactly 33 opcodes defined");
    for (size_t i = 0; i < opcodes.size(); ++i) {
        TEST_ASSERT(opcodes[i] > 0);
        for (size_t j = i + 1; j < opcodes.size(); ++j) {
            TEST_ASSERT(opcodes[i] != opcodes[j]);
        }
    }
}

// ----------------------------------------------------------------------------
// Suite 2: Payload Struct Serialization Round-Trip
// ----------------------------------------------------------------------------
void Test_Payload_SerializationRoundTrip() {
    // 1. Handshake Request
    {
        RemoteGPU::HandshakeRequest src{};
        src.protocol_version = RemoteGPU::RGPU_PROTOCOL_VERSION;
        src.client_flags = RemoteGPU::FLAG_NONE;
        src.client_width = 1920;
        src.client_height = 1080;
        src.preferred_format = 28; // DXGI_FORMAT_R8G8B8A8_UNORM

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

    // 2. Handshake Response
    {
        RemoteGPU::HandshakeResponse src{};
        src.protocol_version = RemoteGPU::RGPU_PROTOCOL_VERSION;
        src.status_code = 0;
        src.feature_level = 0xb000; // D3D_FEATURE_LEVEL_11_0
        src.frame_width = 1920;
        src.frame_height = 1080;
        src.frame_format = 28;
        src.dedicated_vram = 8589934592ULL; // 8 GB
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

    // 3. CreateBufferPayload with trailing vertex data
    {
        RemoteGPU::CreateBufferPayload payload{};
        payload.res_id = 42;
        payload.byte_width = 64;
        payload.usage = 0;
        payload.bind_flags = 1; // VERTEX_BUFFER
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

    // 4. CreateTexture2DPayload
    {
        RemoteGPU::CreateTexture2DPayload src{};
        src.res_id = 7;
        src.width = 256;
        src.height = 256;
        src.mips = 1;
        src.array_size = 1;
        src.format = 28;
        src.sample_count = 1;
        src.sample_quality = 0;
        src.usage = 0;
        src.bind_flags = 8; // SHADER_RESOURCE
        src.cpu_access_flags = 0;
        src.misc_flags = 0;
        src.has_initial_data = 0;
        src.sys_mem_pitch = 1024;
        src.sys_mem_slice_pitch = 262144;
        src.initial_data_size = 0;

        std::vector<uint8_t> buffer(sizeof(src));
        std::memcpy(buffer.data(), &src, sizeof(src));

        RemoteGPU::CreateTexture2DPayload dst{};
        std::memcpy(&dst, buffer.data(), sizeof(dst));

        TEST_ASSERT(dst.res_id == src.res_id);
        TEST_ASSERT(dst.width == src.width);
        TEST_ASSERT(dst.height == src.height);
        TEST_ASSERT(dst.format == src.format);
        TEST_ASSERT(dst.sys_mem_pitch == src.sys_mem_pitch);
    }

    // 5. CreateShaderPayload with synthetic bytecode
    {
        RemoteGPU::CreateShaderPayload payload{};
        payload.res_id = 15;
        payload.bytecode_length = 256;

        std::vector<uint8_t> bytecode(256);
        for (size_t i = 0; i < bytecode.size(); ++i) bytecode[i] = static_cast<uint8_t>(i ^ 0xAA);

        std::vector<uint8_t> buffer(sizeof(payload) + bytecode.size());
        std::memcpy(buffer.data(), &payload, sizeof(payload));
        std::memcpy(buffer.data() + sizeof(payload), bytecode.data(), bytecode.size());

        RemoteGPU::CreateShaderPayload dst{};
        std::memcpy(&dst, buffer.data(), sizeof(dst));
        std::vector<uint8_t> dst_bytecode(dst.bytecode_length);
        std::memcpy(dst_bytecode.data(), buffer.data() + sizeof(dst), dst.bytecode_length);

        TEST_ASSERT(dst.res_id == 15);
        TEST_ASSERT(dst.bytecode_length == 256);
        TEST_ASSERT(std::memcmp(bytecode.data(), dst_bytecode.data(), 256) == 0);
    }

    // 6. CreateInputLayoutPayload with elements
    {
        RemoteGPU::CreateInputLayoutPayload payload{};
        payload.res_id = 22;
        payload.num_elements = 2;
        payload.bytecode_length = 64;

        RemoteGPU::RgpuInputElementDesc elements[2]{};
        std::strncpy(elements[0].semantic_name, "POSITION", sizeof(elements[0].semantic_name) - 1);
        elements[0].semantic_index = 0;
        elements[0].format = 6; // DXGI_FORMAT_R32G32B32_FLOAT
        elements[0].input_slot = 0;
        elements[0].aligned_byte_offset = 0;
        elements[0].input_slot_class = 0;
        elements[0].instance_data_step = 0;

        std::strncpy(elements[1].semantic_name, "COLOR", sizeof(elements[1].semantic_name) - 1);
        elements[1].semantic_index = 0;
        elements[1].format = 2; // DXGI_FORMAT_R32G32B32A32_FLOAT
        elements[1].input_slot = 0;
        elements[1].aligned_byte_offset = 12;
        elements[1].input_slot_class = 0;
        elements[1].instance_data_step = 0;

        std::vector<uint8_t> buffer(sizeof(payload) + sizeof(elements));
        std::memcpy(buffer.data(), &payload, sizeof(payload));
        std::memcpy(buffer.data() + sizeof(payload), elements, sizeof(elements));

        RemoteGPU::CreateInputLayoutPayload dst{};
        std::memcpy(&dst, buffer.data(), sizeof(dst));
        RemoteGPU::RgpuInputElementDesc dst_elems[2]{};
        std::memcpy(dst_elems, buffer.data() + sizeof(dst), sizeof(dst_elems));

        TEST_ASSERT(dst.res_id == 22);
        TEST_ASSERT(dst.num_elements == 2);
        TEST_ASSERT(std::strcmp(dst_elems[0].semantic_name, "POSITION") == 0);
        TEST_ASSERT(dst_elems[0].format == 6);
        TEST_ASSERT(std::strcmp(dst_elems[1].semantic_name, "COLOR") == 0);
        TEST_ASSERT(dst_elems[1].aligned_byte_offset == 12);
    }

    // 7. UpdateSubresourcePayload with box
    {
        RemoteGPU::UpdateSubresourcePayload src{};
        src.res_id = 9;
        src.subresource = 0;
        src.has_dst_box = 1;
        src.dst_box = { 0, 0, 0, 100, 100, 1 };
        src.row_pitch = 400;
        src.depth_pitch = 40000;
        src.data_size = 40000;

        std::vector<uint8_t> buffer(sizeof(src));
        std::memcpy(buffer.data(), &src, sizeof(src));

        RemoteGPU::UpdateSubresourcePayload dst{};
        std::memcpy(&dst, buffer.data(), sizeof(dst));

        TEST_ASSERT(dst.res_id == src.res_id);
        TEST_ASSERT(dst.has_dst_box == 1);
        TEST_ASSERT(dst.dst_box.right == 100);
        TEST_ASSERT(dst.row_pitch == 400);
        TEST_ASSERT(dst.data_size == 40000);
    }

    // 8. WriteBufferDiscardPayload
    {
        RemoteGPU::WriteBufferDiscardPayload src{};
        src.res_id = 3;
        src.byte_offset = 0;
        src.byte_width = 128;

        std::vector<uint8_t> buffer(sizeof(src));
        std::memcpy(buffer.data(), &src, sizeof(src));

        RemoteGPU::WriteBufferDiscardPayload dst{};
        std::memcpy(&dst, buffer.data(), sizeof(dst));

        TEST_ASSERT(dst.res_id == 3);
        TEST_ASSERT(dst.byte_offset == 0);
        TEST_ASSERT(dst.byte_width == 128);
    }

    // 9. DrawPayload and DrawIndexedPayload
    {
        RemoteGPU::DrawPayload draw_src{};
        draw_src.vertex_count = 36;
        draw_src.start_vertex_location = 0;

        std::vector<uint8_t> buf1(sizeof(draw_src));
        std::memcpy(buf1.data(), &draw_src, sizeof(draw_src));
        RemoteGPU::DrawPayload draw_dst{};
        std::memcpy(&draw_dst, buf1.data(), sizeof(draw_dst));

        TEST_ASSERT(draw_dst.vertex_count == 36);
        TEST_ASSERT(draw_dst.start_vertex_location == 0);

        RemoteGPU::DrawIndexedPayload idx_src{};
        idx_src.index_count = 36;
        idx_src.start_index_location = 0;
        idx_src.base_vertex_location = 10;

        std::vector<uint8_t> buf2(sizeof(idx_src));
        std::memcpy(buf2.data(), &idx_src, sizeof(idx_src));
        RemoteGPU::DrawIndexedPayload idx_dst{};
        std::memcpy(&idx_dst, buf2.data(), sizeof(idx_dst));

        TEST_ASSERT(idx_dst.index_count == 36);
        TEST_ASSERT(idx_dst.start_index_location == 0);
        TEST_ASSERT(idx_dst.base_vertex_location == 10);
    }

    // 10. ClearRtvPayload and ClearDsvPayload
    {
        RemoteGPU::ClearRtvPayload rtv_src{};
        rtv_src.res_id = 100;
        rtv_src.color[0] = 0.2f;
        rtv_src.color[1] = 0.4f;
        rtv_src.color[2] = 0.6f;
        rtv_src.color[3] = 1.0f;

        std::vector<uint8_t> buf1(sizeof(rtv_src));
        std::memcpy(buf1.data(), &rtv_src, sizeof(rtv_src));
        RemoteGPU::ClearRtvPayload rtv_dst{};
        std::memcpy(&rtv_dst, buf1.data(), sizeof(rtv_dst));

        TEST_ASSERT(rtv_dst.res_id == 100);
        TEST_ASSERT(rtv_dst.color[0] == 0.2f);
        TEST_ASSERT(rtv_dst.color[1] == 0.4f);
        TEST_ASSERT(rtv_dst.color[2] == 0.6f);
        TEST_ASSERT(rtv_dst.color[3] == 1.0f);

        RemoteGPU::ClearDsvPayload dsv_src{};
        dsv_src.res_id = 200;
        dsv_src.clear_flags = 1; // DEPTH
        dsv_src.depth = 1.0f;
        dsv_src.stencil = 0;

        std::vector<uint8_t> buf2(sizeof(dsv_src));
        std::memcpy(buf2.data(), &dsv_src, sizeof(dsv_src));
        RemoteGPU::ClearDsvPayload dsv_dst{};
        std::memcpy(&dsv_dst, buf2.data(), sizeof(dsv_dst));

        TEST_ASSERT(dsv_dst.res_id == 200);
        TEST_ASSERT(dsv_dst.clear_flags == 1);
        TEST_ASSERT(dsv_dst.depth == 1.0f);
        TEST_ASSERT(dsv_dst.stencil == 0);
    }

    // 11. PresentPayload and FrameDataPayload
    {
        RemoteGPU::PresentPayload pres_src{};
        pres_src.swapchain_id = 1;
        pres_src.sync_interval = 1;
        pres_src.flags = 0;
        pres_src.frame_number = 60;

        std::vector<uint8_t> buf1(sizeof(pres_src));
        std::memcpy(buf1.data(), &pres_src, sizeof(pres_src));
        RemoteGPU::PresentPayload pres_dst{};
        std::memcpy(&pres_dst, buf1.data(), sizeof(pres_dst));

        TEST_ASSERT(pres_dst.swapchain_id == 1);
        TEST_ASSERT(pres_dst.sync_interval == 1);
        TEST_ASSERT(pres_dst.frame_number == 60);

        RemoteGPU::FrameDataPayload frame_src{};
        frame_src.swapchain_id = 1;
        frame_src.width = 800;
        frame_src.height = 600;
        frame_src.format = 28;
        frame_src.row_pitch = 3200;
        frame_src.data_size = 1920000;
        frame_src.frame_number = 60;

        std::vector<uint8_t> buf2(sizeof(frame_src));
        std::memcpy(buf2.data(), &frame_src, sizeof(frame_src));
        RemoteGPU::FrameDataPayload frame_dst{};
        std::memcpy(&frame_dst, buf2.data(), sizeof(frame_dst));

        TEST_ASSERT(frame_dst.swapchain_id == 1);
        TEST_ASSERT(frame_dst.width == 800);
        TEST_ASSERT(frame_dst.height == 600);
        TEST_ASSERT(frame_dst.format == 28);
        TEST_ASSERT(frame_dst.row_pitch == 3200);
        TEST_ASSERT(frame_dst.data_size == 1920000);
        TEST_ASSERT(frame_dst.frame_number == 60);
    }
}

// ----------------------------------------------------------------------------
// Suite 3: Socket Loopback Connection Test
// ----------------------------------------------------------------------------
void Test_Socket_LoopbackConnection() {
    uint16_t target_port = RemoteGPU::RGPU_DEFAULT_PORT; // 48888
    std::promise<uint16_t> port_promise;
    auto port_future = port_promise.get_future();

    std::unique_ptr<RemoteGPU::NetworkSocket> server_conn;
    std::thread server_thread([&]() {
        RemoteGPU::NetworkSocket listener;
        // Attempt default port 48888; if in use, fall back to ephemeral port 0
        if (!listener.BindAndListen(target_port, "127.0.0.1")) {
            if (!listener.BindAndListen(0, "127.0.0.1")) {
                port_promise.set_value(0);
                return;
            }
        }
        uint16_t actual_port = listener.GetLocalPort();
        port_promise.set_value(actual_port);
        server_conn = listener.Accept(5000);
    });

    uint16_t bound_port = port_future.get();
    TEST_ASSERT_MSG(bound_port != 0, "Server successfully bound to loopback port");

    if (bound_port != 0) {
        RemoteGPU::NetworkSocket client;
        bool connected = client.Connect("127.0.0.1", bound_port, 5000);
        TEST_ASSERT_MSG(connected, "Client successfully connected to loopback listener");
        TEST_ASSERT(client.IsValid());

        server_thread.join();

        TEST_ASSERT_MSG(server_conn != nullptr, "Server successfully accepted incoming connection");
        if (server_conn) {
            TEST_ASSERT(server_conn->IsValid());

            // Check TCP_NODELAY option on client socket
            int nodelay = 0;
            int optlen = sizeof(nodelay);
            int opt_res = getsockopt(client.GetSocket(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&nodelay), &optlen);
            TEST_ASSERT(opt_res == 0);
            TEST_ASSERT_MSG(nodelay == 1, "TCP_NODELAY is active on client socket");

            // Check SO_SNDBUF and SO_RCVBUF
            int snd_buf = 0, rcv_buf = 0;
            optlen = sizeof(snd_buf);
            getsockopt(client.GetSocket(), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&snd_buf), &optlen);
            getsockopt(client.GetSocket(), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&rcv_buf), &optlen);
            TEST_ASSERT_MSG(snd_buf >= 1048576, "Socket send buffer expanded (>= 1 MB)");
            TEST_ASSERT_MSG(rcv_buf >= 1048576, "Socket recv buffer expanded (>= 1 MB)");

            server_conn->Close();
        }
        client.Close();
    } else {
        if (server_thread.joinable()) server_thread.join();
    }
}

// ----------------------------------------------------------------------------
// Suite 4: SendPacket & RecvPacket Loopback Transfer Integrity
// ----------------------------------------------------------------------------
void Test_Socket_DataTransfer() {
    std::promise<uint16_t> port_promise;
    auto port_future = port_promise.get_future();

    std::unique_ptr<RemoteGPU::NetworkSocket> server_conn;
    std::thread server_thread([&]() {
        RemoteGPU::NetworkSocket listener;
        if (!listener.BindAndListen(0, "127.0.0.1")) {
            port_promise.set_value(0);
            return;
        }
        port_promise.set_value(listener.GetLocalPort());
        server_conn = listener.Accept(5000);
    });

    uint16_t bound_port = port_future.get();
    TEST_ASSERT(bound_port != 0);

    RemoteGPU::NetworkSocket client;
    bool connected = client.Connect("127.0.0.1", bound_port, 5000);
    TEST_ASSERT(connected);
    server_thread.join();
    TEST_ASSERT(server_conn != nullptr);

    if (!connected || !server_conn) return;

    // --- Sub-Test 4A: Small Graphics Command Sequence ---
    {
        // 1. Handshake Request
        RemoteGPU::PacketHeader hdr{};
        hdr.magic = RemoteGPU::RGPU_MAGIC;
        hdr.opcode = RemoteGPU::OP_HANDSHAKE_REQUEST;
        hdr.flags = RemoteGPU::FLAG_NONE;
        hdr.sequence_id = 1;
        hdr.payload_length = sizeof(RemoteGPU::HandshakeRequest);

        RemoteGPU::HandshakeRequest req{};
        req.protocol_version = RemoteGPU::RGPU_PROTOCOL_VERSION;
        req.client_flags = RemoteGPU::FLAG_NONE;
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

        // 2. CreateBuffer with 128B trailing vertex data
        struct PackedBufferMsg {
            RemoteGPU::CreateBufferPayload meta;
            float vertices[32]; // 128 bytes
        } buf_msg{};
        buf_msg.meta.res_id = 10;
        buf_msg.meta.byte_width = 128;
        buf_msg.meta.usage = 0;
        buf_msg.meta.bind_flags = 1;
        buf_msg.meta.has_initial_data = 1;
        buf_msg.meta.initial_data_size = 128;
        for (int i = 0; i < 32; ++i) buf_msg.vertices[i] = static_cast<float>(i) * 2.5f;

        hdr.opcode = RemoteGPU::OP_CREATE_BUFFER;
        hdr.sequence_id = 2;
        hdr.payload_length = sizeof(buf_msg);
        TEST_ASSERT(client.SendPacket(hdr, &buf_msg));

        TEST_ASSERT(server_conn->RecvPacket(rcv_hdr, rcv_payload));
        TEST_ASSERT(rcv_hdr.sequence_id == 2);
        TEST_ASSERT(rcv_payload.size() == sizeof(buf_msg));
        const auto* recvd_buf = reinterpret_cast<const PackedBufferMsg*>(rcv_payload.data());
        TEST_ASSERT(recvd_buf->meta.res_id == 10);
        TEST_ASSERT(std::memcmp(buf_msg.vertices, recvd_buf->vertices, sizeof(buf_msg.vertices)) == 0);

        // 3. ClearRenderTargetView
        RemoteGPU::ClearRtvPayload clear_rtv{};
        clear_rtv.res_id = 1;
        clear_rtv.color[0] = 0.1f;
        clear_rtv.color[1] = 0.2f;
        clear_rtv.color[2] = 0.3f;
        clear_rtv.color[3] = 1.0f;

        hdr.opcode = RemoteGPU::OP_CLEAR_RTV;
        hdr.sequence_id = 3;
        hdr.payload_length = sizeof(clear_rtv);
        TEST_ASSERT(client.SendPacket(hdr, &clear_rtv));

        TEST_ASSERT(server_conn->RecvPacket(rcv_hdr, rcv_payload));
        TEST_ASSERT(rcv_hdr.sequence_id == 3);
        const auto* recvd_clear = reinterpret_cast<const RemoteGPU::ClearRtvPayload*>(rcv_payload.data());
        TEST_ASSERT(recvd_clear->res_id == 1);
        TEST_ASSERT(recvd_clear->color[2] == 0.3f);

        // 4. DrawIndexed
        RemoteGPU::DrawIndexedPayload draw_idx{};
        draw_idx.index_count = 36;
        draw_idx.start_index_location = 0;
        draw_idx.base_vertex_location = 0;

        hdr.opcode = RemoteGPU::OP_DRAW_INDEXED;
        hdr.sequence_id = 4;
        hdr.payload_length = sizeof(draw_idx);
        TEST_ASSERT(client.SendPacket(hdr, &draw_idx));

        TEST_ASSERT(server_conn->RecvPacket(rcv_hdr, rcv_payload));
        TEST_ASSERT(rcv_hdr.sequence_id == 4);
        const auto* recvd_draw = reinterpret_cast<const RemoteGPU::DrawIndexedPayload*>(rcv_payload.data());
        TEST_ASSERT(recvd_draw->index_count == 36);

        // 5. Present
        RemoteGPU::PresentPayload present{};
        present.swapchain_id = 0;
        present.sync_interval = 1;
        present.flags = 0;
        present.frame_number = 1;

        hdr.opcode = RemoteGPU::OP_PRESENT;
        hdr.sequence_id = 5;
        hdr.payload_length = sizeof(present);
        TEST_ASSERT(client.SendPacket(hdr, &present));

        TEST_ASSERT(server_conn->RecvPacket(rcv_hdr, rcv_payload));
        TEST_ASSERT(rcv_hdr.sequence_id == 5);
        const auto* recvd_pres = reinterpret_cast<const RemoteGPU::PresentPayload*>(rcv_payload.data());
        TEST_ASSERT(recvd_pres->frame_number == 1);
    }

    // --- Sub-Test 4B: Large Multi-Megabyte Payload (4 MB Transfer) ---
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
        hdr.sequence_id = 6;
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
        TEST_ASSERT(rcv_hdr.sequence_id == 6);
        TEST_ASSERT(rcv_hdr.payload_length == payload_size);
        TEST_ASSERT(rcv_payload.size() == payload_size);

        // Byte-by-byte integrity verification
        int cmp_res = std::memcmp(large_payload.data(), rcv_payload.data(), payload_size);
        TEST_ASSERT_MSG(cmp_res == 0, "4 MB payload byte-by-byte integrity matches 100%");
    }

    // --- Sub-Test 4C: Back-to-Back Burst Stream (50 consecutive packets) ---
    {
        const int num_burst = 50;
        for (int i = 0; i < num_burst; ++i) {
            RemoteGPU::PacketHeader hdr{};
            hdr.magic = RemoteGPU::RGPU_MAGIC;
            hdr.opcode = RemoteGPU::OP_DRAW_INDEXED;
            hdr.flags = RemoteGPU::FLAG_NONE;
            hdr.sequence_id = 100 + i;
            hdr.payload_length = sizeof(RemoteGPU::DrawIndexedPayload);

            RemoteGPU::DrawIndexedPayload draw{};
            draw.index_count = static_cast<uint32_t>(i + 1);
            draw.start_index_location = 0;
            draw.base_vertex_location = 0;

            TEST_ASSERT(client.SendPacket(hdr, &draw));
        }

        for (int i = 0; i < num_burst; ++i) {
            RemoteGPU::PacketHeader rcv_hdr{};
            std::vector<uint8_t> rcv_payload;
            TEST_ASSERT(server_conn->RecvPacket(rcv_hdr, rcv_payload));
            TEST_ASSERT(rcv_hdr.sequence_id == static_cast<uint32_t>(100 + i));
            TEST_ASSERT(rcv_hdr.opcode == RemoteGPU::OP_DRAW_INDEXED);
            const auto* draw = reinterpret_cast<const RemoteGPU::DrawIndexedPayload*>(rcv_payload.data());
            TEST_ASSERT(draw->index_count == static_cast<uint32_t>(i + 1));
        }
    }

    server_conn->Close();
    client.Close();
}

// ----------------------------------------------------------------------------
// Suite 5: Peer Disconnection and Error Handling
// ----------------------------------------------------------------------------
void Test_Socket_DisconnectionAndErrors() {
    // --- Sub-Test 5A: Clean Disconnect / Peer Close Detection ---
    {
        std::promise<uint16_t> port_promise;
        auto port_future = port_promise.get_future();

        std::unique_ptr<RemoteGPU::NetworkSocket> server_conn;
        std::thread server_thread([&]() {
            RemoteGPU::NetworkSocket listener;
            if (!listener.BindAndListen(0, "127.0.0.1")) {
                port_promise.set_value(0);
                return;
            }
            port_promise.set_value(listener.GetLocalPort());
            server_conn = listener.Accept(5000);
        });

        uint16_t port = port_future.get();
        TEST_ASSERT(port != 0);

        RemoteGPU::NetworkSocket client;
        TEST_ASSERT(client.Connect("127.0.0.1", port, 5000));
        server_thread.join();
        TEST_ASSERT(server_conn != nullptr);

        // Client explicitly closes connection
        client.Close();

        // Server calls RecvPacket -> must return false (clean EOF) without hanging or crashing
        RemoteGPU::PacketHeader hdr{};
        std::vector<uint8_t> payload;
        bool recv_ok = server_conn->RecvPacket(hdr, payload);
        TEST_ASSERT_MSG(!recv_ok, "Server detected peer closure as clean EOF (returned false)");

        server_conn->Close();
    }

    // --- Sub-Test 5B: Connection Attempt to Unbound / Closed Port ---
    {
        // Bind to ephemeral port to get an OS-assigned port, then close it immediately
        uint16_t closed_port = 0;
        {
            RemoteGPU::NetworkSocket temp;
            if (temp.BindAndListen(0, "127.0.0.1")) {
                closed_port = temp.GetLocalPort();
            }
            // temp destructor closes socket, leaving closed_port definitely unbound
        }
        if (closed_port == 0) closed_port = 48895;

        RemoteGPU::NetworkSocket client;
        auto t0 = std::chrono::high_resolution_clock::now();
        bool connected = client.Connect("127.0.0.1", closed_port, 1000);
        auto t1 = std::chrono::high_resolution_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        TEST_ASSERT_MSG(!connected, "Connect to closed port returned false");
        TEST_ASSERT_MSG(!client.IsValid(), "Socket remains invalid after failed connect");
        TEST_ASSERT_MSG(elapsed_ms < 3000, "Connect failure returned promptly without infinite hang");
    }

    // --- Sub-Test 5C: Truncated Stream / Mid-Stream Disconnect ---
    {
        std::promise<uint16_t> port_promise;
        auto port_future = port_promise.get_future();

        std::unique_ptr<RemoteGPU::NetworkSocket> server_conn;
        std::thread server_thread([&]() {
            RemoteGPU::NetworkSocket listener;
            if (!listener.BindAndListen(0, "127.0.0.1")) {
                port_promise.set_value(0);
                return;
            }
            port_promise.set_value(listener.GetLocalPort());
            server_conn = listener.Accept(5000);
        });

        uint16_t port = port_future.get();
        TEST_ASSERT(port != 0);

        RemoteGPU::NetworkSocket client;
        TEST_ASSERT(client.Connect("127.0.0.1", port, 5000));
        server_thread.join();
        TEST_ASSERT(server_conn != nullptr);

        // Send only 8 bytes of a 16-byte header
        uint8_t partial_hdr[8] = { 'R', 'G', 'P', 'U', 0x01, 0x00, 0x00, 0x00 };
        send(client.GetSocket(), reinterpret_cast<const char*>(partial_hdr), sizeof(partial_hdr), 0);

        // Immediately close the client socket
        client.Close();

        // Server calls RecvPacket -> must detect premature EOF and return false
        RemoteGPU::PacketHeader hdr{};
        std::vector<uint8_t> payload;
        bool recv_ok = server_conn->RecvPacket(hdr, payload);
        TEST_ASSERT_MSG(!recv_ok, "Server detected mid-stream disconnect on truncated header");

        server_conn->Close();
    }

    // --- Sub-Test 5D: Corrupted Magic Rejection ---
    {
        std::promise<uint16_t> port_promise;
        auto port_future = port_promise.get_future();

        std::unique_ptr<RemoteGPU::NetworkSocket> server_conn;
        std::thread server_thread([&]() {
            RemoteGPU::NetworkSocket listener;
            if (!listener.BindAndListen(0, "127.0.0.1")) {
                port_promise.set_value(0);
                return;
            }
            port_promise.set_value(listener.GetLocalPort());
            server_conn = listener.Accept(5000);
        });

        uint16_t port = port_future.get();
        TEST_ASSERT(port != 0);

        RemoteGPU::NetworkSocket client;
        TEST_ASSERT(client.Connect("127.0.0.1", port, 5000));
        server_thread.join();
        TEST_ASSERT(server_conn != nullptr);

        // Send invalid magic
        RemoteGPU::PacketHeader bad_hdr{};
        bad_hdr.magic = 0xDEADBEEF; // Invalid magic
        bad_hdr.opcode = RemoteGPU::OP_HANDSHAKE_REQUEST;
        bad_hdr.flags = RemoteGPU::FLAG_NONE;
        bad_hdr.sequence_id = 1;
        bad_hdr.payload_length = 0;

        send(client.GetSocket(), reinterpret_cast<const char*>(&bad_hdr), sizeof(bad_hdr), 0);

        // Server calls RecvPacket -> should detect bad magic and return false
        RemoteGPU::PacketHeader hdr{};
        std::vector<uint8_t> payload;
        bool recv_ok = server_conn->RecvPacket(hdr, payload);
        TEST_ASSERT_MSG(!recv_ok, "Server rejected packet with invalid magic");

        server_conn->Close();
        client.Close();
    }
}

// ----------------------------------------------------------------------------
// Test Main Entrypoint
// ----------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    std::cout << "============================================================" << std::endl;
    std::cout << "  Remote GPU Offloading - Milestone 1 Transport Test Suite" << std::endl;
    std::cout << "============================================================" << std::endl;

    // 1. Initialize Winsock2
    if (!RemoteGPU::NetworkSocket::InitializeNetwork()) {
        std::cerr << "[FATAL] Failed to initialize Winsock2 networking!" << std::endl;
        return 1;
    }

    auto total_start = std::chrono::high_resolution_clock::now();

    // 2. Execute All Test Suites
    RUN_TEST_SUITE(Test_PacketHeader_PackingAndConstants);
    RUN_TEST_SUITE(Test_Payload_SerializationRoundTrip);
    RUN_TEST_SUITE(Test_Socket_LoopbackConnection);
    RUN_TEST_SUITE(Test_Socket_DataTransfer);
    RUN_TEST_SUITE(Test_Socket_DisconnectionAndErrors);

    auto total_end = std::chrono::high_resolution_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(total_end - total_start).count();

    // 3. Shutdown Winsock2
    RemoteGPU::NetworkSocket::ShutdownNetwork();

    // 4. Print Summary Report
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
