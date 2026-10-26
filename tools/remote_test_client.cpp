#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <fstream>
#include <cstring>

#include "protocol.h"
#include "transport.h"

using namespace RemoteGPU;

struct ColoredVertex {
    float x, y, z;
    float r, g, b, a;
};

// Simple BMP writer (32-bit RGBA / BGRA)
static bool SaveBMP(const std::string& filename, int width, int height, const uint8_t* rgba) {
    std::ofstream file(filename, std::ios::binary);
    if (!file) return false;

    uint32_t imageSize = width * height * 4;
    uint32_t fileSize = 54 + imageSize;
    uint8_t header[54] = {
        'B', 'M',
        static_cast<uint8_t>(fileSize & 0xFF), static_cast<uint8_t>((fileSize >> 8) & 0xFF),
        static_cast<uint8_t>((fileSize >> 16) & 0xFF), static_cast<uint8_t>((fileSize >> 24) & 0xFF),
        0, 0, 0, 0,
        54, 0, 0, 0, // offset
        40, 0, 0, 0, // header size
        static_cast<uint8_t>(width & 0xFF), static_cast<uint8_t>((width >> 8) & 0xFF),
        static_cast<uint8_t>((width >> 16) & 0xFF), static_cast<uint8_t>((width >> 24) & 0xFF),
        static_cast<uint8_t>(height & 0xFF), static_cast<uint8_t>((height >> 8) & 0xFF),
        static_cast<uint8_t>((height >> 16) & 0xFF), static_cast<uint8_t>((height >> 24) & 0xFF),
        1, 0, // planes
        32, 0, // bits per pixel
        0, 0, 0, 0, // uncompressed
        static_cast<uint8_t>(imageSize & 0xFF), static_cast<uint8_t>((imageSize >> 8) & 0xFF),
        static_cast<uint8_t>((imageSize >> 16) & 0xFF), static_cast<uint8_t>((imageSize >> 24) & 0xFF),
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };

    file.write(reinterpret_cast<const char*>(header), 54);
    // BMP stores rows bottom-to-top, in BGRA format
    for (int y = height - 1; y >= 0; --y) {
        for (int x = 0; x < width; ++x) {
            size_t idx = (y * width + x) * 4;
            uint8_t r = rgba[idx + 0];
            uint8_t g = rgba[idx + 1];
            uint8_t b = rgba[idx + 2];
            uint8_t a = rgba[idx + 3];
            uint8_t bgra[4] = { b, g, r, a };
            file.write(reinterpret_cast<const char*>(bgra), 4);
        }
    }
    return true;
}

