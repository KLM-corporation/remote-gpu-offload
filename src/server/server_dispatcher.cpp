#include "server_dispatcher.h"

#include <iostream>
#include <cstring>
#include <algorithm>

namespace RemoteGPU {

// =============================================================================
// ServerResourceManager Implementation
// =============================================================================

void ServerResourceManager::Register(uint32_t res_id, Microsoft::WRL::ComPtr<ID3D11DeviceChild> resource) {
    if (res_id == 0) return;
    m_resources[res_id] = std::move(resource);
}

bool ServerResourceManager::Destroy(uint32_t res_id) {
    if (res_id == 0) return false;
    return m_resources.erase(res_id) > 0;
}

void ServerResourceManager::Clear() {
    m_resources.clear();
}

bool ServerResourceManager::HasResource(uint32_t res_id) const {
    if (res_id == 0) return false;
    return m_resources.find(res_id) != m_resources.end();
}

size_t ServerResourceManager::Count() const {
    return m_resources.size();
}

Microsoft::WRL::ComPtr<ID3D11DeviceChild> ServerResourceManager::GetRaw(uint32_t res_id) const {
    if (res_id == 0) return nullptr;
    auto it = m_resources.find(res_id);
    if (it != m_resources.end()) {
        return it->second;
    }
    return nullptr;
}

// =============================================================================
// ServerDispatcher Implementation
// =============================================================================

ServerDispatcher::ServerDispatcher(HostGpuDevice& gpuDevice)
    : m_gpuDevice(gpuDevice)
{
}

ServerDispatcher::~ServerDispatcher() {
    ResetSession();
}

void ServerDispatcher::ResetSession() {
    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (context) {
        context->ClearState();
        context->Flush();
    }
    m_resources.Clear();
    m_handshakeCompleted = false;
    m_clientProtocolVersion = 0;
    m_frameWidth = 0;
    m_frameHeight = 0;
    m_lastPresentedFrame = 0;
}

bool ServerDispatcher::ProcessPacketStream(NetworkSocket& socket) {
    PacketHeader header = {};
    std::vector<uint8_t> payload;

    while (socket.IsValid()) {
        if (!socket.RecvPacket(header, payload)) {
            // Socket disconnected or timeout reached
            break;
        }

        if (header.magic != RGPU_MAGIC) {
            std::cerr << "[ServerDispatcher] Invalid magic identifier: 0x"
                      << std::hex << header.magic << std::dec << std::endl;
            return false;
        }

        bool ok = DispatchCommand(header, payload.data(), payload.size(), &socket);
        if (!ok) {
            std::cerr << "[ServerDispatcher] DispatchCommand failed for opcode: 0x"
                      << std::hex << header.opcode << std::dec << std::endl;
            if (header.opcode == OP_SHUTDOWN) {
                break;
            }
        }

        if (header.opcode == OP_SHUTDOWN) {
            break;
        }
    }

    return true;
}

bool ServerDispatcher::DispatchCommand(const PacketHeader& header, const uint8_t* payload, size_t payload_size, NetworkSocket* socket) {
    m_totalCommandsProcessed++;

    switch (header.opcode) {
        // 1. Session & Lifecycle
        case OP_HANDSHAKE_REQUEST:
            return HandleHandshakeRequest(header, payload, payload_size, socket);
        case OP_HANDSHAKE_RESPONSE:
            return true;
        case OP_SHUTDOWN:
            return HandleShutdown(header, payload, payload_size);

        // 2. Resource Management
        case OP_CREATE_BUFFER:
            return HandleCreateBuffer(header, payload, payload_size);
        case OP_CREATE_TEXTURE2D:
            return HandleCreateTexture2D(header, payload, payload_size);
        case OP_CREATE_SRV:
            return HandleCreateSrv(header, payload, payload_size);
        case OP_CREATE_RTV:
            return HandleCreateRtv(header, payload, payload_size);
        case OP_CREATE_DSV:
            return HandleCreateDsv(header, payload, payload_size);
        case OP_CREATE_VERTEX_SHADER:
            return HandleCreateVertexShader(header, payload, payload_size);
        case OP_CREATE_PIXEL_SHADER:
            return HandleCreatePixelShader(header, payload, payload_size);
        case OP_CREATE_INPUT_LAYOUT:
            return HandleCreateInputLayout(header, payload, payload_size);
        case OP_CREATE_SAMPLER:
            return HandleCreateSampler(header, payload, payload_size);
        case OP_DESTROY_RESOURCE:
            return HandleDestroyResource(header, payload, payload_size);

        // 3. Pipeline State & Binding
        case OP_IA_SET_INPUT_LAYOUT:
            return HandleIASetInputLayout(header, payload, payload_size);
        case OP_IA_SET_VERTEX_BUFFER:
            return HandleIASetVertexBuffer(header, payload, payload_size);
        case OP_IA_SET_INDEX_BUFFER:
            return HandleIASetIndexBuffer(header, payload, payload_size);
        case OP_IA_SET_TOPOLOGY:
            return HandleIASetTopology(header, payload, payload_size);
        case OP_VS_SET_SHADER:
            return HandleVSSetShader(header, payload, payload_size);
        case OP_VS_SET_CB:
            return HandleVSSetCB(header, payload, payload_size);
        case OP_PS_SET_SHADER:
            return HandlePSSetShader(header, payload, payload_size);
        case OP_PS_SET_CB:
            return HandlePSSetCB(header, payload, payload_size);
        case OP_PS_SET_SRV:
            return HandlePSSetSrv(header, payload, payload_size);
        case OP_PS_SET_SAMPLER:
            return HandlePSSetSampler(header, payload, payload_size);
        case OP_RS_SET_VIEWPORT:
            return HandleRSSetViewport(header, payload, payload_size);
        case OP_OM_SET_RENDER_TARGETS:
            return HandleOMSetRenderTargets(header, payload, payload_size);

        // 4. Updates & Draw Calls
        case OP_UPDATE_SUBRESOURCE:
            return HandleUpdateSubresource(header, payload, payload_size);
        case OP_WRITE_BUFFER_DISCARD:
            return HandleWriteBufferDiscard(header, payload, payload_size);
        case OP_CLEAR_RTV:
            return HandleClearRtv(header, payload, payload_size);
        case OP_CLEAR_DSV:
            return HandleClearDsv(header, payload, payload_size);
        case OP_DRAW:
            return HandleDraw(header, payload, payload_size);
        case OP_DRAW_INDEXED:
            return HandleDrawIndexed(header, payload, payload_size);

        // 5. Presentation
        case OP_PRESENT:
            return HandlePresent(header, payload, payload_size, socket);
        case OP_FRAME_DATA:
            return true;

        default:
            std::cerr << "[ServerDispatcher] Unhandled opcode: 0x"
                      << std::hex << header.opcode << std::dec << std::endl;
            return false;
    }
}

// =============================================================================
// Lifecycle Handlers
// =============================================================================

bool ServerDispatcher::HandleHandshakeRequest(const PacketHeader& header, const uint8_t* payload, size_t size, NetworkSocket* socket) {
    if (size < sizeof(HandshakeRequestPayload)) {
        std::cerr << "[ServerDispatcher] HandshakeRequest payload too small: " << size << std::endl;
        return false;
    }

    const auto* req = reinterpret_cast<const HandshakeRequestPayload*>(payload);
    m_clientProtocolVersion = req->protocol_version;
    m_frameWidth = req->client_width;
    m_frameHeight = req->client_height;

    uint32_t status_code = 0; // 0: OK
    if (req->protocol_version != RGPU_PROTOCOL_VERSION) {
        std::cerr << "[ServerDispatcher] Protocol version mismatch. Client: "
                  << req->protocol_version << ", Server: " << RGPU_PROTOCOL_VERSION << std::endl;
        status_code = 1; // 1: Version Mismatch
    }

    // Configure offscreen targets on HostGpuDevice if valid dimensions requested
    if (status_code == 0 && req->client_width > 0 && req->client_height > 0) {
        if (!m_gpuDevice.CreateOffscreenTargets(req->client_width, req->client_height)) {
            std::cerr << "[ServerDispatcher] Failed to create offscreen render targets ("
                      << req->client_width << "x" << req->client_height << ")" << std::endl;
            status_code = 2; // 2: GPU Init Failed
        }
    }

    // Construct response
    HandshakeResponsePayload resp = {};
    resp.protocol_version = RGPU_PROTOCOL_VERSION;
    resp.status_code = status_code;
    resp.feature_level = static_cast<uint32_t>(m_gpuDevice.GetFeatureLevel());
    resp.frame_width = m_gpuDevice.GetWidth();
    resp.frame_height = m_gpuDevice.GetHeight();
    resp.frame_format = 28; // DXGI_FORMAT_R8G8B8A8_UNORM
    resp.dedicated_vram = m_gpuDevice.GetDedicatedVram();

    const std::string& desc = m_gpuDevice.GetAdapterDescription();
    size_t copy_len = (std::min)(desc.size(), sizeof(resp.adapter_desc) - 1);
    std::memcpy(resp.adapter_desc, desc.c_str(), copy_len);
    resp.adapter_desc[copy_len] = '\0';

    if (socket && socket->IsValid()) {
        PacketHeader resp_header = {};
        resp_header.magic = RGPU_MAGIC;
        resp_header.opcode = OP_HANDSHAKE_RESPONSE;
        resp_header.flags = FLAG_NONE;
        resp_header.sequence_id = header.sequence_id;
        resp_header.payload_length = sizeof(HandshakeResponsePayload);

        if (!socket->SendPacket(resp_header, &resp)) {
            std::cerr << "[ServerDispatcher] Failed to send HandshakeResponse" << std::endl;
            return false;
        }
    }

    m_handshakeCompleted = (status_code == 0);
    return m_handshakeCompleted;
}

bool ServerDispatcher::HandleShutdown(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    uint32_t exit_code = 0;
    if (size >= sizeof(ShutdownPayload)) {
        const auto* p = reinterpret_cast<const ShutdownPayload*>(payload);
        exit_code = p->exit_code;
    }
    std::cout << "[ServerDispatcher] Client requested shutdown (exit_code=" << exit_code << ")." << std::endl;
    ResetSession();
    return true;
}

// =============================================================================
// Resource Creation Handlers
// =============================================================================

bool ServerDispatcher::HandleCreateBuffer(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateBufferPayload)) {
        std::cerr << "[ServerDispatcher] CreateBuffer payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const CreateBufferPayload*>(payload);
    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = p->byte_width;
    desc.Usage = static_cast<D3D11_USAGE>(p->usage);
    desc.BindFlags = p->bind_flags;
    desc.CPUAccessFlags = p->cpu_access_flags;
    desc.MiscFlags = p->misc_flags;
    desc.StructureByteStride = p->structure_byte_stride;

    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    HRESULT hr = S_OK;

    if (p->has_initial_data && p->initial_data_size > 0) {
        if (size < sizeof(CreateBufferPayload) + p->initial_data_size) {
            std::cerr << "[ServerDispatcher] CreateBuffer payload missing initial data" << std::endl;
            return false;
        }
        D3D11_SUBRESOURCE_DATA init_data = {};
        init_data.pSysMem = payload + sizeof(CreateBufferPayload);
        hr = device->CreateBuffer(&desc, &init_data, &buffer);
    } else {
        hr = device->CreateBuffer(&desc, nullptr, &buffer);
    }

    if (FAILED(hr)) {
        std::cerr << "[ServerDispatcher] CreateBuffer failed: 0x" << std::hex << hr << std::dec
                  << " (res_id=" << p->res_id << ")" << std::endl;
        return false;
    }

    m_resources.Register(p->res_id, buffer);
    return true;
}

