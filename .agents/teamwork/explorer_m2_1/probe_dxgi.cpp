#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <iostream>

using Microsoft::WRL::ComPtr;

int main() {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    std::cout << "CreateDXGIFactory1 hr: 0x" << std::hex << hr << std::dec << std::endl;
    if (FAILED(hr)) return 1;

    UINT i = 0;
    ComPtr<IDXGIAdapter1> adapter;
    while (factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        char descUtf8[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, descUtf8, sizeof(descUtf8), nullptr, nullptr);
        std::cout << "Adapter [" << i << "]: " << descUtf8
                  << " | Vendor: 0x" << std::hex << desc.VendorId << std::dec
                  << " | Device: 0x" << std::hex << desc.DeviceId << std::dec
                  << " | Dedicated: " << (desc.DedicatedVideoMemory / (1024 * 1024)) << " MB"
                  << " | Shared: " << (desc.SharedSystemMemory / (1024 * 1024)) << " MB"
                  << " | Flags: 0x" << std::hex << desc.Flags << std::dec
                  << std::endl;
        adapter.Reset();
        i++;
    }
    return 0;
}
