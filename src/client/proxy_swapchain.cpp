#include "proxy_swapchain.h"
#include "proxy_context.h"
#include "frame_codec.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace RemoteGPU {

namespace {

// Writes a top-down 32-bit BMP from an RGBA buffer (used by the automated pixel verifier).
bool WriteBmp(const char* path, const uint8_t* rgba, uint32_t width, uint32_t height) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const uint32_t image_size = width * height * 4;
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    fh.bfType = 0x4D42; // 'BM'
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + image_size;
    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(width);
    ih.biHeight = -static_cast<LONG>(height);
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = image_size;
    std::fwrite(&fh, sizeof(fh), 1, f);
    std::fwrite(&ih, sizeof(ih), 1, f);
    std::vector<uint8_t> row(width * 4);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* src = rgba + static_cast<size_t>(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            row[x * 4 + 0] = src[x * 4 + 2];
            row[x * 4 + 1] = src[x * 4 + 1];
            row[x * 4 + 2] = src[x * 4 + 0];
            row[x * 4 + 3] = src[x * 4 + 3];
        }
        std::fwrite(row.data(), row.size(), 1, f);
    }
    std::fclose(f);
    return true;
}

} // namespace

ProxyDXGISwapChain::ProxyDXGISwapChain(ProxyD3D11Device* device, ProxyD3D11DeviceContext* context,
                                       const DXGI_SWAP_CHAIN_DESC& desc)
    : m_device(device), m_context(context), m_desc(desc) {
    m_device->AddRef();
    m_context->AddRef();

    D3D11_TEXTURE2D_DESC tex = {};
    tex.Width = desc.BufferDesc.Width;
    tex.Height = desc.BufferDesc.Height;
    tex.MipLevels = 1;
    tex.ArraySize = 1;
    tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex.SampleDesc.Count = 1;
    tex.Usage = D3D11_USAGE_DEFAULT;
    tex.BindFlags = D3D11_BIND_RENDER_TARGET;
    m_backBuffer = new ProxyTexture2D(0, tex, device); // res_id 0 == host offscreen target

    m_presenter.SetForceBgraConversion(true);
    m_presenter.Initialize(desc.OutputWindow, tex.Width, tex.Height);
}

ProxyDXGISwapChain::~ProxyDXGISwapChain() {
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
        riid == __uuidof(IDXGISwapChain)) {
        *ppvObject = static_cast<IDXGISwapChain*>(this);
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

    // 1:1 pacing: block until the host has rendered and returned this frame.
    std::shared_ptr<NetworkSocket> socket = m_device->GetSocket();
    PacketHeader hdr = {};
    std::vector<uint8_t> payload;
    for (;;) {
        if (!socket->RecvPacket(hdr, payload)) {
            std::cerr << "[RemoteGPU] Connection lost while waiting for frame " << m_frameNumber << std::endl;
            return DXGI_ERROR_DEVICE_REMOVED;
        }
        if (hdr.opcode == OP_FRAME_DATA) break;
        if (hdr.opcode == OP_SHUTDOWN) return DXGI_ERROR_DEVICE_REMOVED;
        // Any other server->client packet is not expected here; skip it.
    }

    if (payload.size() < sizeof(FrameDataPayload)) return DXGI_ERROR_DEVICE_RESET;
    FrameDataPayload frame;
    std::memcpy(&frame, payload.data(), sizeof(frame));
    const size_t pixel_count = static_cast<size_t>(frame.width) * frame.height;
    const size_t expected = pixel_count * 4;
    const uint8_t* body = payload.data() + sizeof(frame);
    const size_t body_size = payload.size() - sizeof(frame);

    if (hdr.flags & FLAG_COMPRESSED) {
        // m_lastFrame is the previous decoded frame: the delta reference, decoded over in place.
        const bool has_reference = (m_lastFrame.size() == expected);
        if (!has_reference) m_lastFrame.assign(expected, 0);
        if (frame.data_size != body_size ||
            !FrameCodec::Decode(body, body_size, m_lastFrame.data(), pixel_count, has_reference)) {
            std::cerr << "[RemoteGPU] Corrupt compressed frame " << frame.frame_number << std::endl;
            m_lastFrame.clear(); // force the next frame to fail unless it is a key frame
            return DXGI_ERROR_DEVICE_RESET;
        }
    } else {
        if (frame.data_size < expected || body_size < expected) return DXGI_ERROR_DEVICE_RESET;
        m_lastFrame.assign(body, body + expected);
    }
    m_presenter.Present(m_desc.OutputWindow, m_lastFrame.data(), frame.width, frame.height, frame.format);
    return S_OK;
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

HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::ResizeBuffers(UINT, UINT, UINT, DXGI_FORMAT, UINT) {
    // The host render target size is fixed at handshake; GDI stretches to the window.
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

} // namespace RemoteGPU
