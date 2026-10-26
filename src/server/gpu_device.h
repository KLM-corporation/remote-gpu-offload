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

/**
 * @brief Metadata and DXGI COM pointer representation for an enumerated host GPU adapter.
 */
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

/**
 * @brief Host GPU Device manager providing headless Direct3D 11 execution,
 * hardware adapter enumeration & selection, offscreen render targets, and CPU readback.
 */
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
