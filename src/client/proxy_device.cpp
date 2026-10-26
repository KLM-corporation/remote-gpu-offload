#include "proxy_device.h"
#include "proxy_context.h"
#include "proxy_swapchain.h"
#include "proxy_dxgi.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace RemoteGPU {

namespace {

// Stateless pipeline-state objects (blend / depth-stencil / rasterizer) have no wire opcode.
// The host dispatcher renders with its own defaults, so the client only needs a valid COM
// object that reports back the description the application supplied.
template <typename Iface, typename Desc>
class ProxyStateObject : public Iface, public ProxyResourceBase {
public:
    ProxyStateObject(const Desc& desc, ProxyD3D11Device* device)
        : ProxyResourceBase(0, device), m_desc(desc) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) || riid == __uuidof(Iface)) {
            *ppv = static_cast<Iface*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_IProxyResource) {
            *ppv = static_cast<IProxyResource*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return AddRefBase(); }
    ULONG STDMETHODCALLTYPE Release() override { return ReleaseBase(); }

    void STDMETHODCALLTYPE GetDevice(ID3D11Device** ppDevice) override {
        if (ppDevice && m_device) {
            *ppDevice = m_device;
            m_device->AddRef();
        }
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
    void STDMETHODCALLTYPE GetDesc(Desc* pDesc) override {
        if (pDesc) *pDesc = m_desc;
    }

private:
    Desc m_desc;
};

} // namespace

// =============================================================================
// Construction / Lifetime
// =============================================================================

ProxyD3D11Device::ProxyD3D11Device(std::shared_ptr<NetworkSocket> socket, D3D_FEATURE_LEVEL featureLevel)
    : m_socket(std::move(socket)), m_featureLevel(featureLevel) {}

ProxyD3D11Device::~ProxyD3D11Device() {
    if (m_socket && m_socket->IsValid() && !m_deviceRemoved) {
        ShutdownPayload p = {};
        p.exit_code = 0;
        SendCommand(OP_SHUTDOWN, &p, sizeof(p), FLAG_ONEWAY);
    }
    // Break the device <-> context cycle (context only holds a raw device pointer).
    m_immediateContext.Reset();
}

ProxyD3D11DeviceContext* ProxyD3D11Device::GetProxyContext() const noexcept {
    return m_immediateContext.Get();
}

void ProxyD3D11Device::SetProxyContext(Microsoft::WRL::ComPtr<ProxyD3D11DeviceContext> ctx) {
    m_immediateContext = std::move(ctx);
}

bool ProxyD3D11Device::SendPacket(const PacketHeader& header, const void* payload) {
    if (!m_socket || !m_socket->IsValid() || m_deviceRemoved) return false;
    std::lock_guard<std::mutex> lock(m_socketMutex);
    if (!m_socket->SendPacket(header, payload)) {
        m_deviceRemoved = true;
        return false;
    }
    return true;
}

bool ProxyD3D11Device::SendCommand(uint16_t opcode, const void* payload, uint32_t payload_len, uint16_t flags) {
    PacketHeader header = {};
    header.magic = RGPU_MAGIC;
    header.opcode = opcode;
    header.flags = flags;
    header.sequence_id = NextSequenceId();
    header.payload_length = payload_len;
    return SendPacket(header, payload_len ? payload : nullptr);
}

void ProxyD3D11Device::NotifyResourceDestroyed(uint32_t res_id) {
    if (res_id == 0 || m_deviceRemoved) return;
    DestroyResourcePayload p = {res_id};
    SendCommand(OP_DESTROY_RESOURCE, &p, sizeof(p), FLAG_ONEWAY);
}

namespace {

// Sends `fixed` immediately followed by `blob` as a single packet payload.
bool SendWithBlob(ProxyD3D11Device* dev, uint16_t opcode, const void* fixed, size_t fixed_len,
                  const void* blob, size_t blob_len) {
    std::vector<uint8_t> buf(fixed_len + blob_len);
    std::memcpy(buf.data(), fixed, fixed_len);
    if (blob_len) std::memcpy(buf.data() + fixed_len, blob, blob_len);
    return dev->SendCommand(opcode, buf.data(), static_cast<uint32_t>(buf.size()), FLAG_ONEWAY);
}

} // namespace

