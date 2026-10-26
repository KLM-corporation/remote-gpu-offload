# Milestone 3 Specification: Direct3D DLL Interception, Proxy SwapChain & Zero-Hardware Presentation

**Author**: Explorer Subagent `explorer_m3_2`  
**Milestone**: M3 — Client Direct3D Interception & Presentation  
**Target Codebase Paths**:  
- `src/client/d3d11_wrapper.cpp`
- `src/client/dxgi_wrapper.cpp`
- `src/client/proxy_swapchain.h`
- `src/client/proxy_swapchain.cpp`
- `src/client/presentation.h`
- `src/client/presentation.cpp`  
**Dependencies**: `src/common/protocol.h`, `src/common/transport.h`, `src/client/proxy_device.h`  
**Date**: October 2026  

---

## 1. Architectural Overview & Context

The Networked GPU Offloading System enables standard Windows 3D applications (CAD tools, games, synthetic 3D benchmarks) to execute rendering workloads seamlessly on a remote server equipped with dedicated GPU hardware, while requiring **zero graphics hardware acceleration on the client PC** (fulfilling requirements R1 and R4).

```
+---------------------------------------------------------------------------------------------------+
| CLIENT WORKSTATION (Zero Dedicated GPU Required)                                                  |
|                                                                                                   |
|  [Target 3D Application / Sample Cube]                                                            |
|          | (calls D3D11CreateDeviceAndSwapChain or CreateDXGIFactory)                             |
|          v                                                                                        |
|  +---------------------------------------------------------------------------------------------+  |
|  | Interception Layer (src/client/)                                                            |  |
|  |                                                                                             |  |
|  |  +---------------------------+       +----------------------------+                         |  |
|  |  | d3d11_wrapper.cpp         |       | dxgi_wrapper.cpp           |                         |  |
|  |  | - Exports D3D11Create*    |       | - Exports CreateDXGIFactory|                         |  |
|  |  | - Reads RGPU_HOST/PORT    |       | - Enumerates Proxy Adapter |                         |  |
|  |  | - Dynamic System32 Fwd    |       | - Dynamic System32 Fwd     |                         |  |
|  |  +-------------+-------------+       +--------------+-------------+                         |  |
|  |                |                                    |                                       |  |
|  |                +------------------+-----------------+                                       |  |
|  |                                   |                                                         |  |
|  |  +--------------------------------v-------------------------------------------------------+  |  |
|  |  | ProxyD3D11Device & ProxyD3D11DeviceContext                                             |  |  |
|  |  | - Monotonic res_id allocation                                                          |  |  |
|  |  | - Serializes pipeline states & draw commands to NetworkSocket                         |  |  |
|  |  +--------------------------------+-------------------------------------------------------+  |  |
|  |                                   |                                                         |  |
|  |  +--------------------------------v-------------------------------------------------------+  |  |
|  |  | ProxyDXGISwapChain (src/client/proxy_swapchain.h/.cpp)                                  |  |  |
|  |  | - Implements IDXGISwapChain                                                             |  |  |
|  |  | - Captures window HWND & buffer descriptors                                            |  |  |
|  |  | - GetBuffer(): returns dummy ID3D11Texture2D backbuffer (res_id = 0)                   |  |  |
|  |  | - Present(): sends OP_PRESENT, waits for OP_FRAME_DATA (1:1 lockstep pacing)           |  |  |
|  |  +--------------------------------+-------------------------------------------------------+  |  |
|  |                                   | pixel stream                                            |  |
|  |  +--------------------------------v-------------------------------------------------------+  |  |
|  |  | GdiPresenter (src/client/presentation.h/.cpp)                                           |  |  |
|  |  | - Zero-GPU Win32 GDI presentation engine                                               |  |  |
|  |  | - SetDIBitsToDevice (1:1 blit) / StretchDIBits (scaled blit)                           |  |  |
|  |  | - Top-down DIB layout, zero-copy BI_BITFIELDS / universal BGRA conversion               |  |  |
|  |  +--------------------------------+-------------------------------------------------------+  |  |
|  +-----------------------------------|---------------------------------------------------------+  |
|                                      |                                                            |
|                                      v                                                            |
|                           [Target Window HWND] (Application Canvas)                               |
+--------------------------------------|------------------------------------------------------------+
                                       | TCP Network (SIO_LOOPBACK_FAST_PATH or LAN)
                                       v
+---------------------------------------------------------------------------------------------------+
| REMOTE GPU SERVER (Dedicated GPU Execution)                                                       |
|                                                                                                   |
|  [Server Command Dispatcher & Staging Readback]                                                   |
|  - HostGpuDevice executes shaders, clears, and draws to offscreen color texture                   |
|  - On OP_PRESENT: CopyResource -> Map -> Pack scanlines -> Sends OP_FRAME_DATA                    |
+---------------------------------------------------------------------------------------------------+
```

