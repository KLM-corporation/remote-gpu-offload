// Drop-in d3d11.dll: every device the application creates is a remote proxy device.
// Server address: RGPU_SERVER (default 127.0.0.1), port: RGPU_PORT (default 48888).

#include "proxy_device.h"
#include "proxy_dxgi.h"

#include <cstdlib>

using namespace RemoteGPU;

namespace {

const char* ServerHost() {
    const char* host = std::getenv("RGPU_SERVER");
    return (host && *host) ? host : "127.0.0.1";
}

uint16_t ServerPort() {
    const char* port = std::getenv("RGPU_PORT");
    if (port && *port) {
        long value = std::strtol(port, nullptr, 10);
        if (value > 0 && value <= 65535) return static_cast<uint16_t>(value);
    }
    return RGPU_DEFAULT_PORT;
}

bool FeatureLevelAccepted(const D3D_FEATURE_LEVEL* levels, UINT count) {
    if (!levels || count == 0) return true;
    for (UINT i = 0; i < count; ++i) {
        // Accept up to D3D_FEATURE_LEVEL_11_1 (supported natively by server GPU e.g. RTX 4070)
        if (levels[i] <= D3D_FEATURE_LEVEL_11_1) return true;
    }
    return false;
}

} // namespace

extern "C" {

HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
    UINT, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext) {
    if (ppSwapChain && !pSwapChainDesc) return E_INVALIDARG;
    if (!FeatureLevelAccepted(pFeatureLevels, FeatureLevels)) return E_INVALIDARG;

    D3D_FEATURE_LEVEL actualLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = ProxyD3D11Device::CreateProxyDeviceAndSwapChain(ServerHost(), ServerPort(), pSwapChainDesc, ppSwapChain,
                                                           ppDevice, &actualLevel, ppImmediateContext);
    if (FAILED(hr)) return hr;

    if (pFeatureLevel) {
        *pFeatureLevel = actualLevel;
        if (pFeatureLevels && FeatureLevels > 0) {
            for (UINT i = 0; i < FeatureLevels; ++i) {
                if (pFeatureLevels[i] <= actualLevel) {
                    *pFeatureLevel = pFeatureLevels[i];
                    break;
                }
            }
        }
    }
    return hr;
}

HRESULT WINAPI D3D11CreateDevice(
    IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext) {
    return D3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
                                         SDKVersion, nullptr, nullptr, ppDevice, pFeatureLevel, ppImmediateContext);
}

// Backs CreateDXGIFactory/1/2 of the companion dxgi.dll (src/client/dxgi_wrapper.cpp), so that
// factories and devices come from this one module.
HRESULT WINAPI RgpuCreateDXGIFactory(REFIID riid, void** ppFactory) {
    if (!ppFactory) return E_INVALIDARG;
    *ppFactory = nullptr;
    auto* factory = new ProxyDXGIFactory(QueryRemoteAdapter(ServerHost(), ServerPort()));
    HRESULT hr = factory->QueryInterface(riid, ppFactory);
    factory->Release();
    return hr;
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) NetworkSocket::ShutdownNetwork();
    return TRUE;
}
