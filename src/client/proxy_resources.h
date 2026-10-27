#pragma once
#ifndef REMOTE_GPU_PROXY_RESOURCES_H
#define REMOTE_GPU_PROXY_RESOURCES_H

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
#include "dxgi_format_info.h"

#include <cstdint>
#include <vector>
#include <cstring>
#include <atomic>

namespace RemoteGPU {

class ProxyD3D11Device;

// {4B6E1C85-4E7B-4A1C-9E28-87C9E0D90001}
static constexpr GUID IID_IProxyResource = {
    0x4b6e1c85, 0x4e7b, 0x4a1c, { 0x9e, 0x28, 0x87, 0xc9, 0xe0, 0xd9, 0x00, 0x01 }
};

/**
 * @brief Internal COM interface exposed by all client proxy COM wrappers
 * to facilitate fast, safe retrieval of the monotonic resource ID.
 */
class IProxyResource : public IUnknown {
public:
    virtual uint32_t STDMETHODCALLTYPE GetResourceId() const noexcept = 0;
    virtual ProxyD3D11Device* STDMETHODCALLTYPE GetProxyDevice() const noexcept = 0;
};

/**
 * @brief Helper template function to extract client resource ID from an arbitrary COM pointer.
 */
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

/**
 * @brief Base class for all Direct3D 11 proxy child objects.
 */
class ProxyResourceBase : public IProxyResource {
public:
    ProxyResourceBase(uint32_t res_id, ProxyD3D11Device* device)
        : m_resId(res_id), m_device(device), m_refCount(1) {}

    virtual ~ProxyResourceBase() = default;

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

    uint32_t STDMETHODCALLTYPE GetResourceId() const noexcept override {
        return m_resId;
    }

    ProxyD3D11Device* STDMETHODCALLTYPE GetProxyDevice() const noexcept override {
        return m_device;
    }

protected:
    void NotifyDestroy();

    uint32_t          m_resId;
    ProxyD3D11Device* m_device;
    volatile ULONG    m_refCount;
};

// =============================================================================
// ProxyBuffer: ID3D11Buffer
// =============================================================================
class ProxyBuffer : public ID3D11Buffer, public ProxyResourceBase {
public:
    ProxyBuffer(uint32_t res_id, const D3D11_BUFFER_DESC& desc, ProxyD3D11Device* device, const void* pInitialData)
        : ProxyResourceBase(res_id, device), m_desc(desc)
    {
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
    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;
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

    // Dynamic buffer shadow mapping accessors
    uint8_t* GetShadowData() noexcept {
        if (m_shadowData.empty()) {
            m_shadowData.resize(m_desc.ByteWidth ? m_desc.ByteWidth : 256, 0);
        }
        return m_shadowData.data();
    }
    const uint8_t* GetShadowData() const noexcept {
        if (m_shadowData.empty()) {
            const_cast<std::vector<uint8_t>&>(m_shadowData).resize(m_desc.ByteWidth ? m_desc.ByteWidth : 256, 0);
        }
        return m_shadowData.data();
    }
    const D3D11_BUFFER_DESC& GetDesc() const noexcept { return m_desc; }
    bool IsMapped() const noexcept { return m_isMapped; }
    void SetMapped(bool mapped, UINT subres = 0, D3D11_MAP type = D3D11_MAP_WRITE_DISCARD) noexcept {
        m_isMapped = mapped;
        m_mappedSubresource = subres;
        m_mappedType = type;
    }
    D3D11_MAP GetMappedType() const noexcept { return m_mappedType; }

private:
    D3D11_BUFFER_DESC    m_desc{};
    std::vector<uint8_t> m_shadowData;
    bool                 m_isMapped{false};
    UINT                 m_mappedSubresource{0};
    D3D11_MAP            m_mappedType{D3D11_MAP_WRITE_DISCARD};
};

// =============================================================================
// ProxyTexture2D: ID3D11Texture2D & IDXGISurface
// =============================================================================
class ProxyTexture2D : public ID3D11Texture2D, public IDXGISurface, public ProxyResourceBase {
public:
    ProxyTexture2D(uint32_t res_id, const D3D11_TEXTURE2D_DESC& desc, ProxyD3D11Device* device)
        : ProxyResourceBase(res_id, device), m_desc(desc) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
            riid == __uuidof(ID3D11Resource) || riid == __uuidof(ID3D11Texture2D)) {
            *ppvObject = static_cast<ID3D11Texture2D*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IDXGISurface) || riid == __uuidof(IDXGIDeviceSubObject) ||
            riid == __uuidof(IDXGIObject)) {
            *ppvObject = static_cast<IDXGISurface*>(this);
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
    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }

    // ID3D11Resource
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION *pResourceDimension) override {
        if (pResourceDimension) *pResourceDimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
    }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }

    // ID3D11Texture2D
    void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE2D_DESC *pDesc) override {
        if (pDesc) *pDesc = m_desc;
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void**) override { return E_NOTIMPL; }

    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **ppDevice) override {
        if (!ppDevice) return E_POINTER;
        ID3D11Device* dev = nullptr;
        GetDevice(&dev);
        if (!dev) return E_FAIL;
        HRESULT hr = dev->QueryInterface(riid, ppDevice);
        dev->Release();
        return hr;
    }

    // IDXGISurface
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SURFACE_DESC *pDesc) override {
        if (!pDesc) return E_POINTER;
        pDesc->Width = m_desc.Width;
        pDesc->Height = m_desc.Height;
        pDesc->Format = m_desc.Format;
        pDesc->SampleDesc = m_desc.SampleDesc;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Map(DXGI_MAPPED_RECT* pLockedRect, UINT) override {
        if (!pLockedRect) return E_INVALIDARG;
        UINT rowPitch = 0, depthPitch = 0;
        pLockedRect->pBits = GetOrCreateShadowData(0, rowPitch, depthPitch);
        pLockedRect->Pitch = rowPitch;
        m_isMapped = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Unmap() override {
        m_isMapped = false;
        return S_OK;
    }

    uint8_t* GetOrCreateShadowData(UINT subresource, UINT& outRowPitch, UINT& outDepthPitch) {
        const uint32_t mip_levels = m_desc.MipLevels ? m_desc.MipLevels : 1;
        const uint32_t mip = subresource % mip_levels;
        const uint32_t mw = MipSize(m_desc.Width, mip);
        const uint32_t mh = MipSize(m_desc.Height, mip);
        uint64_t row_bytes = 0;
        uint32_t rows = 0;
        if (!SurfaceLayout(m_desc.Format, mw, mh, row_bytes, rows) || row_bytes == 0) {
            row_bytes = static_cast<uint64_t>(mw) * 4;
            rows = mh;
        }
        outRowPitch = static_cast<UINT>(row_bytes);
        outDepthPitch = static_cast<UINT>(row_bytes * rows);
        const size_t needed = static_cast<size_t>(outDepthPitch ? outDepthPitch : 64);
        if (m_shadowData.size() < needed) {
            m_shadowData.resize(needed, 0);
        }
        return m_shadowData.data();
    }
    bool IsMapped() const noexcept { return m_isMapped; }
    void SetMapped(bool mapped, UINT subres = 0, D3D11_MAP type = D3D11_MAP_READ) noexcept {
        m_isMapped = mapped;
        m_mappedSubresource = subres;
        m_mappedType = type;
    }
    UINT GetMappedSubresource() const noexcept { return m_mappedSubresource; }
    D3D11_MAP GetMappedType() const noexcept { return m_mappedType; }
    const D3D11_TEXTURE2D_DESC& GetDesc() const noexcept { return m_desc; }

private:
    D3D11_TEXTURE2D_DESC m_desc{};
    std::vector<uint8_t> m_shadowData;
    bool                 m_isMapped{false};
    UINT                 m_mappedSubresource{0};
    D3D11_MAP            m_mappedType{D3D11_MAP_READ};
};

// =============================================================================
// ProxyTexture3D: ID3D11Texture3D
// =============================================================================
class ProxyTexture3D : public ID3D11Texture3D, public ProxyResourceBase {
public:
    ProxyTexture3D(uint32_t res_id, const D3D11_TEXTURE3D_DESC& desc, ProxyD3D11Device* device)
        : ProxyResourceBase(res_id, device), m_desc(desc) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
            riid == __uuidof(ID3D11Resource) || riid == __uuidof(ID3D11Texture3D)) {
            *ppvObject = static_cast<ID3D11Texture3D*>(this);
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
    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }

    // ID3D11Resource
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION *pResourceDimension) override {
        if (pResourceDimension) *pResourceDimension = D3D11_RESOURCE_DIMENSION_TEXTURE3D;
    }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }

    // ID3D11Texture3D
    void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE3D_DESC *pDesc) override {
        if (pDesc) *pDesc = m_desc;
    }

    uint8_t* GetOrCreateShadowData(UINT subresource, UINT& outRowPitch, UINT& outDepthPitch) {
        const uint32_t mip_levels = m_desc.MipLevels ? m_desc.MipLevels : 1;
        const uint32_t mip = subresource % mip_levels;
        const uint32_t mw = MipSize(m_desc.Width, mip);
        const uint32_t mh = MipSize(m_desc.Height, mip);
        const uint32_t md = MipSize(m_desc.Depth, mip);
        uint64_t row_bytes = 0;
        uint32_t rows = 0;
        if (!SurfaceLayout(m_desc.Format, mw, mh, row_bytes, rows) || row_bytes == 0) {
            row_bytes = static_cast<uint64_t>(mw) * 4;
            rows = mh;
        }
        outRowPitch = static_cast<UINT>(row_bytes);
        outDepthPitch = static_cast<UINT>(row_bytes * rows);
        const size_t needed = static_cast<size_t>(outDepthPitch * (md ? md : 1));
        if (m_shadowData.size() < needed) {
            m_shadowData.resize(needed ? needed : 64, 0);
        }
        return m_shadowData.data();
    }
    bool IsMapped() const noexcept { return m_isMapped; }
    void SetMapped(bool mapped, UINT subres = 0, D3D11_MAP type = D3D11_MAP_READ) noexcept {
        m_isMapped = mapped;
        m_mappedSubresource = subres;
        m_mappedType = type;
    }
    UINT GetMappedSubresource() const noexcept { return m_mappedSubresource; }
    D3D11_MAP GetMappedType() const noexcept { return m_mappedType; }
    const D3D11_TEXTURE3D_DESC& GetDesc() const noexcept { return m_desc; }

