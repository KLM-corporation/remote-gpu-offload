#pragma once
#ifndef PROTOTYPE_SERVER_DISPATCHER_H
#define PROTOTYPE_SERVER_DISPATCHER_H

#include "../../src/common/protocol.h"
#include "../../src/common/transport.h"
#include "../explorer_m2_1/proposed_gpu_device.h"

#include <unordered_map>
#include <vector>
#include <memory>
#include <iostream>
#include <cstring>
#include <d3dcompiler.h>

namespace RemoteGPU {

class PrototypeServerDispatcher {
public:
    PrototypeServerDispatcher(HostGpuDevice* gpu, NetworkSocket* socket)
        : m_gpu(gpu), m_socket(socket) {}

    ~PrototypeServerDispatcher() {
        ResetSession();
    }

    void ResetSession() {
        if (m_gpu && m_gpu->GetContext()) {
            m_gpu->GetContext()->ClearState();
            m_gpu->GetContext()->Flush();
        }
        m_buffers.clear();
        m_textures.clear();
        m_srvs.clear();
        m_rtvs.clear();
        m_dsvs.clear();
        m_vertex_shaders.clear();
        m_pixel_shaders.clear();
        m_input_layouts.clear();
        m_samplers.clear();
        m_handshake_complete = false;
        m_session_active = true;
    }

    bool RunSession() {
        while (m_session_active) {
            if (!ProcessNextPacket()) {
                break;
            }
        }
        ResetSession();
        return true;
    }

    bool ProcessNextPacket() {
        if (!m_socket || !m_socket->IsValid()) return false;

        PacketHeader header{};
        std::vector<uint8_t> payload;
        if (!m_socket->RecvPacket(header, payload)) {
            // Disconnect or error
            m_session_active = false;
            return false;
        }

        return DispatchPacket(header, payload.data(), payload.size());
    }

    bool DispatchPacket(const PacketHeader& header, const uint8_t* payload, size_t payload_size) {
        if (header.magic != RGPU_MAGIC) {
            std::cerr << "[Dispatcher] Invalid magic: 0x" << std::hex << header.magic << std::dec << std::endl;
            return false;
        }

        switch (header.opcode) {
            case OP_HANDSHAKE_REQUEST:
                return HandleHandshake(payload, payload_size);
            case OP_SHUTDOWN:
                m_session_active = false;
                return true;
            case OP_CREATE_BUFFER:
                return HandleCreateBuffer(payload, payload_size);
            case OP_CREATE_TEXTURE2D:
                return HandleCreateTexture2D(payload, payload_size);
            case OP_CREATE_SRV:
                return HandleCreateSrv(payload, payload_size);
            case OP_CREATE_RTV:
                return HandleCreateRtv(payload, payload_size);
            case OP_CREATE_DSV:
                return HandleCreateDsv(payload, payload_size);
            case OP_CREATE_VERTEX_SHADER:
                return HandleCreateVertexShader(payload, payload_size);
            case OP_CREATE_PIXEL_SHADER:
                return HandleCreatePixelShader(payload, payload_size);
            case OP_CREATE_INPUT_LAYOUT:
                return HandleCreateInputLayout(payload, payload_size);
            case OP_DESTROY_RESOURCE:
                return HandleDestroyResource(payload, payload_size);
            case OP_CREATE_SAMPLER:
                return HandleCreateSampler(payload, payload_size);

            case OP_IA_SET_INPUT_LAYOUT:
                return HandleIASetInputLayout(payload, payload_size);
            case OP_IA_SET_VERTEX_BUFFER:
                return HandleIASetVertexBuffer(payload, payload_size);
            case OP_IA_SET_INDEX_BUFFER:
                return HandleIASetIndexBuffer(payload, payload_size);
            case OP_IA_SET_TOPOLOGY:
                return HandleIASetTopology(payload, payload_size);
            case OP_VS_SET_SHADER:
                return HandleVSSetShader(payload, payload_size);
            case OP_VS_SET_CB:
                return HandleVSSetCB(payload, payload_size);
            case OP_PS_SET_SHADER:
                return HandlePSSetShader(payload, payload_size);
            case OP_PS_SET_CB:
                return HandlePSSetCB(payload, payload_size);
            case OP_PS_SET_SRV:
                return HandlePSSetSrv(payload, payload_size);
            case OP_PS_SET_SAMPLER:
                return HandlePSSetSampler(payload, payload_size);
            case OP_RS_SET_VIEWPORT:
                return HandleRSSetViewport(payload, payload_size);
            case OP_OM_SET_RENDER_TARGETS:
                return HandleOMSetRenderTargets(payload, payload_size);

            case OP_UPDATE_SUBRESOURCE:
                return HandleUpdateSubresource(payload, payload_size);
            case OP_WRITE_BUFFER_DISCARD:
                return HandleWriteBufferDiscard(payload, payload_size);
            case OP_CLEAR_RTV:
                return HandleClearRtv(payload, payload_size);
            case OP_CLEAR_DSV:
                return HandleClearDsv(payload, payload_size);
            case OP_DRAW:
                return HandleDraw(payload, payload_size);
            case OP_DRAW_INDEXED:
                return HandleDrawIndexed(payload, payload_size);
            case OP_PRESENT:
                return HandlePresent(payload, payload_size);
            default:
                std::cerr << "[Dispatcher] Unknown opcode: 0x" << std::hex << header.opcode << std::dec << std::endl;
                return false;
        }
    }

