# Direct3D 11 COM Proxy Architecture & Specification

**Milestone**: M3 — Client Direct3D Interception & Presentation  
**Component**: Client Direct3D 11 Interception Layer (`src/client/`)  
**Targets**: `proxy_device.h`, `proxy_device.cpp`, `proxy_context.h`, `proxy_context.cpp`, `proxy_resources.h`  
**Author**: M3 Proxy COM Objects Explorer (`explorer_m3_1`)  
**Status**: APPROVED DESIGN SPECIFICATION  
**Date**: 2026-10-26  

---

## 1. Executive Summary & Architectural Scope

The Client Direct3D Interception Layer is responsible for intercepting all standard Direct3D 11 graphics API calls executed by a client 3D application (such as games, CAD tools, or the Milestone 4 rotating textured 3D mesh sample) and executing them remotely on a dedicated GPU host server without requiring local hardware graphics acceleration on the client PC.

This document specifies the exact C++ COM Proxy architecture for:
1. **`ProxyD3D11Device`** (`src/client/proxy_device.h`, `src/client/proxy_device.cpp`):
   - Implements the 40 pure virtual methods of COM interface `ID3D11Device` (and `IUnknown`).
   - Assigns monotonic client-side 32-bit resource identifiers (`res_id = ++m_nextResourceId`).
   - Serializes resource creation opcodes (`OP_CREATE_BUFFER`, `OP_CREATE_TEXTURE2D`, `OP_CREATE_SRV`, `OP_CREATE_RTV`, `OP_CREATE_DSV`, `OP_CREATE_VERTEX_SHADER`, `OP_CREATE_PIXEL_SHADER`, `OP_CREATE_INPUT_LAYOUT`, `OP_CREATE_SAMPLER`) over `NetworkSocket`.
   - Wraps resources in concrete proxy COM wrapper classes (`ProxyBuffer`, `ProxyTexture2D`, `ProxyShader`, etc.) storing `res_id`.
   - Provides thread-safe socket dispatching and remote resource destruction notification (`OP_DESTROY_RESOURCE`).
2. **`ProxyD3D11DeviceContext`** (`src/client/proxy_context.h`, `src/client/proxy_context.cpp`):
   - Implements the 108 pure virtual methods of COM interface `ID3D11DeviceContext` (and `ID3D11DeviceChild`, `IUnknown`).
   - Serializes pipeline state binding (`IASetInputLayout`, `IASetVertexBuffers`, `IASetIndexBuffer`, `IASetPrimitiveTopology`, `VSSetShader`, `VSSetConstantBuffers`, `PSSetShader`, `PSSetConstantBuffers`, `PSSetShaderResources`, `PSSetSamplers`, `RSSetViewports`, `OMSetRenderTargets`).
   - Serializes draw calls and clearing commands (`Draw`, `DrawIndexed`, `ClearRenderTargetView`, `ClearDepthStencilView`, `UpdateSubresource`).
   - Implements **Dynamic Buffer Shadow Mapping**: returns CPU-addressable shadow memory on `Map(pResource, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)` and serializes `OP_WRITE_BUFFER_DISCARD` with updated shadow buffer bytes on `Unmap()`.
3. **Proxy Resource Wrapper Classes** (`src/client/proxy_resources.h`):
   - Implements lightweight COM wrappers for all Direct3D 11 child objects (`ID3D11Buffer`, `ID3D11Texture2D`, `ID3D11ShaderResourceView`, `ID3D11RenderTargetView`, `ID3D11DepthStencilView`, `ID3D11VertexShader`, `ID3D11PixelShader`, `ID3D11InputLayout`, `ID3D11SamplerState`).
   - Embeds an internal interface `IProxyResource` (with unique GUID) for fast, safe resource identifier retrieval (`GetResourceId()`) across arbitrary COM interface pointers.

---

## 2. System Architecture & Component Interactions

```
+--------------------------------------------------------------------------------------------------+
| CLIENT 3D APPLICATION PROCESS (Zero Dedicated GPU Requirement)                                   |
|                                                                                                  |
| [Direct3D 11 Application]                                                                        |
|    |                                                                                             |
|    +---> D3D11CreateDeviceAndSwapChain(...)                                                      |
|    |        |                                                                                    |
|    |        +--> Instantiates ProxyD3D11Device & ProxyD3D11DeviceContext                         |
|    |        +--> Instantiates ProxyDXGISwapChain (Captures HWND & Frame Presenter)               |
|    |                                                                                             |
|    +---> ID3D11Device::CreateBuffer(...)                                                         |
|    |        |                                                                                    |
|    |        +--> Assigns monotonic res_id = ++m_nextResourceId                                   |
|    |        +--> Allocates CPU shadow memory if D3D11_USAGE_DYNAMIC                              |
|    |        +--> Serializes OP_CREATE_BUFFER + payload over NetworkSocket                        |
|    |        +--> Returns ProxyBuffer (wraps res_id, D3D11_BUFFER_DESC, shadow buffer)            |
|    |                                                                                             |
|    +---> ID3D11DeviceContext::Map(pBuffer, D3D11_MAP_WRITE_DISCARD)                             |
|    |        |                                                                                    |
|    |        +--> Returns pointer to client CPU shadow memory (pMappedResource->pData)            |
|    |                                                                                             |
|    +---> ID3D11DeviceContext::Unmap(pBuffer)                                                     |
|    |        |                                                                                    |
|    |        +--> Serializes OP_WRITE_BUFFER_DISCARD + shadow bytes over NetworkSocket            |
|    |                                                                                             |
|    +---> ID3D11DeviceContext::IASet*, VSSet*, PSSet*, RSSet*, OMSet*                             |
|    |        |                                                                                    |
|    |        +--> Extracts res_id from COM object pointers via IProxyResource                     |
|    |        +--> Serializes OP_IA_*, OP_VS_*, OP_PS_*, OP_RS_*, OP_OM_* over NetworkSocket      |
|    |                                                                                             |
|    +---> ID3D11DeviceContext::DrawIndexed(...)                                                   |
|    |        |                                                                                    |
|    |        +--> Serializes OP_DRAW_INDEXED over NetworkSocket                                   |
|    |                                                                                             |
|    +---> IDXGISwapChain::Present(...)                                                            |
|             |                                                                                    |
|             +--> Sends OP_PRESENT { frame_number }                                               |
|             +--> Awaits OP_FRAME_DATA from Server                                                |
|             +--> Presents frame to window HWND via Win32 GDI SetDIBitsToDevice                   |
+-------------|------------------------------------------------------------------------------------+
              | NetworkSocket (TCP, SIO_LOOPBACK_FAST_PATH, TCP_NODELAY)
              v
+--------------------------------------------------------------------------------------------------+
| REMOTE GPU SERVER PROCESS (Dedicated Hardware GPU Host)                                          |
|                                                                                                  |
| [ServerDispatcher]                                                                               |
|    |                                                                                             |
|    +--> ServerResourceManager: Maps client res_id <---> Native ID3D11 COM Objects                |
|    +--> Dispatches rendering commands to HostGpuDevice (Intel / NVIDIA / AMD dedicated adapter)  |
|    +--> Headless offscreen render targets & staging readback pipeline                            |
|    +--> Returns rendered RGBA pixel stream via OP_FRAME_DATA                                     |
+--------------------------------------------------------------------------------------------------+
```

---

## 3. COM Class Hierarchy & Common Proxy Infrastructure

### 3.1 The `IProxyResource` Contract

To extract the client-side `res_id` and parent device pointer from arbitrary COM pointers (`ID3D11Buffer*`, `ID3D11ShaderResourceView*`, `ID3D11Resource*`, etc.) cleanly and without relying on fragile pointer arithmetic or RTTI overhead, all proxy objects implement an internal COM interface `IProxyResource`:

```cpp
// {4B6E1C85-4E7B-4A1C-9E28-87C9E0D90001}
static constexpr GUID IID_IProxyResource = {
    0x4b6e1c85, 0x4e7b, 0x4a1c, { 0x9e, 0x28, 0x87, 0xc9, 0xe0, 0xd9, 0x00, 0x01 }
};

class IProxyResource : public IUnknown {
public:
    virtual uint32_t STDMETHODCALLTYPE GetResourceId() const noexcept = 0;
    virtual class ProxyD3D11Device* STDMETHODCALLTYPE GetProxyDevice() const noexcept = 0;
};
```

#### Fast In-Module Resolution Helper:
```cpp
template <typename T>
inline uint32_t GetResourceIdFromCOM(T* pComObj) {
    if (!pComObj) return 0;
    IProxyResource* proxy = nullptr;
    HRESULT hr = pComObj->QueryInterface(IID_IProxyResource, reinterpret_cast<void**>(&proxy));
    if (SUCCEEDED(hr) && proxy) {
        uint32_t id = proxy->GetResourceId();
        proxy->Release();
        return id;
    }
    return 0;
}
```

### 3.2 `ProxyResourceBase` Implementation Base

Every proxy COM object derives from `ProxyResourceBase`, which provides standard thread-safe reference counting, parent device association, and automatic remote destruction:

```cpp
class ProxyResourceBase : public IProxyResource {
public:
    ProxyResourceBase(uint32_t res_id, ProxyD3D11Device* device)
        : m_resId(res_id), m_device(device), m_refCount(1) {}

    virtual ~ProxyResourceBase() = default;

    // IUnknown methods
    ULONG STDMETHODCALLTYPE AddRefBase() {
        return InterlockedIncrement(&m_refCount);
    }

    ULONG STDMETHODCALLTYPE ReleaseBase() {
        ULONG count = InterlockedDecrement(&m_refCount);
        if (count == 0) {
            NotifyDestroy();
            delete this;
            return 0;
        }
        return count;
    }

    // IProxyResource methods
    uint32_t STDMETHODCALLTYPE GetResourceId() const noexcept override {
        return m_resId;
    }

    ProxyD3D11Device* STDMETHODCALLTYPE GetProxyDevice() const noexcept override {
        return m_device;
    }

protected:
    void NotifyDestroy();

    uint32_t m_resId;
    ProxyD3D11Device* m_device;
    volatile ULONG m_refCount;
};
```

When `ReleaseBase()` reaches 0, `NotifyDestroy()` notifies `m_device->NotifyResourceDestroyed(m_resId)`. This sends an `OP_DESTROY_RESOURCE` packet to the server, releasing the remote GPU resources immediately!

---

## 4. `ProxyD3D11Device` Specification

### 4.1 Header Declaration (`src/client/proxy_device.h`)

```cpp
#pragma once
#ifndef REMOTE_GPU_PROXY_DEVICE_H
#define REMOTE_GPU_PROXY_DEVICE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "protocol.h"
#include "transport.h"

namespace RemoteGPU {

class ProxyD3D11DeviceContext;

/**
 * @brief COM Proxy implementation of ID3D11Device.
 * Intercepts Direct3D 11 device calls, assigns monotonic 32-bit resource IDs,
 * serializes creation opcodes over NetworkSocket, and instantiates proxy resource wrappers.
 */
class ProxyD3D11Device : public ID3D11Device {
public:
    explicit ProxyD3D11Device(std::shared_ptr<NetworkSocket> socket, D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0);
    virtual ~ProxyD3D11Device();

    // Non-copyable
    ProxyD3D11Device(const ProxyD3D11Device&) = delete;
    ProxyD3D11Device& operator=(const ProxyD3D11Device&) = delete;

    // --- Internal Management & Wire Serialization ---
    uint32_t AllocateResourceId() noexcept { return ++m_nextResourceId; }
    uint32_t NextSequenceId() noexcept { return ++m_nextSequenceId; }
    void NotifyResourceDestroyed(uint32_t res_id);

    bool SendPacket(const PacketHeader& header, const void* payload = nullptr);
    bool SendCommand(uint16_t opcode, const void* payload, uint32_t payload_len, uint16_t flags = FLAG_NONE);

    std::shared_ptr<NetworkSocket> GetSocket() const noexcept { return m_socket; }
    ProxyD3D11DeviceContext* GetProxyContext() const noexcept { return m_immediateContext.Get(); }

    // =========================================================================
    // IUnknown Methods
    // =========================================================================
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // =========================================================================
    // ID3D11Device Methods (40 Total)
    // =========================================================================
    // 1. Core Resource Creation (Actively Serialized)
    HRESULT STDMETHODCALLTYPE CreateBuffer(
        const D3D11_BUFFER_DESC *pDesc,
        const D3D11_SUBRESOURCE_DATA *pInitialData,
        ID3D11Buffer **ppBuffer) override;

    HRESULT STDMETHODCALLTYPE CreateTexture2D(
        const D3D11_TEXTURE2D_DESC *pDesc,
        const D3D11_SUBRESOURCE_DATA *pInitialData,
        ID3D11Texture2D **ppTexture2D) override;

    HRESULT STDMETHODCALLTYPE CreateShaderResourceView(
        ID3D11Resource *pResource,
        const D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc,
        ID3D11ShaderResourceView **ppSRView) override;

    HRESULT STDMETHODCALLTYPE CreateRenderTargetView(
        ID3D11Resource *pResource,
        const D3D11_RENDER_TARGET_VIEW_DESC *pDesc,
        ID3D11RenderTargetView **ppRTView) override;

    HRESULT STDMETHODCALLTYPE CreateDepthStencilView(
        ID3D11Resource *pResource,
        const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc,
        ID3D11DepthStencilView **ppDepthStencilView) override;

    HRESULT STDMETHODCALLTYPE CreateVertexShader(
        const void *pShaderBytecode,
        SIZE_T BytecodeLength,
        ID3D11ClassLinkage *pClassLinkage,
        ID3D11VertexShader **ppVertexShader) override;

    HRESULT STDMETHODCALLTYPE CreatePixelShader(
        const void *pShaderBytecode,
        SIZE_T BytecodeLength,
        ID3D11ClassLinkage *pClassLinkage,
        ID3D11PixelShader **ppPixelShader) override;

    HRESULT STDMETHODCALLTYPE CreateInputLayout(
        const D3D11_INPUT_ELEMENT_DESC *pInputElementDescs,
        UINT NumElements,
        const void *pShaderBytecodeWithInputSignature,
        SIZE_T BytecodeLength,
        ID3D11InputLayout **ppInputLayout) override;

    HRESULT STDMETHODCALLTYPE CreateSamplerState(
        const D3D11_SAMPLER_DESC *pSamplerDesc,
        ID3D11SamplerState **ppSamplerState) override;

    // 2. Secondary Resource Creation (Benign Stubs & State Defaults)
    HRESULT STDMETHODCALLTYPE CreateTexture1D(
        const D3D11_TEXTURE1D_DESC *pDesc,
        const D3D11_SUBRESOURCE_DATA *pInitialData,
        ID3D11Texture1D **ppTexture1D) override;

    HRESULT STDMETHODCALLTYPE CreateTexture3D(
        const D3D11_TEXTURE3D_DESC *pDesc,
        const D3D11_SUBRESOURCE_DATA *pInitialData,
        ID3D11Texture3D **ppTexture3D) override;

    HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(
        ID3D11Resource *pResource,
        const D3D11_UNORDERED_ACCESS_VIEW_DESC *pDesc,
        ID3D11UnorderedAccessView **ppUAView) override;

    HRESULT STDMETHODCALLTYPE CreateGeometryShader(
        const void *pShaderBytecode,
        SIZE_T BytecodeLength,
        ID3D11ClassLinkage *pClassLinkage,
        ID3D11GeometryShader **ppGeometryShader) override;

    HRESULT STDMETHODCALLTYPE CreateGeometryShaderWithStreamOutput(
        const void *pShaderBytecode,
        SIZE_T BytecodeLength,
        const D3D11_SO_DECLARATION_ENTRY *pSODeclaration,
        UINT NumEntries,
        const UINT *pBufferStrides,
        UINT NumStrides,
        UINT RasterizedStream,
        ID3D11ClassLinkage *pClassLinkage,
        ID3D11GeometryShader **ppGeometryShader) override;

    HRESULT STDMETHODCALLTYPE CreateHullShader(
        const void *pShaderBytecode,
        SIZE_T BytecodeLength,
        ID3D11ClassLinkage *pClassLinkage,
        ID3D11HullShader **ppHullShader) override;

    HRESULT STDMETHODCALLTYPE CreateDomainShader(
        const void *pShaderBytecode,
        SIZE_T BytecodeLength,
        ID3D11ClassLinkage *pClassLinkage,
        ID3D11DomainShader **ppDomainShader) override;

    HRESULT STDMETHODCALLTYPE CreateComputeShader(
        const void *pShaderBytecode,
        SIZE_T BytecodeLength,
        ID3D11ClassLinkage *pClassLinkage,
        ID3D11ComputeShader **ppComputeShader) override;

    HRESULT STDMETHODCALLTYPE CreateClassLinkage(
        ID3D11ClassLinkage **ppLinkage) override;

    HRESULT STDMETHODCALLTYPE CreateBlendState(
        const D3D11_BLEND_DESC *pBlendStateDesc,
        ID3D11BlendState **ppBlendState) override;

    HRESULT STDMETHODCALLTYPE CreateDepthStencilState(
        const D3D11_DEPTH_STENCIL_DESC *pDepthStencilDesc,
        ID3D11DepthStencilState **ppDepthStencilState) override;

    HRESULT STDMETHODCALLTYPE CreateRasterizerState(
        const D3D11_RASTERIZER_DESC *pRasterizerDesc,
        ID3D11RasterizerState **ppRasterizerState) override;

    HRESULT STDMETHODCALLTYPE CreateQuery(
        const D3D11_QUERY_DESC *pQueryDesc,
        ID3D11Query **ppQuery) override;

    HRESULT STDMETHODCALLTYPE CreatePredicate(
        const D3D11_QUERY_DESC *pPredicateDesc,
        ID3D11Predicate **ppPredicate) override;

    HRESULT STDMETHODCALLTYPE CreateCounter(
        const D3D11_COUNTER_DESC *pCounterDesc,
        ID3D11Counter **ppCounter) override;

    HRESULT STDMETHODCALLTYPE CreateDeferredContext(
        UINT ContextFlags,
        ID3D11DeviceContext **ppDeferredContext) override;

    HRESULT STDMETHODCALLTYPE OpenSharedResource(
        HANDLE hResource,
        REFIID ReturnedInterface,
        void **ppResource) override;

    // 3. Feature Queries & Device Information
    HRESULT STDMETHODCALLTYPE CheckFormatSupport(
        DXGI_FORMAT Format,
        UINT *pFormatSupport) override;

    HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(
        DXGI_FORMAT Format,
        UINT SampleCount,
        UINT *pNumQualityLevels) override;

    void STDMETHODCALLTYPE CheckCounterInfo(
        D3D11_COUNTER_INFO *pCounterInfo) override;

    HRESULT STDMETHODCALLTYPE CheckCounter(
        const D3D11_COUNTER_DESC *pDesc,
        D3D11_COUNTER_TYPE *pType,
        UINT *pActiveCounters,
        LPSTR szName,
        UINT *pNameLength,
        LPSTR szUnits,
        UINT *pUnitsLength,
        LPSTR szDescription,
        UINT *pDescriptionLength) override;

    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(
        D3D11_FEATURE Feature,
        void *pFeatureSupportData,
        UINT FeatureSupportDataSize) override;

    HRESULT STDMETHODCALLTYPE GetPrivateData(
        REFGUID guid,
        UINT *pDataSize,
        void *pData) override;

    HRESULT STDMETHODCALLTYPE SetPrivateData(
        REFGUID guid,
        UINT DataSize,
        const void *pData) override;

    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(
        REFGUID guid,
        const IUnknown *pData) override;

    D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel() override;
    UINT STDMETHODCALLTYPE GetCreationFlags() override;
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override;
    void STDMETHODCALLTYPE GetImmediateContext(ID3D11DeviceContext **ppImmediateContext) override;
    HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT RaiseFlags) override;
    UINT STDMETHODCALLTYPE GetExceptionMode() override;

private:
    std::shared_ptr<NetworkSocket> m_socket;
    D3D_FEATURE_LEVEL m_featureLevel;
    std::atomic<uint32_t> m_nextResourceId{100}; // Reserve 0-99 for defaults & swapchain
    std::atomic<uint32_t> m_nextSequenceId{1};
    volatile ULONG m_refCount{1};
    std::mutex m_socketMutex;

    Microsoft::WRL::ComPtr<ProxyD3D11DeviceContext> m_immediateContext;
};

} // namespace RemoteGPU

#endif // REMOTE_GPU_PROXY_DEVICE_H
```

