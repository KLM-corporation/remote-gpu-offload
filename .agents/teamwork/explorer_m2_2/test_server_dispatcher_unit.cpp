#include "proposed_server_dispatcher.h"
#include "proposed_gpu_device.h"
#include "transport.h"
#include "protocol.h"

#include <d3dcompiler.h>
#include <iostream>
#include <vector>
#include <cassert>
#include <cmath>
#include <thread>
#include <chrono>

using namespace RemoteGPU;

// Simple HLSL Shaders for Unit Testing
static const char* g_TestHlsl = R"(
cbuffer TestCBuffer : register(b0) {
    float4 u_ColorMod;
};

struct VS_INPUT {
    float3 pos : POSITION;
    float4 col : COLOR;
    float2 uv  : TEXCOORD0;
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float4 col : COLOR;
    float2 uv  : TEXCOORD0;
};

PS_INPUT VSMain(VS_INPUT input) {
    PS_INPUT output;
    output.pos = float4(input.pos, 1.0f);
    output.col = input.col * u_ColorMod;
    output.uv  = input.uv;
    return output;
}

Texture2D g_Tex : register(t0);
SamplerState g_Samp : register(s0);

float4 PSMain(PS_INPUT input) : SV_Target {
    float4 texColor = g_Tex.Sample(g_Samp, input.uv);
    return texColor * input.col;
}
)";

struct Vertex {
    float x, y, z;
    float r, g, b, a;
    float u, v;
};

static void AssertCheck(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[TEST FAIL] " << message << std::endl;
        std::exit(1);
    }
    std::cout << "[TEST PASS] " << message << std::endl;
}

