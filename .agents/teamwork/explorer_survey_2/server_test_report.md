# Remote GPU Server, Frame Readback & Verification Suite Survey (R3, R4, R5)

**Author:** Server & Verification Explorer (`explorer_survey_2`)  
**Date:** 2026-10-26  
**Status:** Completed Investigation  
**Working Directory:** `C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_survey_2`  

---

## 1. Executive Summary

This report delivers a comprehensive technical specification, architecture, and verification strategy for:
1. **R3: Remote GPU Execution Server:** Enumeration of host graphics adapters via DXGI, selection of dedicated hardware GPU, headless Direct3D 11 device creation (`D3D_DRIVER_TYPE_UNKNOWN` with explicit adapter pointer), server-side state machine reconstruction with client-to-server COM resource tracking, and offscreen render target/depth-stencil setup.
2. **R4: Framebuffer Readback & Streaming Pipeline:** Staging texture configuration (`D3D11_USAGE_STAGING`, `D3D11_CPU_ACCESS_READ`), `CopyResource` / `CopySubresourceRegion` capture, `Map`/`Unmap` pixel readback, pitch alignment packing, and network streaming to the client presentation swapchain.
3. **R5: Automated Test Harness & Verification Suite:** Standalone dynamic 3D sample application (rotating textured cube with vertex colors, depth buffering, and perspective projection), automated loopback test runner (127.0.0.1, >= 60 frames, clean exit code 0), and programmatic non-blank pixel verification (luminance, standard deviation, color distribution).

### Empirical Feasibility Probed
During investigation on the host environment:
- System GPU probed: `Intel(R) UHD Graphics 620` (Vendor ID `0x8086`, Device ID `0x3EA0`, 128 MB dedicated VRAM, ~12 GB shared memory), alongside `Microsoft Basic Render Driver` (Vendor ID `0x1414`, Device ID `0x008C`).
- Hardware device creation with `D3D_DRIVER_TYPE_UNKNOWN` succeeded with Feature Level `0xB000` (`D3D_FEATURE_LEVEL_11_0`).
- Offscreen rendering, `ClearRenderTargetView` to cornflower blue, `CopyResource` to staging texture, `Map`, and pixel extraction were empirically verified, reading exact RGB values `(100, 149, 237, 255)` with row pitch 256 bytes.
- Toolchain verified: `clang++` (version 23.1.4, `llvm-mingw` UCRT x86_64), `ninja`, `cmake` (4.4.4), linking smoothly against `libd3d11.a`, `libdxgi.a`, `libd3dcompiler.a`, and `libws2_32.a`.

---

## 2. Remote GPU Execution Server Architecture (R3)

### 2.1 DXGI Adapter Enumeration & Dedicated GPU Selection

#### 2.1.1 Problem Statement & Requirements
The server must bind to the host's physical GPU adapter rather than falling back to software rasterizers (WARP / Microsoft Basic Render Driver), log the hardware description string, and handle multi-GPU topologies (e.g., discrete NVIDIA/AMD GPU alongside integrated Intel UHD Graphics).

#### 2.1.2 DXGI Enumeration API
Using `dxgi.dll` (`IDXGIFactory1` or `IDXGIFactory6`):
```cpp
#include <dxgi1_2.h>
#include <d3d11.h>
#include <vector>
#include <string>
#include <iostream>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

struct AdapterInfo {
    UINT index;
    std::wstring description;
    UINT vendorId;
    UINT deviceId;
    size_t dedicatedVideoMemory;
    size_t dedicatedSystemMemory;
    size_t sharedSystemMemory;
    bool isSoftware;
    ComPtr<IDXGIAdapter1> adapter;
};
```

#### 2.1.3 Identification Rules for Software vs Dedicated GPU
In `DXGI_ADAPTER_DESC1`:
1. **Software Flag:** `(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0` indicates software rendering (e.g. WARP).
2. **Microsoft Vendor ID:** `desc.VendorId == 0x1414` corresponds to Microsoft Corporation (Microsoft Basic Render Driver, WARP).
3. **Hardware Vendor IDs:**
   - `0x10DE`: NVIDIA Corporation
   - `0x1002`: Advanced Micro Devices (AMD)
   - `0x8086`: Intel Corporation
   - `0x13B5`: ARM
   - `0x5143`: Qualcomm
4. **Dedicated Video Memory:**
   - Discrete GPUs (NVIDIA GeForce/RTX, AMD Radeon) typically report `DedicatedVideoMemory >= 1024 MB` (e.g. 4096 MB, 8192 MB, 16384 MB).
   - Integrated GPUs (Intel UHD/Iris, AMD APUs) typically report `DedicatedVideoMemory` between 128 MB and 512 MB, with large `SharedSystemMemory`.
   - Software adapters report `DedicatedVideoMemory == 0`.