    bool IsHandshakeComplete() const { return m_handshake_complete; }
    size_t GetResourceCount() const {
        return m_buffers.size() + m_textures.size() + m_srvs.size() +
               m_rtvs.size() + m_dsvs.size() + m_vertex_shaders.size() +
               m_pixel_shaders.size() + m_input_layouts.size() + m_samplers.size();
    }
    bool HasBuffer(uint32_t id) const { return m_buffers.find(id) != m_buffers.end(); }
    bool HasVertexShader(uint32_t id) const { return m_vertex_shaders.find(id) != m_vertex_shaders.end(); }
    bool HasPixelShader(uint32_t id) const { return m_pixel_shaders.find(id) != m_pixel_shaders.end(); }

private:
    bool HandleHandshake(const uint8_t* payload, size_t size) {
        if (size < sizeof(HandshakeRequestPayload)) return false;
        const auto* req = reinterpret_cast<const HandshakeRequestPayload*>(payload);
        if (req->protocol_version != RGPU_PROTOCOL_VERSION) {
            std::cerr << "[Dispatcher] Handshake protocol version mismatch: " << req->protocol_version << std::endl;
            return false;
        }

        // Initialize offscreen targets to requested size (or fallback default 256x256)
        uint32_t w = req->client_width > 0 ? req->client_width : 256;
        uint32_t h = req->client_height > 0 ? req->client_height : 256;
        if (!m_gpu->CreateOffscreenTargets(w, h)) {
            std::cerr << "[Dispatcher] Failed to create offscreen targets for handshake" << std::endl;
            return false;
        }

        HandshakeResponsePayload resp{};
        resp.protocol_version = RGPU_PROTOCOL_VERSION;
        resp.status_code = 0;
        resp.feature_level = static_cast<uint32_t>(m_gpu->GetFeatureLevel());
        resp.frame_width = w;
        resp.frame_height = h;
        resp.frame_format = 28; // DXGI_FORMAT_R8G8B8A8_UNORM
        resp.dedicated_vram = m_gpu->GetDedicatedVram();
        const std::string& desc = m_gpu->GetAdapterDescription();
        strncpy(resp.adapter_desc, desc.c_str(), sizeof(resp.adapter_desc) - 1);

        PacketHeader resp_hdr{};
        resp_hdr.magic = RGPU_MAGIC;
        resp_hdr.opcode = OP_HANDSHAKE_RESPONSE;
        resp_hdr.flags = FLAG_NONE;
        resp_hdr.sequence_id = 0;
        resp_hdr.payload_length = sizeof(resp);

        m_handshake_complete = true;
        if (m_socket && m_socket->IsValid()) {
            return m_socket->SendPacket(resp_hdr, &resp);
        }
        return true;
    }

