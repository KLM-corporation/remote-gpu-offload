// End-to-end test of the client proxy DLL against a running server.
// Usage: test_client_proxy.exe <path-to-proxy-d3d11.dll> [frames]
// Requires server.exe listening on RGPU_SERVER:RGPU_PORT (default 127.0.0.1:48888).
//
// Renders a triangle whose colour comes from a DYNAMIC constant buffer updated through
// Map(WRITE_DISCARD)/Unmap every frame, presents N frames, then checks the captured last frame:
// the centre must be the triangle colour and a corner must be the clear colour.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                         \
    do {                                                         \
        if (cond) {                                              \
            std::printf("[PASS] %s\n", msg);                     \
        } else {                                                 \
            std::printf("[FAIL] %s (line %d)\n", msg, __LINE__); \
            ++g_failures;                                        \
        }                                                        \
    } while (0)

const char* kShaderSource =
    "cbuffer Tint : register(b0) { float4 tint; };\n"
    "struct VS_IN { float3 pos : POSITION; };\n"
    "struct PS_IN { float4 pos : SV_POSITION; };\n"
    "PS_IN VSMain(VS_IN i) { PS_IN o; o.pos = float4(i.pos, 1.0f); return o; }\n"
    "float4 PSMain(PS_IN i) : SV_TARGET { return tint; }\n";

ID3DBlob* Compile(const char* entry, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(kShaderSource, std::strlen(kShaderSource), nullptr, nullptr, nullptr, entry, target, 0, 0,
                            &code, &errors);
    if (FAILED(hr) && errors) std::printf("%s\n", static_cast<const char*>(errors->GetBufferPointer()));
    if (errors) errors->Release();
    return code;
}

// Reads a top-down 32-bit BGRA BMP written by the proxy swap chain.
bool ReadBmp(const char* path, std::vector<uint8_t>& bgra, int& w, int& h) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    bool ok = std::fread(&fh, sizeof(fh), 1, f) == 1 && std::fread(&ih, sizeof(ih), 1, f) == 1 && fh.bfType == 0x4D42 &&
              ih.biBitCount == 32;
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

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <proxy d3d11.dll> [frames]\n", argv[0]);
        return 2;
    }
    const int frames = argc > 2 ? std::atoi(argv[2]) : 60;
    const UINT width = 320, height = 240;

    char capture[MAX_PATH];
    GetTempPathA(MAX_PATH, capture);
    std::strcat(capture, "rgpu_test_client_proxy.bmp");
    DeleteFileA(capture);
    SetEnvironmentVariableA("RGPU_CAPTURE_FILE", capture);
    _putenv_s("RGPU_CAPTURE_FILE", capture);

    HMODULE proxy = LoadLibraryA(argv[1]);
    CHECK(proxy != nullptr, "proxy d3d11.dll loads");
    if (!proxy) return 1;
    auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(GetProcAddress(proxy, "D3D11CreateDeviceAndSwapChain"));
    CHECK(create != nullptr, "D3D11CreateDeviceAndSwapChain exported");
    if (!create) return 1;

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "RgpuProxyTest";
    RegisterClassA(&wc);
    RECT rc = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "RemoteGPU proxy test", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = width;
    scd.BufferDesc.Height = height;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;

    IDXGISwapChain* swap = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL level = {};
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &scd, &swap, &dev,
                        &level, &ctx);
    CHECK(SUCCEEDED(hr) && swap && dev && ctx, "remote device + swap chain created (server reachable)");
    if (FAILED(hr)) return 1;
    CHECK(level >= D3D_FEATURE_LEVEL_10_0, "feature level reported by server");

    ID3D11Texture2D* back = nullptr;
    hr = swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
    CHECK(SUCCEEDED(hr) && back, "GetBuffer(0) returns back buffer");
    ID3D11RenderTargetView* rtv = nullptr;
    CHECK(SUCCEEDED(dev->CreateRenderTargetView(back, nullptr, &rtv)), "CreateRenderTargetView on back buffer");
    back->Release();

    ID3DBlob* vsb = Compile("VSMain", "vs_4_0");
    ID3DBlob* psb = Compile("PSMain", "ps_4_0");
    CHECK(vsb && psb, "HLSL compiled locally");
    if (!vsb || !psb) return 1;

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;
    CHECK(SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs)), "CreateVertexShader");
    CHECK(SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps)), "CreatePixelShader");
    D3D11_INPUT_ELEMENT_DESC elem = {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    CHECK(SUCCEEDED(dev->CreateInputLayout(&elem, 1, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout)), "CreateInputLayout");

    const float verts[] = {0.0f, 0.8f, 0.0f, 0.8f, -0.8f, 0.0f, -0.8f, -0.8f, 0.0f};
    D3D11_BUFFER_DESC vbd = {sizeof(verts), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA vinit = {verts, 0, 0};
    ID3D11Buffer* vb = nullptr;
    CHECK(SUCCEEDED(dev->CreateBuffer(&vbd, &vinit, &vb)), "CreateBuffer (vertex, initial data)");

    D3D11_BUFFER_DESC cbd = {16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ID3D11Buffer* cb = nullptr;
    CHECK(SUCCEEDED(dev->CreateBuffer(&cbd, nullptr, &cb)), "CreateBuffer (dynamic constant buffer)");

    D3D11_VIEWPORT vp = {0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    const float clear[4] = {0.0f, 0.0f, 1.0f, 1.0f}; // blue
    const float tint[4] = {1.0f, 0.5f, 0.0f, 1.0f};  // orange
    const UINT stride = 12, offset = 0;

    int presented = 0;
    bool map_ok = true;
    for (int i = 0; i < frames; ++i) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) && mapped.pData) {
            std::memcpy(mapped.pData, tint, sizeof(tint));
            ctx->Unmap(cb, 0);
        } else {
            map_ok = false;
        }

        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->ClearRenderTargetView(rtv, clear);
        ctx->IASetInputLayout(layout);
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &cb);
        ctx->Draw(3, 0);
        if (SUCCEEDED(swap->Present(0, 0))) ++presented;
    }
    CHECK(map_ok, "Map(WRITE_DISCARD)/Unmap on dynamic buffer every frame");
    CHECK(presented == frames, "all frames presented with 1:1 server pacing");

    cb->Release();
    vb->Release();
    layout->Release();
    ps->Release();
    vs->Release();
    rtv->Release();
    vsb->Release();
    psb->Release();
    ctx->Release();
    swap->Release(); // writes the captured last frame
    dev->Release();  // sends OP_SHUTDOWN
    DestroyWindow(hwnd);

    std::vector<uint8_t> px;
    int w = 0, h = 0;
    bool have = ReadBmp(capture, px, w, h);
    CHECK(have && w == static_cast<int>(width) && h == static_cast<int>(height), "last frame captured at requested size");
    if (have) {
        auto at = [&](int x, int y) { return &px[(static_cast<size_t>(y) * w + x) * 4]; }; // BGRA
        const uint8_t* centre = at(w / 2, h / 2);
        const uint8_t* corner = at(2, 2);
        std::printf("centre BGRA=(%d,%d,%d) corner BGRA=(%d,%d,%d)\n", centre[0], centre[1], centre[2], corner[0], corner[1], corner[2]);
        CHECK(centre[2] > 200 && centre[1] > 100 && centre[1] < 160 && centre[0] < 30, "centre pixel is the triangle tint (orange)");
        CHECK(corner[0] > 200 && corner[1] < 30 && corner[2] < 30, "corner pixel is the clear colour (blue)");
    }

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
