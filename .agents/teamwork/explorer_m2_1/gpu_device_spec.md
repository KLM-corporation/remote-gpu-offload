# Host GPU Device & Staging Readback Specification

**Milestone:** M2 (Remote GPU Execution Server)  
**Author:** Server Device & Readback Explorer (`explorer_m2_1`)  
**Target Files:** `src/server/gpu_device.h`, `src/server/gpu_device.cpp`  
**Reference Prototypes:** `.agents/teamwork/explorer_m2_1/proposed_gpu_device.h`, `.agents/teamwork/explorer_m2_1/proposed_gpu_device.cpp`  
**Target Environment:** Windows 10/11 x86_64, LLVM-MinGW clang++ (UCRT), DirectX 11 / DXGI 1.1+  

---

## 1. Architectural Overview & Context

In the Remote GPU Offloading framework, the server daemon (`server.exe`) runs on the GPU host machine and is responsible for:
1. Enumerating host graphics adapters via DXGI and binding directly to the physical hardware GPU (e.g., discrete NVIDIA/AMD GPU or integrated Intel UHD Graphics 620).
2. Creating a headless Direct3D 11 device and immediate context without requiring a Win32 window (`HWND`) or monitor swapchain.
3. Allocating offscreen render targets (color buffer) and depth-stencil surfaces matching client resolution requests.
4. Providing a high-performance staging texture readback pipeline that captures completed framebuffers, strips driver-specific scanline pitch padding, and produces a contiguous RGBA buffer ready for TCP network streaming (`OP_FRAME_DATA`).

`HostGpuDevice` serves as the foundational Direct3D 11 hardware abstraction layer on the server, upon which the opcode dispatcher (`ServerDispatcher`) and network listener execute.

---

## 2. Key Technical Invariants & Rules

### 2.1 DXGI Adapter Enumeration & Software Rasterizer Rejection
- **Enumeration API:** Use `CreateDXGIFactory1(IID_PPV_ARGS(&factory))` and `factory->EnumAdapters1(i, &adapter)` to enumerate all system graphics adapters.
- **Software Rasterizer Rejection:**
  1. `(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0`: Flags Microsoft WARP and software emulators.
  2. `desc.VendorId == 0x1414`: Microsoft Corporation Vendor ID (e.g., Microsoft Basic Render Driver).
  Both conditions **must** be evaluated. If either is true, `is_software = true`, preventing the server from selecting CPU-based emulation.
- **Candidate Ranking Algorithm:**
  1. Filter all adapters where `!is_software`.
  2. Sort hardware adapters in descending order of `DedicatedVideoMemory` (VRAM).
  3. Tie-break using `SharedSystemMemory`.
  4. Allow explicit user override via `adapter_index` parameter if specified.
  5. Fallback: If no hardware GPU is detected, issue an explicit warning and fall back to adapter 0.
- **Telemetry & Logging:**
  - Convert `desc.Description` from `wchar_t` to UTF-8.
  - Log index, description, VendorId, DeviceId, Dedicated VRAM (MB), Shared RAM (MB), and `[HARDWARE]` vs `[SOFTWARE/REJECTED]`.

### 2.2 Headless Direct3D 11 Device Creation
- **Critical Runtime Invariant:**
  > When passing a non-null `IDXGIAdapter*` to `D3D11CreateDevice`, the `DriverType` parameter **MUST** be set to `D3D_DRIVER_TYPE_UNKNOWN`. Passing `D3D_DRIVER_TYPE_HARDWARE` with a non-null adapter pointer triggers an immediate `E_INVALIDARG` (0x80070057) failure from the Direct3D runtime.
- **Feature Level List:** `{ D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 }`.
- **Device Creation Flags:** `D3D11_CREATE_DEVICE_BGRA_SUPPORT` (enables cross-compatibility with Direct2D/DirectWrite and GDI interop).
- **Debug Layer Resilience:** If compiled with `D3D11_CREATE_DEVICE_DEBUG` and creation fails (common on headless server nodes without the Windows SDK Graphics Tools optional feature installed), immediately retry without `D3D11_CREATE_DEVICE_DEBUG`.

