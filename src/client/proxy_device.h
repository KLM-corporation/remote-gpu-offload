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
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

#include "protocol.h"
#include "transport.h"
#include "proxy_resources.h"

namespace RemoteGPU {

class ProxyD3D11DeviceContext;

// {4B6E1C85-4E7B-4A1C-9E28-87C9E0D90002}: QueryInterface on any proxy device with this IID
// returns the ProxyD3D11Device itself (as ID3D11Device*). The DXGI factory uses it to recognise
// devices it can build swap chains for.
static constexpr GUID IID_IProxyDevice = {
    0x4b6e1c85, 0x4e7b, 0x4a1c, { 0x9e, 0x28, 0x87, 0xc9, 0xe0, 0xd9, 0x00, 0x02 }
};

// A frame returned by the server, already decompressed (RGBA8, width * 4 bytes per row).
struct DecodedFrame {
    uint32_t             width = 0;
    uint32_t             height = 0;
    uint32_t             format = 0;
    uint64_t             frame_number = 0;
    std::vector<uint8_t> pixels;
};

// Remote adapter identity reported by the server at handshake.
struct RemoteAdapterInfo {
    std::string description = "RemoteGPU remote adapter";
    uint64_t    dedicated_vram = 0;
};

/**
 * @brief COM Proxy implementation of ID3D11Device & IDXGIDevice.
 * Intercepts Direct3D 11 device calls, assigns monotonic 32-bit resource IDs,
 * serializes creation opcodes over NetworkSocket, and instantiates proxy resource wrappers.
 */
class ProxyD3D11Device : public ID3D11Device, public IDXGIDevice {
public:
    explicit ProxyD3D11Device(
        std::shared_ptr<NetworkSocket> socket,
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0
    );
    virtual ~ProxyD3D11Device();

    // Non-copyable
    ProxyD3D11Device(const ProxyD3D11Device&) = delete;
    ProxyD3D11Device& operator=(const ProxyD3D11Device&) = delete;

    // --- Monotonic ID Counters & Wire Serialization ---
    uint32_t AllocateResourceId() noexcept { return ++m_nextResourceId; }
    uint32_t NextSequenceId() noexcept { return ++m_nextSequenceId; }
    void NotifyResourceDestroyed(uint32_t res_id);

    bool SendPacket(const PacketHeader& header, const void* payload = nullptr);
    bool SendCommand(uint16_t opcode, const void* payload, uint32_t payload_len, uint16_t flags = FLAG_NONE);

    std::shared_ptr<NetworkSocket> GetSocket() const noexcept { return m_socket; }
    ProxyD3D11DeviceContext* GetProxyContext() const noexcept;
    void SetProxyContext(Microsoft::WRL::ComPtr<ProxyD3D11DeviceContext> ctx);

    const RemoteAdapterInfo& GetRemoteAdapter() const noexcept { return m_adapterInfo; }
    uint32_t GetTargetWidth() const noexcept { return m_targetWidth; }
    uint32_t GetTargetHeight() const noexcept { return m_targetHeight; }
    // Makes the host back buffer width x height (OP_RESIZE_BUFFERS) if it is not already.
    bool ResizeRemoteTargets(uint32_t width, uint32_t height);

    // Frames returned by the server. A receiver thread reads and decodes OP_FRAME_DATA as soon as
    // it arrives, so the socket is always drained even while the application keeps sending
    // commands (no deadlock with several frames in flight), and decoding overlaps the application.
    bool WaitFrame(DecodedFrame& out, uint32_t timeout_ms); // false: timeout or connection lost
    bool TryPopFrame(DecodedFrame& out);                    // non-blocking

    // Connects to the server and performs the handshake; the host back buffer starts at
    // width x height. Returns a device holding one reference, with its immediate context.
    static HRESULT Connect(const char* host, uint16_t port, uint32_t width, uint32_t height, ProxyD3D11Device** ppDevice);

    // --- Static Factory Helper ---
    static HRESULT CreateProxyDeviceAndSwapChain(
        const char* host,
        uint16_t port,
        const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
        IDXGISwapChain** ppSwapChain,
        ID3D11Device** ppDevice,
        D3D_FEATURE_LEVEL* pFeatureLevel,
        ID3D11DeviceContext** ppImmediateContext
    );

    // =========================================================================
    // IUnknown Methods
    // =========================================================================
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // =========================================================================
    // IDXGIObject Methods
    // =========================================================================
    // SetPrivateData / SetPrivateDataInterface / GetPrivateData share signatures with
    // ID3D11Device and are implemented once below (single override covers both bases).
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) override;