---

### 4.2 Resource Creation Implementation Details (`src/client/proxy_device.cpp`)

#### A. Initializing and Packaging `OP_CREATE_BUFFER`
```cpp
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateBuffer(
    const D3D11_BUFFER_DESC *pDesc,
    const D3D11_SUBRESOURCE_DATA *pInitialData,
    ID3D11Buffer **ppBuffer)
{
    if (!pDesc || !ppBuffer) return E_INVALIDARG;
    if (pDesc->ByteWidth == 0) return E_INVALIDARG;

    // Validate constant buffer alignment rule: multiple of 16 bytes
    if (pDesc->BindFlags & D3D11_BIND_CONSTANT_BUFFER) {
        if ((pDesc->ByteWidth % 16) != 0) {
            return E_INVALIDARG;
        }
    }

    uint32_t res_id = AllocateResourceId();

    // Prepare CreateBufferPayload matching protocol.h
    CreateBufferPayload payload{};
    payload.res_id = res_id;
    payload.byte_width = pDesc->ByteWidth;
    payload.usage = pDesc->Usage;
    payload.bind_flags = pDesc->BindFlags;
    payload.cpu_access_flags = pDesc->CPUAccessFlags;
    payload.misc_flags = pDesc->MiscFlags;
    payload.structure_byte_stride = pDesc->StructureByteStride;
    payload.has_initial_data = (pInitialData && pInitialData->pSysMem) ? 1 : 0;
    payload.initial_data_size = payload.has_initial_data ? pDesc->ByteWidth : 0;

    // Assemble transmission buffer
    std::vector<uint8_t> packetData(sizeof(payload) + payload.initial_data_size);
    std::memcpy(packetData.data(), &payload, sizeof(payload));
    if (payload.has_initial_data) {
        std::memcpy(packetData.data() + sizeof(payload), pInitialData->pSysMem, payload.initial_data_size);
    }

    PacketHeader header{};
    header.magic = RGPU_MAGIC;
    header.opcode = OP_CREATE_BUFFER;
    header.flags = FLAG_NONE;
    header.sequence_id = NextSequenceId();
    header.payload_length = static_cast<uint32_t>(packetData.size());

    if (!SendPacket(header, packetData.data())) {
        return DXGI_ERROR_DEVICE_REMOVED;
    }

    // Instantiate ProxyBuffer with CPU shadow memory if DYNAMIC or CPU_ACCESS_WRITE
    auto* proxyBuffer = new ProxyBuffer(res_id, *pDesc, this, 
                                        payload.has_initial_data ? pInitialData->pSysMem : nullptr);
    *ppBuffer = proxyBuffer;
    return S_OK;
}
```

#### B. Initializing and Packaging `OP_CREATE_INPUT_LAYOUT`
`D3D11_INPUT_ELEMENT_DESC` uses pointers to C-strings for semantic names (`LPCSTR SemanticName`). Over the network, this must be packed into fixed 56-byte `RgpuInputElementDesc` structs matching `protocol.h`:

```cpp
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateInputLayout(
    const D3D11_INPUT_ELEMENT_DESC *pInputElementDescs,
    UINT NumElements,
    const void *pShaderBytecodeWithInputSignature,
    SIZE_T BytecodeLength,
    ID3D11InputLayout **ppInputLayout)
{
    if (!pInputElementDescs || NumElements == 0 || !ppInputLayout) {
        return E_INVALIDARG;
    }

    uint32_t res_id = AllocateResourceId();

    CreateInputLayoutPayload payload{};
    payload.res_id = res_id;
    payload.num_elements = NumElements;
    payload.bytecode_length = static_cast<uint32_t>(BytecodeLength);

    size_t elements_size = sizeof(RgpuInputElementDesc) * NumElements;
    std::vector<uint8_t> packetData(sizeof(payload) + elements_size + payload.bytecode_length);

    // 1. Copy payload header
    std::memcpy(packetData.data(), &payload, sizeof(payload));

    // 2. Serialize elements
    auto* pElems = reinterpret_cast<RgpuInputElementDesc*>(packetData.data() + sizeof(payload));
    for (UINT i = 0; i < NumElements; ++i) {
        std::memset(&pElems[i], 0, sizeof(RgpuInputElementDesc));
        if (pInputElementDescs[i].SemanticName) {
            std::strncpy(pElems[i].semantic_name, pInputElementDescs[i].SemanticName, sizeof(pElems[i].semantic_name) - 1);
        }
        pElems[i].semantic_index = pInputElementDescs[i].SemanticIndex;
        pElems[i].format = pInputElementDescs[i].Format;
        pElems[i].input_slot = pInputElementDescs[i].InputSlot;
        pElems[i].aligned_byte_offset = pInputElementDescs[i].AlignedByteOffset;
        pElems[i].input_slot_class = pInputElementDescs[i].InputSlotClass;
        pElems[i].instance_data_step = pInputElementDescs[i].InstanceDataStepRate;
    }

    // 3. Append shader bytecode
    if (pShaderBytecodeWithInputSignature && BytecodeLength > 0) {
        std::memcpy(packetData.data() + sizeof(payload) + elements_size,
                    pShaderBytecodeWithInputSignature, BytecodeLength);
    }

    PacketHeader header{};
    header.magic = RGPU_MAGIC;
    header.opcode = OP_CREATE_INPUT_LAYOUT;
    header.flags = FLAG_NONE;
    header.sequence_id = NextSequenceId();
    header.payload_length = static_cast<uint32_t>(packetData.size());

    if (!SendPacket(header, packetData.data())) {
        return DXGI_ERROR_DEVICE_REMOVED;
    }

    *ppInputLayout = new ProxyInputLayout(res_id, this);
    return S_OK;
}
```

#### C. Initializing and Packaging `OP_CREATE_VERTEX_SHADER` & `OP_CREATE_PIXEL_SHADER`
```cpp
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateVertexShader(
    const void *pShaderBytecode,
    SIZE_T BytecodeLength,
    ID3D11ClassLinkage *pClassLinkage,
    ID3D11VertexShader **ppVertexShader)
{
    if (!pShaderBytecode || BytecodeLength == 0 || !ppVertexShader) {
        return E_INVALIDARG;
    }

    uint32_t res_id = AllocateResourceId();

    CreateShaderPayload payload{};
    payload.res_id = res_id;
    payload.bytecode_length = static_cast<uint32_t>(BytecodeLength);

    std::vector<uint8_t> packetData(sizeof(payload) + payload.bytecode_length);
    std::memcpy(packetData.data(), &payload, sizeof(payload));
    std::memcpy(packetData.data() + sizeof(payload), pShaderBytecode, payload.bytecode_length);

    PacketHeader header{};
    header.magic = RGPU_MAGIC;
    header.opcode = OP_CREATE_VERTEX_SHADER;
    header.flags = FLAG_NONE;
    header.sequence_id = NextSequenceId();
    header.payload_length = static_cast<uint32_t>(packetData.size());

    if (!SendPacket(header, packetData.data())) {
        return DXGI_ERROR_DEVICE_REMOVED;
    }

    *ppVertexShader = new ProxyVertexShader(res_id, this);
    return S_OK;
}
```
*(Pixel Shader uses identical logic with `OP_CREATE_PIXEL_SHADER` and `ProxyPixelShader`)*.