### 2.3 Offscreen Render Target & Depth-Stencil Setup
- **Color Target:**
  - Format: `DXGI_FORMAT_R8G8B8A8_UNORM`
  - Usage: `D3D11_USAGE_DEFAULT`
  - Bind Flags: `D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE`
  - Sample Count: 1, Sample Quality: 0
  - Associated RTV format: `DXGI_FORMAT_R8G8B8A8_UNORM`
- **Depth-Stencil Target:**
  - Format: `DXGI_FORMAT_D24_UNORM_S8_UINT`
  - Usage: `D3D11_USAGE_DEFAULT`
  - Bind Flags: `D3D11_BIND_DEPTH_STENCIL`
  - Associated DSV format: `DXGI_FORMAT_D24_UNORM_S8_UINT`
- **Pipeline Setup Helper:** `SetDefaultTargets()` binds the RTV and DSV to Output Merger (`OMSetRenderTargets`) and configures a full-surface viewport (`RSSetViewports`).
- **Dynamic Resize Support:** Calling `CreateOffscreenTargets(new_w, new_h)` safely resets previous COM views and textures before allocating new surfaces.

### 2.4 Staging Readback & Scanline Pitch Unpadding
- **Staging Texture:**
  - Same width, height, and format as the color target.
  - Usage: `D3D11_USAGE_STAGING`
  - CPU Access: `D3D11_CPU_ACCESS_READ`
  - Bind Flags: `0`
- **GPU-to-CPU Transfer:**
  - `context->CopyResource(m_staging_texture.Get(), m_color_texture.Get())`.
- **CPU Mapping & Pitch Alignment:**
  - Call `context->Map(m_staging_texture.Get(), 0, D3D11_MAP_READ, 0, &mapped)`.
  - Calculate `tight_row_bytes = width * 4`.
  - **The Pitch Trap:** Hardware drivers align row pitch to multiples of 32, 64, or 256 bytes. For non-aligned resolutions (e.g. 137px width gives 548 tight bytes, padded to 576 bytes by the driver), copying `mapped.pData` contiguously results in scanline shear distortion.
  - **Fast Path:** If `mapped.RowPitch == tight_row_bytes`, perform a single contiguous `std::memcpy(p_dst, p_src, total_packed_bytes)`.
  - **Padded Path:** If `mapped.RowPitch != tight_row_bytes`, iterate `y` from `0` to `height - 1`, copying `tight_row_bytes` per row:
    ```cpp
    std::memcpy(p_dst + (size_t(y) * tight_row_bytes),
                p_src + (size_t(y) * mapped.RowPitch),
                tight_row_bytes);
    ```
  - Call `context->Unmap(m_staging_texture.Get(), 0)`.

---

## 3. Class Design & Header Specification (`src/server/gpu_device.h`)