    bool HandleCreateBuffer(const uint8_t* payload, size_t size) {
        if (size < sizeof(CreateBufferPayload)) return false;
        const auto* p = reinterpret_cast<const CreateBufferPayload*>(payload);

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = p->byte_width;
        desc.Usage = static_cast<D3D11_USAGE>(p->usage);
        desc.BindFlags = p->bind_flags;
        desc.CPUAccessFlags = p->cpu_access_flags;
        desc.MiscFlags = p->misc_flags;
        desc.StructureByteStride = p->structure_byte_stride;

        D3D11_SUBRESOURCE_DATA init_data{};
        D3D11_SUBRESOURCE_DATA* p_init = nullptr;
        if (p->has_initial_data && size >= sizeof(CreateBufferPayload) + p->initial_data_size) {
            init_data.pSysMem = payload + sizeof(CreateBufferPayload);
            p_init = &init_data;
        }

        Microsoft::WRL::ComPtr<ID3D11Buffer> buf;
        HRESULT hr = m_gpu->GetDevice()->CreateBuffer(&desc, p_init, &buf);
        if (FAILED(hr)) {
            std::cerr << "[Dispatcher] CreateBuffer failed: 0x" << std::hex << hr << std::dec << std::endl;
            return false;
        }
        m_buffers[p->res_id] = buf;
        return true;
    }

    bool HandleCreateTexture2D(const uint8_t* payload, size_t size) {
        if (size < sizeof(CreateTexture2DPayload)) return false;
        const auto* p = reinterpret_cast<const CreateTexture2DPayload*>(payload);

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = p->width;
        desc.Height = p->height;
        desc.MipLevels = p->mips;
        desc.ArraySize = p->array_size;
        desc.Format = static_cast<DXGI_FORMAT>(p->format);
        desc.SampleDesc.Count = p->sample_count > 0 ? p->sample_count : 1;
        desc.SampleDesc.Quality = p->sample_quality;
        desc.Usage = static_cast<D3D11_USAGE>(p->usage);
        desc.BindFlags = p->bind_flags;
        desc.CPUAccessFlags = p->cpu_access_flags;
        desc.MiscFlags = p->misc_flags;

        D3D11_SUBRESOURCE_DATA init_data{};
        D3D11_SUBRESOURCE_DATA* p_init = nullptr;
        if (p->has_initial_data && size >= sizeof(CreateTexture2DPayload) + p->initial_data_size) {
            init_data.pSysMem = payload + sizeof(CreateTexture2DPayload);
            init_data.SysMemPitch = p->sys_mem_pitch;
            init_data.SysMemSlicePitch = p->sys_mem_slice_pitch;
            p_init = &init_data;
        }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        HRESULT hr = m_gpu->GetDevice()->CreateTexture2D(&desc, p_init, &tex);
        if (FAILED(hr)) return false;
        m_textures[p->res_id] = tex;
        return true;
    }

    bool HandleCreateSrv(const uint8_t* payload, size_t size) {
        if (size < sizeof(CreateSrvPayload)) return false;
        const auto* p = reinterpret_cast<const CreateSrvPayload*>(payload);
        auto it = m_textures.find(p->resource_id);
        if (it == m_textures.end()) return false;

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        HRESULT hr = m_gpu->GetDevice()->CreateShaderResourceView(it->second.Get(), nullptr, &srv);
        if (FAILED(hr)) return false;
        m_srvs[p->res_id] = srv;
        return true;
    }

