// End-to-end test of compute shaders and unordered access views through the proxy.
// Usage: test_compute.exe <path-to-proxy-d3d11.dll>   (server on RGPU_SERVER:RGPU_PORT)
//
// Compute work, then a draw that displays the results, checked on pixels:
//   - a 64x64 texture UAV cleared to blue with ClearUnorderedAccessViewFloat, then a compute shader
//     writes the constant-buffer colour (green) into columns 0-31 only
//   - a structured buffer UAV cleared to zero with ClearUnorderedAccessViewUint, then a compute
//     shader writes out[i] = in[i] + add for i < 2 (SRV + constant buffer) and out[3] = a texture
//     sample (CS SRV + CS sampler); out[2] keeps the cleared value

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

constexpr UINT kWidth = 384, kHeight = 128;

const char* kShaderSource = R"(
cbuffer Params : register(b0) { float4 fill; float4 add; };

RWTexture2D<float4> outTexture : register(u0);
[numthreads(8, 8, 1)]
void CS_FillLeft(uint3 id : SV_DispatchThreadID) { outTexture[id.xy] = fill; }

StructuredBuffer<float4> inValues : register(t0);
Texture2D sampledTexture : register(t1);
SamplerState csSampler : register(s0);
RWStructuredBuffer<float4> outValues : register(u1);
[numthreads(4, 1, 1)]
void CS_Buffer(uint3 id : SV_DispatchThreadID) {
    if (id.x < 2) outValues[id.x] = inValues[id.x] + add;
    if (id.x == 3) outValues[3] = sampledTexture.SampleLevel(csSampler, float2(0.5, 0.5), 0);
}

struct VS_IN  { float2 pos : POSITION; float2 uv : TEXCOORD; };
struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
VS_OUT VS_Quad(VS_IN i) { VS_OUT o; o.pos = float4(i.pos, 0.5, 1); o.uv = i.uv; return o; }

Texture2D shownTexture : register(t0);
SamplerState psSampler : register(s0);
float4 PS_Texture(VS_OUT i) : SV_TARGET { return shownTexture.Sample(psSampler, i.uv); }