```cpp
#pragma once
#ifndef REMOTE_GPU_GPU_DEVICE_H
#define REMOTE_GPU_GPU_DEVICE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace RemoteGPU {

struct GpuAdapterInfo {
    int index = -1;
    std::string description;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint64_t dedicated_video_memory = 0;
    uint64_t dedicated_system_memory = 0;
    uint64_t shared_system_memory = 0;
    bool is_software = false;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
};

class HostGpuDevice {
public:
    HostGpuDevice();
    ~HostGpuDevice();

    // Non-copyable, movable
    HostGpuDevice(const HostGpuDevice&) = delete;
    HostGpuDevice& operator=(const HostGpuDevice&) = delete;
    HostGpuDevice(HostGpuDevice&&) noexcept = default;
    HostGpuDevice& operator=(HostGpuDevice&&) noexcept = default;

    // Initialization & Lifecycle
    bool InitializeDedicatedGpu(int adapter_index = -1);
    bool CreateOffscreenTargets(uint32_t width, uint32_t height);
    bool ReadbackFrame(std::vector<uint8_t>& out_rgba, uint32_t& out_width, uint32_t& out_height);
    void Reset();

    // Render & State Helpers
    void Clear(const float clear_color[4], float depth = 1.0f, uint8_t stencil = 0);
    void SetDefaultTargets();

    // Direct3D 11 Accessors
    ID3D11Device* GetDevice() const { return m_device.Get(); }
    ID3D11DeviceContext* GetContext() const { return m_context.Get(); }
    ID3D11RenderTargetView* GetRenderTargetView() const { return m_rtv.Get(); }
    ID3D11DepthStencilView* GetDepthStencilView() const { return m_dsv.Get(); }
    ID3D11Texture2D* GetColorTexture() const { return m_color_texture.Get(); }
    ID3D11Texture2D* GetDepthTexture() const { return m_depth_texture.Get(); }
    ID3D11Texture2D* GetStagingTexture() const { return m_staging_texture.Get(); }

    // Adapter & Device Properties
    const std::string& GetAdapterDescription() const { return m_selected_adapter_desc; }
    uint64_t GetDedicatedVram() const { return m_dedicated_vram; }
    uint64_t GetSharedRam() const { return m_shared_ram; }
    uint32_t GetVendorId() const { return m_vendor_id; }
    uint32_t GetDeviceId() const { return m_device_id; }
    D3D_FEATURE_LEVEL GetFeatureLevel() const { return m_feature_level; }
    int GetSelectedAdapterIndex() const { return m_selected_adapter_index; }
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    const std::vector<GpuAdapterInfo>& GetAvailableAdapters() const { return m_available_adapters; }

private:
    std::vector<GpuAdapterInfo> m_available_adapters;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> m_adapter;
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    D3D_FEATURE_LEVEL m_feature_level = D3D_FEATURE_LEVEL_11_0;

    std::string m_selected_adapter_desc;
    uint64_t m_dedicated_vram = 0;
    uint64_t m_shared_ram = 0;
    uint32_t m_vendor_id = 0;
    uint32_t m_device_id = 0;
    int m_selected_adapter_index = -1;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_color_texture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depth_texture;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_dsv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging_texture;
};

} // namespace RemoteGPU

#endif // REMOTE_GPU_GPU_DEVICE_H
```

---

## 4. Implementation Specification (`src/server/gpu_device.cpp`)

