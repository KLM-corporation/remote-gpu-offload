#include "server_dispatcher.h"
#include "dxgi_format_info.h"
#include "frame_codec.h"

#include <iostream>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace RemoteGPU {

namespace {

inline bool IsValidDxgiFormat(uint32_t format) {
    return (format >= 1 && format <= 191);
}

inline bool IsValidD3D11PrimitiveTopology(uint32_t topology) {
    return (topology == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) ||
           (topology >= D3D11_PRIMITIVE_TOPOLOGY_POINTLIST && topology <= D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) ||
           (topology >= D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ && topology <= D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ) ||
           (topology >= D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST && topology <= D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST);
}

} // anonymous namespace

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
    // The compression flag is renegotiated by the next handshake. Byte counters, like the
    // command/frame totals, survive so the session summary can still be logged after OP_SHUTDOWN.
    m_previousFrame.clear();
    m_encodedFrame.clear();
}

bool ServerDispatcher::ProcessPacketStream(NetworkSocket& socket) {
    PacketHeader header = {};
    std::vector<uint8_t> payload;

    while (socket.IsValid()) {
        if (!socket.RecvPacket(header, payload)) {
            // Peer vanished without OP_SHUTDOWN (process killed, network dropped, keepalive expired)
            return false;
        }

        if (header.magic != RGPU_MAGIC) {
            std::cerr << "[ServerDispatcher] Invalid magic identifier: 0x"
                      << std::hex << header.magic << std::dec << std::endl;
            return false;
        }

        bool ok = DispatchCommand(header, payload.data(), payload.size(), &socket);
        if (!ok) {
            static uint32_t s_dispatchFailCount = 0;
            if (s_dispatchFailCount++ < 10) {
                std::cerr << "[ServerDispatcher] DispatchCommand failed for opcode: 0x"
                          << std::hex << header.opcode << std::dec << std::endl;
            }
            if (header.opcode == OP_SHUTDOWN) {
                break;
            }
        }

        if (header.opcode == OP_SHUTDOWN) {
            return true; // graceful client-initiated end of session
        }
    }

    return false;
}

