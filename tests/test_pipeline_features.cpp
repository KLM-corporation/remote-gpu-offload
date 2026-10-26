// End-to-end test of instancing, multiple render targets, vertex shader resources and buffer views.
// Usage: test_pipeline_features.exe <path-to-proxy-d3d11.dll>   (server on RGPU_SERVER:RGPU_PORT)
//
// One frame, one region per feature, each checked on its pixels:
//   top row:    MRT target 0 (red) | MRT target 1 (green) | DrawInstanced, 4 instances
//   bottom row: DrawIndexedInstanced from instance 2 | VS Buffer<float4> | VS Texture2D + sampler |
//               PS StructuredBuffer
// Before the regions, a full-screen magenta draw with no render target bound must land nowhere.

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

constexpr UINT kWidth = 512, kHeight = 256;

const char* kShaderSource = R"(
struct VS_IN  { float2 pos : POSITION; float2 uv : TEXCOORD; };
struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD; float4 col : COLOR; };

VS_OUT VS_Quad(VS_IN i) { VS_OUT o; o.pos = float4(i.pos, 0.5, 1); o.uv = i.uv; o.col = 0; return o; }

Buffer<float4> vsColors : register(t0);
VS_OUT VS_Buf(VS_IN i) { VS_OUT o = VS_Quad(i); o.col = vsColors[1]; return o; }

Texture2D vsTexture : register(t1);
SamplerState vsSampler : register(s0);
VS_OUT VS_Tex(VS_IN i) { VS_OUT o = VS_Quad(i); o.col = vsTexture.SampleLevel(vsSampler, float2(0.5, 0.5), 0); return o; }

struct INST_IN { float2 pos : POSITION; float2 ofs : INSTOFS; float4 col : INSTCOL; };
VS_OUT VS_Inst(INST_IN i) { VS_OUT o; o.pos = float4(i.pos + i.ofs, 0.5, 1); o.uv = 0; o.col = i.col; return o; }

float4 PS_Color(VS_OUT i) : SV_TARGET { return i.col; }
float4 PS_Magenta(VS_OUT i) : SV_TARGET { return float4(1, 0, 1, 1); }

struct MRT_OUT { float4 a : SV_TARGET0; float4 b : SV_TARGET1; };
MRT_OUT PS_Mrt(VS_OUT i) { MRT_OUT o; o.a = float4(1, 0, 0, 1); o.b = float4(0, 1, 0, 1); return o; }

Texture2D psTexture : register(t0);
SamplerState psSampler : register(s0);
float4 PS_Tex(VS_OUT i) : SV_TARGET { return psTexture.Sample(psSampler, i.uv); }

StructuredBuffer<float4> psStructured : register(t3);
float4 PS_Struct(VS_OUT i) : SV_TARGET { return psStructured[2]; }
)";

struct QuadVertex {
    float pos[2];
    float uv[2];
};

struct Instance {
    float ofs[2];
    float col[4];
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

ID3D11Buffer* MakeBuffer(ID3D11Device* dev, const void* data, UINT size, UINT bind, UINT misc = 0, UINT stride = 0) {
    D3D11_BUFFER_DESC bd = {size, D3D11_USAGE_IMMUTABLE, bind, 0, misc, stride};
    D3D11_SUBRESOURCE_DATA init = {data, 0, 0};
    ID3D11Buffer* b = nullptr;
    dev->CreateBuffer(&bd, &init, &b);
    return b;
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
    std::strcat(capture, "rgpu_test_pipeline_features.bmp");
    DeleteFileA(capture);
    _putenv_s("RGPU_CAPTURE_FILE", capture);

    HMODULE proxy = LoadLibraryA(argv[1]);
    auto create = proxy ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(GetProcAddress(proxy, "D3D11CreateDeviceAndSwapChain")) : nullptr;
    CHECK(create != nullptr, "proxy d3d11.dll loads");
    if (!create) return 1;

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "RgpuPipelineFeatures";
    RegisterClassA(&wc);
    RECT rc = {0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "RemoteGPU pipeline features", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
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

    // --- Shaders -----------------------------------------------------------------
    const char* vsNames[] = {"VS_Quad", "VS_Buf", "VS_Tex", "VS_Inst"};
    const char* psNames[] = {"PS_Color", "PS_Magenta", "PS_Mrt", "PS_Tex", "PS_Struct"};
    ID3DBlob* vsBlob[4] = {};
    ID3D11VertexShader* vs[4] = {};
    ID3D11PixelShader* ps[5] = {};
    bool compiled = true;
    for (int i = 0; i < 4; ++i) {
        vsBlob[i] = Compile(vsNames[i], "vs_5_0");
        compiled = compiled && vsBlob[i] && SUCCEEDED(dev->CreateVertexShader(vsBlob[i]->GetBufferPointer(), vsBlob[i]->GetBufferSize(), nullptr, &vs[i]));
    }
    for (int i = 0; i < 5; ++i) {
        ID3DBlob* b = Compile(psNames[i], "ps_5_0");
        compiled = compiled && b && SUCCEEDED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps[i]));
        if (b) b->Release();
    }
    CHECK(compiled, "9 shaders compiled (shader model 5.0)");
    if (!compiled) return 1;
    enum { VS_QUAD, VS_BUF, VS_TEX, VS_INST };
    enum { PS_COLOR, PS_MAGENTA, PS_MRT, PS_TEX, PS_STRUCT };