bool ServerDispatcher::HandleCreateTexture2D(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateTexture2DPayload)) {
        std::cerr << "[ServerDispatcher] CreateTexture2D payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const CreateTexture2DPayload*>(payload);
    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = p->width;
    desc.Height = p->height;
    desc.MipLevels = p->mips;
    desc.ArraySize = p->array_size;
    desc.Format = static_cast<DXGI_FORMAT>(p->format);
    desc.SampleDesc.Count = (p->sample_count > 0) ? p->sample_count : 1;
    desc.SampleDesc.Quality = p->sample_quality;
    desc.Usage = static_cast<D3D11_USAGE>(p->usage);
    desc.BindFlags = p->bind_flags;
    desc.CPUAccessFlags = p->cpu_access_flags;
    desc.MiscFlags = p->misc_flags;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = S_OK;

    if (p->has_initial_data && p->initial_data_size > 0) {
        if (size < sizeof(CreateTexture2DPayload) + p->initial_data_size) {
            std::cerr << "[ServerDispatcher] CreateTexture2D payload missing initial data" << std::endl;
            return false;
        }
        D3D11_SUBRESOURCE_DATA init_data = {};
        init_data.pSysMem = payload + sizeof(CreateTexture2DPayload);
        init_data.SysMemPitch = p->sys_mem_pitch;
        init_data.SysMemSlicePitch = p->sys_mem_slice_pitch;
        hr = device->CreateTexture2D(&desc, &init_data, &texture);
    } else {
        hr = device->CreateTexture2D(&desc, nullptr, &texture);
    }

    if (FAILED(hr)) {
        std::cerr << "[ServerDispatcher] CreateTexture2D failed: 0x" << std::hex << hr << std::dec
                  << " (res_id=" << p->res_id << ")" << std::endl;
        return false;
    }

    m_resources.Register(p->res_id, texture);
    return true;
}