bool ServerDispatcher::DispatchCommand(const PacketHeader& header, const uint8_t* payload, size_t payload_size, NetworkSocket* socket) {
    m_totalCommandsProcessed++;
    if ((m_totalCommandsProcessed % 1000) == 0) {
        ID3D11DeviceContext* ctx = m_gpuDevice.GetContext();
        if (ctx) {
            ctx->Flush();
        }
    }

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
        case OP_CREATE_TEXTURE3D:
            return HandleCreateTexture3D(header, payload, payload_size);
        case OP_CREATE_TEXTURE1D:
            return HandleCreateTexture1D(header, payload, payload_size);
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
        case OP_CREATE_BLEND_STATE:
            return HandleCreateBlendState(header, payload, payload_size);
        case OP_CREATE_DEPTH_STENCIL_STATE:
            return HandleCreateDepthStencilState(header, payload, payload_size);
        case OP_CREATE_RASTERIZER_STATE:
            return HandleCreateRasterizerState(header, payload, payload_size);
        case OP_CREATE_COMPUTE_SHADER:
            return HandleCreateComputeShader(header, payload, payload_size);
        case OP_CREATE_UAV:
            return HandleCreateUav(header, payload, payload_size);
        case OP_CREATE_GEOMETRY_SHADER:
        case OP_CREATE_HULL_SHADER:
        case OP_CREATE_DOMAIN_SHADER:
            return HandleCreateStageShader(header, payload, payload_size);
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
        case OP_OM_SET_BLEND_STATE:
            return HandleOMSetBlendState(header, payload, payload_size);
        case OP_OM_SET_DEPTH_STENCIL_STATE:
            return HandleOMSetDepthStencilState(header, payload, payload_size);
        case OP_RS_SET_STATE:
            return HandleRSSetState(header, payload, payload_size);
        case OP_RS_SET_SCISSOR:
            return HandleRSSetScissor(header, payload, payload_size);
        case OP_OM_SET_RENDER_TARGETS_MRT:
            return HandleOMSetRenderTargetsMrt(header, payload, payload_size);
        case OP_VS_SET_SRV:
            return HandleVSSetSrv(header, payload, payload_size);
        case OP_VS_SET_SAMPLER:
            return HandleVSSetSampler(header, payload, payload_size);
        case OP_CS_SET_SHADER:
            return HandleCSSetShader(header, payload, payload_size);
        case OP_CS_SET_SRV:
            return HandleCSSetSrv(header, payload, payload_size);
        case OP_CS_SET_UAV:
            return HandleCSSetUav(header, payload, payload_size);
        case OP_CS_SET_CB:
            return HandleCSSetCB(header, payload, payload_size);
        case OP_CS_SET_SAMPLER:
            return HandleCSSetSampler(header, payload, payload_size);
        case OP_OM_SET_RTVS_AND_UAVS:
            return HandleOMSetRtvsAndUavs(header, payload, payload_size);
        case OP_STAGE_SET_SHADER:
            return HandleStageSetShader(header, payload, payload_size);
        case OP_STAGE_SET_CB:
        case OP_STAGE_SET_SRV:
        case OP_STAGE_SET_SAMPLER:
            return HandleStageBind(header, payload, payload_size);

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
        case OP_DRAW_INSTANCED:
            return HandleDrawInstanced(header, payload, payload_size);
        case OP_DRAW_INDEXED_INSTANCED:
            return HandleDrawIndexedInstanced(header, payload, payload_size);
        case OP_DISPATCH:
            return HandleDispatch(header, payload, payload_size);
        case OP_CLEAR_UAV_UINT:
        case OP_CLEAR_UAV_FLOAT:
            return HandleClearUav(header, payload, payload_size);
        case OP_COPY_RESOURCE:
            return HandleCopyResource(header, payload, payload_size);
        case OP_COPY_SUBRESOURCE_REGION:
            return HandleCopySubresourceRegion(header, payload, payload_size);

        // 5. Presentation
        case OP_PRESENT:
            return HandlePresent(header, payload, payload_size, socket);
        case OP_FRAME_DATA:
            return true;
        case OP_RESIZE_BUFFERS:
            return HandleResizeBuffers(header, payload, payload_size);

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
    m_frameCompression = (req->client_flags & CLIENT_FLAG_FRAME_COMPRESSION) != 0;
    m_previousFrame.clear(); // first frame of a session is always a key frame

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
    if (p->res_id == 0) {
        std::cerr << "[ServerDispatcher] CreateBuffer: invalid res_id=0" << std::endl;
        return false;
    }

    // Direct3D 11 buffer requirements
    constexpr uint32_t MAX_BUFFER_BYTES = 2048u * 1024u * 1024u; // 2 GB D3D11 limit
    if (p->byte_width == 0 || p->byte_width > MAX_BUFFER_BYTES) {
        std::cerr << "[ServerDispatcher] CreateBuffer: invalid byte_width: " << p->byte_width << std::endl;
        return false;
    }

    // Validate Usage enum (0: DEFAULT, 1: IMMUTABLE, 2: DYNAMIC, 3: STAGING)
    if (p->usage > D3D11_USAGE_STAGING) {
        std::cerr << "[ServerDispatcher] CreateBuffer: invalid usage: " << p->usage << std::endl;
        return false;
    }

    // Constant buffer specific alignment and size rules
    if (p->bind_flags & D3D11_BIND_CONSTANT_BUFFER) {
        if ((p->byte_width % 16) != 0 || p->byte_width > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16) {
            std::cerr << "[ServerDispatcher] CreateBuffer: constant buffer byte_width must be 16-byte aligned and <= 65536, got "
                      << p->byte_width << std::endl;
            return false;
        }
    }

    // Initial data bounds validation
    if (p->has_initial_data && p->initial_data_size > 0) {
        if (p->initial_data_size < p->byte_width) {
            std::cerr << "[ServerDispatcher] CreateBuffer: initial_data_size (" << p->initial_data_size 
                      << ") < byte_width (" << p->byte_width << ")" << std::endl;
            return false;
        }
        if (p->initial_data_size > size - sizeof(CreateBufferPayload)) {
            std::cerr << "[ServerDispatcher] CreateBuffer payload missing initial data" << std::endl;
            return false;
        }
    } else if (p->usage == D3D11_USAGE_IMMUTABLE) {
        std::cerr << "[ServerDispatcher] CreateBuffer: IMMUTABLE buffer requires initial data" << std::endl;
        return false;
    }

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
        D3D11_SUBRESOURCE_DATA init_data = {};
        init_data.pSysMem = payload + sizeof(CreateBufferPayload);
        hr = device->CreateBuffer(&desc, &init_data, &buffer);
    } else {
        hr = device->CreateBuffer(&desc, nullptr, &buffer);
    }

    if (FAILED(hr) && hr == E_OUTOFMEMORY) {
        ID3D11DeviceContext* ctx = m_gpuDevice.GetContext();
        if (ctx) {
            ctx->ClearState();
            ctx->Flush();
        }
        if (p->has_initial_data && p->initial_data_size > 0) {
            D3D11_SUBRESOURCE_DATA init_data = {};
            init_data.pSysMem = payload + sizeof(CreateBufferPayload);
            hr = device->CreateBuffer(&desc, &init_data, &buffer);
        } else {
            hr = device->CreateBuffer(&desc, nullptr, &buffer);
        }
    }

    if (FAILED(hr)) {
        static uint32_t s_bufFailCount = 0;
        if (s_bufFailCount++ < 10) {
            std::cerr << "[ServerDispatcher] CreateBuffer failed: 0x" << std::hex << hr << std::dec
                      << " (res_id=" << p->res_id << ")" << std::endl;
        }
        return true; // Don't abort the session
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
    if (p->res_id == 0) {
        std::cerr << "[ServerDispatcher] CreateTexture2D: invalid res_id=0" << std::endl;
        return false;
    }

    // Direct3D 11 Texture2D architectural limits
    if (p->width == 0 || p->width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        p->height == 0 || p->height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        std::cerr << "[ServerDispatcher] CreateTexture2D: invalid dimensions " << p->width << "x" << p->height << std::endl;
        return false;
    }

    if (p->array_size == 0 || p->array_size > D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION) {
        std::cerr << "[ServerDispatcher] CreateTexture2D: invalid array_size: " << p->array_size << std::endl;
        return false;
    }

    if (p->mips > D3D11_REQ_MIP_LEVELS) {
        std::cerr << "[ServerDispatcher] CreateTexture2D: invalid mips: " << p->mips << std::endl;
        return false;
    }

    if (p->format == DXGI_FORMAT_UNKNOWN || !IsValidDxgiFormat(p->format)) {
        std::cerr << "[ServerDispatcher] CreateTexture2D: invalid format: " << p->format << std::endl;
        return false;
    }

    if (p->usage > D3D11_USAGE_STAGING) {
        std::cerr << "[ServerDispatcher] CreateTexture2D: invalid usage: " << p->usage << std::endl;
        return false;
    }

    // Initial data: one D3D11_SUBRESOURCE_DATA per subresource (mip levels x array slices). Each
    // entry is checked against the exact byte count Direct3D will read, so a client can never make
    // the driver read past the received packet.
    std::vector<D3D11_SUBRESOURCE_DATA> init_data;
    if (p->has_initial_data != TEXTURE_INITIAL_DATA_NONE) {
        const uint32_t mip_levels = p->mips ? p->mips : FullMipCount(p->width, p->height);
        if (mip_levels > FullMipCount(p->width, p->height)) {
            std::cerr << "[ServerDispatcher] CreateTexture2D: " << mip_levels << " mips exceed the full chain" << std::endl;
            return false;
        }
        const size_t count = static_cast<size_t>(mip_levels) * p->array_size;

        std::vector<RgpuSubresourceData> table;
        const uint8_t* blob = nullptr;
        if (p->has_initial_data == TEXTURE_INITIAL_DATA_SINGLE) {
            if (count != 1) {
                std::cerr << "[ServerDispatcher] CreateTexture2D: single initial-data entry for " << count
                          << " subresources" << std::endl;
                return false;
            }
            table.push_back({p->sys_mem_pitch, p->sys_mem_slice_pitch, 0, p->initial_data_size});
            blob = payload + sizeof(CreateTexture2DPayload);
        } else if (p->has_initial_data == TEXTURE_INITIAL_DATA_SUBRESOURCES) {
            const size_t header_end = sizeof(CreateTexture2DPayload) + sizeof(uint32_t);
            if (size < header_end) return false;
            uint32_t declared = 0;
            std::memcpy(&declared, payload + sizeof(CreateTexture2DPayload), sizeof(declared));
            if (declared != count || size - header_end < count * sizeof(RgpuSubresourceData)) {
                std::cerr << "[ServerDispatcher] CreateTexture2D: subresource table mismatch (declared " << declared
                          << ", expected " << count << ")" << std::endl;
                return false;
            }
            table.resize(count);
            std::memcpy(table.data(), payload + header_end, count * sizeof(RgpuSubresourceData));
            blob = payload + header_end + count * sizeof(RgpuSubresourceData);
        } else {
            std::cerr << "[ServerDispatcher] CreateTexture2D: invalid has_initial_data " << p->has_initial_data << std::endl;
            return false;
        }

        const size_t blob_available = size - static_cast<size_t>(blob - payload);
        if (p->initial_data_size > blob_available) {
            std::cerr << "[ServerDispatcher] CreateTexture2D payload missing initial data" << std::endl;
            return false;
        }
        init_data.resize(count);
        for (size_t i = 0; i < count; ++i) {
            const uint32_t mip = static_cast<uint32_t>(i % mip_levels);
            uint64_t row_bytes = 0;
            uint32_t rows = 0;
            if (!SurfaceLayout(p->format, MipSize(p->width, mip), MipSize(p->height, mip), row_bytes, rows)) {
                std::cerr << "[ServerDispatcher] CreateTexture2D: initial data unsupported for format " << p->format << std::endl;
                return false;
            }
            const RgpuSubresourceData& s = table[i];
            const uint64_t needed = static_cast<uint64_t>(s.row_pitch) * (rows - 1) + row_bytes;
            if (s.row_pitch < row_bytes || s.size < needed ||
                static_cast<uint64_t>(s.offset) + s.size > p->initial_data_size) {
                std::cerr << "[ServerDispatcher] CreateTexture2D: subresource " << i << " data out of bounds (pitch "
                          << s.row_pitch << ", size " << s.size << ", needs " << needed << ")" << std::endl;
                return false;
            }
            init_data[i].pSysMem = blob + s.offset;
            init_data[i].SysMemPitch = s.row_pitch;
            init_data[i].SysMemSlicePitch = s.slice_pitch;
        }
    } else if (p->usage == D3D11_USAGE_IMMUTABLE) {
        std::cerr << "[ServerDispatcher] CreateTexture2D: IMMUTABLE texture requires initial data" << std::endl;
        return false;
    }

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

    // Direct3D 11 rule: Staging textures CANNOT have any bind flags
    if (desc.Usage == D3D11_USAGE_STAGING) {
        desc.BindFlags = 0;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = S_OK;

    hr = device->CreateTexture2D(&desc, init_data.empty() ? nullptr : init_data.data(), &texture);
    if (FAILED(hr) && hr == E_OUTOFMEMORY) {
        ID3D11DeviceContext* ctx = m_gpuDevice.GetContext();
        if (ctx) {
            ctx->ClearState();
            ctx->Flush();
        }
        hr = device->CreateTexture2D(&desc, init_data.empty() ? nullptr : init_data.data(), &texture);
    }

    if (FAILED(hr)) {
        static uint32_t s_tex2DFailCount = 0;
        if (s_tex2DFailCount++ < 10) {
            std::cerr << "[ServerDispatcher] CreateTexture2D failed: 0x" << std::hex << hr << std::dec
                      << " (res_id=" << p->res_id << ", " << p->width << "x" << p->height
                      << ", fmt=" << p->format << ", mips=" << p->mips
                      << ", arr=" << p->array_size << ", usage=" << p->usage
                      << ", bind=0x" << std::hex << p->bind_flags << std::dec << ")" << std::endl;
        }
        return true; // Don't abort the session
    }

    m_resources.Register(p->res_id, texture);
    return true;
}

bool ServerDispatcher::HandleCreateTexture3D(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateTexture3DPayload)) {
        std::cerr << "[ServerDispatcher] CreateTexture3D payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const CreateTexture3DPayload*>(payload);
    if (p->res_id == 0 || p->width == 0 || p->height == 0 || p->depth == 0) {
        std::cerr << "[ServerDispatcher] CreateTexture3D: invalid parameters (res_id=" << p->res_id
                  << ", " << p->width << "x" << p->height << "x" << p->depth << ")" << std::endl;
        return false;
    }

    if (!IsValidDxgiFormat(p->format)) {
        std::cerr << "[ServerDispatcher] CreateTexture3D: invalid format: " << p->format << std::endl;
        return false;
    }

    if (p->usage > D3D11_USAGE_STAGING) {
        std::cerr << "[ServerDispatcher] CreateTexture3D: invalid usage: " << p->usage << std::endl;
        return false;
    }

    std::vector<D3D11_SUBRESOURCE_DATA> init_data;
    if (p->has_initial_data != TEXTURE_INITIAL_DATA_NONE) {
        const uint32_t mip_levels = p->mips ? p->mips : FullMipCount3D(p->width, p->height, p->depth);
        if (mip_levels > FullMipCount3D(p->width, p->height, p->depth)) {
            std::cerr << "[ServerDispatcher] CreateTexture3D: " << mip_levels << " mips exceed the full chain" << std::endl;
            return false;
        }
        const size_t count = static_cast<size_t>(mip_levels);

        std::vector<RgpuSubresourceData> table;
        const uint8_t* blob = nullptr;
        if (p->has_initial_data == TEXTURE_INITIAL_DATA_SINGLE) {
            if (count != 1) {
                std::cerr << "[ServerDispatcher] CreateTexture3D: single initial-data entry for " << count
                          << " subresources" << std::endl;
                return false;
            }
            table.push_back({p->sys_mem_pitch, p->sys_mem_slice_pitch, 0, p->initial_data_size});
            blob = payload + sizeof(CreateTexture3DPayload);
        } else if (p->has_initial_data == TEXTURE_INITIAL_DATA_SUBRESOURCES) {
            const size_t header_end = sizeof(CreateTexture3DPayload) + sizeof(uint32_t);
            if (size < header_end) return false;
            uint32_t declared = 0;
            std::memcpy(&declared, payload + sizeof(CreateTexture3DPayload), sizeof(declared));
            if (declared != count || size - header_end < count * sizeof(RgpuSubresourceData)) {
                std::cerr << "[ServerDispatcher] CreateTexture3D: subresource table mismatch" << std::endl;
                return false;
            }
            table.resize(count);
            std::memcpy(table.data(), payload + header_end, count * sizeof(RgpuSubresourceData));
            blob = payload + header_end + count * sizeof(RgpuSubresourceData);
        } else {
            std::cerr << "[ServerDispatcher] CreateTexture3D: invalid has_initial_data " << p->has_initial_data << std::endl;
            return false;
        }

        const size_t blob_available = size - static_cast<size_t>(blob - payload);
        if (p->initial_data_size > blob_available) {
            std::cerr << "[ServerDispatcher] CreateTexture3D payload missing initial data" << std::endl;
            return false;
        }
        init_data.resize(count);
        for (size_t i = 0; i < count; ++i) {
            const uint32_t mip = static_cast<uint32_t>(i);
            const uint32_t mw = MipSize(p->width, mip);
            const uint32_t mh = MipSize(p->height, mip);
            const uint32_t md = MipSize(p->depth, mip);
            uint64_t row_bytes = 0;
            uint32_t rows = 0;
            if (!SurfaceLayout(p->format, mw, mh, row_bytes, rows)) {
                std::cerr << "[ServerDispatcher] CreateTexture3D: initial data unsupported for format " << p->format << std::endl;
                return false;
            }
            const RgpuSubresourceData& s = table[i];
            const uint64_t slice_bytes = (rows > 1) ? (static_cast<uint64_t>(s.row_pitch) * (rows - 1) + row_bytes) : row_bytes;
            const uint64_t needed = (md > 1) ? (static_cast<uint64_t>(s.slice_pitch) * (md - 1) + slice_bytes) : slice_bytes;
            if (s.row_pitch < row_bytes || s.size < needed ||
                static_cast<uint64_t>(s.offset) + s.size > p->initial_data_size) {
                std::cerr << "[ServerDispatcher] CreateTexture3D: subresource " << i << " data out of bounds" << std::endl;
                return false;
            }
            init_data[i].pSysMem = blob + s.offset;
            init_data[i].SysMemPitch = s.row_pitch;
            init_data[i].SysMemSlicePitch = s.slice_pitch;
        }
    } else if (p->usage == D3D11_USAGE_IMMUTABLE) {
        std::cerr << "[ServerDispatcher] CreateTexture3D: IMMUTABLE texture requires initial data" << std::endl;
        return false;
    }

    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    D3D11_TEXTURE3D_DESC desc = {};
    desc.Width = p->width;
    desc.Height = p->height;
    desc.Depth = p->depth;
    desc.MipLevels = p->mips;
    desc.Format = static_cast<DXGI_FORMAT>(p->format);
    desc.Usage = static_cast<D3D11_USAGE>(p->usage);
    desc.BindFlags = p->bind_flags;
    desc.CPUAccessFlags = p->cpu_access_flags;
    desc.MiscFlags = p->misc_flags;

    Microsoft::WRL::ComPtr<ID3D11Texture3D> texture;
    HRESULT hr = device->CreateTexture3D(&desc, init_data.empty() ? nullptr : init_data.data(), &texture);
    if (FAILED(hr) && hr == E_OUTOFMEMORY) {
        ID3D11DeviceContext* ctx = m_gpuDevice.GetContext();
        if (ctx) { ctx->ClearState(); ctx->Flush(); }
        hr = device->CreateTexture3D(&desc, init_data.empty() ? nullptr : init_data.data(), &texture);
    }
    if (FAILED(hr)) {
        static uint32_t s_tex3DFailCount = 0;
        if (s_tex3DFailCount++ < 10) {
            std::cerr << "[ServerDispatcher] CreateTexture3D failed: 0x" << std::hex << hr << std::dec
                      << " (res_id=" << p->res_id << ")" << std::endl;
        }
        return true; // Don't abort the session
    }

    m_resources.Register(p->res_id, texture);
    return true;
}

