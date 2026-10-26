// End-to-end test of resource copies and pixel shader UAVs through the proxy.
// Usage: test_copies.exe <path-to-proxy-d3d11.dll>   (server on RGPU_SERVER:RGPU_PORT)
//
// Five regions, each displaying one result, checked on pixels:
//   1 CopyResource of a red texture into an empty one
//   2 CopySubresourceRegion of a 16x16 red square to (16, 16) of a green render target
//   3 CopySubresourceRegion of mip level 1 (blue) of a 2-level texture into a 32x32 texture
//   4 CopySubresourceRegion of bytes 16..32 of a structured buffer (element 1) to offset 0
//   5 a pixel shader writing a UAV bound with OMSetRenderTargetsAndUnorderedAccessViews

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

constexpr UINT kWidth = 640, kHeight = 128;

const char* kShaderSource = R"(
struct VS_IN  { float2 pos : POSITION; float2 uv : TEXCOORD; };
struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
VS_OUT VS_Quad(VS_IN i) { VS_OUT o; o.pos = float4(i.pos, 0.5, 1); o.uv = i.uv; return o; }

Texture2D shown : register(t0);
SamplerState pointSampler : register(s0);
float4 PS_Texture(VS_OUT i) : SV_TARGET { return shown.Sample(pointSampler, i.uv); }

StructuredBuffer<float4> values : register(t1);
float4 PS_Value0(VS_OUT i) : SV_TARGET { return values[0]; }

RWTexture2D<float4> written : register(u1);
float4 PS_WriteUav(VS_OUT i) : SV_TARGET {
    written[uint2(i.pos.xy)] = float4(1, 0, 1, 1);
    return float4(0, 0, 0, 1);
}
)";

struct QuadVertex {
    float pos[2];
    float uv[2];
};

void AddQuad(std::vector<QuadVertex>& v, float x0, float y0, float x1, float y1) {
    v.push_back({{x0, y1}, {0, 0}});
    v.push_back({{x1, y1}, {1, 0}});
    v.push_back({{x1, y0}, {1, 1}});
    v.push_back({{x0, y1}, {0, 0}});
    v.push_back({{x1, y0}, {1, 1}});
    v.push_back({{x0, y0}, {0, 1}});
}

