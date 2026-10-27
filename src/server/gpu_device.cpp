#include "gpu_device.h"

#include <iostream>
#include <algorithm>
#include <cstring>
#include <cstdlib>

namespace RemoteGPU {

namespace {

std::string WideToUtf8(const wchar_t* wstr) {
    if (!wstr) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (size_needed <= 0) return "";
    std::string result(static_cast<size_t>(size_needed - 1), 0);
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
        DXGI_ADAPTER_DESC1 desc = {};
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

    const auto& chosen = m_available_adapters[static_cast<size_t>(chosen_idx)];
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
    // RGPU_DEBUG_LAYER=1 enables the Direct3D debug layer (needs the Windows "Graphics Tools"
    // optional feature); its errors are printed, de-duplicated, with the status line.
    if (const char* dbg = std::getenv("RGPU_DEBUG_LAYER")) {
        if (dbg[0] == '1') create_flags |= D3D11_CREATE_DEVICE_DEBUG;
    }

    const D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    constexpr UINT num_feature_levels = static_cast<UINT>(sizeof(feature_levels) / sizeof(feature_levels[0]));

    hr = D3D11CreateDevice(
        m_adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN, // MANDATORY for explicit adapter pointer
        nullptr,
        create_flags,
        feature_levels,
        num_feature_levels,
        D3D11_SDK_VERSION,
        &m_device,
        &m_feature_level,
        &m_context
    );

    // Fallback 1: If debug flag fails (SDK debug layer missing on machine), retry without it
    if (FAILED(hr) && (create_flags & D3D11_CREATE_DEVICE_DEBUG)) {
        create_flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            m_adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            create_flags,
            feature_levels,
            num_feature_levels,
            D3D11_SDK_VERSION,
            &m_device,
            &m_feature_level,
            &m_context
        );
    }

    // Fallback 2: If explicit adapter pointer creation fails, try default hardware device
    if (FAILED(hr)) {
        std::cerr << "[HostGpuDevice] Retrying D3D11CreateDevice with D3D_DRIVER_TYPE_HARDWARE..." << std::endl;
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            create_flags & ~D3D11_CREATE_DEVICE_DEBUG,
            feature_levels,
            num_feature_levels,
            D3D11_SDK_VERSION,
            &m_device,
            &m_feature_level,
            &m_context
        );
    }

    // Fallback 3: Try WARP software fallback if hardware device is unavailable
    if (FAILED(hr)) {
        std::cerr << "[HostGpuDevice] Retrying D3D11CreateDevice with D3D_DRIVER_TYPE_WARP..." << std::endl;
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            create_flags & ~D3D11_CREATE_DEVICE_DEBUG,
            feature_levels,
            num_feature_levels,
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

    if (create_flags & D3D11_CREATE_DEVICE_DEBUG) {
        if (SUCCEEDED(m_device.As(&m_info_queue))) {
            std::cout << "[HostGpuDevice] Direct3D debug layer ACTIVE (RGPU_DEBUG_LAYER=1)" << std::endl;
        }
    } else if (std::getenv("RGPU_DEBUG_LAYER")) {
        std::cout << "[HostGpuDevice] Debug layer unavailable: install the Windows optional feature "
                     "\"Graphics Tools\"" << std::endl;
    }

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

void HostGpuDevice::DrainDebugMessages() {
    if (!m_info_queue) return;
    const UINT64 count = m_info_queue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T len = 0;
        if (FAILED(m_info_queue->GetMessage(i, nullptr, &len)) || len == 0) continue;
        std::vector<uint8_t> buf(len);
        auto* msg = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
        if (FAILED(m_info_queue->GetMessage(i, msg, &len))) continue;
        if (msg->Severity > D3D11_MESSAGE_SEVERITY_WARNING) continue; // errors + corruption + warnings
        std::string text(msg->pDescription, msg->DescriptionByteLength ? msg->DescriptionByteLength - 1 : 0);
        // Resource names / addresses vary per call: key on the message id plus the first 90 chars.
        const std::string key = std::to_string(static_cast<int>(msg->ID)) + ":" + text.substr(0, 90);
        if (m_printed_debug_messages.size() < 80 && m_printed_debug_messages.insert(key).second) {
            std::cout << "[D3D11 " << (msg->Severity <= D3D11_MESSAGE_SEVERITY_ERROR ? "ERROR" : "WARN") << "] "
                      << text << std::endl;
        }
    }
    m_info_queue->ClearStoredMessages();
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