    const D3D11_INPUT_ELEMENT_DESC quadElems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    const D3D11_INPUT_ELEMENT_DESC instElems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"INSTOFS", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INSTCOL", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 8, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    ID3D11InputLayout* quadLayout = nullptr;
    ID3D11InputLayout* instLayout = nullptr;
    dev->CreateInputLayout(quadElems, 2, vsBlob[VS_QUAD]->GetBufferPointer(), vsBlob[VS_QUAD]->GetBufferSize(), &quadLayout);
    CHECK(SUCCEEDED(dev->CreateInputLayout(instElems, 3, vsBlob[VS_INST]->GetBufferPointer(), vsBlob[VS_INST]->GetBufferSize(), &instLayout)),
          "per-instance input layout");

    // --- Geometry: one region quad per test, a small quad for instancing --------------------
    // Regions (NDC): top row y 0..1, bottom row y -1..0.
    std::vector<QuadVertex> quads;
    AddQuad(quads, -1, -1, 1, 1);           // 0: full screen
    AddQuad(quads, -1, 0, -0.5f, 1);        // 1: MRT target 0
    AddQuad(quads, -0.5f, 0, 0, 1);         // 2: MRT target 1
    AddQuad(quads, -0.5f, -1, 0, 0);        // 3: VS Buffer<float4>
    AddQuad(quads, 0, -1, 0.5f, 0);         // 4: VS Texture2D
    AddQuad(quads, 0.5f, -1, 1, 0);         // 5: PS StructuredBuffer
    ID3D11Buffer* quadVb = MakeBuffer(dev, quads.data(), static_cast<UINT>(quads.size() * sizeof(QuadVertex)), D3D11_BIND_VERTEX_BUFFER);

    const float s = 0.2f; // small quad around the origin, instanced
    const QuadVertex small[4] = {{{-s, s}, {0, 0}}, {{s, s}, {1, 0}}, {{s, -s}, {1, 1}}, {{-s, -s}, {0, 1}}};
    const QuadVertex smallList[6] = {small[0], small[1], small[2], small[0], small[2], small[3]};
    const uint16_t smallIdx[6] = {0, 1, 2, 0, 2, 3};
    ID3D11Buffer* smallVb = MakeBuffer(dev, smallList, sizeof(smallList), D3D11_BIND_VERTEX_BUFFER);
    ID3D11Buffer* smallIndexedVb = MakeBuffer(dev, small, sizeof(small), D3D11_BIND_VERTEX_BUFFER);
    ID3D11Buffer* smallIb = MakeBuffer(dev, smallIdx, sizeof(smallIdx), D3D11_BIND_INDEX_BUFFER);

    // Instances 0-3: the 2x2 cells of the top-right region; 2-3 are reused by the indexed draw.
    const Instance instances[4] = {
        {{0.25f, 0.75f}, {1, 0, 0, 1}}, {{0.75f, 0.75f}, {0, 1, 0, 1}},
        {{0.25f, 0.25f}, {0, 0, 1, 1}}, {{0.75f, 0.25f}, {1, 1, 0, 1}},
    };
    const Instance indexedInstances[4] = {
        {{0, 0}, {0, 0, 0, 1}}, {{0, 0}, {0, 0, 0, 1}},          // skipped by StartInstanceLocation = 2
        {{-0.75f, -0.25f}, {1, 0.5f, 0, 1}}, {{-0.75f, -0.75f}, {0.5f, 0, 1, 1}},
    };
    ID3D11Buffer* instVb = MakeBuffer(dev, instances, sizeof(instances), D3D11_BIND_VERTEX_BUFFER);
    ID3D11Buffer* indexedInstVb = MakeBuffer(dev, indexedInstances, sizeof(indexedInstances), D3D11_BIND_VERTEX_BUFFER);