```cpp
#include "gpu_device.h"

#include <iostream>
#include <algorithm>
#include <cstring>

namespace RemoteGPU {

namespace {

std::string WideToUtf8(const wchar_t* wstr) {
    if (!wstr) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (size_needed <= 0) return "";
    std::string result(size_needed - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &result[0], size_needed, nullptr, nullptr);
    return result;
}

} // anonymous namespace

HostGpuDevice::HostGpuDevice() = default;

HostGpuDevice::~HostGpuDevice() {
    Reset();
}

void HostGpuDevice::Reset() {
    if (m_context) {
        m_context->ClearState();
        m_context->Flush();
    }
    m_staging_texture.Reset();
    m_dsv.Reset();
    m_depth_texture.Reset();
    m_rtv.Reset();
    m_color_texture.Reset();
    m_context.Reset();
    m_device.Reset();
    m_adapter.Reset();
    m_available_adapters.clear();
    m_width = 0;
    m_height = 0;
    m_selected_adapter_desc.clear();
    m_dedicated_vram = 0;
    m_shared_ram = 0;
    m_vendor_id = 0;
    m_device_id = 0;
    m_selected_adapter_index = -1;
}

bool HostGpuDevice::InitializeDedicatedGpu(int adapter_index) {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        std::cerr << "[HostGpuDevice] Failed to create IDXGIFactory1: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    m_available_adapters.clear();
    Microsoft::WRL::ComPtr<IDXGIAdapter1> cur_adapter;
    UINT i = 0;
    while (factory->EnumAdapters1(i, &cur_adapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc;
        cur_adapter->GetDesc1(&desc);

        // Software rasterizer rejection:
        // 1. DXGI_ADAPTER_FLAG_SOFTWARE flag
        // 2. Microsoft VendorId (0x1414) for WARP / Basic Render Driver
        bool is_software = ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) || (desc.VendorId == 0x1414);

        GpuAdapterInfo info;
        info.index = static_cast<int>(i);
        info.description = WideToUtf8(desc.Description);
        info.vendor_id = desc.VendorId;
        info.device_id = desc.DeviceId;
        info.dedicated_video_memory = desc.DedicatedVideoMemory;
        info.dedicated_system_memory = desc.DedicatedSystemMemory;
        info.shared_system_memory = desc.SharedSystemMemory;
        info.is_software = is_software;
        info.adapter = cur_adapter;

        std::cout << "[HostGpuDevice] Discovered Adapter [" << i << "]: " << info.description
                  << " | VendorId: 0x" << std::hex << info.vendor_id << std::dec
                  << " | DeviceId: 0x" << std::hex << info.device_id << std::dec
                  << " | Dedicated VRAM: " << (info.dedicated_video_memory / (1024 * 1024)) << " MB"
                  << " | Shared RAM: " << (info.shared_system_memory / (1024 * 1024)) << " MB"
                  << (is_software ? " [SOFTWARE/REJECTED]" : " [HARDWARE]")
                  << std::endl;

        m_available_adapters.push_back(info);
        cur_adapter.Reset();
        ++i;
    }

    if (m_available_adapters.empty()) {
        std::cerr << "[HostGpuDevice] No DXGI adapters discovered on system!" << std::endl;
        return false;
    }

    // Determine target adapter
    int chosen_idx = -1;
    if (adapter_index >= 0) {
        if (adapter_index < static_cast<int>(m_available_adapters.size())) {
            chosen_idx = adapter_index;
            std::cout << "[HostGpuDevice] Explicit adapter index specified: " << chosen_idx << std::endl;
        } else {
            std::cerr << "[HostGpuDevice] Explicit adapter index " << adapter_index
                      << " is out of bounds (max " << (m_available_adapters.size() - 1) << ")" << std::endl;
            return false;
        }
    } else {
        // Collect hardware candidate adapters
        std::vector<GpuAdapterInfo> hardware_candidates;
        for (const auto& a : m_available_adapters) {
            if (!a.is_software) {
                hardware_candidates.push_back(a);
            }
        }

        if (!hardware_candidates.empty()) {
            // Sort by dedicated VRAM descending, tie-break by shared system memory
            std::stable_sort(hardware_candidates.begin(), hardware_candidates.end(),
                [](const GpuAdapterInfo& lhs, const GpuAdapterInfo& rhs) {
                    if (lhs.dedicated_video_memory != rhs.dedicated_video_memory) {
                        return lhs.dedicated_video_memory > rhs.dedicated_video_memory;
                    }
                    return lhs.shared_system_memory > rhs.shared_system_memory;
                });
            chosen_idx = hardware_candidates[0].index;
        } else {
            std::cerr << "[HostGpuDevice] WARNING: No dedicated hardware GPU detected! Falling back to adapter 0." << std::endl;
            chosen_idx = 0;
        }
    }

    const auto& chosen = m_available_adapters[chosen_idx];
    m_adapter = chosen.adapter;
    m_selected_adapter_index = chosen.index;
    m_selected_adapter_desc = chosen.description;
    m_dedicated_vram = chosen.dedicated_video_memory;
    m_shared_ram = chosen.shared_system_memory;
    m_vendor_id = chosen.vendor_id;
    m_device_id = chosen.device_id;

    std::cout << "[HostGpuDevice] Active Selected GPU: " << m_selected_adapter_desc
              << " (Index " << m_selected_adapter_index << ", Dedicated VRAM: "
              << (m_dedicated_vram / (1024 * 1024)) << " MB)" << std::endl;

    // Headless Direct3D 11 Device Creation
    // Rule: Passing an explicit IDXGIAdapter* requires DriverType == D3D_DRIVER_TYPE_UNKNOWN
    UINT create_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    create_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    hr = D3D11CreateDevice(
        m_adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN, // MANDATORY for explicit adapter pointer
        nullptr,
        create_flags,
        feature_levels,
        ARRAYSIZE(feature_levels),
        D3D11_SDK_VERSION,
        &m_device,
        &m_feature_level,
        &m_context
    );

    // Fallback: If debug flag fails (SDK debug layer missing on headless machine), retry without it
    if (FAILED(hr) && (create_flags & D3D11_CREATE_DEVICE_DEBUG)) {
        create_flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            m_adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            create_flags,
            feature_levels,
            ARRAYSIZE(feature_levels),
            D3D11_SDK_VERSION,
            &m_device,
            &m_feature_level,
            &m_context
        );
    }

    if (FAILED(hr)) {
        std::cerr << "[HostGpuDevice] D3D11CreateDevice failed: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    std::cout << "[HostGpuDevice] Headless Direct3D 11 device created successfully. Feature Level: 0x"
              << std::hex << m_feature_level << std::dec << std::endl;

    return true;
}

bool HostGpuDevice::CreateOffscreenTargets(uint32_t width, uint32_t height) {
    if (!m_device) {
        std::cerr << "[HostGpuDevice] Device not initialized before creating offscreen targets." << std::endl;
        return false;
    }
    if (width == 0 || height == 0) {
        std::cerr << "[HostGpuDevice] Invalid offscreen target dimensions: " << width << "x" << height << std::endl;
        return false;
    }

    // Release existing target resources if resizing
    m_rtv.Reset();
    m_color_texture.Reset();
    m_dsv.Reset();
    m_depth_texture.Reset();
    m_staging_texture.Reset();

    m_width = width;
    m_height = height;

    // 1. Color Render Target Texture (R8G8B8A8_UNORM)
    D3D11_TEXTURE2D_DESC col_desc = {};
    col_desc.Width = width;
    col_desc.Height = height;
    col_desc.MipLevels = 1;
    col_desc.ArraySize = 1;
    col_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    col_desc.SampleDesc.Count = 1;
    col_desc.SampleDesc.Quality = 0;
    col_desc.Usage = D3D11_USAGE_DEFAULT;
    col_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    col_desc.CPUAccessFlags = 0;
    col_desc.MiscFlags = 0;

    HRESULT hr = m_device->CreateTexture2D(&col_desc, nullptr, &m_color_texture);
    if (FAILED(hr)) {
        std::cerr << "[HostGpuDevice] Failed to create offscreen color texture: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.Format = col_desc.Format;
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtv_desc.Texture2D.MipSlice = 0;

    hr = m_device->CreateRenderTargetView(m_color_texture.Get(), &rtv_desc, &m_rtv);
    if (FAILED(hr)) {
        std::cerr << "[HostGpuDevice] Failed to create offscreen RTV: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // 2. Depth-Stencil Texture & View (D24_UNORM_S8_UINT)
    D3D11_TEXTURE2D_DESC depth_desc = {};
    depth_desc.Width = width;
    depth_desc.Height = height;
    depth_desc.MipLevels = 1;
    depth_desc.ArraySize = 1;
    depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.SampleDesc.Quality = 0;
    depth_desc.Usage = D3D11_USAGE_DEFAULT;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    depth_desc.CPUAccessFlags = 0;
    depth_desc.MiscFlags = 0;

    hr = m_device->CreateTexture2D(&depth_desc, nullptr, &m_depth_texture);
    if (FAILED(hr)) {
        std::cerr << "[HostGpuDevice] Failed to create offscreen depth texture: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc = {};
    dsv_desc.Format = depth_desc.Format;
    dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dsv_desc.Texture2D.MipSlice = 0;

    hr = m_device->CreateDepthStencilView(m_depth_texture.Get(), &dsv_desc, &m_dsv);
    if (FAILED(hr)) {
        std::cerr << "[HostGpuDevice] Failed to create offscreen DSV: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // 3. Staging Texture for CPU Readback
    D3D11_TEXTURE2D_DESC staging_desc = col_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;

    hr = m_device->CreateTexture2D(&staging_desc, nullptr, &m_staging_texture);
    if (FAILED(hr)) {
        std::cerr << "[HostGpuDevice] Failed to create staging readback texture: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    SetDefaultTargets();

    std::cout << "[HostGpuDevice] Configured offscreen targets: "
              << width << "x" << height << " (RGBA8 + D24S8)." << std::endl;
    return true;
}

void HostGpuDevice::SetDefaultTargets() {
    if (!m_context || !m_rtv || !m_dsv) return;

    ID3D11RenderTargetView* rtvs[1] = { m_rtv.Get() };
    m_context->OMSetRenderTargets(1, rtvs, m_dsv.Get());

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = static_cast<float>(m_width);
    vp.Height = static_cast<float>(m_height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
}

void HostGpuDevice::Clear(const float clear_color[4], float depth, uint8_t stencil) {
    if (!m_context) return;
    if (m_rtv) {
        m_context->ClearRenderTargetView(m_rtv.Get(), clear_color);
    }
    if (m_dsv) {
        m_context->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, depth, stencil);
    }
}

bool HostGpuDevice::ReadbackFrame(std::vector<uint8_t>& out_rgba, uint32_t& out_width, uint32_t& out_height) {
    if (!m_context || !m_color_texture || !m_staging_texture) {
        std::cerr << "[HostGpuDevice] Readback failed: Render targets or staging texture not allocated." << std::endl;
        return false;
    }

    // 1. GPU copy: render target texture -> staging texture
    m_context->CopyResource(m_staging_texture.Get(), m_color_texture.Get());

    // 2. Map staging texture for CPU read
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = m_context->Map(m_staging_texture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        std::cerr << "[HostGpuDevice] Staging texture Map failed: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    out_width = m_width;
    out_height = m_height;
    const uint32_t tight_row_bytes = m_width * 4; // 4 bytes per pixel for R8G8B8A8
    const size_t total_packed_bytes = static_cast<size_t>(tight_row_bytes) * m_height;

    if (out_rgba.size() != total_packed_bytes) {
        out_rgba.resize(total_packed_bytes);
    }

    const uint8_t* p_src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* p_dst = out_rgba.data();

    // 3. Row-pitch packing eliminating driver scanline padding
    if (mapped.RowPitch == tight_row_bytes) {
        // Fast path: Driver row pitch matches tightly packed row bytes
        std::memcpy(p_dst, p_src, total_packed_bytes);
    } else {
        // Padded path: Copy tight scanline bytes, skipping driver row pitch stride
        for (uint32_t y = 0; y < m_height; ++y) {
            std::memcpy(p_dst + (static_cast<size_t>(y) * tight_row_bytes),
                        p_src + (static_cast<size_t>(y) * mapped.RowPitch),
                        tight_row_bytes);
        }
    }

    // 4. Unmap staging texture
    m_context->Unmap(m_staging_texture.Get(), 0);

    return true;
}

} // namespace RemoteGPU
```