bool ServerDispatcher::HandleCreateTexture1D(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateTexture1DPayload)) {
        std::cerr << "[ServerDispatcher] CreateTexture1D payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const CreateTexture1DPayload*>(payload);
    if (p->res_id == 0 || p->width == 0 || p->array_size == 0) {
        std::cerr << "[ServerDispatcher] CreateTexture1D: invalid parameters (res_id=" << p->res_id
                  << ", " << p->width << ")" << std::endl;
        return false;
    }

    if (!IsValidDxgiFormat(p->format)) {
        std::cerr << "[ServerDispatcher] CreateTexture1D: invalid format: " << p->format << std::endl;
        return false;
    }

    if (p->usage > D3D11_USAGE_STAGING) {
        std::cerr << "[ServerDispatcher] CreateTexture1D: invalid usage: " << p->usage << std::endl;
        return false;
    }

    std::vector<D3D11_SUBRESOURCE_DATA> init_data;
    if (p->has_initial_data != TEXTURE_INITIAL_DATA_NONE) {
        const uint32_t mip_levels = p->mips ? p->mips : FullMipCount1D(p->width);
        const size_t count = static_cast<size_t>(mip_levels) * p->array_size;

        std::vector<RgpuSubresourceData> table;
        const uint8_t* blob = nullptr;
        if (p->has_initial_data == TEXTURE_INITIAL_DATA_SINGLE) {
            if (count != 1) return false;
            table.push_back({p->sys_mem_pitch, 0, 0, p->initial_data_size});
            blob = payload + sizeof(CreateTexture1DPayload);
        } else if (p->has_initial_data == TEXTURE_INITIAL_DATA_SUBRESOURCES) {
            const size_t header_end = sizeof(CreateTexture1DPayload) + sizeof(uint32_t);
            if (size < header_end) return false;
            uint32_t declared = 0;
            std::memcpy(&declared, payload + sizeof(CreateTexture1DPayload), sizeof(declared));
            if (declared != count || size - header_end < count * sizeof(RgpuSubresourceData)) return false;
            table.resize(count);
            std::memcpy(table.data(), payload + header_end, count * sizeof(RgpuSubresourceData));
            blob = payload + header_end + count * sizeof(RgpuSubresourceData);
        } else {
            return false;
        }

        const size_t blob_available = size - static_cast<size_t>(blob - payload);
        if (p->initial_data_size > blob_available) return false;
        init_data.resize(count);
        for (size_t i = 0; i < count; ++i) {
            const uint32_t mip = static_cast<uint32_t>(i % mip_levels);
            const uint32_t mw = MipSize(p->width, mip);
            uint64_t row_bytes = 0;
            uint32_t rows = 0;
            if (!SurfaceLayout(p->format, mw, 1, row_bytes, rows)) return false;
            const RgpuSubresourceData& s = table[i];
            if (s.size < row_bytes || static_cast<uint64_t>(s.offset) + s.size > p->initial_data_size) return false;
            init_data[i].pSysMem = blob + s.offset;
            init_data[i].SysMemPitch = s.row_pitch;
            init_data[i].SysMemSlicePitch = 0;
        }
    } else if (p->usage == D3D11_USAGE_IMMUTABLE) {
        return false;
    }

    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    D3D11_TEXTURE1D_DESC desc = {};
    desc.Width = p->width;
    desc.MipLevels = p->mips;
    desc.ArraySize = p->array_size;
    desc.Format = static_cast<DXGI_FORMAT>(p->format);
    desc.Usage = static_cast<D3D11_USAGE>(p->usage);
    desc.BindFlags = p->bind_flags;
    desc.CPUAccessFlags = p->cpu_access_flags;
    desc.MiscFlags = p->misc_flags;

    Microsoft::WRL::ComPtr<ID3D11Texture1D> texture;
    HRESULT hr = device->CreateTexture1D(&desc, init_data.empty() ? nullptr : init_data.data(), &texture);
    if (FAILED(hr) && hr == E_OUTOFMEMORY) {
        ID3D11DeviceContext* ctx = m_gpuDevice.GetContext();
        if (ctx) { ctx->ClearState(); ctx->Flush(); }
        hr = device->CreateTexture1D(&desc, init_data.empty() ? nullptr : init_data.data(), &texture);
    }
    if (FAILED(hr)) {
        static uint32_t s_tex1DFailCount = 0;
        if (s_tex1DFailCount++ < 10) {
            std::cerr << "[ServerDispatcher] CreateTexture1D failed: 0x" << std::hex << hr << std::dec
                      << " (res_id=" << p->res_id << ")" << std::endl;
        }
        return true; // Don't abort the session
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
    if (p->res_id == 0) {
        std::cerr << "[ServerDispatcher] CreateSrv: invalid res_id=0" << std::endl;
        return false;
    }

    bool use_desc = (p->format != 0 || p->view_dimension != 0);
    if (use_desc) {
        if (p->format != 0 && !IsValidDxgiFormat(p->format)) {
            std::cerr << "[ServerDispatcher] CreateSrv: invalid format: " << p->format << std::endl;
            return false;
        }
        if (p->view_dimension > D3D11_SRV_DIMENSION_BUFFEREX) {
            std::cerr << "[ServerDispatcher] CreateSrv: invalid view_dimension: " << p->view_dimension << std::endl;
            return false;
        }
    }

    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    if (p->resource_id == 0) {
        resource = m_gpuDevice.GetColorTexture();
    } else {
        resource = m_resources.Get<ID3D11Resource>(p->resource_id);
    }
    if (!resource) {
        std::cerr << "[ServerDispatcher] CreateSrv: underlying resource not found: " << p->resource_id << std::endl;
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
    if (use_desc) {
        desc.Format = static_cast<DXGI_FORMAT>(p->format);
        desc.ViewDimension = static_cast<D3D11_SRV_DIMENSION>(p->view_dimension);
        // The view union is four UINTs at most (see CreateSrvArrayRange); copying them verbatim
        // serves every dimension: buffers, 1D/2D/3D textures, arrays, cubes, multisampled.
        CreateSrvArrayRange range = {0, 1};
        if (size >= sizeof(CreateSrvPayload) + sizeof(CreateSrvArrayRange)) {
            std::memcpy(&range, payload + sizeof(CreateSrvPayload), sizeof(range));
        } else if (desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY) {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
            D3D11_TEXTURE2D_DESC td = {};
            if (SUCCEEDED(resource.As(&tex))) tex->GetDesc(&td);
            range.array_size = td.ArraySize ? td.ArraySize : 1; // older clients: whole array
        }
        const uint32_t fields[4] = {p->most_detailed_mip, p->mip_levels, range.first_array_slice, range.array_size};
        static_assert(sizeof(desc.Texture2DArray) == sizeof(fields), "SRV union is four UINTs");
        std::memcpy(&desc.Texture2DArray, fields, sizeof(fields));
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = device->CreateShaderResourceView(resource.Get(), use_desc ? &desc : nullptr, &srv);
    if (FAILED(hr) && use_desc) {
        // Fallback to default view for the resource
        hr = device->CreateShaderResourceView(resource.Get(), nullptr, &srv);
    }
    if (FAILED(hr)) {
        std::cerr << "[ServerDispatcher] CreateShaderResourceView failed: 0x" << std::hex << hr << std::dec
                  << " (res_id=" << p->res_id << ")" << std::endl;
        return true; // Don't abort the connection
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
    if (p->res_id == 0) {
        std::cerr << "[ServerDispatcher] CreateRtv: invalid res_id=0" << std::endl;
        return false;
    }

    bool use_desc = (p->format != 0);
    if (use_desc) {
        if (!IsValidDxgiFormat(p->format)) {
            std::cerr << "[ServerDispatcher] CreateRtv: invalid format: " << p->format << std::endl;
            return false;
        }
        if (p->view_dimension > D3D11_RTV_DIMENSION_TEXTURE3D) {
            std::cerr << "[ServerDispatcher] CreateRtv: invalid view_dimension: " << p->view_dimension << std::endl;
            return false;
        }
    }

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
        if (use_desc) {
            desc.Format = static_cast<DXGI_FORMAT>(p->format);
            desc.ViewDimension = static_cast<D3D11_RTV_DIMENSION>(p->view_dimension);
            if (desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) {
                Microsoft::WRL::ComPtr<ID3D11Texture2D> tex2D;
                if (SUCCEEDED(resource.As(&tex2D))) {
                    D3D11_TEXTURE2D_DESC td{};
                    tex2D->GetDesc(&td);
                    desc.Texture2DArray.MipSlice = p->mip_slice;
                    desc.Texture2DArray.FirstArraySlice = 0;
                    desc.Texture2DArray.ArraySize = td.ArraySize ? td.ArraySize : 1;
                } else {
                    desc.Texture2DArray.MipSlice = p->mip_slice;
                    desc.Texture2DArray.FirstArraySlice = 0;
                    desc.Texture2DArray.ArraySize = 1;
                }
            } else if (desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE3D) {
                Microsoft::WRL::ComPtr<ID3D11Texture3D> tex3D;
                if (SUCCEEDED(resource.As(&tex3D))) {
                    D3D11_TEXTURE3D_DESC td{};
                    tex3D->GetDesc(&td);
                    desc.Texture3D.MipSlice = p->mip_slice;
                    desc.Texture3D.FirstWSlice = 0;
                    desc.Texture3D.WSize = td.Depth ? td.Depth : 1;
                } else {
                    desc.Texture3D.MipSlice = p->mip_slice;
                    desc.Texture3D.FirstWSlice = 0;
                    desc.Texture3D.WSize = 1;
                }
            } else {
                desc.Texture2D.MipSlice = p->mip_slice;
            }
        }

        HRESULT hr = device->CreateRenderTargetView(resource.Get(), use_desc ? &desc : nullptr, &rtv);
        if (FAILED(hr) && use_desc) {
            // Fallback to default view for the resource
            hr = device->CreateRenderTargetView(resource.Get(), nullptr, &rtv);
        }
        if (FAILED(hr)) {
            std::cerr << "[ServerDispatcher] CreateRenderTargetView failed: 0x" << std::hex << hr << std::dec
                      << " (res_id=" << p->res_id << ")" << std::endl;
            return true; // Don't abort the connection
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
    if (p->res_id == 0) {
        std::cerr << "[ServerDispatcher] CreateDsv: invalid res_id=0" << std::endl;
        return false;
    }

    bool use_desc = (p->format != 0);
    if (use_desc) {
        if (!IsValidDxgiFormat(p->format)) {
            std::cerr << "[ServerDispatcher] CreateDsv: invalid format: " << p->format << std::endl;
            return false;
        }
        if (p->view_dimension > D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY) {
            std::cerr << "[ServerDispatcher] CreateDsv: invalid view_dimension: " << p->view_dimension << std::endl;
            return false;
        }
    }

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
                static uint32_t s_dsvMissingRes = 0;
                if (s_dsvMissingRes++ < 10) {
                    std::cerr << "[ServerDispatcher] CreateDsv: resource not found: " << p->resource_id << std::endl;
                }
                return true; // Don't abort the session
            }
        }

        D3D11_DEPTH_STENCIL_VIEW_DESC desc = {};
        if (use_desc) {
            desc.Format = static_cast<DXGI_FORMAT>(p->format);
            // Convert typeless formats to appropriate depth-stencil view formats
            switch (desc.Format) {
                case DXGI_FORMAT_R32G8X24_TYPELESS: desc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; break;
                case DXGI_FORMAT_R32_TYPELESS:      desc.Format = DXGI_FORMAT_D32_FLOAT; break;
                case DXGI_FORMAT_R24G8_TYPELESS:    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; break;
                case DXGI_FORMAT_R16_TYPELESS:      desc.Format = DXGI_FORMAT_D16_UNORM; break;
                default: break;
            }

            desc.ViewDimension = static_cast<D3D11_DSV_DIMENSION>(p->view_dimension);
            desc.Flags = p->flags;

            if (desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY) {
                Microsoft::WRL::ComPtr<ID3D11Texture2D> tex2D;
                if (SUCCEEDED(resource.As(&tex2D))) {
                    D3D11_TEXTURE2D_DESC td{};
                    tex2D->GetDesc(&td);
                    desc.Texture2DArray.MipSlice = p->mip_slice;
                    desc.Texture2DArray.FirstArraySlice = 0;
                    desc.Texture2DArray.ArraySize = td.ArraySize ? td.ArraySize : 1;
                } else {
                    desc.Texture2DArray.MipSlice = p->mip_slice;
                    desc.Texture2DArray.FirstArraySlice = 0;
                    desc.Texture2DArray.ArraySize = 1;
                }
            } else if (desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE1DARRAY) {
                desc.Texture1DArray.MipSlice = p->mip_slice;
                desc.Texture1DArray.FirstArraySlice = 0;
                desc.Texture1DArray.ArraySize = 1;
            } else if (desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY) {
                Microsoft::WRL::ComPtr<ID3D11Texture2D> tex2D;
                if (SUCCEEDED(resource.As(&tex2D))) {
                    D3D11_TEXTURE2D_DESC td{};
                    tex2D->GetDesc(&td);
                    desc.Texture2DMSArray.FirstArraySlice = 0;
                    desc.Texture2DMSArray.ArraySize = td.ArraySize ? td.ArraySize : 1;
                } else {
                    desc.Texture2DMSArray.FirstArraySlice = 0;
                    desc.Texture2DMSArray.ArraySize = 1;
                }
            } else if (desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE1D) {
                desc.Texture1D.MipSlice = p->mip_slice;
            } else {
                desc.Texture2D.MipSlice = p->mip_slice;
            }
        }

        HRESULT hr = device->CreateDepthStencilView(resource.Get(), use_desc ? &desc : nullptr, &dsv);
        if (FAILED(hr) && use_desc) {
            // Fallback to default DSV
            hr = device->CreateDepthStencilView(resource.Get(), nullptr, &dsv);
        }
        if (FAILED(hr)) {
            static uint32_t s_dsvFailures = 0;
            if (s_dsvFailures++ < 10) {
                std::cerr << "[ServerDispatcher] CreateDepthStencilView failed: 0x" << std::hex << hr << std::dec
                          << " (res_id=" << p->res_id << ")" << std::endl;
            }
            return true; // Don't abort the connection
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
    if (p->res_id == 0) {
        std::cerr << "[ServerDispatcher] CreateVertexShader: invalid res_id=0" << std::endl;
        return false;
    }

    if (p->bytecode_length == 0 || p->bytecode_length > 64 * 1024 * 1024) {
        std::cerr << "[ServerDispatcher] CreateVertexShader: invalid bytecode_length: " << p->bytecode_length << std::endl;
        return false;
    }

    if (p->bytecode_length > size - sizeof(CreateShaderPayload)) {
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
    if (p->res_id == 0) {
        std::cerr << "[ServerDispatcher] CreatePixelShader: invalid res_id=0" << std::endl;
        return false;
    }

    if (p->bytecode_length == 0 || p->bytecode_length > 64 * 1024 * 1024) {
        std::cerr << "[ServerDispatcher] CreatePixelShader: invalid bytecode_length: " << p->bytecode_length << std::endl;
        return false;
    }

    if (p->bytecode_length > size - sizeof(CreateShaderPayload)) {
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
    if (p->res_id == 0) {
        std::cerr << "[ServerDispatcher] CreateInputLayout: invalid res_id=0" << std::endl;
        return false;
    }

    // Direct3D 11 architecture enforces a maximum of 32 input elements
    if (p->num_elements > D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT) {
        std::cerr << "[ServerDispatcher] CreateInputLayout: num_elements (" << p->num_elements 
                  << ") exceeds D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT (" 
                  << D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT << ")" << std::endl;
        return false;
    }

    size_t elements_size = static_cast<size_t>(p->num_elements) * sizeof(RgpuInputElementDesc);
    size_t expected_total = sizeof(CreateInputLayoutPayload) + elements_size + static_cast<size_t>(p->bytecode_length);

    if (size < expected_total) {
        std::cerr << "[ServerDispatcher] CreateInputLayout payload truncated (expected at least " 
                  << expected_total << ", got " << size << ")" << std::endl;
        return false;
    }

    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    const auto* src_elements = reinterpret_cast<const RgpuInputElementDesc*>(payload + sizeof(CreateInputLayoutPayload));
    std::vector<D3D11_INPUT_ELEMENT_DESC> d3d_elements(p->num_elements);
    std::vector<std::string> semantic_names(p->num_elements);

    for (uint32_t i = 0; i < p->num_elements; ++i) {
        // Safe bounded string copy: prevent buffer over-read if semantic name is not null-terminated
        size_t name_len = strnlen(src_elements[i].semantic_name, sizeof(src_elements[i].semantic_name));
        if (name_len == 0) {
            std::cerr << "[ServerDispatcher] CreateInputLayout: empty semantic name at element " << i << std::endl;
            return false;
        }
        semantic_names[i].assign(src_elements[i].semantic_name, name_len);
        d3d_elements[i].SemanticName = semantic_names[i].c_str();

        d3d_elements[i].SemanticIndex = src_elements[i].semantic_index;

        // Format enum validation
        if (src_elements[i].format == DXGI_FORMAT_UNKNOWN || !IsValidDxgiFormat(src_elements[i].format)) {
            std::cerr << "[ServerDispatcher] CreateInputLayout: invalid format " << src_elements[i].format 
                      << " at element " << i << std::endl;
            return false;
        }
        d3d_elements[i].Format = static_cast<DXGI_FORMAT>(src_elements[i].format);

        // Vertex buffer input slot validation (< 32)
        if (src_elements[i].input_slot >= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) {
            std::cerr << "[ServerDispatcher] CreateInputLayout: input_slot " << src_elements[i].input_slot 
                      << " exceeds D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT at element " << i << std::endl;
            return false;
        }
        d3d_elements[i].InputSlot = src_elements[i].input_slot;

        d3d_elements[i].AlignedByteOffset = src_elements[i].aligned_byte_offset;

        // Input classification validation (0: PER_VERTEX_DATA, 1: PER_INSTANCE_DATA)
        if (src_elements[i].input_slot_class > D3D11_INPUT_PER_INSTANCE_DATA) {
            std::cerr << "[ServerDispatcher] CreateInputLayout: invalid input_slot_class " 
                      << src_elements[i].input_slot_class << " at element " << i << std::endl;
            return false;
        }
        d3d_elements[i].InputSlotClass = static_cast<D3D11_INPUT_CLASSIFICATION>(src_elements[i].input_slot_class);
        d3d_elements[i].InstanceDataStepRate = src_elements[i].instance_data_step;
    }

    // Direct3D 11 requires non-empty shader bytecode with input signature to create layout
    if (p->bytecode_length == 0) {
        std::cerr << "[ServerDispatcher] CreateInputLayout: bytecode_length is 0" << std::endl;
        return false;
    }

    const void* bytecode = payload + sizeof(CreateInputLayoutPayload) + elements_size;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;
    const D3D11_INPUT_ELEMENT_DESC* p_descs = (p->num_elements > 0) ? d3d_elements.data() : nullptr;
    HRESULT hr = device->CreateInputLayout(p_descs, p->num_elements, bytecode, p->bytecode_length, &layout);
    if (FAILED(hr)) {
        static uint32_t s_layoutFailures = 0;
        if (s_layoutFailures++ < 10) {
            std::cerr << "[ServerDispatcher] CreateInputLayout failed: 0x" << std::hex << hr << std::dec
                      << " (res_id=" << p->res_id << ")" << std::endl;
        }
        return true; // Don't abort the session
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
    if (p->res_id == 0) {
        std::cerr << "[ServerDispatcher] CreateSampler: invalid res_id=0" << std::endl;
        return false;
    }

    // Direct3D 11 texture address modes: 1 (WRAP) to 5 (MIRROR_ONCE)
    if (p->address_u < 1 || p->address_u > 5 ||
        p->address_v < 1 || p->address_v > 5 ||
        p->address_w < 1 || p->address_w > 5) {
        std::cerr << "[ServerDispatcher] CreateSampler: invalid address mode (u=" 
                  << p->address_u << ", v=" << p->address_v << ", w=" << p->address_w << ")" << std::endl;
        return false;
    }

    // Direct3D 11 comparison functions: 0 (default to NEVER), 1 (NEVER) to 8 (ALWAYS)
    if (p->comparison_func > 8) {
        std::cerr << "[ServerDispatcher] CreateSampler: invalid comparison_func: " << p->comparison_func << std::endl;
        return false;
    }

    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = static_cast<D3D11_FILTER>(p->filter);
    desc.AddressU = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(p->address_u);
    desc.AddressV = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(p->address_v);
    desc.AddressW = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(p->address_w);
    desc.MipLODBias = p->mip_lod_bias;
    desc.MaxAnisotropy = (std::min)(p->max_anisotropy, 16u);
    desc.ComparisonFunc = (p->comparison_func == 0) ? D3D11_COMPARISON_NEVER : static_cast<D3D11_COMPARISON_FUNC>(p->comparison_func);
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
    if (p->res_id != 0) {
        m_resources.Destroy(p->res_id);
    }
    return true; // Destroying a resource is idempotent
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

    if (p->slot >= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) {
        std::cerr << "[ServerDispatcher] Invalid vertex buffer slot: " << p->slot 
                  << " (limit: " << D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT << ")" << std::endl;
        return false;
    }

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

    if (p->format != DXGI_FORMAT_R16_UINT && 
        p->format != DXGI_FORMAT_R32_UINT && 
        p->format != DXGI_FORMAT_UNKNOWN) {
        std::cerr << "[ServerDispatcher] Invalid index buffer format: " << p->format << std::endl;
        return false;
    }

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer = (p->res_id == 0) ? nullptr : m_resources.GetBuffer(p->res_id);
    context->IASetIndexBuffer(buffer.Get(), static_cast<DXGI_FORMAT>(p->format), p->offset);
    return true;
}

bool ServerDispatcher::HandleIASetTopology(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(IASetTopologyPayload)) return false;
    const auto* p = reinterpret_cast<const IASetTopologyPayload*>(payload);

    if (!IsValidD3D11PrimitiveTopology(p->topology)) {
        std::cerr << "[ServerDispatcher] Invalid primitive topology: " << p->topology << std::endl;
        return false;
    }

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

    if (p->slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) {
        std::cerr << "[ServerDispatcher] Invalid VS constant buffer slot: " << p->slot 
                  << " (limit: " << D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT << ")" << std::endl;
        return false;
    }

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

    if (p->slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) {
        std::cerr << "[ServerDispatcher] Invalid PS constant buffer slot: " << p->slot 
                  << " (limit: " << D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT << ")" << std::endl;
        return false;
    }

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

    if (p->slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        std::cerr << "[ServerDispatcher] Invalid PS SRV slot: " << p->slot 
                  << " (limit: " << D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT << ")" << std::endl;
        return false;
    }

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

    if (p->slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        std::cerr << "[ServerDispatcher] Invalid PS sampler slot: " << p->slot 
                  << " (limit: " << D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT << ")" << std::endl;
        return false;
    }

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

    UINT num_viewports = static_cast<UINT>(size / sizeof(RSSetViewportPayload));
    if (num_viewports > D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) { // 16
        std::cerr << "[ServerDispatcher] Viewport count exceeds limit: " << num_viewports << std::endl;
        return false;
    }

    // Validate floating point sanity
    if (!std::isfinite(p->width) || !std::isfinite(p->height) ||
        !std::isfinite(p->top_left_x) || !std::isfinite(p->top_left_y) ||
        !std::isfinite(p->min_depth) || !std::isfinite(p->max_depth)) {
        std::cerr << "[ServerDispatcher] Viewport contains non-finite float values" << std::endl;
        return false;
    }

    if (p->width < 0.0f || p->height < 0.0f || p->width > 32767.0f || p->height > 32767.0f) {
        std::cerr << "[ServerDispatcher] Viewport dimensions out of bounds: " << p->width << "x" << p->height << std::endl;
        return false;
    }

    if (p->min_depth < 0.0f || p->max_depth > 1.0f || p->min_depth > p->max_depth) {
        std::cerr << "[ServerDispatcher] Viewport depth range invalid: [" << p->min_depth << ".." << p->max_depth << "]" << std::endl;
        return false;
    }

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

    // dsv_id == 0 means "no depth buffer", exactly like a null DSV in D3D11. Falling back to the
    // host depth buffer here would depth-test against a buffer the client never clears.
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
    if (p->dsv_id != 0) {
        dsv = m_resources.GetDSV(p->dsv_id);
    }

    ID3D11RenderTargetView* p_rtv = rtv.Get();
    UINT num_rtvs = p_rtv ? 1 : 0;
    if (num_rtvs > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT) {
        std::cerr << "[ServerDispatcher] Render target count exceeds limit: " << num_rtvs << std::endl;
        return false;
    }

    context->OMSetRenderTargets(num_rtvs, p_rtv ? &p_rtv : nullptr, dsv.Get());
    return true;
}

// =============================================================================
// Updates & Draw Call Handlers
// =============================================================================

bool ServerDispatcher::HandleUpdateSubresource(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(UpdateSubresourcePayload)) return false;
    const auto* p = reinterpret_cast<const UpdateSubresourcePayload*>(payload);

    if (p->res_id == 0) return false;

    if (size < sizeof(UpdateSubresourcePayload) + p->data_size) {
        std::cerr << "[ServerDispatcher] UpdateSubresource data truncated" << std::endl;
        return false;
    }

    if (p->has_dst_box) {
        if (p->dst_box.right <= p->dst_box.left ||
            p->dst_box.bottom <= p->dst_box.top ||
            p->dst_box.back <= p->dst_box.front) {
            std::cerr << "[ServerDispatcher] UpdateSubresource invalid dst_box" << std::endl;
            return false;
        }
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

    // Direct3D reads the source by pitch and box without knowing how many bytes arrived, and
    // does not bound-check the box: compute the exact read size and check it here.
    uint64_t needed = 0;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
        res.As(&buffer);
        D3D11_BUFFER_DESC bd = {};
        buffer->GetDesc(&bd);
        if (p->subresource != 0 || (p_box && box.right > bd.ByteWidth)) {
            std::cerr << "[ServerDispatcher] UpdateSubresource: buffer range out of bounds" << std::endl;
            return false;
        }
        needed = p_box ? (box.right - box.left) : bd.ByteWidth;
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        res.As(&texture);
        D3D11_TEXTURE2D_DESC td = {};
        texture->GetDesc(&td);
        if (p->subresource >= td.MipLevels * td.ArraySize) {
            std::cerr << "[ServerDispatcher] UpdateSubresource: subresource " << p->subresource << " out of range" << std::endl;
            return false;
        }
        const uint32_t mip = p->subresource % td.MipLevels;
        const uint32_t mw = MipSize(td.Width, mip), mh = MipSize(td.Height, mip);
        if (p_box && (box.right > mw || box.bottom > mh || box.front != 0 || box.back != 1)) {
            std::cerr << "[ServerDispatcher] UpdateSubresource: box outside the " << mw << "x" << mh << " subresource" << std::endl;
            return false;
        }
        uint64_t row_bytes = 0;
        uint32_t rows = 0;
        if (!SurfaceLayout(td.Format, p_box ? box.right - box.left : mw, p_box ? box.bottom - box.top : mh, row_bytes, rows) ||
            p->row_pitch < row_bytes) {
            std::cerr << "[ServerDispatcher] UpdateSubresource: unsupported format or row pitch too small" << std::endl;
            return false;
        }
        needed = static_cast<uint64_t>(p->row_pitch) * (rows - 1) + row_bytes;
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
        Microsoft::WRL::ComPtr<ID3D11Texture3D> texture;
        res.As(&texture);
        D3D11_TEXTURE3D_DESC td = {};
        texture->GetDesc(&td);
        if (p->subresource >= td.MipLevels) {
            std::cerr << "[ServerDispatcher] UpdateSubresource: subresource " << p->subresource << " out of range" << std::endl;
            return false;
        }
        const uint32_t mip = p->subresource;
        const uint32_t mw = MipSize(td.Width, mip), mh = MipSize(td.Height, mip), md = MipSize(td.Depth, mip);
        if (p_box && (box.right > mw || box.bottom > mh || box.back > md)) {
            std::cerr << "[ServerDispatcher] UpdateSubresource: box outside the subresource" << std::endl;
            return false;
        }
        uint64_t row_bytes = 0;
        uint32_t rows = 0;
        if (!SurfaceLayout(td.Format, p_box ? box.right - box.left : mw, p_box ? box.bottom - box.top : mh, row_bytes, rows) ||
            p->row_pitch < row_bytes) {
            std::cerr << "[ServerDispatcher] UpdateSubresource: unsupported format or row pitch too small" << std::endl;
            return false;
        }
        const uint32_t depth = p_box ? (box.back - box.front) : md;
        const uint64_t slice_bytes = (rows > 1) ? (static_cast<uint64_t>(p->row_pitch) * (rows - 1) + row_bytes) : row_bytes;
        needed = (depth > 1) ? (static_cast<uint64_t>(p->depth_pitch) * (depth - 1) + slice_bytes) : slice_bytes;
    } else {
        std::cerr << "[ServerDispatcher] UpdateSubresource: unsupported resource dimension " << dim << std::endl;
        return false;
    }
    if (p->data_size < needed) {
        std::cerr << "[ServerDispatcher] UpdateSubresource: " << p->data_size << " bytes sent, " << needed << " needed" << std::endl;
        return false;
    }

    const void* p_data = payload + sizeof(UpdateSubresourcePayload);
    context->UpdateSubresource(res.Get(), p->subresource, p_box, p_data, p->row_pitch, p->depth_pitch);
    return true;
}

bool ServerDispatcher::HandleWriteBufferDiscard(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(WriteBufferDiscardPayload)) return false;
    const auto* p = reinterpret_cast<const WriteBufferDiscardPayload*>(payload);

    if (p->res_id == 0) return false;

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

    if (static_cast<uint64_t>(p->byte_offset) + static_cast<uint64_t>(p->byte_width) > desc.ByteWidth) {
        std::cerr << "[ServerDispatcher] WriteBufferDiscard: write range exceeds buffer byte width ("
                  << (p->byte_offset + p->byte_width) << " > " << desc.ByteWidth << ")" << std::endl;
        return false;
    }

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

    float color[4];
    for (int i = 0; i < 4; ++i) {
        color[i] = std::isfinite(p->color[i]) ? p->color[i] : 0.0f;
    }

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
        context->ClearRenderTargetView(rtv.Get(), color);
    }
    return true;
}

bool ServerDispatcher::HandleClearDsv(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(ClearDsvPayload)) return false;
    const auto* p = reinterpret_cast<const ClearDsvPayload*>(payload);

    const UINT valid_mask = D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL;
    UINT clear_flags = p->clear_flags & valid_mask;
    if (clear_flags == 0) clear_flags = D3D11_CLEAR_DEPTH;

    float depth = p->depth;
    if ((clear_flags & D3D11_CLEAR_DEPTH) != 0) {
        if (!std::isfinite(depth) || depth < 0.0f) depth = 0.0f;
        else if (depth > 1.0f) depth = 1.0f;
    } else {
        depth = 1.0f;
    }

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
        context->ClearDepthStencilView(dsv.Get(), clear_flags, depth, p->stencil);
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

bool ServerDispatcher::HandleDrawInstanced(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(DrawInstancedPayload)) return false;
    const auto* p = reinterpret_cast<const DrawInstancedPayload*>(payload);
    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;
    context->DrawInstanced(p->vertex_count_per_instance, p->instance_count, p->start_vertex_location,
                           p->start_instance_location);
    return true;
}

bool ServerDispatcher::HandleDrawIndexedInstanced(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(DrawIndexedInstancedPayload)) return false;
    const auto* p = reinterpret_cast<const DrawIndexedInstancedPayload*>(payload);
    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;
    context->DrawIndexedInstanced(p->index_count_per_instance, p->instance_count, p->start_index_location,
                                  p->base_vertex_location, p->start_instance_location);
    return true;
}

bool ServerDispatcher::HandleOMSetRenderTargetsMrt(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(OMSetRenderTargetsMrtPayload)) return false;
    const auto* p = reinterpret_cast<const OMSetRenderTargetsMrtPayload*>(payload);
    if (p->num_rtvs > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT) {
        std::cerr << "[ServerDispatcher] OMSetRenderTargetsMrt: " << p->num_rtvs << " render targets exceed the limit" << std::endl;
        return false;
    }
    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    // Unlike the single-RTV opcode, id 0 is an empty slot: no fallback to the host back buffer.
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11RenderTargetView* raw[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    for (uint32_t i = 0; i < p->num_rtvs; ++i) {
        rtvs[i] = m_resources.GetRTV(p->rtv_ids[i]);
        raw[i] = rtvs[i].Get();
    }
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv = m_resources.GetDSV(p->dsv_id);
    context->OMSetRenderTargets(p->num_rtvs, p->num_rtvs ? raw : nullptr, dsv.Get());
    return true;
}

bool ServerDispatcher::HandleVSSetSrv(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(PSSetSrvPayload)) return false;
    const auto* p = reinterpret_cast<const PSSetSrvPayload*>(payload);
    if (p->slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        std::cerr << "[ServerDispatcher] Invalid VS SRV slot: " << p->slot << std::endl;
        return false;
    }
    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv = m_resources.GetSRV(p->res_id);
    ID3D11ShaderResourceView* p_srv = srv.Get();
    context->VSSetShaderResources(p->slot, 1, &p_srv);
    return true;
}