ID3DBlob* Compile(const char* entry, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    if (FAILED(D3DCompile(kShaderSource, std::strlen(kShaderSource), nullptr, nullptr, nullptr, entry, target, 0, 0, &code, &errors)) && errors) {
        std::printf("%s: %s\n", entry, static_cast<const char*>(errors->GetBufferPointer()));
    }
    if (errors) errors->Release();
    return code;
}

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
    std::strcat(capture, "rgpu_test_copies.bmp");
    DeleteFileA(capture);
    _putenv_s("RGPU_CAPTURE_FILE", capture);

    HMODULE proxy = LoadLibraryA(argv[1]);
    auto create = proxy ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(GetProcAddress(proxy, "D3D11CreateDeviceAndSwapChain")) : nullptr;
    CHECK(create != nullptr, "proxy d3d11.dll loads");
    if (!create) return 1;

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "RgpuCopiesTest";
    RegisterClassA(&wc);
    RECT rc = {0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "RemoteGPU copies", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
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

    ID3DBlob* vsBlob = Compile("VS_Quad", "vs_5_0");
    ID3D11VertexShader* vs = nullptr;
    dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
    const char* psNames[3] = {"PS_Texture", "PS_Value0", "PS_WriteUav"};
    ID3D11PixelShader* ps[3] = {};
    for (int i = 0; i < 3; ++i) {
        ID3DBlob* b = Compile(psNames[i], "ps_5_0");
        if (b) dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps[i]);
        if (b) b->Release();
    }
    CHECK(vs && ps[0] && ps[1] && ps[2], "shaders compiled");
    if (!vs || !ps[2]) return 1;

    auto texture = [&](UINT size, UINT mips, UINT bind, const D3D11_SUBRESOURCE_DATA* init) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = size;
        td.MipLevels = mips;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = bind;
        ID3D11Texture2D* t = nullptr;
        dev->CreateTexture2D(&td, init, &t);
        return t;
    };
    const uint32_t red = 0xFF0000FFu, blue = 0xFFFF0000u, green = 0xFF00FF00u;
    std::vector<uint32_t> red64(64 * 64, red), green32(32 * 32, green), blue32(32 * 32, blue);

    // 1. CopyResource: red source -> empty destination.
    D3D11_SUBRESOURCE_DATA redInit = {red64.data(), 64 * 4, 0};
    ID3D11Texture2D* redTex = texture(64, 1, D3D11_BIND_SHADER_RESOURCE, &redInit);
    ID3D11Texture2D* copyDst = texture(64, 1, D3D11_BIND_SHADER_RESOURCE, nullptr);

    // 2. Region copy target: a render target cleared green.
    ID3D11Texture2D* regionDst = texture(64, 1, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, nullptr);
    ID3D11RenderTargetView* regionRtv = nullptr;
    dev->CreateRenderTargetView(regionDst, nullptr, &regionRtv);

    // 3. Two-level texture, level 1 blue, copied into a 32x32 texture.
    D3D11_SUBRESOURCE_DATA mipInit[2] = {{green32.data(), 64 * 4, 0}, {blue32.data(), 32 * 4, 0}};
    std::vector<uint32_t> green64(64 * 64, green);
    mipInit[0].pSysMem = green64.data();
    ID3D11Texture2D* mipSrc = texture(64, 2, D3D11_BIND_SHADER_RESOURCE, mipInit);
    ID3D11Texture2D* mipDst = texture(32, 1, D3D11_BIND_SHADER_RESOURCE, nullptr);

    // 4. Structured buffers: element 1 of src is yellow; copy its 16 bytes to offset 0 of dst.
    const float srcValues[4][4] = {{1, 0, 0, 1}, {1, 1, 0, 1}, {0, 0, 1, 1}, {0, 1, 0, 1}};
    D3D11_BUFFER_DESC bd = {sizeof(srcValues), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                            D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16};
    D3D11_SUBRESOURCE_DATA bufInit = {srcValues, 0, 0};
    ID3D11Buffer* srcBuf = nullptr;
    ID3D11Buffer* dstBuf = nullptr;
    dev->CreateBuffer(&bd, &bufInit, &srcBuf);
    dev->CreateBuffer(&bd, nullptr, &dstBuf);

    // 5. Pixel shader UAV target.
    ID3D11Texture2D* uavTex = texture(64, 1, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr);
    ID3D11UnorderedAccessView* uav = nullptr;
    CHECK(SUCCEEDED(dev->CreateUnorderedAccessView(uavTex, nullptr, &uav)), "UAV for the pixel shader");
    ID3D11Texture2D* scratchTex = texture(64, 1, D3D11_BIND_RENDER_TARGET, nullptr);
    ID3D11RenderTargetView* scratchRtv = nullptr;
    dev->CreateRenderTargetView(scratchTex, nullptr, &scratchRtv);

    ID3D11ShaderResourceView* srvs[5] = {};
    dev->CreateShaderResourceView(copyDst, nullptr, &srvs[0]);
    dev->CreateShaderResourceView(regionDst, nullptr, &srvs[1]);
    dev->CreateShaderResourceView(mipDst, nullptr, &srvs[2]);
    D3D11_SHADER_RESOURCE_VIEW_DESC bufView = {};
    bufView.Format = DXGI_FORMAT_UNKNOWN;
    bufView.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    bufView.Buffer.NumElements = 4;
    dev->CreateShaderResourceView(dstBuf, &bufView, &srvs[3]);
    dev->CreateShaderResourceView(uavTex, nullptr, &srvs[4]);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* sampler = nullptr;
    dev->CreateSamplerState(&sd, &sampler);

    std::vector<QuadVertex> quads;
    AddQuad(quads, -1, -1, 1, 1); // full screen (UAV pass)
    for (int r = 0; r < 5; ++r) AddQuad(quads, -1 + r * 0.4f, -1, -1 + (r + 1) * 0.4f, 1);
    D3D11_BUFFER_DESC vbd = {static_cast<UINT>(quads.size() * sizeof(QuadVertex)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA vinit = {quads.data(), 0, 0};
    ID3D11Buffer* vb = nullptr;
    dev->CreateBuffer(&vbd, &vinit, &vb);
    const D3D11_INPUT_ELEMENT_DESC elems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11InputLayout* layout = nullptr;
    dev->CreateInputLayout(elems, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout);
    ID3D11Texture2D* back = nullptr;
    ID3D11RenderTargetView* backRtv = nullptr;
    swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
    dev->CreateRenderTargetView(back, nullptr, &backRtv);
    back->Release();

    const float greenF[4] = {0, 1, 0, 1}, gray[4] = {0.25f, 0.25f, 0.25f, 1};
    const D3D11_VIEWPORT vp64 = {0, 0, 64, 64, 0, 1};
    const D3D11_VIEWPORT vpBack = {0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0, 1};
    const UINT stride = sizeof(QuadVertex), offset = 0;
    ID3D11ShaderResourceView* nullSrv[2] = {};
    ID3D11UnorderedAccessView* nullUav = nullptr;
    bool presented = true;
    for (int frame = 0; frame < 3 && presented; ++frame) {
        ctx->IASetInputLayout(layout);
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShaderResources(0, 2, nullSrv);

        // Copies.
        ctx->CopyResource(copyDst, redTex);
        ctx->OMSetRenderTargets(1, &regionRtv, nullptr);
        ctx->ClearRenderTargetView(regionRtv, greenF);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        const D3D11_BOX square = {0, 0, 0, 16, 16, 1};
        ctx->CopySubresourceRegion(regionDst, 0, 16, 16, 0, redTex, 0, &square);
        ctx->CopySubresourceRegion(mipDst, 0, 0, 0, 0, mipSrc, 1, nullptr);
        const D3D11_BOX element1 = {16, 0, 0, 32, 1, 1};
        ctx->CopySubresourceRegion(dstBuf, 0, 0, 0, 0, srcBuf, 0, &element1);

        // Pixel shader UAV pass: RTV in slot 0, UAV in slot 1.
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, &scratchRtv, nullptr, 1, 1, &uav, nullptr);
        ctx->RSSetViewports(1, &vp64);
        ctx->PSSetShader(ps[2], nullptr, 0);
        ctx->Draw(6, 0);
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, &backRtv, nullptr, 1, 1, &nullUav, nullptr);

        // Display.
        ctx->RSSetViewports(1, &vpBack);
        ctx->ClearRenderTargetView(backRtv, gray);
        ctx->PSSetSamplers(0, 1, &sampler);
        const int tex_regions[4] = {0, 1, 2, 4};
        ctx->PSSetShader(ps[0], nullptr, 0);
        for (int r : tex_regions) {
            ctx->PSSetShaderResources(0, 1, &srvs[r]);
            ctx->Draw(6, 6 * (r + 1));
        }
        ctx->PSSetShader(ps[1], nullptr, 0);
        ctx->PSSetShaderResources(1, 1, &srvs[3]);
        ctx->Draw(6, 6 * 4);
        presented = SUCCEEDED(swap->Present(0, 0));
    }
    CHECK(presented, "frames presented");

    ctx->ClearState();
    for (auto* v : srvs) if (v) v->Release();
    for (auto* t : {redTex, copyDst, regionDst, mipSrc, mipDst, uavTex, scratchTex}) if (t) t->Release();
    for (auto* b : {srcBuf, dstBuf, vb}) if (b) b->Release();
    for (auto* r : {regionRtv, scratchRtv, backRtv}) if (r) r->Release();
    for (auto* p : ps) if (p) p->Release();
    uav->Release();
    sampler->Release();
    layout->Release();
    vs->Release();
    vsBlob->Release();
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
    // Pixel at (u, v) inside region r.
    auto at = [&](int r, float u, float v) {
        const int px = static_cast<int>((r + u) * (w / 5.0f)), py = static_cast<int>(v * (h - 1));
        const uint8_t* p = &f[(static_cast<size_t>(py) * w + px) * 4];
        return (static_cast<uint32_t>(p[0]) << 16) | (p[1] << 8) | p[2];
    };
    auto rgb = [](uint32_t c) {
        static char buf[8][24];
        static int slot = 0;
        char* s = buf[slot++ % 8];
        std::snprintf(s, 24, "(%u,%u,%u)", c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF);
        return s;
    };
    auto is = [](uint32_t c, int r, int g, int b) {
        return std::abs(static_cast<int>(c & 0xFF) - r) <= 6 && std::abs(static_cast<int>((c >> 8) & 0xFF) - g) <= 6 &&
               std::abs(static_cast<int>((c >> 16) & 0xFF) - b) <= 6;
    };

    uint32_t c = at(0, 0.5f, 0.5f);
    CHECK(is(c, 255, 0, 0), "CopyResource copied the red texture: %s", rgb(c));
    c = at(1, 0.375f, 0.375f);
    CHECK(is(c, 255, 0, 0), "CopySubresourceRegion placed the 16x16 square at (16,16): %s", rgb(c));
    c = at(1, 0.75f, 0.75f);
    CHECK(is(c, 0, 255, 0), "outside the copied square the render target stays green: %s", rgb(c));
    c = at(1, 0.125f, 0.125f);
    CHECK(is(c, 0, 255, 0), "the destination offset was honoured (texel (8,8) stays green): %s", rgb(c));
    c = at(2, 0.5f, 0.5f);
    CHECK(is(c, 0, 0, 255), "mip level 1 (blue) copied into a 32x32 texture: %s", rgb(c));
    c = at(3, 0.5f, 0.5f);
    CHECK(is(c, 255, 255, 0), "buffer bytes 16..32 (element 1, yellow) copied to element 0: %s", rgb(c));
    c = at(4, 0.5f, 0.5f);
    CHECK(is(c, 255, 0, 255), "pixel shader wrote its UAV (OMSetRenderTargetsAndUnorderedAccessViews): %s", rgb(c));

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
