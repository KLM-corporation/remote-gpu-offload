#include "proxy_context.h"

#include <algorithm>
#include <cstring>

namespace RemoteGPU {

ProxyD3D11DeviceContext::ProxyD3D11DeviceContext(ProxyD3D11Device* device) : m_device(device) {}

bool ProxyD3D11DeviceContext::Send(uint16_t opcode, const void* payload, uint32_t length) {
    return m_device && m_device->SendCommand(opcode, payload, length, FLAG_ONEWAY);
}

// =============================================================================
// IUnknown / ID3D11DeviceChild
// =============================================================================

HRESULT STDMETHODCALLTYPE ProxyD3D11DeviceContext::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) || riid == __uuidof(ID3D11DeviceContext)) {
        *ppvObject = static_cast<ID3D11DeviceContext*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE ProxyD3D11DeviceContext::AddRef() { return InterlockedIncrement(&m_refCount); }

ULONG STDMETHODCALLTYPE ProxyD3D11DeviceContext::Release() {
    ULONG count = InterlockedDecrement(&m_refCount);
    if (count == 0) delete this;
    return count;
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::GetDevice(ID3D11Device** ppDevice) {
    if (!ppDevice) return;
    *ppDevice = m_device;
    if (m_device) m_device->AddRef();
}

HRESULT STDMETHODCALLTYPE ProxyD3D11DeviceContext::GetPrivateData(REFGUID, UINT*, void*) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE ProxyD3D11DeviceContext::SetPrivateData(REFGUID, UINT, const void*) { return S_OK; }
HRESULT STDMETHODCALLTYPE ProxyD3D11DeviceContext::SetPrivateDataInterface(REFGUID, const IUnknown*) { return S_OK; }

// =============================================================================
// Input assembler
// =============================================================================

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::IASetInputLayout(ID3D11InputLayout* pInputLayout) {
    IASetInputLayoutPayload p = {GetResourceIdFromCOM(pInputLayout)};
    Send(OP_IA_SET_INPUT_LAYOUT, &p, sizeof(p));
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::IASetVertexBuffers(UINT StartSlot, UINT NumBuffers,
                                                                   ID3D11Buffer* const* ppVertexBuffers,
                                                                   const UINT* pStrides, const UINT* pOffsets) {
    for (UINT i = 0; i < NumBuffers; ++i) {
        IASetVertexBufferPayload p = {};
        p.slot = StartSlot + i;
        p.res_id = ppVertexBuffers ? GetResourceIdFromCOM(ppVertexBuffers[i]) : 0;
        p.stride = pStrides ? pStrides[i] : 0;
        p.offset = pOffsets ? pOffsets[i] : 0;
        Send(OP_IA_SET_VERTEX_BUFFER, &p, sizeof(p));
    }
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::IASetIndexBuffer(ID3D11Buffer* pIndexBuffer, DXGI_FORMAT Format, UINT Offset) {
    IASetIndexBufferPayload p = {GetResourceIdFromCOM(pIndexBuffer), static_cast<uint32_t>(Format), Offset};
    Send(OP_IA_SET_INDEX_BUFFER, &p, sizeof(p));
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology) {
    IASetTopologyPayload p = {static_cast<uint32_t>(Topology)};
    Send(OP_IA_SET_TOPOLOGY, &p, sizeof(p));
}

// =============================================================================
// Shader stages
// =============================================================================

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::VSSetShader(ID3D11VertexShader* pVertexShader, ID3D11ClassInstance* const*, UINT) {
    VSSetShaderPayload p = {GetResourceIdFromCOM(pVertexShader)};
    Send(OP_VS_SET_SHADER, &p, sizeof(p));
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::PSSetShader(ID3D11PixelShader* pPixelShader, ID3D11ClassInstance* const*, UINT) {
    PSSetShaderPayload p = {GetResourceIdFromCOM(pPixelShader)};
    Send(OP_PS_SET_SHADER, &p, sizeof(p));
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) {
    for (UINT i = 0; i < NumBuffers; ++i) {
        VSSetConstantBufferPayload p = {StartSlot + i, ppConstantBuffers ? GetResourceIdFromCOM(ppConstantBuffers[i]) : 0};
        Send(OP_VS_SET_CB, &p, sizeof(p));
    }
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) {
    for (UINT i = 0; i < NumBuffers; ++i) {
        PSSetConstantBufferPayload p = {StartSlot + i, ppConstantBuffers ? GetResourceIdFromCOM(ppConstantBuffers[i]) : 0};
        Send(OP_PS_SET_CB, &p, sizeof(p));
    }
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::PSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    for (UINT i = 0; i < NumViews; ++i) {
        PSSetSrvPayload p = {StartSlot + i, ppShaderResourceViews ? GetResourceIdFromCOM(ppShaderResourceViews[i]) : 0};
        Send(OP_PS_SET_SRV, &p, sizeof(p));
    }
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::PSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) {
    for (UINT i = 0; i < NumSamplers; ++i) {
        PSSetSamplerPayload p = {StartSlot + i, ppSamplers ? GetResourceIdFromCOM(ppSamplers[i]) : 0};
        Send(OP_PS_SET_SAMPLER, &p, sizeof(p));
    }
}

// =============================================================================
// Rasterizer / output merger
// =============================================================================

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT* pViewports) {
    if (NumViewports == 0 || !pViewports) return;
    RSSetViewportPayload p = {pViewports[0].TopLeftX, pViewports[0].TopLeftY, pViewports[0].Width,
                              pViewports[0].Height, pViewports[0].MinDepth, pViewports[0].MaxDepth};
    Send(OP_RS_SET_VIEWPORT, &p, sizeof(p));
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::OMSetRenderTargets(UINT NumViews, ID3D11RenderTargetView* const* ppRenderTargetViews,
                                                                   ID3D11DepthStencilView* pDepthStencilView) {
    OMSetRenderTargetsPayload p = {};
    p.rtv_id = (NumViews > 0 && ppRenderTargetViews) ? GetResourceIdFromCOM(ppRenderTargetViews[0]) : 0;
    p.dsv_id = GetResourceIdFromCOM(pDepthStencilView);
    Send(OP_OM_SET_RENDER_TARGETS, &p, sizeof(p));
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::OMSetRenderTargetsAndUnorderedAccessViews(
    UINT NumRTVs, ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView,
    UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*) {
    if (NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
        OMSetRenderTargets(NumRTVs, ppRenderTargetViews, pDepthStencilView);
    }
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::ClearRenderTargetView(ID3D11RenderTargetView* pRenderTargetView, const FLOAT ColorRGBA[4]) {
    if (!pRenderTargetView || !ColorRGBA) return;
    ClearRtvPayload p = {};
    p.res_id = GetResourceIdFromCOM(pRenderTargetView);
    std::memcpy(p.color, ColorRGBA, sizeof(p.color));
    Send(OP_CLEAR_RTV, &p, sizeof(p));
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::ClearDepthStencilView(ID3D11DepthStencilView* pDepthStencilView, UINT ClearFlags,
                                                                      FLOAT Depth, UINT8 Stencil) {
    if (!pDepthStencilView) return;
    ClearDsvPayload p = {};
    p.res_id = GetResourceIdFromCOM(pDepthStencilView);
    p.clear_flags = ClearFlags;
    p.depth = Depth;
    p.stencil = Stencil;
    Send(OP_CLEAR_DSV, &p, sizeof(p));
}

// =============================================================================
// Draw calls
// =============================================================================

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::Draw(UINT VertexCount, UINT StartVertexLocation) {
    DrawPayload p = {VertexCount, StartVertexLocation};
    Send(OP_DRAW, &p, sizeof(p));
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) {
    DrawIndexedPayload p = {IndexCount, StartIndexLocation, BaseVertexLocation};
    Send(OP_DRAW_INDEXED, &p, sizeof(p));
}

// =============================================================================
// Dynamic buffer mapping (client-side shadow memory)
// =============================================================================

HRESULT STDMETHODCALLTYPE ProxyD3D11DeviceContext::Map(ID3D11Resource* pResource, UINT Subresource, D3D11_MAP MapType, UINT,
                                                       D3D11_MAPPED_SUBRESOURCE* pMappedResource) {
    if (!pResource || !pMappedResource) return E_INVALIDARG;

    D3D11_RESOURCE_DIMENSION dim;
    pResource->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return E_NOTIMPL; // texture CPU mapping is not forwarded

    auto* buffer = static_cast<ProxyBuffer*>(static_cast<ID3D11Buffer*>(pResource));
    if (!buffer->GetShadowData()) return E_FAIL; // not a CPU-writable buffer
    if (MapType == D3D11_MAP_READ || MapType == D3D11_MAP_READ_WRITE) return E_NOTIMPL;
    if (buffer->IsMapped()) return DXGI_ERROR_WAS_STILL_DRAWING;

    buffer->SetMapped(true, Subresource, MapType);
    pMappedResource->pData = buffer->GetShadowData();
    pMappedResource->RowPitch = buffer->GetDesc().ByteWidth;
    pMappedResource->DepthPitch = buffer->GetDesc().ByteWidth;
    return S_OK;
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::Unmap(ID3D11Resource* pResource, UINT) {
    if (!pResource) return;
    D3D11_RESOURCE_DIMENSION dim;
    pResource->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return;

    auto* buffer = static_cast<ProxyBuffer*>(static_cast<ID3D11Buffer*>(pResource));
    if (!buffer->IsMapped()) return;
    buffer->SetMapped(false);

    const uint32_t size = buffer->GetDesc().ByteWidth;
    std::vector<uint8_t> msg(sizeof(WriteBufferDiscardPayload) + size);
    WriteBufferDiscardPayload head = {buffer->GetResourceId(), 0, size};
    std::memcpy(msg.data(), &head, sizeof(head));
    std::memcpy(msg.data() + sizeof(head), buffer->GetShadowData(), size);
    Send(OP_WRITE_BUFFER_DISCARD, msg.data(), static_cast<uint32_t>(msg.size()));
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::UpdateSubresource(ID3D11Resource* pDstResource, UINT DstSubresource,
                                                                  const D3D11_BOX* pDstBox, const void* pSrcData,
                                                                  UINT SrcRowPitch, UINT SrcDepthPitch) {
    if (!pDstResource || !pSrcData) return;

    D3D11_RESOURCE_DIMENSION dim;
    pDstResource->GetType(&dim);

    uint32_t data_size = 0;
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC desc;
        static_cast<ID3D11Buffer*>(pDstResource)->GetDesc(&desc);
        data_size = pDstBox ? (pDstBox->right - pDstBox->left) : desc.ByteWidth;
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC desc;
        static_cast<ID3D11Texture2D*>(pDstResource)->GetDesc(&desc);
        const uint32_t rows = pDstBox ? (pDstBox->bottom - pDstBox->top) : desc.Height;
        data_size = SrcRowPitch * rows;
    } else {
        return;
    }
    if (data_size == 0) return;

    UpdateSubresourcePayload head = {};
    head.res_id = GetResourceIdFromCOM(pDstResource);
    head.subresource = DstSubresource;
    head.has_dst_box = pDstBox ? 1 : 0;
    if (pDstBox) head.dst_box = {pDstBox->left, pDstBox->top, pDstBox->front, pDstBox->right, pDstBox->bottom, pDstBox->back};
    head.row_pitch = SrcRowPitch;
    head.depth_pitch = SrcDepthPitch;
    head.data_size = data_size;

    std::vector<uint8_t> msg(sizeof(head) + data_size);
    std::memcpy(msg.data(), &head, sizeof(head));
    std::memcpy(msg.data() + sizeof(head), pSrcData, data_size);
    Send(OP_UPDATE_SUBRESOURCE, msg.data(), static_cast<uint32_t>(msg.size()));
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::Flush() {}
void STDMETHODCALLTYPE ProxyD3D11DeviceContext::ClearState() {}

// =============================================================================
// Remaining ID3D11DeviceContext entry points: benign defaults
// =============================================================================

#include "generated/proxy_context_stubs.inc"

} // namespace RemoteGPU