bool ServerDispatcher::HandleVSSetSampler(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(PSSetSamplerPayload)) return false;
    const auto* p = reinterpret_cast<const PSSetSamplerPayload*>(payload);
    if (p->slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        std::cerr << "[ServerDispatcher] Invalid VS sampler slot: " << p->slot << std::endl;
        return false;
    }
    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler = m_resources.GetSamplerState(p->res_id);
    ID3D11SamplerState* p_sampler = sampler.Get();
    context->VSSetSamplers(p->slot, 1, &p_sampler);
    return true;
}

// =============================================================================
// Compute Shaders & Unordered Access Views
// =============================================================================

bool ServerDispatcher::HandleCreateComputeShader(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateShaderPayload)) return false;
    const auto* p = reinterpret_cast<const CreateShaderPayload*>(payload);
    if (p->res_id == 0 || p->bytecode_length == 0 || p->bytecode_length > 64 * 1024 * 1024 ||
        p->bytecode_length > size - sizeof(CreateShaderPayload)) {
        std::cerr << "[ServerDispatcher] CreateComputeShader: invalid res_id or bytecode length" << std::endl;
        return false;
    }
    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> cs;
    HRESULT hr = device->CreateComputeShader(payload + sizeof(CreateShaderPayload), p->bytecode_length, nullptr, &cs);
    if (FAILED(hr)) {
        std::cerr << "[ServerDispatcher] CreateComputeShader failed: 0x" << std::hex << hr << std::dec
                  << " (res_id=" << p->res_id << ")" << std::endl;
        return false;
    }
    m_resources.Register(p->res_id, cs);
    return true;
}