StructuredBuffer<float4> shownValues : register(t1);
float4 PS_Value1(VS_OUT i) : SV_TARGET { return shownValues[1]; }
float4 PS_Value2(VS_OUT i) : SV_TARGET { return shownValues[2]; }
float4 PS_Value3(VS_OUT i) : SV_TARGET { return shownValues[3]; }
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
    std::strcat(capture, "rgpu_test_compute.bmp");
    DeleteFileA(capture);
    _putenv_s("RGPU_CAPTURE_FILE", capture);

    HMODULE proxy = LoadLibraryA(argv[1]);
    auto create = proxy ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(GetProcAddress(proxy, "D3D11CreateDeviceAndSwapChain")) : nullptr;
    CHECK(create != nullptr, "proxy d3d11.dll loads");
    if (!create) return 1;

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "RgpuComputeTest";
    RegisterClassA(&wc);
    RECT rc = {0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "RemoteGPU compute", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
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

    // --- Shaders -------------------------------------------------------------------------------
    ID3D11ComputeShader* csFill = nullptr;
    ID3D11ComputeShader* csBuffer = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps[4] = {};
    ID3DBlob* b = Compile("CS_FillLeft", "cs_5_0");
    CHECK(b && SUCCEEDED(dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &csFill)), "CreateComputeShader (texture fill)");
    if (b) b->Release();
    b = Compile("CS_Buffer", "cs_5_0");
    CHECK(b && SUCCEEDED(dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &csBuffer)), "CreateComputeShader (buffer)");
    if (b) b->Release();
    ID3DBlob* vsBlob = Compile("VS_Quad", "vs_5_0");
    dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
    const char* psNames[4] = {"PS_Texture", "PS_Value1", "PS_Value2", "PS_Value3"};
    for (int i = 0; i < 4; ++i) {
        b = Compile(psNames[i], "ps_5_0");
        if (b) dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps[i]);
        if (b) b->Release();
    }
    if (!csFill || !csBuffer || !vs || !ps[3]) return 1;

    // --- Compute resources -------------------------------------------------------------------
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 64;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* outTex = nullptr;
    ID3D11UnorderedAccessView* outTexUav = nullptr;
    ID3D11ShaderResourceView* outTexSrv = nullptr;
    dev->CreateTexture2D(&td, nullptr, &outTex);
    CHECK(SUCCEEDED(dev->CreateUnorderedAccessView(outTex, nullptr, &outTexUav)), "texture UAV (no description)");
    dev->CreateShaderResourceView(outTex, nullptr, &outTexSrv);

    auto makeStructured = [&](const void* init, UINT bind) {
        D3D11_BUFFER_DESC bd = {4 * 16, init ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT, bind, 0,
                                D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16};
        D3D11_SUBRESOURCE_DATA data = {init, 0, 0};
        ID3D11Buffer* buf = nullptr;
        dev->CreateBuffer(&bd, init ? &data : nullptr, &buf);
        return buf;
    };
    const float inValues[4][4] = {{0, 1, 0, 1}, {1, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    ID3D11Buffer* inBuf = makeStructured(inValues, D3D11_BIND_SHADER_RESOURCE);
    ID3D11Buffer* outBuf = makeStructured(nullptr, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.NumElements = 4;
    ID3D11ShaderResourceView* inSrv = nullptr;
    ID3D11ShaderResourceView* outSrv = nullptr;
    dev->CreateShaderResourceView(inBuf, &srvDesc, &inSrv);
    dev->CreateShaderResourceView(outBuf, &srvDesc, &outSrv);
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = 4;
    ID3D11UnorderedAccessView* outBufUav = nullptr;
    CHECK(SUCCEEDED(dev->CreateUnorderedAccessView(outBuf, &uavDesc, &outBufUav)), "structured buffer UAV (BUFFER, 4 elements)");

    const float params[8] = {0, 1, 0, 1, /* add */ 0, 0, 1, 1}; // fill green; in[1] + add = magenta
    D3D11_BUFFER_DESC cbd = {sizeof(params), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA cbInit = {params, 0, 0};
    ID3D11Buffer* cb = nullptr;
    dev->CreateBuffer(&cbd, &cbInit, &cb);

    const uint32_t orange = 0xFF0080FFu; // RGBA bytes FF 80 00 FF
    td.Width = td.Height = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA oinit = {&orange, 4, 0};
    ID3D11Texture2D* sampledTex = nullptr;
    ID3D11ShaderResourceView* sampledSrv = nullptr;
    dev->CreateTexture2D(&td, &oinit, &sampledTex);
    dev->CreateShaderResourceView(sampledTex, nullptr, &sampledSrv);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* sampler = nullptr;
    dev->CreateSamplerState(&sd, &sampler);

    // --- Display geometry -------------------------------------------------------------------------
    std::vector<QuadVertex> quads;
    AddQuad(quads, -1, -1, 0, 1);            // texture
    AddQuad(quads, 0, -1, 1.0f / 3, 1);      // out[1]
    AddQuad(quads, 1.0f / 3, -1, 2.0f / 3, 1); // out[2]
    AddQuad(quads, 2.0f / 3, -1, 1, 1);      // out[3]
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

    // --- Frame ---------------------------------------------------------------------------------
    const float blue[4] = {0, 0, 1, 1};
    const UINT zeros[4] = {0, 0, 0, 0};
    const float gray[4] = {0.25f, 0.25f, 0.25f, 1};
    const D3D11_VIEWPORT vp = {0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0, 1};
    const UINT stride = sizeof(QuadVertex), offset = 0;
    ID3D11UnorderedAccessView* nullUav[2] = {};
    ID3D11ShaderResourceView* nullSrv[2] = {};
    bool presented = true;
    for (int frame = 0; frame < 3 && presented; ++frame) {
        // Compute pass.
        ctx->ClearUnorderedAccessViewFloat(outTexUav, blue);
        ctx->ClearUnorderedAccessViewUint(outBufUav, zeros);
        ctx->CSSetConstantBuffers(0, 1, &cb);
        ctx->CSSetShader(csFill, nullptr, 0);
        ctx->CSSetUnorderedAccessViews(0, 1, &outTexUav, nullptr);
        ctx->Dispatch(4, 8, 1); // 32 x 64 threads: the left half only
        ID3D11ShaderResourceView* csSrvs[2] = {inSrv, sampledSrv};
        ctx->CSSetShader(csBuffer, nullptr, 0);
        ctx->CSSetShaderResources(0, 2, csSrvs);
        ctx->CSSetSamplers(0, 1, &sampler);
        ctx->CSSetUnorderedAccessViews(1, 1, &outBufUav, nullptr);
        ctx->Dispatch(1, 1, 1);
        ctx->CSSetUnorderedAccessViews(0, 2, nullUav, nullptr); // release the UAVs before reading
        ctx->CSSetShaderResources(0, 2, nullSrv);

        // Display pass.
        ctx->OMSetRenderTargets(1, &backRtv, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->ClearRenderTargetView(backRtv, gray);
        ctx->IASetInputLayout(layout);
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0);
        ID3D11ShaderResourceView* psSrvs[2] = {outTexSrv, outSrv};
        ctx->PSSetShaderResources(0, 2, psSrvs);
        ctx->PSSetSamplers(0, 1, &sampler);
        for (int i = 0; i < 4; ++i) {
            ctx->PSSetShader(ps[i], nullptr, 0);
            ctx->Draw(6, 6 * i);
        }
        ctx->PSSetShaderResources(0, 2, nullSrv);
        presented = SUCCEEDED(swap->Present(0, 0));
    }
    CHECK(presented, "frames presented");

    ctx->ClearState();
    for (auto* p : {inBuf, outBuf, cb, vb}) if (p) p->Release();
    for (auto* v : {outTexSrv, inSrv, outSrv, sampledSrv}) if (v) v->Release();
    for (auto* u : {outTexUav, outBufUav}) if (u) u->Release();
    for (auto* t : {outTex, sampledTex}) if (t) t->Release();
    for (auto* p : ps) if (p) p->Release();
    sampler->Release();
    layout->Release();
    vs->Release();
    vsBlob->Release();
    csFill->Release();
    csBuffer->Release();
    backRtv->Release();
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

    uint32_t c = at(-0.75f, 0);
    CHECK(is(c, 0, 255, 0), "compute shader wrote the constant-buffer colour into the texture UAV: %s", rgb(c));
    c = at(-0.25f, 0);
    CHECK(is(c, 0, 0, 255), "columns outside the dispatch keep ClearUnorderedAccessViewFloat's blue: %s", rgb(c));
    c = at(1.0f / 6, 0);
    CHECK(is(c, 255, 0, 255), "RWStructuredBuffer[1] = StructuredBuffer[1] + cbuffer: %s", rgb(c));
    c = at(0.5f, 0);
    CHECK(is(c, 0, 0, 0), "RWStructuredBuffer[2] keeps ClearUnorderedAccessViewUint's zero: %s", rgb(c));
    c = at(5.0f / 6, 0);
    CHECK(is(c, 255, 128, 0), "compute shader sampled a texture with CSSetSamplers: %s", rgb(c));

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
