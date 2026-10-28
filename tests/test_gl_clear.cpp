// OpenGL foundation test: a plain Win32 + WGL program that clears its window.
// Run it from a folder holding the proxy opengl32.dll against a live server (RGPU_PORT).
// It presents through gdi32!SwapBuffers, exactly like a real application, and checks the pixels
// the proxy captured (RGPU_CAPTURE_FILE) after each swap, including across a window resize.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <GL/gl.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
    const char* capture_path = "gl_capture.bmp";
    SetEnvironmentVariableA("RGPU_CAPTURE_FILE", capture_path);

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

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(rc);
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);

    std::printf("\n%s\n", g_failures ? "GL CLEAR TEST: FAILED" : "GL CLEAR TEST: PASSED");
    return g_failures ? 1 : 0;
}