bool ServerDispatcher::HandleCreateUav(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateUavPayload)) return false;
    const auto* p = reinterpret_cast<const CreateUavPayload*>(payload);
    if (p->res_id == 0 || p->resource_id == 0) {
        std::cerr << "[ServerDispatcher] CreateUav: invalid res_id or resource_id" << std::endl;
        return false;
    }
    if (p->view_dimension > D3D11_UAV_DIMENSION_TEXTURE3D || (p->format != 0 && !IsValidDxgiFormat(p->format))) {
        std::cerr << "[ServerDispatcher] CreateUav: invalid format " << p->format << " or dimension " << p->view_dimension << std::endl;
        return false;
    }
    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;
    Microsoft::WRL::ComPtr<ID3D11Resource> resource = m_resources.Get<ID3D11Resource>(p->resource_id);
    if (!resource) {
        std::cerr << "[ServerDispatcher] CreateUav: underlying resource not found: " << p->resource_id << std::endl;
        return false;
    }
    const bool use_desc = (p->format != 0 || p->view_dimension != 0);
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc = {};
    desc.Format = static_cast<DXGI_FORMAT>(p->format);
    desc.ViewDimension = static_cast<D3D11_UAV_DIMENSION>(p->view_dimension);
    static_assert(sizeof(desc.Buffer) == sizeof(p->fields), "UAV union is three UINTs");
    std::memcpy(&desc.Buffer, p->fields, sizeof(p->fields));

    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    HRESULT hr = device->CreateUnorderedAccessView(resource.Get(), use_desc ? &desc : nullptr, &uav);
    if (FAILED(hr)) {
        std::cerr << "[ServerDispatcher] CreateUnorderedAccessView failed: 0x" << std::hex << hr << std::dec
                  << " (res_id=" << p->res_id << ")" << std::endl;
        return false;
    }
    m_resources.Register(p->res_id, uav);
    return true;
}