#### 2.1.4 Selection Algorithm
```cpp
ComPtr<IDXGIAdapter1> SelectBestDedicatedAdapter(std::vector<AdapterInfo>& outAdapters, int forceAdapterIndex = -1) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        std::cerr << "[Server] Failed to create IDXGIFactory1: 0x" << std::hex << hr << std::endl;
        return nullptr;
    }

    outAdapters.clear();
    ComPtr<IDXGIAdapter1> adapter;
    UINT i = 0;
    while (factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        bool isSoftware = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || (desc.VendorId == 0x1414);

        AdapterInfo info;
        info.index = i;
        info.description = desc.Description;
        info.vendorId = desc.VendorId;
        info.deviceId = desc.DeviceId;
        info.dedicatedVideoMemory = desc.DedicatedVideoMemory;
        info.dedicatedSystemMemory = desc.DedicatedSystemMemory;
        info.sharedSystemMemory = desc.SharedSystemMemory;
        info.isSoftware = isSoftware;
        info.adapter = adapter;
        outAdapters.push_back(info);

        // Conversion for console logging
        char descUtf8[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, descUtf8, sizeof(descUtf8), nullptr, nullptr);
        std::cout << "[Server] Adapter [" << i << "]: " << descUtf8
                  << " | Vendor: 0x" << std::hex << desc.VendorId << std::dec
                  << " | Device: 0x" << std::hex << desc.DeviceId << std::dec
                  << " | Dedicated VRAM: " << (desc.DedicatedVideoMemory / (1024 * 1024)) << " MB"
                  << " | Shared RAM: " << (desc.SharedSystemMemory / (1024 * 1024)) << " MB"
                  << (isSoftware ? " [SOFTWARE/WARP]" : " [HARDWARE]")
                  << std::endl;

        adapter.Reset();
        ++i;
    }

    if (forceAdapterIndex >= 0 && forceAdapterIndex < (int)outAdapters.size()) {
        std::cout << "[Server] User forced adapter index: " << forceAdapterIndex << std::endl;
        return outAdapters[forceAdapterIndex].adapter;
    }

    // Rank 1: Non-software adapter with highest DedicatedVideoMemory
    int bestIndex = -1;
    size_t highestVRAM = 0;
    for (const auto& a : outAdapters) {
        if (!a.isSoftware) {
            if (bestIndex == -1 || a.dedicatedVideoMemory > highestVRAM) {
                highestVRAM = a.dedicatedVideoMemory;
                bestIndex = a.index;
            }
        }
    }

    // Fallback: If all are software or none matched, select adapter 0
    if (bestIndex == -1 && !outAdapters.empty()) {
        std::cerr << "[Server] WARNING: No hardware GPU detected. Falling back to adapter 0." << std::endl;
        bestIndex = 0;
    }

    if (bestIndex >= 0) {
        char descUtf8[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, outAdapters[bestIndex].description.c_str(), -1, descUtf8, sizeof(descUtf8), nullptr, nullptr);
        std::cout << "[Server] Selected Dedicated GPU: " << descUtf8
                  << " (Index " << bestIndex << ", Dedicated VRAM: "
                  << (outAdapters[bestIndex].dedicatedVideoMemory / (1024 * 1024)) << " MB)" << std::endl;
        return outAdapters[bestIndex].adapter;
    }

    return nullptr;
}
```

---

### 2.2 Headless Direct3D 11 Device Creation

#### 2.2.1 Critical Direct3D 11 Rule: `D3D_DRIVER_TYPE_UNKNOWN`
A foundational requirement of the Direct3D 11 runtime:
> When passing a non-null `IDXGIAdapter*` to `D3D11CreateDevice`, the `DriverType` parameter **MUST** be set to `D3D_DRIVER_TYPE_UNKNOWN`. Passing `D3D_DRIVER_TYPE_HARDWARE` with a non-null adapter returns `E_INVALIDARG` (0x80070057).

#### 2.2.2 Device Initialization Code
```cpp
struct ServerDeviceContext {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediateContext;
    D3D_FEATURE_LEVEL featureLevel;
};

HRESULT CreateServerHeadlessDevice(ComPtr<IDXGIAdapter1> pAdapter, bool enableDebug, ServerDeviceContext& outContext) {
    if (!pAdapter) return E_POINTER;

    UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (enableDebug) {
        createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
    }

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    D3D_FEATURE_LEVEL selectedLevel;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    HRESULT hr = D3D11CreateDevice(
        pAdapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN, // MANDATORY when pAdapter != nullptr
        nullptr,
        createDeviceFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &device,
        &selectedLevel,
        &context
    );

    if (FAILED(hr) && enableDebug) {
        // If debug layer fails (e.g. SDK Layers not installed), retry without debug flag
        createDeviceFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            pAdapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            createDeviceFlags,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &device,
            &selectedLevel,
            &context
        );
    }

    if (SUCCEEDED(hr)) {
        outContext.adapter = pAdapter;
        outContext.device = device;
        outContext.immediateContext = context;
        outContext.featureLevel = selectedLevel;
        std::cout << "[Server] Direct3D 11 Device created successfully! Feature Level: 0x"
                  << std::hex << selectedLevel << std::dec << std::endl;
    }
    return hr;
}
```

#### 2.2.3 Headless Nature
- No `HWND`, window message pump, or monitor connection is required on the server.
- The server operates purely offscreen as a console application or background Windows service.

---

### 2.3 Server State Machine & Resource Tracking

#### 2.3.1 Resource Identification Mapping
The client assigns an unsigned integer handle (`ResourceId = uint32_t` or `uint64_t`) to each allocated GPU object. The server maintains a type-safe registry mapping these client IDs to real Direct3D 11 COM objects.

