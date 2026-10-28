#include "gl_host_device.h"

#include <GL/gl.h>

#include <cstdio>
#include <cstring>
#include <iostream>

namespace RemoteGPU {

namespace {

// Constants and entry points newer than OpenGL 1.1 (not in the system gl.h).
constexpr GLenum kFramebuffer = 0x8D40;
constexpr GLenum kReadFramebuffer = 0x8CA8;
constexpr GLenum kRenderbuffer = 0x8D41;
constexpr GLenum kColorAttachment0 = 0x8CE0;
constexpr GLenum kDepthStencilAttachment = 0x821A;
constexpr GLenum kRgba8 = 0x8058;
constexpr GLenum kDepth24Stencil8 = 0x88F0;
constexpr GLenum kFramebufferComplete = 0x8CD5;
constexpr GLenum kPixelPackBuffer = 0x88EB;
constexpr GLenum kPixelPackBufferBinding = 0x88ED;
constexpr GLenum kReadFramebufferBinding = 0x8CAA;
constexpr GLenum kPackRowLength = 0x0D02;
constexpr GLenum kPackAlignment = 0x0D05;

constexpr int kContextMajorVersion = 0x2091;
constexpr int kContextMinorVersion = 0x2092;
constexpr int kContextFlags = 0x2094;
constexpr int kContextProfileMask = 0x9126;

using GlGenFn = void(WINAPI*)(GLsizei, GLuint*);
using GlDeleteFn = void(WINAPI*)(GLsizei, const GLuint*);
using GlBindFn = void(WINAPI*)(GLenum, GLuint);
using GlRenderbufferStorageFn = void(WINAPI*)(GLenum, GLenum, GLsizei, GLsizei);
using GlFramebufferRenderbufferFn = void(WINAPI*)(GLenum, GLenum, GLenum, GLuint);
using GlCheckFramebufferStatusFn = GLenum(WINAPI*)(GLenum);
using CreateContextAttribsFn = HGLRC(WINAPI*)(HDC, HGLRC, const int*);

struct GlApi {
    GlGenFn genFramebuffers = nullptr;
    GlGenFn genRenderbuffers = nullptr;
    GlDeleteFn deleteFramebuffers = nullptr;
    GlDeleteFn deleteRenderbuffers = nullptr;
    GlBindFn bindFramebuffer = nullptr;
    GlBindFn bindRenderbuffer = nullptr;
    GlBindFn bindBuffer = nullptr;
    GlRenderbufferStorageFn renderbufferStorage = nullptr;
    GlFramebufferRenderbufferFn framebufferRenderbuffer = nullptr;
    GlCheckFramebufferStatusFn checkFramebufferStatus = nullptr;
};

GlApi g_api; // The context belongs to one thread at a time, and so do these pointers.

template <typename Fn>
bool Load(Fn& fn, const char* name) {
    fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(wglGetProcAddress(name)));
    return fn != nullptr;
}

bool LoadApi() {
    return Load(g_api.genFramebuffers, "glGenFramebuffers") &&
           Load(g_api.genRenderbuffers, "glGenRenderbuffers") &&
           Load(g_api.deleteFramebuffers, "glDeleteFramebuffers") &&
           Load(g_api.deleteRenderbuffers, "glDeleteRenderbuffers") &&
           Load(g_api.bindFramebuffer, "glBindFramebuffer") &&
           Load(g_api.bindRenderbuffer, "glBindRenderbuffer") &&
           Load(g_api.bindBuffer, "glBindBuffer") &&
           Load(g_api.renderbufferStorage, "glRenderbufferStorage") &&
           Load(g_api.framebufferRenderbuffer, "glFramebufferRenderbuffer") &&
           Load(g_api.checkFramebufferStatus, "glCheckFramebufferStatus");
}

const wchar_t kWindowClass[] = L"RemoteGpuGlHost";

bool EnsureWindowClass() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    registered = true;
    return true;
}

} // namespace

GlHostDevice::~GlHostDevice() { Destroy(); }

bool GlHostDevice::Create(uint32_t major, uint32_t minor, uint32_t profile_mask, uint32_t flags,
                          uint32_t width, uint32_t height) {
    Destroy();
    if (!EnsureWindowClass()) return false;

    m_hwnd = CreateWindowExW(0, kWindowClass, L"RemoteGPU GL host", WS_POPUP, 0, 0, 1, 1,
                             nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!m_hwnd) return false;
    m_hdc = GetDC(m_hwnd);
    if (!m_hdc) { Destroy(); return false; }

    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    const int format = ChoosePixelFormat(m_hdc, &pfd);
    if (format == 0 || !SetPixelFormat(m_hdc, format, &pfd)) { Destroy(); return false; }

    // A legacy context is needed first: wglCreateContextAttribsARB can only be fetched with one current.
    HGLRC legacy = wglCreateContext(m_hdc);
    if (!legacy || !wglMakeCurrent(m_hdc, legacy)) {
        if (legacy) wglDeleteContext(legacy);
        Destroy();
        return false;
    }

    HGLRC context = legacy;
    if (major != 0) {
        CreateContextAttribsFn create = nullptr;
        if (Load(create, "wglCreateContextAttribsARB")) {
            const int attribs[] = {
                kContextMajorVersion, static_cast<int>(major),
                kContextMinorVersion, static_cast<int>(minor),
                kContextFlags, static_cast<int>(flags),
                kContextProfileMask, static_cast<int>(profile_mask),
                0
            };
            HGLRC versioned = create(m_hdc, nullptr, attribs);
            if (versioned) {
                wglMakeCurrent(nullptr, nullptr);
                wglDeleteContext(legacy);
                if (!wglMakeCurrent(m_hdc, versioned)) {
                    wglDeleteContext(versioned);
                    Destroy();
                    return false;
                }
                context = versioned;
            } else {
                std::cerr << "[GlHost] Context " << major << "." << minor
                          << " refused by the driver; using a legacy context." << std::endl;
            }
        }
    }
    m_glrc = context;

    if (!LoadApi() || !CreateAttachments(width, height)) {
        std::cerr << "[GlHost] Offscreen framebuffer setup failed." << std::endl;
        Destroy();
        return false;
    }

    GLint gl_major = 0, gl_minor = 0;
    glGetIntegerv(0x821B, &gl_major); // GL_MAJOR_VERSION; stays 0 (with an error) before GL 3.0
    glGetIntegerv(0x821C, &gl_minor);
    glGetError();
    if (gl_major == 0) {
        const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        if (version) std::sscanf(version, "%d.%d", &gl_major, &gl_minor);
    }
    m_major = static_cast<uint32_t>(gl_major);
    m_minor = static_cast<uint32_t>(gl_minor);
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    m_renderer = renderer ? renderer : "unknown";
    return true;
}

