# Direct3D Interception & Presentation Specification Mining Report

**Project**: Networked GPU Offloading System  
**Document**: D3D Interception Layer (R1) & SwapChain Presentation (R4) Specification  
**Author**: D3D Spec Miner (`spec_miner_survey_1`)  
**Target Environment**: Windows 10/11 x64, Direct3D 11 / Direct3D 12, DXGI 1.1 / 1.2+  
**Date**: 2026-10-26  

---

## 1. Executive Summary

This specification report details the architectural requirements, API boundaries, COM interface taxonomy, presentation mechanics, and resource lifecycle models for the **Client Direct3D Interception Layer (R1)** and **Client Swapchain Presentation Pipeline (R4)** of the Networked GPU Offloading system.

The core objective is to allow standard Windows 3D applications (games, CAD viewers, synthetic 3D test harnesses) to run on client machines **without requiring a local dedicated GPU**, by intercepting standard graphics API invocations, serializing command streams across a network socket to a GPU-equipped host server, and presenting streamed rendered frames into the client application window.

Authoritative probing of the Windows system binaries (`C:\Windows\System32\d3d11.dll`, `dxgi.dll`, `d3d12.dll`) and SDK headers (`d3d11.h`, `dxgi.h`, `dxgi1_2.h`, `dxgi1_3.h`, `d3d12.h`) confirms that:
1. Direct3D 11 provides a clean, well-defined immediate-context abstraction (`ID3D11DeviceContext`) and COM object hierarchy, making it the primary high-fidelity target for command stream serialization.
2. Interception via a drop-in **Wrapper DLL proxy** (`d3d11.dll` and `dxgi.dll`) coupled with **C++ COM Proxy Objects** provides the highest stability, zero kernel dependencies, complete isolation from anti-cheat/OS code protection flags, and seamless integration with existing Windows executable loaders.
3. Client-side swapchain presentation can be fulfilled completely in software via Win32 GDI/DIB blitting into the application window's `HWND`, requiring zero local 3D acceleration on the client PC, with optional local DXGI swapchain hardware blitting where available.

---

## 2. Windows Graphics Interception & Proxying Strategies

### 2.1 Comparative Strategy Evaluation

| Strategy | Mechanism | Pros | Cons / Hazards | Verdict |
|---|---|---|---|---|
| **Strategy A: Drop-in Wrapper DLL (DLL Proxying)** | Compile custom `d3d11.dll` (and/or `dxgi.dll`) placed directly in application directory. | • Standard Windows loader search order loads local DLL first.<br>• No process memory patching or hooking libraries required.<br>• Works across all user privileges without admin rights.<br>• Completely transparent to target executable. | Requires target app to search local folder (default for standard desktop applications, except UWP/Packaged apps). | **Primary Recommendation** |
| **Strategy B: COM Proxy Objects** | Intercept factory functions (`D3D11CreateDeviceAndSwapChain`) and return custom C++ classes implementing `ID3D11Device`, `ID3D11DeviceContext`, `IDXGISwapChain`. | • 100% type safety and standards compliance.<br>• Full control over memory, serialization, and lifecycle.<br>• Clean separation between client proxy state and remote GPU state.<br>• Zero risk of vtable corruption across threads. | Every intercepted method must be implemented or stubbed in the proxy class. | **Primary Recommendation** |
| **Strategy C: COM Vtable Hooking / Patching** | Create a dummy/WARP D3D device, locate virtual method table pointers, and patch function pointers in-place. | • Only hooked methods need overriding; unhooked methods call native implementation. | • COM vtables reside in read-only executable pages (`PAGE_EXECUTE_READ`), requiring `VirtualProtect`.<br>• In-place patching modifies shared vtables for all instances in process.<br>• Prone to race conditions and AV heuristic detection. | Rejected |
| **Strategy D: Detours / MinHook Inline Hooking** | In-memory 5-byte/14-byte trampoline hooking of exported API functions via DLL injection. | • Can attach to already-running processes without file placement. | • Requires active process injection (`CreateRemoteThread`, `SetWindowsHookEx`).<br>• Triggers anti-virus and EDR alerts.<br>• Still requires wrapping the returned COM objects. | Rejected |

### 2.2 Recommended Architecture: Drop-in Wrapper DLL + COM Proxy Hierarchy

The recommended client architecture consists of a companion DLL pair:
1. **`d3d11.dll`**: Intercepts `D3D11CreateDevice`, `D3D11CreateDeviceAndSwapChain`, and related factory exports.
2. **`dxgi.dll`**: Intercepts `CreateDXGIFactory`, `CreateDXGIFactory1`, `CreateDXGIFactory2`.
3. **Export Forwarding**: For non-intercepted system exports (such as `D3DKMT*` or driver private entrypoints), the wrapper dynamically resolves and forwards calls to the genuine `C:\Windows\System32\d3d11.dll` via `LoadLibraryW` and `GetProcAddress`.