    // --- MRT targets -------------------------------------------------------------------------
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 64;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* mrtTex[2] = {};
    ID3D11RenderTargetView* mrtRtv[2] = {};
    ID3D11ShaderResourceView* mrtSrv[2] = {};
    bool mrtOk = true;
    for (int i = 0; i < 2; ++i) {
        mrtOk = mrtOk && SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &mrtTex[i])) &&
                SUCCEEDED(dev->CreateRenderTargetView(mrtTex[i], nullptr, &mrtRtv[i])) &&
                SUCCEEDED(dev->CreateShaderResourceView(mrtTex[i], nullptr, &mrtSrv[i]));
    }
    CHECK(mrtOk, "two render-target textures with RTV + SRV");

    // --- Buffer views --------------------------------------------------------------------------
    const float colors[3][4] = {{1, 1, 1, 1}, {0, 0, 1, 1}, {0, 1, 1, 1}}; // [1] blue for VS, [2] cyan for PS
    ID3D11Buffer* typedBuf = MakeBuffer(dev, colors, sizeof(colors), D3D11_BIND_SHADER_RESOURCE);
    ID3D11Buffer* structBuf = MakeBuffer(dev, colors, sizeof(colors), D3D11_BIND_SHADER_RESOURCE,
                                         D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16);
    D3D11_SHADER_RESOURCE_VIEW_DESC bufView = {};
    bufView.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    bufView.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    bufView.Buffer.FirstElement = 0;
    bufView.Buffer.NumElements = 3;
    ID3D11ShaderResourceView* typedSrv = nullptr;
    ID3D11ShaderResourceView* structSrv = nullptr;
    CHECK(typedBuf && SUCCEEDED(dev->CreateShaderResourceView(typedBuf, &bufView, &typedSrv)), "typed buffer SRV (R32G32B32A32_FLOAT, 3 elements)");
    bufView.Format = DXGI_FORMAT_UNKNOWN;
    CHECK(structBuf && SUCCEEDED(dev->CreateShaderResourceView(structBuf, &bufView, &structSrv)), "structured buffer SRV (stride 16, 3 elements)");

    // --- Vertex texture fetch: 1x1 yellow texture + point sampler -----------------------------
    const uint32_t yellow = 0xFF00FFFFu; // RGBA bytes: FF FF 00 FF
    td.Width = td.Height = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA yinit = {&yellow, 4, 0};
    ID3D11Texture2D* vsTex = nullptr;
    ID3D11ShaderResourceView* vsTexSrv = nullptr;
    dev->CreateTexture2D(&td, &yinit, &vsTex);
    dev->CreateShaderResourceView(vsTex, nullptr, &vsTexSrv);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* sampler = nullptr;
    dev->CreateSamplerState(&sd, &sampler);

    ID3D11Texture2D* back = nullptr;
    ID3D11RenderTargetView* backRtv = nullptr;
    swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
    dev->CreateRenderTargetView(back, nullptr, &backRtv);
    back->Release();

    // --- Frame -------------------------------------------------------------------------------
    const float black[4] = {0, 0, 0, 1};
    const D3D11_VIEWPORT vpMrt = {0, 0, 64, 64, 0, 1};
    const D3D11_VIEWPORT vpBack = {0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0, 1};
    const UINT quadStride = sizeof(QuadVertex), instStride = sizeof(Instance), zero = 0;
    ID3D11ShaderResourceView* nullSrv[4] = {};
    bool presented = true;
    for (int frame = 0; frame < 3 && presented; ++frame) {
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Pass 1: both MRT targets written by one draw.
        ctx->PSSetShaderResources(0, 4, nullSrv);
        ctx->OMSetRenderTargets(2, mrtRtv, nullptr);
        ctx->ClearRenderTargetView(mrtRtv[0], black);
        ctx->ClearRenderTargetView(mrtRtv[1], black);
        ctx->RSSetViewports(1, &vpMrt);
        ctx->IASetInputLayout(quadLayout);
        ctx->IASetVertexBuffers(0, 1, &quadVb, &quadStride, &zero);
        ctx->VSSetShader(vs[VS_QUAD], nullptr, 0);
        ctx->PSSetShader(ps[PS_MRT], nullptr, 0);
        ctx->Draw(6, 0);

        // Back buffer: clear, then a magenta draw with no render target bound must change nothing.
        ctx->OMSetRenderTargets(1, &backRtv, nullptr);
        ctx->RSSetViewports(1, &vpBack);
        ctx->ClearRenderTargetView(backRtv, black);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        ctx->PSSetShader(ps[PS_MAGENTA], nullptr, 0);
        ctx->Draw(6, 0);
        ctx->OMSetRenderTargets(1, &backRtv, nullptr);

        // Regions 1-2: show the MRT targets.
        ctx->PSSetShader(ps[PS_TEX], nullptr, 0);
        ctx->PSSetSamplers(0, 1, &sampler);
        ctx->PSSetShaderResources(0, 1, &mrtSrv[0]);
        ctx->Draw(6, 6);
        ctx->PSSetShaderResources(0, 1, &mrtSrv[1]);
        ctx->Draw(6, 12);

        // Region 3: DrawInstanced, 4 instances.
        ID3D11Buffer* instVbs[2] = {smallVb, instVb};
        const UINT strides[2] = {quadStride, instStride}, offsets[2] = {0, 0};
        ctx->IASetInputLayout(instLayout);
        ctx->IASetVertexBuffers(0, 2, instVbs, strides, offsets);
        ctx->VSSetShader(vs[VS_INST], nullptr, 0);
        ctx->PSSetShader(ps[PS_COLOR], nullptr, 0);
        ctx->DrawInstanced(6, 4, 0, 0);

        // Region 4: DrawIndexedInstanced, 2 instances starting at instance 2.
        ID3D11Buffer* idxVbs[2] = {smallIndexedVb, indexedInstVb};
        ctx->IASetVertexBuffers(0, 2, idxVbs, strides, offsets);
        ctx->IASetIndexBuffer(smallIb, DXGI_FORMAT_R16_UINT, 0);
        ctx->DrawIndexedInstanced(6, 2, 0, 0, 2);

        // Region 5: VS reads a typed buffer.
        ctx->IASetInputLayout(quadLayout);
        ctx->IASetVertexBuffers(0, 1, &quadVb, &quadStride, &zero);
        ctx->VSSetShader(vs[VS_BUF], nullptr, 0);
        ctx->VSSetShaderResources(0, 1, &typedSrv);
        ctx->Draw(6, 18);

        // Region 6: VS samples a texture with its own sampler.
        ctx->VSSetShader(vs[VS_TEX], nullptr, 0);
        ctx->VSSetShaderResources(1, 1, &vsTexSrv);
        ctx->VSSetSamplers(0, 1, &sampler);
        ctx->Draw(6, 24);

        // Region 7: PS reads a structured buffer.
        ctx->VSSetShader(vs[VS_QUAD], nullptr, 0);
        ctx->PSSetShader(ps[PS_STRUCT], nullptr, 0);
        ctx->PSSetShaderResources(3, 1, &structSrv);
        ctx->Draw(6, 30);

        presented = SUCCEEDED(swap->Present(0, 0));
    }
    CHECK(presented, "frames presented");

    ctx->ClearState();
    for (auto* p : {quadVb, smallVb, smallIndexedVb, smallIb, instVb, indexedInstVb, typedBuf, structBuf}) if (p) p->Release();
    for (int i = 0; i < 2; ++i) {
        if (mrtSrv[i]) mrtSrv[i]->Release();
        if (mrtRtv[i]) mrtRtv[i]->Release();
        if (mrtTex[i]) mrtTex[i]->Release();
    }
    for (auto* v : {typedSrv, structSrv, vsTexSrv}) if (v) v->Release();
    if (vsTex) vsTex->Release();
    sampler->Release();
    quadLayout->Release();
    if (instLayout) instLayout->Release();
    for (auto* v : vs) if (v) v->Release();
    for (auto* p : ps) if (p) p->Release();
    for (auto* b : vsBlob) if (b) b->Release();
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
    auto at = [&](float x, float y) { // NDC, y up -> packed 0xBBGGRR
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

    uint32_t c = at(-0.75f, 0.5f);
    CHECK(is(c, 255, 0, 0), "MRT target 0 written red: %s", rgb(c));
    c = at(-0.25f, 0.5f);
    CHECK(is(c, 0, 255, 0), "MRT target 1 written green by the same draw: %s", rgb(c));
    const int instExpected[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}};
    for (int i = 0; i < 4; ++i) {
        c = at(instances[i].ofs[0], instances[i].ofs[1]);
        CHECK(is(c, instExpected[i][0], instExpected[i][1], instExpected[i][2]), "DrawInstanced instance %d: %s", i, rgb(c));
    }
    c = at(-0.75f, -0.25f);
    CHECK(is(c, 255, 128, 0), "DrawIndexedInstanced instance 2 (StartInstanceLocation): %s", rgb(c));
    c = at(-0.75f, -0.75f);
    CHECK(is(c, 128, 0, 255), "DrawIndexedInstanced instance 3: %s", rgb(c));
    c = at(-0.25f, -0.5f);
    CHECK(is(c, 0, 0, 255), "VS reads Buffer<float4>[1] through VSSetShaderResources: %s", rgb(c));
    c = at(0.25f, -0.5f);
    CHECK(is(c, 255, 255, 0), "VS samples a texture through VSSetShaderResources + VSSetSamplers: %s", rgb(c));
    c = at(0.75f, -0.5f);
    CHECK(is(c, 0, 255, 255), "PS reads StructuredBuffer[2]: %s", rgb(c));
    c = at(0.5f, 0.5f);
    CHECK(is(c, 0, 0, 0), "no-render-target draw left the back buffer untouched (gap between instances): %s", rgb(c));
    c = at(-0.75f, -0.5f);
    CHECK(is(c, 0, 0, 0), "background between indexed instances untouched: %s", rgb(c));

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
