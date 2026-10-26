#include "proxy_dxgi.h"
#include "proxy_swapchain.h"

#include <cstring>
#include <mutex>

namespace RemoteGPU {

namespace {

std::mutex        g_adapterMutex;
bool              g_adapterKnown = false;
RemoteAdapterInfo g_adapter;

// Wraps a swap chain around a proxy device. Real D3D devices are refused: this factory can only
// present frames rendered by the remote server.
HRESULT CreateProxySwapChain(IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC desc, ProxyDXGISwapChain** ppSwapChain) {
    if (!pDevice || !ppSwapChain) return DXGI_ERROR_INVALID_CALL;
    *ppSwapChain = nullptr;

    ID3D11Device* raw = nullptr;
    if (FAILED(pDevice->QueryInterface(IID_IProxyDevice, reinterpret_cast<void**>(&raw))) || !raw)
        return DXGI_ERROR_INVALID_CALL;
    auto* device = static_cast<ProxyD3D11Device*>(raw);

    ResolveSwapChainSize(desc.OutputWindow, desc.BufferDesc.Width, desc.BufferDesc.Height);
    HRESULT hr = S_OK;
    if (!device->ResizeRemoteTargets(desc.BufferDesc.Width, desc.BufferDesc.Height)) {
        hr = DXGI_ERROR_DEVICE_REMOVED;
    } else {
        *ppSwapChain = new ProxyDXGISwapChain(device, device->GetProxyContext(), desc);
    }
    raw->Release();
    return hr;
}

} // namespace

void RememberRemoteAdapter(const RemoteAdapterInfo& info) {
    std::lock_guard<std::mutex> lock(g_adapterMutex);
    g_adapter = info;
    g_adapterKnown = true;
}

RemoteAdapterInfo QueryRemoteAdapter(const char* host, uint16_t port) {
    {
        std::lock_guard<std::mutex> lock(g_adapterMutex);
        if (g_adapterKnown) return g_adapter;
    }
    // Short probe session: handshake (which records the adapter) then OP_SHUTDOWN on release.
    ProxyD3D11Device* probe = nullptr;
    if (SUCCEEDED(ProxyD3D11Device::Connect(host, port, 8, 8, &probe))) {
        probe->Release();
    }
    std::lock_guard<std::mutex> lock(g_adapterMutex);
    return g_adapter; // the generic description when the server could not be reached
}

// =============================================================================
// ProxyDXGIFactory
// =============================================================================

ProxyDXGIFactory::ProxyDXGIFactory(const RemoteAdapterInfo& adapter) : m_adapter(adapter) {}

HRESULT STDMETHODCALLTYPE ProxyDXGIFactory::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIFactory) ||
        riid == __uuidof(IDXGIFactory1) || riid == __uuidof(IDXGIFactory2)) {
        *ppvObject = static_cast<IDXGIFactory2*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE ProxyDXGIFactory::AddRef() { return InterlockedIncrement(&m_refCount); }

ULONG STDMETHODCALLTYPE ProxyDXGIFactory::Release() {
    ULONG count = InterlockedDecrement(&m_refCount);
    if (count == 0) delete this;
    return count;
}

HRESULT STDMETHODCALLTYPE ProxyDXGIFactory::SetPrivateData(REFGUID, UINT, const void*) { return S_OK; }
HRESULT STDMETHODCALLTYPE ProxyDXGIFactory::SetPrivateDataInterface(REFGUID, const IUnknown*) { return S_OK; }
HRESULT STDMETHODCALLTYPE ProxyDXGIFactory::GetPrivateData(REFGUID, UINT*, void*) { return DXGI_ERROR_NOT_FOUND; }

HRESULT STDMETHODCALLTYPE ProxyDXGIFactory::GetParent(REFIID, void** ppParent) {
    if (ppParent) *ppParent = nullptr;
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE ProxyDXGIFactory::EnumAdapters(UINT Adapter, IDXGIAdapter** ppAdapter) {
    if (!ppAdapter) return E_INVALIDARG;
    *ppAdapter = nullptr;
    if (Adapter != 0) return DXGI_ERROR_NOT_FOUND;
    *ppAdapter = new ProxyDXGIAdapter(m_adapter);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyDXGIFactory::EnumAdapters1(UINT Adapter, IDXGIAdapter1** ppAdapter) {
    if (!ppAdapter) return E_INVALIDARG;
    *ppAdapter = nullptr;
    if (Adapter != 0) return DXGI_ERROR_NOT_FOUND;
    *ppAdapter = new ProxyDXGIAdapter(m_adapter);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyDXGIFactory::MakeWindowAssociation(HWND WindowHandle, UINT) {
    m_associatedWindow = WindowHandle; // Alt+Enter is never handled: there is no fullscreen mode
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyDXGIFactory::GetWindowAssociation(HWND* pWindowHandle) {
    if (!pWindowHandle) return DXGI_ERROR_INVALID_CALL;
    *pWindowHandle = m_associatedWindow;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyDXGIFactory::CreateSwapChain(IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                                            IDXGISwapChain** ppSwapChain) {
    if (!pDesc || !ppSwapChain) return DXGI_ERROR_INVALID_CALL;
    ProxyDXGISwapChain* sc = nullptr;
    HRESULT hr = CreateProxySwapChain(pDevice, *pDesc, &sc);
    *ppSwapChain = sc;
    return hr;
}

HRESULT STDMETHODCALLTYPE ProxyDXGIFactory::CreateSoftwareAdapter(HMODULE, IDXGIAdapter** ppAdapter) {
    if (ppAdapter) *ppAdapter = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
}

WINBOOL STDMETHODCALLTYPE ProxyDXGIFactory::IsCurrent() { return TRUE; }
WINBOOL STDMETHODCALLTYPE ProxyDXGIFactory::IsWindowedStereoEnabled() { return FALSE; }

HRESULT STDMETHODCALLTYPE ProxyDXGIFactory::CreateSwapChainForHwnd(IUnknown* pDevice, HWND hWnd,
                                                                   const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                                   const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
                                                                   IDXGIOutput*, IDXGISwapChain1** ppSwapChain) {
    if (!pDesc || !ppSwapChain || !hWnd) return DXGI_ERROR_INVALID_CALL;
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferDesc.Width = pDesc->Width;
    desc.BufferDesc.Height = pDesc->Height;
    desc.BufferDesc.Format = pDesc->Format;
    desc.SampleDesc = pDesc->SampleDesc;
    desc.BufferUsage = pDesc->BufferUsage;
    desc.BufferCount = pDesc->BufferCount;
    desc.OutputWindow = hWnd;
    desc.Windowed = TRUE;
    desc.SwapEffect = pDesc->SwapEffect;
    desc.Flags = pDesc->Flags;

    ProxyDXGISwapChain* sc = nullptr;
    HRESULT hr = CreateProxySwapChain(pDevice, desc, &sc);
    *ppSwapChain = sc;
    return hr;
}

HRESULT STDMETHODCALLTYPE ProxyDXGIFactory::CreateSwapChainForCoreWindow(IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
                                                                         IDXGIOutput*, IDXGISwapChain1** ppSwapChain) {
    if (ppSwapChain) *ppSwapChain = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
}

HRESULT STDMETHODCALLTYPE ProxyDXGIFactory::CreateSwapChainForComposition(IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*,
                                                                          IDXGISwapChain1** ppSwapChain) {
    if (ppSwapChain) *ppSwapChain = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
}

HRESULT STDMETHODCALLTYPE ProxyDXGIFactory::GetSharedResourceAdapterLuid(HANDLE, LUID*) { return DXGI_ERROR_UNSUPPORTED; }
HRESULT STDMETHODCALLTYPE ProxyDXGIFactory::RegisterStereoStatusWindow(HWND, UINT, DWORD*) { return DXGI_ERROR_UNSUPPORTED; }
HRESULT STDMETHODCALLTYPE ProxyDXGIFactory::RegisterStereoStatusEvent(HANDLE, DWORD*) { return DXGI_ERROR_UNSUPPORTED; }
void STDMETHODCALLTYPE ProxyDXGIFactory::UnregisterStereoStatus(DWORD) {}
HRESULT STDMETHODCALLTYPE ProxyDXGIFactory::RegisterOcclusionStatusWindow(HWND, UINT, DWORD*) { return DXGI_ERROR_UNSUPPORTED; }
HRESULT STDMETHODCALLTYPE ProxyDXGIFactory::RegisterOcclusionStatusEvent(HANDLE, DWORD*) { return DXGI_ERROR_UNSUPPORTED; }
void STDMETHODCALLTYPE ProxyDXGIFactory::UnregisterOcclusionStatus(DWORD) {}

// =============================================================================
// ProxyDXGIAdapter
// =============================================================================

ProxyDXGIAdapter::ProxyDXGIAdapter(const RemoteAdapterInfo& info) : m_info(info) {}

HRESULT STDMETHODCALLTYPE ProxyDXGIAdapter::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIAdapter) ||
        riid == __uuidof(IDXGIAdapter1)) {
        *ppvObject = static_cast<IDXGIAdapter1*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE ProxyDXGIAdapter::AddRef() { return InterlockedIncrement(&m_refCount); }

ULONG STDMETHODCALLTYPE ProxyDXGIAdapter::Release() {
    ULONG count = InterlockedDecrement(&m_refCount);
    if (count == 0) delete this;
    return count;
}

HRESULT STDMETHODCALLTYPE ProxyDXGIAdapter::SetPrivateData(REFGUID, UINT, const void*) { return S_OK; }
HRESULT STDMETHODCALLTYPE ProxyDXGIAdapter::SetPrivateDataInterface(REFGUID, const IUnknown*) { return S_OK; }
HRESULT STDMETHODCALLTYPE ProxyDXGIAdapter::GetPrivateData(REFGUID, UINT*, void*) { return DXGI_ERROR_NOT_FOUND; }

HRESULT STDMETHODCALLTYPE ProxyDXGIAdapter::GetParent(REFIID riid, void** ppParent) {
    if (!ppParent) return E_INVALIDARG;
    auto* factory = new ProxyDXGIFactory(m_info);
    HRESULT hr = factory->QueryInterface(riid, ppParent);
    factory->Release();
    return hr;
}

HRESULT STDMETHODCALLTYPE ProxyDXGIAdapter::EnumOutputs(UINT, IDXGIOutput** ppOutput) {
    if (ppOutput) *ppOutput = nullptr;
    return DXGI_ERROR_NOT_FOUND;
}

HRESULT STDMETHODCALLTYPE ProxyDXGIAdapter::GetDesc(DXGI_ADAPTER_DESC* pDesc) {
    if (!pDesc) return E_INVALIDARG;
    DXGI_ADAPTER_DESC1 desc1;
    GetDesc1(&desc1);
    std::memcpy(pDesc, &desc1, sizeof(*pDesc)); // DESC1 extends DESC with a trailing Flags field
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyDXGIAdapter::GetDesc1(DXGI_ADAPTER_DESC1* pDesc) {
    if (!pDesc) return E_INVALIDARG;
    std::memset(pDesc, 0, sizeof(*pDesc));
    const std::string name = "RemoteGPU: " + m_info.description;
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, pDesc->Description, ARRAYSIZE(pDesc->Description) - 1);
    pDesc->DedicatedVideoMemory = static_cast<SIZE_T>(m_info.dedicated_vram);
    pDesc->AdapterLuid.LowPart = 0x52475055; // 'RGPU': stable, never matches a local adapter
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyDXGIAdapter::CheckInterfaceSupport(REFGUID, LARGE_INTEGER*) {
    return DXGI_ERROR_UNSUPPORTED; // only Direct3D 10 devices are queried through this method
}

} // namespace RemoteGPU