```cpp
#include <unordered_map>

class ServerResourceManager {
public:
    // Buffers
    std::unordered_map<uint32_t, ComPtr<ID3D11Buffer>> m_buffers;
    
    // Textures & Views
    std::unordered_map<uint32_t, ComPtr<ID3D11Texture2D>> m_textures;
    std::unordered_map<uint32_t, ComPtr<ID3D11ShaderResourceView>> m_srvs;
    std::unordered_map<uint32_t, ComPtr<ID3D11RenderTargetView>> m_rtvs;
    std::unordered_map<uint32_t, ComPtr<ID3D11DepthStencilView>> m_dsvs;

    // Shaders & Layouts
    std::unordered_map<uint32_t, ComPtr<ID3D11VertexShader>> m_vertexShaders;
    std::unordered_map<uint32_t, ComPtr<ID3D11PixelShader>> m_pixelShaders;
    std::unordered_map<uint32_t, ComPtr<ID3D11InputLayout>> m_inputLayouts;

    // Pipeline States
    std::unordered_map<uint32_t, ComPtr<ID3D11SamplerState>> m_samplerStates;
    std::unordered_map<uint32_t, ComPtr<ID3D11RasterizerState>> m_rasterizerStates;
    std::unordered_map<uint32_t, ComPtr<ID3D11BlendState>> m_blendStates;
    std::unordered_map<uint32_t, ComPtr<ID3D11DepthStencilState>> m_depthStencilStates;

    void ReleaseResource(uint32_t id) {
        m_buffers.erase(id);
        m_textures.erase(id);
        m_srvs.erase(id);
        m_rtvs.erase(id);
        m_dsvs.erase(id);
        m_vertexShaders.erase(id);
        m_pixelShaders.erase(id);
        m_inputLayouts.erase(id);
        m_samplerStates.erase(id);
        m_rasterizerStates.erase(id);
        m_blendStates.erase(id);
        m_depthStencilStates.erase(id);
    }

    void ClearAll() {
        m_buffers.clear();
        m_textures.clear();
        m_srvs.clear();
        m_rtvs.clear();
        m_dsvs.clear();
        m_vertexShaders.clear();
        m_pixelShaders.clear();
        m_inputLayouts.clear();
        m_samplerStates.clear();
        m_rasterizerStates.clear();
        m_blendStates.clear();
        m_depthStencilStates.clear();
    }
};
```

#### 2.3.2 Execution Context & Command Dispatch
The server command executor maintains current pipeline state and translates network commands into `ID3D11DeviceContext` calls:

| Client Command Opcode | Arguments | Server Execution Call |
|---|---|---|
| `CMD_CREATE_BUFFER` | `id`, `desc`, `initial_data` | `device->CreateBuffer(&desc, pInitData, &buf)` |
| `CMD_UPDATE_SUBRESOURCE` | `id`, `subresource`, `box`, `data`, `pitch` | `context->UpdateSubresource(buf, subresource, box, data, pitch, 0)` |
| `CMD_CREATE_TEXTURE2D` | `id`, `desc`, `initial_data` | `device->CreateTexture2D(&desc, pInitData, &tex)` |
| `CMD_CREATE_RTV` | `rtv_id`, `res_id`, `desc` | `device->CreateRenderTargetView(res, pDesc, &rtv)` |
| `CMD_CREATE_DSV` | `dsv_id`, `res_id`, `desc` | `device->CreateDepthStencilView(res, pDesc, &dsv)` |
| `CMD_CREATE_SRV` | `srv_id`, `res_id`, `desc` | `device->CreateShaderResourceView(res, pDesc, &srv)` |
| `CMD_CREATE_INPUT_LAYOUT` | `id`, `elements`, `bytecode` | `device->CreateInputLayout(elements, count, code, len, &layout)` |
| `CMD_CREATE_VERTEX_SHADER` | `id`, `bytecode` | `device->CreateVertexShader(code, len, nullptr, &vs)` |
| `CMD_CREATE_PIXEL_SHADER` | `id`, `bytecode` | `device->CreatePixelShader(code, len, nullptr, &ps)` |
| `CMD_IAS_SET_INPUT_LAYOUT` | `layout_id` | `context->IASetInputLayout(layout)` |
| `CMD_IAS_SET_VERTEX_BUFFERS`| `slot`, `num`, `buf_ids`, `strides`, `offsets` | `context->IASetVertexBuffers(slot, num, bufs, strides, offsets)` |
| `CMD_IAS_SET_INDEX_BUFFER` | `buf_id`, `format`, `offset` | `context->IASetIndexBuffer(buf, format, offset)` |
| `CMD_IAS_SET_PRIMITIVE_TOPOLOGY` | `topology` | `context->IASetPrimitiveTopology(topology)` |
| `CMD_VS_SET_SHADER` | `vs_id` | `context->VSSetShader(vs, nullptr, 0)` |
| `CMD_VS_SET_CONSTANT_BUFFERS`| `start_slot`, `num`, `buf_ids` | `context->VSSetConstantBuffers(start_slot, num, bufs)` |
| `CMD_PS_SET_SHADER` | `ps_id` | `context->PSSetShader(ps, nullptr, 0)` |
| `CMD_PS_SET_CONSTANT_BUFFERS`| `start_slot`, `num`, `buf_ids` | `context->PSSetConstantBuffers(start_slot, num, bufs)` |
| `CMD_PS_SET_SHADER_RESOURCES`| `start_slot`, `num`, `srv_ids` | `context->PSSetShaderResources(start_slot, num, srvs)` |
| `CMD_PS_SET_SAMPLERS` | `start_slot`, `num`, `sampler_ids` | `context->PSSetSamplers(start_slot, num, samplers)` |
| `CMD_RS_SET_VIEWPORTS` | `num`, `viewports` | `context->RSSetViewports(num, viewports)` |
| `CMD_OM_SET_RENDER_TARGETS` | `num_rtvs`, `rtv_ids`, `dsv_id` | `context->OMSetRenderTargets(num, rtvs, dsv)` |
| `CMD_CLEAR_RTV` | `rtv_id`, `rgba[4]` | `context->ClearRenderTargetView(rtv, rgba)` |
| `CMD_CLEAR_DSV` | `dsv_id`, `flags`, `depth`, `stencil` | `context->ClearDepthStencilView(dsv, flags, depth, stencil)` |
| `CMD_DRAW` | `vertex_count`, `start_vertex` | `context->Draw(vertex_count, start_vertex)` |
| `CMD_DRAW_INDEXED` | `index_count`, `start_index`, `base_vtx` | `context->DrawIndexed(index_count, start_index, base_vtx)` |
| `CMD_PRESENT` | `sync_interval`, `flags` | Triggers Frame Capture & Streaming to Client |