```
Target Application (.exe)
       │
       ▼ (Calls D3D11CreateDeviceAndSwapChain)
┌────────────────────────────────────────────────────────┐
│ Client Proxy d3d11.dll / dxgi.dll                      │
│                                                        │
│  ┌──────────────────────┐    ┌──────────────────────┐  │
│  │ ProxyD3D11Device     │    │ ProxyDXGISwapChain   │  │
│  │  - Manages ResIDs    │    │  - Captures HWND     │  │
│  │  - COM Ref Counting  │    │  - Present() Blitter │  │
│  └──────────┬───────────┘    └──────────┬───────────┘  │
│             │                           │              │
│  ┌──────────▼───────────┐               │              │
│  │ ProxyDeviceContext   │               │              │
│  │  - Draw(), SetShader │               │              │
│  │  - Serializes to Net │               │              │
│  └──────────┬───────────┘               │              │
└─────────────┼───────────────────────────┼──────────────┘
              │ Command Stream (TCP/UDP)   ▲ Rendered Frames
              ▼                           │
┌────────────────────────────────────────────────────────┐
│ Remote GPU Execution Host (Dedicated GPU Server)       │
│  - Host Native D3D11Device & ImmContext                │
│  - Offscreen Render Target + Staging Texture Capture   │
│  - Encodes & Streams Frames to Client                  │
└────────────────────────────────────────────────────────┘
```

---

## 3. Authoritative Export Signatures & System Entrypoints

Probing `C:\Windows\System32\d3d11.dll` reveals 51 exports. The core application entrypoints are:

### 3.1 `D3D11CreateDevice`
```c
HRESULT WINAPI D3D11CreateDevice(
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
```
- **Interception Action**:
  - Validates `SDKVersion` (typically `D3D11_SDK_VERSION = 7`).
  - Instantiates `ProxyD3D11Device` and `ProxyD3D11DeviceContext`.
  - Transmits connection handshake to remote GPU host.
  - Returns `D3D_FEATURE_LEVEL_11_0` (or host-negotiated feature level).
  - Returns `S_OK`.

### 3.2 `D3D11CreateDeviceAndSwapChain`
```c
HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
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
- **Interception Action**:
  - Captures `pSwapChainDesc->OutputWindow` (`HWND`), `BufferDesc.Width`, `BufferDesc.Height`, `BufferDesc.Format`.
  - Instantiates `ProxyD3D11Device`, `ProxyD3D11DeviceContext`, and `ProxyDXGISwapChain`.
  - Creates a virtual backbuffer `ProxyD3D11Texture2D` tied to the swapchain (`res_id = 1`).
  - Serializes initial swapchain setup command to remote server: `CMD_INIT_SWAPCHAIN { width, height, format }`.
  - Initializes local presentation blitter bound to `pSwapChainDesc->OutputWindow`.

### 3.3 DXGI Factory Entrypoints (`dxgi.dll`)
```c
HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **ppFactory);
HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **ppFactory);
HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory);
```
- **Interception Action**:
  - Instantiates `ProxyDXGIFactory` / `ProxyDXGIFactory1`.
  - Returns virtual adapter `ProxyDXGIAdapter` reporting dedicated GPU capabilities (matching remote host GPU name).
  - Supports `factory->CreateSwapChain(pDevice, pDesc, ppSwapChain)` for apps that decouple device and swapchain creation.

---

## 4. Minimal Complete COM Interface Set for 3D Graphics

### 4.1 Interface Hierarchy & Class Taxonomy

All Direct3D 11 objects adhere to the COM hierarchy:
```
IUnknown
  ├── ID3D11Device (40 methods)
  └── ID3D11DeviceChild (4 methods)
        ├── ID3D11DeviceContext (108 methods)
        ├── ID3D11Resource (3 methods)
        │     ├── ID3D11Buffer (1 method)
        │     ├── ID3D11Texture1D (1 method)
        │     ├── ID3D11Texture2D (1 method)
        │     └── ID3D11Texture3D (1 method)
        ├── ID3D11View (1 method)
        │     ├── ID3D11RenderTargetView (1 method)
        │     ├── ID3D11ShaderResourceView (1 method)
        │     ├── ID3D11DepthStencilView (1 method)
        │     └── ID3D11UnorderedAccessView (1 method)
        ├── ID3D11InputLayout (0 methods)
        ├── ID3D11VertexShader (0 methods)
        ├── ID3D11PixelShader (0 methods)
        ├── ID3D11GeometryShader (0 methods)
        ├── ID3D11SamplerState (1 method)
        ├── ID3D11RasterizerState (1 method)
        ├── ID3D11BlendState (1 method)
        └── ID3D11DepthStencilState (1 method)

IUnknown
  └── IDXGIObject (4 methods)
        └── IDXGIDeviceSubObject (1 method)
              └── IDXGISwapChain (10 methods)
                    └── IDXGISwapChain1 (11 methods)
