// Standalone Direct3D 11 sample: a rotating, textured, vertex-coloured cube with depth testing.
// It is a plain D3D11 application. Placed next to the proxy d3d11.dll it renders on the remote
// GPU server; on its own it renders on the local GPU.
//
// Usage: sample_cube.exe [--frames N] [--width W] [--height H]
//   --frames N   exit after N presented frames (default 0: run until the window is closed)
// Exit codes: 0 success, 1 device creation failed, 2 resource setup failed, 3 Present failed.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cube_geometry.h"
#include "shaders.h"

using Microsoft::WRL::ComPtr;

namespace {

struct Options {
    int frames = 0;
    UINT width = 640;
    UINT height = 480;
};

bool ParseArgs(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--frames" || arg == "--width" || arg == "--height") && i + 1 < argc) {
            const int value = std::atoi(argv[++i]);
            if (value < 0 || (arg != "--frames" && (value < 16 || value > 4096))) {
                std::fprintf(stderr, "[Sample] Invalid value for %s\n", arg.c_str());
                return false;
            }
            if (arg == "--frames") opt.frames = value;
            else if (arg == "--width") opt.width = static_cast<UINT>(value);
            else opt.height = static_cast<UINT>(value);
        } else {
            std::fprintf(stderr, "usage: %s [--frames N] [--width W] [--height H]\n", argv[0]);
            return false;
        }
    }
    return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

ComPtr<ID3DBlob> CompileShader(const char* entry, const char* target) {
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(Sample::kCubeShaderSource, std::strlen(Sample::kCubeShaderSource), "cube.hlsl", nullptr,
                            nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[Sample] Shader %s failed: %s\n", entry,
                     errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
        return nullptr;
    }
    return code;
}