void GlHostDevice::Destroy() {
    if (m_glrc) {
        wglMakeCurrent(m_hdc, m_glrc);
        DestroyAttachments();
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(m_glrc);
        m_glrc = nullptr;
    }
    if (m_hdc) { ReleaseDC(m_hwnd, m_hdc); m_hdc = nullptr; }
    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
    m_width = m_height = 0;
}

bool GlHostDevice::CreateAttachments(uint32_t width, uint32_t height) {
    g_api.genFramebuffers(1, &m_fbo);
    g_api.genRenderbuffers(1, &m_colorRb);
    g_api.genRenderbuffers(1, &m_depthRb);
    g_api.bindRenderbuffer(kRenderbuffer, m_colorRb);
    g_api.renderbufferStorage(kRenderbuffer, kRgba8, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    g_api.bindRenderbuffer(kRenderbuffer, m_depthRb);
    g_api.renderbufferStorage(kRenderbuffer, kDepth24Stencil8, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    g_api.bindRenderbuffer(kRenderbuffer, 0);
    g_api.bindFramebuffer(kFramebuffer, m_fbo);
    g_api.framebufferRenderbuffer(kFramebuffer, kColorAttachment0, kRenderbuffer, m_colorRb);
    g_api.framebufferRenderbuffer(kFramebuffer, kDepthStencilAttachment, kRenderbuffer, m_depthRb);
    if (g_api.checkFramebufferStatus(kFramebuffer) != kFramebufferComplete || glGetError() != GL_NO_ERROR) {
        return false;
    }
    m_width = width;
    m_height = height;
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glScissor(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    return true;
}

void GlHostDevice::DestroyAttachments() {
    if (!g_api.deleteFramebuffers) return;
    g_api.bindFramebuffer(kFramebuffer, 0);
    if (m_fbo) g_api.deleteFramebuffers(1, &m_fbo);
    if (m_colorRb) g_api.deleteRenderbuffers(1, &m_colorRb);
    if (m_depthRb) g_api.deleteRenderbuffers(1, &m_depthRb);
    m_fbo = m_colorRb = m_depthRb = 0;
}

bool GlHostDevice::Resize(uint32_t width, uint32_t height) {
    if (!m_glrc) return false;
    DestroyAttachments();
    if (!CreateAttachments(width, height)) return false;
    return true;
}

bool GlHostDevice::ReadbackFrame(std::vector<uint8_t>& out) {
    if (!m_glrc || m_width == 0 || m_height == 0) return false;

    GLint old_read_fbo = 0, old_pack_buffer = 0, old_alignment = 4, old_row_length = 0;
    glGetIntegerv(kReadFramebufferBinding, &old_read_fbo);
    glGetIntegerv(kPixelPackBufferBinding, &old_pack_buffer);
    glGetIntegerv(kPackAlignment, &old_alignment);
    glGetIntegerv(kPackRowLength, &old_row_length);

    g_api.bindFramebuffer(kReadFramebuffer, m_fbo);
    g_api.bindBuffer(kPixelPackBuffer, 0);
    glPixelStorei(kPackAlignment, 1);
    glPixelStorei(kPackRowLength, 0);

    const size_t row_bytes = static_cast<size_t>(m_width) * 4;
    m_bottomUp.resize(row_bytes * m_height);
    glReadPixels(0, 0, static_cast<GLsizei>(m_width), static_cast<GLsizei>(m_height),
                 GL_RGBA, GL_UNSIGNED_BYTE, m_bottomUp.data());
    const bool ok = glGetError() == GL_NO_ERROR;

    glPixelStorei(kPackAlignment, old_alignment);
    glPixelStorei(kPackRowLength, old_row_length);
    g_api.bindBuffer(kPixelPackBuffer, static_cast<GLuint>(old_pack_buffer));
    g_api.bindFramebuffer(kReadFramebuffer, static_cast<GLuint>(old_read_fbo));
    if (!ok) return false;

    out.resize(m_bottomUp.size());
    for (uint32_t y = 0; y < m_height; ++y) {
        std::memcpy(out.data() + static_cast<size_t>(y) * row_bytes,
                    m_bottomUp.data() + static_cast<size_t>(m_height - 1 - y) * row_bytes, row_bytes);
    }
    return true;
}

} // namespace RemoteGPU