bool ServerDispatcher::HandleCreateSrv(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateSrvPayload)) {
        std::cerr << "[ServerDispatcher] CreateSrv payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const CreateSrvPayload*>(payload);
    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    Microsoft::WRL::ComPtr<ID3D11Resource> resource = m_resources.Get<ID3D11Resource>(p->resource_id);
    if (!resource) {
        std::cerr << "[ServerDispatcher] CreateSrv: underlying resource not found: " << p->resource_id << std::endl;
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
    bool use_desc = (p->format != 0 || p->view_dimension != 0);
    if (use_desc) {
        desc.Format = static_cast<DXGI_FORMAT>(p->format);
        desc.ViewDimension = static_cast<D3D11_SRV_DIMENSION>(p->view_dimension);
        if (desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
            desc.Texture2D.MostDetailedMip = p->most_detailed_mip;
            desc.Texture2D.MipLevels = p->mip_levels;
        }
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = device->CreateShaderResourceView(resource.Get(), use_desc ? &desc : nullptr, &srv);
    if (FAILED(hr)) {
        std::cerr << "[ServerDispatcher] CreateShaderResourceView failed: 0x" << std::hex << hr << std::dec
                  << " (res_id=" << p->res_id << ")" << std::endl;
        return false;
    }

    m_resources.Register(p->res_id, srv);
    return true;
}

bool ServerDispatcher::HandleCreateRtv(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateRtvPayload)) {
        std::cerr << "[ServerDispatcher] CreateRtv payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const CreateRtvPayload*>(payload);
    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;

    // Special case: resource_id == 0 indicates binding to the host offscreen default backbuffer
    if (p->resource_id == 0) {
        rtv = m_gpuDevice.GetRenderTargetView();
        if (!rtv && m_gpuDevice.GetColorTexture()) {
            HRESULT hr = device->CreateRenderTargetView(m_gpuDevice.GetColorTexture(), nullptr, &rtv);
            if (FAILED(hr)) {
                std::cerr << "[ServerDispatcher] CreateRenderTargetView on default backbuffer failed: 0x"
                          << std::hex << hr << std::dec << std::endl;
                return false;
            }
        }
    } else {
        Microsoft::WRL::ComPtr<ID3D11Resource> resource = m_resources.Get<ID3D11Resource>(p->resource_id);
        if (!resource) {
            // Fallback: if client passes an ID representing the swapchain backbuffer, use host color texture
            if (m_gpuDevice.GetColorTexture()) {
                resource = m_gpuDevice.GetColorTexture();
            } else {
                std::cerr << "[ServerDispatcher] CreateRtv: resource not found: " << p->resource_id << std::endl;
                return false;
            }
        }

        D3D11_RENDER_TARGET_VIEW_DESC desc = {};
        bool use_desc = (p->format != 0);
        if (use_desc) {
            desc.Format = static_cast<DXGI_FORMAT>(p->format);
            desc.ViewDimension = static_cast<D3D11_RTV_DIMENSION>(p->view_dimension);
            desc.Texture2D.MipSlice = p->mip_slice;
        }

        HRESULT hr = device->CreateRenderTargetView(resource.Get(), use_desc ? &desc : nullptr, &rtv);
        if (FAILED(hr)) {
            std::cerr << "[ServerDispatcher] CreateRenderTargetView failed: 0x" << std::hex << hr << std::dec
                      << " (res_id=" << p->res_id << ")" << std::endl;
            return false;
        }
    }

    m_resources.Register(p->res_id, rtv);
    return true;
}

bool ServerDispatcher::HandleCreateDsv(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateDsvPayload)) {
        std::cerr << "[ServerDispatcher] CreateDsv payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const CreateDsvPayload*>(payload);
    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;

    // Special case: resource_id == 0 indicates host offscreen depth-stencil buffer
    if (p->resource_id == 0) {
        dsv = m_gpuDevice.GetDepthStencilView();
        if (!dsv && m_gpuDevice.GetDepthTexture()) {
            HRESULT hr = device->CreateDepthStencilView(m_gpuDevice.GetDepthTexture(), nullptr, &dsv);
            if (FAILED(hr)) {
                std::cerr << "[ServerDispatcher] CreateDepthStencilView on default depth buffer failed: 0x"
                          << std::hex << hr << std::dec << std::endl;
                return false;
            }
        }
    } else {
        Microsoft::WRL::ComPtr<ID3D11Resource> resource = m_resources.Get<ID3D11Resource>(p->resource_id);
        if (!resource) {
            if (m_gpuDevice.GetDepthTexture()) {
                resource = m_gpuDevice.GetDepthTexture();
            } else {
                std::cerr << "[ServerDispatcher] CreateDsv: resource not found: " << p->resource_id << std::endl;
                return false;
            }
        }

        D3D11_DEPTH_STENCIL_VIEW_DESC desc = {};
        bool use_desc = (p->format != 0);
        if (use_desc) {
            desc.Format = static_cast<DXGI_FORMAT>(p->format);
            desc.ViewDimension = static_cast<D3D11_DSV_DIMENSION>(p->view_dimension);
            desc.Flags = p->flags;
            desc.Texture2D.MipSlice = p->mip_slice;
        }

        HRESULT hr = device->CreateDepthStencilView(resource.Get(), use_desc ? &desc : nullptr, &dsv);
        if (FAILED(hr)) {
            std::cerr << "[ServerDispatcher] CreateDepthStencilView failed: 0x" << std::hex << hr << std::dec
                      << " (res_id=" << p->res_id << ")" << std::endl;
            return false;
        }
    }

    m_resources.Register(p->res_id, dsv);
    return true;
}

