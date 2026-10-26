// End-to-end test of the DXGI path of the proxy (dxgi.dll + d3d11.dll) against a running server.
// Usage: test_dxgi_proxy.exe <folder containing the proxy dxgi.dll and d3d11.dll>
//
// Follows what most applications do: CreateDXGIFactory1 -> EnumAdapters1 -> D3D11CreateDevice on
// that adapter -> IDXGIFactory2::CreateSwapChainForHwnd -> render -> ResizeBuffers -> render.
// The captured last frame must have the post-resize size and colour.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                                 \
    do {                                                 \
        if (cond) {                                      \
            std::printf("[PASS] ");                      \
        } else {                                         \
            std::printf("[FAIL] (line %d) ", __LINE__);  \
            ++g_failures;                                \
        }                                                \
        std::printf(__VA_ARGS__);                        \
        std::printf("\n");                               \
    } while (0)

bool ReadBmp(const char* path, std::vector<uint8_t>& bgra, int& w, int& h) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    bool ok = std::fread(&fh, sizeof(fh), 1, f) == 1 && std::fread(&ih, sizeof(ih), 1, f) == 1 && ih.biBitCount == 32;
    if (ok) {
        w = ih.biWidth;
        h = std::abs(ih.biHeight);
        bgra.resize(static_cast<size_t>(w) * h * 4);
        std::fseek(f, fh.bfOffBits, SEEK_SET);
        ok = std::fread(bgra.data(), bgra.size(), 1, f) == 1;
    }
    std::fclose(f);
    return ok;
}

bool RenderFrames(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGISwapChain1* swap, const float color[4], int frames) {
    ID3D11Texture2D* back = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) return false;
    HRESULT hr = dev->CreateRenderTargetView(back, nullptr, &rtv);
    back->Release();
    if (FAILED(hr)) return false;
    bool ok = true;
    for (int i = 0; i < frames && ok; ++i) {
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, color);
        ok = SUCCEEDED(swap->Present(0, 0));
    }
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    rtv->Release(); // all back buffer references must be gone before ResizeBuffers
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <proxy dll folder>\n", argv[0]);
        return 2;
    }
    const std::string dir = argv[1];

    char capture[MAX_PATH];
    GetTempPathA(MAX_PATH, capture);
    std::strcat(capture, "rgpu_test_dxgi_proxy.bmp");
    DeleteFileA(capture);
    _putenv_s("RGPU_CAPTURE_FILE", capture);

    HMODULE dxgi = LoadLibraryA((dir + "\\dxgi.dll").c_str());
    HMODULE d3d11 = LoadLibraryA((dir + "\\d3d11.dll").c_str());
    CHECK(dxgi && d3d11, "proxy dxgi.dll and d3d11.dll load");
    if (!dxgi || !d3d11) return 1;
    auto createFactory1 = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    auto createDevice = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3d11, "D3D11CreateDevice"));
    CHECK(createFactory1 && createDevice, "CreateDXGIFactory1 and D3D11CreateDevice exported");
    if (!createFactory1 || !createDevice) return 1;

    IDXGIFactory1* factory = nullptr;
    CHECK(SUCCEEDED(createFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory,
          "CreateDXGIFactory1 returns a factory");
    if (!factory) return 1;

    IDXGIAdapter1* adapter = nullptr;
    CHECK(SUCCEEDED(factory->EnumAdapters1(0, &adapter)) && adapter, "EnumAdapters1(0) returns the remote adapter");
    IDXGIAdapter1* none = nullptr;
    CHECK(factory->EnumAdapters1(1, &none) == DXGI_ERROR_NOT_FOUND, "EnumAdapters1(1) reports no further adapter");
    DXGI_ADAPTER_DESC1 ad = {};
    adapter->GetDesc1(&ad);
    char name[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name), nullptr, nullptr);
    CHECK(std::strncmp(name, "RemoteGPU: ", 11) == 0 && std::strstr(name, "remote adapter") == nullptr,
          "adapter describes the server GPU: \"%s\", %llu MB", name,
          static_cast<unsigned long long>(ad.DedicatedVideoMemory / (1024 * 1024)));

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = createDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    CHECK(SUCCEEDED(hr) && dev && ctx, "D3D11CreateDevice on the enumerated adapter");
    if (FAILED(hr)) return 1;

    IDXGIAdapter* devAdapter = nullptr;
    IDXGIDevice* dxgiDev = nullptr;
    IDXGIFactory2* parent = nullptr;
    bool chain = SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev))) &&
                 SUCCEEDED(dxgiDev->GetAdapter(&devAdapter)) &&
                 SUCCEEDED(devAdapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&parent)));
    CHECK(chain, "device -> IDXGIDevice -> GetAdapter -> GetParent(IDXGIFactory2)");
    if (parent) parent->Release();
    if (devAdapter) devAdapter->Release();
    if (dxgiDev) dxgiDev->Release();

    IDXGIFactory2* factory2 = nullptr;
    CHECK(SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory2))), "factory is an IDXGIFactory2");

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "RgpuDxgiProxyTest";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "RemoteGPU DXGI proxy test", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              700, 560, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    CHECK(SUCCEEDED(factory2->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER)), "MakeWindowAssociation");

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = 640;
    sd.Height = 480;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* swap = nullptr;
    hr = factory2->CreateSwapChainForHwnd(dev, hwnd, &sd, nullptr, nullptr, &swap);
    CHECK(SUCCEEDED(hr) && swap, "CreateSwapChainForHwnd with the proxy device");
    if (FAILED(hr)) return 1;
    DXGI_SWAP_CHAIN_DESC1 got = {};
    swap->GetDesc1(&got);
    CHECK(got.Width == 640 && got.Height == 480, "GetDesc1 reports 640x480");

    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    CHECK(RenderFrames(dev, ctx, swap, red, 10), "10 frames presented at 640x480");
    CHECK(SUCCEEDED(swap->ResizeBuffers(0, 400, 300, DXGI_FORMAT_UNKNOWN, 0)), "ResizeBuffers to 400x300");
    swap->GetDesc1(&got);
    CHECK(got.Width == 400 && got.Height == 300, "GetDesc1 reports 400x300 after resize");
    CHECK(RenderFrames(dev, ctx, swap, green, 10), "10 frames presented at 400x300");

    swap->Release(); // writes the captured frame
    factory2->Release();
    ctx->Release();
    dev->Release();
    adapter->Release();
    factory->Release();
    DestroyWindow(hwnd);

    std::vector<uint8_t> px;
    int w = 0, h = 0;
    const bool have = ReadBmp(capture, px, w, h);
    CHECK(have && w == 400 && h == 300, "captured frame has the resized size (%dx%d)", w, h);
    if (have && !px.empty()) {
        const uint8_t* c = &px[(static_cast<size_t>(h / 2) * w + w / 2) * 4];
        CHECK(c[1] > 200 && c[2] < 30 && c[0] < 30, "captured frame is the post-resize clear colour (green)");
    }

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
