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

#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <cstdint>

using Microsoft::WRL::ComPtr;

struct GpuAdapterInfo {
    int index = -1;
    std::string description;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint64_t dedicated_video_memory = 0;
    uint64_t dedicated_system_memory = 0;
    uint64_t shared_system_memory = 0;
    bool is_software = false;
    ComPtr<IDXGIAdapter1> adapter;
};

class HostGpuDevice {
public:
    HostGpuDevice() = default;
    ~HostGpuDevice() = default;

    bool InitializeDedicatedGpu(int adapter_index = -1);
    bool CreateOffscreenTargets(uint32_t width, uint32_t height);
    bool ReadbackFrame(std::vector<uint8_t>& out_rgba, uint32_t& out_width, uint32_t& out_height);

    ID3D11Device* GetDevice() const { return m_device.Get(); }
    ID3D11DeviceContext* GetContext() const { return m_context.Get(); }
    ID3D11RenderTargetView* GetRenderTargetView() const { return m_rtv.Get(); }
    ID3D11DepthStencilView* GetDepthStencilView() const { return m_dsv.Get(); }
    ID3D11Texture2D* GetColorTexture() const { return m_color_texture.Get(); }
    ID3D11Texture2D* GetDepthTexture() const { return m_depth_texture.Get(); }
    ID3D11Texture2D* GetStagingTexture() const { return m_staging_texture.Get(); }

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

    void Clear(const float clear_color[4], float depth = 1.0f, uint8_t stencil = 0);
    void SetDefaultTargets();

private:
    std::vector<GpuAdapterInfo> m_available_adapters;
    ComPtr<IDXGIAdapter1> m_adapter;
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    D3D_FEATURE_LEVEL m_feature_level = D3D_FEATURE_LEVEL_11_0;

    std::string m_selected_adapter_desc;
    uint64_t m_dedicated_vram = 0;
    uint64_t m_shared_ram = 0;
    uint32_t m_vendor_id = 0;
    uint32_t m_device_id = 0;
    int m_selected_adapter_index = -1;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    ComPtr<ID3D11Texture2D> m_color_texture;
    ComPtr<ID3D11RenderTargetView> m_rtv;
    ComPtr<ID3D11Texture2D> m_depth_texture;
    ComPtr<ID3D11DepthStencilView> m_dsv;
    ComPtr<ID3D11Texture2D> m_staging_texture;
};

static std::string WideToUtf8(const wchar_t* wstr) {
    if (!wstr) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (size_needed <= 0) return "";
    std::string result(size_needed - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &result[0], size_needed, nullptr, nullptr);
    return result;
}

