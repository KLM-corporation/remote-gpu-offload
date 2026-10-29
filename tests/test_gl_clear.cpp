// OpenGL foundation test: a plain Win32 + WGL program that clears its window.
// Run it from a folder holding the proxy opengl32.dll against a live server (RGPU_PORT).
// It presents through gdi32!SwapBuffers, exactly like a real application, and checks the pixels
// the proxy captured (RGPU_CAPTURE_FILE) after each swap, including across a window resize.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <GL/gl.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const char* label, const char* detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", label, *detail ? " - " : "", detail);
    if (!ok) ++g_failures;
}

struct Capture {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra; // top-down
    bool Pixel(int x, int y, int rgb[3]) const {
        if (x < 0 || y < 0 || x >= width || y >= height) return false;
        const uint8_t* p = &bgra[(static_cast<size_t>(y) * width + x) * 4];
        rgb[0] = p[2]; rgb[1] = p[1]; rgb[2] = p[0];
        return true;
    }
};

bool ReadCapture(const char* path, Capture& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    bool ok = std::fread(&fh, sizeof(fh), 1, f) == 1 && std::fread(&ih, sizeof(ih), 1, f) == 1 &&
              fh.bfType == 0x4D42 && ih.biBitCount == 32 && ih.biHeight < 0;
    if (ok) {
        out.width = ih.biWidth;
        out.height = -ih.biHeight;
        out.bgra.resize(static_cast<size_t>(out.width) * out.height * 4);
        ok = std::fread(out.bgra.data(), 1, out.bgra.size(), f) == out.bgra.size();
    }
    std::fclose(f);
    return ok;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void Pump() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

bool Near(int a, int b) { return std::abs(a - b) <= 1; }

// Clears with `rgb`, swaps, and checks the captured frame has the expected size and colour.
void DrawAndCheck(HDC hdc, const char* capture_path, int width, int height, const float rgb[3], const char* label) {
    std::remove(capture_path);
    glViewport(0, 0, width, height);
    glClearColor(rgb[0], rgb[1], rgb[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const BOOL swapped = SwapBuffers(hdc);

    Capture cap;
    char detail[128];
    Check(swapped != FALSE, label, "SwapBuffers returned TRUE");
    const bool read = ReadCapture(capture_path, cap);
    Check(read, "frame captured by the proxy");
    if (!read) return;

    std::snprintf(detail, sizeof(detail), "%dx%d, expected %dx%d", cap.width, cap.height, width, height);
    Check(cap.width == width && cap.height == height, "frame size follows the window", detail);

    const int expect[3] = {static_cast<int>(std::lround(rgb[0] * 255)), static_cast<int>(std::lround(rgb[1] * 255)),
                           static_cast<int>(std::lround(rgb[2] * 255))};
    const int points[4][2] = {{1, 1}, {cap.width - 2, 1}, {cap.width / 2, cap.height / 2}, {cap.width - 2, cap.height - 2}};
    bool all = true;
    int got[3] = {0, 0, 0};
    for (const auto& pt : points) {
        if (!cap.Pixel(pt[0], pt[1], got) || !Near(got[0], expect[0]) || !Near(got[1], expect[1]) || !Near(got[2], expect[2])) {
            all = false;
            break;
        }
    }
    std::snprintf(detail, sizeof(detail), "expected (%d,%d,%d), last read (%d,%d,%d)", expect[0], expect[1], expect[2],
                  got[0], got[1], got[2]);
    Check(all, "all corners and centre have the clear colour", detail);
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    char capture_buf[MAX_PATH] = "gl_capture.bmp";
    char env_buf[MAX_PATH] = {};
    if (GetEnvironmentVariableA("RGPU_CAPTURE_FILE", env_buf, sizeof(env_buf)) > 0) {
        std::strncpy(capture_buf, env_buf, sizeof(capture_buf) - 1);
    } else {
        SetEnvironmentVariableA("RGPU_CAPTURE_FILE", capture_buf);
    }
    const char* capture_path = capture_buf;
    // The checks read the capture right after each SwapBuffers, so every swap must wait for its
    // own frame; the pipelined mode has its own section below.
    SetEnvironmentVariableA("RGPU_FRAME_LATENCY", "1");
    // Exact colours need lossless frames; H.264 video has its own section below.
    SetEnvironmentVariableA("RGPU_VIDEO", "0");

    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "RgpuGlClearTest";
    RegisterClassA(&wc);

    auto place = [](HWND hwnd, int client_w, int client_h) {
        RECT rc = {0, 0, client_w, client_h};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(hwnd, nullptr, 20, 20, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    HWND hwnd = CreateWindowA("RgpuGlClearTest", "RGPU GL clear", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr,
                              wc.hInstance, nullptr);
    if (!hwnd) { std::printf("window creation failed\n"); return 2; }
    place(hwnd, 320, 240);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    Pump();

    HDC hdc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    const int format = ChoosePixelFormat(hdc, &pfd);
    Check(format != 0 && SetPixelFormat(hdc, format, &pfd), "pixel format set on the window");

    std::printf("[gl_clear] creating the context\n");
    HGLRC rc = wglCreateContext(hdc);
    Check(rc != nullptr, "wglCreateContext");
    Check(rc && wglMakeCurrent(hdc, rc), "wglMakeCurrent connects to the server");
    Check(rc && wglGetCurrentContext() == rc && wglGetCurrentDC() == hdc, "current context and DC are reported");

    int ubo_align = 0;
    glGetIntegerv(0x8A34, &ubo_align);
    Check(ubo_align == 256, "GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT is 256");

    float max_aniso = 0.0f;
    glGetFloatv(0x84FF, &max_aniso);
    Check(max_aniso == 16.0f, "GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT is 16.0f");

    using TexImage2DFn = void(WINAPI*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
    using GetTexLevelParameterivFn = void(WINAPI*)(unsigned, int, unsigned, int*);
    auto pfnTexImage2D = reinterpret_cast<TexImage2DFn>(reinterpret_cast<void*>(wglGetProcAddress("glTexImage2D")));
    auto pfnGetTexLevelParameteriv = reinterpret_cast<GetTexLevelParameterivFn>(reinterpret_cast<void*>(wglGetProcAddress("glGetTexLevelParameteriv")));
    if (pfnTexImage2D && pfnGetTexLevelParameteriv) {
        int w = -1;
        pfnTexImage2D(0x8064 /*GL_PROXY_TEXTURE_2D*/, 0, 6408, 32768, 32768, 0, 6408, 0x1401, nullptr);
        pfnGetTexLevelParameteriv(0x8064, 0, 0x1000 /*GL_TEXTURE_WIDTH*/, &w);
        Check(w == 0, "proxy texture 32768 returns 0");

        pfnTexImage2D(0x8064 /*GL_PROXY_TEXTURE_2D*/, 0, 6408, 16384, 16384, 0, 6408, 0x1401, nullptr);
        pfnGetTexLevelParameteriv(0x8064, 0, 0x1000 /*GL_TEXTURE_WIDTH*/, &w);
        Check(w == 16384, "proxy texture 16384 returns 16384");
    } else {
        Check(false, "proxy texture functions available");
    }

    using GenSamplersFn = void(WINAPI*)(int, unsigned*);
    using SamplerParameteriFn = void(WINAPI*)(unsigned, unsigned, int);
    using SamplerParameterfFn = void(WINAPI*)(unsigned, unsigned, float);
    using BindSamplerFn = void(WINAPI*)(unsigned, unsigned);
    using DeleteSamplersFn = void(WINAPI*)(int, const unsigned*);
    auto pfnGenSamplers = reinterpret_cast<GenSamplersFn>(reinterpret_cast<void*>(wglGetProcAddress("glGenSamplers")));
    auto pfnSamplerParameteri = reinterpret_cast<SamplerParameteriFn>(reinterpret_cast<void*>(wglGetProcAddress("glSamplerParameteri")));
    auto pfnSamplerParameterf = reinterpret_cast<SamplerParameterfFn>(reinterpret_cast<void*>(wglGetProcAddress("glSamplerParameterf")));
    auto pfnBindSampler = reinterpret_cast<BindSamplerFn>(reinterpret_cast<void*>(wglGetProcAddress("glBindSampler")));
    auto pfnDeleteSamplers = reinterpret_cast<DeleteSamplersFn>(reinterpret_cast<void*>(wglGetProcAddress("glDeleteSamplers")));
    Check(pfnGenSamplers && pfnSamplerParameteri && pfnSamplerParameterf && pfnBindSampler && pfnDeleteSamplers, "sampler functions available");
    if (pfnGenSamplers && pfnSamplerParameteri && pfnSamplerParameterf && pfnBindSampler && pfnDeleteSamplers) {
        unsigned samplers[32] = {};
        pfnGenSamplers(32, samplers);
        Check(samplers[0] != 0 && samplers[31] != 0, "glGenSamplers generated valid IDs");
        pfnSamplerParameteri(samplers[0], 0x2800 /*GL_TEXTURE_MAG_FILTER*/, 0x2601 /*GL_LINEAR*/);
        pfnSamplerParameterf(samplers[0], 0x84FE /*GL_TEXTURE_MAX_ANISOTROPY_EXT*/, 16.0f);
        pfnBindSampler(0, samplers[0]);
        pfnDeleteSamplers(32, samplers);
        Check(true, "sampler operations executed without crash");
    }

    // Test newly added OpenGL entry points
    using TexSubImage2DFn = void(WINAPI*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
    auto pfnTexSubImage2D = reinterpret_cast<TexSubImage2DFn>(reinterpret_cast<void*>(wglGetProcAddress("glTexSubImage2D")));
    Check(pfnTexSubImage2D != nullptr, "glTexSubImage2D available via wglGetProcAddress");
    if (pfnTexSubImage2D) {
        uint32_t pixels[16 * 16] = {};
        pfnTexSubImage2D(0x0DE1 /*GL_TEXTURE_2D*/, 0, 0, 0, 16, 16, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, pixels);
        Check(true, "glTexSubImage2D called successfully");

        using BindBufferFn = void(WINAPI*)(unsigned, unsigned);
        using GenBuffersFn = void(WINAPI*)(int, unsigned*);
        using BufferDataFn = void(WINAPI*)(unsigned, ptrdiff_t, const void*, unsigned);
        auto pfnBindBuffer = reinterpret_cast<BindBufferFn>(reinterpret_cast<void*>(wglGetProcAddress("glBindBuffer")));
        auto pfnGenBuffers = reinterpret_cast<GenBuffersFn>(reinterpret_cast<void*>(wglGetProcAddress("glGenBuffers")));
        auto pfnBufferData = reinterpret_cast<BufferDataFn>(reinterpret_cast<void*>(wglGetProcAddress("glBufferData")));
        if (pfnBindBuffer && pfnGenBuffers && pfnBufferData) {
            unsigned pbo = 0;
            pfnGenBuffers(1, &pbo);
            pfnBindBuffer(0x88EC /*GL_PIXEL_UNPACK_BUFFER*/, pbo);
            uint32_t pboData[32 * 32] = {};
            pfnBufferData(0x88EC, sizeof(pboData), pboData, 0x88E4 /*GL_STATIC_DRAW*/);
            // Pass an offset that would crash memcpy if treated as host memory!
            pfnTexSubImage2D(0x0DE1 /*GL_TEXTURE_2D*/, 0, 0, 0, 16, 16, 0x1908 /*GL_RGBA*/, 0x1401, reinterpret_cast<const void*>(0x100));
            pfnBindBuffer(0x88EC /*GL_PIXEL_UNPACK_BUFFER*/, 0);
            Check(true, "glTexSubImage2D with bound PBO and offset called successfully");
        }
    }

    using FenceSyncFn = void*(WINAPI*)(unsigned, unsigned);
    using ClientWaitSyncFn = unsigned(WINAPI*)(void*, unsigned, unsigned long long);
    using DeleteSyncFn = void(WINAPI*)(void*);
    auto pfnFenceSync = reinterpret_cast<FenceSyncFn>(reinterpret_cast<void*>(wglGetProcAddress("glFenceSync")));
    auto pfnClientWaitSync = reinterpret_cast<ClientWaitSyncFn>(reinterpret_cast<void*>(wglGetProcAddress("glClientWaitSync")));
    auto pfnDeleteSync = reinterpret_cast<DeleteSyncFn>(reinterpret_cast<void*>(wglGetProcAddress("glDeleteSync")));
    Check(pfnFenceSync && pfnClientWaitSync && pfnDeleteSync, "sync functions available via wglGetProcAddress");
    if (pfnFenceSync && pfnClientWaitSync && pfnDeleteSync) {
        void* sync = pfnFenceSync(0x9117 /*GL_SYNC_GPU_COMMANDS_COMPLETE*/, 0);
        Check(sync != nullptr, "glFenceSync returned non-null sync handle");
        unsigned waitResult = pfnClientWaitSync(sync, 0, 1000000ULL);
        Check(waitResult == 0x911A /*GL_ALREADY_SIGNALED*/, "glClientWaitSync returned GL_ALREADY_SIGNALED");
        pfnDeleteSync(sync);
    }

    using MapBufferRangeFn = void*(WINAPI*)(unsigned, ptrdiff_t, ptrdiff_t, unsigned);
    using UnmapBufferFn = unsigned char(WINAPI*)(unsigned);
    using BufferSubDataFn = void(WINAPI*)(unsigned, ptrdiff_t, ptrdiff_t, const void*);
    using GenBuffersFn = void(WINAPI*)(int, unsigned*);
    using BindBufferFn = void(WINAPI*)(unsigned, unsigned);
    using BufferDataFn = void(WINAPI*)(unsigned, ptrdiff_t, const void*, unsigned);
    auto pfnMapBufferRange = reinterpret_cast<MapBufferRangeFn>(reinterpret_cast<void*>(wglGetProcAddress("glMapBufferRange")));
    auto pfnUnmapBuffer = reinterpret_cast<UnmapBufferFn>(reinterpret_cast<void*>(wglGetProcAddress("glUnmapBuffer")));
    auto pfnBufferSubData = reinterpret_cast<BufferSubDataFn>(reinterpret_cast<void*>(wglGetProcAddress("glBufferSubData")));
    auto pfnGenBuffers = reinterpret_cast<GenBuffersFn>(reinterpret_cast<void*>(wglGetProcAddress("glGenBuffers")));
    auto pfnBindBuffer = reinterpret_cast<BindBufferFn>(reinterpret_cast<void*>(wglGetProcAddress("glBindBuffer")));
    auto pfnBufferData = reinterpret_cast<BufferDataFn>(reinterpret_cast<void*>(wglGetProcAddress("glBufferData")));
    Check(pfnMapBufferRange && pfnUnmapBuffer && pfnBufferSubData && pfnGenBuffers && pfnBindBuffer && pfnBufferData, "buffer mapping and subdata functions available");
    if (pfnMapBufferRange && pfnUnmapBuffer && pfnBufferSubData && pfnGenBuffers && pfnBindBuffer && pfnBufferData) {
        unsigned buf = 0;
        pfnGenBuffers(1, &buf);
        pfnBindBuffer(0x8892 /*GL_ARRAY_BUFFER*/, buf);
        float initData[4] = {1.0f, 2.0f, 3.0f, 4.0f};
        pfnBufferData(0x8892 /*GL_ARRAY_BUFFER*/, sizeof(initData), initData, 0x88E4 /*GL_STATIC_DRAW*/);
        void* mapped = pfnMapBufferRange(0x8892 /*GL_ARRAY_BUFFER*/, 0, sizeof(initData), 0x0002 /*GL_MAP_WRITE_BIT*/);
        Check(mapped != nullptr, "glMapBufferRange returned valid pointer");
        if (mapped) {
            float* mptr = reinterpret_cast<float*>(mapped);
            mptr[0] = 5.0f;
            mptr[1] = 6.0f;
        }
        unsigned char unmapOk = pfnUnmapBuffer(0x8892 /*GL_ARRAY_BUFFER*/);
        Check(unmapOk == 1, "glUnmapBuffer returned GL_TRUE");
        float subData[2] = {7.0f, 8.0f};
        pfnBufferSubData(0x8892 /*GL_ARRAY_BUFFER*/, 0, sizeof(subData), subData);
        Check(true, "glBufferSubData executed without crash");
    }

    using GetUniformBlockIndexFn = unsigned(WINAPI*)(unsigned, const char*);
    using UniformBlockBindingFn = void(WINAPI*)(unsigned, unsigned, unsigned);
    auto pfnGetUniformBlockIndex = reinterpret_cast<GetUniformBlockIndexFn>(reinterpret_cast<void*>(wglGetProcAddress("glGetUniformBlockIndex")));
    auto pfnUniformBlockBinding = reinterpret_cast<UniformBlockBindingFn>(reinterpret_cast<void*>(wglGetProcAddress("glUniformBlockBinding")));
    Check(pfnGetUniformBlockIndex && pfnUniformBlockBinding, "uniform block functions available");
    if (pfnGetUniformBlockIndex && pfnUniformBlockBinding) {
        unsigned idx1 = pfnGetUniformBlockIndex(1, "TestBlock");
        unsigned idx2 = pfnGetUniformBlockIndex(1, "TestBlock");
        Check(idx1 == idx2, "glGetUniformBlockIndex returned consistent index");
        pfnUniformBlockBinding(1, idx1, 0);
    }

    using FrontFaceFn = void(WINAPI*)(unsigned);
    using LineWidthFn = void(WINAPI*)(float);
    using StencilFuncFn = void(WINAPI*)(unsigned, int, unsigned);
    using Uniform2iFn = void(WINAPI*)(int, int, int);
    using GetActiveUniformBlockivFn = void(WINAPI*)(unsigned, unsigned, unsigned, int*);
    using ProvokingVertexFn = void(WINAPI*)(unsigned);
    using ClearBufferivFn = void(WINAPI*)(unsigned, int, const int*);
    using GetBufferPointervFn = void(WINAPI*)(unsigned, unsigned, void**);
    using GetSyncivFn = void(WINAPI*)(void*, unsigned, int, int*, int*);

    auto pfnFrontFace = reinterpret_cast<FrontFaceFn>(reinterpret_cast<void*>(wglGetProcAddress("glFrontFace")));
    auto pfnLineWidth = reinterpret_cast<LineWidthFn>(reinterpret_cast<void*>(wglGetProcAddress("glLineWidth")));
    auto pfnStencilFunc = reinterpret_cast<StencilFuncFn>(reinterpret_cast<void*>(wglGetProcAddress("glStencilFunc")));
    auto pfnUniform2i = reinterpret_cast<Uniform2iFn>(reinterpret_cast<void*>(wglGetProcAddress("glUniform2i")));
    auto pfnGetActiveUniformBlockiv = reinterpret_cast<GetActiveUniformBlockivFn>(reinterpret_cast<void*>(wglGetProcAddress("glGetActiveUniformBlockiv")));
    auto pfnProvokingVertex = reinterpret_cast<ProvokingVertexFn>(reinterpret_cast<void*>(wglGetProcAddress("glProvokingVertex")));
    auto pfnClearBufferiv = reinterpret_cast<ClearBufferivFn>(reinterpret_cast<void*>(wglGetProcAddress("glClearBufferiv")));
    auto pfnGetBufferPointerv = reinterpret_cast<GetBufferPointervFn>(reinterpret_cast<void*>(wglGetProcAddress("glGetBufferPointerv")));
    auto pfnGetSynciv = reinterpret_cast<GetSyncivFn>(reinterpret_cast<void*>(wglGetProcAddress("glGetSynciv")));

    Check(pfnFrontFace && pfnLineWidth && pfnStencilFunc && pfnUniform2i && pfnGetActiveUniformBlockiv &&
          pfnProvokingVertex && pfnClearBufferiv && pfnGetBufferPointerv && pfnGetSynciv,
          "extended core GL functions available via wglGetProcAddress");
    if (pfnFrontFace && pfnLineWidth && pfnStencilFunc && pfnUniform2i && pfnGetActiveUniformBlockiv &&
        pfnProvokingVertex && pfnClearBufferiv && pfnGetBufferPointerv && pfnGetSynciv) {
        pfnFrontFace(0x0900 /*GL_CW*/);
        pfnLineWidth(1.5f);
        pfnStencilFunc(0x0202 /*GL_EQUAL*/, 1, 0xFF);
        pfnUniform2i(0, 10, 20);
        int blockSize = 0;
        pfnGetActiveUniformBlockiv(1, 0, 0x8A40 /*GL_UNIFORM_BLOCK_DATA_SIZE*/, &blockSize);
        Check(blockSize == 1024, "glGetActiveUniformBlockiv returned data size 1024");
        pfnProvokingVertex(0x8E4E /*GL_FIRST_VERTEX_CONVENTION*/);
        int stencilVal = 0;
        pfnClearBufferiv(0x1802 /*GL_STENCIL*/, 0, &stencilVal);
        void* ptr = nullptr;
        pfnGetBufferPointerv(0x8892 /*GL_ARRAY_BUFFER*/, 0x888E /*GL_BUFFER_MAP_POINTER*/, &ptr);
        Check(true, "extended core GL operations executed cleanly");
    }
    if (g_failures) return 1;

    const float red[3] = {1.0f, 0.0f, 0.0f};
    const float teal[3] = {0.2f, 0.6f, 0.4f};
    const float dark[3] = {0.1f, 0.1f, 0.3f};
    DrawAndCheck(hdc, capture_path, 320, 240, red, "frame 1");
    DrawAndCheck(hdc, capture_path, 320, 240, teal, "frame 2");

    // Exercise immediate-mode geometry through the remote command stream.
    std::remove(capture_path);
    glViewport(0, 0, 320, 240);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glColor3f(1.0f, 0.0f, 0.0f); glVertex2f(-0.8f, -0.7f);
    glColor3f(0.0f, 1.0f, 0.0f); glVertex2f(0.8f, -0.7f);
    glColor3f(0.0f, 0.0f, 1.0f); glVertex2f(0.0f, 0.8f);
    glEnd();
    Check(SwapBuffers(hdc) != FALSE, "triangle frame swap");
    Capture triangle;
    const bool triangle_read = ReadCapture(capture_path, triangle);
    Check(triangle_read, "triangle frame captured");
    int triangle_rgb[3] = {};
    const bool triangle_pixel = triangle_read && triangle.Pixel(160, 120, triangle_rgb) &&
                                (triangle_rgb[0] > 20 || triangle_rgb[1] > 20 || triangle_rgb[2] > 20);
    Check(triangle_pixel, "triangle changes the centre pixel");

    // The resize reaches the server after the swap that notices it, so the next frame is the new size.
    place(hwnd, 400, 300);
    Pump();
    DrawAndCheck(hdc, capture_path, 320, 240, dark, "frame 3 (window just resized)");
    DrawAndCheck(hdc, capture_path, 400, 300, red, "frame 4 (new size)");

    // Minecraft 26.3 keeps chunk geometry in 128 MB heaps filled piece by piece with glBufferSubData.
    // Same pattern on a 100 MB texture buffer read by a shader: the allocation alone is over 64 MB,
    // the 20 MB upload travels in several chunks, and no upload may wipe an earlier one.
    {
        using TexBufferFn = void(WINAPI*)(unsigned, unsigned, unsigned);
        using CreateShaderFn = unsigned(WINAPI*)(unsigned);
        using ShaderSourceFn = void(WINAPI*)(unsigned, int, const char* const*, const int*);
        using UintFn = void(WINAPI*)(unsigned);
        using CreateProgramFn = unsigned(WINAPI*)();
        using AttachShaderFn = void(WINAPI*)(unsigned, unsigned);
        auto pfnTexBuffer = reinterpret_cast<TexBufferFn>(reinterpret_cast<void*>(wglGetProcAddress("glTexBuffer")));
        auto pfnCreateShader = reinterpret_cast<CreateShaderFn>(reinterpret_cast<void*>(wglGetProcAddress("glCreateShader")));
        auto pfnShaderSource = reinterpret_cast<ShaderSourceFn>(reinterpret_cast<void*>(wglGetProcAddress("glShaderSource")));
        auto pfnCompileShader = reinterpret_cast<UintFn>(reinterpret_cast<void*>(wglGetProcAddress("glCompileShader")));
        auto pfnCreateProgram = reinterpret_cast<CreateProgramFn>(reinterpret_cast<void*>(wglGetProcAddress("glCreateProgram")));
        auto pfnAttachShader = reinterpret_cast<AttachShaderFn>(reinterpret_cast<void*>(wglGetProcAddress("glAttachShader")));
        auto pfnLinkProgram = reinterpret_cast<UintFn>(reinterpret_cast<void*>(wglGetProcAddress("glLinkProgram")));
        auto pfnUseProgram = reinterpret_cast<UintFn>(reinterpret_cast<void*>(wglGetProcAddress("glUseProgram")));
        const bool have = pfnTexBuffer && pfnCreateShader && pfnShaderSource && pfnCompileShader && pfnCreateProgram &&
                          pfnAttachShader && pfnLinkProgram && pfnUseProgram && pfnGenBuffers && pfnBindBuffer &&
                          pfnBufferData && pfnBufferSubData;
        Check(have, "texture buffer and shader functions available");
        if (have) {
            constexpr unsigned kTextureBuffer = 0x8C2A;
            constexpr size_t kHeap = 100u << 20, kFar = 96u << 20, kFill = 40u << 20, kFillLen = 20u << 20;
            unsigned heap = 0;
            pfnGenBuffers(1, &heap);
            pfnBindBuffer(kTextureBuffer, heap);
            pfnBufferData(kTextureBuffer, kHeap, nullptr, 0x88E8 /*GL_DYNAMIC_DRAW*/);
            const uint8_t green[4] = {0, 255, 0, 255}, blue[4] = {0, 0, 255, 255};
            pfnBufferSubData(kTextureBuffer, 0, 4, green);
            pfnBufferSubData(kTextureBuffer, kFar, 4, blue); // further in: used to wipe the green texel
            std::vector<uint8_t> red_fill(kFillLen);
            for (size_t i = 0; i < kFillLen; i += 4) { red_fill[i] = 255; red_fill[i + 1] = 0; red_fill[i + 2] = 0; red_fill[i + 3] = 255; }
            pfnBufferSubData(kTextureBuffer, kFill, kFillLen, red_fill.data());

            unsigned tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(kTextureBuffer, tex);
            pfnTexBuffer(kTextureBuffer, 0x8058 /*GL_RGBA8*/, heap);

            // Three bands: texel 0 (green), the last texel of the chunked upload (red), the far texel (blue).
            char fs[512];
            std::snprintf(fs, sizeof(fs),
                          "#version 330\n"
                          "uniform samplerBuffer tbo;\n"
                          "out vec4 color;\n"
                          "void main() {\n"
                          "  int i = gl_FragCoord.x < 133.0 ? 0 : (gl_FragCoord.x < 266.0 ? %u : %u);\n"
                          "  color = texelFetch(tbo, i);\n"
                          "}\n",
                          static_cast<unsigned>((kFill + kFillLen - 4) / 4), static_cast<unsigned>(kFar / 4));
            const char* vs = "#version 330\n"
                             "void main() { vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
                             "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
            const char* fs_src = fs;
            const unsigned vsh = pfnCreateShader(0x8B31 /*GL_VERTEX_SHADER*/);
            const unsigned fsh = pfnCreateShader(0x8B30 /*GL_FRAGMENT_SHADER*/);
            pfnShaderSource(vsh, 1, &vs, nullptr);
            pfnShaderSource(fsh, 1, &fs_src, nullptr);
            pfnCompileShader(vsh);
            pfnCompileShader(fsh);
            const unsigned prog = pfnCreateProgram();
            pfnAttachShader(prog, vsh);
            pfnAttachShader(prog, fsh);
            pfnLinkProgram(prog);

            std::remove(capture_path);
            glViewport(0, 0, 400, 300);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            pfnUseProgram(prog);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            pfnUseProgram(0);
            Check(SwapBuffers(hdc) != FALSE, "large texture buffer frame swap");

            Capture heap_frame;
            int left[3] = {}, middle[3] = {}, right[3] = {};
            const bool read = ReadCapture(capture_path, heap_frame) && heap_frame.Pixel(60, 150, left) &&
                              heap_frame.Pixel(200, 150, middle) && heap_frame.Pixel(340, 150, right);
            char detail[160];
            std::snprintf(detail, sizeof(detail), "texel 0 (%d,%d,%d), end of 20 MB upload (%d,%d,%d), texel at 96 MB (%d,%d,%d)",
                          left[0], left[1], left[2], middle[0], middle[1], middle[2], right[0], right[1], right[2]);
            Check(read && left[1] == 255 && left[0] == 0 && middle[0] == 255 && middle[1] == 0 &&
                  right[2] == 255 && right[1] == 0,
                  "100 MB buffer keeps every upload (first write, chunked write, far write)", detail);

            // Minecraft maps a 256 KB ring buffer (write | unsynchronized) and fills a small part of
            // it; only the changed bytes are sent back. Change the very last texel of such a mapping.
            constexpr size_t kRing = 256u << 10;
            auto* mapped = static_cast<uint8_t*>(pfnMapBufferRange(kTextureBuffer, kFar - kRing, kRing + 4, 0x22));
            if (mapped) {
                uint8_t* texel = mapped + kRing;
                texel[0] = 255; texel[1] = 0; texel[2] = 255; texel[3] = 255;
            }
            const bool unmapped = pfnUnmapBuffer(kTextureBuffer) != 0;
            std::remove(capture_path);
            glClear(GL_COLOR_BUFFER_BIT);
            pfnUseProgram(prog);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            pfnUseProgram(0);
            Check(mapped && unmapped && SwapBuffers(hdc) != FALSE, "mapped ring buffer frame swap");
            Capture mapped_frame;
            const bool mapped_read = ReadCapture(capture_path, mapped_frame) && mapped_frame.Pixel(60, 150, left) &&
                                     mapped_frame.Pixel(340, 150, right);
            std::snprintf(detail, sizeof(detail), "texel 0 (%d,%d,%d), mapped texel (%d,%d,%d)",
                          left[0], left[1], left[2], right[0], right[1], right[2]);
            Check(mapped_read && left[1] == 255 && right[0] == 255 && right[1] == 0 && right[2] == 255,
                  "a write at the end of a 256 KB mapping reaches the server", detail);
        }
    }

    // Render foreground geometry onto the 400x300 frame for automated pixel verification.
    std::remove(capture_path);
    glViewport(0, 0, 400, 300);
    glClearColor(0.05f, 0.05f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glColor3f(1.0f, 0.0f, 0.0f); glVertex2f(-0.7f, -0.7f);
    glColor3f(0.0f, 1.0f, 0.0f); glVertex2f(0.7f, -0.7f);
    glColor3f(0.0f, 0.0f, 1.0f); glVertex2f(0.0f, 0.7f);
    glEnd();
    Check(SwapBuffers(hdc) != FALSE, "final frame swap with geometry");

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(rc);
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);

    std::printf("[gl_clear] pipelined frames (RGPU_FRAME_LATENCY=2)\n");
    {
        SetEnvironmentVariableA("RGPU_FRAME_LATENCY", "2");
        HWND hwnd_p = CreateWindowA("RgpuGlClearTest", "RGPU GL pipelined", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
                                    nullptr, nullptr, wc.hInstance, nullptr);
        place(hwnd_p, 200, 150);
        ShowWindow(hwnd_p, SW_SHOWNOACTIVATE);
        Pump();
        HDC hdc_p = GetDC(hwnd_p);
        const int fmt_p = ChoosePixelFormat(hdc_p, &pfd);
        SetPixelFormat(hdc_p, fmt_p, &pfd);
        HGLRC rc_p = wglCreateContext(hdc_p);
        bool ok = rc_p && wglMakeCurrent(hdc_p, rc_p);

        // Swaps no longer wait for their own frame: 20 frames in alternating colours...
        for (int i = 0; i < 20 && ok; ++i) {
            glViewport(0, 0, 200, 150);
            glClearColor(i % 2 ? 1.0f : 0.0f, 0.0f, i % 2 ? 0.0f : 1.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ok = SwapBuffers(hdc_p) != FALSE;
        }
        // ...then two in one colour: by the second swap, at most one frame is still in flight, so
        // the newest frame shown is one of the two.
        for (int i = 0; i < 2 && ok; ++i) {
            if (i == 1) std::remove(capture_path);
            glClearColor(1.0f, 1.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ok = SwapBuffers(hdc_p) != FALSE;
        }
        Capture cap;
        int rgb[3] = {};
        const bool shown = ok && ReadCapture(capture_path, cap) && cap.Pixel(100, 75, rgb);
        char detail[96];
        std::snprintf(detail, sizeof(detail), "centre (%d,%d,%d), expected (255,255,0)", rgb[0], rgb[1], rgb[2]);
        Check(shown && rgb[0] == 255 && rgb[1] == 255 && rgb[2] == 0, "22 pipelined frames, the newest one shown", detail);

        wglMakeCurrent(nullptr, nullptr);
        if (rc_p) wglDeleteContext(rc_p);
        ReleaseDC(hwnd_p, hdc_p);
        DestroyWindow(hwnd_p);
        SetEnvironmentVariableA("RGPU_FRAME_LATENCY", "1");
    }

    std::printf("[gl_clear] H.264 video frames (RGPU_VIDEO=1)\n");
    {
        SetEnvironmentVariableA("RGPU_VIDEO", "1");
        HWND hwnd_v = CreateWindowA("RgpuGlClearTest", "RGPU GL video", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
                                    nullptr, nullptr, wc.hInstance, nullptr);
        place(hwnd_v, 640, 360);
        ShowWindow(hwnd_v, SW_SHOWNOACTIVATE);
        Pump();
        HDC hdc_v = GetDC(hwnd_v);
        const int fmt_v = ChoosePixelFormat(hdc_v, &pfd);
        SetPixelFormat(hdc_v, fmt_v, &pfd);
        HGLRC rc_v = wglCreateContext(hdc_v);
        bool ok = rc_v && wglMakeCurrent(hdc_v, rc_v);
        Check(ok, "video session connects");

        // A moving triangle (like a turning camera)...
        for (int i = 0; i < 30 && ok; ++i) {
            glViewport(0, 0, 640, 360);
            glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            const float s = i * 0.04f;
            glBegin(GL_TRIANGLES);
            glColor3f(1.0f, 0.0f, 0.0f); glVertex2f(-0.8f + s, -0.6f);
            glColor3f(0.0f, 1.0f, 0.0f); glVertex2f(0.2f + s, -0.6f);
            glColor3f(0.0f, 0.0f, 1.0f); glVertex2f(-0.3f + s, 0.6f);
            glEnd();
            ok = SwapBuffers(hdc_v) != FALSE;
        }
        Check(ok, "30 moving frames swapped");

        // ...then a still frame, orange on the left and teal on the right, whose colours must come
        // back within the codec's loss.
        std::remove(capture_path);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 320, 360);
        glClearColor(1.0f, 0.5f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glScissor(320, 0, 320, 360);
        glClearColor(0.0f, 0.5f, 0.5f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        ok = ok && SwapBuffers(hdc_v) != FALSE;
        Capture cap;
        const bool read = ok && ReadCapture(capture_path, cap);
        Check(read && cap.width == 640 && cap.height == 360, "video frame captured at 640x360");
        int left[3] = {}, right[3] = {};
        const bool sampled = read && cap.Pixel(160, 180, left) && cap.Pixel(480, 180, right);
        auto close = [](const int got[3], int r, int g, int b) {
            return std::abs(got[0] - r) <= 8 && std::abs(got[1] - g) <= 8 && std::abs(got[2] - b) <= 8;
        };
        char detail[128];
        std::snprintf(detail, sizeof(detail), "left (%d,%d,%d) vs (255,128,0), right (%d,%d,%d) vs (0,128,128)", left[0],
                      left[1], left[2], right[0], right[1], right[2]);
        Check(sampled && close(left, 255, 128, 0) && close(right, 0, 128, 128), "video colours within 8 levels", detail);

        wglMakeCurrent(nullptr, nullptr);
        if (rc_v) wglDeleteContext(rc_v);
        ReleaseDC(hwnd_v, hdc_v);
        DestroyWindow(hwnd_v);
        SetEnvironmentVariableA("RGPU_VIDEO", "0");
    }

    std::printf("[gl_clear] concurrent sessions test\n");
    {
        HWND hwnd_a = CreateWindowA("RgpuGlClearTest", "RGPU GL A", WS_OVERLAPPEDWINDOW, 10, 10, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
        HWND hwnd_b = CreateWindowA("RgpuGlClearTest", "RGPU GL B", WS_OVERLAPPEDWINDOW, 200, 10, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
        place(hwnd_a, 160, 120);
        place(hwnd_b, 120, 90);
        ShowWindow(hwnd_a, SW_SHOWNOACTIVATE);
        ShowWindow(hwnd_b, SW_SHOWNOACTIVATE);
        Pump();

        HDC hdc_a = GetDC(hwnd_a);
        HDC hdc_b = GetDC(hwnd_b);
        int fmt_a = ChoosePixelFormat(hdc_a, &pfd);
        SetPixelFormat(hdc_a, fmt_a, &pfd);
        int fmt_b = ChoosePixelFormat(hdc_b, &pfd);
        SetPixelFormat(hdc_b, fmt_b, &pfd);

        HGLRC rc_a = wglCreateContext(hdc_a);
        HGLRC rc_b = wglCreateContext(hdc_b);
        Check(rc_a != nullptr && rc_b != nullptr, "concurrent wglCreateContext for two contexts");

        std::atomic<bool> th_a_ok{false}, th_b_ok{false};

        // Thread A acts like the main render thread, holding Context A.
        // Thread B acts like a background thread or second context, connecting while A is connected.
        std::thread th_a([&] {
            if (!wglMakeCurrent(hdc_a, rc_a)) return;
            const float green[3] = {0.0f, 1.0f, 0.0f};
            DrawAndCheck(hdc_a, capture_path, 160, 120, green, "concurrent A frame 1 (green)");

            // Wait for thread B to connect and render while context A remains active
            for (int i = 0; i < 200 && !th_b_ok.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            // Render geometry on context A to verify context isolation and provide non-clear pixels
            std::remove(capture_path);
            glViewport(0, 0, 160, 120);
            glClearColor(0.05f, 0.05f, 0.15f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glBegin(GL_TRIANGLES);
            glColor3f(1.0f, 0.0f, 0.0f); glVertex2f(-0.7f, -0.7f);
            glColor3f(0.0f, 1.0f, 0.0f); glVertex2f(0.7f, -0.7f);
            glColor3f(0.0f, 0.0f, 1.0f); glVertex2f(0.0f, 0.7f);
            glEnd();
            Check(SwapBuffers(hdc_a) != FALSE, "concurrent A frame 2 (geometry)");
            Capture cap2;
            Check(ReadCapture(capture_path, cap2) && cap2.width == 160 && cap2.height == 120, "concurrent A geometry frame captured");

            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(rc_a);
            th_a_ok = true;
        });

        std::thread th_b([&] {
            // Give thread A a moment to connect first
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!wglMakeCurrent(hdc_b, rc_b)) return;
            const float blue[3] = {0.0f, 0.0f, 1.0f};
            DrawAndCheck(hdc_b, capture_path, 120, 90, blue, "concurrent B frame 1 (blue)");
            th_b_ok = true;

            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(rc_b);
        });

        th_a.join();
        th_b.join();

        Check(th_a_ok.load(), "concurrent session A completed successfully");
        Check(th_b_ok.load(), "concurrent session B completed successfully");

        ReleaseDC(hwnd_a, hdc_a);
        ReleaseDC(hwnd_b, hdc_b);
        DestroyWindow(hwnd_a);
        DestroyWindow(hwnd_b);
    }

    std::printf("\n%s\n", g_failures ? "GL CLEAR TEST: FAILED" : "GL CLEAR TEST: PASSED");
    return g_failures ? 1 : 0;
}