bool ServerDispatcher::HandleCreateVertexShader(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateShaderPayload)) {
        std::cerr << "[ServerDispatcher] CreateVertexShader payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const CreateShaderPayload*>(payload);
    if (size < sizeof(CreateShaderPayload) + p->bytecode_length) {
        std::cerr << "[ServerDispatcher] CreateVertexShader bytecode truncated" << std::endl;
        return false;
    }

    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    const void* bytecode = payload + sizeof(CreateShaderPayload);
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
    HRESULT hr = device->CreateVertexShader(bytecode, p->bytecode_length, nullptr, &vs);
    if (FAILED(hr)) {
        std::cerr << "[ServerDispatcher] CreateVertexShader failed: 0x" << std::hex << hr << std::dec
                  << " (res_id=" << p->res_id << ")" << std::endl;
        return false;
    }

    m_resources.Register(p->res_id, vs);
    return true;
}

bool ServerDispatcher::HandleCreatePixelShader(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateShaderPayload)) {
        std::cerr << "[ServerDispatcher] CreatePixelShader payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const CreateShaderPayload*>(payload);
    if (size < sizeof(CreateShaderPayload) + p->bytecode_length) {
        std::cerr << "[ServerDispatcher] CreatePixelShader bytecode truncated" << std::endl;
        return false;
    }

    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    const void* bytecode = payload + sizeof(CreateShaderPayload);
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
    HRESULT hr = device->CreatePixelShader(bytecode, p->bytecode_length, nullptr, &ps);
    if (FAILED(hr)) {
        std::cerr << "[ServerDispatcher] CreatePixelShader failed: 0x" << std::hex << hr << std::dec
                  << " (res_id=" << p->res_id << ")" << std::endl;
        return false;
    }

    m_resources.Register(p->res_id, ps);
    return true;
}