bool HostGpuDevice::InitializeDedicatedGpu(int adapter_index) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        std::cerr << "[HostGpuDevice] Failed to create IDXGIFactory1: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    m_available_adapters.clear();
    ComPtr<IDXGIAdapter1> cur_adapter;
    UINT i = 0;
    while (factory->EnumAdapters1(i, &cur_adapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc;
        cur_adapter->GetDesc1(&desc);

        // Software rasterizer rejection:
        // 1. DXGI_ADAPTER_FLAG_SOFTWARE flag
        // 2. Microsoft VendorId (0x1414)
        bool is_software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || (desc.VendorId == 0x1414);

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

        std::cout << "[HostGpuDevice] Found Adapter [" << i << "]: " << info.description
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
            std::cout << "[HostGpuDevice] User explicitly requested adapter index: " << chosen_idx << std::endl;
        } else {
            std::cerr << "[HostGpuDevice] Explicit adapter index " << adapter_index
                      << " is out of range (max " << (m_available_adapters.size() - 1) << ")" << std::endl;
            return false;
        }
    } else {
        // Filter hardware adapters and sort by dedicated video memory descending
        std::vector<GpuAdapterInfo> hardware_candidates;
        for (const auto& a : m_available_adapters) {
            if (!a.is_software) {
                hardware_candidates.push_back(a);
            }
        }

        if (!hardware_candidates.empty()) {
            std::stable_sort(hardware_candidates.begin(), hardware_candidates.end(),
                [](const GpuAdapterInfo& lhs, const GpuAdapterInfo& rhs) {
                    if (lhs.dedicated_video_memory != rhs.dedicated_video_memory) {
                        return lhs.dedicated_video_memory > rhs.dedicated_video_memory;
                    }
                    return lhs.shared_system_memory > rhs.shared_system_memory;
                });
            chosen_idx = hardware_candidates[0].index;
        } else {
            std::cerr << "[HostGpuDevice] WARNING: No hardware GPU found! Falling back to adapter 0." << std::endl;
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

    std::cout << "[HostGpuDevice] Selected Adapter [" << m_selected_adapter_index << "]: "
              << m_selected_adapter_desc << " (" << (m_dedicated_vram / (1024 * 1024)) << " MB VRAM)" << std::endl;

    // Headless Direct3D 11 Device Creation
    // Rule: When passing an explicit IDXGIAdapter*, DriverType MUST be D3D_DRIVER_TYPE_UNKNOWN
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

    // If debug layer failed (e.g. SDK layers not installed on headless host), retry without it
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
        std::cerr << "[HostGpuDevice] D3D11CreateDevice failed with HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
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

    // Reset previous targets if resizing
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
        std::cerr << "[HostGpuDevice] Failed to create offscreen color texture: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.Format = col_desc.Format;
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtv_desc.Texture2D.MipSlice = 0;

    hr = m_device->CreateRenderTargetView(m_color_texture.Get(), &rtv_desc, &m_rtv);
    if (FAILED(hr)) {
        std::cerr << "[HostGpuDevice] Failed to create offscreen RTV: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // 2. Depth-Stencil Texture (D24_UNORM_S8_UINT)
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
        std::cerr << "[HostGpuDevice] Failed to create offscreen depth texture: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc = {};
    dsv_desc.Format = depth_desc.Format;
    dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dsv_desc.Texture2D.MipSlice = 0;

    hr = m_device->CreateDepthStencilView(m_depth_texture.Get(), &dsv_desc, &m_dsv);
    if (FAILED(hr)) {
        std::cerr << "[HostGpuDevice] Failed to create offscreen DSV: 0x" << std::hex << hr << std::dec << std::endl;
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
        std::cerr << "[HostGpuDevice] Failed to create staging readback texture: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    SetDefaultTargets();

    std::cout << "[HostGpuDevice] Offscreen render targets created successfully ("
              << width << "x" << height << ", RGBA8 + D24S8)." << std::endl;
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
        std::cerr << "[HostGpuDevice] Cannot readback: resources not initialized." << std::endl;
        return false;
    }

    // 1. Copy GPU render target texture to CPU staging texture
    m_context->CopyResource(m_staging_texture.Get(), m_color_texture.Get());

    // 2. Map staging texture for CPU read
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = m_context->Map(m_staging_texture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        std::cerr << "[HostGpuDevice] Staging texture Map failed: 0x" << std::hex << hr << std::dec << std::endl;
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

    // 3. Row-pitch packing eliminating scanline padding
    if (mapped.RowPitch == tight_row_bytes) {
        // Fast path: Driver row pitch is identical to tightly packed width * 4
        std::memcpy(p_dst, p_src, total_packed_bytes);
    } else {
        // Padded path: Copy scanline by scanline, skipping driver row pitch stride padding
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

int main() {
    std::cout << "============================================================" << std::endl;
    std::cout << "[Test] HostGpuDevice Direct3D 11 Headless & Readback Test" << std::endl;
    std::cout << "============================================================" << std::endl;

    HostGpuDevice gpu;
    if (!gpu.InitializeDedicatedGpu()) {
        std::cerr << "[FAIL] Failed to initialize dedicated GPU!" << std::endl;
        return 1;
    }

    std::cout << "[PASS] InitializeDedicatedGpu succeeded on: " << gpu.GetAdapterDescription() << std::endl;

    constexpr uint32_t TEST_WIDTH = 800;
    constexpr uint32_t TEST_HEIGHT = 600;
    if (!gpu.CreateOffscreenTargets(TEST_WIDTH, TEST_HEIGHT)) {
        std::cerr << "[FAIL] Failed to create offscreen targets!" << std::endl;
        return 1;
    }

    std::cout << "[PASS] CreateOffscreenTargets (800x600) succeeded." << std::endl;

    // Test Clear to Cornflower Blue: RGBA(0.392f, 0.584f, 0.929f, 1.0f) => RGB(100, 149, 237, 255)
    const float cornflower_blue[4] = { 0.392156899f, 0.584313750f, 0.929411829f, 1.0f };
    gpu.Clear(cornflower_blue);

    // Readback
    std::vector<uint8_t> frame_pixels;
    uint32_t rb_width = 0, rb_height = 0;
    if (!gpu.ReadbackFrame(frame_pixels, rb_width, rb_height)) {
        std::cerr << "[FAIL] ReadbackFrame failed!" << std::endl;
        return 1;
    }

    std::cout << "[PASS] ReadbackFrame returned " << frame_pixels.size() << " bytes." << std::endl;
    if (rb_width != TEST_WIDTH || rb_height != TEST_HEIGHT) {
        std::cerr << "[FAIL] Readback dimensions mismatch: got " << rb_width << "x" << rb_height << std::endl;
        return 1;
    }

    // Inspect first pixel and center pixel
    uint8_t r0 = frame_pixels[0];
    uint8_t g0 = frame_pixels[1];
    uint8_t b0 = frame_pixels[2];
    uint8_t a0 = frame_pixels[3];
    std::cout << "Top-left pixel [0, 0]: R=" << (int)r0 << " G=" << (int)g0 << " B=" << (int)b0 << " A=" << (int)a0 << std::endl;

    size_t center_idx = ((TEST_HEIGHT / 2) * TEST_WIDTH + (TEST_WIDTH / 2)) * 4;
    uint8_t rc = frame_pixels[center_idx + 0];
    uint8_t gc = frame_pixels[center_idx + 1];
    uint8_t bc = frame_pixels[center_idx + 2];
    uint8_t ac = frame_pixels[center_idx + 3];
    std::cout << "Center pixel [" << (TEST_WIDTH/2) << ", " << (TEST_HEIGHT/2) << "]: R=" << (int)rc << " G=" << (int)gc << " B=" << (int)bc << " A=" << (int)ac << std::endl;

    // Verify expected clear color: 100, 149, 237, 255 (allow +/- 1 tolerance for floating point rounding)
    auto check_color = [](uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        return (std::abs((int)r - 100) <= 1) &&
               (std::abs((int)g - 149) <= 1) &&
               (std::abs((int)b - 237) <= 1) &&
               (a == 255);
    };

    if (!check_color(r0, g0, b0, a0) || !check_color(rc, gc, bc, ac)) {
        std::cerr << "[FAIL] Pixel color mismatch! Expected ~100, 149, 237, 255" << std::endl;
        return 1;
    }

    std::cout << "[PASS] Pixel color matches expected Cornflower Blue." << std::endl;
    std::cout << "[SUCCESS] All HostGpuDevice empirical checks PASSED!" << std::endl;

    return 0;
}