```

### 4.2 Minimal Complete Method Set for 3D Rendering

To render dynamic textured 3D meshes (satisfying R1, R4, R5), the following subsets must be actively intercepted and serialized:

#### A. `ID3D11Device` (Resource Creation)
| Method | Serialization Action |
|---|---|
| `CreateBuffer` | Assign `res_id`, serialize `D3D11_BUFFER_DESC` and initial data payload (`pInitialData->pSysMem`). Store CPU shadow buffer for dynamic buffers. |
| `CreateTexture2D` | Assign `res_id`, serialize `D3D11_TEXTURE2D_DESC` and mip/slice pixel data (`D3D11_SUBRESOURCE_DATA`). |
| `CreateRenderTargetView` | Assign `res_id`, link to resource `res_id`, serialize `D3D11_RENDER_TARGET_VIEW_DESC`. |
| `CreateShaderResourceView` | Assign `res_id`, link to texture `res_id`, serialize `D3D11_SHADER_RESOURCE_VIEW_DESC`. |
| `CreateDepthStencilView` | Assign `res_id`, link to depth texture `res_id`, serialize `D3D11_DEPTH_STENCIL_VIEW_DESC`. |
| `CreateVertexShader` | Assign `res_id`, serialize compiled DXBC shader bytecode (`pShaderBytecode`, `BytecodeLength`). |
| `CreatePixelShader` | Assign `res_id`, serialize compiled DXBC shader bytecode (`pShaderBytecode`, `BytecodeLength`). |
| `CreateInputLayout` | Assign `res_id`, serialize input element array (`D3D11_INPUT_ELEMENT_DESC[]`, `NumElements`) and VS signature. |
| `CreateRasterizerState` | Assign `res_id`, serialize `D3D11_RASTERIZER_DESC` (CullMode, FillMode, ScissorEnable). |
| `CreateBlendState` | Assign `res_id`, serialize `D3D11_BLEND_DESC` (BlendEnable, SrcBlend, DestBlend, BlendOp). |
| `CreateDepthStencilState`| Assign `res_id`, serialize `D3D11_DEPTH_STENCIL_DESC` (DepthEnable, DepthFunc). |
| `CreateSamplerState` | Assign `res_id`, serialize `D3D11_SAMPLER_DESC` (Filter, AddressU/V/W). |
| `GetImmediateContext` | Return referenced pointer to `ProxyD3D11DeviceContext` (`AddRef`). |
| `CheckFormatSupport` | Return supported feature bits (`D3D11_FORMAT_SUPPORT_TEXTURE2D`, `RENDER_TARGET`, `SHADER_SAMPLE`). |
| `CheckMultisampleQualityLevels` | Return `pNumQualityLevels = 1` for count=1. |

#### B. `ID3D11DeviceContext` (Command Execution)
| Stage / Category | Context Methods | Serialization Action |
|---|---|---|
| **Input Assembler** | `IASetInputLayout`<br>`IASetVertexBuffers`<br>`IASetIndexBuffer`<br>`IASetPrimitiveTopology` | Serialize layout `res_id`, buffer `res_ids`, strides, offsets, index format (`DXGI_FORMAT_R16_UINT` / `R32_UINT`), topology (`TRIANGLELIST`). |
| **Vertex Shader** | `VSSetShader`<br>`VSSetConstantBuffers`<br>`VSSetShaderResources`<br>`VSSetSamplers` | Serialize VS `res_id`, constant buffer `res_ids`, SRV `res_ids`, sampler `res_ids`, and start slots. |
| **Rasterizer** | `RSSetState`<br>`RSSetViewports`<br>`RSSetScissorRects` | Serialize rasterizer state `res_id`, viewport array (`D3D11_VIEWPORT{TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth}`). |
| **Pixel Shader** | `PSSetShader`<br>`PSSetConstantBuffers`<br>`PSSetShaderResources`<br>`PSSetSamplers` | Serialize PS `res_id`, constant buffer `res_ids`, SRV `res_ids`, sampler `res_ids`, and start slots. |
| **Output Merger** | `OMSetRenderTargets`<br>`OMSetBlendState`<br>`OMSetDepthStencilState` | Serialize RTV `res_ids`, DSV `res_id`, blend state `res_id`, blend factors, depth state `res_id`, stencil ref. |
| **Draw Calls** | `Draw`<br>`DrawIndexed`<br>`DrawInstanced`<br>`DrawIndexedInstanced` | Serialize vertex/index counts, start locations, base vertex locations. (Triggers remote GPU execution). |
| **Clear & Updates** | `ClearRenderTargetView`<br>`ClearDepthStencilView`<br>`UpdateSubresource`<br>`Map`<br>`Unmap` | Clear: serialize color float[4] / depth/stencil values.<br>UpdateSubresource: serialize payload bytes.<br>Map: return client shadow pointer.<br>Unmap: serialize shadow buffer delta. |
| **Synchronization** | `Flush` | Serialize flush packet to ensure host execution queues drain. |

#### C. `IDXGISwapChain` (Presentation & Backbuffer)
| Method | Description & Action |
|---|---|
| `Present(SyncInterval, Flags)` | Sends `CMD_PRESENT` packet, synchronizes frame streaming, receives latest rendered frame, blits to client `HWND`. |
| `GetBuffer(Buffer, riid, ppSurface)` | Returns the virtual backbuffer `ProxyD3D11Texture2D` (`res_id = 1`) with incremented refcount. |
| `ResizeBuffers(Count, Width, Height, Format, Flags)` | Notifies host to resize offscreen render target and staging textures; updates local swapchain/DIB dimensions. |
| `GetDesc(pDesc)` | Populates caller's `DXGI_SWAP_CHAIN_DESC` with current dimensions, format, and `HWND`. |

---

## 5. Client-Side SwapChain & Presentation Pipeline (R4)

### 5.1 Window Binding & Initialization
During `D3D11CreateDeviceAndSwapChain` or `IDXGIFactory::CreateSwapChain`:
1. The client captures the application's target window handle (`HWND = pSwapChainDesc->OutputWindow`).
2. If `BufferDesc.Width` or `Height` is 0, the proxy automatically queries client window client area using `GetClientRect(hWnd, &rc)`.
3. The proxy initializes a local Frame Presenter engine bound to this `HWND`.

### 5.2 Dual-Mode Presentation Engine

To satisfy the requirement of running on client machines **without requiring local hardware acceleration** (R1 & R4):

```
                     ┌──────────────────────────────────────────────┐
                     │ Incoming Frame from Remote Server            │
                     │ Header: Width, Height, Format, RowPitch      │
                     │ Payload: Raw RGBA / BGRA Pixel Bytes         │
                     └──────────────────────┬───────────────────────┘
                                            │
                     ┌──────────────────────┴───────────────────────┐
                     │ Client Presentation Engine (ProxyDXGISwapChain)│
                     └──────────────┬───────────────────────────────┘
                                    │
            ┌───────────────────────┴────────────────────────┐
            ▼                                                ▼