private:
    D3D11_TEXTURE3D_DESC m_desc{};
    std::vector<uint8_t> m_shadowData;
    bool                 m_isMapped{false};
    UINT                 m_mappedSubresource{0};
    D3D11_MAP            m_mappedType{D3D11_MAP_READ};
};

// =============================================================================
// ProxyTexture1D: ID3D11Texture1D
// =============================================================================
class ProxyTexture1D : public ID3D11Texture1D, public ProxyResourceBase {
public:
    ProxyTexture1D(uint32_t res_id, const D3D11_TEXTURE1D_DESC& desc, ProxyD3D11Device* device)
        : ProxyResourceBase(res_id, device), m_desc(desc) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
            riid == __uuidof(ID3D11Resource) || riid == __uuidof(ID3D11Texture1D)) {
            *ppvObject = static_cast<ID3D11Texture1D*>(this);
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
    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }

    // ID3D11Resource
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION *pResourceDimension) override {
        if (pResourceDimension) *pResourceDimension = D3D11_RESOURCE_DIMENSION_TEXTURE1D;
    }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }

    // ID3D11Texture1D
    void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE1D_DESC *pDesc) override {
        if (pDesc) *pDesc = m_desc;
    }

    uint8_t* GetOrCreateShadowData(UINT subresource, UINT& outRowPitch, UINT& outDepthPitch) {
        const uint32_t mip_levels = m_desc.MipLevels ? m_desc.MipLevels : 1;
        const uint32_t mip = subresource % mip_levels;
        const uint32_t mw = MipSize(m_desc.Width, mip);
        uint64_t row_bytes = 0;
        uint32_t rows = 0;
        if (!SurfaceLayout(m_desc.Format, mw, 1, row_bytes, rows) || row_bytes == 0) {
            row_bytes = static_cast<uint64_t>(mw) * 4;
        }
        outRowPitch = static_cast<UINT>(row_bytes);
        outDepthPitch = static_cast<UINT>(row_bytes);
        const size_t needed = static_cast<size_t>(row_bytes ? row_bytes : 64);
        if (m_shadowData.size() < needed) {
            m_shadowData.resize(needed, 0);
        }
        return m_shadowData.data();
    }
    bool IsMapped() const noexcept { return m_isMapped; }
    void SetMapped(bool mapped, UINT subres = 0, D3D11_MAP type = D3D11_MAP_READ) noexcept {
        m_isMapped = mapped;
        m_mappedSubresource = subres;
        m_mappedType = type;
    }
    UINT GetMappedSubresource() const noexcept { return m_mappedSubresource; }
    D3D11_MAP GetMappedType() const noexcept { return m_mappedType; }
    const D3D11_TEXTURE1D_DESC& GetDesc() const noexcept { return m_desc; }