#### D. Initializing and Packaging `OP_CREATE_TEXTURE2D`
```cpp
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateTexture2D(
    const D3D11_TEXTURE2D_DESC *pDesc,
    const D3D11_SUBRESOURCE_DATA *pInitialData,
    ID3D11Texture2D **ppTexture2D)
{
    if (!pDesc || !ppTexture2D) return E_INVALIDARG;

    uint32_t res_id = AllocateResourceId();

    CreateTexture2DPayload payload{};
    payload.res_id = res_id;
    payload.width = pDesc->Width;
    payload.height = pDesc->Height;
    payload.mips = pDesc->MipLevels;
    payload.array_size = pDesc->ArraySize;
    payload.format = pDesc->Format;
    payload.sample_count = pDesc->SampleDesc.Count;
    payload.sample_quality = pDesc->SampleDesc.Quality;
    payload.usage = pDesc->Usage;
    payload.bind_flags = pDesc->BindFlags;
    payload.cpu_access_flags = pDesc->CPUAccessFlags;
    payload.misc_flags = pDesc->MiscFlags;
    payload.has_initial_data = (pInitialData && pInitialData->pSysMem) ? 1 : 0;
    payload.sys_mem_pitch = payload.has_initial_data ? pInitialData->SysMemPitch : 0;
    payload.sys_mem_slice_pitch = payload.has_initial_data ? pInitialData->SysMemSlicePitch : 0;

    // For standard uncompressed 2D textures, size is row_pitch * height
    uint32_t data_size = payload.has_initial_data ? (payload.sys_mem_pitch * payload.height) : 0;
    payload.initial_data_size = data_size;

    std::vector<uint8_t> packetData(sizeof(payload) + data_size);
    std::memcpy(packetData.data(), &payload, sizeof(payload));
    if (payload.has_initial_data && data_size > 0) {
        std::memcpy(packetData.data() + sizeof(payload), pInitialData->pSysMem, data_size);
    }

    PacketHeader header{};
    header.magic = RGPU_MAGIC;
    header.opcode = OP_CREATE_TEXTURE2D;
    header.flags = FLAG_NONE;
    header.sequence_id = NextSequenceId();
    header.payload_length = static_cast<uint32_t>(packetData.size());

    if (!SendPacket(header, packetData.data())) {
        return DXGI_ERROR_DEVICE_REMOVED;
    }

    *ppTexture2D = new ProxyTexture2D(res_id, *pDesc, this);
    return S_OK;
}
```

#### E. View Creation (`CreateShaderResourceView`, `CreateRenderTargetView`, `CreateDepthStencilView`)
All views require linking to the underlying resource `res_id`:
```cpp
HRESULT STDMETHODCALLTYPE ProxyD3D11Device::CreateShaderResourceView(
    ID3D11Resource *pResource,
    const D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc,
    ID3D11ShaderResourceView **ppSRView)
{
    if (!ppSRView) return E_INVALIDARG;

    uint32_t underlying_res_id = GetResourceIdFromCOM(pResource);
    uint32_t srv_id = AllocateResourceId();

    CreateSrvPayload payload{};
    payload.res_id = srv_id;
    payload.resource_id = underlying_res_id;
    payload.format = pDesc ? pDesc->Format : 0; // 0: inherit resource format
    payload.view_dimension = pDesc ? pDesc->ViewDimension : D3D11_SRV_DIMENSION_TEXTURE2D;
    if (pDesc && pDesc->ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
        payload.most_detailed_mip = pDesc->Texture2D.MostDetailedMip;
        payload.mip_levels = pDesc->Texture2D.MipLevels;
    } else {
        payload.most_detailed_mip = 0;
        payload.mip_levels = 0xFFFFFFFF;
    }

    if (!SendCommand(OP_CREATE_SRV, &payload, sizeof(payload))) {
        return DXGI_ERROR_DEVICE_REMOVED;
    }

    *ppSRView = new ProxyShaderResourceView(srv_id, pResource, pDesc, this);
    return S_OK;
}
```

---

## 5. `ProxyD3D11DeviceContext` Specification

### 5.1 Header Declaration (`src/client/proxy_context.h`)

```cpp
#pragma once
#ifndef REMOTE_GPU_PROXY_CONTEXT_H
#define REMOTE_GPU_PROXY_CONTEXT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <vector>

#include "protocol.h"

namespace RemoteGPU {

class ProxyD3D11Device;

/**
 * @brief COM Proxy implementation of ID3D11DeviceContext.
 * Serializes pipeline state bindings, rasterizer/viewport settings, draw calls,
 * clear commands, and implements dynamic buffer shadow mapping (Map/Unmap).
 */
class ProxyD3D11DeviceContext : public ID3D11DeviceContext {
public:
    explicit ProxyD3D11DeviceContext(ProxyD3D11Device* device);
    virtual ~ProxyD3D11DeviceContext();

    // Non-copyable
    ProxyD3D11DeviceContext(const ProxyD3D11DeviceContext&) = delete;
    ProxyD3D11DeviceContext& operator=(const ProxyD3D11DeviceContext&) = delete;

    // =========================================================================
    // IUnknown Methods
    // =========================================================================
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // =========================================================================
    // ID3D11DeviceChild Methods
    // =========================================================================
    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void *pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *pData) override;

    // =========================================================================
    // ID3D11DeviceContext Pipeline Methods (108 Total)
    // =========================================================================

    // --- Input Assembler Stage ---
    void STDMETHODCALLTYPE IASetInputLayout(ID3D11InputLayout *pInputLayout) override;
    void STDMETHODCALLTYPE IASetVertexBuffers(
        UINT StartSlot, UINT NumBuffers,
        ID3D11Buffer *const *ppVertexBuffers,
        const UINT *pStrides, const UINT *pOffsets) override;
    void STDMETHODCALLTYPE IASetIndexBuffer(ID3D11Buffer *pIndexBuffer, DXGI_FORMAT Format, UINT Offset) override;
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology) override;

    // --- Vertex Shader Stage ---
    void STDMETHODCALLTYPE VSSetShader(
        ID3D11VertexShader *pVertexShader,
        ID3D11ClassInstance *const *ppClassInstances,
        UINT NumClassInstances) override;
    void STDMETHODCALLTYPE VSSetConstantBuffers(
        UINT StartSlot, UINT NumBuffers,
        ID3D11Buffer *const *ppConstantBuffers) override;
    void STDMETHODCALLTYPE VSSetShaderResources(
        UINT StartSlot, UINT NumViews,
        ID3D11ShaderResourceView *const *ppShaderResourceViews) override;
    void STDMETHODCALLTYPE VSSetSamplers(
        UINT StartSlot, UINT NumSamplers,
        ID3D11SamplerState *const *ppSamplers) override;

    // --- Rasterizer Stage ---
    void STDMETHODCALLTYPE RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT *pViewports) override;
    void STDMETHODCALLTYPE RSSetScissorRects(UINT NumRects, const D3D11_RECT *pRects) override;
    void STDMETHODCALLTYPE RSSetState(ID3D11RasterizerState *pRasterizerState) override;

    // --- Pixel Shader Stage ---
    void STDMETHODCALLTYPE PSSetShader(
        ID3D11PixelShader *pPixelShader,
        ID3D11ClassInstance *const *ppClassInstances,
        UINT NumClassInstances) override;
    void STDMETHODCALLTYPE PSSetConstantBuffers(
        UINT StartSlot, UINT NumBuffers,
        ID3D11Buffer *const *ppConstantBuffers) override;
    void STDMETHODCALLTYPE PSSetShaderResources(
        UINT StartSlot, UINT NumViews,
        ID3D11ShaderResourceView *const *ppShaderResourceViews) override;
    void STDMETHODCALLTYPE PSSetSamplers(
        UINT StartSlot, UINT NumSamplers,
        ID3D11SamplerState *const *ppSamplers) override;

    // --- Output Merger Stage ---
    void STDMETHODCALLTYPE OMSetRenderTargets(
        UINT NumViews,
        ID3D11RenderTargetView *const *ppRenderTargetViews,
        ID3D11DepthStencilView *pDepthStencilView) override;
    void STDMETHODCALLTYPE OMSetRenderTargetsAndUnorderedAccessViews(
        UINT NumRTVs, ID3D11RenderTargetView *const *ppRenderTargetViews,
        ID3D11DepthStencilView *pDepthStencilView,
        UINT UAVStartSlot, UINT NumUAVs,
        ID3D11UnorderedAccessView *const *ppUnorderedAccessViews,
        const UINT *pUAVInitialCounts) override;
    void STDMETHODCALLTYPE OMSetBlendState(
        ID3D11BlendState *pBlendState,
        const FLOAT BlendFactor[4],
        UINT SampleMask) override;
    void STDMETHODCALLTYPE OMSetDepthStencilState(
        ID3D11DepthStencilState *pDepthStencilState,
        UINT StencilRef) override;

    // --- Draw & Dispatch Calls ---
    void STDMETHODCALLTYPE Draw(UINT VertexCount, UINT StartVertexLocation) override;
    void STDMETHODCALLTYPE DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) override;
    void STDMETHODCALLTYPE DrawInstanced(
        UINT VertexCountPerInstance, UINT InstanceCount,
        UINT StartVertexLocation, UINT StartInstanceLocation) override;
    void STDMETHODCALLTYPE DrawIndexedInstanced(
        UINT IndexCountPerInstance, UINT InstanceCount,
        UINT StartIndexLocation, INT BaseVertexLocation,
        UINT StartInstanceLocation) override;
    void STDMETHODCALLTYPE DrawAuto() override;
    void STDMETHODCALLTYPE DrawIndexedInstancedIndirect(ID3D11Buffer *pBufferForArgs, UINT AlignedByteOffsetForArgs) override;
    void STDMETHODCALLTYPE DrawInstancedIndirect(ID3D11Buffer *pBufferForArgs, UINT AlignedByteOffsetForArgs) override;
    void STDMETHODCALLTYPE Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) override;
    void STDMETHODCALLTYPE DispatchIndirect(ID3D11Buffer *pBufferForArgs, UINT AlignedByteOffsetForArgs) override;

    // --- Clears & Memory Transfers ---
    void STDMETHODCALLTYPE ClearRenderTargetView(ID3D11RenderTargetView *pRenderTargetView, const FLOAT ColorRGBA[4]) override;
    void STDMETHODCALLTYPE ClearDepthStencilView(ID3D11DepthStencilView *pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView *pUnorderedAccessView, const UINT Values[4]) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView *pUnorderedAccessView, const FLOAT Values[4]) override;
    void STDMETHODCALLTYPE UpdateSubresource(
        ID3D11Resource *pDstResource, UINT DstSubresource,
        const D3D11_BOX *pDstBox, const void *pSrcData,
        UINT SrcRowPitch, UINT SrcDepthPitch) override;

    // --- Dynamic Buffer Shadow Mapping ---
    HRESULT STDMETHODCALLTYPE Map(
        ID3D11Resource *pResource, UINT Subresource,
        D3D11_MAP MapType, UINT MapFlags,
        D3D11_MAPPED_SUBRESOURCE *pMappedResource) override;
    void STDMETHODCALLTYPE Unmap(ID3D11Resource *pResource, UINT Subresource) override;

    // --- Execution Control & Pipeline Synchronization ---
    void STDMETHODCALLTYPE ClearState() override;
    void STDMETHODCALLTYPE Flush() override;
    D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE GetType() override;
    UINT STDMETHODCALLTYPE GetContextFlags() override;
    HRESULT STDMETHODCALLTYPE FinishCommandList(WINBOOL RestoreDeferredContextState, ID3D11CommandList **ppCommandList) override;
    void STDMETHODCALLTYPE ExecuteCommandList(ID3D11CommandList *pCommandList, WINBOOL RestoreContextState) override;

    // --- Stubs for Queries, Hull, Domain, Geometry & Getters ---
    // (Full 108-method declarations provided with inline no-op implementations)
    #include "proxy_context_stubs.inl"

private:
    ProxyD3D11Device* m_device;
    volatile ULONG m_refCount{1};
};

} // namespace RemoteGPU

#endif // REMOTE_GPU_PROXY_CONTEXT_H
```