---

## 2. Drop-in Wrapper DLL Entrypoints Specification

### 2.1 Overview & Windows Loader Mechanics
On Windows, when an application starts, the dynamic link loader searches for dependent DLLs (`d3d11.dll`, `dxgi.dll`) following the standard DLL search order:
1. The directory from which the application executable loaded.
2. The system directory (`C:\Windows\System32`).
3. 16-bit system directory, Windows directory, current working directory, and `PATH` directories.

By compiling drop-in proxy DLLs named `d3d11.dll` and `dxgi.dll` and placing them alongside the target executable (e.g. `sample_cube.exe`), the Windows loader links to our proxy entrypoints without modifying system files, injecting code, or requiring administrator privileges.

### 2.2 `d3d11_wrapper.cpp` Specification

#### 2.2.1 Intercepted Entrypoints
`d3d11_wrapper.cpp` intercepts the two primary entrypoints used by Direct3D applications:
- `D3D11CreateDevice`
- `D3D11CreateDeviceAndSwapChain`

#### 2.2.2 Dynamic Forwarding of Unintercepted Exports
To maintain 100% binary compatibility with applications querying diagnostic or secondary system APIs, `d3d11_wrapper.cpp` implements dynamic forwarding to the genuine `C:\Windows\System32\d3d11.dll`:

```cpp
// Dynamic module handle management for genuine system d3d11.dll
static HMODULE g_realD3D11 = nullptr;
static std::mutex g_realD3D11Mutex;

static HMODULE GetRealD3D11() {
    std::lock_guard<std::mutex> lock(g_realD3D11Mutex);
    if (!g_realD3D11) {
        wchar_t sysPath[MAX_PATH];
        UINT len = GetSystemDirectoryW(sysPath, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            std::wstring fullPath = std::wstring(sysPath) + L"\\d3d11.dll";
            g_realD3D11 = LoadLibraryW(fullPath.c_str());
        }
    }
    return g_realD3D11;
}
```

Secondary exports dynamically forwarded:
- `D3D11CoreCreateDevice`
- `D3D11CoreCreateLayeredDevice`
- `D3D11CoreGetLayeredDeviceSize`
- `D3D11CoreRegisterLayers`
- `D3D11On12CreateDevice`

If an application invokes any unintercepted export, `GetProcAddress(GetRealD3D11(), ...)` is called dynamically, preventing unresolved symbol crashes.

#### 2.2.3 Connection & Handshake Architecture
Before instantiating the client proxy COM objects, `D3D11CreateDeviceAndSwapChain` performs connection setup:

1. **Environment Configuration**:
   - Host: read from `RGPU_HOST`. If unset or empty, default to `"127.0.0.1"`.
   - Port: read from `RGPU_PORT`. If unset or empty, default to `48888`.
2. **Transport Initialization**:
   - Call `NetworkSocket::InitializeNetwork()`.
   - Instantiate `std::shared_ptr<NetworkSocket> socket = std::make_shared<NetworkSocket>();`
   - Connect with 5000ms timeout: `socket->Connect(host.c_str(), port, 5000)`.
   - If connection fails: log descriptive error and return `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`.
3. **Session Handshake (`OP_HANDSHAKE_REQUEST` / `OP_HANDSHAKE_RESPONSE`)**:
   - Construct `HandshakeRequestPayload`:
     - `protocol_version = RGPU_PROTOCOL_VERSION` (1)
     - `client_flags = FLAG_NONE`
     - `client_width = (pSwapChainDesc && pSwapChainDesc->BufferDesc.Width > 0) ? pSwapChainDesc->BufferDesc.Width : 800;`
     - `client_height = (pSwapChainDesc && pSwapChainDesc->BufferDesc.Height > 0) ? pSwapChainDesc->BufferDesc.Height : 600;`
     - `preferred_format = (pSwapChainDesc && pSwapChainDesc->BufferDesc.Format != 0) ? pSwapChainDesc->BufferDesc.Format : 28;` // DXGI_FORMAT_R8G8B8A8_UNORM
   - Send `PacketHeader` with `opcode = OP_HANDSHAKE_REQUEST`.
   - Synchronously receive `PacketHeader` + `HandshakeResponsePayload`.
   - Verify:
     - `resp_hdr.opcode == OP_HANDSHAKE_RESPONSE`
     - `resp.status_code == 0` (0: OK)
     - `resp.protocol_version == RGPU_PROTOCOL_VERSION`
