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
#include <cstdint>
#include <cstring>

using Microsoft::WRL::ComPtr;

int main() {
    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter;
    factory->EnumAdapters1(0, &adapter);

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL fl;
    D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &fl, &context);

    // Test non-standard width: 137 x 89
    constexpr UINT W = 137;
    constexpr UINT H = 89;

    D3D11_TEXTURE2D_DESC col_desc = {};
    col_desc.Width = W;
    col_desc.Height = H;
    col_desc.MipLevels = 1;
    col_desc.ArraySize = 1;
    col_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    col_desc.SampleDesc.Count = 1;
    col_desc.Usage = D3D11_USAGE_DEFAULT;
    col_desc.BindFlags = D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> col_tex;
    device->CreateTexture2D(&col_desc, nullptr, &col_tex);

    ComPtr<ID3D11RenderTargetView> rtv;
    device->CreateRenderTargetView(col_tex.Get(), nullptr, &rtv);

    D3D11_TEXTURE2D_DESC stg_desc = col_desc;
    stg_desc.Usage = D3D11_USAGE_STAGING;
    stg_desc.BindFlags = 0;
    stg_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> stg_tex;
    device->CreateTexture2D(&stg_desc, nullptr, &stg_tex);

    // Clear to green: RGBA(0, 1.0, 0, 1.0)
    float green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    context->ClearRenderTargetView(rtv.Get(), green);

    context->CopyResource(stg_tex.Get(), col_tex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    context->Map(stg_tex.Get(), 0, D3D11_MAP_READ, 0, &mapped);

    std::cout << "Texture: " << W << "x" << H << std::endl;
    std::cout << "Tight row bytes: " << (W * 4) << std::endl;
    std::cout << "Mapped RowPitch: " << mapped.RowPitch << std::endl;

    std::vector<uint8_t> packed(W * H * 4);
    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    for (UINT y = 0; y < H; ++y) {
        std::memcpy(packed.data() + (y * W * 4), src + (y * mapped.RowPitch), W * 4);
    }
    context->Unmap(stg_tex.Get(), 0);

    // Verify all pixels in packed are pure green (0, 255, 0, 255)
    bool ok = true;
    for (size_t i = 0; i < packed.size(); i += 4) {
        if (packed[i] != 0 || packed[i+1] != 255 || packed[i+2] != 0 || packed[i+3] != 255) {
            std::cerr << "Mismatch at pixel " << (i/4) << "!" << std::endl;
            ok = false;
            break;
        }
    }
    if (ok) {
        std::cout << "[PASS] Pitch unpadding verified on odd dimension (" << W << "x" << H << ")!" << std::endl;
    }
    return ok ? 0 : 1;
}
