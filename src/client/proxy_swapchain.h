#pragma once
#ifndef REMOTE_GPU_PROXY_SWAPCHAIN_H
#define REMOTE_GPU_PROXY_SWAPCHAIN_H

#include "proxy_device.h"
#include "presentation.h"

#include <dxgi1_2.h>

namespace RemoteGPU {

class ProxyD3D11DeviceContext;

/**
 * @brief COM proxy of IDXGISwapChain.
 * Present() sends OP_PRESENT, blocks until the matching OP_FRAME_DATA arrives (1:1 frame pacing),
 * and blits the pixels into OutputWindow through GDI. The back buffer is a ProxyTexture2D with
 * res_id 0, which the host maps to its offscreen render target.
 */
class ProxyDXGISwapChain : public IDXGISwapChain1 {
public:
    ProxyDXGISwapChain(ProxyD3D11Device* device, ProxyD3D11DeviceContext* context, const DXGI_SWAP_CHAIN_DESC& desc);
    virtual ~ProxyDXGISwapChain();

    ProxyDXGISwapChain(const ProxyDXGISwapChain&) = delete;
    ProxyDXGISwapChain& operator=(const ProxyDXGISwapChain&) = delete;

    uint64_t GetPresentedFrameCount() const noexcept { return m_frameNumber; }
    const std::vector<uint8_t>& GetLastFrame() const noexcept { return m_lastFrame; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override;

    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice) override;

    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) override;
    HRESULT STDMETHODCALLTYPE SetFullscreenState(WINBOOL Fullscreen, IDXGIOutput* pTarget) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenState(WINBOOL* pFullscreen, IDXGIOutput** ppTarget) override;
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) override;
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) override;
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** ppOutput) override;
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override;
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount) override;

    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* pHwnd) override;
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void** ppUnk) override;
    HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) override;
    WINBOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override;
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) override;
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* pColor) override;
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* pColor) override;
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override;
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* pRotation) override;

private:
    void CreateBackBuffer();

    ProxyD3D11Device*        m_device;   // owned reference
    ProxyD3D11DeviceContext* m_context;  // owned reference
    DXGI_SWAP_CHAIN_DESC     m_desc{};
    ProxyTexture2D*          m_backBuffer{nullptr};
    GdiPresenter             m_presenter;
    std::vector<uint8_t>     m_lastFrame;
    uint64_t                 m_frameNumber{0};
    DXGI_RGBA                m_backgroundColor{0, 0, 0, 1};
    volatile ULONG           m_refCount{1};
};

} // namespace RemoteGPU

#endif // REMOTE_GPU_PROXY_SWAPCHAIN_H
