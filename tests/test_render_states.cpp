// End-to-end test of fixed-function state forwarding (blend, depth-stencil, rasterizer, scissor).
// Usage: test_render_states.exe <path-to-proxy-d3d11.dll>
// Requires a multi-session server on RGPU_SERVER:RGPU_PORT: each scenario opens its own session,
// renders a few frames and checks pixels of its captured last frame.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
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

constexpr UINT kSize = 256;
const float kBlue[4] = {0.0f, 0.0f, 1.0f, 1.0f};

const char* kShaderSource =
    "struct VS_IN { float3 pos : POSITION; float4 col : COLOR; };\n"
    "struct PS_IN { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
    "PS_IN VSMain(VS_IN i) { PS_IN o; o.pos = float4(i.pos, 1.0f); o.col = i.col; return o; }\n"
    "float4 PSMain(PS_IN i) : SV_TARGET { return i.col; }\n";

struct Vertex {
    float pos[3];
    float col[4];
};

struct Rgb {
    int r, g, b;
};

PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN g_create = nullptr;
ID3DBlob* g_vs = nullptr;
ID3DBlob* g_ps = nullptr;
HWND g_hwnd = nullptr;

// Everything a scenario needs: device, context, back buffer views and a dynamic vertex buffer.
struct Scene {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11Buffer* vb = nullptr;