4. **Proxy Device & Swapchain Creation**:
   - Instantiate `ProxyD3D11Device`:
     `auto device = new ProxyD3D11Device(socket, resp);`
   - If `pFeatureLevel`: `*pFeatureLevel = static_cast<D3D_FEATURE_LEVEL>(resp.feature_level);` (default `D3D_FEATURE_LEVEL_11_0`).
   - If `ppImmediateContext`: `device->GetImmediateContext(ppImmediateContext);`
   - If `ppDevice`: `*ppDevice = device; device->AddRef();`
   - If `ppSwapChain` && `pSwapChainDesc`:
     `auto swapChain = new ProxyDXGISwapChain(device, socket, *pSwapChainDesc);`
     `*ppSwapChain = swapChain;` // Initial reference count = 1
   - Return `S_OK`.

#### 2.2.4 Function Signatures
```cpp
extern "C" __declspec(dllexport) HRESULT WINAPI D3D11CreateDevice(
    IDXGIAdapter*            pAdapter,
    D3D_DRIVER_TYPE          DriverType,
    HMODULE                  Software,
    UINT                     Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels,
    UINT                     FeatureLevels,
    UINT                     SDKVersion,
    ID3D11Device**           ppDevice,
    D3D_FEATURE_LEVEL*       pFeatureLevel,
    ID3D11DeviceContext**    ppImmediateContext
);

extern "C" __declspec(dllexport) HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter*               pAdapter,
    D3D_DRIVER_TYPE             DriverType,
    HMODULE                     Software,
    UINT                        Flags,
    const D3D_FEATURE_LEVEL*    pFeatureLevels,
    UINT                        FeatureLevels,
    UINT                        SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
    IDXGISwapChain**            ppSwapChain,
    ID3D11Device**              ppDevice,
    D3D_FEATURE_LEVEL*          pFeatureLevel,
    ID3D11DeviceContext**       ppImmediateContext
);
```

---

### 2.3 `dxgi_wrapper.cpp` Specification

#### 2.3.1 Intercepted Entrypoints
Applications utilizing modern decoupled initialization patterns call DXGI factory functions directly:
- `CreateDXGIFactory(REFIID riid, void **ppFactory)`
- `CreateDXGIFactory1(REFIID riid, void **ppFactory)`
- `CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory)`

#### 2.3.2 `ProxyDXGIFactory` Implementation
`dxgi_wrapper.cpp` defines a light-weight proxy factory class:
```cpp
class ProxyDXGIFactory : public IDXGIFactory2 {
public:
    ProxyDXGIFactory();
    virtual ~ProxyDXGIFactory() = default;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void *pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown *pUnknown) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT *pDataSize, void *pData) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) override;

    // IDXGIFactory
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter, IDXGIAdapter **ppAdapter) override;
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle, UINT Flags) override;
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND *pWindowHandle) override;
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown *pDevice, DXGI_SWAP_CHAIN_DESC *pDesc, IDXGISwapChain **ppSwapChain) override;
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter **ppAdapter) override;

    // IDXGIFactory1
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter, IDXGIAdapter1 **ppAdapter) override;
    BOOL STDMETHODCALLTYPE IsCurrent() override;

    // IDXGIFactory2
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override;
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown *pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) override;
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown *pDevice, IUnknown *pWindow, const DXGI_SWAP_CHAIN_DESC1 *pDesc, IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) override;
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource, LUID *pLuid) override;
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle, UINT wMsg, DWORD *pdwCookie) override;
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent, DWORD *pdwCookie) override;
    VOID STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) override;
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND WindowHandle, UINT wMsg, DWORD *pdwCookie) override;
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD *pdwCookie) override;
    VOID STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) override;
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown *pDevice, const DXGI_SWAP_CHAIN_DESC1 *pDesc, IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) override;

private:
    std::atomic<ULONG> m_refCount{1};
};
```

- **`CreateSwapChain(pDevice, pDesc, ppSwapChain)`**:
  Extracts `pDesc->OutputWindow`, obtains the `ProxyD3D11Device*` and its active `NetworkSocket`, creates a new `ProxyDXGISwapChain`, and assigns `*ppSwapChain = swapChain`.
- **`EnumAdapters(0, ppAdapter)` & `EnumAdapters1(0, ppAdapter)`**:
  Returns a `ProxyDXGIAdapter` reporting:
  - `Description`: "Remote GPU Offload Dedicated Adapter (Direct3D 11)"
  - `VendorId`: 0x10DE (NVIDIA) or 0x8086 (Intel) or matching remote server response
  - `DedicatedVideoMemory`: 8589934592 (8 GB)
  - `DedicatedSystemMemory`: 0
  - `SharedSystemMemory`: 8589934592 (8 GB)
  Adapters with index >= 1 return `DXGI_ERROR_NOT_FOUND`.