bool ServerDispatcher::HandleCreateInputLayout(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateInputLayoutPayload)) {
        std::cerr << "[ServerDispatcher] CreateInputLayout payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const CreateInputLayoutPayload*>(payload);
    size_t elements_size = static_cast<size_t>(p->num_elements) * sizeof(RgpuInputElementDesc);
    size_t expected_total = sizeof(CreateInputLayoutPayload) + elements_size + p->bytecode_length;

    if (size < expected_total) {
        std::cerr << "[ServerDispatcher] CreateInputLayout payload truncated" << std::endl;
        return false;
    }

    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    const auto* src_elements = reinterpret_cast<const RgpuInputElementDesc*>(payload + sizeof(CreateInputLayoutPayload));
    std::vector<D3D11_INPUT_ELEMENT_DESC> d3d_elements(p->num_elements);
    std::vector<std::string> semantic_names(p->num_elements);

    for (uint32_t i = 0; i < p->num_elements; ++i) {
        semantic_names[i] = src_elements[i].semantic_name;
        d3d_elements[i].SemanticName = semantic_names[i].c_str();
        d3d_elements[i].SemanticIndex = src_elements[i].semantic_index;
        d3d_elements[i].Format = static_cast<DXGI_FORMAT>(src_elements[i].format);
        d3d_elements[i].InputSlot = src_elements[i].input_slot;
        d3d_elements[i].AlignedByteOffset = src_elements[i].aligned_byte_offset;
        d3d_elements[i].InputSlotClass = static_cast<D3D11_INPUT_CLASSIFICATION>(src_elements[i].input_slot_class);
        d3d_elements[i].InstanceDataStepRate = src_elements[i].instance_data_step;
    }

    const void* bytecode = payload + sizeof(CreateInputLayoutPayload) + elements_size;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;
    HRESULT hr = device->CreateInputLayout(d3d_elements.data(), p->num_elements, bytecode, p->bytecode_length, &layout);
    if (FAILED(hr)) {
        std::cerr << "[ServerDispatcher] CreateInputLayout failed: 0x" << std::hex << hr << std::dec
                  << " (res_id=" << p->res_id << ")" << std::endl;
        return false;
    }

    m_resources.Register(p->res_id, layout);
    return true;
}

