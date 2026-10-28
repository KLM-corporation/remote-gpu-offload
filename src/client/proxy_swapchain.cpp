#include "proxy_swapchain.h"
#include "proxy_context.h"
#include "frame_codec.h"
#include "bmp_writer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace RemoteGPU {

ProxyDXGISwapChain::ProxyDXGISwapChain(ProxyD3D11Device* device, ProxyD3D11DeviceContext* context,
                                       const DXGI_SWAP_CHAIN_DESC& desc)
    : m_device(device), m_context(context), m_desc(desc) {
    m_device->AddRef();
    m_context->AddRef();
    // RGPU_FRAME_LATENCY: frames allowed in flight (1 = wait for every frame, as before).
    if (const char* latency = std::getenv("RGPU_FRAME_LATENCY")) {
        const long value = std::strtol(latency, nullptr, 10);
        if (value >= 1 && value <= 8) m_maxFramesInFlight = static_cast<uint32_t>(value);
    }
    CreateBackBuffer();
    m_presenter.SetForceBgraConversion(true);
    m_presenter.Initialize(desc.OutputWindow, desc.BufferDesc.Width, desc.BufferDesc.Height);
}

void ProxyDXGISwapChain::CreateBackBuffer() {
    if (m_backBuffer) m_backBuffer->Release();
    D3D11_TEXTURE2D_DESC tex = {};
    tex.Width = m_desc.BufferDesc.Width;
    tex.Height = m_desc.BufferDesc.Height;
    tex.MipLevels = 1;
    tex.ArraySize = 1;
    tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex.SampleDesc.Count = 1;
    tex.Usage = D3D11_USAGE_DEFAULT;
    tex.BindFlags = D3D11_BIND_RENDER_TARGET;
    m_backBuffer = new ProxyTexture2D(0, tex, m_device); // res_id 0 == host offscreen target
}

ProxyDXGISwapChain::~ProxyDXGISwapChain() {
    DrainFrames(); // the frames still in flight, so the capture is the last presented frame
    if (const char* path = std::getenv("RGPU_CAPTURE_FILE")) {
        if (!m_lastFrame.empty() && WriteBmp(path, m_lastFrame.data(), m_desc.BufferDesc.Width, m_desc.BufferDesc.Height)) {
            std::cerr << "[RemoteGPU] Last frame written to " << path << std::endl;
        }
    }
    m_presenter.Shutdown();
    if (m_backBuffer) m_backBuffer->Release();
    m_context->Release();
    m_device->Release();
}

