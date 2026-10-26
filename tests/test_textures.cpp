// End-to-end test of texture subresource forwarding: mip chains, texture arrays, BC compressed
// formats and UpdateSubresource on a mip level.
// Usage: test_textures.exe <path-to-proxy-d3d11.dll>   (server on RGPU_SERVER:RGPU_PORT)
//
// One frame, three rows of stripes, each stripe sampling one subresource with point filtering:
//   top:    mip levels 0..6 of a 64x64 texture (level 3 overwritten with UpdateSubresource)
//   middle: slices 0..2 of a texture array
//   bottom: a BC1-compressed texture (left) and an explicit array view starting at slice 2 (right)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

constexpr UINT kWidth = 448, kHeight = 192;

const char* kShaderSource =
    "Texture2D mips : register(t0);\n"
    "Texture2DArray slices : register(t1);\n"
    "Texture2D bc : register(t2);\n"
    "Texture2DArray sliceView : register(t3);\n"
    "SamplerState pointSampler : register(s0);\n"
    "struct VS_IN { float2 pos : POSITION; float2 uv : TEXCOORD; float3 sel : SELECT; };\n"
    "struct PS_IN { float4 pos : SV_POSITION; float2 uv : TEXCOORD; float3 sel : SELECT; };\n"
    "PS_IN VSMain(VS_IN i) { PS_IN o; o.pos = float4(i.pos, 0.5f, 1.0f); o.uv = i.uv; o.sel = i.sel; return o; }\n"
    "float4 PSMain(PS_IN i) : SV_TARGET {\n"
    "  if (i.sel.x < 0.5f) return mips.SampleLevel(pointSampler, i.uv, i.sel.y);\n"
    "  if (i.sel.x < 1.5f) return slices.SampleLevel(pointSampler, float3(i.uv, i.sel.z), 0);\n"
    "  if (i.sel.x < 2.5f) return bc.SampleLevel(pointSampler, i.uv, 0);\n"
    "  return sliceView.SampleLevel(pointSampler, float3(i.uv, 0), 0);\n"
    "}\n";

struct Vertex {
    float pos[2];
    float uv[2];
    float sel[3]; // kind (0 mips, 1 array, 2 BC, 3 explicit array view), mip level, array slice
};

void AddQuad(std::vector<Vertex>& v, float x0, float y0, float x1, float y1, float kind, float lod, float slice) {
    auto p = [&](float x, float y, float u, float t) { return Vertex{{x, y}, {u, t}, {kind, lod, slice}}; };
    v.push_back(p(x0, y1, 0, 0));
    v.push_back(p(x1, y1, 1, 0));
    v.push_back(p(x1, y0, 1, 1));
    v.push_back(p(x0, y1, 0, 0));
    v.push_back(p(x1, y0, 1, 1));
    v.push_back(p(x0, y0, 0, 1));
}