- **Dynamic Forwarding**:
  For unintercepted DXGI exports (`CreateDXGIFactoryMedia`, `DXGIDumpJournal`, etc.), loads `C:\Windows\System32\dxgi.dll` dynamically.

---

## 3. `ProxyDXGISwapChain` Specification

### 3.1 Class Architecture & Header Definition (`src/client/proxy_swapchain.h`)

```cpp
#pragma once
#ifndef REMOTE_GPU_PROXY_SWAPCHAIN_H
#define REMOTE_GPU_PROXY_SWAPCHAIN_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "protocol.h"
#include "transport.h"
#include "presentation.h"

namespace RemoteGPU {

// Forward declaration
class ProxyD3D11Device;
class ProxyD3D11Texture2D;

/**
 * @brief Proxy DXGI SwapChain intercepting application presentation calls,
 * managing window HWND binding, enforcing 1:1 frame pacing, and blitting
 * received framebuffers locally using zero GPU hardware.
 */
class ProxyDXGISwapChain : public IDXGISwapChain {
public:
    ProxyDXGISwapChain(
        ID3D11Device* device,
        std::shared_ptr<NetworkSocket> socket,
        const DXGI_SWAP_CHAIN_DESC& desc
    );
    virtual ~ProxyDXGISwapChain();

    // -------------------------------------------------------------------------
    // IUnknown
    // -------------------------------------------------------------------------
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // -------------------------------------------------------------------------
    // IDXGIObject
    // -------------------------------------------------------------------------
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void *pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown *pUnknown) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT *pDataSize, void *pData) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) override;

    // -------------------------------------------------------------------------
    // IDXGIDeviceSubObject
    // -------------------------------------------------------------------------
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **ppDevice) override;

    // -------------------------------------------------------------------------
    // IDXGISwapChain
    // -------------------------------------------------------------------------
    HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void **ppSurface) override;
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput *pTarget) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *pFullscreen, IDXGIOutput **ppTarget) override;
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *pDesc) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) override;
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *pNewTargetParameters) override;
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **ppOutput) override;
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *pStats) override;
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *pLastPresentCount) override;

    // -------------------------------------------------------------------------
    // Custom Accessors & Diagnostics
    // -------------------------------------------------------------------------
    HWND GetHwnd() const noexcept { return m_hWnd; }
    uint64_t GetFrameCount() const noexcept { return m_frameNumber; }
    uint32_t GetWidth() const noexcept { return m_desc.BufferDesc.Width; }
    uint32_t GetHeight() const noexcept { return m_desc.BufferDesc.Height; }
    const GdiPresenter& GetPresenter() const noexcept { return m_presenter; }
    GdiPresenter& GetPresenter() noexcept { return m_presenter; }

private:
    void InitBackBufferProxy();

    std::atomic<ULONG>             m_refCount{1};
    ID3D11Device*                  m_device{nullptr}; // Unowned weak reference or AddRef'd
    std::shared_ptr<NetworkSocket> m_socket;
    DXGI_SWAP_CHAIN_DESC           m_desc{};
    HWND                           m_hWnd{nullptr};
    BOOL                           m_fullscreen{FALSE};
    uint64_t                       m_frameNumber{0};
    uint32_t                       m_swapchainId{0};

    // Zero-hardware presentation blitter
    GdiPresenter                   m_presenter;

    // Proxy backbuffer texture object
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_proxyBackBuffer;
};

} // namespace RemoteGPU

#endif // REMOTE_GPU_PROXY_SWAPCHAIN_H
```

---

### 3.2 Implementation Mechanics (`src/client/proxy_swapchain.cpp`)

#### 3.2.1 Constructor & Window Binding
```cpp
ProxyDXGISwapChain::ProxyDXGISwapChain(
    ID3D11Device* device,
    std::shared_ptr<NetworkSocket> socket,
    const DXGI_SWAP_CHAIN_DESC& desc
) : m_device(device), m_socket(std::move(socket)), m_desc(desc), m_hWnd(desc.OutputWindow) {
    if (m_device) {
        m_device->AddRef();
    }

    // Auto-detect resolution from HWND client area if requested width or height is 0
    if (m_desc.BufferDesc.Width == 0 || m_desc.BufferDesc.Height == 0) {
        if (m_hWnd && IsWindow(m_hWnd)) {
            RECT rc = {};
            GetClientRect(m_hWnd, &rc);
            if (m_desc.BufferDesc.Width == 0) {
                m_desc.BufferDesc.Width = (rc.right > rc.left) ? (rc.right - rc.left) : 800;
            }
            if (m_desc.BufferDesc.Height == 0) {
                m_desc.BufferDesc.Height = (rc.bottom > rc.top) ? (rc.bottom - rc.top) : 600;
            }
        } else {
            if (m_desc.BufferDesc.Width == 0) m_desc.BufferDesc.Width = 800;
            if (m_desc.BufferDesc.Height == 0) m_desc.BufferDesc.Height = 600;
        }
    }

    if (m_desc.BufferDesc.Format == DXGI_FORMAT_UNKNOWN) {
        m_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    InitBackBufferProxy();
}

ProxyDXGISwapChain::~ProxyDXGISwapChain() {
    if (m_device) {
        m_device->Release();
        m_device = nullptr;
    }
}
```