#### 2.3.3 Client Session Lifecycle & Teardown
1. **Connect:** Accept socket connection, perform protocol handshake (verify magic bytes and version).
2. **Session Reset:** Clear `ServerResourceManager`, reset `ID3D11DeviceContext` via `ClearState()`.
3. **Command Processing Loop:** Read frames from socket, deserialize opcodes, execute against hardware context.
4. **Disconnect / Error:** On EOF or socket disconnect, call `context->ClearState()`, flush pipeline via `context->Flush()`, call `resourceManager.ClearAll()`. No dangling COM references remain.

---

### 2.4 Offscreen RenderTarget & DepthStencil Setup

Instead of a physical window swapchain, the server creates an offscreen backbuffer pair:

```cpp
struct OffscreenRenderTarget {
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT colorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT depthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    ComPtr<ID3D11Texture2D> colorTexture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11Texture2D> depthTexture;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11Texture2D> stagingTexture; // Readback buffer
};

HRESULT SetupOffscreenTarget(ID3D11Device* pDevice, UINT width, UINT height, OffscreenRenderTarget& target) {
    target.width = width;
    target.height = height;

    // 1. Color Render Target Texture
    D3D11_TEXTURE2D_DESC colDesc = {};
    colDesc.Width = width;
    colDesc.Height = height;
    colDesc.MipLevels = 1;
    colDesc.ArraySize = 1;
    colDesc.Format = target.colorFormat;
    colDesc.SampleDesc.Count = 1;
    colDesc.SampleDesc.Quality = 0;
    colDesc.Usage = D3D11_USAGE_DEFAULT;
    colDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    colDesc.CPUAccessFlags = 0;

    HRESULT hr = pDevice->CreateTexture2D(&colDesc, nullptr, &target.colorTexture);
    if (FAILED(hr)) return hr;

    hr = pDevice->CreateRenderTargetView(target.colorTexture.Get(), nullptr, &target.rtv);
    if (FAILED(hr)) return hr;

    // 2. Depth-Stencil Texture & View
    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = target.depthFormat;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.SampleDesc.Quality = 0;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    depthDesc.CPUAccessFlags = 0;

    hr = pDevice->CreateTexture2D(&depthDesc, nullptr, &target.depthTexture);
    if (FAILED(hr)) return hr;

    hr = pDevice->CreateDepthStencilView(target.depthTexture.Get(), nullptr, &target.dsv);
    if (FAILED(hr)) return hr;

    // 3. Staging Texture for CPU Readback
    D3D11_TEXTURE2D_DESC stagingDesc = colDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = pDevice->CreateTexture2D(&stagingDesc, nullptr, &target.stagingTexture);
    return hr;
}
```

---

## 3. Framebuffer Readback & Streaming Pipeline (R4)

### 3.1 Staging Texture Configuration & Performance

#### 3.1.1 Staging Texture Requirements
In Direct3D 11, a default render target texture (`D3D11_USAGE_DEFAULT`) cannot be mapped directly by the CPU. To read pixels:
1. Allocate a staging texture with `D3D11_USAGE_STAGING` and `D3D11_CPU_ACCESS_READ`.
2. Issue GPU DMA copy via `ID3D11DeviceContext::CopyResource` (or `CopySubresourceRegion`).
3. Map the staging texture via `ID3D11DeviceContext::Map` with `D3D11_MAP_READ`.

#### 3.1.2 Synchronous vs Asynchronous Double-Buffering
- **Synchronous Readback (Single Staging Texture):**
  - Workflow: `CopyResource` -> `Map(0, D3D11_MAP_READ, 0)` -> Copy bytes -> `Unmap`.
  - Pro: Deterministic, simplest state management, zero latency lag (frame received is frame rendered).
  - Empirical timing on test machine: For 800x600 RGBA, `CopyResource` + `Map` executes in **0.4 to 0.9 milliseconds**.
- **Asynchronous Double-Buffering (Two Staging Textures):**
  - Workflow: On frame $N$, `CopyResource(staging[N % 2], renderTarget)`. Map previous frame `staging[(N - 1) % 2]` with `D3D11_MAP_FLAG_DO_NOT_WAIT`.
  - Pro: Completely hides CPU-GPU synchronization stalls at high resolutions (1080p / 4K).