    bool HandleCreateRtv(const uint8_t* payload, size_t size) {
        if (size < sizeof(CreateRtvPayload)) return false;
        const auto* p = reinterpret_cast<const CreateRtvPayload*>(payload);
        auto it = m_textures.find(p->resource_id);
        if (it == m_textures.end()) return false;

        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        HRESULT hr = m_gpu->GetDevice()->CreateRenderTargetView(it->second.Get(), nullptr, &rtv);
        if (FAILED(hr)) return false;
        m_rtvs[p->res_id] = rtv;
        return true;
    }

    bool HandleCreateDsv(const uint8_t* payload, size_t size) {
        if (size < sizeof(CreateDsvPayload)) return false;
        const auto* p = reinterpret_cast<const CreateDsvPayload*>(payload);
        auto it = m_textures.find(p->resource_id);
        if (it == m_textures.end()) return false;

        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
        HRESULT hr = m_gpu->GetDevice()->CreateDepthStencilView(it->second.Get(), nullptr, &dsv);
        if (FAILED(hr)) return false;
        m_dsvs[p->res_id] = dsv;
        return true;
    }

    bool HandleCreateVertexShader(const uint8_t* payload, size_t size) {
        if (size < sizeof(CreateShaderPayload)) return false;
        const auto* p = reinterpret_cast<const CreateShaderPayload*>(payload);
        if (size < sizeof(CreateShaderPayload) + p->bytecode_length) return false;

        const void* code = payload + sizeof(CreateShaderPayload);
        Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
        HRESULT hr = m_gpu->GetDevice()->CreateVertexShader(code, p->bytecode_length, nullptr, &vs);
        if (FAILED(hr)) {
            std::cerr << "[Dispatcher] CreateVertexShader failed: 0x" << std::hex << hr << std::dec << std::endl;
            return false;
        }
        m_vertex_shaders[p->res_id] = vs;
        return true;
    }

    bool HandleCreatePixelShader(const uint8_t* payload, size_t size) {
        if (size < sizeof(CreateShaderPayload)) return false;
        const auto* p = reinterpret_cast<const CreateShaderPayload*>(payload);
        if (size < sizeof(CreateShaderPayload) + p->bytecode_length) return false;

        const void* code = payload + sizeof(CreateShaderPayload);
        Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
        HRESULT hr = m_gpu->GetDevice()->CreatePixelShader(code, p->bytecode_length, nullptr, &ps);
        if (FAILED(hr)) {
            std::cerr << "[Dispatcher] CreatePixelShader failed: 0x" << std::hex << hr << std::dec << std::endl;
            return false;
        }
        m_pixel_shaders[p->res_id] = ps;
        return true;
    }

    bool HandleCreateInputLayout(const uint8_t* payload, size_t size) {
        if (size < sizeof(CreateInputLayoutPayload)) return false;
        const auto* p = reinterpret_cast<const CreateInputLayoutPayload*>(payload);

        size_t elem_bytes = p->num_elements * sizeof(RgpuInputElementDesc);
        if (size < sizeof(CreateInputLayoutPayload) + elem_bytes + p->bytecode_length) return false;

        const auto* raw_elems = reinterpret_cast<const RgpuInputElementDesc*>(payload + sizeof(CreateInputLayoutPayload));
        const void* code = payload + sizeof(CreateInputLayoutPayload) + elem_bytes;

        std::vector<D3D11_INPUT_ELEMENT_DESC> descs(p->num_elements);
        for (uint32_t i = 0; i < p->num_elements; ++i) {
            descs[i].SemanticName = raw_elems[i].semantic_name;
            descs[i].SemanticIndex = raw_elems[i].semantic_index;
            descs[i].Format = static_cast<DXGI_FORMAT>(raw_elems[i].format);
            descs[i].InputSlot = raw_elems[i].input_slot;
            descs[i].AlignedByteOffset = raw_elems[i].aligned_byte_offset;
            descs[i].InputSlotClass = static_cast<D3D11_INPUT_CLASSIFICATION>(raw_elems[i].input_slot_class);
            descs[i].InstanceDataStepRate = raw_elems[i].instance_data_step;
        }

        Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;
        HRESULT hr = m_gpu->GetDevice()->CreateInputLayout(descs.data(), p->num_elements, code, p->bytecode_length, &layout);
        if (FAILED(hr)) {
            std::cerr << "[Dispatcher] CreateInputLayout failed: 0x" << std::hex << hr << std::dec << std::endl;
            return false;
        }
        m_input_layouts[p->res_id] = layout;
        return true;
    }