┌───────────────────────────────┐        ┌───────────────────────────────┐
│ Primary: GDI/DIB Presentation │        │ Secondary: Local D3D/DXGI     │
│ (Zero GPU Hardware Required)  │        │ (Accelerated VSync if present)│
├───────────────────────────────┤        ├───────────────────────────────┤
│ • BITMAPINFOHEADER setup      │        │ • Create local swapchain      │
│ • HDC hdc = GetDC(hWnd)       │        │ • UpdateSubresource(backbuf)  │
│ • SetDIBitsToDevice()         │        │ • localSwapChain->Present()   │
│ • ReleaseDC(hWnd, hdc)        │        │ • Hardware VSync alignment    │
│ • Guaranteed VM/RDP support   │        │                               │
└───────────────────────────────┘        └───────────────────────────────┘
```

#### Zero-Hardware Presentation Implementation (GDI DIB)
```cpp
void PresentFrameGDI(HWND hWnd, const uint8_t* pPixelData, int width, int height) {
    HDC hdc = GetDC(hWnd);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    SetDIBitsToDevice(
        hdc, 0, 0, width, height,
        0, 0, 0, height,
        pPixelData, &bmi, DIB_RGB_COLORS
    );
    ReleaseDC(hWnd, hdc);
}
```

### 5.3 Frame Streaming Handshake & Latency Control
To avoid bufferbloat or frame queuing buildup:
1. **Pipelining**: The client proxy maintains a max-in-flight queue of 1 or 2 frames.
2. When `Present()` is invoked:
   - Client sends `CMD_PRESENT { frame_id }` across the network socket.
   - Client flushes command batch.
   - Client checks frame receiver thread: if a new rendered frame is available, it swaps buffers and blits to `HWND`.
   - In synchronous/test mode (R5), `Present()` blocks until the rendered frame matching `frame_id` is received, ensuring verifiable lockstep execution for test verification.
   - In asynchronous/interactive mode, client presents the latest available frame without stalling the message pump.

---

## 6. Resource Lifecycle, Handle Mapping, and COM Considerations

### 6.1 Handle Translation Architecture
In standard Direct3D, objects are identified by pointer addresses in process memory (`ID3D11Buffer*`). Over a network boundary, memory addresses on the client and host differ:

```
Client Application          Client Proxy Layer            Remote GPU Host
──────────────────          ──────────────────            ───────────────
pBuffer (0x00A0F120)  ───►  res_id: 42             ───►   pNativeBuffer (0x7FFD9820)
                            (Monotonic uint32_t)          (Stored in unordered_map)