private:
    D3D11_TEXTURE1D_DESC m_desc{};
    std::vector<uint8_t> m_shadowData;
    bool                 m_isMapped{false};
    UINT                 m_mappedSubresource{0};
    D3D11_MAP            m_mappedType{D3D11_MAP_READ};
};

// =============================================================================
// ProxyQuery: ID3D11Query / ID3D11Predicate
// =============================================================================
class ProxyQuery : public ID3D11Query, public ProxyResourceBase {
public:
    ProxyQuery(uint32_t res_id, const D3D11_QUERY_DESC& desc, ProxyD3D11Device* device)
        : ProxyResourceBase(res_id, device), m_desc(desc) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
            riid == __uuidof(ID3D11Asynchronous) || riid == __uuidof(ID3D11Query)) {
            *ppvObject = static_cast<ID3D11Query*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(ID3D11Predicate)) {
            *ppvObject = static_cast<ID3D11Predicate*>(reinterpret_cast<ID3D11Predicate*>(this));
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
    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }

    // ID3D11Asynchronous
    UINT STDMETHODCALLTYPE GetDataSize() override {
        switch (m_desc.Query) {
            case D3D11_QUERY_EVENT: return sizeof(BOOL);
            case D3D11_QUERY_OCCLUSION: return sizeof(UINT64);
            case D3D11_QUERY_TIMESTAMP: return sizeof(UINT64);
            case D3D11_QUERY_TIMESTAMP_DISJOINT: return sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT);
            case D3D11_QUERY_PIPELINE_STATISTICS: return sizeof(D3D11_QUERY_DATA_PIPELINE_STATISTICS);
            case D3D11_QUERY_OCCLUSION_PREDICATE: return sizeof(BOOL);
            case D3D11_QUERY_SO_STATISTICS: return sizeof(D3D11_QUERY_DATA_SO_STATISTICS);
            case D3D11_QUERY_SO_OVERFLOW_PREDICATE: return sizeof(BOOL);
            default: return 0;
        }
    }

    // ID3D11Query
    void STDMETHODCALLTYPE GetDesc(D3D11_QUERY_DESC *pDesc) override {
        if (pDesc) *pDesc = m_desc;
    }

private:
    D3D11_QUERY_DESC m_desc{};
};