    bool HandleDestroyResource(const uint8_t* payload, size_t size) {
        if (size < sizeof(DestroyResourcePayload)) return false;
        const auto* p = reinterpret_cast<const DestroyResourcePayload*>(payload);
        m_buffers.erase(p->res_id);
        m_textures.erase(p->res_id);
        m_srvs.erase(p->res_id);
        m_rtvs.erase(p->res_id);
        m_dsvs.erase(p->res_id);
        m_vertex_shaders.erase(p->res_id);
        m_pixel_shaders.erase(p->res_id);
        m_input_layouts.erase(p->res_id);
        m_samplers.erase(p->res_id);
        return true;
    }

    bool HandleCreateSampler(const uint8_t* payload, size_t size) {
        if (size < sizeof(CreateSamplerPayload)) return false;
        const auto* p = reinterpret_cast<const CreateSamplerPayload*>(payload);

        D3D11_SAMPLER_DESC desc{};
        desc.Filter = static_cast<D3D11_FILTER>(p->filter);
        desc.AddressU = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(p->address_u);
        desc.AddressV = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(p->address_v);
        desc.AddressW = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(p->address_w);
        desc.MipLODBias = p->mip_lod_bias;
        desc.MaxAnisotropy = p->max_anisotropy;
        desc.ComparisonFunc = static_cast<D3D11_COMPARISON_FUNC>(p->comparison_func);
        memcpy(desc.BorderColor, p->border_color, sizeof(desc.BorderColor));
        desc.MinLOD = p->min_lod;
        desc.MaxLOD = p->max_lod;

        Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
        HRESULT hr = m_gpu->GetDevice()->CreateSamplerState(&desc, &sampler);
        if (FAILED(hr)) return false;
        m_samplers[p->res_id] = sampler;
        return true;
    }

    bool HandleIASetInputLayout(const uint8_t* payload, size_t size) {
        if (size < sizeof(IASetInputLayoutPayload)) return false;
        const auto* p = reinterpret_cast<const IASetInputLayoutPayload*>(payload);
        if (p->res_id == 0) {
            m_gpu->GetContext()->IASetInputLayout(nullptr);
            return true;
        }
        auto it = m_input_layouts.find(p->res_id);
        if (it == m_input_layouts.end()) return false;
        m_gpu->GetContext()->IASetInputLayout(it->second.Get());
        return true;
    }

    bool HandleIASetVertexBuffer(const uint8_t* payload, size_t size) {
        if (size < sizeof(IASetVertexBufferPayload)) return false;
        const auto* p = reinterpret_cast<const IASetVertexBufferPayload*>(payload);
        if (p->res_id == 0) {
            ID3D11Buffer* null_buf = nullptr;
            UINT stride = 0, offset = 0;
            m_gpu->GetContext()->IASetVertexBuffers(p->slot, 1, &null_buf, &stride, &offset);
            return true;
        }
        auto it = m_buffers.find(p->res_id);
        if (it == m_buffers.end()) return false;
        ID3D11Buffer* buf = it->second.Get();
        UINT stride = p->stride;
        UINT offset = p->offset;
        m_gpu->GetContext()->IASetVertexBuffers(p->slot, 1, &buf, &stride, &offset);
        return true;
    }

    bool HandleIASetIndexBuffer(const uint8_t* payload, size_t size) {
        if (size < sizeof(IASetIndexBufferPayload)) return false;
        const auto* p = reinterpret_cast<const IASetIndexBufferPayload*>(payload);
        if (p->res_id == 0) {
            m_gpu->GetContext()->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
            return true;
        }
        auto it = m_buffers.find(p->res_id);
        if (it == m_buffers.end()) return false;
        m_gpu->GetContext()->IASetIndexBuffer(it->second.Get(), static_cast<DXGI_FORMAT>(p->format), p->offset);
        return true;
    }