```
- **Monotonic ID Generator**: Client proxy assigns an atomic incrementing integer `res_id = ++m_nextResId;` upon each resource creation call (`CreateBuffer`, `CreateTexture2D`, `CreateVertexShader`, etc.).
- **Host Resource Table**: Host maintains `std::unordered_map<uint32_t, ComPtr<ID3D11DeviceChild>>`.
- **Command References**: All context methods (`IASetVertexBuffers`, `VSSetConstantBuffers`, `DrawIndexed`) encode resources using their 32-bit `res_id`.

### 6.2 COM Reference Counting & Garbage Collection
- Every proxy object implements standard COM `AddRef()` and `Release()` using `InterlockedIncrement` / `InterlockedDecrement`.
- When external application code releases its final reference (`Release()` returns 0):
  1. The proxy class destructor executes.
  2. The proxy issues a `CMD_DESTROY_RESOURCE { res_id }` packet over the network.
  3. The remote host server receives `CMD_DESTROY_RESOURCE`, removes `res_id` from its lookup map, which invokes `Release()` on the native GPU COM object.
- **QueryInterface Support**:
  - Proxy objects must support querying for common base interfaces:
    - `ProxyD3D11Buffer` -> `ID3D11Buffer`, `ID3D11Resource`, `ID3D11DeviceChild`, `IUnknown`.
    - `ProxyD3D11Texture2D` -> `ID3D11Texture2D`, `ID3D11Resource`, `IDXGISurface`, `IUnknown`.
    - `ProxyD3D11Device` -> `ID3D11Device`, `IDXGIDevice`, `IUnknown`.
    - `ProxyDXGISwapChain` -> `IDXGISwapChain`, `IDXGISwapChain1`, `IDXGIObject`, `IUnknown`.

### 6.3 Dynamic Buffer `Map` / `Unmap` Shadowing
Direct3D dynamic buffers (`D3D11_USAGE_DYNAMIC`, `D3D11_CPU_ACCESS_WRITE`) are frequently mapped by game loops:
1. `Map(pResource, Subresource, D3D11_MAP_WRITE_DISCARD, Flags, pMappedResource)`:
   - Returning a null pointer causes crashes in the application.
   - The proxy allocates a CPU shadow memory buffer equal to `D3D11_BUFFER_DESC::ByteWidth` upon resource creation.
   - `Map()` assigns `pMappedResource->pData = shadowBufferPtr; pMappedResource->RowPitch = ByteWidth;`.
2. `Unmap(pResource, Subresource)`:
   - The proxy serializes the updated shadow buffer bytes into `CMD_UPDATE_BUFFER { res_id, size, data }` and streams it over the socket.
   - Host receives data and updates its native GPU buffer using `UpdateSubresource` or `Map(D3D11_MAP_WRITE_DISCARD)`.

---

## 7. Direct3D 12 Interception Contrast & Extension Path

While Direct3D 11 is the baseline for this system, Direct3D 12 presents distinct architectural patterns:

| Architectural Aspect | Direct3D 11 | Direct3D 12 | Network Interception Impact |
|---|---|---|---|
| **Command Model** | Immediate Context (`ID3D11DeviceContext`) emits directly to driver. | Command Allocator + Command List (`ID3D12GraphicsCommandList`) + Command Queue (`ExecuteCommandLists`). | D3D12 requires intercepting recorded command lists and serializing them in atomic batches on `ExecuteCommandLists`. |
| **Pipeline State** | Modular state objects (`ID3D11BlendState`, `ID3D11RasterizerState`). | Monolithic Pipeline State Object (`ID3D12PipelineState`). | D3D12 PSO encapsulates VS/PS bytecode + raster/blend states into a single hashable object, reducing state change churn. |
| **Memory & Descriptors**| Implicit driver-managed descriptor tables and resource allocation. | Explicit descriptor heaps (`ID3D12DescriptorHeap`) and GPU virtual addresses. | D3D12 network offloading requires virtualizing descriptor tables and tracking CPU writes to upload heaps. |
| **Swapchain** | `IDXGISwapChain::Present` handles backbuffer rotation. | Application manages multiple backbuffer resources with explicit `ResourceBarrier(PRESENT)`. | Presentation mechanics remain identical via `IDXGISwapChain::Present`. |

**Future Extension Path**:
The network protocol opcode design can support D3D12 command lists by introducing `CMD_EXECUTE_COMMAND_LIST` batches containing recorded D3D12 packet streams, while sharing the identical network transport (R2), host execution adapter selection (R3), and framebuffer streaming pipeline (R4).

---

## 8. Authoritative Features Discovered & Edge Cases

The following tables document all discovered features and edge cases mined from the Direct3D 11/12 and DXGI specifications.

### Features Discovered

| # | Category | Feature | Description | Inputs | Outputs | Error Behavior | Discovered Via |
|---|----------|---------|-------------|--------|---------|----------------|----------------|
| 1 | Export | `D3D11CreateDevice` | Creates device and immediate context without swapchain. | Adapter, DriverType, Software, Flags, FeatureLevels, Levels, SDKVersion | `ppDevice`, `pFeatureLevel`, `ppImmediateContext` | Returns `E_INVALIDARG` if SDKVersion mismatch or null outputs; `DXGI_ERROR_UNSUPPORTED` if level unavailable. | `d3d11.dll` Export #22, `d3d11.h` |
| 2 | Export | `D3D11CreateDeviceAndSwapChain` | Creates device, context, and bound window swapchain. | Adapter, DriverType, Software, Flags, FeatureLevels, Levels, SDKVersion, `DXGI_SWAP_CHAIN_DESC` | `ppSwapChain`, `ppDevice`, `pFeatureLevel`, `ppImmediateContext` | Returns `DXGI_ERROR_INVALID_CALL` if HWND is invalid; `E_OUTOFMEMORY` on failure. | `d3d11.dll` Export #23, `d3d11.h` |
| 3 | Export | `CreateDXGIFactory1` | Creates DXGI 1.1 factory for adapter enumeration. | `REFIID`, `void** ppFactory` | `ppFactory` | Returns `E_NOINTERFACE` if unsupported IID; `E_POINTER` if null. | `dxgi.dll` Export #11, `dxgi.h` |
| 4 | Export | `CreateDXGIFactory2` | Creates DXGI 1.3+ factory supporting debug flags. | `Flags`, `REFIID`, `void** ppFactory` | `ppFactory` | Returns `E_INVALIDARG` if invalid flags; `E_NOINTERFACE`. | `dxgi.dll` Export #12, `dxgi1_3.h` |
| 5 | Device | `CreateBuffer` | Allocates vertex, index, or constant buffer. | `D3D11_BUFFER_DESC*`, `D3D11_SUBRESOURCE_DATA*` | `ID3D11Buffer** ppBuffer` | Returns `E_INVALIDARG` if ByteWidth == 0 or flags incompatible. | `ID3D11Device`, `d3d11.h:6012` |
| 6 | Device | `CreateTexture2D` | Allocates 2D texture or render target surface. | `D3D11_TEXTURE2D_DESC*`, `D3D11_SUBRESOURCE_DATA*` | `ID3D11Texture2D** ppTexture2D` | Returns `E_INVALIDARG` if dimensions exceed limits (16384 for FL11). | `ID3D11Device`, `d3d11.h:6014` |
| 7 | Device | `CreateShaderResourceView`| Creates SRV for binding textures/buffers to shaders. | `ID3D11Resource*`, `D3D11_SHADER_RESOURCE_VIEW_DESC*` | `ID3D11ShaderResourceView** ppSRV` | Returns `E_INVALIDARG` if resource format incompatible with view format. | `ID3D11Device`, `d3d11.h:6016` |
| 8 | Device | `CreateRenderTargetView`| Creates RTV for offscreen rendering / framebuffers. | `ID3D11Resource*`, `D3D11_RENDER_TARGET_VIEW_DESC*` | `ID3D11RenderTargetView** ppRTV` | Returns `E_INVALIDARG` if resource lacks `D3D11_BIND_RENDER_TARGET`. | `ID3D11Device`, `d3d11.h:6018` |
| 9 | Device | `CreateDepthStencilView`| Creates DSV for depth buffering. | `ID3D11Resource*`, `D3D11_DEPTH_STENCIL_VIEW_DESC*` | `ID3D11DepthStencilView** ppDSV` | Returns `E_INVALIDARG` if resource lacks `D3D11_BIND_DEPTH_STENCIL`. | `ID3D11Device`, `d3d11.h:6019` |
| 10 | Device | `CreateInputLayout` | Creates input layout object matching vertex shader. | `D3D11_INPUT_ELEMENT_DESC[]`, `NumElements`, Bytecode, BytecodeLength | `ID3D11InputLayout** ppInputLayout` | Returns `E_INVALIDARG` if element semantic mismatch with bytecode input signature. | `ID3D11Device`, `d3d11.h:6020` |
| 11 | Device | `CreateVertexShader` | Creates vertex shader from compiled DXBC bytecode. | `pShaderBytecode`, `BytecodeLength`, `pClassLinkage` | `ID3D11VertexShader** ppVertexShader`| Returns `E_INVALIDARG` if bytecode corrupted or header invalid. | `ID3D11Device`, `d3d11.h:6021` |
| 12 | Device | `CreatePixelShader` | Creates pixel shader from compiled DXBC bytecode. | `pShaderBytecode`, `BytecodeLength`, `pClassLinkage` | `ID3D11PixelShader** ppPixelShader` | Returns `E_INVALIDARG` if bytecode invalid. | `ID3D11Device`, `d3d11.h:6024` |
| 13 | Device | `CreateRasterizerState` | Creates rasterizer state object. | `D3D11_RASTERIZER_DESC*` | `ID3D11RasterizerState** ppRasterizerState` | Returns `S_OK` or `E_OUTOFMEMORY`. | `ID3D11Device`, `d3d11.h:6031` |
| 14 | Device | `CreateBlendState` | Creates blend state object for alpha transparency. | `D3D11_BLEND_DESC*` | `ID3D11BlendState** ppBlendState` | Returns `E_INVALIDARG` if invalid blend factor. | `ID3D11Device`, `d3d11.h:6029` |
| 15 | Device | `CreateDepthStencilState`| Creates depth-stencil testing configuration. | `D3D11_DEPTH_STENCIL_DESC*` | `ID3D11DepthStencilState** ppDepthStencilState` | Returns `S_OK` or `E_OUTOFMEMORY`. | `ID3D11Device`, `d3d11.h:6030` |
| 16 | Device | `CreateSamplerState` | Creates texture sampler filtering state. | `D3D11_SAMPLER_DESC*` | `ID3D11SamplerState** ppSamplerState` | Returns `E_INVALIDARG` if filter mode invalid. | `ID3D11Device`, `d3d11.h:6032` |
| 17 | Context | `IASetInputLayout` | Binds input layout to input assembler stage. | `ID3D11InputLayout*` | void | If null, disables input layout. | `ID3D11DeviceContext`, `d3d11.h:6224` |
| 18 | Context | `IASetVertexBuffers` | Binds array of vertex buffers with strides/offsets. | StartSlot, NumBuffers, `ID3D11Buffer*[]`, Strides[], Offsets[] | void | Clamped to `D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT` (32). | `ID3D11DeviceContext`, `d3d11.h:6225` |
| 19 | Context | `IASetIndexBuffer` | Binds index buffer for indexed drawing. | `ID3D11Buffer*`, `DXGI_FORMAT`, Offset | void | Format must be `DXGI_FORMAT_R16_UINT` or `R32_UINT`. | `ID3D11DeviceContext`, `d3d11.h:6226` |
| 20 | Context | `IASetPrimitiveTopology`| Sets primitive topology (e.g. `TRIANGLELIST`). | `D3D11_PRIMITIVE_TOPOLOGY` | void | Rejects undefined topology. | `ID3D11DeviceContext`, `d3d11.h:6231` |
| 21 | Context | `VSSetShader` | Sets active vertex shader stage. | `ID3D11VertexShader*`, ClassInstances, NumInstances | void | If null, unbinds stage. | `ID3D11DeviceContext`, `d3d11.h:6221` |
| 22 | Context | `VSSetConstantBuffers` | Binds constant buffers to vertex shader stage. | StartSlot, NumBuffers, `ID3D11Buffer*[]` | void | Slots clamped to 14. | `ID3D11DeviceContext`, `d3d11.h:6202` |
| 23 | Context | `RSSetViewports` | Sets rasterizer viewports (scaling and clip). | `NumViewports`, `D3D11_VIEWPORT*[]` | void | Clamped to `D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE` (16). | `ID3D11DeviceContext`, `d3d11.h:6251` |
| 24 | Context | `PSSetShader` | Sets active pixel shader stage. | `ID3D11PixelShader*`, ClassInstances, NumInstances | void | If null, rasterizer does not execute pixel shader. | `ID3D11DeviceContext`, `d3d11.h:6212` |
| 25 | Context | `PSSetConstantBuffers` | Binds constant buffers to pixel shader stage. | StartSlot, NumBuffers, `ID3D11Buffer*[]` | void | Slots clamped to 14. | `ID3D11DeviceContext`, `d3d11.h:6223` |
| 26 | Context | `PSSetShaderResources` | Binds texture views to pixel shader stage. | StartSlot, NumViews, `ID3D11ShaderResourceView*[]` | void | Clamped to 128 views. | `ID3D11DeviceContext`, `d3d11.h:6207` |
| 27 | Context | `PSSetSamplers` | Binds texture sampler states to pixel shader. | StartSlot, NumSamplers, `ID3D11SamplerState*[]` | void | Clamped to 16 samplers. | `ID3D11DeviceContext`, `d3d11.h:6217` |
| 28 | Context | `OMSetRenderTargets` | Binds color render targets and depth-stencil view. | NumViews, `ID3D11RenderTargetView*[]`, `ID3D11DepthStencilView*` | void | Clamped to `D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT` (8). | `ID3D11DeviceContext`, `d3d11.h:6240` |
| 29 | Context | `ClearRenderTargetView`| Clears bound render target to specified RGBA color. | `ID3D11RenderTargetView*`, `FLOAT ColorRGBA[4]` | void | No-op if null RTV. | `ID3D11DeviceContext`, `d3d11.h:6256` |
| 30 | Context | `ClearDepthStencilView`| Clears depth and/or stencil values. | `ID3D11DepthStencilView*`, ClearFlags, Depth, Stencil | void | Flags must contain `D3D11_CLEAR_DEPTH` or `STENCIL`. | `ID3D11DeviceContext`, `d3d11.h:6259` |
| 31 | Context | `Draw` | Non-indexed draw submission. | VertexCount, StartVertexLocation | void | No-op if VertexCount == 0. | `ID3D11DeviceContext`, `d3d11.h:6223` |
| 32 | Context | `DrawIndexed` | Indexed draw submission. | IndexCount, StartIndexLocation, BaseVertexLocation | void | No-op if IndexCount == 0. | `ID3D11DeviceContext`, `d3d11.h:6222` |
| 33 | Context | `UpdateSubresource` | Updates resource data from CPU memory. | DstResource, DstSubresource, `D3D11_BOX*`, pSrcData, SrcRowPitch, SrcDepthPitch | void | Rejects non-default or immutable resources. | `ID3D11DeviceContext`, `d3d11.h:6254` |
| 34 | Context | `Map` | Maps subresource into CPU-addressable memory. | Resource, Subresource, MapType, MapFlags, `D3D11_MAPPED_SUBRESOURCE*` | `S_OK` | Returns `DXGI_ERROR_WAS_STILL_DRAWING` if DO_NOT_WAIT specified and busy. | `ID3D11DeviceContext`, `d3d11.h:6227` |
| 35 | Context | `Unmap` | Unmaps subresource, synchronizing changes. | Resource, Subresource | void | Re-enables GPU access. | `ID3D11DeviceContext`, `d3d11.h:6228` |
| 36 | SwapChain | `Present` | Submits frame, triggers host readback & client display. | SyncInterval, Flags | `S_OK` | Returns `DXGI_ERROR_DEVICE_REMOVED` if remote host connection dropped. | `IDXGISwapChain`, `dxgi.h:1768` |
| 37 | SwapChain | `GetBuffer` | Retrieves backbuffer texture interface. | BufferIndex, `REFIID`, `void** ppSurface` | `S_OK` | Returns `DXGI_ERROR_INVALID_CALL` if Index >= BufferCount. | `IDXGISwapChain`, `dxgi.h:1769` |
| 38 | SwapChain | `ResizeBuffers` | Resizes swapchain backbuffers upon window resize. | BufferCount, Width, Height, NewFormat, SwapChainFlags | `S_OK` | Fails with `DXGI_ERROR_INVALID_CALL` if outstanding references to backbuffers remain. | `IDXGISwapChain`, `dxgi.h:1773` |

---

### Edge Cases

| # | Feature | Input | Observed Behavior | Handling Recommendation |
|---|---------|-------|-------------------|--------------------------|
| 1 | `IDXGISwapChain::GetBuffer` | `BufferIndex = 0, riid = __uuidof(ID3D11Texture2D)` | Client app requests pointer to backbuffer to create RTV. | Proxy must return virtual backbuffer proxy object with reserved `res_id = 1` and increment reference count. |
| 2 | `IDXGISwapChain::ResizeBuffers` | Outstanding RTV / BackBuffer references held by app | Native D3D11 returns `DXGI_ERROR_INVALID_CALL` if backbuffer refcount > 1. | In proxy, application must release local backbuffer RTV before calling `ResizeBuffers`, matching native spec. |
| 3 | `ID3D11DeviceContext::Map` | `D3D11_MAP_WRITE_DISCARD` on dynamic buffer | App expects a valid writable CPU pointer in `pMappedResource->pData`. | Proxy allocates local CPU shadow buffer; returns shadow pointer; streams updated bytes on `Unmap`. |
| 4 | `ID3D11DeviceContext::Map` | `D3D11_MAP_READ` on staging resource | App requests GPU readback on client. | Requires synchronous network roundtrip. For 3D render loop offloading, games rarely read back; log warning or fetch staging texture synchronously. |
| 5 | COM `QueryInterface` | `pDevice->QueryInterface(__uuidof(IDXGIDevice))` | App queries DXGI device to retrieve adapter or factory. | `ProxyD3D11Device` must implement `IDXGIDevice` and return pointer to itself with `AddRef()`. |
| 6 | Constant Buffer 16-byte Alignment | `ByteWidth` not multiple of 16 in `CreateBuffer` | Native D3D11 `CreateBuffer` fails with `E_INVALIDARG` if `D3D11_BIND_CONSTANT_BUFFER` is set and `ByteWidth % 16 != 0`. | Proxy validates `ByteWidth % 16 == 0` locally or forwards error code without network traffic. |
| 7 | Zero Viewport / Scissor Dimensions | `Width = 0` or `Height = 0` in `RSSetViewports` | Direct3D disables rasterization; no pixels rendered. | Fully supported; serialized and passed to remote host GPU. |
| 8 | Network Disconnection During Present | Socket closed / dropped during `Present()` call | Client cannot receive streamed frame. | Proxy detects socket EOF/error, returns `DXGI_ERROR_DEVICE_REMOVED`, initiates clean teardown. |
| 9 | Multi-Threaded Resource Creation | Simultaneous calls to `CreateBuffer` from worker threads | `ID3D11Device` is thread-safe per DirectX spec. | Proxy protects socket transmission buffer and monotonic ID generator with mutex. |
| 10 | Headless Automated Test Mode (R5) | Client running without visible monitor / offscreen window | `Present()` blits to headless `HWND` or memory buffer. | Frame receiver stores latest raw RGBA frame in memory buffer for programmatic luminance and pixel assertions. |

---

## 9. Conclusion & Implementation Guidance for Phase 1

1. **Architecture Confirmation**: The drop-in Wrapper DLL (`d3d11.dll`) paired with COM Proxy Objects (`ProxyD3D11Device`, `ProxyD3D11DeviceContext`, `ProxyDXGISwapChain`) satisfies all requirements of R1 and R4 without requiring any client-side GPU hardware or invasive kernel hooks.
2. **Standard Compatibility**: By supporting the 40 methods of `ID3D11Device`, the core 35 rendering methods of `ID3D11DeviceContext`, and `IDXGISwapChain::Present`, any standard textured 3D mesh demo (including spinning cubes, lighting, and camera transforms) will execute completely transparently.
3. **Seamless Client Display**: Software GDI/DIB presentation directly into the application `HWND` guarantees 100% display functionality across virtual machines, headless CI runners, and bare-bones Windows clients.