// =============================================================================
// ProxyShaderResourceView: ID3D11ShaderResourceView
// =============================================================================
class ProxyShaderResourceView : public ID3D11ShaderResourceView, public ProxyResourceBase {
public:
    ProxyShaderResourceView(uint32_t res_id, ID3D11Resource* pRes, const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc, ProxyD3D11Device* device)
        : ProxyResourceBase(res_id, device), m_resource(pRes)
    {
        if (pDesc) {
            m_desc = *pDesc;
        } else if (pRes) {
            std::memset(&m_desc, 0, sizeof(m_desc));
            D3D11_RESOURCE_DIMENSION dim;
            pRes->GetType(&dim);
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
                if (SUCCEEDED(pRes->QueryInterface(IID_PPV_ARGS(&tex)))) {
                    D3D11_TEXTURE2D_DESC td;
                    tex->GetDesc(&td);
                    m_desc.Format = td.Format;
                    if (td.ArraySize > 1) {
                        m_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                        m_desc.Texture2DArray.FirstArraySlice = 0;
                        m_desc.Texture2DArray.ArraySize = td.ArraySize;
                        m_desc.Texture2DArray.MipLevels = td.MipLevels;
                    } else {
                        m_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                        m_desc.Texture2D.MipLevels = td.MipLevels;
                    }
                }
            } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
                Microsoft::WRL::ComPtr<ID3D11Texture3D> tex;
                if (SUCCEEDED(pRes->QueryInterface(IID_PPV_ARGS(&tex)))) {
                    D3D11_TEXTURE3D_DESC td;
                    tex->GetDesc(&td);
                    m_desc.Format = td.Format;
                    m_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
                    m_desc.Texture3D.MipLevels = td.MipLevels;
                }
            } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
                m_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            }
        } else {
            std::memset(&m_desc, 0, sizeof(m_desc));
        }
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

    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }

    void STDMETHODCALLTYPE GetResource(ID3D11Resource **ppResource) override {
        if (ppResource) {
            *ppResource = m_resource.Get();
            if (m_resource) m_resource->AddRef();
        }
    }
    void STDMETHODCALLTYPE GetDesc(D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc) override {
        if (pDesc) *pDesc = m_desc;
    }

private:
    Microsoft::WRL::ComPtr<ID3D11Resource> m_resource;
    D3D11_SHADER_RESOURCE_VIEW_DESC        m_desc{};
};

// =============================================================================
// ProxyRenderTargetView: ID3D11RenderTargetView
// =============================================================================
class ProxyRenderTargetView : public ID3D11RenderTargetView, public ProxyResourceBase {
public:
    ProxyRenderTargetView(uint32_t res_id, ID3D11Resource* pRes, const D3D11_RENDER_TARGET_VIEW_DESC* pDesc, ProxyD3D11Device* device)
        : ProxyResourceBase(res_id, device), m_resource(pRes)
    {
        if (pDesc) {
            m_desc = *pDesc;
        } else {
            std::memset(&m_desc, 0, sizeof(m_desc));
            if (pRes) {
                D3D11_RESOURCE_DIMENSION dim;
                pRes->GetType(&dim);
                if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
                    if (SUCCEEDED(pRes->QueryInterface(IID_PPV_ARGS(&tex)))) {
                        D3D11_TEXTURE2D_DESC td;
                        tex->GetDesc(&td);
                        m_desc.Format = td.Format;
                        if (td.ArraySize > 1) {
                            m_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                            m_desc.Texture2DArray.FirstArraySlice = 0;
                            m_desc.Texture2DArray.ArraySize = td.ArraySize;
                            m_desc.Texture2DArray.MipSlice = 0;
                        } else {
                            m_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                            m_desc.Texture2D.MipSlice = 0;
                        }
                    }
                }
            }
        }
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

    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }

    void STDMETHODCALLTYPE GetResource(ID3D11Resource **ppResource) override {
        if (ppResource) {
            *ppResource = m_resource.Get();
            if (m_resource) m_resource->AddRef();
        }
    }
    void STDMETHODCALLTYPE GetDesc(D3D11_RENDER_TARGET_VIEW_DESC *pDesc) override {
        if (pDesc) *pDesc = m_desc;
    }

private:
    Microsoft::WRL::ComPtr<ID3D11Resource> m_resource;
    D3D11_RENDER_TARGET_VIEW_DESC          m_desc{};
};

// =============================================================================
// ProxyDepthStencilView: ID3D11DepthStencilView
// =============================================================================
class ProxyDepthStencilView : public ID3D11DepthStencilView, public ProxyResourceBase {
public:
    ProxyDepthStencilView(uint32_t res_id, ID3D11Resource* pRes, const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc, ProxyD3D11Device* device)
        : ProxyResourceBase(res_id, device), m_resource(pRes)
    {
        if (pDesc) {
            m_desc = *pDesc;
        } else {
            std::memset(&m_desc, 0, sizeof(m_desc));
            if (pRes) {
                D3D11_RESOURCE_DIMENSION dim;
                pRes->GetType(&dim);
                if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
                    if (SUCCEEDED(pRes->QueryInterface(IID_PPV_ARGS(&tex)))) {
                        D3D11_TEXTURE2D_DESC td;
                        tex->GetDesc(&td);
                        m_desc.Format = td.Format;
                        if (td.ArraySize > 1) {
                            m_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
                            m_desc.Texture2DArray.FirstArraySlice = 0;
                            m_desc.Texture2DArray.ArraySize = td.ArraySize;
                            m_desc.Texture2DArray.MipSlice = 0;
                        } else {
                            m_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                            m_desc.Texture2D.MipSlice = 0;
                        }
                    }
                }
            }
        }
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

    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }

    void STDMETHODCALLTYPE GetResource(ID3D11Resource **ppResource) override {
        if (ppResource) {
            *ppResource = m_resource.Get();
            if (m_resource) m_resource->AddRef();
        }
    }
    void STDMETHODCALLTYPE GetDesc(D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc) override {
        if (pDesc) *pDesc = m_desc;
    }