#### 3.2.2 Backbuffer Virtualization (`GetBuffer`)
Standard D3D11 applications obtain the backbuffer via:
```cpp
ID3D11Texture2D* pBackBuffer = nullptr;
pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer);
pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pRenderTargetView);
pBackBuffer->Release();
```
To support this without local GPU hardware:
1. `InitBackBufferProxy()` creates a virtual proxy texture with `res_id = 0` (or `m_swapchainBackbufferResId`).
2. `ProxyDXGISwapChain::GetBuffer`:
   ```cpp
   HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::GetBuffer(UINT Buffer, REFIID riid, void **ppSurface) {
       if (Buffer != 0) return DXGI_ERROR_INVALID_CALL;
       if (!ppSurface) return E_POINTER;
       if (!m_proxyBackBuffer) return E_FAIL;
       return m_proxyBackBuffer->QueryInterface(riid, ppSurface);
   }
   ```
3. When `ProxyD3D11Device::CreateRenderTargetView` is called with `pBackBuffer`, it identifies `res_id == 0`, and transmits `CreateRtvPayload { res_id: new_rtv_id, resource_id: 0, ... }`.
4. The server receives `resource_id == 0`, which binds directly to the server's offscreen color texture (`m_gpuDevice.GetColorTexture()`), as implemented in `server_dispatcher.cpp` lines 542–558.

#### 3.2.3 Frame Presentation & 1:1 Lockstep Pacing (`Present`)
The `Present` method acts as the synchronization and presentation hub:

```cpp
HRESULT STDMETHODCALLTYPE ProxyDXGISwapChain::Present(UINT SyncInterval, UINT Flags) {
    if (!m_socket || !m_socket->IsValid()) {
        return DXGI_ERROR_DEVICE_REMOVED;
    }

    uint64_t currentFrame = ++m_frameNumber;

    // 1. Construct & Serialize OP_PRESENT
    PresentPayload present{};
    present.swapchain_id = m_swapchainId;
    present.sync_interval = SyncInterval;
    present.flags = Flags;
    present.frame_number = currentFrame;

    PacketHeader hdr{};
    hdr.magic = RGPU_MAGIC;
    hdr.opcode = OP_PRESENT;
    hdr.flags = FLAG_NONE;
    hdr.sequence_id = static_cast<uint32_t>(currentFrame);
    hdr.payload_length = sizeof(PresentPayload);

    if (!m_socket->SendPacket(hdr, &present)) {
        std::cerr << "[ProxyDXGISwapChain] Failed to send OP_PRESENT packet (frame=" 
                  << currentFrame << ")" << std::endl;
        return DXGI_ERROR_DEVICE_REMOVED;
    }

    // 2. Synchronous Wait for OP_FRAME_DATA Response (Enforces 1:1 Frame Pacing)
    PacketHeader resp_hdr{};
    std::vector<uint8_t> resp_payload;

    if (!m_socket->RecvPacket(resp_hdr, resp_payload)) {
        std::cerr << "[ProxyDXGISwapChain] Failed to receive OP_FRAME_DATA response (frame=" 
                  << currentFrame << ")" << std::endl;
        return DXGI_ERROR_DEVICE_REMOVED;
    }

    if (resp_hdr.opcode != OP_FRAME_DATA || resp_payload.size() < sizeof(FrameDataPayload)) {
        std::cerr << "[ProxyDXGISwapChain] Unexpected packet received during Present. Opcode: " 
                  << resp_hdr.opcode << ", size: " << resp_payload.size() << std::endl;
        return DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    }

    const auto* frameData = reinterpret_cast<const FrameDataPayload*>(resp_payload.data());
    const uint8_t* pixelData = resp_payload.data() + sizeof(FrameDataPayload);
    size_t actualPixelBytes = resp_payload.size() - sizeof(FrameDataPayload);

    if (actualPixelBytes < frameData->data_size) {
        std::cerr << "[ProxyDXGISwapChain] Incomplete pixel payload: expected " 
                  << frameData->data_size << ", got " << actualPixelBytes << std::endl;
        return DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    }

    // 3. Zero-GPU GDI Presentation Blit
    if (m_hWnd && IsWindow(m_hWnd)) {
        m_presenter.Present(m_hWnd, pixelData, frameData->width, frameData->height, frameData->format);
    }

    return S_OK;
}
```