    void Draw(const std::vector<Vertex>& verts) {
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (FAILED(ctx->Map(vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
        std::memcpy(m.pData, verts.data(), verts.size() * sizeof(Vertex));
        ctx->Unmap(vb, 0);
        ctx->Draw(static_cast<UINT>(verts.size()), 0);
    }

    // Axis-aligned quad in NDC, wound clockwise (front-facing by default).
    void Quad(float x0, float y0, float x1, float y1, float z, const float c[4]) {
        auto v = [&](float x, float y) { return Vertex{{x, y, z}, {c[0], c[1], c[2], c[3]}}; };
        Draw({v(x0, y1), v(x1, y1), v(x1, y0), v(x0, y1), v(x1, y0), v(x0, y0)});
    }

    // Large triangle around the centre, wound counter-clockwise (back-facing by default).
    void CcwTriangle(const float c[4]) {
        auto v = [&](float x, float y) { return Vertex{{x, y, 0.5f}, {c[0], c[1], c[2], c[3]}}; };
        Draw({v(0.0f, 0.9f), v(-0.9f, -0.9f), v(0.9f, -0.9f)});
    }
};

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

// Runs one scenario in its own session and returns the captured frame (BGRA, kSize x kSize).
bool RunScenario(const char* name, const std::function<void(Scene&)>& setup, const std::function<void(Scene&)>& draw,
                 std::vector<uint8_t>& frame) {
    char capture[MAX_PATH];
    GetTempPathA(MAX_PATH, capture);
    std::strcat(capture, "rgpu_states_");
    std::strcat(capture, name);
    std::strcat(capture, ".bmp");
    DeleteFileA(capture);
    _putenv_s("RGPU_CAPTURE_FILE", capture);

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = kSize;
    scd.BufferDesc.Height = kSize;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = g_hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;

    Scene s;
    IDXGISwapChain* swap = nullptr;
    if (FAILED(g_create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &scd, &swap, &s.dev,
                        nullptr, &s.ctx))) {
        return false;
    }

    ID3D11Texture2D* back = nullptr;
    swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
    s.dev->CreateRenderTargetView(back, nullptr, &s.rtv);
    back->Release();

    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width = dd.Height = kSize;
    dd.MipLevels = dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D* depth = nullptr;
    s.dev->CreateTexture2D(&dd, nullptr, &depth);
    s.dev->CreateDepthStencilView(depth, nullptr, &s.dsv);

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;
    s.dev->CreateVertexShader(g_vs->GetBufferPointer(), g_vs->GetBufferSize(), nullptr, &vs);
    s.dev->CreatePixelShader(g_ps->GetBufferPointer(), g_ps->GetBufferSize(), nullptr, &ps);
    const D3D11_INPUT_ELEMENT_DESC elems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    s.dev->CreateInputLayout(elems, 2, g_vs->GetBufferPointer(), g_vs->GetBufferSize(), &layout);
    D3D11_BUFFER_DESC bd = {16 * sizeof(Vertex), D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    s.dev->CreateBuffer(&bd, nullptr, &s.vb);

    setup(s);
    const D3D11_VIEWPORT vp = {0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0, 1};
    const UINT stride = sizeof(Vertex), offset = 0;
    bool ok = true;
    for (int frame_i = 0; frame_i < 3 && ok; ++frame_i) {
        s.ctx->OMSetRenderTargets(1, &s.rtv, s.dsv);
        s.ctx->RSSetViewports(1, &vp);
        s.ctx->ClearRenderTargetView(s.rtv, kBlue);
        s.ctx->ClearDepthStencilView(s.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        s.ctx->IASetInputLayout(layout);
        s.ctx->IASetVertexBuffers(0, 1, &s.vb, &stride, &offset);
        s.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        s.ctx->VSSetShader(vs, nullptr, 0);
        s.ctx->PSSetShader(ps, nullptr, 0);
        draw(s);
        ok = SUCCEEDED(swap->Present(0, 0));
    }

    s.vb->Release();
    layout->Release();
    ps->Release();
    vs->Release();
    s.dsv->Release();
    depth->Release();
    s.rtv->Release();
    swap->Release(); // writes the capture
    s.ctx->Release();
    s.dev->Release();

    int w = 0, h = 0;
    return ok && ReadBmp(capture, frame, w, h) && w == static_cast<int>(kSize) && h == static_cast<int>(kSize);
}

// Pixel at NDC (x, y), y up.
Rgb At(const std::vector<uint8_t>& f, float x, float y) {
    int px = static_cast<int>((x + 1.0f) * 0.5f * (kSize - 1));
    int py = static_cast<int>((1.0f - y) * 0.5f * (kSize - 1));
    const uint8_t* p = &f[(static_cast<size_t>(py) * kSize + px) * 4];
    return {p[2], p[1], p[0]};
}

bool Near(Rgb c, int r, int g, int b, int tol = 12) {
    return std::abs(c.r - r) <= tol && std::abs(c.g - g) <= tol && std::abs(c.b - b) <= tol;
}

const char* Fmt(Rgb c) {
    static char buf[4][32];
    static int slot = 0;
    char* s = buf[slot++ % 4];
    std::snprintf(s, 32, "(%d,%d,%d)", c.r, c.g, c.b);
    return s;
}

ID3D11RasterizerState* MakeRaster(ID3D11Device* dev, D3D11_FILL_MODE fill, D3D11_CULL_MODE cull, BOOL ccw, BOOL scissor) {
    D3D11_RASTERIZER_DESC d = {};
    d.FillMode = fill;
    d.CullMode = cull;
    d.FrontCounterClockwise = ccw;
    d.DepthClipEnable = TRUE;
    d.ScissorEnable = scissor;
    ID3D11RasterizerState* s = nullptr;
    dev->CreateRasterizerState(&d, &s);
    return s;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <proxy d3d11.dll>\n", argv[0]);
        return 2;
    }
    HMODULE proxy = LoadLibraryA(argv[1]);
    g_create = proxy ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(GetProcAddress(proxy, "D3D11CreateDeviceAndSwapChain")) : nullptr;
    CHECK(g_create != nullptr, "proxy d3d11.dll loads");
    if (!g_create) return 1;

    D3DCompile(kShaderSource, std::strlen(kShaderSource), nullptr, nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &g_vs, nullptr);
    D3DCompile(kShaderSource, std::strlen(kShaderSource), nullptr, nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &g_ps, nullptr);
    CHECK(g_vs && g_ps, "shaders compiled");
    if (!g_vs || !g_ps) return 1;

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "RgpuStatesTest";
    RegisterClassA(&wc);
    RECT rc = {0, 0, static_cast<LONG>(kSize), static_cast<LONG>(kSize)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowA(wc.lpszClassName, "RemoteGPU render states", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);

    const float yellow[4] = {1, 1, 0, 1}, red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1};
    const float halfRed[4] = {1, 0, 0, 0.5f};
    std::vector<uint8_t> f;
    auto none = [](Scene&) {};

    std::printf("== Rasterizer state\n");
    if (RunScenario("cull_default", none, [&](Scene& s) { s.CcwTriangle(yellow); }, f))
        CHECK(Near(At(f, 0, 0), 0, 0, 255), "default state culls a counter-clockwise triangle: centre %s", Fmt(At(f, 0, 0)));
    else CHECK(false, "cull_default scenario ran");

    ID3D11RasterizerState* rs = nullptr;
    if (RunScenario("cull_none", [&](Scene& s) { rs = MakeRaster(s.dev, D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, FALSE); },
                    [&](Scene& s) { s.ctx->RSSetState(rs); s.CcwTriangle(yellow); }, f))
        CHECK(Near(At(f, 0, 0), 255, 255, 0), "CullMode NONE draws it: centre %s", Fmt(At(f, 0, 0)));
    else CHECK(false, "cull_none scenario ran");
    if (rs) { rs->Release(); rs = nullptr; }

    if (RunScenario("front_ccw", [&](Scene& s) { rs = MakeRaster(s.dev, D3D11_FILL_SOLID, D3D11_CULL_BACK, TRUE, FALSE); },
                    [&](Scene& s) { s.ctx->RSSetState(rs); s.CcwTriangle(yellow); }, f))
        CHECK(Near(At(f, 0, 0), 255, 255, 0), "FrontCounterClockwise makes it front-facing: centre %s", Fmt(At(f, 0, 0)));
    else CHECK(false, "front_ccw scenario ran");
    if (rs) { rs->Release(); rs = nullptr; }

    if (RunScenario("wireframe", [&](Scene& s) { rs = MakeRaster(s.dev, D3D11_FILL_WIREFRAME, D3D11_CULL_NONE, FALSE, FALSE); },
                    [&](Scene& s) { s.ctx->RSSetState(rs); s.CcwTriangle(yellow); }, f)) {
        int edge = 0;
        for (size_t i = 0; i < f.size(); i += 4) edge += (f[i + 2] > 200 && f[i + 1] > 200) ? 1 : 0;
        CHECK(Near(At(f, 0, -0.2f), 0, 0, 255) && edge > 300 && edge < 6000,
              "wireframe draws edges only: interior %s, %d edge pixels", Fmt(At(f, 0, -0.2f)), edge);
    } else CHECK(false, "wireframe scenario ran");
    if (rs) { rs->Release(); rs = nullptr; }

    if (RunScenario("scissor", [&](Scene& s) { rs = MakeRaster(s.dev, D3D11_FILL_SOLID, D3D11_CULL_BACK, FALSE, TRUE); },
                    [&](Scene& s) {
                        const D3D11_RECT r = {0, 0, static_cast<LONG>(kSize / 2), static_cast<LONG>(kSize)};
                        s.ctx->RSSetState(rs);
                        s.ctx->RSSetScissorRects(1, &r);
                        s.Quad(-1, -1, 1, 1, 0.5f, red);
                    }, f))
        CHECK(Near(At(f, -0.5f, 0), 255, 0, 0) && Near(At(f, 0.5f, 0), 0, 0, 255),
              "scissor keeps the left half only: left %s, right %s", Fmt(At(f, -0.5f, 0)), Fmt(At(f, 0.5f, 0)));
    else CHECK(false, "scissor scenario ran");
    if (rs) { rs->Release(); rs = nullptr; }

    std::printf("== Blend state\n");
    ID3D11BlendState* bs = nullptr;
    auto makeAlphaBlend = [&](Scene& s) {
        D3D11_BLEND_DESC d = {}; // render targets 1-7 left zeroed, as applications do
        d.RenderTarget[0].BlendEnable = TRUE;
        d.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        d.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        s.dev->CreateBlendState(&d, &bs);
    };
    if (RunScenario("blend_alpha", makeAlphaBlend, [&](Scene& s) {
            s.ctx->OMSetBlendState(bs, nullptr, 0xFFFFFFFF);
            s.Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.5f, halfRed);
        }, f))
        CHECK(Near(At(f, 0, 0), 128, 0, 128, 16), "50%% red over blue blends to purple: centre %s", Fmt(At(f, 0, 0)));
    else CHECK(false, "blend_alpha scenario ran");

    if (RunScenario("blend_reset", makeAlphaBlend, [&](Scene& s) {
            s.ctx->OMSetBlendState(bs, nullptr, 0xFFFFFFFF);
            s.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            s.Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.5f, halfRed);
        }, f))
        CHECK(Near(At(f, 0, 0), 255, 0, 0), "a null blend state restores opaque output: centre %s", Fmt(At(f, 0, 0)));
    else CHECK(false, "blend_reset scenario ran");
    if (bs) { bs->Release(); bs = nullptr; }

    std::printf("== Depth-stencil state\n");
    if (RunScenario("depth_default", none, [&](Scene& s) {
            s.Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.2f, green);
            s.Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.8f, red);
        }, f))
        CHECK(Near(At(f, 0, 0), 0, 255, 0), "default depth test keeps the nearer quad: centre %s", Fmt(At(f, 0, 0)));
    else CHECK(false, "depth_default scenario ran");

    ID3D11DepthStencilState* ds = nullptr;
    if (RunScenario("depth_off", [&](Scene& s) {
            D3D11_DEPTH_STENCIL_DESC d = {}; // DepthEnable FALSE, everything else zeroed
            s.dev->CreateDepthStencilState(&d, &ds);
        }, [&](Scene& s) {
            s.ctx->OMSetDepthStencilState(ds, 0);
            s.Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.2f, green);
            s.Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.8f, red);
        }, f))
        CHECK(Near(At(f, 0, 0), 255, 0, 0), "depth disabled keeps the last quad drawn: centre %s", Fmt(At(f, 0, 0)));
    else CHECK(false, "depth_off scenario ran");
    if (ds) { ds->Release(); ds = nullptr; }

    ID3D11DepthStencilState* mark = nullptr;
    ID3D11DepthStencilState* test = nullptr;
    ID3D11BlendState* noColor = nullptr;
    if (RunScenario("stencil", [&](Scene& s) {
            D3D11_DEPTH_STENCIL_DESC d = {};
            d.StencilEnable = TRUE;
            d.StencilReadMask = d.StencilWriteMask = 0xFF;
            d.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS};
            d.BackFace = d.FrontFace;
            s.dev->CreateDepthStencilState(&d, &mark);
            d.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_EQUAL};
            d.BackFace = d.FrontFace;
            s.dev->CreateDepthStencilState(&d, &test);
            D3D11_BLEND_DESC b = {};
            b.RenderTarget[0].RenderTargetWriteMask = 0; // write stencil only
            s.dev->CreateBlendState(&b, &noColor);
        }, [&](Scene& s) {
            s.ctx->OMSetBlendState(noColor, nullptr, 0xFFFFFFFF);
            s.ctx->OMSetDepthStencilState(mark, 1);
            s.Quad(-1, -1, 0, 1, 0.5f, red);            // marks the left half, invisible
            s.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            s.ctx->OMSetDepthStencilState(test, 1);
            s.Quad(-1, -1, 1, 1, 0.5f, green);          // only where stencil == 1
        }, f))
        CHECK(Near(At(f, -0.5f, 0), 0, 255, 0) && Near(At(f, 0.5f, 0), 0, 0, 255),
              "stencil mask + colour write mask: left %s, right %s", Fmt(At(f, -0.5f, 0)), Fmt(At(f, 0.5f, 0)));
    else CHECK(false, "stencil scenario ran");
    if (mark) mark->Release();
    if (test) test->Release();
    if (noColor) noColor->Release();

    DestroyWindow(g_hwnd);
    g_vs->Release();
    g_ps->Release();
    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
