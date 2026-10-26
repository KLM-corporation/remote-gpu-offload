// End-to-end test of the geometry and tessellation stages through the proxy.
// Usage: test_shader_stages.exe <path-to-proxy-d3d11.dll>   (server on RGPU_SERVER:RGPU_PORT)
//
// Top half: two points expanded into quads by a geometry shader. Quad size comes from a GS constant
// buffer, quad 0's colour from a GS Buffer<float4>, quad 1's from a texture sampled with a GS sampler.
// Bottom half (wireframe): two triangle patches in one draw. The hull shader reads each patch's
// tessellation factor from its constant buffer (1 left, 12 right); the domain shader colours vertices
// with a texture (DS SRV + sampler) tinted by its constant buffer. Every feature leaves a visible trace:
// without GS the points are single pixels, without HS/DS a patch list draws nothing.

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

constexpr UINT kWidth = 512, kHeight = 512;

const char* kShaderSource = R"(
struct PS_IN { float4 pos : SV_POSITION; float4 col : COLOR; };
float4 PS_Color(PS_IN i) : SV_TARGET { return i.col; }

// ---- Geometry stage ----
struct GS_IN { float2 center : CENTER; uint id : POINTID; };
GS_IN VS_Point(float2 pos : POSITION, uint id : SV_VertexID) { GS_IN o; o.center = pos; o.id = id; return o; }

cbuffer GSParams : register(b0) { float4 halfSize; };
Buffer<float4> gsColors : register(t0);
Texture2D gsTexture : register(t1);
SamplerState gsSampler : register(s0);

[maxvertexcount(4)]
void GS_Quad(point GS_IN p[1], inout TriangleStream<PS_IN> stream) {
    PS_IN v;
    v.col = p[0].id == 0 ? gsColors[0] : gsTexture.SampleLevel(gsSampler, float2(0.5, 0.5), 0);
    float2 c = p[0].center, h = halfSize.xy;
    v.pos = float4(c + float2(-h.x,  h.y), 0.5, 1); stream.Append(v);
    v.pos = float4(c + float2( h.x,  h.y), 0.5, 1); stream.Append(v);
    v.pos = float4(c + float2(-h.x, -h.y), 0.5, 1); stream.Append(v);
    v.pos = float4(c + float2( h.x, -h.y), 0.5, 1); stream.Append(v);
}

// ---- Tessellation stages ----
struct CP { float3 pos : POSITION; };
CP VS_Patch(CP i) { return i; }

cbuffer HSParams : register(b0) { float4 factors; }; // x: patch 0, y: patch 1
struct PatchConst { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
PatchConst HS_Const(InputPatch<CP, 3> patch, uint id : SV_PrimitiveID) {
    float f = id == 0 ? factors.x : factors.y;
    PatchConst o;
    o.edges[0] = o.edges[1] = o.edges[2] = f;
    o.inside = f;
    return o;
}
[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("HS_Const")]
CP HS_Main(InputPatch<CP, 3> patch, uint i : SV_OutputControlPointID) { return patch[i]; }

cbuffer DSParams : register(b0) { float4 tint; };
Texture2D dsTexture : register(t0);
SamplerState dsSampler : register(s0);
[domain("tri")]
PS_IN DS_Main(PatchConst c, float3 bary : SV_DomainLocation, const OutputPatch<CP, 3> patch) {
    PS_IN o;
    float3 p = bary.x * patch[0].pos + bary.y * patch[1].pos + bary.z * patch[2].pos;
    o.pos = float4(p, 1);
    o.col = dsTexture.SampleLevel(dsSampler, float2(0.5, 0.5), 0) * tint;
    return o;
}
)";

ID3DBlob* Compile(const char* entry, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    if (FAILED(D3DCompile(kShaderSource, std::strlen(kShaderSource), nullptr, nullptr, nullptr, entry, target, 0, 0, &code, &errors)) && errors) {
        std::printf("%s: %s\n", entry, static_cast<const char*>(errors->GetBufferPointer()));
    }
    if (errors) errors->Release();
    return code;
}