private:
    Microsoft::WRL::ComPtr<ID3D11Resource> m_resource;
    D3D11_DEPTH_STENCIL_VIEW_DESC          m_desc{};
};

// =============================================================================
// ProxyVertexShader: ID3D11VertexShader
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

    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
};

// =============================================================================
// ProxyPixelShader: ID3D11PixelShader
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

    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
};

// =============================================================================
// ProxyInputLayout: ID3D11InputLayout
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

    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
};

// =============================================================================
// ProxySamplerState: ID3D11SamplerState
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

    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }

    void STDMETHODCALLTYPE GetDesc(D3D11_SAMPLER_DESC *pDesc) override {
        if (pDesc) *pDesc = m_desc;
    }

private:
    D3D11_SAMPLER_DESC m_desc{};
};

// =============================================================================
// ProxyComputeShader: ID3D11ComputeShader
// =============================================================================
class ProxyComputeShader : public ID3D11ComputeShader, public ProxyResourceBase {
public:
    ProxyComputeShader(uint32_t res_id, ProxyD3D11Device* device)
        : ProxyResourceBase(res_id, device) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) || riid == __uuidof(ID3D11ComputeShader)) {
            *ppvObject = static_cast<ID3D11ComputeShader*>(this);
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

    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
};

// =============================================================================
// ProxyStageShader<I>: ID3D11GeometryShader / ID3D11HullShader / ID3D11DomainShader
// =============================================================================
template <typename Iface>
class ProxyStageShader : public Iface, public ProxyResourceBase {
public:
    ProxyStageShader(uint32_t res_id, ProxyD3D11Device* device)
        : ProxyResourceBase(res_id, device) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) || riid == __uuidof(Iface)) {
            *ppvObject = static_cast<Iface*>(this);
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

    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
};

// =============================================================================
// ProxyUnorderedAccessView: ID3D11UnorderedAccessView
// =============================================================================
class ProxyUnorderedAccessView : public ID3D11UnorderedAccessView, public ProxyResourceBase {
public:
    ProxyUnorderedAccessView(uint32_t res_id, ID3D11Resource* pRes, const D3D11_UNORDERED_ACCESS_VIEW_DESC* pDesc, ProxyD3D11Device* device)
        : ProxyResourceBase(res_id, device), m_resource(pRes)
    {
        if (pDesc) m_desc = *pDesc;
        else std::memset(&m_desc, 0, sizeof(m_desc));
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
            riid == __uuidof(ID3D11View) || riid == __uuidof(ID3D11UnorderedAccessView)) {
            *ppvObject = static_cast<ID3D11UnorderedAccessView*>(this);
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

    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }

    void STDMETHODCALLTYPE GetResource(ID3D11Resource **ppResource) override {
        if (ppResource) {
            *ppResource = m_resource.Get();
            if (m_resource) m_resource->AddRef();
        }
    }
    void STDMETHODCALLTYPE GetDesc(D3D11_UNORDERED_ACCESS_VIEW_DESC *pDesc) override {
        if (pDesc) *pDesc = m_desc;
    }

private:
    Microsoft::WRL::ComPtr<ID3D11Resource> m_resource;
    D3D11_UNORDERED_ACCESS_VIEW_DESC       m_desc{};
};

} // namespace RemoteGPU

#endif // REMOTE_GPU_PROXY_RESOURCES_H