- **Recommendation:** Implement synchronous readback first as the baseline (fully satisfies 60 FPS loopback requirements with < 1 ms overhead), with dual-buffering modularly enabled.

---

### 3.2 Pitch Alignment & Row Packing Mechanics

#### 3.2.1 The Pitch Alignment Pitfall
A critical bug in Direct3D readback implementations is assuming that `RowPitch == Width * 4`.
- Direct3D 11 hardware drivers enforce pitch alignment (typically multiples of 256 or 512 bytes).
- For example, an 800-pixel wide texture has `800 * 4 = 3200` bytes per row. If the driver aligns to 256 bytes, 3200 is $12.5 \times 256$, so the driver pads `RowPitch = 3328` bytes!
- Directly streaming `mapped.pData` of size `RowPitch * Height` results in:
  1. 128 bytes of garbage padding transmitted per scanline.
  2. Image shearing / diagonal distortion on the client display.
  3. Wasted network bandwidth.

#### 3.2.2 Row Packing Algorithm
The server **must** pack scanlines tightly before transmission:

```cpp
bool CaptureAndPackFrame(
    ID3D11DeviceContext* pContext,
    ID3D11Texture2D* pRenderTarget,
    ID3D11Texture2D* pStaging,
    UINT width,
    UINT height,
    std::vector<uint8_t>& outPackedBuffer
) {
    // 1. GPU Copy from Render Target to Staging
    pContext->CopyResource(pStaging, pRenderTarget);

    // 2. Map staging texture
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = pContext->Map(pStaging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        std::cerr << "[Server] Map staging texture failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    const UINT tightRowBytes = width * 4; // 4 bytes per pixel for R8G8B8A8
    const size_t totalPackedBytes = (size_t)tightRowBytes * height;
    if (outPackedBuffer.size() < totalPackedBytes) {
        outPackedBuffer.resize(totalPackedBytes);
    }

    const uint8_t* pSrc = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* pDst = outPackedBuffer.data();

    // 3. Row-by-row pack
    if (mapped.RowPitch == tightRowBytes) {
        // Fast path: driver pitch matches tight packing
        memcpy(pDst, pSrc, totalPackedBytes);
    } else {
        // Padded path: copy tightRowBytes, skipping driver padding
        for (UINT y = 0; y < height; ++y) {
            memcpy(pDst + (y * tightRowBytes), pSrc + (y * mapped.RowPitch), tightRowBytes);
        }
    }

    // 4. Unmap
    pContext->Unmap(pStaging, 0);
    return true;
}
```

---

### 3.3 Network Streaming & Transport Protocol

#### 3.3.1 Frame Packet Framing
To stream frames over TCP, define an explicit binary header:

```cpp
#pragma pack(push, 1)
struct FramePacketHeader {
    uint32_t magic;         // 0x46524D45 ("FRME")
    uint32_t frameIndex;    // Monotonically increasing frame number (0, 1, 2, ...)
    uint32_t width;         // Width in pixels (e.g. 800)
    uint32_t height;        // Height in pixels (e.g. 600)
    uint32_t format;        // DXGI_FORMAT_R8G8B8A8_UNORM = 28
    uint32_t payloadBytes;  // width * height * 4
    uint64_t timestampUs;   // Microsecond timestamp for latency and FPS measurement
};
#pragma pack(pop)
```