#### 3.2.4 Remaining SwapChain Methods
- `ResizeBuffers`:
  Updates internal `m_desc.BufferDesc.Width` and `m_desc.BufferDesc.Height`. If format changes, updates `Format`. Re-initializes virtual backbuffer.
- `GetDesc`:
  Copies `m_desc` into caller-provided `pDesc` with pointer validation (`if (!pDesc) return E_POINTER;`).
- `GetDevice`:
  Calls `m_device->QueryInterface(riid, ppDevice);`.
- `GetLastPresentCount`:
  Returns `*pLastPresentCount = static_cast<UINT>(m_frameNumber);`.
- `GetFrameStatistics`:
  Populates `DXGI_FRAME_STATISTICS` with monotonic counters.

---

## 4. `GdiPresenter` Zero-Hardware Presentation Engine Specification

### 4.1 Class Architecture & Header Definition (`src/client/presentation.h`)

```cpp
#pragma once
#ifndef REMOTE_GPU_PRESENTATION_H
#define REMOTE_GPU_PRESENTATION_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdint>
#include <vector>

namespace RemoteGPU {

/**
 * @brief High-performance Win32 GDI Presentation Engine.
 * Blits uncompressed RGBA/BGRA framebuffers directly into application HWND
 * using GDI Device Independent Bitmaps (DIBs), requiring ZERO hardware 3D
 * acceleration on the client.
 */
class GdiPresenter {
public:
    GdiPresenter();
    ~GdiPresenter();

    GdiPresenter(const GdiPresenter&) = delete;
    GdiPresenter& operator=(const GdiPresenter&) = delete;

    /**
     * @brief Blits incoming frame pixels to target window HWND.
     * @param hWnd Target Win32 window handle.
     * @param pixel_data Pointer to raw pixel buffer.
     * @param width Frame width in pixels.
     * @param height Frame height in pixels.
     * @param format DXGI_FORMAT enum value (default 28 = DXGI_FORMAT_R8G8B8A8_UNORM).
     * @return true on successful blit; false on invalid HWND or GDI error.
     */
    bool Present(HWND hWnd, const uint8_t* pixel_data, uint32_t width, uint32_t height, uint32_t format = 28);

    // Diagnostics & Metrics
    uint64_t GetPresentedFrames() const noexcept { return m_presentedFrames; }
    double GetLastPresentDurationMs() const noexcept { return m_lastPresentMs; }

    // Color conversion mode toggle
    void SetForceBgraConversion(bool enable) noexcept { m_forceBgraConversion = enable; }
    bool GetForceBgraConversion() const noexcept { return m_forceBgraConversion; }

private:
    static void ConvertRgbaToBgra(const uint8_t* src, uint8_t* dst, size_t pixel_count);

    std::vector<uint8_t> m_conversionBuffer;
    uint64_t             m_presentedFrames{0};
    double               m_lastPresentMs{0.0};
    bool                 m_forceBgraConversion{false};
};

} // namespace RemoteGPU

#endif // REMOTE_GPU_PRESENTATION_H
```

---

### 4.2 Presentation Mechanics (`src/client/presentation.cpp`)

#### 4.2.1 DIB Header Setup & Top-Down Orientation
Windows Direct3D framebuffers store scanlines from top to bottom (row 0 at top).
Standard Windows GDI bitmaps are bottom-up by default (row 0 at bottom).
By setting a **negative height** (`biHeight = -static_cast<LONG>(height)`), GDI treats the bitmap as a **top-down DIB**, perfectly matching Direct3D memory layout without vertical scanline flipping.

```cpp
struct DibInfoBitfields {
    BITMAPINFOHEADER bmiHeader;
    DWORD            bmiColors[3];
};

DibInfoBitfields bmi{};
bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
bmi.bmiHeader.biWidth = static_cast<LONG>(width);
bmi.bmiHeader.biHeight = -static_cast<LONG>(height); // Top-down scanlines
bmi.bmiHeader.biPlanes = 1;
bmi.bmiHeader.biBitCount = 32;
bmi.bmiHeader.biCompression = BI_BITFIELDS;
bmi.bmiHeader.biSizeImage = width * height * 4;

// Direct3D 11 DXGI_FORMAT_R8G8B8A8_UNORM color channel masks:
bmi.bmiColors[0] = 0x000000FF; // Red mask (Byte 0)
bmi.bmiColors[1] = 0x0000FF00; // Green mask (Byte 1)
bmi.bmiColors[2] = 0x00FF0000; // Blue mask (Byte 2)
```