int main() {
    std::cout << "============================================================" << std::endl;
    std::cout << "[Test] Server Resource Manager & Dispatcher Comprehensive Unit Test" << std::endl;
    std::cout << "============================================================" << std::endl;

    // 1. Initialize Winsock and HostGpuDevice
    AssertCheck(NetworkSocket::InitializeNetwork(), "InitializeNetwork");

    HostGpuDevice gpu;
    AssertCheck(gpu.InitializeDedicatedGpu(), "InitializeDedicatedGpu on host");
    AssertCheck(gpu.CreateOffscreenTargets(256, 256), "CreateOffscreenTargets (256x256)");

    ID3D11Device* device = gpu.GetDevice();
    ID3D11DeviceContext* context = gpu.GetContext();
    AssertCheck(device != nullptr && context != nullptr, "D3D11 device and context are valid");

    // 2. Test ServerResourceManager in isolation
    std::cout << "\n--- Testing ServerResourceManager ---" << std::endl;
    ServerResourceManager resMgr;
    AssertCheck(resMgr.Count() == 0, "Initial resource count is 0");

    // Create a temporary D3D11Buffer to register
    D3D11_BUFFER_DESC bdesc = {};
    bdesc.ByteWidth = 64;
    bdesc.Usage = D3D11_USAGE_DEFAULT;
    bdesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    Microsoft::WRL::ComPtr<ID3D11Buffer> testBuffer;
    HRESULT hr = device->CreateBuffer(&bdesc, nullptr, &testBuffer);
    AssertCheck(SUCCEEDED(hr), "CreateBuffer for resource manager test");

    resMgr.Register(101, testBuffer);
    AssertCheck(resMgr.Count() == 1, "Resource count is 1 after registration");
    AssertCheck(resMgr.HasResource(101), "HasResource(101) == true");
    AssertCheck(!resMgr.HasResource(999), "HasResource(999) == false");

    // Retrieve via typed getters
    Microsoft::WRL::ComPtr<ID3D11Buffer> retrievedBuf = resMgr.GetBuffer(101);
    AssertCheck(retrievedBuf.Get() == testBuffer.Get(), "GetBuffer(101) returns identical COM pointer");

    // Type safety check: requesting texture from a buffer ID must safely return nullptr
    Microsoft::WRL::ComPtr<ID3D11Texture2D> wrongType = resMgr.GetTexture2D(101);
    AssertCheck(wrongType == nullptr, "GetTexture2D(101) safely returns nullptr (COM QueryInterface rejected)");

    // Destroy resource
    AssertCheck(resMgr.Destroy(101), "Destroy(101) succeeded");
    AssertCheck(!resMgr.HasResource(101), "HasResource(101) is false after destroy");
    AssertCheck(resMgr.Count() == 0, "Resource count is 0 after destroy");

    // 3. Test ServerDispatcher Command Handling
    std::cout << "\n--- Testing ServerDispatcher Command Handling ---" << std::endl;
    ServerDispatcher dispatcher(gpu);
    AssertCheck(!dispatcher.IsHandshakeCompleted(), "Handshake is initially false");

    // Test Handshake Request
    PacketHeader hs_hdr = {};
    hs_hdr.magic = RGPU_MAGIC;
    hs_hdr.opcode = OP_HANDSHAKE_REQUEST;
    hs_hdr.sequence_id = 1;
    hs_hdr.payload_length = sizeof(HandshakeRequestPayload);

    HandshakeRequestPayload hs_req = {};
    hs_req.protocol_version = RGPU_PROTOCOL_VERSION;
    hs_req.client_width = 256;
    hs_req.client_height = 256;
    hs_req.preferred_format = 28; // R8G8B8A8_UNORM

    AssertCheck(dispatcher.DispatchCommand(hs_hdr, reinterpret_cast<const uint8_t*>(&hs_req), sizeof(hs_req)),
                "Dispatch OP_HANDSHAKE_REQUEST");
    AssertCheck(dispatcher.IsHandshakeCompleted(), "Handshake completed successfully");

    // Compile HLSL Shaders via D3DCompile
    std::cout << "\n--- Compiling Test HLSL Shaders ---" << std::endl;
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errBlob;

    hr = D3DCompile(g_TestHlsl, std::strlen(g_TestHlsl), "TestHLSL", nullptr, nullptr,
                    "VSMain", "vs_4_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr) && errBlob) {
        std::cerr << "VS Compile Error: " << (char*)errBlob->GetBufferPointer() << std::endl;
    }
    AssertCheck(SUCCEEDED(hr), "D3DCompile VSMain (vs_4_0)");

    hr = D3DCompile(g_TestHlsl, std::strlen(g_TestHlsl), "TestHLSL", nullptr, nullptr,
                    "PSMain", "ps_4_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr) && errBlob) {
        std::cerr << "PS Compile Error: " << (char*)errBlob->GetBufferPointer() << std::endl;
    }
    AssertCheck(SUCCEEDED(hr), "D3DCompile PSMain (ps_4_0)");

    // Test OP_CREATE_VERTEX_SHADER
    uint32_t vs_id = 10;
    std::vector<uint8_t> vs_payload(sizeof(CreateShaderPayload) + vsBlob->GetBufferSize());
    auto* vs_p = reinterpret_cast<CreateShaderPayload*>(vs_payload.data());
    vs_p->res_id = vs_id;
    vs_p->bytecode_length = static_cast<uint32_t>(vsBlob->GetBufferSize());
    std::memcpy(vs_payload.data() + sizeof(CreateShaderPayload), vsBlob->GetBufferPointer(), vsBlob->GetBufferSize());

    PacketHeader cmd_hdr = {};
    cmd_hdr.magic = RGPU_MAGIC;
    cmd_hdr.sequence_id = 2;

    cmd_hdr.opcode = OP_CREATE_VERTEX_SHADER;
    cmd_hdr.payload_length = static_cast<uint32_t>(vs_payload.size());
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, vs_payload.data(), vs_payload.size()),
                "Dispatch OP_CREATE_VERTEX_SHADER");
    AssertCheck(dispatcher.GetResourceManager().GetVertexShader(vs_id) != nullptr, "VertexShader COM object created");

    // Test OP_CREATE_PIXEL_SHADER
    uint32_t ps_id = 11;
    std::vector<uint8_t> ps_payload(sizeof(CreateShaderPayload) + psBlob->GetBufferSize());
    auto* ps_p = reinterpret_cast<CreateShaderPayload*>(ps_payload.data());
    ps_p->res_id = ps_id;
    ps_p->bytecode_length = static_cast<uint32_t>(psBlob->GetBufferSize());
    std::memcpy(ps_payload.data() + sizeof(CreateShaderPayload), psBlob->GetBufferPointer(), psBlob->GetBufferSize());

    cmd_hdr.opcode = OP_CREATE_PIXEL_SHADER;
    cmd_hdr.payload_length = static_cast<uint32_t>(ps_payload.size());
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, ps_payload.data(), ps_payload.size()),
                "Dispatch OP_CREATE_PIXEL_SHADER");
    AssertCheck(dispatcher.GetResourceManager().GetPixelShader(ps_id) != nullptr, "PixelShader COM object created");

    // Test OP_CREATE_INPUT_LAYOUT
    uint32_t il_id = 12;
    RgpuInputElementDesc elements[3] = {};
    // POSITION
    std::strcpy(elements[0].semantic_name, "POSITION");
    elements[0].semantic_index = 0;
    elements[0].format = 6; // DXGI_FORMAT_R32G32B32_FLOAT
    elements[0].input_slot = 0;
    elements[0].aligned_byte_offset = 0;
    // COLOR
    std::strcpy(elements[1].semantic_name, "COLOR");
    elements[1].semantic_index = 0;
    elements[1].format = 2; // DXGI_FORMAT_R32G32B32A32_FLOAT
    elements[1].input_slot = 0;
    elements[1].aligned_byte_offset = 12;
    // TEXCOORD
    std::strcpy(elements[2].semantic_name, "TEXCOORD");
    elements[2].semantic_index = 0;
    elements[2].format = 16; // DXGI_FORMAT_R32G32_FLOAT
    elements[2].input_slot = 0;
    elements[2].aligned_byte_offset = 28;

    size_t il_payload_size = sizeof(CreateInputLayoutPayload) + (3 * sizeof(RgpuInputElementDesc)) + vsBlob->GetBufferSize();
    std::vector<uint8_t> il_payload(il_payload_size);
    auto* il_p = reinterpret_cast<CreateInputLayoutPayload*>(il_payload.data());
    il_p->res_id = il_id;
    il_p->num_elements = 3;
    il_p->bytecode_length = static_cast<uint32_t>(vsBlob->GetBufferSize());

    std::memcpy(il_payload.data() + sizeof(CreateInputLayoutPayload), elements, sizeof(elements));
    std::memcpy(il_payload.data() + sizeof(CreateInputLayoutPayload) + sizeof(elements),
                vsBlob->GetBufferPointer(), vsBlob->GetBufferSize());

    cmd_hdr.opcode = OP_CREATE_INPUT_LAYOUT;
    cmd_hdr.payload_length = static_cast<uint32_t>(il_payload.size());
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, il_payload.data(), il_payload.size()),
                "Dispatch OP_CREATE_INPUT_LAYOUT");
    AssertCheck(dispatcher.GetResourceManager().GetInputLayout(il_id) != nullptr, "InputLayout COM object created");

    // Test OP_CREATE_BUFFER: Vertex Buffer with 3 vertices (Triangle)
    uint32_t vb_id = 20;
    Vertex triangle_vertices[3] = {
        {  0.0f,  0.5f, 0.0f,  1.0f, 0.0f, 0.0f, 1.0f,  0.5f, 0.0f }, // Top (Red)
        {  0.5f, -0.5f, 0.0f,  0.0f, 1.0f, 0.0f, 1.0f,  1.0f, 1.0f }, // Bottom-Right (Green)
        { -0.5f, -0.5f, 0.0f,  0.0f, 0.0f, 1.0f, 1.0f,  0.0f, 1.0f }  // Bottom-Left (Blue)
    };

    size_t vb_data_size = sizeof(triangle_vertices);
    std::vector<uint8_t> vb_payload(sizeof(CreateBufferPayload) + vb_data_size);
    auto* vb_p = reinterpret_cast<CreateBufferPayload*>(vb_payload.data());
    vb_p->res_id = vb_id;
    vb_p->byte_width = static_cast<uint32_t>(vb_data_size);
    vb_p->usage = 0; // D3D11_USAGE_DEFAULT
    vb_p->bind_flags = 1; // D3D11_BIND_VERTEX_BUFFER
    vb_p->cpu_access_flags = 0;
    vb_p->structure_byte_stride = sizeof(Vertex);
    vb_p->has_initial_data = 1;
    vb_p->initial_data_size = static_cast<uint32_t>(vb_data_size);
    std::memcpy(vb_payload.data() + sizeof(CreateBufferPayload), triangle_vertices, vb_data_size);

    cmd_hdr.opcode = OP_CREATE_BUFFER;
    cmd_hdr.payload_length = static_cast<uint32_t>(vb_payload.size());
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, vb_payload.data(), vb_payload.size()),
                "Dispatch OP_CREATE_BUFFER (Vertex Buffer with initial data)");
    AssertCheck(dispatcher.GetResourceManager().GetBuffer(vb_id) != nullptr, "VertexBuffer COM object created");

    // Test OP_CREATE_BUFFER: Dynamic Constant Buffer
    uint32_t cb_id = 21;
    CreateBufferPayload cb_p = {};
    cb_p.res_id = cb_id;
    cb_p.byte_width = 16; // 4 floats
    cb_p.usage = 2; // D3D11_USAGE_DYNAMIC
    cb_p.bind_flags = 4; // D3D11_BIND_CONSTANT_BUFFER
    cb_p.cpu_access_flags = 0x10000; // D3D11_CPU_ACCESS_WRITE
    cb_p.has_initial_data = 0;

    cmd_hdr.opcode = OP_CREATE_BUFFER;
    cmd_hdr.payload_length = sizeof(cb_p);
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, reinterpret_cast<const uint8_t*>(&cb_p), sizeof(cb_p)),
                "Dispatch OP_CREATE_BUFFER (Dynamic Constant Buffer)");
    AssertCheck(dispatcher.GetResourceManager().GetBuffer(cb_id) != nullptr, "ConstantBuffer COM object created");

    // Test OP_CREATE_TEXTURE2D: 4x4 Checkerboard Texture
    uint32_t tex_id = 30;
    uint32_t tex_pixels[16] = {
        0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
        0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
        0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
        0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF
    }; // Solid white texture so triangle colors show purely
    size_t tex_data_size = sizeof(tex_pixels);
    std::vector<uint8_t> tex_payload(sizeof(CreateTexture2DPayload) + tex_data_size);
    auto* tex_p = reinterpret_cast<CreateTexture2DPayload*>(tex_payload.data());
    tex_p->res_id = tex_id;
    tex_p->width = 4;
    tex_p->height = 4;
    tex_p->mips = 1;
    tex_p->array_size = 1;
    tex_p->format = 28; // DXGI_FORMAT_R8G8B8A8_UNORM
    tex_p->sample_count = 1;
    tex_p->usage = 0; // D3D11_USAGE_DEFAULT
    tex_p->bind_flags = 8; // D3D11_BIND_SHADER_RESOURCE
    tex_p->has_initial_data = 1;
    tex_p->sys_mem_pitch = 4 * sizeof(uint32_t);
    tex_p->initial_data_size = static_cast<uint32_t>(tex_data_size);
    std::memcpy(tex_payload.data() + sizeof(CreateTexture2DPayload), tex_pixels, tex_data_size);

    cmd_hdr.opcode = OP_CREATE_TEXTURE2D;
    cmd_hdr.payload_length = static_cast<uint32_t>(tex_payload.size());
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, tex_payload.data(), tex_payload.size()),
                "Dispatch OP_CREATE_TEXTURE2D");
    AssertCheck(dispatcher.GetResourceManager().GetTexture2D(tex_id) != nullptr, "Texture2D COM object created");

    // Test OP_CREATE_SRV
    uint32_t srv_id = 31;
    CreateSrvPayload srv_p = {};
    srv_p.res_id = srv_id;
    srv_p.resource_id = tex_id;
    srv_p.format = 28;
    srv_p.view_dimension = 4; // D3D11_SRV_DIMENSION_TEXTURE2D
    srv_p.most_detailed_mip = 0;
    srv_p.mip_levels = 1;

    cmd_hdr.opcode = OP_CREATE_SRV;
    cmd_hdr.payload_length = sizeof(srv_p);
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, reinterpret_cast<const uint8_t*>(&srv_p), sizeof(srv_p)),
                "Dispatch OP_CREATE_SRV");
    AssertCheck(dispatcher.GetResourceManager().GetSRV(srv_id) != nullptr, "SRV COM object created");

    // Test OP_CREATE_SAMPLER
    uint32_t samp_id = 32;
    CreateSamplerPayload samp_p = {};
    samp_p.res_id = samp_id;
    samp_p.filter = 0x15; // MIN_MAG_MIP_LINEAR
    samp_p.address_u = 1; // CLAMP
    samp_p.address_v = 1;
    samp_p.address_w = 1;
    samp_p.comparison_func = 8; // ALWAYS
    samp_p.min_lod = 0.0f;
    samp_p.max_lod = 3.402823466e+38f;

    cmd_hdr.opcode = OP_CREATE_SAMPLER;
    cmd_hdr.payload_length = sizeof(samp_p);
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, reinterpret_cast<const uint8_t*>(&samp_p), sizeof(samp_p)),
                "Dispatch OP_CREATE_SAMPLER");
    AssertCheck(dispatcher.GetResourceManager().GetSamplerState(samp_id) != nullptr, "Sampler COM object created");

    // 4. Test Pipeline Binding Opcodes
    std::cout << "\n--- Testing Pipeline State Binding Opcodes ---" << std::endl;

    // Test OP_CREATE_RTV on default backbuffer (resource_id = 0)
    uint32_t rtv_id = 1;
    CreateRtvPayload rtv_p = {};
    rtv_p.res_id = rtv_id;
    rtv_p.resource_id = 0; // default backbuffer
    rtv_p.format = 28;
    rtv_p.view_dimension = 4;
    cmd_hdr.opcode = OP_CREATE_RTV;
    cmd_hdr.payload_length = sizeof(rtv_p);
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, reinterpret_cast<const uint8_t*>(&rtv_p), sizeof(rtv_p)),
                "Dispatch OP_CREATE_RTV (default backbuffer)");
    AssertCheck(dispatcher.GetResourceManager().GetRTV(rtv_id) != nullptr, "RTV COM object created and registered");

    // Test OP_CREATE_DSV on default depth buffer (resource_id = 0)
    uint32_t dsv_id = 2;
    CreateDsvPayload dsv_p = {};
    dsv_p.res_id = dsv_id;
    dsv_p.resource_id = 0; // default depth
    dsv_p.format = 45; // DXGI_FORMAT_D24_UNORM_S8_UINT
    dsv_p.view_dimension = 3;
    cmd_hdr.opcode = OP_CREATE_DSV;
    cmd_hdr.payload_length = sizeof(dsv_p);
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, reinterpret_cast<const uint8_t*>(&dsv_p), sizeof(dsv_p)),
                "Dispatch OP_CREATE_DSV (default depth buffer)");
    AssertCheck(dispatcher.GetResourceManager().GetDSV(dsv_id) != nullptr, "DSV COM object created and registered");

    // Viewport
    RSSetViewportPayload vp_p = { 0.0f, 0.0f, 256.0f, 256.0f, 0.0f, 1.0f };
    cmd_hdr.opcode = OP_RS_SET_VIEWPORT;
    cmd_hdr.payload_length = sizeof(vp_p);
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, reinterpret_cast<const uint8_t*>(&vp_p), sizeof(vp_p)),
                "Dispatch OP_RS_SET_VIEWPORT");

    // OM Set Render Targets (bind registered rtv_id and dsv_id)
    OMSetRenderTargetsPayload om_p = { rtv_id, dsv_id };
    cmd_hdr.opcode = OP_OM_SET_RENDER_TARGETS;
    cmd_hdr.payload_length = sizeof(om_p);
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, reinterpret_cast<const uint8_t*>(&om_p), sizeof(om_p)),
                "Dispatch OP_OM_SET_RENDER_TARGETS");

    // Clear RTV to Cornflower Blue: RGBA(100, 149, 237, 255) -> (0.392f, 0.584f, 0.929f, 1.0f)
    ClearRtvPayload clear_p = { 0, { 100.0f / 255.0f, 149.0f / 255.0f, 237.0f / 255.0f, 1.0f } };
    cmd_hdr.opcode = OP_CLEAR_RTV;
    cmd_hdr.payload_length = sizeof(clear_p);
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, reinterpret_cast<const uint8_t*>(&clear_p), sizeof(clear_p)),
                "Dispatch OP_CLEAR_RTV (Cornflower Blue)");

    // Clear DSV
    ClearDsvPayload clear_dsv_p = {};
    clear_dsv_p.res_id = 0;
    clear_dsv_p.clear_flags = 1 | 2; // DEPTH | STENCIL
    clear_dsv_p.depth = 1.0f;
    clear_dsv_p.stencil = 0;
    cmd_hdr.opcode = OP_CLEAR_DSV;
    cmd_hdr.payload_length = sizeof(clear_dsv_p);
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, reinterpret_cast<const uint8_t*>(&clear_dsv_p), sizeof(clear_dsv_p)),
                "Dispatch OP_CLEAR_DSV");

    // Input Layout
    IASetInputLayoutPayload ia_il_p = { il_id };
    cmd_hdr.opcode = OP_IA_SET_INPUT_LAYOUT;
    cmd_hdr.payload_length = sizeof(ia_il_p);
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, reinterpret_cast<const uint8_t*>(&ia_il_p), sizeof(ia_il_p)),
                "Dispatch OP_IA_SET_INPUT_LAYOUT");

    // Vertex Buffer
    IASetVertexBufferPayload ia_vb_p = { 0, vb_id, sizeof(Vertex), 0 };
    cmd_hdr.opcode = OP_IA_SET_VERTEX_BUFFER;
    cmd_hdr.payload_length = sizeof(ia_vb_p);
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, reinterpret_cast<const uint8_t*>(&ia_vb_p), sizeof(ia_vb_p)),
                "Dispatch OP_IA_SET_VERTEX_BUFFER");

    // Primitive Topology
    IASetTopologyPayload ia_top_p = { 4 }; // D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
    cmd_hdr.opcode = OP_IA_SET_TOPOLOGY;
    cmd_hdr.payload_length = sizeof(ia_top_p);
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, reinterpret_cast<const uint8_t*>(&ia_top_p), sizeof(ia_top_p)),
                "Dispatch OP_IA_SET_TOPOLOGY (TRIANGLELIST)");

    // Shaders
    VSSetShaderPayload vs_set_p = { vs_id };
    cmd_hdr.opcode = OP_VS_SET_SHADER;
    cmd_hdr.payload_length = sizeof(vs_set_p);
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, reinterpret_cast<const uint8_t*>(&vs_set_p), sizeof(vs_set_p)),
                "Dispatch OP_VS_SET_SHADER");

    PSSetShaderPayload ps_set_p = { ps_id };
    cmd_hdr.opcode = OP_PS_SET_SHADER;
    cmd_hdr.payload_length = sizeof(ps_set_p);
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, reinterpret_cast<const uint8_t*>(&ps_set_p), sizeof(ps_set_p)),
                "Dispatch OP_PS_SET_SHADER");

    // Constant Buffer to VS
    VSSetConstantBufferPayload vs_cb_p = { 0, cb_id };
    cmd_hdr.opcode = OP_VS_SET_CB;
    cmd_hdr.payload_length = sizeof(vs_cb_p);
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, reinterpret_cast<const uint8_t*>(&vs_cb_p), sizeof(vs_cb_p)),
                "Dispatch OP_VS_SET_CB");

    // SRV & Sampler to PS
    PSSetSrvPayload ps_srv_p = { 0, srv_id };
    cmd_hdr.opcode = OP_PS_SET_SRV;
    cmd_hdr.payload_length = sizeof(ps_srv_p);
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, reinterpret_cast<const uint8_t*>(&ps_srv_p), sizeof(ps_srv_p)),
                "Dispatch OP_PS_SET_SRV");

    PSSetSamplerPayload ps_samp_p = { 0, samp_id };
    cmd_hdr.opcode = OP_PS_SET_SAMPLER;
    cmd_hdr.payload_length = sizeof(ps_samp_p);
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, reinterpret_cast<const uint8_t*>(&ps_samp_p), sizeof(ps_samp_p)),
                "Dispatch OP_PS_SET_SAMPLER");

    // Update Constant Buffer via OP_WRITE_BUFFER_DISCARD
    float color_mod[4] = { 1.0f, 1.0f, 1.0f, 1.0f }; // Identity multiplier
    std::vector<uint8_t> cb_discard_payload(sizeof(WriteBufferDiscardPayload) + sizeof(color_mod));
    auto* discard_p = reinterpret_cast<WriteBufferDiscardPayload*>(cb_discard_payload.data());
    discard_p->res_id = cb_id;
    discard_p->byte_offset = 0;
    discard_p->byte_width = sizeof(color_mod);
    std::memcpy(cb_discard_payload.data() + sizeof(WriteBufferDiscardPayload), color_mod, sizeof(color_mod));

    cmd_hdr.opcode = OP_WRITE_BUFFER_DISCARD;
    cmd_hdr.payload_length = static_cast<uint32_t>(cb_discard_payload.size());
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, cb_discard_payload.data(), cb_discard_payload.size()),
                "Dispatch OP_WRITE_BUFFER_DISCARD");

    // 5. Issue Draw Call
    std::cout << "\n--- Issuing Draw Call ---" << std::endl;
    DrawPayload draw_p = { 3, 0 }; // Draw 3 vertices
    cmd_hdr.opcode = OP_DRAW;
    cmd_hdr.payload_length = sizeof(draw_p);
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, reinterpret_cast<const uint8_t*>(&draw_p), sizeof(draw_p)),
                "Dispatch OP_DRAW (3 vertices)");

    // 6. Test OP_PRESENT and Readback
    std::cout << "\n--- Testing OP_PRESENT and Readback ---" << std::endl;
    PresentPayload present_p = { 0, 1, 0, 1 }; // frame 1
    cmd_hdr.opcode = OP_PRESENT;
    cmd_hdr.payload_length = sizeof(present_p);
    AssertCheck(dispatcher.DispatchCommand(cmd_hdr, reinterpret_cast<const uint8_t*>(&present_p), sizeof(present_p), nullptr),
                "Dispatch OP_PRESENT");
    AssertCheck(dispatcher.GetTotalFramesPresented() == 1, "TotalFramesPresented is 1");
    AssertCheck(dispatcher.GetLastPresentedFrame() == 1, "LastPresentedFrame is 1");

    const std::vector<uint8_t>& frame_buffer = dispatcher.GetLastReadbackBuffer();
    AssertCheck(frame_buffer.size() == 256 * 256 * 4, "Readback buffer size is 256x256x4 (262,144 bytes)");

    // Verify Corner Pixel: Should be Cornflower Blue background
    // (100, 149, 237, 255)
    uint8_t r_corner = frame_buffer[0];
    uint8_t g_corner = frame_buffer[1];
    uint8_t b_corner = frame_buffer[2];
    uint8_t a_corner = frame_buffer[3];
    std::cout << "Top-left Corner Pixel [0, 0]: R=" << (int)r_corner
              << " G=" << (int)g_corner << " B=" << (int)b_corner << " A=" << (int)a_corner << std::endl;
    AssertCheck(std::abs((int)r_corner - 100) <= 2 &&
                std::abs((int)g_corner - 149) <= 2 &&
                std::abs((int)b_corner - 237) <= 2,
                "Corner pixel matches clear color Cornflower Blue");

    // Verify Center Pixel: (x=128, y=140) should be inside the rasterized triangle!
    size_t center_idx = (140 * 256 + 128) * 4;
    uint8_t r_center = frame_buffer[center_idx];
    uint8_t g_center = frame_buffer[center_idx + 1];
    uint8_t b_center = frame_buffer[center_idx + 2];
    uint8_t a_center = frame_buffer[center_idx + 3];
    std::cout << "Triangle Interior Pixel [128, 140]: R=" << (int)r_center
              << " G=" << (int)g_center << " B=" << (int)b_center << " A=" << (int)a_center << std::endl;

    // Check that interior pixel is distinctly NOT Cornflower Blue (diff > 30)
    int diff_from_bg = std::abs((int)r_center - 100) + std::abs((int)g_center - 149) + std::abs((int)b_center - 237);
    AssertCheck(diff_from_bg > 30, "Interior pixel is distinctly rasterized geometry (not clear color)");
    AssertCheck(a_center == 255, "Alpha channel is 255");

    // Compute frame statistical metrics
    size_t total_pixels = 256 * 256;
    double sum_lum = 0.0;
    double sum_lum_sq = 0.0;
    size_t non_clear_count = 0;

    for (size_t i = 0; i < frame_buffer.size(); i += 4) {
        uint8_t r = frame_buffer[i];
        uint8_t g = frame_buffer[i + 1];
        uint8_t b = frame_buffer[i + 2];

        double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        sum_lum += lum;
        sum_lum_sq += lum * lum;

        int diff = std::abs((int)r - 100) + std::abs((int)g - 149) + std::abs((int)b - 237);
        if (diff > 30) {
            non_clear_count++;
        }
    }

    double mean_lum = sum_lum / total_pixels;
    double variance = (sum_lum_sq / total_pixels) - (mean_lum * mean_lum);
    double std_lum = (variance > 0) ? std::sqrt(variance) : 0.0;
    double non_clear_ratio = static_cast<double>(non_clear_count) / total_pixels;

    std::cout << "--- Mathematical Pixel Verification ---" << std::endl;
    std::cout << "Mean Luminance:    " << mean_lum << std::endl;
    std::cout << "StdDev Luminance:  " << std_lum << std::endl;
    std::cout << "Non-Clear Pixels:  " << non_clear_count << " (" << (non_clear_ratio * 100.0) << "%)" << std::endl;

    AssertCheck(mean_lum >= 10.0 && mean_lum <= 245.0, "Mean luminance in valid range [10, 245]");
    AssertCheck(std_lum >= 3.0, "Luminance StdDev >= 3.0 indicates non-flat shaded render");
    AssertCheck(non_clear_ratio >= 0.02, "Non-clear ratio >= 2% confirms geometry rasterization");

    // 7. Test Network Socket Streaming over Loopback (127.0.0.1)
    std::cout << "\n--- Testing Network Socket Streaming over Loopback ---" << std::endl;
    uint16_t test_port = 48899;
    NetworkSocket listen_sock;
    AssertCheck(listen_sock.BindAndListen(test_port, "127.0.0.1"), "Server socket BindAndListen (127.0.0.1:48899)");

    std::unique_ptr<NetworkSocket> server_client_sock;
    std::thread server_accept_thread([&]() {
        server_client_sock = listen_sock.Accept(3000);
    });

    NetworkSocket client_sock;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    AssertCheck(client_sock.Connect("127.0.0.1", test_port, 3000), "Client socket Connect (127.0.0.1:48899)");

    server_accept_thread.join();
    AssertCheck(server_client_sock != nullptr && server_client_sock->IsValid(), "Server accepted client connection");

    // Client sends OP_HANDSHAKE_REQUEST over socket
    PacketHeader net_hs_hdr = {};
    net_hs_hdr.magic = RGPU_MAGIC;
    net_hs_hdr.opcode = OP_HANDSHAKE_REQUEST;
    net_hs_hdr.sequence_id = 100;
    net_hs_hdr.payload_length = sizeof(HandshakeRequestPayload);

    HandshakeRequestPayload net_hs_req = {};
    net_hs_req.protocol_version = RGPU_PROTOCOL_VERSION;
    net_hs_req.client_width = 256;
    net_hs_req.client_height = 256;
    net_hs_req.preferred_format = 28;

    AssertCheck(client_sock.SendPacket(net_hs_hdr, &net_hs_req), "Client SendPacket(OP_HANDSHAKE_REQUEST)");

    // Server reads packet and dispatches
    PacketHeader srv_recv_hdr = {};
    std::vector<uint8_t> srv_recv_payload;
    AssertCheck(server_client_sock->RecvPacket(srv_recv_hdr, srv_recv_payload), "Server RecvPacket");
    AssertCheck(srv_recv_hdr.opcode == OP_HANDSHAKE_REQUEST, "Server received OP_HANDSHAKE_REQUEST");

    AssertCheck(dispatcher.DispatchCommand(srv_recv_hdr, srv_recv_payload.data(), srv_recv_payload.size(), server_client_sock.get()),
                "Server DispatchCommand sends HandshakeResponse back to client");

    // Client reads OP_HANDSHAKE_RESPONSE
    PacketHeader cli_recv_hdr = {};
    std::vector<uint8_t> cli_recv_payload;
    AssertCheck(client_sock.RecvPacket(cli_recv_hdr, cli_recv_payload), "Client RecvPacket(OP_HANDSHAKE_RESPONSE)");
    AssertCheck(cli_recv_hdr.opcode == OP_HANDSHAKE_RESPONSE, "Opcode is OP_HANDSHAKE_RESPONSE");
    AssertCheck(cli_recv_hdr.sequence_id == 100, "Sequence ID matches (100)");

    const auto* hs_resp = reinterpret_cast<const HandshakeResponsePayload*>(cli_recv_payload.data());
    AssertCheck(hs_resp->status_code == 0, "Handshake status_code == 0 (OK)");
    AssertCheck(hs_resp->frame_width == 256 && hs_resp->frame_height == 256, "Response dimensions 256x256");
    std::cout << "Adapter returned in handshake: " << hs_resp->adapter_desc << std::endl;

    // Client requests OP_PRESENT over socket
    PacketHeader net_present_hdr = {};
    net_present_hdr.magic = RGPU_MAGIC;
    net_present_hdr.opcode = OP_PRESENT;
    net_present_hdr.sequence_id = 101;
    net_present_hdr.payload_length = sizeof(PresentPayload);

    PresentPayload net_present_payload = { 0, 1, 0, 42 };
    AssertCheck(client_sock.SendPacket(net_present_hdr, &net_present_payload), "Client SendPacket(OP_PRESENT)");

    // Server receives and dispatches OP_PRESENT with response
    AssertCheck(server_client_sock->RecvPacket(srv_recv_hdr, srv_recv_payload), "Server RecvPacket(OP_PRESENT)");
    AssertCheck(dispatcher.DispatchCommand(srv_recv_hdr, srv_recv_payload.data(), srv_recv_payload.size(), server_client_sock.get()),
                "Server DispatchCommand(OP_PRESENT) streams OP_FRAME_DATA back to client");

    // Client receives OP_FRAME_DATA
    AssertCheck(client_sock.RecvPacket(cli_recv_hdr, cli_recv_payload), "Client RecvPacket(OP_FRAME_DATA)");
    AssertCheck(cli_recv_hdr.opcode == OP_FRAME_DATA, "Opcode is OP_FRAME_DATA");
    AssertCheck(cli_recv_hdr.sequence_id == 101, "Sequence ID synchronized (101)");

    const auto* frame_hdr = reinterpret_cast<const FrameDataPayload*>(cli_recv_payload.data());
    AssertCheck(frame_hdr->frame_number == 42, "Frame number synchronized (42)");
    AssertCheck(frame_hdr->width == 256 && frame_hdr->height == 256, "Frame size 256x256");
    AssertCheck(frame_hdr->data_size == 256 * 256 * 4, "Data size 262,144 bytes");
    AssertCheck(cli_recv_payload.size() == sizeof(FrameDataPayload) + (256 * 256 * 4), "Full payload size verified");

    // Close sockets
    client_sock.Close();
    server_client_sock->Close();
    listen_sock.Close();
    NetworkSocket::ShutdownNetwork();

    std::cout << "\n============================================================" << std::endl;
    std::cout << "[SUCCESS] ALL Server Resource Manager & Dispatcher Unit Tests PASSED!" << std::endl;
    std::cout << "============================================================" << std::endl;
    return 0;
}