bool ServerDispatcher::HandleCreateSampler(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateSamplerPayload)) {
        std::cerr << "[ServerDispatcher] CreateSampler payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const CreateSamplerPayload*>(payload);
    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = static_cast<D3D11_FILTER>(p->filter);
    desc.AddressU = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(p->address_u);
    desc.AddressV = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(p->address_v);
    desc.AddressW = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(p->address_w);
    desc.MipLODBias = p->mip_lod_bias;
    desc.MaxAnisotropy = p->max_anisotropy;
    desc.ComparisonFunc = static_cast<D3D11_COMPARISON_FUNC>(p->comparison_func);
    std::memcpy(desc.BorderColor, p->border_color, sizeof(desc.BorderColor));
    desc.MinLOD = p->min_lod;
    desc.MaxLOD = p->max_lod;

    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
    HRESULT hr = device->CreateSamplerState(&desc, &sampler);
    if (FAILED(hr)) {
        std::cerr << "[ServerDispatcher] CreateSamplerState failed: 0x" << std::hex << hr << std::dec
                  << " (res_id=" << p->res_id << ")" << std::endl;
        return false;
    }

    m_resources.Register(p->res_id, sampler);
    return true;
}

bool ServerDispatcher::HandleDestroyResource(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(DestroyResourcePayload)) {
        std::cerr << "[ServerDispatcher] DestroyResource payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const DestroyResourcePayload*>(payload);
    return m_resources.Destroy(p->res_id);
}

// =============================================================================
// Pipeline State Handlers
// =============================================================================

bool ServerDispatcher::HandleIASetInputLayout(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(IASetInputLayoutPayload)) return false;
    const auto* p = reinterpret_cast<const IASetInputLayoutPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout = (p->res_id == 0) ? nullptr : m_resources.GetInputLayout(p->res_id);
    context->IASetInputLayout(layout.Get());
    return true;
}

bool ServerDispatcher::HandleIASetVertexBuffer(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(IASetVertexBufferPayload)) return false;
    const auto* p = reinterpret_cast<const IASetVertexBufferPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer = (p->res_id == 0) ? nullptr : m_resources.GetBuffer(p->res_id);
    ID3D11Buffer* p_buffer = buffer.Get();
    UINT stride = p->stride;
    UINT offset = p->offset;

    context->IASetVertexBuffers(p->slot, 1, &p_buffer, &stride, &offset);
    return true;
}

bool ServerDispatcher::HandleIASetIndexBuffer(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(IASetIndexBufferPayload)) return false;
    const auto* p = reinterpret_cast<const IASetIndexBufferPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer = (p->res_id == 0) ? nullptr : m_resources.GetBuffer(p->res_id);
    context->IASetIndexBuffer(buffer.Get(), static_cast<DXGI_FORMAT>(p->format), p->offset);
    return true;
}

bool ServerDispatcher::HandleIASetTopology(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(IASetTopologyPayload)) return false;
    const auto* p = reinterpret_cast<const IASetTopologyPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    context->IASetPrimitiveTopology(static_cast<D3D11_PRIMITIVE_TOPOLOGY>(p->topology));
    return true;
}

bool ServerDispatcher::HandleVSSetShader(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(VSSetShaderPayload)) return false;
    const auto* p = reinterpret_cast<const VSSetShaderPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs = (p->res_id == 0) ? nullptr : m_resources.GetVertexShader(p->res_id);
    context->VSSetShader(vs.Get(), nullptr, 0);
    return true;
}

bool ServerDispatcher::HandleVSSetCB(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(VSSetConstantBufferPayload)) return false;
    const auto* p = reinterpret_cast<const VSSetConstantBufferPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11Buffer> cb = (p->res_id == 0) ? nullptr : m_resources.GetBuffer(p->res_id);
    ID3D11Buffer* p_cb = cb.Get();
    context->VSSetConstantBuffers(p->slot, 1, &p_cb);
    return true;
}

bool ServerDispatcher::HandlePSSetShader(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(PSSetShaderPayload)) return false;
    const auto* p = reinterpret_cast<const PSSetShaderPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps = (p->res_id == 0) ? nullptr : m_resources.GetPixelShader(p->res_id);
    context->PSSetShader(ps.Get(), nullptr, 0);
    return true;
}