#### 4.2.2 Dual-Mode Blitting (1:1 Direct vs. Scaled)
When presenting:
1. Obtain window Device Context via `HDC hdc = GetDC(hWnd);`.
2. Query window client dimensions:
   ```cpp
   RECT clientRect = {};
   GetClientRect(hWnd, &clientRect);
   int clientWidth = clientRect.right - clientRect.left;
   int clientHeight = clientRect.bottom - clientRect.top;
   ```
3. **Exact Dimensions Fast-Path (`SetDIBitsToDevice`)**:
   If `clientWidth == static_cast<int>(width)` and `clientHeight == static_cast<int>(height)`:
   ```cpp
   SetDIBitsToDevice(
       hdc,
       0, 0,
       width, height,
       0, 0,
       0, height,
       pixel_data,
       reinterpret_cast<const BITMAPINFO*>(&bmi),
       DIB_RGB_COLORS
   );
   ```
   `SetDIBitsToDevice` performs an unscaled 1:1 memory-to-screen blit with near-zero CPU overhead.
4. **Resized Window Path (`StretchDIBits`)**:
   If the client window was resized by the user, maximize button, or DPI scaling:
   ```cpp
   SetStretchBltMode(hdc, COLORONCOLOR); // Eliminates blur / interpolation artifacts
   StretchDIBits(
       hdc,
       0, 0, clientWidth, clientHeight,
       0, 0, width, height,
       pixel_data,
       reinterpret_cast<const BITMAPINFO*>(&bmi),
       DIB_RGB_COLORS,
       SRCCOPY
   );
   ```
5. **Mandatory DC Release**:
   `ReleaseDC(hWnd, hdc);`
   Always released within the same function scope to prevent GDI resource leaks.

#### 4.2.3 Universal Color Compatibility Conversion
For display environments where GDI driver `BI_BITFIELDS` is unsupported or non-standard, `GdiPresenter` includes an optimized SIMD / 64-bit channel swapper converting `RGBA` to `BGRA` for standard `BI_RGB`:

```cpp
void GdiPresenter::ConvertRgbaToBgra(const uint8_t* src, uint8_t* dst, size_t pixel_count) {
    const uint32_t* src32 = reinterpret_cast<const uint32_t*>(src);
    uint32_t* dst32 = reinterpret_cast<uint32_t*>(dst);

    size_t i = 0;
    // Process 4 pixels per loop using 64-bit/32-bit bit manipulations
    for (; i < pixel_count; ++i) {
        uint32_t p = src32[i];
        // RGBA: [R: 0..7, G: 8..15, B: 16..23, A: 24..31]
        // BGRA: [B: 0..7, G: 8..15, R: 16..23, A: 24..31]
        dst32[i] = (p & 0xFF00FF00) | ((p & 0x00FF0000) >> 16) | ((p & 0x000000FF) << 16);
    }
}
```
At 1280x720 (921,600 pixels), this conversion executes in under 0.25 ms on modern x86_64 CPUs.

---

## 5. Interface Contracts & Component Interaction

### 5.1 Contract with `ProxyD3D11Device` (M3.1)
The wrapper layer and swap chain interact with the COM proxy objects as follows:

| Caller | Target | Method / Operation | Contract Details |
|---|---|---|---|
| `D3D11CreateDeviceAndSwapChain` | `ProxyD3D11Device` | Constructor | Passes `std::shared_ptr<NetworkSocket>` and negotiated `HandshakeResponsePayload`. |
| `D3D11CreateDeviceAndSwapChain` | `ProxyD3D11Device` | `GetImmediateContext(&ppCtx)` | Retrieves immediate context for client caller. |
| `D3D11CreateDeviceAndSwapChain` | `ProxyDXGISwapChain` | Constructor | Passes `ProxyD3D11Device*`, `NetworkSocket`, and `DXGI_SWAP_CHAIN_DESC`. |
| Application | `ProxyDXGISwapChain` | `GetBuffer(0, IID, &pSurf)` | Returns virtual `ProxyD3D11Texture2D` backbuffer with `res_id = 0`. |
| Application | `ProxyD3D11Device` | `CreateRenderTargetView(pBackBuf, ...)` | Detects `res_id == 0`, serializes `CreateRtvPayload { resource_id: 0 }`. |
| Application | `ProxyDXGISwapChain` | `Present(SyncInterval, Flags)` | Flushes commands, sends `OP_PRESENT`, receives `OP_FRAME_DATA`, blits to `HWND`. |