    bool HandleIASetTopology(const uint8_t* payload, size_t size) {
        if (size < sizeof(IASetTopologyPayload)) return false;
        const auto* p = reinterpret_cast<const IASetTopologyPayload*>(payload);
        m_gpu->GetContext()->IASetPrimitiveTopology(static_cast<D3D11_PRIMITIVE_TOPOLOGY>(p->topology));
        return true;
    }

    bool HandleVSSetShader(const uint8_t* payload, size_t size) {
        if (size < sizeof(VSSetShaderPayload)) return false;
        const auto* p = reinterpret_cast<const VSSetShaderPayload*>(payload);
        if (p->res_id == 0) {
            m_gpu->GetContext()->VSSetShader(nullptr, nullptr, 0);
            return true;
        }
        auto it = m_vertex_shaders.find(p->res_id);
        if (it == m_vertex_shaders.end()) return false;
        m_gpu->GetContext()->VSSetShader(it->second.Get(), nullptr, 0);
        return true;
    }

    bool HandleVSSetCB(const uint8_t* payload, size_t size) {
        if (size < sizeof(VSSetConstantBufferPayload)) return false;
        const auto* p = reinterpret_cast<const VSSetConstantBufferPayload*>(payload);
        ID3D11Buffer* buf = nullptr;
        if (p->res_id != 0) {
            auto it = m_buffers.find(p->res_id);
            if (it == m_buffers.end()) return false;
            buf = it->second.Get();
        }
        m_gpu->GetContext()->VSSetConstantBuffers(p->slot, 1, &buf);
        return true;
    }

    bool HandlePSSetShader(const uint8_t* payload, size_t size) {
        if (size < sizeof(PSSetShaderPayload)) return false;
        const auto* p = reinterpret_cast<const PSSetShaderPayload*>(payload);
        if (p->res_id == 0) {
            m_gpu->GetContext()->PSSetShader(nullptr, nullptr, 0);
            return true;
        }
        auto it = m_pixel_shaders.find(p->res_id);
        if (it == m_pixel_shaders.end()) return false;
        m_gpu->GetContext()->PSSetShader(it->second.Get(), nullptr, 0);
        return true;
    }

    bool HandlePSSetCB(const uint8_t* payload, size_t size) {
        if (size < sizeof(PSSetConstantBufferPayload)) return false;
        const auto* p = reinterpret_cast<const PSSetConstantBufferPayload*>(payload);
        ID3D11Buffer* buf = nullptr;
        if (p->res_id != 0) {
            auto it = m_buffers.find(p->res_id);
            if (it == m_buffers.end()) return false;
            buf = it->second.Get();
        }
        m_gpu->GetContext()->PSSetConstantBuffers(p->slot, 1, &buf);
        return true;
    }

    bool HandlePSSetSrv(const uint8_t* payload, size_t size) {
        if (size < sizeof(PSSetSrvPayload)) return false;
        const auto* p = reinterpret_cast<const PSSetSrvPayload*>(payload);
        ID3D11ShaderResourceView* srv = nullptr;
        if (p->res_id != 0) {
            auto it = m_srvs.find(p->res_id);
            if (it == m_srvs.end()) return false;
            srv = it->second.Get();
        }
        m_gpu->GetContext()->PSSetShaderResources(p->slot, 1, &srv);
        return true;
    }

    bool HandlePSSetSampler(const uint8_t* payload, size_t size) {
        if (size < sizeof(PSSetSamplerPayload)) return false;
        const auto* p = reinterpret_cast<const PSSetSamplerPayload*>(payload);
        ID3D11SamplerState* s = nullptr;
        if (p->res_id != 0) {
            auto it = m_samplers.find(p->res_id);
            if (it == m_samplers.end()) return false;
            s = it->second.Get();
        }
        m_gpu->GetContext()->PSSetSamplers(p->slot, 1, &s);
        return true;
    }