ID3D11Buffer* MakeBuffer(ID3D11Device* dev, const void* data, UINT size, UINT bind) {
    D3D11_BUFFER_DESC bd = {size, D3D11_USAGE_IMMUTABLE, bind, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA init = {data, 0, 0};
    ID3D11Buffer* b = nullptr;
    dev->CreateBuffer(&bd, &init, &b);
    return b;
}

ID3D11ShaderResourceView* MakeTexture1x1(ID3D11Device* dev, uint32_t rgba, ID3D11Texture2D** tex) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {&rgba, 4, 0};
    ID3D11ShaderResourceView* srv = nullptr;
    dev->CreateTexture2D(&td, &init, tex);
    dev->CreateShaderResourceView(*tex, nullptr, &srv);
    return srv;
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
    std::strcat(capture, "rgpu_test_shader_stages.bmp");
    DeleteFileA(capture);
    _putenv_s("RGPU_CAPTURE_FILE", capture);

    HMODULE proxy = LoadLibraryA(argv[1]);
    auto create = proxy ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(GetProcAddress(proxy, "D3D11CreateDeviceAndSwapChain")) : nullptr;
    CHECK(create != nullptr, "proxy d3d11.dll loads");
    if (!create) return 1;

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "RgpuStagesTest";
    RegisterClassA(&wc);
    RECT rc = {0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "RemoteGPU shader stages", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
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

    // --- Shaders ---------------------------------------------------------------------------
    ID3DBlob* vsPointBlob = Compile("VS_Point", "vs_5_0");
    ID3DBlob* vsPatchBlob = Compile("VS_Patch", "vs_5_0");
    ID3DBlob* gsBlob = Compile("GS_Quad", "gs_5_0");
    ID3DBlob* hsBlob = Compile("HS_Main", "hs_5_0");
    ID3DBlob* dsBlob = Compile("DS_Main", "ds_5_0");
    ID3DBlob* psBlob = Compile("PS_Color", "ps_5_0");
    if (!vsPointBlob || !vsPatchBlob || !gsBlob || !hsBlob || !dsBlob || !psBlob) {
        CHECK(false, "shaders compiled");
        return 1;
    }
    ID3D11VertexShader* vsPoint = nullptr;
    ID3D11VertexShader* vsPatch = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* ds = nullptr;
    ID3D11PixelShader* ps = nullptr;
    dev->CreateVertexShader(vsPointBlob->GetBufferPointer(), vsPointBlob->GetBufferSize(), nullptr, &vsPoint);
    dev->CreateVertexShader(vsPatchBlob->GetBufferPointer(), vsPatchBlob->GetBufferSize(), nullptr, &vsPatch);
    CHECK(SUCCEEDED(dev->CreateGeometryShader(gsBlob->GetBufferPointer(), gsBlob->GetBufferSize(), nullptr, &gs)), "CreateGeometryShader");
    CHECK(SUCCEEDED(dev->CreateHullShader(hsBlob->GetBufferPointer(), hsBlob->GetBufferSize(), nullptr, &hs)), "CreateHullShader");
    CHECK(SUCCEEDED(dev->CreateDomainShader(dsBlob->GetBufferPointer(), dsBlob->GetBufferSize(), nullptr, &ds)), "CreateDomainShader");
    dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);

    const D3D11_INPUT_ELEMENT_DESC pointElem = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    const D3D11_INPUT_ELEMENT_DESC patchElem = {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    ID3D11InputLayout* pointLayout = nullptr;
    ID3D11InputLayout* patchLayout = nullptr;
    dev->CreateInputLayout(&pointElem, 1, vsPointBlob->GetBufferPointer(), vsPointBlob->GetBufferSize(), &pointLayout);
    dev->CreateInputLayout(&patchElem, 1, vsPatchBlob->GetBufferPointer(), vsPatchBlob->GetBufferSize(), &patchLayout);

    // --- Data -------------------------------------------------------------------------------
    const float points[2][2] = {{-0.5f, 0.5f}, {0.5f, 0.5f}};
    const float patches[6][3] = {
        {-0.9f, -0.9f, 0.5f}, {-0.5f, -0.1f, 0.5f}, {-0.1f, -0.9f, 0.5f}, // patch 0: factor 1
        {0.1f, -0.9f, 0.5f},  {0.5f, -0.1f, 0.5f},  {0.9f, -0.9f, 0.5f},  // patch 1: factor 12
    };
    ID3D11Buffer* pointVb = MakeBuffer(dev, points, sizeof(points), D3D11_BIND_VERTEX_BUFFER);
    ID3D11Buffer* patchVb = MakeBuffer(dev, patches, sizeof(patches), D3D11_BIND_VERTEX_BUFFER);

    const float gsParams[4] = {0.25f, 0.25f, 0, 0};
    const float hsParams[4] = {1, 12, 0, 0};
    const float dsParams[4] = {0, 1, 1, 1}; // cyan tint over a white texture
    ID3D11Buffer* gsCb = MakeBuffer(dev, gsParams, sizeof(gsParams), D3D11_BIND_CONSTANT_BUFFER);
    ID3D11Buffer* hsCb = MakeBuffer(dev, hsParams, sizeof(hsParams), D3D11_BIND_CONSTANT_BUFFER);
    ID3D11Buffer* dsCb = MakeBuffer(dev, dsParams, sizeof(dsParams), D3D11_BIND_CONSTANT_BUFFER);

    const float gsColorData[2][4] = {{1, 0, 0, 1}, {0, 0, 0, 1}};
    ID3D11Buffer* gsColorBuf = MakeBuffer(dev, gsColorData, sizeof(gsColorData), D3D11_BIND_SHADER_RESOURCE);
    D3D11_SHADER_RESOURCE_VIEW_DESC bufView = {};
    bufView.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    bufView.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    bufView.Buffer.NumElements = 2;
    ID3D11ShaderResourceView* gsColorSrv = nullptr;
    dev->CreateShaderResourceView(gsColorBuf, &bufView, &gsColorSrv);
    ID3D11Texture2D* yellowTex = nullptr;
    ID3D11Texture2D* whiteTex = nullptr;
    ID3D11ShaderResourceView* yellowSrv = MakeTexture1x1(dev, 0xFF00FFFFu, &yellowTex);
    ID3D11ShaderResourceView* whiteSrv = MakeTexture1x1(dev, 0xFFFFFFFFu, &whiteTex);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* sampler = nullptr;
    dev->CreateSamplerState(&sd, &sampler);

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_WIREFRAME;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11RasterizerState* wireframe = nullptr;
    dev->CreateRasterizerState(&rd, &wireframe);

    ID3D11Texture2D* back = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
    dev->CreateRenderTargetView(back, nullptr, &rtv);
    back->Release();

    // --- Frames -----------------------------------------------------------------------------
    const float black[4] = {0, 0, 0, 1};
    const D3D11_VIEWPORT vp = {0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0, 1};
    const UINT pointStride = 8, patchStride = 12, zero = 0;
    ID3D11ShaderResourceView* gsSrvs[2] = {gsColorSrv, yellowSrv};
    bool presented = true;
    for (int frame = 0; frame < 3 && presented; ++frame) {
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->ClearRenderTargetView(rtv, black);
        ctx->PSSetShader(ps, nullptr, 0);

        // Geometry shader: points -> quads.
        ctx->RSSetState(nullptr);
        ctx->IASetInputLayout(pointLayout);
        ctx->IASetVertexBuffers(0, 1, &pointVb, &pointStride, &zero);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        ctx->VSSetShader(vsPoint, nullptr, 0);
        ctx->GSSetShader(gs, nullptr, 0);
        ctx->GSSetConstantBuffers(0, 1, &gsCb);
        ctx->GSSetShaderResources(0, 2, gsSrvs);
        ctx->GSSetSamplers(0, 1, &sampler);
        ctx->Draw(2, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);

        // Tessellation: two triangle patches, wireframe.
        ctx->RSSetState(wireframe);
        ctx->IASetInputLayout(patchLayout);
        ctx->IASetVertexBuffers(0, 1, &patchVb, &patchStride, &zero);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
        ctx->VSSetShader(vsPatch, nullptr, 0);
        ctx->HSSetShader(hs, nullptr, 0);
        ctx->HSSetConstantBuffers(0, 1, &hsCb);
        ctx->DSSetShader(ds, nullptr, 0);
        ctx->DSSetConstantBuffers(0, 1, &dsCb);
        ctx->DSSetShaderResources(0, 1, &whiteSrv);
        ctx->DSSetSamplers(0, 1, &sampler);
        ctx->Draw(6, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);

        presented = SUCCEEDED(swap->Present(0, 0));
    }
    CHECK(presented, "frames presented");

    ctx->ClearState();
    for (auto* b : {pointVb, patchVb, gsCb, hsCb, dsCb, gsColorBuf}) if (b) b->Release();
    for (auto* v : {gsColorSrv, yellowSrv, whiteSrv}) if (v) v->Release();
    for (auto* t : {yellowTex, whiteTex}) if (t) t->Release();
    for (auto* b : {vsPointBlob, vsPatchBlob, gsBlob, hsBlob, dsBlob, psBlob}) b->Release();
    sampler->Release();
    wireframe->Release();
    pointLayout->Release();
    patchLayout->Release();
    vsPoint->Release();
    vsPatch->Release();
    if (gs) gs->Release();
    if (hs) hs->Release();
    if (ds) ds->Release();
    ps->Release();
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
    auto at = [&](float x, float y) {
        const int px = static_cast<int>((x + 1) * 0.5f * (w - 1)), py = static_cast<int>((1 - y) * 0.5f * (h - 1));
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

    uint32_t c = at(-0.5f, 0.5f);
    CHECK(is(c, 255, 0, 0), "GS expanded point 0 into a quad coloured from a GS Buffer<float4>: %s", rgb(c));
    c = at(-0.5f, 0.7f);
    CHECK(is(c, 255, 0, 0), "quad extends to the GS constant-buffer half size (0.7 inside 0.5 +/- 0.25): %s", rgb(c));
    c = at(-0.5f, 0.85f);
    CHECK(is(c, 0, 0, 0), "and no further (0.85 outside): %s", rgb(c));
    c = at(0.5f, 0.5f);
    CHECK(is(c, 255, 255, 0), "GS point 1 coloured by a texture sampled with GSSetSamplers: %s", rgb(c));

    int left = 0, right = 0;
    for (int y = h / 2; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = &f[(static_cast<size_t>(y) * w + x) * 4];
            const bool cyan = p[2] < 60 && p[1] > 200 && p[0] > 200;
            if (cyan) (x < w / 2 ? left : right)++;
        }
    }
    CHECK(left > 100, "patch 0 drawn in cyan (DS texture x DS constant-buffer tint): %d edge pixels", left);
    CHECK(right > left * 3, "patch 1 tessellated far more finely (HS factor 12 vs 1): %d vs %d edge pixels", right, left);

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