// =============================================================================
// IUnknown / IDXGIObject / IDXGIDeviceSubObject
// =============================================================================

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
        riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1)) {
        *ppvObject = static_cast<IDXGISwapChain1*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE ProxyDXGISwapChain::AddRef() { return InterlockedIncrement(&m_refCount); }

ULONG STDMETHODCALLTYPE ProxyDXGISwapChain::Release() {
    ULONG count = InterlockedDecrement(&m_refCount);
    if (count == 0) delete this;
    return count;
}

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::SetPrivateData(REFGUID, UINT, const void*) { return S_OK; }
HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::SetPrivateDataInterface(REFGUID, const IUnknown*) { return S_OK; }
HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::GetPrivateData(REFGUID, UINT*, void*) { return DXGI_ERROR_NOT_FOUND; }

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::GetParent(REFIID, void** ppParent) {
    if (ppParent) *ppParent = nullptr;
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::GetDevice(REFIID riid, void** ppDevice) {
    return m_device->QueryInterface(riid, ppDevice);
}

// =============================================================================
// IDXGISwapChain
// =============================================================================

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::Present(UINT SyncInterval, UINT Flags) {
    if (Flags & DXGI_PRESENT_TEST) return S_OK;

    PresentPayload p = {};
    p.swapchain_id = 0;
    p.sync_interval = SyncInterval;
    p.flags = Flags;
    p.frame_number = ++m_frameNumber;
    if (!m_device->SendCommand(OP_PRESENT, &p, sizeof(p), FLAG_SYNC | FLAG_RESPONSE_REQUIRED | FLAG_END_OF_FRAME))
        return DXGI_ERROR_DEVICE_REMOVED;
    ++m_framesInFlight;

    // Show whatever already came back, then block only while too many frames are in flight:
    // the application renders frame N+1 while the server still works on frame N.
    DecodedFrame frame;
    while (m_device->TryPopFrame(frame)) ShowFrame(frame);
    while (m_framesInFlight >= m_maxFramesInFlight) {
        if (!m_device->WaitFrame(frame, kFrameTimeoutMs)) {
            std::cerr << "[RemoteGPU] Connection lost while waiting for frame " << (m_frameNumber - m_framesInFlight + 1) << std::endl;
            m_framesInFlight = 0;
            return DXGI_ERROR_DEVICE_REMOVED;
        }
        ShowFrame(frame);
    }
    return S_OK;
}

void ProxyDXGISwapChain::ShowFrame(DecodedFrame& frame) {
    if (m_framesInFlight > 0) --m_framesInFlight;
    m_lastFrame.swap(frame.pixels);
    m_presenter.Present(m_desc.OutputWindow, m_lastFrame.data(), frame.width, frame.height, frame.format);
}

void ProxyDXGISwapChain::DrainFrames() {
    DecodedFrame frame;
    while (m_framesInFlight > 0) {
        if (!m_device->WaitFrame(frame, kFrameTimeoutMs)) {
            m_framesInFlight = 0;
            return;
        }
        ShowFrame(frame);
    }
}

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) {
    if (!ppSurface) return E_INVALIDARG;
    *ppSurface = nullptr;
    if (Buffer != 0) return DXGI_ERROR_INVALID_CALL;
    return m_backBuffer->QueryInterface(riid, ppSurface);
}

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::SetFullscreenState(WINBOOL Fullscreen, IDXGIOutput*) {
    return Fullscreen ? DXGI_ERROR_NOT_CURRENTLY_AVAILABLE : S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::GetFullscreenState(WINBOOL* pFullscreen, IDXGIOutput** ppTarget) {
    if (pFullscreen) *pFullscreen = FALSE;
    if (ppTarget) *ppTarget = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) {
    if (!pDesc) return E_INVALIDARG;
    *pDesc = m_desc;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height,
                                                            DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    ResolveSwapChainSize(m_desc.OutputWindow, Width, Height);
    DrainFrames(); // frames in flight still have the old size
    if (!m_device->ResizeRemoteTargets(Width, Height)) return DXGI_ERROR_DEVICE_REMOVED;
    if (BufferCount) m_desc.BufferCount = BufferCount;
    if (NewFormat != DXGI_FORMAT_UNKNOWN) m_desc.BufferDesc.Format = NewFormat;
    m_desc.BufferDesc.Width = Width;
    m_desc.BufferDesc.Height = Height;
    m_desc.Flags = SwapChainFlags;
    CreateBackBuffer(); // views on the old back buffer must be recreated by the application
    m_lastFrame.clear();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::ResizeTarget(const DXGI_MODE_DESC*) { return S_OK; }

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::GetContainingOutput(IDXGIOutput** ppOutput) {
    if (ppOutput) *ppOutput = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
}

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS*) {
    return DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
}

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::GetLastPresentCount(UINT* pLastPresentCount) {
    if (!pLastPresentCount) return E_INVALIDARG;
    *pLastPresentCount = static_cast<UINT>(m_frameNumber);
    return S_OK;
}

// =============================================================================
// IDXGISwapChain1
// =============================================================================

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) {
    if (!pDesc) return E_INVALIDARG;
    *pDesc = {};
    pDesc->Width = m_desc.BufferDesc.Width;
    pDesc->Height = m_desc.BufferDesc.Height;
    pDesc->Format = m_desc.BufferDesc.Format;
    pDesc->SampleDesc = m_desc.SampleDesc;
    pDesc->BufferUsage = m_desc.BufferUsage;
    pDesc->BufferCount = m_desc.BufferCount;
    pDesc->Scaling = DXGI_SCALING_STRETCH;
    pDesc->SwapEffect = m_desc.SwapEffect;
    pDesc->AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    pDesc->Flags = m_desc.Flags;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) {
    if (!pDesc) return E_INVALIDARG;
    *pDesc = {};
    pDesc->RefreshRate = m_desc.BufferDesc.RefreshRate;
    pDesc->Windowed = TRUE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::GetHwnd(HWND* pHwnd) {
    if (!pHwnd) return E_INVALIDARG;
    *pHwnd = m_desc.OutputWindow;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::GetCoreWindow(REFIID, void** ppUnk) {
    if (ppUnk) *ppUnk = nullptr;
    return DXGI_ERROR_INVALID_CALL;
}

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS*) {
    return Present(SyncInterval, PresentFlags); // dirty rectangles are an optimisation hint only
}

WINBOOL STDMETHODCALLTYPE ProxyDXGISwapChain::IsTemporaryMonoSupported() { return FALSE; }

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) {
    if (ppRestrictToOutput) *ppRestrictToOutput = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::SetBackgroundColor(const DXGI_RGBA* pColor) {
    if (!pColor) return E_INVALIDARG;
    m_backgroundColor = *pColor;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::GetBackgroundColor(DXGI_RGBA* pColor) {
    if (!pColor) return E_INVALIDARG;
    *pColor = m_backgroundColor;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::SetRotation(DXGI_MODE_ROTATION Rotation) {
    return Rotation == DXGI_MODE_ROTATION_IDENTITY ? S_OK : DXGI_ERROR_UNSUPPORTED;
}

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::GetRotation(DXGI_MODE_ROTATION* pRotation) {
    if (!pRotation) return E_INVALIDARG;
    *pRotation = DXGI_MODE_ROTATION_IDENTITY;
    return S_OK;
}

} // namespace RemoteGPU