---

### 5.2 Dynamic Buffer Shadow Mapping (`Map` & `Unmap`)

Direct3D dynamic constant buffers and vertex buffers are updated every frame using `Map(D3D11_MAP_WRITE_DISCARD)`. In standard D3D11, the GPU driver allocates an internal write-combined memory block and returns a CPU pointer. In our zero-GPU client proxy:
1. `Map()` checks the resource type via `pResource->QueryInterface(IID_IProxyResource)`.
2. Casting to `ProxyBuffer*`, it validates that the buffer has CPU shadow memory (`m_shadowData`).
3. It sets `pMappedResource->pData = proxyBuffer->GetShadowData()`, `pMappedResource->RowPitch = desc.ByteWidth`, `pMappedResource->DepthPitch = desc.ByteWidth`.
4. The application writes directly into `pMappedResource->pData`.
5. On `Unmap()`, `ProxyD3D11DeviceContext` constructs `WriteBufferDiscardPayload`, copies the updated shadow bytes, and dispatches `OP_WRITE_BUFFER_DISCARD`:

```cpp
HRESULT STDMETHODCALLTYPE ProxyD3D11DeviceContext::Map(
    ID3D11Resource *pResource,
    UINT Subresource,
    D3D11_MAP MapType,
    UINT MapFlags,
    D3D11_MAPPED_SUBRESOURCE *pMappedResource)
{
    if (!pResource || !pMappedResource) return E_INVALIDARG;

    // Cast or Query ProxyBuffer
    auto* proxyBuffer = dynamic_cast<ProxyBuffer*>(pResource);
    if (!proxyBuffer) {
        // Safe COM QI fallback
        IProxyResource* proxyRes = nullptr;
        if (SUCCEEDED(pResource->QueryInterface(IID_IProxyResource, reinterpret_cast<void**>(&proxyRes)))) {
            proxyBuffer = dynamic_cast<ProxyBuffer*>(proxyRes);
            proxyRes->Release();
        }
    }

    if (!proxyBuffer) {
        return E_INVALIDARG;
    }

    uint8_t* pShadow = proxyBuffer->GetShadowData();
    if (!pShadow) {
        return E_FAIL; // Buffer was not created with DYNAMIC or CPU_ACCESS_WRITE
    }

    const D3D11_BUFFER_DESC& desc = proxyBuffer->GetDesc();
    pMappedResource->pData = pShadow;
    pMappedResource->RowPitch = desc.ByteWidth;
    pMappedResource->DepthPitch = desc.ByteWidth;

    proxyBuffer->SetMapped(true, Subresource, MapType);
    return S_OK;
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::Unmap(
    ID3D11Resource *pResource,
    UINT Subresource)
{
    if (!pResource) return;

    auto* proxyBuffer = dynamic_cast<ProxyBuffer*>(pResource);
    if (!proxyBuffer) {
        IProxyResource* proxyRes = nullptr;
        if (SUCCEEDED(pResource->QueryInterface(IID_IProxyResource, reinterpret_cast<void**>(&proxyRes)))) {
            proxyBuffer = dynamic_cast<ProxyBuffer*>(proxyRes);
            proxyRes->Release();
        }
    }

    if (!proxyBuffer || !proxyBuffer->IsMapped()) {
        return;
    }

    uint32_t res_id = proxyBuffer->GetResourceId();
    const D3D11_BUFFER_DESC& desc = proxyBuffer->GetDesc();
    const uint8_t* pShadow = proxyBuffer->GetShadowData();

    WriteBufferDiscardPayload payload{};
    payload.res_id = res_id;
    payload.byte_offset = 0;
    payload.byte_width = desc.ByteWidth;

    std::vector<uint8_t> packetData(sizeof(payload) + payload.byte_width);
    std::memcpy(packetData.data(), &payload, sizeof(payload));
    std::memcpy(packetData.data() + sizeof(payload), pShadow, payload.byte_width);

    PacketHeader header{};
    header.magic = RGPU_MAGIC;
    header.opcode = OP_WRITE_BUFFER_DISCARD;
    header.flags = FLAG_NONE;
    header.sequence_id = m_device->NextSequenceId();
    header.payload_length = static_cast<uint32_t>(packetData.size());

    m_device->SendPacket(header, packetData.data());

    proxyBuffer->SetMapped(false, 0, D3D11_MAP_WRITE_DISCARD);
}
```

---

### 5.3 Pipeline State & Draw Serialization