    bool HandleRSSetViewport(const uint8_t* payload, size_t size) {
        if (size < sizeof(RSSetViewportPayload)) return false;
        const auto* p = reinterpret_cast<const RSSetViewportPayload*>(payload);
        D3D11_VIEWPORT vp{};
        vp.TopLeftX = p->top_left_x;
        vp.TopLeftY = p->top_left_y;
        vp.Width = p->width;
        vp.Height = p->height;
        vp.MinDepth = p->min_depth;
        vp.MaxDepth = p->max_depth;
        m_gpu->GetContext()->RSSetViewports(1, &vp);
        return true;
    }

    bool HandleOMSetRenderTargets(const uint8_t* payload, size_t size) {
        if (size < sizeof(OMSetRenderTargetsPayload)) return false;
        const auto* p = reinterpret_cast<const OMSetRenderTargetsPayload*>(payload);
        ID3D11RenderTargetView* rtv = nullptr;
        ID3D11DepthStencilView* dsv = nullptr;
        if (p->rtv_id == 0) {
            rtv = m_gpu->GetRenderTargetView(); // Default offscreen RTV
        } else {
            auto it = m_rtvs.find(p->rtv_id);
            if (it != m_rtvs.end()) rtv = it->second.Get();
        }
        if (p->dsv_id == 0) {
            dsv = m_gpu->GetDepthStencilView(); // Default offscreen DSV
        } else {
            auto it = m_dsvs.find(p->dsv_id);
            if (it != m_dsvs.end()) dsv = it->second.Get();
        }
        m_gpu->GetContext()->OMSetRenderTargets(1, &rtv, dsv);
        return true;
    }

    bool HandleUpdateSubresource(const uint8_t* payload, size_t size) {
        if (size < sizeof(UpdateSubresourcePayload)) return false;
        const auto* p = reinterpret_cast<const UpdateSubresourcePayload*>(payload);
        if (size < sizeof(UpdateSubresourcePayload) + p->data_size) return false;

        const void* data = payload + sizeof(UpdateSubresourcePayload);
        ID3D11Resource* res = nullptr;
        auto it_buf = m_buffers.find(p->res_id);
        if (it_buf != m_buffers.end()) res = it_buf->second.Get();
        else {
            auto it_tex = m_textures.find(p->res_id);
            if (it_tex != m_textures.end()) res = it_tex->second.Get();
        }
        if (!res) return false;

        D3D11_BOX box{};
        D3D11_BOX* p_box = nullptr;
        if (p->has_dst_box) {
            box.left = p->dst_box.left;
            box.top = p->dst_box.top;
            box.front = p->dst_box.front;
            box.right = p->dst_box.right;
            box.bottom = p->dst_box.bottom;
            box.back = p->dst_box.back;
            p_box = &box;
        }
        m_gpu->GetContext()->UpdateSubresource(res, p->subresource, p_box, data, p->row_pitch, p->depth_pitch);
        return true;
    }

    bool HandleWriteBufferDiscard(const uint8_t* payload, size_t size) {
        if (size < sizeof(WriteBufferDiscardPayload)) return false;
        const auto* p = reinterpret_cast<const WriteBufferDiscardPayload*>(payload);
        if (size < sizeof(WriteBufferDiscardPayload) + p->byte_width) return false;

        auto it = m_buffers.find(p->res_id);
        if (it == m_buffers.end()) return false;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = m_gpu->GetContext()->Map(it->second.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) return false;

        memcpy(static_cast<uint8_t*>(mapped.pData) + p->byte_offset, payload + sizeof(WriteBufferDiscardPayload), p->byte_width);
        m_gpu->GetContext()->Unmap(it->second.Get(), 0);
        return true;
    }