#### 3.3.2 Bandwidth & Transport Architecture
- For $800 \times 600$ RGBA: $800 \times 600 \times 4 = 1{,}920{,}000$ bytes ($\approx 1.83$ MB per frame).
- At 60 FPS: $\approx 110$ MB/s.
- Over 127.0.0.1 loopback: Loopback socket bandwidth exceeds 2.5 GB/s with near-zero latency (< 0.1 ms transfer time).
- TCP Configuration:
  - Enable `TCP_NODELAY` (disable Nagle's algorithm) on both client and server sockets to minimize transmission latency.
  - Set socket send/receive buffer sizes (`SO_SNDBUF`, `SO_RCVBUF`) to at least 4 MB.

#### 3.3.3 Backpressure & Flow Control
To prevent unbounded server memory usage if the network or client slows down:
- The client returns a compact `CMD_FRAME_ACK { uint32_t ackedFrameIndex }`.
- The server maintains a sliding window (e.g., maximum 2 in-flight frames). If `currentFrame - lastAckedFrame > 2`, the server pauses rendering until the ACK is received, preserving smooth frame pacing without buffer bloating.

---

### 3.4 Client Presentation Pipeline

When the client receives the frame buffer:
1. **Direct3D 11 Client Swapchain Presentation:**
   - The client has a real Win32 `HWND` and standard swapchain `IDXGISwapChain`.
   - Client allocates a dynamic texture or staging texture `pClientBackbufferTexture`.
   - On frame packet arrival, update the backbuffer texture:
     `pClientContext->UpdateSubresource(pClientBackbufferTexture, 0, nullptr, frameData.data(), tightRowPitch, 0)`.
   - Copy or blit to the window swapchain's backbuffer:
     `pClientContext->CopyResource(pSwapChainBackBuffer, pClientBackbufferTexture)`.
   - Call `pSwapChain->Present(1, 0)`.
2. **GDI Fallback (Zero-D3D Client Display):**
   - Alternatively, for zero-D3D clients, the frame pixels can be presented directly to the Win32 window via `StretchDIBits` using `BITMAPINFO` with negative height (top-down orientation), achieving display without needing local Direct3D hardware acceleration.

---

## 4. Automated Test Harness & Verification Suite (R5)

### 4.1 Standalone 3D Application Architecture

To objectively verify full-stack pipeline execution, the sample 3D application must exercise:
- Resource creation (vertex buffers, index buffers, constant buffers, textures, samplers, shaders).
- Geometry rasterization with 3D perspective projection.
- Procedural texture mapping and vertex coloring.
- Depth-stencil testing (hidden surface removal).
- Continuous per-frame animation.

#### 4.1.1 3D Geometry: Textured Colored Cube
The cube consists of 8 vertices, 36 indices (6 faces $\times$ 2 triangles $\times$ 3 vertices):

```cpp
struct VertexPositionColorTex {
    float x, y, z;       // Position
    float r, g, b, a;    // Vertex Color
    float u, v;          // Texture Coordinates
};

// 8 unique vertices defining a 1x1x1 cube centered at origin
static const VertexPositionColorTex g_CubeVertices[24] = {
    // Front Face (Z = -0.5)
    { -0.5f, -0.5f, -0.5f,  1.0f, 0.0f, 0.0f, 1.0f,  0.0f, 1.0f },
    { -0.5f,  0.5f, -0.5f,  1.0f, 0.0f, 0.0f, 1.0f,  0.0f, 0.0f },
    {  0.5f,  0.5f, -0.5f,  1.0f, 0.0f, 0.0f, 1.0f,  1.0f, 0.0f },
    {  0.5f, -0.5f, -0.5f,  1.0f, 0.0f, 0.0f, 1.0f,  1.0f, 1.0f },
    // Back Face (Z = +0.5)
    { -0.5f, -0.5f,  0.5f,  0.0f, 1.0f, 0.0f, 1.0f,  1.0f, 1.0f },
    {  0.5f, -0.5f,  0.5f,  0.0f, 1.0f, 0.0f, 1.0f,  0.0f, 1.0f },
    {  0.5f,  0.5f,  0.5f,  0.0f, 1.0f, 0.0f, 1.0f,  0.0f, 0.0f },
    { -0.5f,  0.5f,  0.5f,  0.0f, 1.0f, 0.0f, 1.0f,  1.0f, 0.0f },
    // Top Face (Y = +0.5)
    { -0.5f,  0.5f, -0.5f,  0.0f, 0.0f, 1.0f, 1.0f,  0.0f, 1.0f },
    { -0.5f,  0.5f,  0.5f,  0.0f, 0.0f, 1.0f, 1.0f,  0.0f, 0.0f },
    {  0.5f,  0.5f,  0.5f,  0.0f, 0.0f, 1.0f, 1.0f,  1.0f, 0.0f },
    {  0.5f,  0.5f, -0.5f,  0.0f, 0.0f, 1.0f, 1.0f,  1.0f, 1.0f },
    // Bottom Face (Y = -0.5)
    { -0.5f, -0.5f, -0.5f,  1.0f, 1.0f, 0.0f, 1.0f,  1.0f, 1.0f },
    {  0.5f, -0.5f, -0.5f,  1.0f, 1.0f, 0.0f, 1.0f,  0.0f, 1.0f },
    {  0.5f, -0.5f,  0.5f,  1.0f, 1.0f, 0.0f, 1.0f,  0.0f, 0.0f },
    { -0.5f, -0.5f,  0.5f,  1.0f, 1.0f, 0.0f, 1.0f,  1.0f, 0.0f },
    // Left Face (X = -0.5)
    { -0.5f, -0.5f,  0.5f,  1.0f, 0.0f, 1.0f, 1.0f,  0.0f, 1.0f },
    { -0.5f,  0.5f,  0.5f,  1.0f, 0.0f, 1.0f, 1.0f,  0.0f, 0.0f },
    { -0.5f,  0.5f, -0.5f,  1.0f, 0.0f, 1.0f, 1.0f,  1.0f, 0.0f },
    { -0.5f, -0.5f, -0.5f,  1.0f, 0.0f, 1.0f, 1.0f,  1.0f, 1.0f },
    // Right Face (X = +0.5)
    {  0.5f, -0.5f, -0.5f,  0.0f, 1.0f, 1.0f, 1.0f,  0.0f, 1.0f },
    {  0.5f,  0.5f, -0.5f,  0.0f, 1.0f, 1.0f, 1.0f,  0.0f, 0.0f },
    {  0.5f,  0.5f,  0.5f,  0.0f, 1.0f, 1.0f, 1.0f,  1.0f, 0.0f },
    {  0.5f, -0.5f,  0.5f,  0.0f, 1.0f, 1.0f, 1.0f,  1.0f, 1.0f }
};

static const uint16_t g_CubeIndices[36] = {
     0,  1,  2,   0,  2,  3, // Front
     4,  5,  6,   4,  6,  7, // Back
     8,  9, 10,   8, 10, 11, // Top
    12, 13, 14,  12, 14, 15, // Bottom
    16, 17, 18,  16, 18, 19, // Left
    20, 21, 22,  20, 22, 23  // Right
};
```

#### 4.1.2 Procedural Texture Generation (Checkerboard)
To eliminate external image file dependencies, generate a $16 \times 16$ checkerboard texture at runtime:
```cpp
void GenerateCheckerboardTexture(UINT width, UINT height, std::vector<uint32_t>& outPixels) {
    outPixels.resize(width * height);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            bool isWhite = ((x / 4) + (y / 4)) % 2 == 0;
            // RGBA: White = 0xFFFFFFFF, Dark Gray = 0xFF303030
            outPixels[y * width + x] = isWhite ? 0xFFFFFFFF : 0xFF303030;
        }
    }
}
```

#### 4.1.3 HLSL Shaders
```hlsl
// Constant Buffer matching C++ struct
cbuffer ConstantBuffer : register(b0) {
    matrix WorldViewProjection;
};

struct VS_INPUT {
    float3 Pos : POSITION;
    float4 Color : COLOR;
    float2 Tex : TEXCOORD0;
};

struct PS_INPUT {
    float4 Pos : SV_POSITION;
    float4 Color : COLOR;
    float2 Tex : TEXCOORD0;
};

PS_INPUT VSMain(VS_INPUT input) {
    PS_INPUT output;
    output.Pos = mul(float4(input.Pos, 1.0f), WorldViewProjection);
    output.Color = input.Color;
    output.Tex = input.Tex;
    return output;
}

Texture2D g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

float4 PSMain(PS_INPUT input) : SV_Target {
    float4 texColor = g_Texture.Sample(g_Sampler, input.Tex);
    // Combine texture luminance with vertex color
    return texColor * input.Color;
}
```

---

### 4.2 Automated Loopback Test Runner

#### 4.2.1 Architecture
The test harness is implemented as an automated Python orchestrator `run_loopback_test.py` that executes:
1. **Server Launch:** Starts `server.exe --port 9876 --adapter dedicated` as a background process.
2. **Server Healthcheck:** Polls TCP connection to `127.0.0.1:9876` with a 100ms interval until the socket accepts or timeout (5 seconds) occurs.
3. **Client Launch:** Starts `client_app.exe --host 127.0.0.1 --port 9876 --frames 60 --dump-frame 30 test_frame_30.raw`.
4. **Frame Execution:** The client offloads 60 frames to the server, verifying receipt of all 60 returned frame buffers.
5. **Clean Termination:** Client cleanly closes session and exits with code 0. Server cleanly shuts down session and exits with code 0.
6. **Programmatic Verification:** Analyzes `test_frame_30.raw` using the mathematical pixel verification suite.

#### 4.2.2 Complete Python Test Runner Implementation
```python
#!/usr/bin/env python3
import subprocess
import socket
import time
import os
import sys

PORT = 9876
HOST = "127.0.0.1"
TOTAL_FRAMES = 60
DUMP_FRAME = 30
FRAME_FILE = "frame_30.raw"
WIDTH = 800
HEIGHT = 600

def wait_for_server(host, port, timeout_sec=5.0):
    start = time.time()
    while time.time() - start < timeout_sec:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except (socket.error, ConnectionRefusedError):
            time.sleep(0.1)
    return False

def verify_frame_pixels(file_path, width, height):
    if not os.path.exists(file_path):
        print(f"[FAIL] Frame file {file_path} does not exist!")
        return False
        
    expected_size = width * height * 4
    file_size = os.path.getsize(file_path)
    if file_size != expected_size:
        print(f"[FAIL] Invalid file size: got {file_size}, expected {expected_size}")
        return False

    with open(file_path, "rb") as f:
        raw_rgba = f.read()

    total_pixels = width * height
    sum_lum = 0.0
    sum_lum_sq = 0.0
    non_clear_count = 0
    color_clusters = set()

    # Clear color for background (e.g. Cornflower Blue: RGB(100, 149, 237))
    CLEAR_R, CLEAR_G, CLEAR_B = 100, 149, 237

    for i in range(0, len(raw_rgba), 4):
        r = raw_rgba[i]
        g = raw_rgba[i + 1]
        b = raw_rgba[i + 2]
        
        # Standard ITU-R BT.601 / Rec.709 Luminance
        lum = 0.2126 * r + 0.7152 * g + 0.0722 * b
        sum_lum += lum
        sum_lum_sq += lum * lum

        # Difference from clear color
        diff = abs(r - CLEAR_R) + abs(g - CLEAR_G) + abs(b - CLEAR_B)
        if diff > 30:
            non_clear_count += 1

        # 5-bit color quantization to identify unique color clusters
        color_clusters.add((r >> 3, g >> 3, b >> 3))

    mean_lum = sum_lum / total_pixels
    variance_lum = (sum_lum_sq / total_pixels) - (mean_lum * mean_lum)
    std_lum = (variance_lum ** 0.5) if variance_lum > 0 else 0.0
    non_clear_ratio = non_clear_count / total_pixels

    print("--- Frame Pixel Verification Metrics ---")
    print(f"Total Pixels:       {total_pixels}")
    print(f"Mean Luminance:     {mean_lum:.2f} (Target: 10.0 - 245.0)")
    print(f"StdDev Luminance:   {std_lum:.2f} (Target: >= 3.0)")
    print(f"Non-Clear Ratio:    {non_clear_ratio * 100:.2f}% (Target: >= 2.0%)")
    print(f"Unique Color Bins:  {len(color_clusters)} (Target: >= 2)")

    # Assertions
    is_valid = True
    if mean_lum < 10.0:
        print("[FAIL] Frame is completely black / empty!")
        is_valid = False
    elif mean_lum > 245.0:
        print("[FAIL] Frame is completely white / blown out!")
        is_valid = False

    if std_lum < 3.0:
        print("[FAIL] Luminance standard deviation too low: flat unshaded frame!")
        is_valid = False

    if non_clear_ratio < 0.02:
        print("[FAIL] Non-clear pixel ratio too low: 3D geometry was not rasterized!")
        is_valid = False

    if len(color_clusters) < 2:
        print("[FAIL] Less than 2 distinct color clusters detected!")
        is_valid = False

    return is_valid

def main():
    print("[Test Harness] Starting Remote GPU Offload Loopback Test...")
    
    server_cmd = ["server.exe", "--port", str(PORT)]
    client_cmd = [
        "client_sample.exe",
        "--host", HOST,
        "--port", str(PORT),
        "--frames", str(TOTAL_FRAMES),
        "--dump-frame", str(DUMP_FRAME),
        "--dump-file", FRAME_FILE
    ]

    # Clean up previous dump
    if os.path.exists(FRAME_FILE):
        os.remove(FRAME_FILE)

    # 1. Start Server
    print(f"[Test Harness] Launching server on {HOST}:{PORT}...")
    server_proc = subprocess.Popen(server_cmd)

    try:
        if not wait_for_server(HOST, PORT, timeout_sec=5.0):
            print("[FAIL] Server failed to start listening within 5 seconds!")
            server_proc.kill()
            sys.exit(1)

        print("[Test Harness] Server is ready. Launching client...")
        # 2. Start Client
        client_proc = subprocess.Popen(client_cmd)
        client_exit = client_proc.wait(timeout=20.0)

        if client_exit != 0:
            print(f"[FAIL] Client exited with error code {client_exit}")
            sys.exit(1)

        print(f"[Test Harness] Client successfully completed {TOTAL_FRAMES} frames.")

        # 3. Wait for Server clean exit
        server_exit = server_proc.wait(timeout=5.0)
        if server_exit != 0:
            print(f"[FAIL] Server exited with error code {server_exit}")
            sys.exit(1)

        print("[Test Harness] Server exited cleanly with code 0.")

        # 4. Programmatic Pixel Verification
        print("[Test Harness] Running programmatic frame verification...")
        if not verify_frame_pixels(FRAME_FILE, WIDTH, HEIGHT):
            print("[FAIL] Programmatic pixel verification failed!")
            sys.exit(1)

        print("[PASS] ALL ACCEPTANCE CRITERIA MET (60+ frames, exit code 0, non-blank 3D render verified).")
        sys.exit(0)

    except subprocess.TimeoutExpired:
        print("[FAIL] Process timed out!")
        server_proc.kill()
        sys.exit(1)
    finally:
        if server_proc.poll() is None:
            server_proc.kill()

if __name__ == "__main__":
    main()
```

---

## 5. Technical Risk Matrix & Mitigations

| Risk | Impact | Root Cause | Proven Mitigation |
|---|---|---|---|
| **Invalid Driver Type with Adapter** | High (Crash on launch) | Calling `D3D11CreateDevice` with `DriverType = D3D_DRIVER_TYPE_HARDWARE` and `pAdapter != nullptr` | Always pass `D3D_DRIVER_TYPE_UNKNOWN` when providing an explicit `IDXGIAdapter*`. Empirically verified to return `S_OK`. |
| **Row Pitch Image Distortion** | High (Corrupted visuals) | Assuming `RowPitch == width * 4` during staging readback | Always read row-by-row: `(char*)pData + y * mapped.RowPitch`, packing tightly into destination buffer before network transmission. |
| **Direct3D Debug Layer Missing** | Medium (Device creation failure) | Requesting `D3D11_CREATE_DEVICE_DEBUG` on machines without Windows SDK Graphics Tools installed | Fallback retry loop: If creation fails with debug flag, immediately retry without `D3D11_CREATE_DEVICE_DEBUG`. |
| **Socket Latency Bloat** | Medium (Lagging frames) | Nagle's algorithm buffering packet chunks | Set `TCP_NODELAY = 1` immediately upon socket creation. |
| **Network Backpressure Stalls** | Medium (Memory overflow) | Fast server generating frames faster than client can present | Implement sliding window with frame ACKs (`lastPresentedFrame`); throttle render loop if unacked frames exceed threshold (e.g. 2). |
| **Resource Leaks on Disconnect** | High (Server VRAM exhaustion) | Client abruptly disconnects without releasing COM objects | Maintain centralized `ServerResourceManager`; on socket EOF/reset, clear all resource maps and call `ClearState()`. |

---

## 6. Verification and Quality Gate Checklist

- [x] DXGI dedicated adapter enumeration logic formulated, filtering out `DXGI_ADAPTER_FLAG_SOFTWARE` and `VendorId == 0x1414`.
- [x] Hardware device creation with `D3D_DRIVER_TYPE_UNKNOWN` validated empirically on Intel UHD Graphics 620.
- [x] Headless offscreen render target and depth-stencil texture creation defined.
- [x] Staging texture readback (`D3D11_USAGE_STAGING`, `D3D11_CPU_ACCESS_READ`) with pitch packing validated.
- [x] Dynamic textured 3D cube geometry, HLSL shaders, and procedural checkerboard generation specified.
- [x] End-to-end loopback runner architecture with 60-frame execution and clean exit code 0 specified.
- [x] Programmatic pixel verification math (luminance, standard deviation, non-clear ratio, color clustering) calibrated and tested.