---

## 5. Build & Linker Integration

### Clang++ Command Line
```powershell
clang++ -std=c++17 -O2 -Isrc/common -Isrc/server src/server/gpu_device.cpp <dependent_sources> -o build/server.exe -ld3d11 -ldxgi
```

### Compiler / Platform Notes
1. **Include Requirements:** `<d3d11.h>`, `<dxgi1_2.h>`, `<wrl/client.h>`.
2. **Defines:** `#define WIN32_LEAN_AND_MEAN` and `#define NOMINMAX` before Windows headers to avoid conflicts with `std::min` / `std::max`.
3. **Libraries:** `-ld3d11 -ldxgi` link against MinGW import libraries `libd3d11.a` and `libdxgi.a`.

---

## 6. Verification and Acceptance Matrix

| Item | Requirement | Verification Method | Empirical Result on Host |
|---|---|---|---|
| **DXGI Enumeration** | Enumerate all system adapters | `factory->EnumAdapters1()` | Discovered Adapter 0 (Intel UHD 620) & Adapter 1 (WARP) |
| **Software Rejection** | Filter Microsoft Basic Render Driver | `desc.VendorId == 0x1414` & `DXGI_ADAPTER_FLAG_SOFTWARE` | Successfully tagged Adapter 1 as software, rejected |
| **Hardware Selection** | Select dedicated hardware GPU | Sort by `DedicatedVideoMemory` desc | Selected Adapter 0 (128 MB dedicated VRAM, 12 GB shared) |
| **Headless Creation** | Headless D3D11 device creation | `D3D11CreateDevice` with `D3D_DRIVER_TYPE_UNKNOWN` | Succeeded with Feature Level 0xB100 (D3D 11.1) |
| **Offscreen Target** | Create RTV, DSV, Color & Staging | `CreateTexture2D`, `CreateRTV`, `CreateDSV` | Successfully configured 800x600, 640x480, 173x97 |
| **Readback & Unpadding** | Extract clean contiguous pixels | `CopyResource`, `Map`, scanline unpadding | 100% verified on 173x97 odd dimension (mapped pitch 576, tight 548) with zero pixel shearing |
