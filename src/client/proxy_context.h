#pragma once
#ifndef REMOTE_GPU_PROXY_CONTEXT_H
#define REMOTE_GPU_PROXY_CONTEXT_H

#include "proxy_device.h"

namespace RemoteGPU {

/**
 * @brief COM proxy of ID3D11DeviceContext (immediate context).
 * Pipeline binding, clear, draw and buffer-update calls are serialized to the host GPU
 * server; every other entry point is a benign default (see generated/proxy_context_stubs.inc).
 */
class ProxyD3D11DeviceContext : public ID3D11DeviceContext {
public:
    explicit ProxyD3D11DeviceContext(ProxyD3D11Device* device);
    virtual ~ProxyD3D11DeviceContext() = default;

    ProxyD3D11DeviceContext(const ProxyD3D11DeviceContext&) = delete;
    ProxyD3D11DeviceContext& operator=(const ProxyD3D11DeviceContext&) = delete;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ID3D11DeviceChild
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;

    // ID3D11DeviceContext (generated list of all 108 methods)
#include "generated/proxy_context_decls.inc"

private:
    bool Send(uint16_t opcode, const void* payload, uint32_t length);

    ProxyD3D11Device* m_device;
    volatile ULONG    m_refCount{1};
};

} // namespace RemoteGPU

#endif // REMOTE_GPU_PROXY_CONTEXT_H