    bool HandleClearRtv(const uint8_t* payload, size_t size) {
        if (size < sizeof(ClearRtvPayload)) return false;
        const auto* p = reinterpret_cast<const ClearRtvPayload*>(payload);
        ID3D11RenderTargetView* rtv = nullptr;
        if (p->res_id == 0) {
            rtv = m_gpu->GetRenderTargetView();
        } else {
            auto it = m_rtvs.find(p->res_id);
            if (it != m_rtvs.end()) rtv = it->second.Get();
        }
        if (!rtv) return false;
        m_gpu->GetContext()->ClearRenderTargetView(rtv, p->color);
        return true;
    }

    bool HandleClearDsv(const uint8_t* payload, size_t size) {
        if (size < sizeof(ClearDsvPayload)) return false;
        const auto* p = reinterpret_cast<const ClearDsvPayload*>(payload);
        ID3D11DepthStencilView* dsv = nullptr;
        if (p->res_id == 0) {
            dsv = m_gpu->GetDepthStencilView();
        } else {
            auto it = m_dsvs.find(p->res_id);
            if (it != m_dsvs.end()) dsv = it->second.Get();
        }
        if (!dsv) return false;
        m_gpu->GetContext()->ClearDepthStencilView(dsv, p->clear_flags, p->depth, p->stencil);
        return true;
    }

    bool HandleDraw(const uint8_t* payload, size_t size) {
        if (size < sizeof(DrawPayload)) return false;
        const auto* p = reinterpret_cast<const DrawPayload*>(payload);
        m_gpu->GetContext()->Draw(p->vertex_count, p->start_vertex_location);
        return true;
    }

    bool HandleDrawIndexed(const uint8_t* payload, size_t size) {
        if (size < sizeof(DrawIndexedPayload)) return false;
        const auto* p = reinterpret_cast<const DrawIndexedPayload*>(payload);
        m_gpu->GetContext()->DrawIndexed(p->index_count, p->start_index_location, p->base_vertex_location);
        return true;
    }

    bool HandlePresent(const uint8_t* payload, size_t size) {
        if (size < sizeof(PresentPayload)) return false;
        const auto* p = reinterpret_cast<const PresentPayload*>(payload);

        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        if (!m_gpu->ReadbackFrame(rgba, w, h)) {
            std::cerr << "[Dispatcher] ReadbackFrame failed during Present" << std::endl;
            return false;
        }

        FrameDataPayload resp{};
        resp.swapchain_id = p->swapchain_id;
        resp.width = w;
        resp.height = h;
        resp.format = 28; // DXGI_FORMAT_R8G8B8A8_UNORM
        resp.row_pitch = w * 4;
        resp.data_size = static_cast<uint32_t>(rgba.size());
        resp.frame_number = p->frame_number;

        PacketHeader resp_hdr{};
        resp_hdr.magic = RGPU_MAGIC;
        resp_hdr.opcode = OP_FRAME_DATA;
        resp_hdr.flags = FLAG_END_OF_FRAME;
        resp_hdr.sequence_id = 1;
        resp_hdr.payload_length = sizeof(resp) + static_cast<uint32_t>(rgba.size());

        if (m_socket && m_socket->IsValid()) {
            std::vector<uint8_t> full_payload(sizeof(resp) + rgba.size());
            memcpy(full_payload.data(), &resp, sizeof(resp));
            memcpy(full_payload.data() + sizeof(resp), rgba.data(), rgba.size());
            return m_socket->SendPacket(resp_hdr, full_payload.data());
        }
        return true;
    }

    HostGpuDevice* m_gpu;
    NetworkSocket* m_socket;
    bool m_handshake_complete = false;
    bool m_session_active = true;

    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11Buffer>> m_buffers;
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11Texture2D>> m_textures;
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_srvs;
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> m_rtvs;
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11DepthStencilView>> m_dsvs;
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11VertexShader>> m_vertex_shaders;
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11PixelShader>> m_pixel_shaders;
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11InputLayout>> m_input_layouts;
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11SamplerState>> m_samplers;
};

} // namespace RemoteGPU

#endif // PROTOTYPE_SERVER_DISPATCHER_H