// =============================================================================
// IUnknown
// =============================================================================

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11Device) || riid == IID_IProxyDevice) {
        *ppvObject = static_cast<ID3D11Device*>(this);
    } else if (riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDevice)) {
        *ppvObject = static_cast<IDXGIDevice*>(this);
    } else {
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

ULONG STDMETHODCALLTYPE ProxyD3D11Device::AddRef() {
    return InterlockedIncrement(&m_refCount);
}

ULONG STDMETHODCALLTYPE ProxyD3D11Device::Release() {
    ULONG count = InterlockedDecrement(&m_refCount);
    if (count == 0) delete this;
    return count;
}

// =============================================================================
// IDXGIObject / IDXGIDevice
// =============================================================================

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::SetPrivateData(REFGUID, UINT, const void*) { return S_OK; }
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::SetPrivateDataInterface(REFGUID, const IUnknown*) { return S_OK; }
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::GetPrivateData(REFGUID, UINT*, void*) { return DXGI_ERROR_NOT_FOUND; }
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::GetParent(REFIID, void** ppParent) {
    if (ppParent) *ppParent = nullptr;
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::GetAdapter(IDXGIAdapter** pAdapter) {
    if (!pAdapter) return E_INVALIDARG;
    *pAdapter = new ProxyDXGIAdapter(m_adapterInfo);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateSurface(const DXGI_SURFACE_DESC*, UINT, DXGI_USAGE,
                                                          const DXGI_SHARED_RESOURCE*, IDXGISurface** ppSurface) {
    if (ppSurface) *ppSurface = nullptr;
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::QueryResourceResidency(IUnknown* const*, DXGI_RESIDENCY* pResidencyStatus,
                                                                   UINT NumResources) {
    if (pResidencyStatus) {
        for (UINT i = 0; i < NumResources; ++i) pResidencyStatus[i] = DXGI_RESIDENCY_FULLY_RESIDENT;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::SetGPUThreadPriority(INT) { return S_OK; }
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::GetGPUThreadPriority(INT* pPriority) {
    if (pPriority) *pPriority = 0;
    return S_OK;
}

// =============================================================================
// Actively serialized resource creation
// =============================================================================

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateBuffer(const D3D11_BUFFER_DESC* pDesc,
                                                         const D3D11_SUBRESOURCE_DATA* pInitialData,
                                                         ID3D11Buffer** ppBuffer) {
    if (!pDesc) return E_INVALIDARG;
    if (!ppBuffer) return S_FALSE; // Validation-only call
    if (m_deviceRemoved) return DXGI_ERROR_DEVICE_REMOVED;

    const uint32_t id = AllocateResourceId();
    const bool has_data = pInitialData && pInitialData->pSysMem;

    CreateBufferPayload p = {};
    p.res_id = id;
    p.byte_width = pDesc->ByteWidth;
    p.usage = pDesc->Usage;
    p.bind_flags = pDesc->BindFlags;
    p.cpu_access_flags = pDesc->CPUAccessFlags;
    p.misc_flags = pDesc->MiscFlags;
    p.structure_byte_stride = pDesc->StructureByteStride;
    p.has_initial_data = has_data ? 1 : 0;
    p.initial_data_size = has_data ? pDesc->ByteWidth : 0;

    if (!SendWithBlob(this, OP_CREATE_BUFFER, &p, sizeof(p), has_data ? pInitialData->pSysMem : nullptr, p.initial_data_size))
        return DXGI_ERROR_DEVICE_REMOVED;

    *ppBuffer = new ProxyBuffer(id, *pDesc, this, has_data ? pInitialData->pSysMem : nullptr);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateTexture2D(const D3D11_TEXTURE2D_DESC* pDesc,
                                                            const D3D11_SUBRESOURCE_DATA* pInitialData,
                                                            ID3D11Texture2D** ppTexture2D) {
    if (!pDesc) return E_INVALIDARG;
    if (!ppTexture2D) return S_FALSE;
    if (m_deviceRemoved) return DXGI_ERROR_DEVICE_REMOVED;

    const uint32_t id = AllocateResourceId();
    const bool has_data = pInitialData && pInitialData->pSysMem;
    const uint32_t data_size = has_data ? pInitialData->SysMemPitch * pDesc->Height : 0;

    CreateTexture2DPayload p = {};
    p.res_id = id;
    p.width = pDesc->Width;
    p.height = pDesc->Height;
    p.mips = pDesc->MipLevels;
    p.array_size = pDesc->ArraySize;
    p.format = pDesc->Format;
    p.sample_count = pDesc->SampleDesc.Count;
    p.sample_quality = pDesc->SampleDesc.Quality;
    p.usage = pDesc->Usage;
    p.bind_flags = pDesc->BindFlags;
    p.cpu_access_flags = pDesc->CPUAccessFlags;
    p.misc_flags = pDesc->MiscFlags;
    p.has_initial_data = has_data ? 1 : 0;
    p.sys_mem_pitch = has_data ? pInitialData->SysMemPitch : 0;
    p.sys_mem_slice_pitch = has_data ? pInitialData->SysMemSlicePitch : 0;
    p.initial_data_size = data_size;

    if (!SendWithBlob(this, OP_CREATE_TEXTURE2D, &p, sizeof(p), has_data ? pInitialData->pSysMem : nullptr, data_size))
        return DXGI_ERROR_DEVICE_REMOVED;

    *ppTexture2D = new ProxyTexture2D(id, *pDesc, this);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateShaderResourceView(ID3D11Resource* pResource,
                                                                     const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc,
                                                                     ID3D11ShaderResourceView** ppSRView) {
    if (!pResource) return E_INVALIDARG;
    if (!ppSRView) return S_FALSE;

    const uint32_t id = AllocateResourceId();
    CreateSrvPayload p = {};
    p.res_id = id;
    p.resource_id = GetResourceIdFromCOM(pResource);
    p.format = pDesc ? pDesc->Format : DXGI_FORMAT_UNKNOWN;
    p.view_dimension = pDesc ? pDesc->ViewDimension : D3D11_SRV_DIMENSION_TEXTURE2D;
    p.most_detailed_mip = pDesc ? pDesc->Texture2D.MostDetailedMip : 0;
    p.mip_levels = pDesc ? pDesc->Texture2D.MipLevels : 0xFFFFFFFFu;

    if (!SendCommand(OP_CREATE_SRV, &p, sizeof(p), FLAG_ONEWAY)) return DXGI_ERROR_DEVICE_REMOVED;
    *ppSRView = new ProxyShaderResourceView(id, pResource, pDesc, this);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateRenderTargetView(ID3D11Resource* pResource,
                                                                   const D3D11_RENDER_TARGET_VIEW_DESC* pDesc,
                                                                   ID3D11RenderTargetView** ppRTView) {
    if (!pResource) return E_INVALIDARG;
    if (!ppRTView) return S_FALSE;

    const uint32_t id = AllocateResourceId();
    CreateRtvPayload p = {};
    p.res_id = id;
    p.resource_id = GetResourceIdFromCOM(pResource); // 0 == swap-chain back buffer (host offscreen target)
    p.format = pDesc ? pDesc->Format : DXGI_FORMAT_UNKNOWN;
    p.view_dimension = pDesc ? pDesc->ViewDimension : D3D11_RTV_DIMENSION_TEXTURE2D;
    p.mip_slice = pDesc ? pDesc->Texture2D.MipSlice : 0;

    if (!SendCommand(OP_CREATE_RTV, &p, sizeof(p), FLAG_ONEWAY)) return DXGI_ERROR_DEVICE_REMOVED;
    *ppRTView = new ProxyRenderTargetView(id, pResource, pDesc, this);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateDepthStencilView(ID3D11Resource* pResource,
                                                                   const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc,
                                                                   ID3D11DepthStencilView** ppDepthStencilView) {
    if (!pResource) return E_INVALIDARG;
    if (!ppDepthStencilView) return S_FALSE;

    const uint32_t id = AllocateResourceId();
    CreateDsvPayload p = {};
    p.res_id = id;
    p.resource_id = GetResourceIdFromCOM(pResource);
    p.format = pDesc ? pDesc->Format : DXGI_FORMAT_UNKNOWN;
    p.view_dimension = pDesc ? pDesc->ViewDimension : D3D11_DSV_DIMENSION_TEXTURE2D;
    p.mip_slice = pDesc ? pDesc->Texture2D.MipSlice : 0;
    p.flags = pDesc ? pDesc->Flags : 0;

    if (!SendCommand(OP_CREATE_DSV, &p, sizeof(p), FLAG_ONEWAY)) return DXGI_ERROR_DEVICE_REMOVED;
    *ppDepthStencilView = new ProxyDepthStencilView(id, pResource, pDesc, this);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateVertexShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
                                                               ID3D11ClassLinkage*, ID3D11VertexShader** ppVertexShader) {
    if (!pShaderBytecode || BytecodeLength == 0) return E_INVALIDARG;
    if (!ppVertexShader) return S_FALSE;

    const uint32_t id = AllocateResourceId();
    CreateShaderPayload p = {id, static_cast<uint32_t>(BytecodeLength)};
    if (!SendWithBlob(this, OP_CREATE_VERTEX_SHADER, &p, sizeof(p), pShaderBytecode, BytecodeLength))
        return DXGI_ERROR_DEVICE_REMOVED;
    *ppVertexShader = new ProxyVertexShader(id, this);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreatePixelShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
                                                              ID3D11ClassLinkage*, ID3D11PixelShader** ppPixelShader) {
    if (!pShaderBytecode || BytecodeLength == 0) return E_INVALIDARG;
    if (!ppPixelShader) return S_FALSE;

    const uint32_t id = AllocateResourceId();
    CreateShaderPayload p = {id, static_cast<uint32_t>(BytecodeLength)};
    if (!SendWithBlob(this, OP_CREATE_PIXEL_SHADER, &p, sizeof(p), pShaderBytecode, BytecodeLength))
        return DXGI_ERROR_DEVICE_REMOVED;
    *ppPixelShader = new ProxyPixelShader(id, this);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* pInputElementDescs,
                                                              UINT NumElements,
                                                              const void* pShaderBytecodeWithInputSignature,
                                                              SIZE_T BytecodeLength,
                                                              ID3D11InputLayout** ppInputLayout) {
    if (!pInputElementDescs || NumElements == 0 || !pShaderBytecodeWithInputSignature || BytecodeLength == 0)
        return E_INVALIDARG;
    if (!ppInputLayout) return S_FALSE;

    const uint32_t id = AllocateResourceId();
    CreateInputLayoutPayload head = {id, NumElements, static_cast<uint32_t>(BytecodeLength)};

    std::vector<uint8_t> buf(sizeof(head) + NumElements * sizeof(RgpuInputElementDesc) + BytecodeLength);
    std::memcpy(buf.data(), &head, sizeof(head));
    auto* elems = reinterpret_cast<RgpuInputElementDesc*>(buf.data() + sizeof(head));
    for (UINT i = 0; i < NumElements; ++i) {
        RgpuInputElementDesc e = {};
        if (pInputElementDescs[i].SemanticName) {
            std::strncpy(e.semantic_name, pInputElementDescs[i].SemanticName, sizeof(e.semantic_name) - 1);
        }
        e.semantic_index = pInputElementDescs[i].SemanticIndex;
        e.format = pInputElementDescs[i].Format;
        e.input_slot = pInputElementDescs[i].InputSlot;
        e.aligned_byte_offset = pInputElementDescs[i].AlignedByteOffset;
        e.input_slot_class = pInputElementDescs[i].InputSlotClass;
        e.instance_data_step = pInputElementDescs[i].InstanceDataStepRate;
        std::memcpy(&elems[i], &e, sizeof(e));
    }
    std::memcpy(buf.data() + sizeof(head) + NumElements * sizeof(RgpuInputElementDesc),
                pShaderBytecodeWithInputSignature, BytecodeLength);

    if (!SendCommand(OP_CREATE_INPUT_LAYOUT, buf.data(), static_cast<uint32_t>(buf.size()), FLAG_ONEWAY))
        return DXGI_ERROR_DEVICE_REMOVED;
    *ppInputLayout = new ProxyInputLayout(id, this);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateSamplerState(const D3D11_SAMPLER_DESC* pSamplerDesc,
                                                               ID3D11SamplerState** ppSamplerState) {
    if (!pSamplerDesc) return E_INVALIDARG;
    if (!ppSamplerState) return S_FALSE;

    const uint32_t id = AllocateResourceId();
    CreateSamplerPayload p = {};
    p.res_id = id;
    p.filter = pSamplerDesc->Filter;
    p.address_u = pSamplerDesc->AddressU;
    p.address_v = pSamplerDesc->AddressV;
    p.address_w = pSamplerDesc->AddressW;
    p.mip_lod_bias = pSamplerDesc->MipLODBias;
    p.max_anisotropy = pSamplerDesc->MaxAnisotropy;
    p.comparison_func = pSamplerDesc->ComparisonFunc;
    std::memcpy(p.border_color, pSamplerDesc->BorderColor, sizeof(p.border_color));
    p.min_lod = pSamplerDesc->MinLOD;
    p.max_lod = pSamplerDesc->MaxLOD;

    if (!SendCommand(OP_CREATE_SAMPLER, &p, sizeof(p), FLAG_ONEWAY)) return DXGI_ERROR_DEVICE_REMOVED;
    *ppSamplerState = new ProxySamplerState(id, *pSamplerDesc, this);
    return S_OK;
}

// =============================================================================
// Pipeline-state objects (client-side only) and unsupported creation
// =============================================================================

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateBlendState(const D3D11_BLEND_DESC* pDesc, ID3D11BlendState** pp) {
    if (!pDesc) return E_INVALIDARG;
    if (!pp) return S_FALSE;
    *pp = new ProxyStateObject<ID3D11BlendState, D3D11_BLEND_DESC>(*pDesc, this);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* pDesc,
                                                                    ID3D11DepthStencilState** pp) {
    if (!pDesc) return E_INVALIDARG;
    if (!pp) return S_FALSE;
    *pp = new ProxyStateObject<ID3D11DepthStencilState, D3D11_DEPTH_STENCIL_DESC>(*pDesc, this);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateRasterizerState(const D3D11_RASTERIZER_DESC* pDesc,
                                                                  ID3D11RasterizerState** pp) {
    if (!pDesc) return E_INVALIDARG;
    if (!pp) return S_FALSE;
    *pp = new ProxyStateObject<ID3D11RasterizerState, D3D11_RASTERIZER_DESC>(*pDesc, this);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateTexture1D(const D3D11_TEXTURE1D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture1D**) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateTexture3D(const D3D11_TEXTURE3D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture3D**) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateUnorderedAccessView(ID3D11Resource*, const D3D11_UNORDERED_ACCESS_VIEW_DESC*, ID3D11UnorderedAccessView**) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateGeometryShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11GeometryShader**) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateGeometryShaderWithStreamOutput(const void*, SIZE_T, const D3D11_SO_DECLARATION_ENTRY*, UINT, const UINT*, UINT, UINT, ID3D11ClassLinkage*, ID3D11GeometryShader**) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateHullShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11HullShader**) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateDomainShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11DomainShader**) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateComputeShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateClassLinkage(ID3D11ClassLinkage**) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateQuery(const D3D11_QUERY_DESC*, ID3D11Query**) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreatePredicate(const D3D11_QUERY_DESC*, ID3D11Predicate**) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateCounter(const D3D11_COUNTER_DESC*, ID3D11Counter**) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateDeferredContext(UINT, ID3D11DeviceContext**) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::OpenSharedResource(HANDLE, REFIID, void**) { return E_NOTIMPL; }

// =============================================================================
// Feature queries & device information
// =============================================================================

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CheckFormatSupport(DXGI_FORMAT Format, UINT* pFormatSupport) {
    if (!pFormatSupport) return E_INVALIDARG;
    if (Format == DXGI_FORMAT_UNKNOWN) {
        *pFormatSupport = 0;
        return E_FAIL;
    }
    *pFormatSupport = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
                      D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_DEPTH_STENCIL |
                      D3D11_FORMAT_SUPPORT_CPU_LOCKABLE | D3D11_FORMAT_SUPPORT_DISPLAY;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CheckMultisampleQualityLevels(DXGI_FORMAT, UINT SampleCount, UINT* pNumQualityLevels) {
    if (!pNumQualityLevels) return E_INVALIDARG;
    *pNumQualityLevels = (SampleCount == 1) ? 1 : 0; // MSAA is not forwarded
    return S_OK;
}

void STDMETHODCALLTYPE ProxyD3D11Device::CheckCounterInfo(D3D11_COUNTER_INFO* pCounterInfo) {
    if (pCounterInfo) std::memset(pCounterInfo, 0, sizeof(*pCounterInfo));
}

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CheckCounter(const D3D11_COUNTER_DESC*, D3D11_COUNTER_TYPE*, UINT*, LPSTR, UINT*,
                                                         LPSTR, UINT*, LPSTR, UINT*) {
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CheckFeatureSupport(D3D11_FEATURE, void* pFeatureSupportData, UINT FeatureSupportDataSize) {
    if (!pFeatureSupportData) return E_INVALIDARG;
    std::memset(pFeatureSupportData, 0, FeatureSupportDataSize);
    return S_OK;
}

D3D_FEATURE_LEVEL STDMETHODCALLTYPE ProxyD3D11Device::GetFeatureLevel() { return m_featureLevel; }
UINT STDMETHODCALLTYPE ProxyD3D11Device::GetCreationFlags() { return 0; }
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::GetDeviceRemovedReason() { return m_deviceRemoved ? DXGI_ERROR_DEVICE_REMOVED : S_OK; }

void STDMETHODCALLTYPE ProxyD3D11Device::GetImmediateContext(ID3D11DeviceContext** ppImmediateContext) {
    if (!ppImmediateContext) return;
    *ppImmediateContext = m_immediateContext.Get();
    if (*ppImmediateContext) (*ppImmediateContext)->AddRef();
}

HRESULT STDMETHODCALLTYPE ProxyD3D11Device::SetExceptionMode(UINT) { return S_OK; }
UINT STDMETHODCALLTYPE ProxyD3D11Device::GetExceptionMode() { return 0; }

// =============================================================================
// Factory: connect, handshake, assemble device + context + swap chain
// =============================================================================

bool ProxyD3D11Device::ResizeRemoteTargets(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return false;
    if (width == m_targetWidth && height == m_targetHeight) return true;
    ResizeBuffersPayload p = {width, height};
    if (!SendCommand(OP_RESIZE_BUFFERS, &p, sizeof(p), FLAG_ONEWAY)) return false;
    m_targetWidth = width;
    m_targetHeight = height;
    return true;
}

HRESULT ProxyD3D11Device::CreateProxyDeviceAndSwapChain(const char* host, uint16_t port,
                                                        const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                        IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
                                                        D3D_FEATURE_LEVEL* pFeatureLevel,
                                                        ID3D11DeviceContext** ppImmediateContext) {
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (pSwapChainDesc) {
        desc = *pSwapChainDesc;
        ResolveSwapChainSize(desc.OutputWindow, desc.BufferDesc.Width, desc.BufferDesc.Height);
    }

    Microsoft::WRL::ComPtr<ProxyD3D11Device> device;
    HRESULT hr = Connect(host, port, pSwapChainDesc ? desc.BufferDesc.Width : 800,
                         pSwapChainDesc ? desc.BufferDesc.Height : 600, device.GetAddressOf());
    if (FAILED(hr)) return hr;
    ProxyD3D11DeviceContext* context = device->GetProxyContext();

    if (ppSwapChain && pSwapChainDesc) {
        desc.BufferDesc.Width = device->GetTargetWidth();
        desc.BufferDesc.Height = device->GetTargetHeight();
        Microsoft::WRL::ComPtr<ProxyDXGISwapChain> sc;
        sc.Attach(new ProxyDXGISwapChain(device.Get(), context, desc));
        *ppSwapChain = sc.Detach();
    }
    if (pFeatureLevel) *pFeatureLevel = device->GetFeatureLevel();
    if (ppImmediateContext) {
        *ppImmediateContext = context;
        context->AddRef();
    }
    if (ppDevice) *ppDevice = device.Detach();
    return S_OK;
}

HRESULT ProxyD3D11Device::Connect(const char* host, uint16_t port, uint32_t width, uint32_t height,
                                  ProxyD3D11Device** ppDevice) {
    if (!ppDevice) return E_INVALIDARG;
    *ppDevice = nullptr;
    if (!NetworkSocket::InitializeNetwork()) return E_FAIL;

    auto socket = std::make_shared<NetworkSocket>();
    if (!socket->Connect(host, port)) {
        std::cerr << "[RemoteGPU] Cannot connect to GPU server " << host << ":" << port << std::endl;
        return DXGI_ERROR_UNSUPPORTED;
    }
    if (width == 0 || height == 0) { width = 800; height = 600; }

    HandshakeRequestPayload req = {};
    req.protocol_version = RGPU_PROTOCOL_VERSION;
    req.client_width = width;
    req.client_height = height;
    req.preferred_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    // Frame compression is on unless RGPU_COMPRESSION=0 (useful to compare bandwidth).
    const char* compression = std::getenv("RGPU_COMPRESSION");
    req.client_flags = (compression && std::strcmp(compression, "0") == 0) ? CLIENT_FLAG_NONE
                                                                          : CLIENT_FLAG_FRAME_COMPRESSION;

    PacketHeader hdr = {};
    hdr.magic = RGPU_MAGIC;
    hdr.opcode = OP_HANDSHAKE_REQUEST;
    hdr.flags = FLAG_SYNC | FLAG_RESPONSE_REQUIRED;
    hdr.sequence_id = 1;
    hdr.payload_length = sizeof(req);
    if (!socket->SendPacket(hdr, &req)) return DXGI_ERROR_DEVICE_REMOVED;

    PacketHeader rhdr = {};
    HandshakeResponsePayload resp = {};
    if (!socket->RecvPacket(rhdr, &resp, sizeof(resp)) || rhdr.opcode != OP_HANDSHAKE_RESPONSE ||
        rhdr.payload_length != sizeof(resp) || resp.status_code != 0) {
        std::cerr << "[RemoteGPU] Handshake rejected (status=" << resp.status_code << ")" << std::endl;
        return DXGI_ERROR_UNSUPPORTED;
    }
    std::cerr << "[RemoteGPU] Connected. Remote adapter: " << resp.adapter_desc << std::endl;

    const D3D_FEATURE_LEVEL level = static_cast<D3D_FEATURE_LEVEL>(resp.feature_level);
    Microsoft::WRL::ComPtr<ProxyD3D11Device> device;
    device.Attach(new ProxyD3D11Device(socket, level));
    device->m_nextSequenceId = 2; // sequence 1 was consumed by the handshake

    device->m_targetWidth = resp.frame_width;
    device->m_targetHeight = resp.frame_height;
    resp.adapter_desc[sizeof(resp.adapter_desc) - 1] = '\0';
    device->m_adapterInfo.description = resp.adapter_desc;
    device->m_adapterInfo.dedicated_vram = resp.dedicated_vram;
    RememberRemoteAdapter(device->m_adapterInfo);

    Microsoft::WRL::ComPtr<ProxyD3D11DeviceContext> context;
    context.Attach(new ProxyD3D11DeviceContext(device.Get()));
    device->SetProxyContext(context);

    *ppDevice = device.Detach();
    return S_OK;
}

void ResolveSwapChainSize(HWND window, UINT& width, UINT& height) {
    if ((width == 0 || height == 0) && window) {
        RECT rc = {};
        if (GetClientRect(window, &rc)) {
            if (width == 0) width = static_cast<UINT>(rc.right - rc.left);
            if (height == 0) height = static_cast<UINT>(rc.bottom - rc.top);
        }
    }
    if (width == 0) width = 8;
    if (height == 0) height = 8;
}

} // namespace RemoteGPU