    // =========================================================================
    // IDXGIDevice Methods
    // =========================================================================
    HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter **pAdapter) override;
    HRESULT STDMETHODCALLTYPE CreateSurface(
        const DXGI_SURFACE_DESC *pDesc,
        UINT NumSurfaces,
        DXGI_USAGE Usage,
        const DXGI_SHARED_RESOURCE *pSharedResource,
        IDXGISurface **ppSurface
    ) override;
    HRESULT STDMETHODCALLTYPE QueryResourceResidency(
        IUnknown *const *ppResources,
        DXGI_RESIDENCY *pResidencyStatus,
        UINT NumResources
    ) override;
    HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT Priority) override;
    HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT *pPriority) override;

    // =========================================================================
    // ID3D11Device Methods (40 Total)
    // =========================================================================
    // 1. Actively Serialized Resource Creation
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

    // 2. Secondary Resource Creation
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
    D3D_FEATURE_LEVEL              m_featureLevel{D3D_FEATURE_LEVEL_11_0};
    std::atomic<uint32_t>          m_nextResourceId{0};
    std::atomic<uint32_t>          m_nextSequenceId{1};
    volatile ULONG                 m_refCount{1};
    std::mutex                     m_socketMutex;
    bool                           m_deviceRemoved{false};
    RemoteAdapterInfo              m_adapterInfo;
    uint32_t                       m_targetWidth{0};
    uint32_t                       m_targetHeight{0};

    void StartFrameReceiver();
    void FrameReceiverLoop();
    std::thread                    m_receiver;
    std::mutex                     m_frameMutex;
    std::condition_variable        m_frameReady;
    std::deque<DecodedFrame>       m_frames;           // decoded, not yet presented
    bool                           m_receiverStopped{false};
    std::vector<uint8_t>           m_frameReference;   // last decoded frame (delta reference)

    Microsoft::WRL::ComPtr<ProxyD3D11DeviceContext> m_immediateContext;
};

// DXGI rule for a 0 width/height in a swap chain description: use the window's client area.
void ResolveSwapChainSize(HWND window, UINT& width, UINT& height);

// Implementations of inline ProxyResourceBase methods needing ProxyD3D11Device:
inline void ProxyResourceBase::NotifyDestroy() {
    if (m_device && m_resId != 0) {
        m_device->NotifyResourceDestroyed(m_resId);
    }
}

inline void ProxyBuffer::GetDevice(ID3D11Device **ppDevice) {
    if (ppDevice && m_device) {
        *ppDevice = m_device;
        m_device->AddRef();
    }
}

inline void ProxyTexture2D::GetDevice(ID3D11Device **ppDevice) {
    if (ppDevice && m_device) {
        *ppDevice = m_device;
        m_device->AddRef();
    }
}

inline void ProxyShaderResourceView::GetDevice(ID3D11Device **ppDevice) {
    if (ppDevice && m_device) {
        *ppDevice = m_device;
        m_device->AddRef();
    }
}

inline void ProxyRenderTargetView::GetDevice(ID3D11Device **ppDevice) {
    if (ppDevice && m_device) {
        *ppDevice = m_device;
        m_device->AddRef();
    }
}

inline void ProxyDepthStencilView::GetDevice(ID3D11Device **ppDevice) {
    if (ppDevice && m_device) {
        *ppDevice = m_device;
        m_device->AddRef();
    }
}

inline void ProxyVertexShader::GetDevice(ID3D11Device **ppDevice) {
    if (ppDevice && m_device) {
        *ppDevice = m_device;
        m_device->AddRef();
    }
}

inline void ProxyPixelShader::GetDevice(ID3D11Device **ppDevice) {
    if (ppDevice && m_device) {
        *ppDevice = m_device;
        m_device->AddRef();
    }
}

inline void ProxyInputLayout::GetDevice(ID3D11Device **ppDevice) {
    if (ppDevice && m_device) {
        *ppDevice = m_device;
        m_device->AddRef();
    }
}

inline void ProxySamplerState::GetDevice(ID3D11Device **ppDevice) {
    if (ppDevice && m_device) {
        *ppDevice = m_device;
        m_device->AddRef();
    }
}

inline void ProxyComputeShader::GetDevice(ID3D11Device **ppDevice) {
    if (ppDevice && m_device) {
        *ppDevice = m_device;
        m_device->AddRef();
    }
}

template <typename Iface>
inline void ProxyStageShader<Iface>::GetDevice(ID3D11Device **ppDevice) {
    if (ppDevice && m_device) {
        *ppDevice = m_device;
        m_device->AddRef();
    }
}

inline void ProxyUnorderedAccessView::GetDevice(ID3D11Device **ppDevice) {
    if (ppDevice && m_device) {
        *ppDevice = m_device;
        m_device->AddRef();
    }
}

} // namespace RemoteGPU

#endif // REMOTE_GPU_PROXY_DEVICE_H