bool ServerDispatcher::HandleCSSetShader(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(VSSetShaderPayload)) return false;
    const auto* p = reinterpret_cast<const VSSetShaderPayload*>(payload);
    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> cs = m_resources.Get<ID3D11ComputeShader>(p->res_id);
    context->CSSetShader(cs.Get(), nullptr, 0);
    return true;
}

bool ServerDispatcher::HandleCSSetSrv(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(PSSetSrvPayload)) return false;
    const auto* p = reinterpret_cast<const PSSetSrvPayload*>(payload);
    if (p->slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) return false;
    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv = m_resources.GetSRV(p->res_id);
    ID3D11ShaderResourceView* p_srv = srv.Get();
    context->CSSetShaderResources(p->slot, 1, &p_srv);
    return true;
}

bool ServerDispatcher::HandleCSSetUav(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CSSetUavPayload)) return false;
    const auto* p = reinterpret_cast<const CSSetUavPayload*>(payload);
    if (p->slot >= D3D11_1_UAV_SLOT_COUNT) {
        std::cerr << "[ServerDispatcher] Invalid CS UAV slot: " << p->slot << std::endl;
        return false;
    }
    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav = m_resources.Get<ID3D11UnorderedAccessView>(p->res_id);
    ID3D11UnorderedAccessView* p_uav = uav.Get();
    const UINT count = p->initial_count;
    context->CSSetUnorderedAccessViews(p->slot, 1, &p_uav, &count);
    return true;
}

bool ServerDispatcher::HandleCSSetCB(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(VSSetConstantBufferPayload)) return false;
    const auto* p = reinterpret_cast<const VSSetConstantBufferPayload*>(payload);
    if (p->slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) return false;
    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb = m_resources.GetBuffer(p->res_id);
    ID3D11Buffer* p_cb = cb.Get();
    context->CSSetConstantBuffers(p->slot, 1, &p_cb);
    return true;
}

bool ServerDispatcher::HandleCSSetSampler(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(PSSetSamplerPayload)) return false;
    const auto* p = reinterpret_cast<const PSSetSamplerPayload*>(payload);
    if (p->slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) return false;
    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler = m_resources.GetSamplerState(p->res_id);
    ID3D11SamplerState* p_sampler = sampler.Get();
    context->CSSetSamplers(p->slot, 1, &p_sampler);
    return true;
}

bool ServerDispatcher::HandleDispatch(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(DispatchPayload)) return false;
    const auto* p = reinterpret_cast<const DispatchPayload*>(payload);
    const UINT limit = D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
    if (p->thread_group_x > limit || p->thread_group_y > limit || p->thread_group_z > limit) {
        std::cerr << "[ServerDispatcher] Dispatch: thread group count exceeds " << limit << std::endl;
        return false;
    }
    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;
    context->Dispatch(p->thread_group_x, p->thread_group_y, p->thread_group_z);
    return true;
}

// =============================================================================
// Geometry & Tessellation Stages (GS, HS, DS)
// =============================================================================

bool ServerDispatcher::HandleCreateStageShader(const PacketHeader& header, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateShaderPayload)) return false;
    const auto* p = reinterpret_cast<const CreateShaderPayload*>(payload);
    if (p->res_id == 0 || p->bytecode_length == 0 || p->bytecode_length > 64 * 1024 * 1024 ||
        p->bytecode_length > size - sizeof(CreateShaderPayload)) {
        std::cerr << "[ServerDispatcher] CreateStageShader: invalid res_id or bytecode length" << std::endl;
        return false;
    }
    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;
    const void* bytecode = payload + sizeof(CreateShaderPayload);

    HRESULT hr = E_INVALIDARG;
    Microsoft::WRL::ComPtr<ID3D11DeviceChild> shader;
    if (header.opcode == OP_CREATE_GEOMETRY_SHADER) {
        Microsoft::WRL::ComPtr<ID3D11GeometryShader> gs;
        hr = device->CreateGeometryShader(bytecode, p->bytecode_length, nullptr, &gs);
        shader = gs;
    } else if (header.opcode == OP_CREATE_HULL_SHADER) {
        Microsoft::WRL::ComPtr<ID3D11HullShader> hs;
        hr = device->CreateHullShader(bytecode, p->bytecode_length, nullptr, &hs);
        shader = hs;
    } else {
        Microsoft::WRL::ComPtr<ID3D11DomainShader> ds;
        hr = device->CreateDomainShader(bytecode, p->bytecode_length, nullptr, &ds);
        shader = ds;
    }
    if (FAILED(hr)) {
        std::cerr << "[ServerDispatcher] Create GS/HS/DS failed: 0x" << std::hex << hr << std::dec
                  << " (opcode 0x" << std::hex << header.opcode << std::dec << ", res_id=" << p->res_id << ")" << std::endl;
        return false;
    }
    m_resources.Register(p->res_id, shader);
    return true;
}

bool ServerDispatcher::HandleStageSetShader(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(StageSetShaderPayload)) return false;
    const auto* p = reinterpret_cast<const StageSetShaderPayload*>(payload);
    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;
    switch (p->stage) {
        case STAGE_GEOMETRY: context->GSSetShader(m_resources.Get<ID3D11GeometryShader>(p->res_id).Get(), nullptr, 0); return true;
        case STAGE_HULL:     context->HSSetShader(m_resources.Get<ID3D11HullShader>(p->res_id).Get(), nullptr, 0); return true;
        case STAGE_DOMAIN:   context->DSSetShader(m_resources.Get<ID3D11DomainShader>(p->res_id).Get(), nullptr, 0); return true;
        default:
            std::cerr << "[ServerDispatcher] StageSetShader: invalid stage " << p->stage << std::endl;
            return false;
    }
}

bool ServerDispatcher::HandleStageBind(const PacketHeader& header, const uint8_t* payload, size_t size) {
    if (size < sizeof(StageBindPayload)) return false;
    const auto* p = reinterpret_cast<const StageBindPayload*>(payload);
    if (p->stage < STAGE_GEOMETRY || p->stage > STAGE_DOMAIN) {
        std::cerr << "[ServerDispatcher] StageBind: invalid stage " << p->stage << std::endl;
        return false;
    }
    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    if (header.opcode == OP_STAGE_SET_CB) {
        if (p->slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) return false;
        Microsoft::WRL::ComPtr<ID3D11Buffer> cb = m_resources.GetBuffer(p->res_id);
        ID3D11Buffer* raw = cb.Get();
        if (p->stage == STAGE_GEOMETRY) context->GSSetConstantBuffers(p->slot, 1, &raw);
        else if (p->stage == STAGE_HULL) context->HSSetConstantBuffers(p->slot, 1, &raw);
        else context->DSSetConstantBuffers(p->slot, 1, &raw);
    } else if (header.opcode == OP_STAGE_SET_SRV) {
        if (p->slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) return false;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv = m_resources.GetSRV(p->res_id);
        ID3D11ShaderResourceView* raw = srv.Get();
        if (p->stage == STAGE_GEOMETRY) context->GSSetShaderResources(p->slot, 1, &raw);
        else if (p->stage == STAGE_HULL) context->HSSetShaderResources(p->slot, 1, &raw);
        else context->DSSetShaderResources(p->slot, 1, &raw);
    } else {
        if (p->slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) return false;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler = m_resources.GetSamplerState(p->res_id);
        ID3D11SamplerState* raw = sampler.Get();
        if (p->stage == STAGE_GEOMETRY) context->GSSetSamplers(p->slot, 1, &raw);
        else if (p->stage == STAGE_HULL) context->HSSetSamplers(p->slot, 1, &raw);
        else context->DSSetSamplers(p->slot, 1, &raw);
    }
    return true;
}