uint32_t Rgba(int r, int g, int b) { return 0xFF000000u | (b << 16) | (g << 8) | r; }

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

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <proxy d3d11.dll>\n", argv[0]);
        return 2;
    }
    char capture[MAX_PATH];
    GetTempPathA(MAX_PATH, capture);
    std::strcat(capture, "rgpu_test_textures.bmp");
    DeleteFileA(capture);
    _putenv_s("RGPU_CAPTURE_FILE", capture);

    HMODULE proxy = LoadLibraryA(argv[1]);
    auto create = proxy ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(GetProcAddress(proxy, "D3D11CreateDeviceAndSwapChain")) : nullptr;
    CHECK(create != nullptr, "proxy d3d11.dll loads");
    if (!create) return 1;

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "RgpuTexturesTest";
    RegisterClassA(&wc);
    RECT rc = {0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "RemoteGPU textures", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = kWidth;
    scd.BufferDesc.Height = kHeight;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    IDXGISwapChain* swap = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &scd, &swap, &dev, nullptr, &ctx);
    CHECK(SUCCEEDED(hr), "remote device created");
    if (FAILED(hr)) return 1;

    // --- Mip chain: 64x64, full chain (MipLevels = 0 -> 7 levels), one solid colour per level.
    const uint32_t mipColors[7] = {Rgba(255, 0, 0), Rgba(0, 255, 0), Rgba(0, 0, 255), Rgba(255, 255, 0),
                                   Rgba(0, 255, 255), Rgba(255, 0, 255), Rgba(128, 128, 128)};
    std::vector<std::vector<uint32_t>> mipData(7);
    D3D11_SUBRESOURCE_DATA mipInit[7] = {};
    for (UINT m = 0; m < 7; ++m) {
        const UINT size = 64 >> m;
        mipData[m].assign(static_cast<size_t>(size) * size, mipColors[m]);
        mipInit[m] = {mipData[m].data(), size * 4, 0};
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 64;
    td.MipLevels = 0;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* mipTex = nullptr;
    CHECK(SUCCEEDED(dev->CreateTexture2D(&td, mipInit, &mipTex)), "CreateTexture2D with a full 7-level mip chain");

    // Overwrite level 3 (8x8) with white through UpdateSubresource.
    std::vector<uint32_t> white(8 * 8, Rgba(255, 255, 255));
    ctx->UpdateSubresource(mipTex, D3D11CalcSubresource(3, 0, 7), nullptr, white.data(), 8 * 4, 0);

    // --- Texture array: 3 slices of 16x16.
    const uint32_t sliceColors[3] = {Rgba(255, 128, 0), Rgba(128, 0, 255), Rgba(0, 128, 128)};
    std::vector<std::vector<uint32_t>> sliceData(3);
    D3D11_SUBRESOURCE_DATA sliceInit[3] = {};
    for (UINT s = 0; s < 3; ++s) {
        sliceData[s].assign(16 * 16, sliceColors[s]);
        sliceInit[s] = {sliceData[s].data(), 16 * 4, 0};
    }
    td.Width = td.Height = 16;
    td.MipLevels = 1;
    td.ArraySize = 3;
    ID3D11Texture2D* arrayTex = nullptr;
    CHECK(SUCCEEDED(dev->CreateTexture2D(&td, sliceInit, &arrayTex)), "CreateTexture2D array with 3 slices");

    // --- BC1: 8x8 = 2x2 blocks, every block solid green (color0 = color1 = 0x07E0, indices 0).
    std::vector<uint8_t> bc1;
    for (int b = 0; b < 4; ++b) {
        const uint8_t block[8] = {0xE0, 0x07, 0xE0, 0x07, 0, 0, 0, 0};
        bc1.insert(bc1.end(), block, block + 8);
    }
    td.Width = td.Height = 8;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_BC1_UNORM;
    D3D11_SUBRESOURCE_DATA bcInit = {bc1.data(), 2 * 8, 0}; // pitch = 2 blocks * 8 bytes
    ID3D11Texture2D* bcTex = nullptr;
    CHECK(SUCCEEDED(dev->CreateTexture2D(&td, &bcInit, &bcTex)), "CreateTexture2D BC1 compressed");

    ID3D11ShaderResourceView* srvs[4] = {};
    dev->CreateShaderResourceView(mipTex, nullptr, &srvs[0]);
    dev->CreateShaderResourceView(arrayTex, nullptr, &srvs[1]);
    dev->CreateShaderResourceView(bcTex, nullptr, &srvs[2]);
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
    viewDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    viewDesc.Texture2DArray.MipLevels = 1;
    viewDesc.Texture2DArray.FirstArraySlice = 2;
    viewDesc.Texture2DArray.ArraySize = 1;
    CHECK(SUCCEEDED(dev->CreateShaderResourceView(arrayTex, &viewDesc, &srvs[3])), "explicit TEXTURE2DARRAY view (slice 2 only)");
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* sampler = nullptr;
    dev->CreateSamplerState(&sd, &sampler);

    ID3DBlob* vsb = nullptr;
    ID3DBlob* psb = nullptr;
    D3DCompile(kShaderSource, std::strlen(kShaderSource), nullptr, nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vsb, nullptr);
    D3DCompile(kShaderSource, std::strlen(kShaderSource), nullptr, nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &psb, nullptr);
    CHECK(vsb && psb, "shaders compiled");
    if (!vsb || !psb) return 1;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
    const D3D11_INPUT_ELEMENT_DESC elems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"SELECT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev->CreateInputLayout(elems, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout);

    std::vector<Vertex> verts;
    for (int m = 0; m < 7; ++m) AddQuad(verts, -1 + m * 2.0f / 7, 1.0f / 3, -1 + (m + 1) * 2.0f / 7, 1, 0, static_cast<float>(m), 0);
    for (int s = 0; s < 3; ++s) AddQuad(verts, -1 + s * 2.0f / 3, -1.0f / 3, -1 + (s + 1) * 2.0f / 3, 1.0f / 3, 1, 0, static_cast<float>(s));
    AddQuad(verts, -1, -1, 0, -1.0f / 3, 2, 0, 0);
    AddQuad(verts, 0, -1, 1, -1.0f / 3, 3, 0, 0);
    D3D11_BUFFER_DESC bd = {static_cast<UINT>(verts.size() * sizeof(Vertex)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA vinit = {verts.data(), 0, 0};
    ID3D11Buffer* vb = nullptr;
    dev->CreateBuffer(&bd, &vinit, &vb);

    ID3D11Texture2D* back = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
    dev->CreateRenderTargetView(back, nullptr, &rtv);
    back->Release();

    const D3D11_VIEWPORT vp = {0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0, 1};
    const float black[4] = {0, 0, 0, 1};
    const UINT stride = sizeof(Vertex), offset = 0;
    bool presented = true;
    for (int i = 0; i < 3 && presented; ++i) {
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->ClearRenderTargetView(rtv, black);
        ctx->IASetInputLayout(layout);
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->PSSetShaderResources(0, 4, srvs);
        ctx->PSSetSamplers(0, 1, &sampler);
        ctx->Draw(static_cast<UINT>(verts.size()), 0);
        presented = SUCCEEDED(swap->Present(0, 0));
    }
    CHECK(presented, "frames presented");

    vb->Release();
    layout->Release();
    ps->Release();
    vs->Release();
    vsb->Release();
    psb->Release();
    sampler->Release();
    for (auto* s : srvs) if (s) s->Release();
    if (bcTex) bcTex->Release();
    if (arrayTex) arrayTex->Release();
    if (mipTex) mipTex->Release();
    rtv->Release();
    swap->Release(); // writes the capture
    ctx->Release();
    dev->Release();
    DestroyWindow(hwnd);

    std::vector<uint8_t> f;
    int w = 0, h = 0;
    if (!ReadBmp(capture, f, w, h) || w != static_cast<int>(kWidth) || h != static_cast<int>(kHeight)) {
        CHECK(false, "frame captured");
        return 1;
    }
    auto at = [&](float x, float y) { // NDC, y up -> (r, g, b)
        const int px = static_cast<int>((x + 1) * 0.5f * (w - 1)), py = static_cast<int>((1 - y) * 0.5f * (h - 1));
        const uint8_t* p = &f[(static_cast<size_t>(py) * w + px) * 4];
        return Rgba(p[2], p[1], p[0]);
    };
    auto rgb = [](uint32_t c) {
        static char buf[8][24];
        static int slot = 0;
        char* s = buf[slot++ % 8];
        std::snprintf(s, 24, "(%u,%u,%u)", c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF);
        return s;
    };
    auto close_to = [](uint32_t a, uint32_t b) {
        for (int k = 0; k < 24; k += 8)
            if (std::abs(static_cast<int>((a >> k) & 0xFF) - static_cast<int>((b >> k) & 0xFF)) > 6) return false;
        return true;
    };

    for (int m = 0; m < 7; ++m) {
        const uint32_t expected = (m == 3) ? Rgba(255, 255, 255) : mipColors[m];
        const uint32_t got = at(-1 + (m + 0.5f) * 2.0f / 7, 2.0f / 3);
        CHECK(close_to(got, expected), "mip level %d %s: got %s, expected %s", m, m == 3 ? "(UpdateSubresource)" : "", rgb(got), rgb(expected));
    }
    for (int s = 0; s < 3; ++s) {
        const uint32_t got = at(-1 + (s + 0.5f) * 2.0f / 3, 0);
        CHECK(close_to(got, sliceColors[s]), "array slice %d: got %s, expected %s", s, rgb(got), rgb(sliceColors[s]));
    }
    const uint32_t gotBc = at(-0.5f, -2.0f / 3);
    CHECK(close_to(gotBc, Rgba(0, 255, 0)), "BC1 compressed texture: got %s, expected (0,255,0)", rgb(gotBc));

    const uint32_t gotView = at(0.5f, -2.0f / 3);
    CHECK(close_to(gotView, sliceColors[2]), "explicit array view starts at slice 2: got %s, expected %s", rgb(gotView), rgb(sliceColors[2]));

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