bool ServerDispatcher::HandlePSSetCB(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(PSSetConstantBufferPayload)) return false;
    const auto* p = reinterpret_cast<const PSSetConstantBufferPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11Buffer> cb = (p->res_id == 0) ? nullptr : m_resources.GetBuffer(p->res_id);
    ID3D11Buffer* p_cb = cb.Get();
    context->PSSetConstantBuffers(p->slot, 1, &p_cb);
    return true;
}

bool ServerDispatcher::HandlePSSetSrv(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(PSSetSrvPayload)) return false;
    const auto* p = reinterpret_cast<const PSSetSrvPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv = (p->res_id == 0) ? nullptr : m_resources.GetSRV(p->res_id);
    ID3D11ShaderResourceView* p_srv = srv.Get();
    context->PSSetShaderResources(p->slot, 1, &p_srv);
    return true;
}

bool ServerDispatcher::HandlePSSetSampler(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(PSSetSamplerPayload)) return false;
    const auto* p = reinterpret_cast<const PSSetSamplerPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler = (p->res_id == 0) ? nullptr : m_resources.GetSamplerState(p->res_id);
    ID3D11SamplerState* p_sampler = sampler.Get();
    context->PSSetSamplers(p->slot, 1, &p_sampler);
    return true;
}

bool ServerDispatcher::HandleRSSetViewport(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(RSSetViewportPayload)) return false;
    const auto* p = reinterpret_cast<const RSSetViewportPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = p->top_left_x;
    vp.TopLeftY = p->top_left_y;
    vp.Width = p->width;
    vp.Height = p->height;
    vp.MinDepth = p->min_depth;
    vp.MaxDepth = p->max_depth;

    context->RSSetViewports(1, &vp);
    return true;
}

bool ServerDispatcher::HandleOMSetRenderTargets(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(OMSetRenderTargetsPayload)) return false;
    const auto* p = reinterpret_cast<const OMSetRenderTargetsPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    if (p->rtv_id != 0) {
        rtv = m_resources.GetRTV(p->rtv_id);
    }
    // Fallback to host default offscreen render target if not explicitly in map
    if (!rtv) {
        rtv = m_gpuDevice.GetRenderTargetView();
    }

    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
    if (p->dsv_id != 0) {
        dsv = m_resources.GetDSV(p->dsv_id);
    }
    if (!dsv) {
        dsv = m_gpuDevice.GetDepthStencilView();
    }

    ID3D11RenderTargetView* p_rtv = rtv.Get();
    context->OMSetRenderTargets(p_rtv ? 1 : 0, p_rtv ? &p_rtv : nullptr, dsv.Get());
    return true;
}

// =============================================================================
// Updates & Draw Call Handlers
// =============================================================================

bool ServerDispatcher::HandleUpdateSubresource(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(UpdateSubresourcePayload)) return false;
    const auto* p = reinterpret_cast<const UpdateSubresourcePayload*>(payload);

    if (size < sizeof(UpdateSubresourcePayload) + p->data_size) {
        std::cerr << "[ServerDispatcher] UpdateSubresource data truncated" << std::endl;
        return false;
    }

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11Resource> res = m_resources.Get<ID3D11Resource>(p->res_id);
    if (!res) {
        std::cerr << "[ServerDispatcher] UpdateSubresource: resource not found: " << p->res_id << std::endl;
        return false;
    }

    D3D11_BOX box = {};
    const D3D11_BOX* p_box = nullptr;
    if (p->has_dst_box) {
        box.left = p->dst_box.left;
        box.top = p->dst_box.top;
        box.front = p->dst_box.front;
        box.right = p->dst_box.right;
        box.bottom = p->dst_box.bottom;
        box.back = p->dst_box.back;
        p_box = &box;
    }

    const void* p_data = payload + sizeof(UpdateSubresourcePayload);
    context->UpdateSubresource(res.Get(), p->subresource, p_box, p_data, p->row_pitch, p->depth_pitch);
    return true;
}