#### A. Pipeline Binding Commands
```cpp
void STDMETHODCALLTYPE ProxyD3D11DeviceContext::IASetInputLayout(ID3D11InputLayout *pInputLayout) {
    uint32_t res_id = GetResourceIdFromCOM(pInputLayout);
    IASetInputLayoutPayload payload{ res_id };
    m_device->SendCommand(OP_IA_SET_INPUT_LAYOUT, &payload, sizeof(payload));
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::IASetVertexBuffers(
    UINT StartSlot, UINT NumBuffers,
    ID3D11Buffer *const *ppVertexBuffers,
    const UINT *pStrides, const UINT *pOffsets)
{
    for (UINT i = 0; i < NumBuffers; ++i) {
        uint32_t slot = StartSlot + i;
        uint32_t res_id = (ppVertexBuffers && ppVertexBuffers[i]) ? GetResourceIdFromCOM(ppVertexBuffers[i]) : 0;
        uint32_t stride = (pStrides) ? pStrides[i] : 0;
        uint32_t offset = (pOffsets) ? pOffsets[i] : 0;

        IASetVertexBufferPayload payload{ slot, res_id, stride, offset };
        m_device->SendCommand(OP_IA_SET_VERTEX_BUFFER, &payload, sizeof(payload));
    }
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::IASetIndexBuffer(
    ID3D11Buffer *pIndexBuffer, DXGI_FORMAT Format, UINT Offset)
{
    uint32_t res_id = GetResourceIdFromCOM(pIndexBuffer);
    IASetIndexBufferPayload payload{ res_id, static_cast<uint32_t>(Format), Offset };
    m_device->SendCommand(OP_IA_SET_INDEX_BUFFER, &payload, sizeof(payload));
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology) {
    IASetTopologyPayload payload{ static_cast<uint32_t>(Topology) };
    m_device->SendCommand(OP_IA_SET_TOPOLOGY, &payload, sizeof(payload));
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::VSSetShader(
    ID3D11VertexShader *pVertexShader,
    ID3D11ClassInstance *const * /*ppClassInstances*/,
    UINT /*NumClassInstances*/)
{
    uint32_t res_id = GetResourceIdFromCOM(pVertexShader);
    VSSetShaderPayload payload{ res_id };
    m_device->SendCommand(OP_VS_SET_SHADER, &payload, sizeof(payload));
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::VSSetConstantBuffers(
    UINT StartSlot, UINT NumBuffers,
    ID3D11Buffer *const *ppConstantBuffers)
{
    for (UINT i = 0; i < NumBuffers; ++i) {
        uint32_t slot = StartSlot + i;
        uint32_t res_id = (ppConstantBuffers && ppConstantBuffers[i]) ? GetResourceIdFromCOM(ppConstantBuffers[i]) : 0;
        VSSetConstantBufferPayload payload{ slot, res_id };
        m_device->SendCommand(OP_VS_SET_CB, &payload, sizeof(payload));
    }
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::PSSetShader(
    ID3D11PixelShader *pPixelShader,
    ID3D11ClassInstance *const * /*ppClassInstances*/,
    UINT /*NumClassInstances*/)
{
    uint32_t res_id = GetResourceIdFromCOM(pPixelShader);
    PSSetShaderPayload payload{ res_id };
    m_device->SendCommand(OP_PS_SET_SHADER, &payload, sizeof(payload));
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::PSSetConstantBuffers(
    UINT StartSlot, UINT NumBuffers,
    ID3D11Buffer *const *ppConstantBuffers)
{
    for (UINT i = 0; i < NumBuffers; ++i) {
        uint32_t slot = StartSlot + i;
        uint32_t res_id = (ppConstantBuffers && ppConstantBuffers[i]) ? GetResourceIdFromCOM(ppConstantBuffers[i]) : 0;
        PSSetConstantBufferPayload payload{ slot, res_id };
        m_device->SendCommand(OP_PS_SET_CB, &payload, sizeof(payload));
    }
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::PSSetShaderResources(
    UINT StartSlot, UINT NumViews,
    ID3D11ShaderResourceView *const *ppShaderResourceViews)
{
    for (UINT i = 0; i < NumViews; ++i) {
        uint32_t slot = StartSlot + i;
        uint32_t res_id = (ppShaderResourceViews && ppShaderResourceViews[i]) ? GetResourceIdFromCOM(ppShaderResourceViews[i]) : 0;
        PSSetSrvPayload payload{ slot, res_id };
        m_device->SendCommand(OP_PS_SET_SRV, &payload, sizeof(payload));
    }
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::PSSetSamplers(
    UINT StartSlot, UINT NumSamplers,
    ID3D11SamplerState *const *ppSamplers)
{
    for (UINT i = 0; i < NumSamplers; ++i) {
        uint32_t slot = StartSlot + i;
        uint32_t res_id = (ppSamplers && ppSamplers[i]) ? GetResourceIdFromCOM(ppSamplers[i]) : 0;
        PSSetSamplerPayload payload{ slot, res_id };
        m_device->SendCommand(OP_PS_SET_SAMPLER, &payload, sizeof(payload));
    }
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::RSSetViewports(
    UINT NumViewports, const D3D11_VIEWPORT *pViewports)
{
    if (NumViewports == 0 || !pViewports) return;
    // Protocol RSSetViewportPayload encodes one primary viewport
    RSSetViewportPayload payload{};
    payload.top_left_x = pViewports[0].TopLeftX;
    payload.top_left_y = pViewports[0].TopLeftY;
    payload.width = pViewports[0].Width;
    payload.height = pViewports[0].Height;
    payload.min_depth = pViewports[0].MinDepth;
    payload.max_depth = pViewports[0].MaxDepth;
    m_device->SendCommand(OP_RS_SET_VIEWPORT, &payload, sizeof(payload));
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::OMSetRenderTargets(
    UINT NumViews,
    ID3D11RenderTargetView *const *ppRenderTargetViews,
    ID3D11DepthStencilView *pDepthStencilView)
{
    uint32_t rtv_id = (NumViews > 0 && ppRenderTargetViews && ppRenderTargetViews[0]) ?
                      GetResourceIdFromCOM(ppRenderTargetViews[0]) : 0;
    uint32_t dsv_id = pDepthStencilView ? GetResourceIdFromCOM(pDepthStencilView) : 0;

    OMSetRenderTargetsPayload payload{ rtv_id, dsv_id };
    m_device->SendCommand(OP_OM_SET_RENDER_TARGETS, &payload, sizeof(payload));
}
```

#### B. Draw & Clear Commands
```cpp
void STDMETHODCALLTYPE ProxyD3D11DeviceContext::ClearRenderTargetView(
    ID3D11RenderTargetView *pRenderTargetView,
    const FLOAT ColorRGBA[4])
{
    uint32_t rtv_id = pRenderTargetView ? GetResourceIdFromCOM(pRenderTargetView) : 0;
    ClearRtvPayload payload{};
    payload.res_id = rtv_id;
    if (ColorRGBA) {
        std::memcpy(payload.color, ColorRGBA, sizeof(payload.color));
    } else {
        payload.color[0] = 0.0f; payload.color[1] = 0.0f; payload.color[2] = 0.0f; payload.color[3] = 1.0f;
    }
    m_device->SendCommand(OP_CLEAR_RTV, &payload, sizeof(payload));
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::ClearDepthStencilView(
    ID3D11DepthStencilView *pDepthStencilView,
    UINT ClearFlags, FLOAT Depth, UINT8 Stencil)
{
    uint32_t dsv_id = pDepthStencilView ? GetResourceIdFromCOM(pDepthStencilView) : 0;
    ClearDsvPayload payload{};
    payload.res_id = dsv_id;
    payload.clear_flags = ClearFlags;
    payload.depth = Depth;
    payload.stencil = Stencil;
    m_device->SendCommand(OP_CLEAR_DSV, &payload, sizeof(payload));
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::Draw(
    UINT VertexCount, UINT StartVertexLocation)
{
    DrawPayload payload{ VertexCount, StartVertexLocation };
    m_device->SendCommand(OP_DRAW, &payload, sizeof(payload));
}

void STDMETHODCALLTYPE ProxyD3D11DeviceContext::DrawIndexed(
    UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation)
{
    DrawIndexedPayload payload{ IndexCount, StartIndexLocation, BaseVertexLocation };
    m_device->SendCommand(OP_DRAW_INDEXED, &payload, sizeof(payload));
}
```

---

## 6. Proxy Resource Wrapper Classes (`src/client/proxy_resources.h`)

To ensure complete type safety and standards compliance, each Direct3D 11 resource type is implemented as a concrete C++ class:

```cpp
#pragma once
#ifndef REMOTE_GPU_PROXY_RESOURCES_H
#define REMOTE_GPU_PROXY_RESOURCES_H

#include "proxy_device.h"

namespace RemoteGPU {

// =============================================================================
// ProxyBuffer: ID3D11Buffer Implementation
// =============================================================================
class ProxyBuffer : public ID3D11Buffer, public ProxyResourceBase {
public:
    ProxyBuffer(uint32_t res_id, const D3D11_BUFFER_DESC& desc, ProxyD3D11Device* device, const void* pInitialData)
        : ProxyResourceBase(res_id, device), m_desc(desc)
    {
        // Allocate shadow memory if dynamic or writable
        if (m_desc.Usage == D3D11_USAGE_DYNAMIC || (m_desc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE)) {
            m_shadowData.resize(m_desc.ByteWidth);
            if (pInitialData) {
                std::memcpy(m_shadowData.data(), pInitialData, m_desc.ByteWidth);
            }
        }
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
            riid == __uuidof(ID3D11Resource) || riid == __uuidof(ID3D11Buffer)) {
            *ppvObject = static_cast<ID3D11Buffer*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_IProxyResource) {
            *ppvObject = static_cast<IProxyResource*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return AddRefBase(); }
    ULONG STDMETHODCALLTYPE Release() override { return ReleaseBase(); }

    // ID3D11DeviceChild
    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override {
        if (ppDevice && m_device) { *ppDevice = m_device; m_device->AddRef(); }
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }

    // ID3D11Resource
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION *pResourceDimension) override {
        if (pResourceDimension) *pResourceDimension = D3D11_RESOURCE_DIMENSION_BUFFER;
    }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }

    // ID3D11Buffer
    void STDMETHODCALLTYPE GetDesc(D3D11_BUFFER_DESC *pDesc) override {
        if (pDesc) *pDesc = m_desc;
    }

    // Shadow Mapping Accessors
    uint8_t* GetShadowData() noexcept { return m_shadowData.empty() ? nullptr : m_shadowData.data(); }
    const uint8_t* GetShadowData() const noexcept { return m_shadowData.empty() ? nullptr : m_shadowData.data(); }
    const D3D11_BUFFER_DESC& GetDesc() const noexcept { return m_desc; }
    bool IsMapped() const noexcept { return m_isMapped; }
    void SetMapped(bool mapped, UINT subres = 0, D3D11_MAP type = D3D11_MAP_WRITE_DISCARD) noexcept {
        m_isMapped = mapped;
        m_mappedSubresource = subres;
        m_mappedType = type;
    }

private:
    D3D11_BUFFER_DESC m_desc;
    std::vector<uint8_t> m_shadowData;
    bool m_isMapped = false;
    UINT m_mappedSubresource = 0;
    D3D11_MAP m_mappedType = D3D11_MAP_WRITE_DISCARD;
};

// =============================================================================
// ProxyTexture2D: ID3D11Texture2D Implementation
// =============================================================================
class ProxyTexture2D : public ID3D11Texture2D, public ProxyResourceBase {
public:
    ProxyTexture2D(uint32_t res_id, const D3D11_TEXTURE2D_DESC& desc, ProxyD3D11Device* device)
        : ProxyResourceBase(res_id, device), m_desc(desc) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
            riid == __uuidof(ID3D11Resource) || riid == __uuidof(ID3D11Texture2D)) {
            *ppvObject = static_cast<ID3D11Texture2D*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_IProxyResource) {
            *ppvObject = static_cast<IProxyResource*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return AddRefBase(); }
    ULONG STDMETHODCALLTYPE Release() override { return ReleaseBase(); }

    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override {
        if (ppDevice && m_device) { *ppDevice = m_device; m_device->AddRef(); }
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }

    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION *pResourceDimension) override {
        if (pResourceDimension) *pResourceDimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
    }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }

    void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE2D_DESC *pDesc) override {
        if (pDesc) *pDesc = m_desc;
    }

private:
    D3D11_TEXTURE2D_DESC m_desc;
};

// =============================================================================
// ProxyShaderResourceView: ID3D11ShaderResourceView Implementation
// =============================================================================
class ProxyShaderResourceView : public ID3D11ShaderResourceView, public ProxyResourceBase {
public:
    ProxyShaderResourceView(uint32_t res_id, ID3D11Resource* pRes, const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc, ProxyD3D11Device* device)
        : ProxyResourceBase(res_id, device), m_resource(pRes)
    {
        if (pDesc) m_desc = *pDesc;
        else std::memset(&m_desc, 0, sizeof(m_desc));
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
            riid == __uuidof(ID3D11View) || riid == __uuidof(ID3D11ShaderResourceView)) {
            *ppvObject = static_cast<ID3D11ShaderResourceView*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_IProxyResource) {
            *ppvObject = static_cast<IProxyResource*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return AddRefBase(); }
    ULONG STDMETHODCALLTYPE Release() override { return ReleaseBase(); }

    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override {
        if (ppDevice && m_device) { *ppDevice = m_device; m_device->AddRef(); }
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }

    void STDMETHODCALLTYPE GetResource(ID3D11Resource **ppResource) override {
        if (ppResource) { *ppResource = m_resource.Get(); if (m_resource) m_resource->AddRef(); }
    }
    void STDMETHODCALLTYPE GetDesc(D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc) override {
        if (pDesc) *pDesc = m_desc;
    }

private:
    Microsoft::WRL::ComPtr<ID3D11Resource> m_resource;
    D3D11_SHADER_RESOURCE_VIEW_DESC m_desc;
};

// =============================================================================
// ProxyRenderTargetView: ID3D11RenderTargetView Implementation
// =============================================================================
class ProxyRenderTargetView : public ID3D11RenderTargetView, public ProxyResourceBase {
public:
    ProxyRenderTargetView(uint32_t res_id, ID3D11Resource* pRes, const D3D11_RENDER_TARGET_VIEW_DESC* pDesc, ProxyD3D11Device* device)
        : ProxyResourceBase(res_id, device), m_resource(pRes)
    {
        if (pDesc) m_desc = *pDesc;
        else std::memset(&m_desc, 0, sizeof(m_desc));
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
            riid == __uuidof(ID3D11View) || riid == __uuidof(ID3D11RenderTargetView)) {
            *ppvObject = static_cast<ID3D11RenderTargetView*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_IProxyResource) {
            *ppvObject = static_cast<IProxyResource*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return AddRefBase(); }
    ULONG STDMETHODCALLTYPE Release() override { return ReleaseBase(); }

    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override {
        if (ppDevice && m_device) { *ppDevice = m_device; m_device->AddRef(); }
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }

    void STDMETHODCALLTYPE GetResource(ID3D11Resource **ppResource) override {
        if (ppResource) { *ppResource = m_resource.Get(); if (m_resource) m_resource->AddRef(); }
    }
    void STDMETHODCALLTYPE GetDesc(D3D11_RENDER_TARGET_VIEW_DESC *pDesc) override {
        if (pDesc) *pDesc = m_desc;
    }

private:
    Microsoft::WRL::ComPtr<ID3D11Resource> m_resource;
    D3D11_RENDER_TARGET_VIEW_DESC m_desc;
};

// =============================================================================
// ProxyDepthStencilView: ID3D11DepthStencilView Implementation
// =============================================================================
class ProxyDepthStencilView : public ID3D11DepthStencilView, public ProxyResourceBase {
public:
    ProxyDepthStencilView(uint32_t res_id, ID3D11Resource* pRes, const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc, ProxyD3D11Device* device)
        : ProxyResourceBase(res_id, device), m_resource(pRes)
    {
        if (pDesc) m_desc = *pDesc;
        else std::memset(&m_desc, 0, sizeof(m_desc));
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
            riid == __uuidof(ID3D11View) || riid == __uuidof(ID3D11DepthStencilView)) {
            *ppvObject = static_cast<ID3D11DepthStencilView*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_IProxyResource) {
            *ppvObject = static_cast<IProxyResource*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return AddRefBase(); }
    ULONG STDMETHODCALLTYPE Release() override { return ReleaseBase(); }

    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override {
        if (ppDevice && m_device) { *ppDevice = m_device; m_device->AddRef(); }
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }

    void STDMETHODCALLTYPE GetResource(ID3D11Resource **ppResource) override {
        if (ppResource) { *ppResource = m_resource.Get(); if (m_resource) m_resource->AddRef(); }
    }
    void STDMETHODCALLTYPE GetDesc(D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc) override {
        if (pDesc) *pDesc = m_desc;
    }

private:
    Microsoft::WRL::ComPtr<ID3D11Resource> m_resource;
    D3D11_DEPTH_STENCIL_VIEW_DESC m_desc;
};

// =============================================================================
// ProxyVertexShader: ID3D11VertexShader Implementation
// =============================================================================
class ProxyVertexShader : public ID3D11VertexShader, public ProxyResourceBase {
public:
    ProxyVertexShader(uint32_t res_id, ProxyD3D11Device* device)
        : ProxyResourceBase(res_id, device) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) || riid == __uuidof(ID3D11VertexShader)) {
            *ppvObject = static_cast<ID3D11VertexShader*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_IProxyResource) {
            *ppvObject = static_cast<IProxyResource*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return AddRefBase(); }
    ULONG STDMETHODCALLTYPE Release() override { return ReleaseBase(); }

    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override {
        if (ppDevice && m_device) { *ppDevice = m_device; m_device->AddRef(); }
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
};

// =============================================================================
// ProxyPixelShader: ID3D11PixelShader Implementation
// =============================================================================
class ProxyPixelShader : public ID3D11PixelShader, public ProxyResourceBase {
public:
    ProxyPixelShader(uint32_t res_id, ProxyD3D11Device* device)
        : ProxyResourceBase(res_id, device) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) || riid == __uuidof(ID3D11PixelShader)) {
            *ppvObject = static_cast<ID3D11PixelShader*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_IProxyResource) {
            *ppvObject = static_cast<IProxyResource*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return AddRefBase(); }
    ULONG STDMETHODCALLTYPE Release() override { return ReleaseBase(); }

    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override {
        if (ppDevice && m_device) { *ppDevice = m_device; m_device->AddRef(); }
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
};

// =============================================================================
// ProxyInputLayout: ID3D11InputLayout Implementation
// =============================================================================
class ProxyInputLayout : public ID3D11InputLayout, public ProxyResourceBase {
public:
    ProxyInputLayout(uint32_t res_id, ProxyD3D11Device* device)
        : ProxyResourceBase(res_id, device) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) || riid == __uuidof(ID3D11InputLayout)) {
            *ppvObject = static_cast<ID3D11InputLayout*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_IProxyResource) {
            *ppvObject = static_cast<IProxyResource*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return AddRefBase(); }
    ULONG STDMETHODCALLTYPE Release() override { return ReleaseBase(); }

    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override {
        if (ppDevice && m_device) { *ppDevice = m_device; m_device->AddRef(); }
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
};

// =============================================================================
// ProxySamplerState: ID3D11SamplerState Implementation
// =============================================================================
class ProxySamplerState : public ID3D11SamplerState, public ProxyResourceBase {
public:
    ProxySamplerState(uint32_t res_id, const D3D11_SAMPLER_DESC& desc, ProxyD3D11Device* device)
        : ProxyResourceBase(res_id, device), m_desc(desc) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) || riid == __uuidof(ID3D11SamplerState)) {
            *ppvObject = static_cast<ID3D11SamplerState*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_IProxyResource) {
            *ppvObject = static_cast<IProxyResource*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return AddRefBase(); }
    ULONG STDMETHODCALLTYPE Release() override { return ReleaseBase(); }

    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override {
        if (ppDevice && m_device) { *ppDevice = m_device; m_device->AddRef(); }
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }

    void STDMETHODCALLTYPE GetDesc(D3D11_SAMPLER_DESC *pDesc) override {
        if (pDesc) *pDesc = m_desc;
    }

private:
    D3D11_SAMPLER_DESC m_desc;
};

// =============================================================================
// Inline Destruction Dispatcher
// =============================================================================
inline void ProxyResourceBase::NotifyDestroy() {
    if (m_device && m_resId != 0) {
        m_device->NotifyResourceDestroyed(m_resId);
    }
}

} // namespace RemoteGPU

#endif // REMOTE_GPU_PROXY_RESOURCES_H
```