int main(int argc, char* argv[]) {
    if (!NetworkSocket::InitializeNetwork()) {
        std::cerr << "[Client Error] Failed to initialize Winsock!" << std::endl;
        return 1;
    }
    std::string host = (argc > 1) ? argv[1] : "192.168.1.17";
    uint16_t port = (argc > 2) ? static_cast<uint16_t>(std::stoi(argv[2])) : 48888;
    uint32_t width = (argc > 3) ? static_cast<uint32_t>(std::stoi(argv[3])) : 512;
    uint32_t height = (argc > 4) ? static_cast<uint32_t>(std::stoi(argv[4])) : 512;

    std::cout << "============================================================" << std::endl;
    std::cout << "  Remote GPU Offloading Test Client" << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << "[Client] Target Server Address : " << host << ":" << port << std::endl;
    std::cout << "[Client] Requested Resolution  : " << width << "x" << height << std::endl;

    NetworkSocket client;
    std::cout << "[Client] Connecting to remote host..." << std::endl;
    auto t0 = std::chrono::high_resolution_clock::now();
    if (!client.Connect(host.c_str(), port, 5000)) {
        std::cerr << "[Client Error] Failed to connect to " << host << ":" << port << std::endl;
        return 1;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double conn_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << "[Client] Connected in " << conn_ms << " ms!" << std::endl;

    // 1. Handshake
    std::cout << "[Client] Sending OP_HANDSHAKE_REQUEST..." << std::endl;
    HandshakeRequestPayload req{};
    req.protocol_version = RGPU_PROTOCOL_VERSION;
    req.client_flags = FLAG_NONE;
    req.client_width = width;
    req.client_height = height;
    req.preferred_format = 28; // DXGI_FORMAT_R8G8B8A8_UNORM

    PacketHeader req_hdr{};
    req_hdr.magic = RGPU_MAGIC;
    req_hdr.opcode = OP_HANDSHAKE_REQUEST;
    req_hdr.flags = FLAG_NONE;
    req_hdr.sequence_id = 1;
    req_hdr.payload_length = sizeof(req);

    if (!client.SendPacket(req_hdr, &req)) {
        std::cerr << "[Client Error] Failed to send handshake!" << std::endl;
        return 1;
    }

    PacketHeader resp_hdr{};
    std::vector<uint8_t> resp_payload;
    if (!client.RecvPacket(resp_hdr, resp_payload)) {
        std::cerr << "[Client Error] Handshake response timed out!" << std::endl;
        return 1;
    }

    if (resp_hdr.opcode != OP_HANDSHAKE_RESPONSE || resp_payload.size() < sizeof(HandshakeResponsePayload)) {
        std::cerr << "[Client Error] Invalid handshake response opcode=" << resp_hdr.opcode << std::endl;
        return 1;
    }

    const auto* resp = reinterpret_cast<const HandshakeResponsePayload*>(resp_payload.data());
    std::cout << "[Client] Handshake Successful!" << std::endl;
    std::cout << "[Client]   Remote Server Protocol Version : " << resp->protocol_version << std::endl;
    std::cout << "[Client]   Negotiated Frame Dimensions    : " << resp->frame_width << "x" << resp->frame_height << std::endl;
    std::cout << "[Client]   Remote GPU Adapter Name        : " << resp->adapter_desc << std::endl;
    std::cout << "[Client]   Remote Dedicated VRAM          : " << (resp->dedicated_vram / (1024 * 1024)) << " MB" << std::endl;

    // 2. Compile shaders locally
    std::cout << "[Client] Compiling HLSL shaders..." << std::endl;
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

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vsBlob, nullptr);
    D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &psBlob, nullptr);

    if (!vsBlob || !psBlob) {
        std::cerr << "[Client Error] Failed to compile HLSL shaders!" << std::endl;
        return 1;
    }

    // 3. Create Vertex Shader on Remote Server
    {
        CreateShaderPayload p{ 101, static_cast<uint32_t>(vsBlob->GetBufferSize()) };
        std::vector<uint8_t> pl(sizeof(p) + p.bytecode_length);
        std::memcpy(pl.data(), &p, sizeof(p));
        std::memcpy(pl.data() + sizeof(p), vsBlob->GetBufferPointer(), p.bytecode_length);
        PacketHeader h{ RGPU_MAGIC, OP_CREATE_VERTEX_SHADER, FLAG_NONE, 2, static_cast<uint32_t>(pl.size()) };
        client.SendPacket(h, pl.data());
    }

    // 4. Create Pixel Shader on Remote Server
    {
        CreateShaderPayload p{ 102, static_cast<uint32_t>(psBlob->GetBufferSize()) };
        std::vector<uint8_t> pl(sizeof(p) + p.bytecode_length);
        std::memcpy(pl.data(), &p, sizeof(p));
        std::memcpy(pl.data() + sizeof(p), psBlob->GetBufferPointer(), p.bytecode_length);
        PacketHeader h{ RGPU_MAGIC, OP_CREATE_PIXEL_SHADER, FLAG_NONE, 3, static_cast<uint32_t>(pl.size()) };
        client.SendPacket(h, pl.data());
    }

    // 5. Create Input Layout on Remote Server
    {
        CreateInputLayoutPayload p{ 103, 2, static_cast<uint32_t>(vsBlob->GetBufferSize()) };
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
        client.SendPacket(h, pl.data());
    }

    // 6. Create Vertex Buffer with Vibrant Triangle
    {
        ColoredVertex verts[3] = {
            {  0.0f,  0.6f, 0.5f,  1.0f, 0.1f, 0.1f, 1.0f }, // Top Red
            {  0.6f, -0.6f, 0.5f,  0.1f, 1.0f, 0.1f, 1.0f }, // Right Green
            { -0.6f, -0.6f, 0.5f,  0.1f, 0.1f, 1.0f, 1.0f }  // Left Blue
        };
        CreateBufferPayload p{};
        p.res_id = 104;
        p.byte_width = sizeof(verts);
        p.bind_flags = 1; // D3D11_BIND_VERTEX_BUFFER
        p.has_initial_data = 1;
        p.initial_data_size = sizeof(verts);

        std::vector<uint8_t> pl(sizeof(p) + sizeof(verts));
        std::memcpy(pl.data(), &p, sizeof(p));
        std::memcpy(pl.data() + sizeof(p), verts, sizeof(verts));
        PacketHeader h{ RGPU_MAGIC, OP_CREATE_BUFFER, FLAG_NONE, 5, static_cast<uint32_t>(pl.size()) };
        client.SendPacket(h, pl.data());
    }

    // 7. Bind Pipeline State
    {
        IASetInputLayoutPayload p{ 103 };
        PacketHeader h{ RGPU_MAGIC, OP_IA_SET_INPUT_LAYOUT, FLAG_NONE, 6, sizeof(p) };
        client.SendPacket(h, &p);
    }
    {
        IASetVertexBufferPayload p{ 0, 104, sizeof(ColoredVertex), 0 };
        PacketHeader h{ RGPU_MAGIC, OP_IA_SET_VERTEX_BUFFER, FLAG_NONE, 7, sizeof(p) };
        client.SendPacket(h, &p);
    }
    {
        IASetTopologyPayload p{ D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST };
        PacketHeader h{ RGPU_MAGIC, OP_IA_SET_TOPOLOGY, FLAG_NONE, 8, sizeof(p) };
        client.SendPacket(h, &p);
    }
    {
        VSSetShaderPayload p{ 101 };
        PacketHeader h{ RGPU_MAGIC, OP_VS_SET_SHADER, FLAG_NONE, 9, sizeof(p) };
        client.SendPacket(h, &p);
    }
    {
        PSSetShaderPayload p{ 102 };
        PacketHeader h{ RGPU_MAGIC, OP_PS_SET_SHADER, FLAG_NONE, 10, sizeof(p) };
        client.SendPacket(h, &p);
    }
    {
        RSSetViewportPayload p{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
        PacketHeader h{ RGPU_MAGIC, OP_RS_SET_VIEWPORT, FLAG_NONE, 11, sizeof(p) };
        client.SendPacket(h, &p);
    }
    {
        OMSetRenderTargetsPayload p{ 0, 0 };
        PacketHeader h{ RGPU_MAGIC, OP_OM_SET_RENDER_TARGETS, FLAG_NONE, 12, sizeof(p) };
        client.SendPacket(h, &p);
    }

    // 8. Clear RTV & Draw
    std::cout << "[Client] Offloading Draw call to remote GPU..." << std::endl;
    {
        ClearRtvPayload p{};
        p.res_id = 0; // Default offscreen backbuffer RTV
        p.color[0] = 0.10f; p.color[1] = 0.05f; p.color[2] = 0.20f; p.color[3] = 1.0f; // Deep violet
        PacketHeader h{ RGPU_MAGIC, OP_CLEAR_RTV, FLAG_NONE, 13, sizeof(p) };
        client.SendPacket(h, &p);
    }
    {
        ClearDsvPayload p{};
        p.res_id = 0; // Default DSV
        p.clear_flags = D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL;
        p.depth = 1.0f;
        p.stencil = 0;
        PacketHeader h{ RGPU_MAGIC, OP_CLEAR_DSV, FLAG_NONE, 14, sizeof(p) };
        client.SendPacket(h, &p);
    }
    {
        DrawPayload p{ 3, 0 };
        PacketHeader h{ RGPU_MAGIC, OP_DRAW, FLAG_NONE, 15, sizeof(p) };
        client.SendPacket(h, &p);
    }

    // 9. Present & Readback
    std::cout << "[Client] Requesting frame execution & readback (OP_PRESENT)..." << std::endl;
    auto t_render_start = std::chrono::high_resolution_clock::now();
    {
        PresentPayload p{ 0, 0, 0, 1 };
        PacketHeader h{ RGPU_MAGIC, OP_PRESENT, FLAG_NONE, 14, sizeof(p) };
        client.SendPacket(h, &p);
    }

    // 10. Receive Frame Data
    PacketHeader frame_hdr{};
    std::vector<uint8_t> frame_payload;
    if (!client.RecvPacket(frame_hdr, frame_payload)) {
        std::cerr << "[Client Error] Failed to receive frame data!" << std::endl;
        return 1;
    }
    auto t_render_end = std::chrono::high_resolution_clock::now();
    double frame_rtt_ms = std::chrono::duration<double, std::milli>(t_render_end - t_render_start).count();

    if (frame_hdr.opcode != OP_FRAME_DATA || frame_payload.size() < sizeof(FrameDataPayload)) {
        std::cerr << "[Client Error] Received unexpected packet opcode=" << frame_hdr.opcode << std::endl;
        return 1;
    }

    const auto* frame_meta = reinterpret_cast<const FrameDataPayload*>(frame_payload.data());
    const uint8_t* pixels = frame_payload.data() + sizeof(FrameDataPayload);

    std::cout << "[Client] *** FRAME RECEIVED FROM REMOTE GPU! ***" << std::endl;
    std::cout << "[Client]   Frame Resolution   : " << frame_meta->width << "x" << frame_meta->height << std::endl;
    std::cout << "[Client]   Frame Data Payload : " << frame_meta->data_size << " bytes (" << (frame_meta->data_size / (1024.0 * 1024.0)) << " MB)" << std::endl;
    std::cout << "[Client]   Full Round-Trip    : " << frame_rtt_ms << " ms" << std::endl;

    // Save image
    std::string bmp_path = "rtx4070_render.bmp";
    if (SaveBMP(bmp_path, frame_meta->width, frame_meta->height, pixels)) {
        std::cout << "[Client] Successfully saved rendered image to: " << bmp_path << std::endl;
    }

    // 11. Graceful Shutdown
    ShutdownPayload shut{ 0 };
    PacketHeader shut_hdr{ RGPU_MAGIC, OP_SHUTDOWN, FLAG_NONE, 15, sizeof(shut) };
    client.SendPacket(shut_hdr, &shut);
    client.Close();
    NetworkSocket::ShutdownNetwork();

    std::cout << "============================================================" << std::endl;
    std::cout << "  SUCCESS: Direct3D 11 Offloading to Remote RTX 4070 Complete!" << std::endl;
    std::cout << "============================================================" << std::endl;

    return 0;
}