bool ServerDispatcher::HandleWriteBufferDiscard(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(WriteBufferDiscardPayload)) return false;
    const auto* p = reinterpret_cast<const WriteBufferDiscardPayload*>(payload);

    if (size < sizeof(WriteBufferDiscardPayload) + p->byte_width) {
        std::cerr << "[ServerDispatcher] WriteBufferDiscard data truncated" << std::endl;
        return false;
    }

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11Buffer> buf = m_resources.GetBuffer(p->res_id);
    if (!buf) {
        std::cerr << "[ServerDispatcher] WriteBufferDiscard: buffer not found: " << p->res_id << std::endl;
        return false;
    }

    const void* p_data = payload + sizeof(WriteBufferDiscardPayload);

    // Check buffer usage to determine Map(DISCARD) vs UpdateSubresource
    D3D11_BUFFER_DESC desc = {};
    buf->GetDesc(&desc);

    if ((desc.Usage == D3D11_USAGE_DYNAMIC) && (desc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE)) {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = context->Map(buf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            std::memcpy(static_cast<uint8_t*>(mapped.pData) + p->byte_offset, p_data, p->byte_width);
            context->Unmap(buf.Get(), 0);
            return true;
        }
    }

    // Fallback: Default usage buffers update via UpdateSubresource
    context->UpdateSubresource(buf.Get(), 0, nullptr, p_data, 0, 0);
    return true;
}

bool ServerDispatcher::HandleClearRtv(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(ClearRtvPayload)) return false;
    const auto* p = reinterpret_cast<const ClearRtvPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    if (p->res_id == 0) {
        rtv = m_gpuDevice.GetRenderTargetView();
    } else {
        rtv = m_resources.GetRTV(p->res_id);
        if (!rtv) {
            rtv = m_gpuDevice.GetRenderTargetView();
        }
    }

    if (rtv) {
        context->ClearRenderTargetView(rtv.Get(), p->color);
    }
    return true;
}

bool ServerDispatcher::HandleClearDsv(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(ClearDsvPayload)) return false;
    const auto* p = reinterpret_cast<const ClearDsvPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
    if (p->res_id == 0) {
        dsv = m_gpuDevice.GetDepthStencilView();
    } else {
        dsv = m_resources.GetDSV(p->res_id);
        if (!dsv) {
            dsv = m_gpuDevice.GetDepthStencilView();
        }
    }

    if (dsv) {
        context->ClearDepthStencilView(dsv.Get(), p->clear_flags, p->depth, p->stencil);
    }
    return true;
}

bool ServerDispatcher::HandleDraw(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(DrawPayload)) return false;
    const auto* p = reinterpret_cast<const DrawPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    context->Draw(p->vertex_count, p->start_vertex_location);
    return true;
}

bool ServerDispatcher::HandleDrawIndexed(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(DrawIndexedPayload)) return false;
    const auto* p = reinterpret_cast<const DrawIndexedPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    context->DrawIndexed(p->index_count, p->start_index_location, p->base_vertex_location);
    return true;
}

// =============================================================================
// Presentation Handler
// =============================================================================

bool ServerDispatcher::HandlePresent(const PacketHeader& header, const uint8_t* payload, size_t size, NetworkSocket* socket) {
    if (size < sizeof(PresentPayload)) {
        std::cerr << "[ServerDispatcher] Present payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const PresentPayload*>(payload);
    uint32_t width = 0;
    uint32_t height = 0;

    if (!m_gpuDevice.ReadbackFrame(m_frameReadbackBuffer, width, height)) {
        std::cerr << "[ServerDispatcher] ReadbackFrame failed during OP_PRESENT." << std::endl;
        return false;
    }

    if (socket && socket->IsValid()) {
        PacketHeader resp_header = {};
        resp_header.magic = RGPU_MAGIC;
        resp_header.opcode = OP_FRAME_DATA;
        resp_header.flags = FLAG_NONE;
        resp_header.sequence_id = header.sequence_id; // Sequence synchronization
        resp_header.payload_length = static_cast<uint32_t>(sizeof(FrameDataPayload) + m_frameReadbackBuffer.size());

        FrameDataPayload frame_payload = {};
        frame_payload.swapchain_id = p->swapchain_id;
        frame_payload.width = width;
        frame_payload.height = height;
        frame_payload.format = 28; // DXGI_FORMAT_R8G8B8A8_UNORM
        frame_payload.row_pitch = width * 4;
        frame_payload.data_size = static_cast<uint32_t>(m_frameReadbackBuffer.size());
        frame_payload.frame_number = p->frame_number; // Echo monotonic client frame number

        // Send header + frame payload + pixel buffer
        if (!socket->SendExact(&resp_header, sizeof(resp_header))) return false;
        if (!socket->SendExact(&frame_payload, sizeof(frame_payload))) return false;
        if (!m_frameReadbackBuffer.empty()) {
            if (!socket->SendExact(m_frameReadbackBuffer.data(), m_frameReadbackBuffer.size())) return false;
        }
    }

    m_lastPresentedFrame = p->frame_number;
    m_totalFramesPresented++;
    return true;
}

} // namespace RemoteGPU