// =============================================================================
// Resource Copies & Pixel Shader UAVs
// =============================================================================

namespace {

// Size of one subresource in copy units: bytes (x only) for buffers, texels for 2D textures.
// Returns false for other resource kinds or an out-of-range subresource.
bool SubresourceExtent(ID3D11Resource* res, uint32_t subresource, D3D11_RESOURCE_DIMENSION& dim,
                       uint32_t& width, uint32_t& height) {
    res->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC bd = {};
        static_cast<ID3D11Buffer*>(res)->GetDesc(&bd);
        width = bd.ByteWidth;
        height = 1;
        return subresource == 0;
    }
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC td = {};
        static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
        if (subresource >= td.MipLevels * td.ArraySize) return false;
        const uint32_t mip = subresource % td.MipLevels;
        width = MipSize(td.Width, mip);
        height = MipSize(td.Height, mip);
        return true;
    }
    return false;
}

} // namespace

bool ServerDispatcher::HandleCopyResource(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CopyResourcePayload)) return false;
    const auto* p = reinterpret_cast<const CopyResourcePayload*>(payload);
    Microsoft::WRL::ComPtr<ID3D11Resource> dst = m_resources.Get<ID3D11Resource>(p->dst_id);
    Microsoft::WRL::ComPtr<ID3D11Resource> src = m_resources.Get<ID3D11Resource>(p->src_id);
    if (!dst || !src || dst.Get() == src.Get()) {
        std::cerr << "[ServerDispatcher] CopyResource: missing or identical resources (" << p->dst_id << ", " << p->src_id << ")" << std::endl;
        return false;
    }
    D3D11_RESOURCE_DIMENSION dd = D3D11_RESOURCE_DIMENSION_UNKNOWN, sd = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    dst->GetType(&dd);
    src->GetType(&sd);
    bool same_shape = (dd == sd);
    if (same_shape && dd == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC a = {}, b = {};
        static_cast<ID3D11Buffer*>(dst.Get())->GetDesc(&a);
        static_cast<ID3D11Buffer*>(src.Get())->GetDesc(&b);
        same_shape = a.ByteWidth == b.ByteWidth;
    } else if (same_shape && dd == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC a = {}, b = {};
        static_cast<ID3D11Texture2D*>(dst.Get())->GetDesc(&a);
        static_cast<ID3D11Texture2D*>(src.Get())->GetDesc(&b);
        same_shape = a.Width == b.Width && a.Height == b.Height && a.MipLevels == b.MipLevels &&
                     a.ArraySize == b.ArraySize && a.SampleDesc.Count == b.SampleDesc.Count;
    }
    if (!same_shape) {
        std::cerr << "[ServerDispatcher] CopyResource: resources differ in type or size" << std::endl;
        return false;
    }
    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;
    context->CopyResource(dst.Get(), src.Get());
    return true;
}

bool ServerDispatcher::HandleCopySubresourceRegion(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CopySubresourceRegionPayload)) return false;
    const auto* p = reinterpret_cast<const CopySubresourceRegionPayload*>(payload);
    Microsoft::WRL::ComPtr<ID3D11Resource> dst = m_resources.Get<ID3D11Resource>(p->dst_id);
    Microsoft::WRL::ComPtr<ID3D11Resource> src = m_resources.Get<ID3D11Resource>(p->src_id);
    if (!dst || !src) {
        std::cerr << "[ServerDispatcher] CopySubresourceRegion: resource not found" << std::endl;
        return false;
    }

    // Direct3D does not bound-check the region: an out-of-range copy is undefined behaviour in
    // the driver, so both sides are checked here.
    D3D11_RESOURCE_DIMENSION dd, sd;
    uint32_t dw = 0, dh = 0, sw = 0, sh = 0;
    if (!SubresourceExtent(dst.Get(), p->dst_subresource, dd, dw, dh) ||
        !SubresourceExtent(src.Get(), p->src_subresource, sd, sw, sh) || dd != sd) {
        std::cerr << "[ServerDispatcher] CopySubresourceRegion: unsupported or mismatched subresources" << std::endl;
        return false;
    }
    D3D11_BOX box = {0, 0, 0, sw, sh, 1};
    if (p->has_src_box) {
        box = {p->src_box.left, p->src_box.top, p->src_box.front, p->src_box.right, p->src_box.bottom, p->src_box.back};
    }
    const bool box_ok = box.left < box.right && box.right <= sw && box.top < box.bottom && box.bottom <= sh &&
                        box.front == 0 && box.back == 1;
    const bool dst_ok = p->dst_z == 0 && static_cast<uint64_t>(p->dst_x) + (box.right - box.left) <= dw &&
                        static_cast<uint64_t>(p->dst_y) + (box.bottom - box.top) <= dh;
    if (!box_ok || !dst_ok) {
        std::cerr << "[ServerDispatcher] CopySubresourceRegion: region out of bounds" << std::endl;
        return false;
    }
    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;
    context->CopySubresourceRegion(dst.Get(), p->dst_subresource, p->dst_x, p->dst_y, p->dst_z, src.Get(),
                                   p->src_subresource, p->has_src_box ? &box : nullptr);
    return true;
}

bool ServerDispatcher::HandleOMSetRtvsAndUavs(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(OMSetRtvsAndUavsPayload)) return false;
    const auto* p = reinterpret_cast<const OMSetRtvsAndUavsPayload*>(payload);
    const bool keep_rtvs = p->num_rtvs == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL;
    const bool keep_uavs = p->num_uavs == D3D11_KEEP_UNORDERED_ACCESS_VIEWS;
    if ((!keep_rtvs && p->num_rtvs > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT) ||
        (!keep_uavs && (p->num_uavs > 8 || p->uav_start_slot > D3D11_1_UAV_SLOT_COUNT - p->num_uavs ||
                        (!keep_rtvs && p->num_uavs > 0 && p->uav_start_slot < p->num_rtvs)))) {
        std::cerr << "[ServerDispatcher] OMSetRtvsAndUavs: invalid slots (rtvs " << p->num_rtvs << ", uav start "
                  << p->uav_start_slot << ", uavs " << p->num_uavs << ")" << std::endl;
        return false;
    }
    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtvs[8];
    ID3D11RenderTargetView* raw_rtvs[8] = {};
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
    if (!keep_rtvs) {
        for (uint32_t i = 0; i < p->num_rtvs; ++i) {
            rtvs[i] = m_resources.GetRTV(p->rtv_ids[i]);
            raw_rtvs[i] = rtvs[i].Get();
        }
        dsv = m_resources.GetDSV(p->dsv_id);
    }
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uavs[8];
    ID3D11UnorderedAccessView* raw_uavs[8] = {};
    if (!keep_uavs) {
        for (uint32_t i = 0; i < p->num_uavs; ++i) {
            uavs[i] = m_resources.Get<ID3D11UnorderedAccessView>(p->uav_ids[i]);
            raw_uavs[i] = uavs[i].Get();
        }
    }
    context->OMSetRenderTargetsAndUnorderedAccessViews(p->num_rtvs, keep_rtvs ? nullptr : raw_rtvs, dsv.Get(),
                                                       p->uav_start_slot, p->num_uavs, keep_uavs ? nullptr : raw_uavs,
                                                       keep_uavs ? nullptr : p->initial_counts);
    return true;
}

bool ServerDispatcher::HandleClearUav(const PacketHeader& header, const uint8_t* payload, size_t size) {
    if (size < sizeof(ClearUavPayload)) return false;
    const auto* p = reinterpret_cast<const ClearUavPayload*>(payload);
    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav = m_resources.Get<ID3D11UnorderedAccessView>(p->res_id);
    if (!uav) {
        std::cerr << "[ServerDispatcher] ClearUav: UAV not found: " << p->res_id << std::endl;
        return false;
    }
    if (header.opcode == OP_CLEAR_UAV_UINT) {
        context->ClearUnorderedAccessViewUint(uav.Get(), p->values);
    } else {
        float values[4];
        std::memcpy(values, p->values, sizeof(values));
        context->ClearUnorderedAccessViewFloat(uav.Get(), values);
    }
    return true;
}

// =============================================================================
// Fixed-Function State Handlers (blend, depth-stencil, rasterizer, scissor)
// =============================================================================

namespace {

bool ValidBlend(uint32_t v) { return v >= D3D11_BLEND_ZERO && v <= D3D11_BLEND_INV_SRC1_ALPHA && v != 12 && v != 13; }
bool ValidBlendOp(uint32_t v) { return v >= D3D11_BLEND_OP_ADD && v <= D3D11_BLEND_OP_MAX; }
bool ValidComparison(uint32_t v) { return v >= D3D11_COMPARISON_NEVER && v <= D3D11_COMPARISON_ALWAYS; }
bool ValidStencilOp(uint32_t v) { return v >= D3D11_STENCIL_OP_KEEP && v <= D3D11_STENCIL_OP_DECR; }

bool ValidStencilFace(const RgpuStencilOpDesc& f) {
    return ValidStencilOp(f.fail_op) && ValidStencilOp(f.depth_fail_op) && ValidStencilOp(f.pass_op) && ValidComparison(f.func);
}

D3D11_DEPTH_STENCILOP_DESC ToStencilFace(const RgpuStencilOpDesc& f) {
    D3D11_DEPTH_STENCILOP_DESC d;
    d.StencilFailOp = static_cast<D3D11_STENCIL_OP>(f.fail_op);
    d.StencilDepthFailOp = static_cast<D3D11_STENCIL_OP>(f.depth_fail_op);
    d.StencilPassOp = static_cast<D3D11_STENCIL_OP>(f.pass_op);
    d.StencilFunc = static_cast<D3D11_COMPARISON_FUNC>(f.func);
    return d;
}

} // namespace