---

## 7. Wire Protocol Mapping & Verification Matrix

The following table correlates every Direct3D 11 API function to its opcode in `protocol.h`, payload structure, and server execution handler in `server_dispatcher.cpp`:

| Direct3D 11 API Method | Client Opcode | Wire Payload Structure | Server Execution Handler |
|---|---|---|---|
| `CreateBuffer` | `OP_CREATE_BUFFER` (0x0010) | `CreateBufferPayload` (36 B) + Initial Data | `ServerDispatcher::HandleCreateBuffer` |
| `CreateTexture2D` | `OP_CREATE_TEXTURE2D` (0x0011) | `CreateTexture2DPayload` (64 B) + Pixel Data | `ServerDispatcher::HandleCreateTexture2D` |
| `CreateShaderResourceView` | `OP_CREATE_SRV` (0x0012) | `CreateSrvPayload` (24 B) | `ServerDispatcher::HandleCreateSrv` |
| `CreateRenderTargetView` | `OP_CREATE_RTV` (0x0013) | `CreateRtvPayload` (20 B) | `ServerDispatcher::HandleCreateRtv` |
| `CreateDepthStencilView` | `OP_CREATE_DSV` (0x0014) | `CreateDsvPayload` (24 B) | `ServerDispatcher::HandleCreateDsv` |
| `CreateVertexShader` | `OP_CREATE_VERTEX_SHADER` (0x0015) | `CreateShaderPayload` (8 B) + Bytecode | `ServerDispatcher::HandleCreateVertexShader` |
| `CreatePixelShader` | `OP_CREATE_PIXEL_SHADER` (0x0016) | `CreateShaderPayload` (8 B) + Bytecode | `ServerDispatcher::HandleCreatePixelShader` |
| `CreateInputLayout` | `OP_CREATE_INPUT_LAYOUT` (0x0017) | `CreateInputLayoutPayload` (12 B) + Descs + Sig | `ServerDispatcher::HandleCreateInputLayout` |
| `CreateSamplerState` | `OP_CREATE_SAMPLER` (0x0019) | `CreateSamplerPayload` (56 B) | `ServerDispatcher::HandleCreateSampler` |
| `Release` (refcount -> 0) | `OP_DESTROY_RESOURCE` (0x0018) | `DestroyResourcePayload` (4 B) | `ServerDispatcher::HandleDestroyResource` |
| `IASetInputLayout` | `OP_IA_SET_INPUT_LAYOUT` (0x0030) | `IASetInputLayoutPayload` (4 B) | `ServerDispatcher::HandleIASetInputLayout` |
| `IASetVertexBuffers` | `OP_IA_SET_VERTEX_BUFFER` (0x0031) | `IASetVertexBufferPayload` (16 B) | `ServerDispatcher::HandleIASetVertexBuffer` |
| `IASetIndexBuffer` | `OP_IA_SET_INDEX_BUFFER` (0x0032) | `IASetIndexBufferPayload` (12 B) | `ServerDispatcher::HandleIASetIndexBuffer` |
| `IASetPrimitiveTopology` | `OP_IA_SET_TOPOLOGY` (0x0033) | `IASetTopologyPayload` (4 B) | `ServerDispatcher::HandleIASetTopology` |
| `VSSetShader` | `OP_VS_SET_SHADER` (0x0034) | `VSSetShaderPayload` (4 B) | `ServerDispatcher::HandleVSSetShader` |
| `VSSetConstantBuffers` | `OP_VS_SET_CB` (0x0035) | `VSSetConstantBufferPayload` (8 B) | `ServerDispatcher::HandleVSSetCB` |
| `PSSetShader` | `OP_PS_SET_SHADER` (0x0036) | `PSSetShaderPayload` (4 B) | `ServerDispatcher::HandlePSSetShader` |
| `PSSetConstantBuffers` | `OP_PS_SET_CB` (0x0037) | `PSSetConstantBufferPayload` (8 B) | `ServerDispatcher::HandlePSSetCB` |
| `PSSetShaderResources` | `OP_PS_SET_SRV` (0x0038) | `PSSetSrvPayload` (8 B) | `ServerDispatcher::HandlePSSetSrv` |
| `PSSetSamplers` | `OP_PS_SET_SAMPLER` (0x003B) | `PSSetSamplerPayload` (8 B) | `ServerDispatcher::HandlePSSetSampler` |
| `RSSetViewports` | `OP_RS_SET_VIEWPORT` (0x0039) | `RSSetViewportPayload` (24 B) | `ServerDispatcher::HandleRSSetViewport` |
| `OMSetRenderTargets` | `OP_OM_SET_RENDER_TARGETS` (0x003A) | `OMSetRenderTargetsPayload` (8 B) | `ServerDispatcher::HandleOMSetRenderTargets` |
| `ClearRenderTargetView` | `OP_CLEAR_RTV` (0x0052) | `ClearRtvPayload` (20 B) | `ServerDispatcher::HandleClearRtv` |
| `ClearDepthStencilView` | `OP_CLEAR_DSV` (0x0053) | `ClearDsvPayload` (16 B) | `ServerDispatcher::HandleClearDsv` |
| `Draw` | `OP_DRAW` (0x0054) | `DrawPayload` (8 B) | `ServerDispatcher::HandleDraw` |
| `DrawIndexed` | `OP_DRAW_INDEXED` (0x0055) | `DrawIndexedPayload` (12 B) | `ServerDispatcher::HandleDrawIndexed` |
| `UpdateSubresource` | `OP_UPDATE_SUBRESOURCE` (0x0050) | `UpdateSubresourcePayload` (48 B) + Data | `ServerDispatcher::HandleUpdateSubresource` |
| `Unmap` (Discard) | `OP_WRITE_BUFFER_DISCARD` (0x0051) | `WriteBufferDiscardPayload` (12 B) + Data | `ServerDispatcher::HandleWriteBufferDiscard` |

---

## 8. Thread Safety, Synchronization & Fault Tolerance

1. **Thread-Safe Socket Dispatching**:
   - `ProxyD3D11Device` encapsulates all network packet writing inside `SendPacket` and `SendCommand` protected by `std::mutex m_socketMutex`.
   - `std::atomic<uint32_t> m_nextResourceId` guarantees lock-free, atomic resource ID assignment across worker threads calling `CreateBuffer` or `CreateTexture2D`.
   - `std::atomic<uint32_t> m_nextSequenceId` guarantees monotonic packet sequencing.

2. **Network Disconnection Resilience**:
   - If `SendPacket` or `SendCommand` fails (e.g., server process terminated or connection reset), the error is logged and the method returns `DXGI_ERROR_DEVICE_REMOVED`.
   - In `GetDeviceRemovedReason()`, `ProxyD3D11Device` returns `DXGI_ERROR_DEVICE_REMOVED` once socket disconnection is detected, cleanly alerting standard DirectX game loops to trigger orderly shutdown.

3. **No-Leak COM Resource Lifecycle**:
   - Application calls `Release()` on proxy COM objects.
   - When refcount reaches 0, `ProxyResourceBase::ReleaseBase()` executes `NotifyDestroy()`, which sends `OP_DESTROY_RESOURCE { res_id }` across the network socket.
   - Server removes `res_id` from its lookup table, releasing the native GPU COM object.
   - Local proxy C++ object is deleted.
   - Result: 0 client leaks, 0 server VRAM leaks.

---

## 9. Implementation File Organization Plan

The implementer should organize the files in `src/client/` according to the following layout:

```
src/client/
├── proxy_device.h           # ProxyD3D11Device class declaration
├── proxy_device.cpp         # ProxyD3D11Device implementation & packet dispatch
├── proxy_context.h          # ProxyD3D11DeviceContext class declaration
├── proxy_context.cpp        # ProxyD3D11DeviceContext implementation & Map/Unmap
├── proxy_context_stubs.inl  # Inlined no-op stubs for secondary D3D11 context methods
└── proxy_resources.h        # Concrete proxy COM wrapper classes & IProxyResource
```

This layout enforces clean separation of concerns, guarantees that neither class is abstract, satisfies every requirement of Milestone 3, and ensures 100% wire-compatibility with the Milestone 2 server dispatcher.