// 64x64 RGBA checkerboard (8x8 cells) used as the cube texture.
std::vector<uint32_t> MakeCheckerboard(UINT size) {
    std::vector<uint32_t> pixels(size * size);
    for (UINT y = 0; y < size; ++y)
        for (UINT x = 0; x < size; ++x)
            pixels[y * size + x] = (((x / 8) + (y / 8)) & 1) ? 0xFFFFFFFFu : 0xFF707070u;
    return pixels;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!ParseArgs(argc, argv, opt)) return 2;

    // --- Window -------------------------------------------------------------
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "RgpuSampleCube";
    RegisterClassA(&wc);
    RECT rc = {0, 0, static_cast<LONG>(opt.width), static_cast<LONG>(opt.height)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "RemoteGPU sample cube", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 2;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    // --- Device & swap chain ---------------------------------------------------
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = opt.width;
    scd.BufferDesc.Height = opt.height;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    const D3D_FEATURE_LEVEL wanted = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &wanted, 1,
                                               D3D11_SDK_VERSION, &scd, &swap, &dev, nullptr, &ctx);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[Sample] D3D11CreateDeviceAndSwapChain failed: 0x%08lX\n", static_cast<unsigned long>(hr));
        return 1;
    }

    // --- Render targets ---------------------------------------------------------
    ComPtr<ID3D11Texture2D> back;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11DepthStencilView> dsv;
    bool ok = SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), &back)) &&
              SUCCEEDED(dev->CreateRenderTargetView(back.Get(), nullptr, &rtv));

    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width = opt.width;
    dd.Height = opt.height;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&dd, nullptr, &depth)) &&
         SUCCEEDED(dev->CreateDepthStencilView(depth.Get(), nullptr, &dsv));

    // --- Shaders & input layout --------------------------------------------------
    ComPtr<ID3DBlob> vsb = CompileShader("VSMain", "vs_4_0");
    ComPtr<ID3DBlob> psb = CompileShader("PSMain", "ps_4_0");
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    const D3D11_INPUT_ELEMENT_DESC elems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Sample::Vertex, pos), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Sample::Vertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Sample::Vertex, uv), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ok = ok && vsb && psb &&
         SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs)) &&
         SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps)) &&
         SUCCEEDED(dev->CreateInputLayout(elems, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout));

    // --- Geometry ------------------------------------------------------------------
    std::vector<Sample::Vertex> vertices;
    std::vector<uint16_t> indices;
    Sample::BuildCube(vertices, indices);

    ComPtr<ID3D11Buffer> vb, ib, cb;
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Sample::Vertex));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = {vertices.data(), 0, 0};
    ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, &init, &vb));

    bd.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint16_t));
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    init.pSysMem = indices.data();
    ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, &init, &ib));

    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = sizeof(Sample::Mat4);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &cb));

    // --- Texture & sampler -----------------------------------------------------------
    const UINT tex_size = 64;
    std::vector<uint32_t> checker = MakeCheckerboard(tex_size);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = tex_size;
    td.Height = tex_size;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA tinit = {checker.data(), tex_size * 4, 0};
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11SamplerState> sampler;
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxAnisotropy = 1;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&td, &tinit, &tex)) &&
         SUCCEEDED(dev->CreateShaderResourceView(tex.Get(), nullptr, &srv)) &&
         SUCCEEDED(dev->CreateSamplerState(&sd, &sampler));

    if (!ok) {
        std::fprintf(stderr, "[Sample] Resource setup failed.\n");
        return 2;
    }

    // --- Render loop -------------------------------------------------------------------
    const float aspect = static_cast<float>(opt.width) / static_cast<float>(opt.height);
    const Sample::Mat4 view = Sample::Translation(0.0f, 0.0f, 2.6f);
    const Sample::Mat4 proj = Sample::PerspectiveFovLH(1.0f, aspect, 0.1f, 100.0f);
    const D3D11_VIEWPORT vp = {0, 0, static_cast<float>(opt.width), static_cast<float>(opt.height), 0, 1};
    const float clear[4] = {0.05f, 0.06f, 0.15f, 1.0f};
    const UINT stride = sizeof(Sample::Vertex), offset = 0;
    ID3D11ShaderResourceView* srvs[] = {srv.Get()};
    ID3D11SamplerState* samplers[] = {sampler.Get()};

    int presented = 0;
    int exit_code = 0;
    const auto t_start = std::chrono::steady_clock::now();
    bool running = true;
    while (running && (opt.frames == 0 || presented < opt.frames)) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!running) break;

        // Animation is driven by the frame index so every run produces the same frames.
        const float t = presented * (1.0f / 60.0f);
        const Sample::Mat4 world = Sample::Multiply(Sample::RotationY(0.8f + t * 1.1f), Sample::RotationX(0.5f + t * 0.7f));
        const Sample::Mat4 wvp = Sample::Transpose(Sample::Multiply(Sample::Multiply(world, view), proj));

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::fprintf(stderr, "[Sample] Map of constant buffer failed.\n");
            exit_code = 2;
            break;
        }
        std::memcpy(mapped.pData, &wvp, sizeof(wvp));
        ctx->Unmap(cb.Get(), 0);

        ID3D11RenderTargetView* rtvs[] = {rtv.Get()};
        ID3D11Buffer* vbs[] = {vb.Get()};
        ID3D11Buffer* cbs[] = {cb.Get()};
        ctx->OMSetRenderTargets(1, rtvs, dsv.Get());
        ctx->RSSetViewports(1, &vp);
        ctx->ClearRenderTargetView(rtv.Get(), clear);
        ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        ctx->IASetInputLayout(layout.Get());
        ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
        ctx->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, cbs);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->PSSetShaderResources(0, 1, srvs);
        ctx->PSSetSamplers(0, 1, samplers);
        ctx->DrawIndexed(static_cast<UINT>(indices.size()), 0, 0);

        hr = swap->Present(0, 0);
        if (FAILED(hr)) {
            std::fprintf(stderr, "[Sample] Present failed at frame %d: 0x%08lX\n", presented + 1, static_cast<unsigned long>(hr));
            exit_code = 3;
            break;
        }
        ++presented;
    }

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    std::printf("[Sample] Presented %d frames in %.2f s (%.1f FPS)\n", presented, seconds,
                seconds > 0 ? presented / seconds : 0.0);

    // Release device objects before the window goes away (the proxy swap chain captures on release).
    ctx->ClearState();
    sampler.Reset(); srv.Reset(); tex.Reset(); cb.Reset(); ib.Reset(); vb.Reset();
    layout.Reset(); ps.Reset(); vs.Reset(); dsv.Reset(); depth.Reset(); rtv.Reset(); back.Reset();
    ctx.Reset(); swap.Reset(); dev.Reset();
    DestroyWindow(hwnd);
    return exit_code;
}