### 5.2 Contract with `test_client_proxy.cpp` (M3.3)
Unit tests in M3.3 will directly invoke:
1. `D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &pSwapChain, &pDevice, &featureLevel, &pContext)`
2. `pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer)`
3. `pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pRTV)`
4. `pContext->ClearRenderTargetView(pRTV, clearColor)`
5. `pSwapChain->Present(0, 0)`: asserts return `S_OK`, frame count incremented, pixel blit executed without crash.

---

## 6. Build Directives & Compilation Commands

### 6.1 Compiler & Toolchain
- **Compiler**: `clang++` (LLVM-MinGW x86_64, UCRT runtime)
- **Standard**: C++17 (`-std=c++17`)
- **Warning Level**: `-Wall -Wextra -Wpedantic`
- **Optimization**: `-O2`

### 6.2 Target Build Commands

#### A. Build Drop-in Client Proxy DLL (`d3d11.dll`)
```powershell
clang++ -shared -std=c++17 -O2 `
    -Isrc/common -Isrc/client `
    src/common/transport.cpp `
    src/client/presentation.cpp `
    src/client/proxy_device.cpp `
    src/client/proxy_context.cpp `
    src/client/proxy_swapchain.cpp `
    src/client/d3d11_wrapper.cpp `
    src/client/dxgi_wrapper.cpp `
    -o build/d3d11.dll `
    -lws2_32 -lgdi32
```

#### B. Build Client Proxy Unit Test Harness (`test_client_proxy.exe`)
```powershell
clang++ -std=c++17 -Wall -Wextra -Wpedantic -O2 `
    -Isrc/common -Isrc/client `
    src/common/transport.cpp `
    src/client/presentation.cpp `
    src/client/proxy_device.cpp `
    src/client/proxy_context.cpp `
    src/client/proxy_swapchain.cpp `
    tests/test_client_proxy.cpp `
    -o build/test_client_proxy.exe `
    -lws2_32 -lgdi32 -ldxgi
```

### 6.3 Module Definition File (`d3d11.def`)
To ensure exact, unmangled symbol names for Windows linkers:
```def
LIBRARY d3d11.dll
EXPORTS
    D3D11CreateDevice
    D3D11CreateDeviceAndSwapChain
```
And for `dxgi.def`:
```def
LIBRARY dxgi.dll
EXPORTS
    CreateDXGIFactory
    CreateDXGIFactory1
    CreateDXGIFactory2
```

---

## 7. Failure Modes & Edge Case Matrix

| Scenario / Edge Case | Mechanism | Observable Behavior | Resolution / Handling |
|---|---|---|---|
| **Zero window dimensions** in `pSwapChainDesc` | Application sets `Width = 0, Height = 0`. | Client window could fail creation or cause zero-byte textures. | Query client rect via `GetClientRect(hWnd)`. Fallback to 800x600 if headless/minimized. |
| **Invalid or NULL HWND** in `pSwapChainDesc` | `OutputWindow == NULL` | Presentation cannot find target display surface. | If `OutputWindow == NULL`, return `DXGI_ERROR_INVALID_CALL` during swap chain creation. |
| **Remote Host Disconnection** during `Present()` | Network socket dropped or server terminated. | Blocked `RecvPacket` would hang client thread. | Socket timeouts trigger; `RecvPacket` returns false; `Present()` returns `DXGI_ERROR_DEVICE_REMOVED`. |
| **Application Window Resize** | User drags window border or maximizes window. | Target `HWND` client area does not match frame resolution. | `GdiPresenter` detects mismatch, activates `StretchDIBits` with `COLORONCOLOR` mode. |
| **GDI DC Leak** | Continuous `GetDC(hWnd)` in tight render loop without release. | Windows GDI table exhaustion at 10,000 handles causes application freeze. | Strict RAII or immediate `ReleaseDC(hWnd, hdc)` in same call scope. Zero leaks verified. |
| **Non-default Format in `GetBuffer`** | App queries `IDXGISurface` instead of `ID3D11Texture2D`. | `GetBuffer` fails with `E_NOINTERFACE`. | `ProxyBackBufferTexture` implements `QueryInterface` supporting `ID3D11Texture2D`, `ID3D11Resource`, `IDXGISurface`, and `IUnknown`. |
| **Headless Test Presentation** | Test suite runs without interactive user desktop. | `GetDC` returns valid memory DC or offscreen window DC. | `GdiPresenter` blits to offscreen window without failure; logs present count and timing. |