bool ServerDispatcher::HandleCreateBlendState(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateBlendStatePayload)) {
        std::cerr << "[ServerDispatcher] CreateBlendState payload too small: " << size << std::endl;
        return false;
    }
    const auto* p = reinterpret_cast<const CreateBlendStatePayload*>(payload);
    if (p->res_id == 0) {
        std::cerr << "[ServerDispatcher] CreateBlendState: invalid res_id=0" << std::endl;
        return false;
    }

    D3D11_BLEND_DESC desc = {};
    desc.AlphaToCoverageEnable = p->alpha_to_coverage ? TRUE : FALSE;
    desc.IndependentBlendEnable = p->independent_blend ? TRUE : FALSE;
    for (int i = 0; i < 8; ++i) {
        const RgpuRenderTargetBlend& s = p->render_target[i];
        if (!ValidBlend(s.src_blend) || !ValidBlend(s.dest_blend) || !ValidBlend(s.src_blend_alpha) ||
            !ValidBlend(s.dest_blend_alpha) || !ValidBlendOp(s.blend_op) || !ValidBlendOp(s.blend_op_alpha) ||
            s.write_mask > D3D11_COLOR_WRITE_ENABLE_ALL) {
            std::cerr << "[ServerDispatcher] CreateBlendState: invalid render target " << i << " description" << std::endl;
            return false;
        }
        D3D11_RENDER_TARGET_BLEND_DESC& d = desc.RenderTarget[i];
        d.BlendEnable = s.blend_enable ? TRUE : FALSE;
        d.SrcBlend = static_cast<D3D11_BLEND>(s.src_blend);
        d.DestBlend = static_cast<D3D11_BLEND>(s.dest_blend);
        d.BlendOp = static_cast<D3D11_BLEND_OP>(s.blend_op);
        d.SrcBlendAlpha = static_cast<D3D11_BLEND>(s.src_blend_alpha);
        d.DestBlendAlpha = static_cast<D3D11_BLEND>(s.dest_blend_alpha);
        d.BlendOpAlpha = static_cast<D3D11_BLEND_OP>(s.blend_op_alpha);
        d.RenderTargetWriteMask = static_cast<UINT8>(s.write_mask);
    }

    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;
    Microsoft::WRL::ComPtr<ID3D11BlendState> state;
    HRESULT hr = device->CreateBlendState(&desc, &state);
    if (FAILED(hr)) {
        std::cerr << "[ServerDispatcher] CreateBlendState failed: 0x" << std::hex << hr << std::dec
                  << " (res_id=" << p->res_id << ")" << std::endl;
        return false;
    }
    m_resources.Register(p->res_id, state);
    return true;
}

bool ServerDispatcher::HandleCreateDepthStencilState(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateDepthStencilStatePayload)) {
        std::cerr << "[ServerDispatcher] CreateDepthStencilState payload too small: " << size << std::endl;
        return false;
    }
    const auto* p = reinterpret_cast<const CreateDepthStencilStatePayload*>(payload);
    if (p->res_id == 0) {
        std::cerr << "[ServerDispatcher] CreateDepthStencilState: invalid res_id=0" << std::endl;
        return false;
    }
    if (p->depth_write_mask > D3D11_DEPTH_WRITE_MASK_ALL || !ValidComparison(p->depth_func) ||
        !ValidStencilFace(p->front_face) || !ValidStencilFace(p->back_face)) {
        std::cerr << "[ServerDispatcher] CreateDepthStencilState: invalid description" << std::endl;
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC desc = {};
    desc.DepthEnable = p->depth_enable ? TRUE : FALSE;
    desc.DepthWriteMask = static_cast<D3D11_DEPTH_WRITE_MASK>(p->depth_write_mask);
    desc.DepthFunc = static_cast<D3D11_COMPARISON_FUNC>(p->depth_func);
    desc.StencilEnable = p->stencil_enable ? TRUE : FALSE;
    desc.StencilReadMask = static_cast<UINT8>(p->stencil_read_mask);
    desc.StencilWriteMask = static_cast<UINT8>(p->stencil_write_mask);
    desc.FrontFace = ToStencilFace(p->front_face);
    desc.BackFace = ToStencilFace(p->back_face);

    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> state;
    HRESULT hr = device->CreateDepthStencilState(&desc, &state);
    if (FAILED(hr)) {
        std::cerr << "[ServerDispatcher] CreateDepthStencilState failed: 0x" << std::hex << hr << std::dec
                  << " (res_id=" << p->res_id << ")" << std::endl;
        return false;
    }
    m_resources.Register(p->res_id, state);
    return true;
}

bool ServerDispatcher::HandleCreateRasterizerState(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateRasterizerStatePayload)) {
        std::cerr << "[ServerDispatcher] CreateRasterizerState payload too small: " << size << std::endl;
        return false;
    }
    const auto* p = reinterpret_cast<const CreateRasterizerStatePayload*>(payload);
    if (p->res_id == 0) {
        std::cerr << "[ServerDispatcher] CreateRasterizerState: invalid res_id=0" << std::endl;
        return false;
    }
    if ((p->fill_mode != D3D11_FILL_WIREFRAME && p->fill_mode != D3D11_FILL_SOLID) ||
        p->cull_mode < D3D11_CULL_NONE || p->cull_mode > D3D11_CULL_BACK ||
        !std::isfinite(p->depth_bias_clamp) || !std::isfinite(p->slope_scaled_depth_bias)) {
        std::cerr << "[ServerDispatcher] CreateRasterizerState: invalid description (fill=" << p->fill_mode
                  << ", cull=" << p->cull_mode << ")" << std::endl;
        return false;
    }

    D3D11_RASTERIZER_DESC desc = {};
    desc.FillMode = static_cast<D3D11_FILL_MODE>(p->fill_mode);
    desc.CullMode = static_cast<D3D11_CULL_MODE>(p->cull_mode);
    desc.FrontCounterClockwise = p->front_counter_clockwise ? TRUE : FALSE;
    desc.DepthBias = p->depth_bias;
    desc.DepthBiasClamp = p->depth_bias_clamp;
    desc.SlopeScaledDepthBias = p->slope_scaled_depth_bias;
    desc.DepthClipEnable = p->depth_clip_enable ? TRUE : FALSE;
    desc.ScissorEnable = p->scissor_enable ? TRUE : FALSE;
    desc.MultisampleEnable = p->multisample_enable ? TRUE : FALSE;
    desc.AntialiasedLineEnable = p->antialiased_line_enable ? TRUE : FALSE;

    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> state;
    HRESULT hr = device->CreateRasterizerState(&desc, &state);
    if (FAILED(hr)) {
        std::cerr << "[ServerDispatcher] CreateRasterizerState failed: 0x" << std::hex << hr << std::dec
                  << " (res_id=" << p->res_id << ")" << std::endl;
        return false;
    }
    m_resources.Register(p->res_id, state);
    return true;
}

bool ServerDispatcher::HandleOMSetBlendState(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(OMSetBlendStatePayload)) return false;
    const auto* p = reinterpret_cast<const OMSetBlendStatePayload*>(payload);
    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;
    // An unknown id falls back to the default state, like a null state in D3D11.
    Microsoft::WRL::ComPtr<ID3D11BlendState> state = m_resources.Get<ID3D11BlendState>(p->res_id);
    float factor[4];
    for (int i = 0; i < 4; ++i) factor[i] = std::isfinite(p->blend_factor[i]) ? p->blend_factor[i] : 1.0f;
    context->OMSetBlendState(state.Get(), factor, p->sample_mask);
    return true;
}

bool ServerDispatcher::HandleOMSetDepthStencilState(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(OMSetDepthStencilStatePayload)) return false;
    const auto* p = reinterpret_cast<const OMSetDepthStencilStatePayload*>(payload);
    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> state = m_resources.Get<ID3D11DepthStencilState>(p->res_id);
    context->OMSetDepthStencilState(state.Get(), p->stencil_ref & 0xFF);
    return true;
}

bool ServerDispatcher::HandleRSSetState(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(RSSetStatePayload)) return false;
    const auto* p = reinterpret_cast<const RSSetStatePayload*>(payload);
    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> state = m_resources.Get<ID3D11RasterizerState>(p->res_id);
    context->RSSetState(state.Get());
    return true;
}

bool ServerDispatcher::HandleRSSetScissor(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(RSSetScissorPayload)) return false;
    const auto* p = reinterpret_cast<const RSSetScissorPayload*>(payload);
    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;
    const D3D11_RECT rect = {p->left, p->top, p->right, p->bottom};
    context->RSSetScissorRects(1, &rect);
    return true;
}

// =============================================================================
// Presentation Handlers
// =============================================================================

bool ServerDispatcher::HandleResizeBuffers(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(ResizeBuffersPayload)) {
        std::cerr << "[ServerDispatcher] ResizeBuffers payload too small: " << size << std::endl;
        return false;
    }
    const auto* p = reinterpret_cast<const ResizeBuffersPayload*>(payload);
    if (p->width == 0 || p->width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        p->height == 0 || p->height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        std::cerr << "[ServerDispatcher] ResizeBuffers: invalid size " << p->width << "x" << p->height << std::endl;
        return false;
    }
    if (!m_gpuDevice.CreateOffscreenTargets(p->width, p->height)) {
        std::cerr << "[ServerDispatcher] ResizeBuffers: failed to recreate targets at "
                  << p->width << "x" << p->height << std::endl;
        return false;
    }
    m_frameWidth = p->width;
    m_frameHeight = p->height;
    m_previousFrame.clear(); // next frame is a key frame
    return true;
}

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
        // Compress when the client negotiated it and it actually saves bytes; otherwise send raw.
        // Either way the client ends up holding exactly m_frameReadbackBuffer, which becomes the
        // reference for the next delta frame.
        const size_t pixel_count = static_cast<size_t>(width) * height;
        const bool tightly_packed = (m_frameReadbackBuffer.size() == pixel_count * 4);
        bool compressed = false;
        if (m_frameCompression && tightly_packed) {
            const bool has_reference = (m_previousFrame.size() == m_frameReadbackBuffer.size());
            FrameCodec::Encode(m_frameReadbackBuffer.data(), has_reference ? m_previousFrame.data() : nullptr,
                               pixel_count, m_encodedFrame);
            compressed = m_encodedFrame.size() < m_frameReadbackBuffer.size();
        }
        const std::vector<uint8_t>& body = compressed ? m_encodedFrame : m_frameReadbackBuffer;

        PacketHeader resp_header = {};
        resp_header.magic = RGPU_MAGIC;
        resp_header.opcode = OP_FRAME_DATA;
        resp_header.flags = compressed ? FLAG_COMPRESSED : FLAG_NONE;
        resp_header.sequence_id = header.sequence_id; // Sequence synchronization
        resp_header.payload_length = static_cast<uint32_t>(sizeof(FrameDataPayload) + body.size());

        FrameDataPayload frame_payload = {};
        frame_payload.swapchain_id = p->swapchain_id;
        frame_payload.width = width;
        frame_payload.height = height;
        frame_payload.format = 28; // DXGI_FORMAT_R8G8B8A8_UNORM
        frame_payload.row_pitch = width * 4;
        frame_payload.data_size = static_cast<uint32_t>(body.size());
        frame_payload.frame_number = p->frame_number; // Echo monotonic client frame number

        // Send header + frame payload + pixel (or encoded) bytes
        if (!socket->SendExact(&resp_header, sizeof(resp_header))) return false;
        if (!socket->SendExact(&frame_payload, sizeof(frame_payload))) return false;
        if (!body.empty()) {
            if (!socket->SendExact(body.data(), body.size())) return false;
        }

        m_rawFrameBytes += m_frameReadbackBuffer.size();
        m_sentFrameBytes += body.size();
        if (m_frameCompression) m_previousFrame.swap(m_frameReadbackBuffer);
    }

    m_lastPresentedFrame = p->frame_number;
    m_totalFramesPresented++;
    if (m_totalFramesPresented % 60 == 0 || m_totalFramesPresented == 1) {
        std::cout << "[Server] Active rendering: " << m_totalFramesPresented << " frames presented ("
                  << width << "x" << height << ", active resources: " << m_resources.Count() << ")" << std::endl;
    }
    return true;
}

} // namespace RemoteGPU
