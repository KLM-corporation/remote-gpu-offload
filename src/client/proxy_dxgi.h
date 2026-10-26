#pragma once
#ifndef REMOTE_GPU_PROXY_DXGI_H
#define REMOTE_GPU_PROXY_DXGI_H

#include "proxy_device.h"

#include <dxgi1_2.h>

namespace RemoteGPU {

/**
 * @brief IDXGIFactory2 proxy. Builds ProxyDXGISwapChain objects for proxy devices (CreateSwapChain,
 * CreateSwapChainForHwnd) and enumerates a single adapter: the remote GPU.
 */
class ProxyDXGIFactory : public IDXGIFactory2 {
public:
    explicit ProxyDXGIFactory(const RemoteAdapterInfo& adapter);
    virtual ~ProxyDXGIFactory() = default;

    ProxyDXGIFactory(const ProxyDXGIFactory&) = delete;
    ProxyDXGIFactory& operator=(const ProxyDXGIFactory&) = delete;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override;

    // IDXGIFactory
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter, IDXGIAdapter** ppAdapter) override;
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle, UINT Flags) override;
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* pWindowHandle) override;
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain) override;
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter** ppAdapter) override;

    // IDXGIFactory1
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter, IDXGIAdapter1** ppAdapter) override;
    WINBOOL STDMETHODCALLTYPE IsCurrent() override;

    // IDXGIFactory2
    WINBOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override;
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                                     IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) override;
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* pDevice, IUnknown* pWindow, const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                           IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) override;
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource, LUID* pLuid) override;
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle, UINT wMsg, DWORD* pdwCookie) override;
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent, DWORD* pdwCookie) override;
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) override;
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND WindowHandle, UINT wMsg, DWORD* pdwCookie) override;
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD* pdwCookie) override;
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) override;
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* pDevice, const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                            IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) override;

private:
    RemoteAdapterInfo m_adapter;
    HWND              m_associatedWindow{nullptr};
    volatile ULONG    m_refCount{1};
};

/**
 * @brief IDXGIAdapter1 proxy describing the server's GPU. It has no outputs (the client never
 * drives a display through DXGI) and its parent is a ProxyDXGIFactory.
 */
class ProxyDXGIAdapter : public IDXGIAdapter1 {
public:
    explicit ProxyDXGIAdapter(const RemoteAdapterInfo& info);
    virtual ~ProxyDXGIAdapter() = default;

    ProxyDXGIAdapter(const ProxyDXGIAdapter&) = delete;
    ProxyDXGIAdapter& operator=(const ProxyDXGIAdapter&) = delete;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override;

    // IDXGIAdapter / IDXGIAdapter1
    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT Output, IDXGIOutput** ppOutput) override;
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID InterfaceName, LARGE_INTEGER* pUMDVersion) override;
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1* pDesc) override;

private:
    RemoteAdapterInfo m_info;
    volatile ULONG    m_refCount{1};
};

// Remote adapter identity shared by every factory of the process. Devices record it when they
// connect; a factory created before any device (CreateDXGIFactory*) probes the server once.
void RememberRemoteAdapter(const RemoteAdapterInfo& info);
RemoteAdapterInfo QueryRemoteAdapter(const char* host, uint16_t port);

} // namespace RemoteGPU

#endif // REMOTE_GPU_PROXY_DXGI_H
