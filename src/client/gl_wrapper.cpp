// Drop-in opengl32.dll: every OpenGL context the application creates is rendered on the GPU
// server. Server address: RGPU_SERVER (default 127.0.0.1 or 192.168.1.17 for Java), port: RGPU_PORT.
//
// gdi32's SwapBuffers and pixel-format functions dispatch to the opengl32.dll loaded in the
// process, so exporting wglSwapBuffers and the wgl pixel-format calls is all that is needed.

#ifndef _GDI32_
#define _GDI32_
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "gl_session.h"

using namespace RemoteGPU;

namespace {

std::ofstream g_logFile;
std::mutex g_logMutex;

void Log(const char* fmt, ...) {
    static int s_verbose = -1;
    if (s_verbose == -1) {
        char env[16] = {};
        if (GetEnvironmentVariableA("RGPU_GL_VERBOSE", env, sizeof(env)) > 0) {
            s_verbose = (env[0] == '1') ? 1 : 0;
        } else {
            s_verbose = 0;
        }
    }
    if (!s_verbose) {
        if (std::strncmp(fmt, "[WGL", 4) != 0 &&
            std::strncmp(fmt, "[RemoteGPU", 10) != 0 &&
            std::strncmp(fmt, "[DllMain", 8) != 0 &&
            std::strncmp(fmt, "[ERROR", 6) != 0 &&
            std::strncmp(fmt, "[wglGetProcAddress -> REGISTER_STUB", 35) != 0) {
            return;
        }
    }

    std::lock_guard<std::mutex> lock(g_logMutex);
    static bool initialized = false;
    if (!initialized) {
        initialized = true;
        std::string path = "rgpu_gl_intercept.log";
        if (GetFileAttributesA("C:\\Users\\LHRIC\\curseforge\\minecraft\\Instances\\test") != INVALID_FILE_ATTRIBUTES) {
            path = "C:\\Users\\LHRIC\\curseforge\\minecraft\\Instances\\test\\rgpu_gl_intercept.log";
        }
        g_logFile.open(path, std::ios::out | std::ios::app);
        if (g_logFile.is_open()) {
            char exe[MAX_PATH] = {};
            GetModuleFileNameA(NULL, exe, sizeof(exe));
            g_logFile << "\n============================================================\n"
                      << "[RemoteGPU GL Intercept Log] Process: " << exe
                      << " (PID: " << GetCurrentProcessId() << ")\n"
                      << "============================================================\n";
            g_logFile.flush();
        }
    }
    if (!g_logFile.is_open()) return;
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    g_logFile << buffer << "\n";
    g_logFile.flush();
}

struct GlContext {
    GlCreateContextPayload request = {};
    HDC hdc = nullptr;
    HWND hwnd = nullptr;
    GlSession session;
    bool connected = false;
};

std::mutex g_mutex;
std::map<HGLRC, std::unique_ptr<GlContext>> g_contexts;
std::map<HDC, int> g_pixelFormats;
constexpr int kPixelFormat = 1;
thread_local GlContext* t_current = nullptr;
thread_local HDC t_currentDc = nullptr;

constexpr uint32_t kFallbackWidth = 854;
constexpr uint32_t kFallbackHeight = 480;

void WindowSize(HDC hdc, HWND& hwnd, uint32_t& width, uint32_t& height) {
    hwnd = WindowFromDC(hdc);
    RECT rc = {};
    width = height = 0;
    if (hwnd && GetClientRect(hwnd, &rc)) {
        width = static_cast<uint32_t>(rc.right - rc.left);
        height = static_cast<uint32_t>(rc.bottom - rc.top);
    }
    if (width == 0 || height == 0 || width > GL_MAX_FRAMEBUFFER_DIM || height > GL_MAX_FRAMEBUFFER_DIM) {
        width = kFallbackWidth;
        height = kFallbackHeight;
    }
}

bool DoSwapBuffers(HDC hdc) {
    GlContext* ctx = t_current;
    if (!ctx || t_currentDc != hdc) return false;
    return ctx->session.Swap(ctx->hwnd);
}

HGLRC CreateContextInternal(const GlCreateContextPayload& request) {
    auto ctx = std::make_unique<GlContext>();
    ctx->request = request;
    HGLRC handle = reinterpret_cast<HGLRC>(ctx.get());
    std::lock_guard<std::mutex> lock(g_mutex);
    g_contexts.emplace(handle, std::move(ctx));
    return handle;
}

GlContext* FindContext(HGLRC handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_contexts.find(handle);
    return it == g_contexts.end() ? nullptr : it->second.get();
}

// --- Dynamic Stub Infrastructure ----------------------------------------------------------------
constexpr size_t kMaxStubs = 512;
struct StubEntry {
    const char* name = nullptr;
    uint32_t callCount = 0;
};
std::mutex g_stubMutex;
StubEntry g_stubEntries[kMaxStubs];
size_t g_stubCount = 0;

void LogStubExecution(size_t index) {
    if (index >= kMaxStubs) return;
    const char* name = g_stubEntries[index].name;
    uint32_t count = ++g_stubEntries[index].callCount;
    if (count <= 2) {
        Log("[GL_STUB_CALLED #%u] %s", count, name ? name : "unknown");
    }
}

template <size_t N>
void* WINAPI GenericStub() {
    LogStubExecution(N);
    return nullptr;
}

using StubProc = void*(WINAPI*)();

template <size_t... Is>
constexpr std::array<StubProc, sizeof...(Is)> MakeStubTable(std::index_sequence<Is...>) {
    return { (&GenericStub<Is>)... };
}

static constexpr auto g_stubFunctions = MakeStubTable(std::make_index_sequence<kMaxStubs>{});

PROC FindOrCreateStub(const char* name) {
    std::lock_guard<std::mutex> lock(g_stubMutex);
    for (size_t i = 0; i < g_stubCount; ++i) {
        if (g_stubEntries[i].name && std::strcmp(g_stubEntries[i].name, name) == 0) {
            return reinterpret_cast<PROC>(g_stubFunctions[i]);
        }
    }
    if (g_stubCount < g_stubFunctions.size()) {
        size_t idx = g_stubCount++;
        char* nameCopy = _strdup(name);
        g_stubEntries[idx].name = nameCopy;
        Log("[wglGetProcAddress -> REGISTER_STUB #%zu] %s", idx, name);
        return reinterpret_cast<PROC>(g_stubFunctions[idx]);
    }
    Log("[wglGetProcAddress -> OUT_OF_STUBS] %s", name);
    return nullptr;
}

// Forward declarations
void* WINAPI CreateContextAttribsArb(HDC, HGLRC, const int* attribs);
const char* WINAPI GetExtensionsStringArb(HDC);
BOOL WINAPI SwapIntervalExt(int);
int WINAPI GetSwapIntervalExt();

BOOL WINAPI wglChoosePixelFormatARB(HDC hdc, const int* piAttribIList, const FLOAT* pfAttribFList,
                                    UINT nMaxFormats, int* piFormats, UINT* nNumFormats);
BOOL WINAPI wglGetPixelFormatAttribivARB(HDC hdc, int iPixelFormat, int iLayerPlane,
                                        UINT nAttributes, const int* piAttributes, int* piValues);
BOOL WINAPI wglGetPixelFormatAttribfvARB(HDC hdc, int iPixelFormat, int iLayerPlane,
                                        UINT nAttributes, const int* piAttributes, FLOAT* pfValues);

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        Log("[DllMain] opengl32.dll attached to process %lu", GetCurrentProcessId());
    }
    return TRUE;
}

extern "C" {

HGLRC WINAPI wglCreateContext(HDC) {
    Log("[WGL] wglCreateContext");
    GlCreateContextPayload request = {}; // legacy context
    return CreateContextInternal(request);
}

HGLRC WINAPI wglCreateLayerContext(HDC hdc, int) { return wglCreateContext(hdc); }

BOOL WINAPI wglDeleteContext(HGLRC handle) {
    Log("[WGL] wglDeleteContext (handle: %p)", handle);
    std::unique_ptr<GlContext> doomed;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_contexts.find(handle);
        if (it == g_contexts.end()) return FALSE;
        doomed = std::move(it->second);
        g_contexts.erase(it);
    }
    if (t_current == doomed.get()) { t_current = nullptr; t_currentDc = nullptr; }
    return TRUE;
}

BOOL WINAPI wglMakeCurrent(HDC hdc, HGLRC handle) {
    Log("[WGL] wglMakeCurrent(hdc: %p, handle: %p)", hdc, handle);
    if (!handle) {
        if (t_current) t_current->session.Flush();
        t_current = nullptr;
        t_currentDc = nullptr;
        return TRUE;
    }
    GlContext* ctx = FindContext(handle);
    if (!ctx || !hdc) return FALSE;

    if (!ctx->connected) {
        GlCreateContextPayload request = ctx->request;
        WindowSize(hdc, ctx->hwnd, request.width, request.height);
        Log("[WGL] Connecting session: %ux%u, requested version %u.%u",
            request.width, request.height, request.major, request.minor);
        if (!ctx->session.Connect(request)) {
            Log("[WGL Error] Connect failed!");
            return FALSE;
        }
        ctx->connected = true;
        ctx->hdc = hdc;
        Log("[WGL] Connected to remote host! Active renderer: %s", ctx->session.Renderer().c_str());
    }
    t_current = ctx;
    t_currentDc = hdc;
    return TRUE;
}

HGLRC WINAPI wglGetCurrentContext() {
    if (!t_current) return nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& entry : g_contexts) {
        if (entry.second.get() == t_current) return entry.first;
    }
    return nullptr;
}

HDC WINAPI wglGetCurrentDC() { return t_current ? t_currentDc : nullptr; }

BOOL WINAPI wglShareLists(HGLRC, HGLRC) { return TRUE; }

BOOL WINAPI wglSwapBuffers(HDC hdc) { return DoSwapBuffers(hdc) ? TRUE : FALSE; }
BOOL WINAPI wglSwapLayerBuffers(HDC hdc, UINT) { return wglSwapBuffers(hdc); }

int WINAPI wglChoosePixelFormat(HDC, const PIXELFORMATDESCRIPTOR*) { return kPixelFormat; }

BOOL WINAPI wglSetPixelFormat(HDC hdc, int format, const PIXELFORMATDESCRIPTOR*) {
    if (format != kPixelFormat) return FALSE;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pixelFormats[hdc] = format;
    return TRUE;
}

int WINAPI wglGetPixelFormat(HDC hdc) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_pixelFormats.find(hdc);
    return it == g_pixelFormats.end() ? 0 : it->second;
}

int WINAPI wglDescribePixelFormat(HDC, int format, UINT bytes, PIXELFORMATDESCRIPTOR* pfd) {
    if (pfd) {
        if (format != kPixelFormat || bytes < sizeof(PIXELFORMATDESCRIPTOR)) return 0;
        *pfd = {};
        pfd->nSize = sizeof(PIXELFORMATDESCRIPTOR);
        pfd->nVersion = 1;
        pfd->dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER | PFD_SUPPORT_COMPOSITION;
        pfd->iPixelType = PFD_TYPE_RGBA;
        pfd->cColorBits = 32;
        pfd->cRedBits = pfd->cGreenBits = pfd->cBlueBits = pfd->cAlphaBits = 8;
        pfd->cRedShift = 0;
        pfd->cGreenShift = 8;
        pfd->cBlueShift = 16;
        pfd->cAlphaShift = 24;
        pfd->cDepthBits = 24;
        pfd->cStencilBits = 8;
        pfd->iLayerType = PFD_MAIN_PLANE;
    }
    return 1;
}

// --- Extended Queries and Core GL ---------------------------------------------------------------

static const char* const kCoreExtensions[] = {
    "GL_ARB_vertex_buffer_object",
    "GL_ARB_vertex_array_object",
    "GL_ARB_vertex_shader",
    "GL_ARB_fragment_shader",
    "GL_ARB_shader_objects",
    "GL_ARB_shading_language_100",
    "GL_ARB_framebuffer_object",
    "GL_ARB_multitexture",
    "GL_ARB_texture_non_power_of_two",
    "GL_ARB_texture_compression",
    "GL_ARB_draw_buffers",
    "GL_ARB_draw_elements_base_vertex",
    "GL_ARB_draw_indirect",
    "GL_ARB_multi_draw_indirect",
    "GL_ARB_buffer_storage",
    "GL_ARB_clip_control",
    "GL_ARB_direct_state_access",
    "GL_ARB_texture_filter_anisotropic",
    "GL_ARB_base_instance",
    "GL_ARB_shader_draw_parameters",
    "GL_KHR_debug",
    "GL_EXT_texture_filter_anisotropic"
};
constexpr size_t kNumCoreExtensions = sizeof(kCoreExtensions) / sizeof(kCoreExtensions[0]);

const unsigned char* WINAPI glGetString(unsigned name) {
    Log("[GL] glGetString(0x%X)", name);
    switch (name) {
        case 0x1F00: // GL_VENDOR
            return reinterpret_cast<const unsigned char*>("NVIDIA Corporation");
        case 0x1F01: { // GL_RENDERER
            if (t_current && !t_current->session.Renderer().empty()) {
                return reinterpret_cast<const unsigned char*>(t_current->session.Renderer().c_str());
            }
            return reinterpret_cast<const unsigned char*>("NVIDIA GeForce RTX 4070 (RemoteGPU)");
        }
        case 0x1F02: // GL_VERSION
            return reinterpret_cast<const unsigned char*>("4.6.0 NVIDIA 560.94");
        case 0x8B8C: // GL_SHADING_LANGUAGE_VERSION
            return reinterpret_cast<const unsigned char*>("4.60 NVIDIA");
        case 0x1F03: // GL_EXTENSIONS (legacy)
            return reinterpret_cast<const unsigned char*>(
                "GL_ARB_vertex_buffer_object GL_ARB_vertex_array_object GL_ARB_vertex_shader "
                "GL_ARB_fragment_shader GL_ARB_shader_objects GL_ARB_framebuffer_object "
                "GL_ARB_multitexture GL_ARB_direct_state_access GL_KHR_debug");
        default:
            return reinterpret_cast<const unsigned char*>("");
    }
}

const unsigned char* WINAPI glGetStringi(unsigned name, unsigned index) {
    Log("[GL] glGetStringi(0x%X, %u)", name, index);
    if (name == 0x1F03 && index < kNumCoreExtensions) {
        return reinterpret_cast<const unsigned char*>(kCoreExtensions[index]);
    }
    return reinterpret_cast<const unsigned char*>("");
}

void WINAPI glGetIntegerv(unsigned pname, int* params) {
    if (!params) return;
    Log("[GL] glGetIntegerv(0x%X)", pname);
    switch (pname) {
        case 0x821B: *params = 4; break; // GL_MAJOR_VERSION
        case 0x821C: *params = 6; break; // GL_MINOR_VERSION
        case 0x821D: *params = static_cast<int>(kNumCoreExtensions); break; // GL_NUM_EXTENSIONS
        case 0x0D33: *params = 16384; break; // GL_MAX_TEXTURE_SIZE
        case 0x806C: *params = 2048; break;  // GL_MAX_3D_TEXTURE_SIZE
        case 0x851C: *params = 2048; break;  // GL_MAX_CUBE_MAP_TEXTURE_SIZE
        case 0x8872: *params = 16; break;    // GL_MAX_TEXTURE_IMAGE_UNITS
        case 0x8871: *params = 16; break;    // GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS
        case 0x8B4D: *params = 16; break;    // GL_MAX_TEXTURE_COORDS
        case 0x8B4A: *params = 1024; break;  // GL_MAX_VARYING_FLOATS
        case 0x8B4C: *params = 1024; break;  // GL_MAX_VERTEX_UNIFORM_COMPONENTS
        case 0x8DFB: *params = 1024; break;  // GL_MAX_FRAGMENT_UNIFORM_COMPONENTS
        case 0x8869: *params = 16; break;    // GL_MAX_VERTEX_ATTRIBS
        case 0x8824: *params = 8; break;     // GL_MAX_DRAW_BUFFERS
        case 0x8CDF: *params = 8; break;     // GL_MAX_COLOR_ATTACHMENTS
        case 0x8A2F: *params = 16; break;    // GL_MAX_UNIFORM_BUFFER_BINDINGS
        case 0x8A30: *params = 65536; break; // GL_MAX_UNIFORM_BLOCK_SIZE
        case 0x8A34: *params = 256; break;   // GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT
        case 0x84FF: *params = 16; break;    // GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
        case 0x9126: *params = 1; break;     // GL_CONTEXT_PROFILE_MASK (Core)
        case 0x0BA2: // GL_VIEWPORT
            params[0] = 0; params[1] = 0;
            params[2] = t_current ? t_current->session.Width() : kFallbackWidth;
            params[3] = t_current ? t_current->session.Height() : kFallbackHeight;
            break;
        default:
            *params = 0;
            break;
    }
}

void WINAPI glGetFloatv(unsigned pname, float* params) {
    if (!params) return;
    Log("[GL] glGetFloatv(0x%X)", pname);
    if (pname == 0x0BA2) { // GL_VIEWPORT
        params[0] = 0.0f; params[1] = 0.0f;
        params[2] = static_cast<float>(t_current ? t_current->session.Width() : kFallbackWidth);
        params[3] = static_cast<float>(t_current ? t_current->session.Height() : kFallbackHeight);
    } else if (pname == 0x84FF) { // GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
        *params = 16.0f;
    } else {
        *params = 0.0f;
    }
}

void WINAPI glGetDoublev(unsigned pname, double* params) {
    if (!params) return;
    Log("[GL] glGetDoublev(0x%X)", pname);
    if (pname == 0x0BA2) { // GL_VIEWPORT
        params[0] = 0.0; params[1] = 0.0;
        params[2] = static_cast<double>(t_current ? t_current->session.Width() : kFallbackWidth);
        params[3] = static_cast<double>(t_current ? t_current->session.Height() : kFallbackHeight);
    } else if (pname == 0x84FF) { // GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
        *params = 16.0;
    } else {
        *params = 0.0;
    }
}

// Capability queries used by LWJGL during the bootstrap context. These do not
// change rendering state, but must return well-formed values instead of the
// nullptr returned by the old generic stubs.
void WINAPI glGetInteger64v(unsigned pname, long long* params) {
    if (!params) return;
    Log("[GL] glGetInteger64v(0x%X)", pname);
    if (pname == 0x0BA2) {
        params[0] = 0; params[1] = 0;
        params[2] = t_current ? t_current->session.Width() : kFallbackWidth;
        params[3] = t_current ? t_current->session.Height() : kFallbackHeight;
    } else {
        *params = 0;
    }
}

void WINAPI glGetInternalformativ(unsigned, unsigned, unsigned pname, int count, int* params) {
    if (!params || count <= 0) return;
    Log("[GL] glGetInternalformativ(0x%X)", pname);
    for (int i = 0; i < count; ++i) params[i] = (pname == 0x8262) ? 1 : 0;
}

struct ProxyTextureLevel {
    int width = 0;
    int height = 0;
    int internalformat = 0;
};
static std::map<std::pair<unsigned, int>, ProxyTextureLevel> g_proxyTextures;
static std::mutex g_proxyMutex;

void WINAPI glGetTexLevelParameteriv(unsigned target, int level, unsigned pname, int* params) {
    if (!params) return;
    *params = 0;
    std::lock_guard<std::mutex> lock(g_proxyMutex);
    auto it = g_proxyTextures.find({target, level});
    if (it != g_proxyTextures.end()) {
        if (pname == 0x1000) {        // GL_TEXTURE_WIDTH
            *params = it->second.width;
        } else if (pname == 0x1001) { // GL_TEXTURE_HEIGHT
            *params = it->second.height;
        } else if (pname == 0x1003) { // GL_TEXTURE_INTERNAL_FORMAT
            *params = it->second.internalformat;
        }
    }
    Log("[GL] glGetTexLevelParameteriv(target: 0x%X, level: %d, pname: 0x%X) -> %d", target, level, pname, *params);
}

void WINAPI glPixelStorei(unsigned pname, int param) {
    Log("[GL] glPixelStorei(0x%X, %d)", pname, param);
}

void WINAPI glGetIntegeri_v(unsigned pname, unsigned index, int* params) {
    if (!params) return;
    Log("[GL] glGetIntegeri_v(0x%X, %u)", pname, index);
    glGetIntegerv(pname, params);
}

void WINAPI glGetInteger64i_v(unsigned target, unsigned index, long long* data) {
    if (data) *data = 0;
}

struct ClientBuffer {
    std::vector<uint8_t> storage;
};
static std::map<unsigned, ClientBuffer> g_clientBuffers;
static std::map<unsigned, unsigned> g_currentBoundBuffers;
static std::mutex g_bufferMutex;

inline bool IsServerBufferTarget(unsigned target) {
    return target == 0x8892 /*GL_ARRAY_BUFFER*/ ||
           target == 0x8893 /*GL_ELEMENT_ARRAY_BUFFER*/ ||
           target == 0x8A11 /*GL_UNIFORM_BUFFER*/;
}

void WINAPI glGetBufferParameteriv(unsigned target, unsigned pname, int* params) {
    if (!params) return;
    *params = 0;
    std::lock_guard<std::mutex> lock(g_bufferMutex);
    unsigned boundId = g_currentBoundBuffers[target];
    if (boundId != 0) {
        auto it = g_clientBuffers.find(boundId);
        if (it != g_clientBuffers.end()) {
            if (pname == 0x8764) { // GL_BUFFER_SIZE
                *params = static_cast<int>(it->second.storage.size());
            } else if (pname == 0x8882) { // GL_BUFFER_MAPPED
                *params = 0;
            }
        }
    }
}

void WINAPI glGetBufferParameteri64v(unsigned target, unsigned pname, long long* params) {
    if (!params) return;
    *params = 0;
    std::lock_guard<std::mutex> lock(g_bufferMutex);
    unsigned boundId = g_currentBoundBuffers[target];
    if (boundId != 0) {
        auto it = g_clientBuffers.find(boundId);
        if (it != g_clientBuffers.end()) {
            if (pname == 0x8764) { // GL_BUFFER_SIZE
                *params = static_cast<long long>(it->second.storage.size());
            }
        }
    }
}

void WINAPI glGetVertexAttribiv(unsigned, unsigned pname, int* params) {
    if (!params) return;
    Log("[GL] glGetVertexAttribiv(0x%X)", pname);
    *params = (pname == 0x8622) ? 1 : 0;
}

void WINAPI glGetUniformiv(unsigned, int, int* params) {
    if (params) *params = 0;
}

void WINAPI glGetUniformfv(unsigned, int, float* params) {
    if (params) *params = 0.0f;
}

void WINAPI glBindFramebuffer(unsigned target, unsigned framebuffer) {
    Log("[GL] glBindFramebuffer(0x%X, %u)", target, framebuffer);
    if (t_current) {
        GlBindFramebufferArgs args = {target, framebuffer};
        t_current->session.Append(GLF_BindFramebuffer, &args, sizeof(args));
    }
}

void WINAPI glBindRenderbuffer(unsigned target, unsigned renderbuffer) {
    Log("[GL] glBindRenderbuffer(0x%X, %u)", target, renderbuffer);
}

void WINAPI glFramebufferTexture2D(unsigned target, unsigned attachment, unsigned textarget, unsigned texture, int level) {
    Log("[GL] glFramebufferTexture2D(target: 0x%X, att: 0x%X, textarget: 0x%X, tex: %u, lvl: %d)",
        target, attachment, textarget, texture, level);
    if (t_current) {
        GlFramebufferTexture2DArgs args = {target, attachment, textarget, texture, level};
        t_current->session.Append(GLF_FramebufferTexture2D, &args, sizeof(args));
    }
}
void WINAPI glFramebufferRenderbuffer(unsigned, unsigned, unsigned, unsigned) {}
void WINAPI glFramebufferTextureLayer(unsigned target, unsigned attachment, unsigned texture, int level, int layer) {
    Log("[GL] glFramebufferTextureLayer(0x%X, 0x%X, %u, %d, %d)", target, attachment, texture, level, layer);
}
void WINAPI glFramebufferTexture(unsigned target, unsigned attachment, unsigned texture, int level) {
    Log("[GL] glFramebufferTexture(0x%X, 0x%X, %u, %d)", target, attachment, texture, level);
}
void WINAPI glFramebufferTexture1D(unsigned target, unsigned attachment, unsigned textarget, unsigned texture, int level) {
    Log("[GL] glFramebufferTexture1D(0x%X, 0x%X, 0x%X, %u, %d)", target, attachment, textarget, texture, level);
}
void WINAPI glFramebufferTexture3D(unsigned target, unsigned attachment, unsigned textarget, unsigned texture, int level, int zoffset) {
    Log("[GL] glFramebufferTexture3D(0x%X, 0x%X, 0x%X, %u, %d, %d)", target, attachment, textarget, texture, level, zoffset);
}
void WINAPI glGetFramebufferAttachmentParameteriv(unsigned, unsigned, unsigned, int* params) {
    if (params) *params = 0;
}
void WINAPI glGetRenderbufferParameteriv(unsigned, unsigned, int* params) {
    if (params) *params = 0;
}
void WINAPI glBindFragDataLocation(unsigned program, unsigned colorNumber, const char* name) {
    Log("[GL] glBindFragDataLocation(%u, %u, %s)", program, colorNumber, name ? name : "null");
}
int WINAPI glGetFragDataLocation(unsigned program, const char* name) {
    Log("[GL] glGetFragDataLocation(%u, %s)", program, name ? name : "null");
    return 0;
}
void WINAPI glGenFramebuffers(int count, unsigned* ids) {
    static unsigned next_id = 1;
    if (!ids || count < 0) return;
    for (int i = 0; i < count; ++i) ids[i] = next_id++;
    if (t_current && count > 0) {
        std::vector<uint8_t> payload(sizeof(GlGenArgs) + count * sizeof(uint32_t));
        GlGenArgs args = {static_cast<uint32_t>(count)};
        std::memcpy(payload.data(), &args, sizeof(args));
        std::memcpy(payload.data() + sizeof(args), ids, count * sizeof(uint32_t));
        t_current->session.Append(GLF_GenFramebuffers, payload.data(), static_cast<uint32_t>(payload.size()));
    }
}
void WINAPI glDeleteFramebuffers(int count, const unsigned* ids) {
    if (t_current && ids && count > 0) {
        std::vector<uint8_t> payload(sizeof(GlGenArgs) + count * sizeof(uint32_t));
        GlGenArgs args = {static_cast<uint32_t>(count)};
        std::memcpy(payload.data(), &args, sizeof(args));
        std::memcpy(payload.data() + sizeof(args), ids, count * sizeof(uint32_t));
        t_current->session.Append(GLF_DeleteFramebuffers, payload.data(), static_cast<uint32_t>(payload.size()));
    }
}

static unsigned g_nextRenderbufferId = 1;
void WINAPI glGenRenderbuffers(int count, unsigned* ids) {
    if (!ids || count < 0) return;
    for (int i = 0; i < count; ++i) ids[i] = g_nextRenderbufferId++;
}
void WINAPI glDeleteRenderbuffers(int, const unsigned*) {}
void WINAPI glRenderbufferStorage(unsigned target, unsigned internalformat, int width, int height) {
    Log("[GL] glRenderbufferStorage(0x%X, 0x%X, %dx%d)", target, internalformat, width, height);
}
void WINAPI glRenderbufferStorageMultisample(unsigned target, int samples, unsigned internalformat, int width, int height) {
    Log("[GL] glRenderbufferStorageMultisample(0x%X, %d, 0x%X, %dx%d)", target, samples, internalformat, width, height);
}
void WINAPI glBlitFramebuffer(int srcX0, int srcY0, int srcX1, int srcY1,
                             int dstX0, int dstY0, int dstX1, int dstY1,
                             unsigned mask, unsigned filter) {
    Log("[GL] glBlitFramebuffer(%d,%d,%d,%d -> %d,%d,%d,%d, mask: 0x%X, filter: 0x%X)",
        srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
    if (t_current) {
        GlBlitFramebufferArgs args = {srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter};
        t_current->session.Append(GLF_BlitFramebuffer, &args, sizeof(args));
    }
}
void WINAPI glDrawBuffers(int n, const unsigned* bufs) {
    Log("[GL] glDrawBuffers(n: %d)", n);
    if (t_current && bufs && n > 0) {
        std::vector<uint8_t> payload(sizeof(GlDrawBuffersArgs) + n * sizeof(uint32_t));
        GlDrawBuffersArgs args = {static_cast<uint32_t>(n)};
        std::memcpy(payload.data(), &args, sizeof(args));
        std::memcpy(payload.data() + sizeof(args), bufs, n * sizeof(uint32_t));
        t_current->session.Append(GLF_DrawBuffers, payload.data(), static_cast<uint32_t>(payload.size()));
    }
}

void WINAPI glGetBooleanv(unsigned pname, unsigned char* params) {
    if (!params) return;
    switch (pname) {
        case 0x0C10: *params = 1; break; // GL_DOUBLEBUFFER
        case 0x0C11: *params = 0; break; // GL_STEREO
        default:     *params = 0; break;
    }
}

void WINAPI glGetBooleani_v(unsigned target, unsigned index, unsigned char* data) {
    if (data) *data = 0;
}

unsigned char WINAPI glIsEnabledi(unsigned target, unsigned index) {
    return 0;
}

unsigned WINAPI glGetError() {
    Log("[GL] glGetError()");
    return 0; // GL_NO_ERROR
}

void WINAPI glGetAttachedShaders(unsigned, int maxCount, int* count, unsigned* shaders) {
    if (count) *count = 0;
    if (shaders && maxCount > 0) shaders[0] = 0;
}

void WINAPI glGetActiveAttrib(unsigned, unsigned, int maxLength, int* length, int* size,
                              unsigned* type, char* name) {
    if (length) *length = 0;
    if (size) *size = 0;
    if (type) *type = 0;
    if (name && maxLength > 0) name[0] = 0;
}

void WINAPI glGetActiveUniform(unsigned, unsigned, int maxLength, int* length, int* size,
                               unsigned* type, char* name) {
    if (length) *length = 0;
    if (size) *size = 0;
    if (type) *type = 0;
    if (name && maxLength > 0) name[0] = 0;
}

void WINAPI glGetShaderSource(unsigned, int maxLength, int* length, char* source) {
    if (length) *length = 0;
    if (source && maxLength > 0) source[0] = 0;
}

int WINAPI glGetUniformLocationARB(unsigned program, const char* name) {
    Log("[GL] glGetUniformLocationARB(%u, %s)", program, name ? name : "null");
    static unsigned next_location = 1;
    return static_cast<int>(next_location++);
}

void WINAPI glEnable(unsigned cap) {
    Log("[GL] glEnable(0x%X)", cap);
    if (t_current) {
        GlIdArgs args = {cap};
        t_current->session.Append(GLF_Enable, &args, sizeof(args));
    }
}
void WINAPI glDisable(unsigned cap) {
    Log("[GL] glDisable(0x%X)", cap);
    if (t_current) {
        GlIdArgs args = {cap};
        t_current->session.Append(GLF_Disable, &args, sizeof(args));
    }
}
unsigned char WINAPI glIsEnabled(unsigned) { return 1; }

inline size_t GetPixelSize(unsigned format, unsigned type) {
    size_t comp_size = 1;
    switch (type) {
        case 0x1400: /*GL_BYTE*/
        case 0x1401: /*GL_UNSIGNED_BYTE*/ comp_size = 1; break;
        case 0x1402: /*GL_SHORT*/
        case 0x1403: /*GL_UNSIGNED_SHORT*/
        case 0x140B: /*GL_HALF_FLOAT*/ comp_size = 2; break;
        case 0x1404: /*GL_INT*/
        case 0x1405: /*GL_UNSIGNED_INT*/
        case 0x1406: /*GL_FLOAT*/ comp_size = 4; break;
        case 0x8363: /*GL_UNSIGNED_SHORT_5_6_5*/
        case 0x8033: /*GL_UNSIGNED_SHORT_4_4_4_4*/
        case 0x8034: /*GL_UNSIGNED_SHORT_5_5_5_1*/ return 2;
        case 0x8032: /*GL_UNSIGNED_INT_8_8_8_8*/
        case 0x8367: /*GL_UNSIGNED_INT_8_8_8_8_REV*/
        case 0x8368: /*GL_UNSIGNED_INT_10_10_10_2*/
        case 0x84FA: /*GL_UNSIGNED_INT_24_8*/ return 4;
        default: comp_size = 1; break;
    }
    size_t num_comps = 4;
    switch (format) {
        case 0x1902: /*GL_DEPTH_COMPONENT*/
        case 0x1903: /*GL_RED*/
        case 0x1906: /*GL_ALPHA*/
        case 0x1909: /*GL_LUMINANCE*/
        case 0x8229: /*GL_RED_INTEGER*/ num_comps = 1; break;
        case 0x8227: /*GL_RG*/
        case 0x190A: /*GL_LUMINANCE_ALPHA*/
        case 0x822C: /*GL_RG_INTEGER*/ num_comps = 2; break;
        case 0x1907: /*GL_RGB*/
        case 0x80E0: /*GL_BGR*/
        case 0x8D98: /*GL_RGB_INTEGER*/ num_comps = 3; break;
        case 0x1908: /*GL_RGBA*/
        case 0x80E1: /*GL_BGRA*/
        case 0x8D99: /*GL_RGBA_INTEGER*/ num_comps = 4; break;
        default: num_comps = 4; break;
    }
    return comp_size * num_comps;
}

// --- Textures & VAO Stubs/Trackers --------------------------------------------------------------
static unsigned g_nextTextureId = 1;
static unsigned g_nextVaoId = 1;
static unsigned g_nextUniformLoc = 1;

void WINAPI glGenTextures(int count, unsigned* textures) {
    Log("[GL] glGenTextures(%d)", count);
    if (!textures || count < 0) return;
    for (int i = 0; i < count; ++i) textures[i] = g_nextTextureId++;
    if (t_current && count > 0) {
        std::vector<uint8_t> payload(sizeof(GlGenArgs) + count * sizeof(uint32_t));
        GlGenArgs args = {static_cast<uint32_t>(count)};
        std::memcpy(payload.data(), &args, sizeof(args));
        std::memcpy(payload.data() + sizeof(args), textures, count * sizeof(uint32_t));
        t_current->session.Append(GLF_GenTextures, payload.data(), static_cast<uint32_t>(payload.size()));
    }
}

void WINAPI glBindTexture(unsigned target, unsigned texture) {
    Log("[GL] glBindTexture(target: 0x%X, id: %u)", target, texture);
    if (t_current) {
        GlBindTextureArgs args = {target, texture};
        t_current->session.Append(GLF_BindTexture, &args, sizeof(args));
    }
}

void WINAPI glDeleteTextures(int count, const unsigned* textures) {
    Log("[GL] glDeleteTextures(%d)", count);
    if (t_current && textures && count > 0) {
        std::vector<uint8_t> payload(sizeof(GlGenArgs) + count * sizeof(uint32_t));
        GlGenArgs args = {static_cast<uint32_t>(count)};
        std::memcpy(payload.data(), &args, sizeof(args));
        std::memcpy(payload.data() + sizeof(args), textures, count * sizeof(uint32_t));
        t_current->session.Append(GLF_DeleteTextures, payload.data(), static_cast<uint32_t>(payload.size()));
    }
}

void WINAPI glTexParameteri(unsigned target, unsigned pname, int param) {
    Log("[GL] glTexParameteri(0x%X, 0x%X, %d)", target, pname, param);
    if (t_current) {
        GlTexParameteriArgs args = {target, pname, param};
        t_current->session.Append(GLF_TexParameteri, &args, sizeof(args));
    }
}

void WINAPI glTexParameterf(unsigned target, unsigned pname, float param) {
    Log("[GL] glTexParameterf(0x%X, 0x%X, %f)", target, pname, param);
    glTexParameteri(target, pname, static_cast<int>(param));
}

void WINAPI glTexParameteriv(unsigned target, unsigned pname, const int* params) {
    Log("[GL] glTexParameteriv(0x%X, 0x%X)", target, pname);
    if (params) glTexParameteri(target, pname, *params);
}

void WINAPI glTexParameterfv(unsigned target, unsigned pname, const float* params) {
    Log("[GL] glTexParameterfv(0x%X, 0x%X)", target, pname);
    if (params) glTexParameteri(target, pname, static_cast<int>(*params));
}

void WINAPI glGetTexParameteriv(unsigned target, unsigned pname, int* params) {
    if (params) *params = 0;
}

void WINAPI glGetTexParameterfv(unsigned target, unsigned pname, float* params) {
    if (params) *params = 0.0f;
}

void WINAPI glTexImage1D(unsigned target, int level, int internalformat, int width,
                         int border, unsigned format, unsigned type, const void* pixels) {
    Log("[GL] glTexImage1D(0x%X, level: %d, internal: %d, %d, pixels: %p)",
        target, level, internalformat, width, pixels);
    const bool isProxy = (target == 0x8063 /*GL_PROXY_TEXTURE_1D*/);
    const int maxDim = 16384;
    std::lock_guard<std::mutex> lock(g_proxyMutex);
    if (isProxy) {
        if (width > 0 && width <= maxDim) {
            g_proxyTextures[{target, level}] = {width, 1, internalformat};
        } else {
            g_proxyTextures[{target, level}] = {0, 0, 0};
        }
    } else {
        g_proxyTextures[{target, level}] = {width, 1, internalformat};
    }
}

void WINAPI glTexImage2D(unsigned target, int level, int internalformat, int width, int height,
                         int border, unsigned format, unsigned type, const void* pixels) {
    Log("[GL] glTexImage2D(0x%X, level: %d, internal: %d, %dx%d, pixels: %p)",
        target, level, internalformat, width, height, pixels);
    const bool isProxy = (target == 0x8063 /*GL_PROXY_TEXTURE_1D*/ ||
                          target == 0x8064 /*GL_PROXY_TEXTURE_2D*/ ||
                          target == 0x8070 /*GL_PROXY_TEXTURE_3D*/ ||
                          target == 0x851B /*GL_PROXY_TEXTURE_CUBE_MAP*/ ||
                          target == 0x8C1F /*GL_PROXY_TEXTURE_2D_ARRAY*/);
    const int maxDim = (target == 0x8070 || target == 0x851B) ? 2048 : 16384;
    {
        std::lock_guard<std::mutex> lock(g_proxyMutex);
        if (isProxy) {
            if (width > 0 && height > 0 && width <= maxDim && height <= maxDim) {
                g_proxyTextures[{target, level}] = {width, height, internalformat};
            } else {
                g_proxyTextures[{target, level}] = {0, 0, 0};
            }
        } else {
            g_proxyTextures[{target, level}] = {width, height, internalformat};
        }
    }
    if (isProxy || !t_current) return;
    const size_t byte_count = (pixels && width > 0 && height > 0) ? (static_cast<size_t>(width) * height * GetPixelSize(format, type)) : 0;
    GlTexImage2DArgs args = {target, level, internalformat, width, height, border, format, type, static_cast<uint32_t>(byte_count)};
    std::vector<uint8_t> payload(sizeof(args) + byte_count);
    std::memcpy(payload.data(), &args, sizeof(args));
    if (byte_count && pixels) std::memcpy(payload.data() + sizeof(args), pixels, byte_count);
    t_current->session.Append(GLF_TexImage2D, payload.data(), static_cast<uint32_t>(payload.size()));
}

void WINAPI glTexImage3D(unsigned target, int level, int internalformat, int width, int height, int depth,
                         int border, unsigned format, unsigned type, const void* pixels) {
    Log("[GL] glTexImage3D(0x%X, level: %d, internal: %d, %dx%dx%d, pixels: %p)",
        target, level, internalformat, width, height, depth, pixels);
    const bool isProxy = (target == 0x8070 /*GL_PROXY_TEXTURE_3D*/);
    const int maxDim = 2048;
    std::lock_guard<std::mutex> lock(g_proxyMutex);
    if (isProxy) {
        if (width > 0 && height > 0 && depth > 0 && width <= maxDim && height <= maxDim && depth <= maxDim) {
            g_proxyTextures[{target, level}] = {width, height, internalformat};
        } else {
            g_proxyTextures[{target, level}] = {0, 0, 0};
        }
    } else {
        g_proxyTextures[{target, level}] = {width, height, internalformat};
    }
}

void WINAPI glTexSubImage1D(unsigned target, int level, int xoffset, int width,
                            unsigned format, unsigned type, const void* pixels) {
    Log("[GL] glTexSubImage1D(0x%X, level: %d, xoff: %d, width: %d, format: 0x%X, type: 0x%X, pixels: %p)",
        target, level, xoffset, width, format, type, pixels);
}

void WINAPI glTexSubImage2D(unsigned target, int level, int xoffset, int yoffset, int width, int height,
                            unsigned format, unsigned type, const void* pixels) {
    Log("[GL] glTexSubImage2D(0x%X, level: %d, xoff: %d, yoff: %d, %dx%d, format: 0x%X, type: 0x%X, pixels: %p)",
        target, level, xoffset, yoffset, width, height, format, type, pixels);
    if (!t_current) return;
    const size_t byte_count = (pixels && width > 0 && height > 0) ? (static_cast<size_t>(width) * height * GetPixelSize(format, type)) : 0;
    GlTexSubImage2DArgs args = {target, level, xoffset, yoffset, width, height, format, type, static_cast<uint32_t>(byte_count)};
    std::vector<uint8_t> payload(sizeof(args) + byte_count);
    std::memcpy(payload.data(), &args, sizeof(args));
    if (byte_count && pixels) std::memcpy(payload.data() + sizeof(args), pixels, byte_count);
    t_current->session.Append(GLF_TexSubImage2D, payload.data(), static_cast<uint32_t>(payload.size()));
}

void WINAPI glTexSubImage3D(unsigned target, int level, int xoffset, int yoffset, int zoffset,
                            int width, int height, int depth, unsigned format, unsigned type, const void* pixels) {
    Log("[GL] glTexSubImage3D(0x%X, level: %d, xoff: %d, yoff: %d, zoff: %d, %dx%dx%d, format: 0x%X, type: 0x%X, pixels: %p)",
        target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels);
}

void WINAPI glCopyTexSubImage2D(unsigned target, int level, int xoffset, int yoffset,
                                int x, int y, int width, int height) {
    Log("[GL] glCopyTexSubImage2D(target: 0x%X, level: %d, xoff: %d, yoff: %d, %d, %d, %dx%d)",
        target, level, xoffset, yoffset, x, y, width, height);
}

void WINAPI glGenerateMipmap(unsigned target) {
    Log("[GL] glGenerateMipmap(target: 0x%X)", target);
    if (t_current) {
        GlIdArgs args = {target};
        t_current->session.Append(GLF_GenerateMipmap, &args, sizeof(args));
    }
}

void WINAPI glCopyTexImage1D(unsigned target, int level, unsigned internalformat, int x, int y, int width, int border) {
    Log("[GL] glCopyTexImage1D(0x%X, level: %d, internal: 0x%X, %d, %d, %d)", target, level, internalformat, x, y, width);
}

void WINAPI glCopyTexImage2D(unsigned target, int level, unsigned internalformat, int x, int y, int width, int height, int border) {
    Log("[GL] glCopyTexImage2D(0x%X, level: %d, internal: 0x%X, %d, %d, %dx%d)", target, level, internalformat, x, y, width, height);
}

void WINAPI glCopyTexSubImage1D(unsigned target, int level, int xoffset, int x, int y, int width) {
    Log("[GL] glCopyTexSubImage1D(0x%X, %d, %d, %d, %d, %d)", target, level, xoffset, x, y, width);
}

void WINAPI glCopyTexSubImage3D(unsigned target, int level, int xoffset, int yoffset, int zoffset, int x, int y, int width, int height) {
    Log("[GL] glCopyTexSubImage3D(0x%X, %d, %d, %d, %d, %d, %d, %dx%d)", target, level, xoffset, yoffset, zoffset, x, y, width, height);
}

void WINAPI glPixelStoref(unsigned pname, float param) {
    Log("[GL] glPixelStoref(0x%X, %f)", pname, param);
}

void WINAPI glGetTexLevelParameterfv(unsigned target, int level, unsigned pname, float* params) {
    if (!params) return;
    int iparam = 0;
    glGetTexLevelParameteriv(target, level, pname, &iparam);
    *params = static_cast<float>(iparam);
}

void WINAPI glTexParameterIiv(unsigned target, unsigned pname, const int* params) {
    Log("[GL] glTexParameterIiv(0x%X, 0x%X)", target, pname);
}

void WINAPI glTexParameterIuiv(unsigned target, unsigned pname, const unsigned* params) {
    Log("[GL] glTexParameterIuiv(0x%X, 0x%X)", target, pname);
}

void WINAPI glGetTexParameterIiv(unsigned, unsigned, int* params) {
    if (params) *params = 0;
}

void WINAPI glGetTexParameterIuiv(unsigned, unsigned, unsigned* params) {
    if (params) *params = 0;
}

void WINAPI glActiveTexture(unsigned texture) {
    Log("[GL] glActiveTexture(0x%X)", texture);
    if (t_current) {
        GlIdArgs args = {texture};
        t_current->session.Append(GLF_ActiveTexture, &args, sizeof(args));
    }
}

void WINAPI glClientActiveTexture(unsigned texture) {
    glActiveTexture(texture);
}

void WINAPI glGenVertexArrays(int count, unsigned* arrays) {
    Log("[GL] glGenVertexArrays(%d)", count);
    if (!arrays || count < 0) return;
    for (int i = 0; i < count; ++i) arrays[i] = g_nextVaoId++;
    if (t_current && count > 0) {
        std::vector<uint8_t> payload(sizeof(GlGenArgs) + count * sizeof(uint32_t));
        GlGenArgs args = {static_cast<uint32_t>(count)};
        std::memcpy(payload.data(), &args, sizeof(args));
        std::memcpy(payload.data() + sizeof(args), arrays, count * sizeof(uint32_t));
        t_current->session.Append(GLF_GenVertexArrays, payload.data(), static_cast<uint32_t>(payload.size()));
    }
}

void WINAPI glBindVertexArray(unsigned array) {
    Log("[GL] glBindVertexArray(%u)", array);
    if (t_current) {
        GlIdArgs args = {array};
        t_current->session.Append(GLF_BindVertexArray, &args, sizeof(args));
    }
}

void WINAPI glDeleteVertexArrays(int count, const unsigned* arrays) {
    Log("[GL] glDeleteVertexArrays(%d)", count);
    if (t_current && arrays && count > 0) {
        std::vector<uint8_t> payload(sizeof(GlGenArgs) + count * sizeof(uint32_t));
        GlGenArgs args = {static_cast<uint32_t>(count)};
        std::memcpy(payload.data(), &args, sizeof(args));
        std::memcpy(payload.data() + sizeof(args), arrays, count * sizeof(uint32_t));
        t_current->session.Append(GLF_DeleteVertexArrays, payload.data(), static_cast<uint32_t>(payload.size()));
    }
}

void WINAPI glVertexAttribPointer(unsigned index, int size, unsigned type, unsigned char normalized,
                                  int stride, const void* pointer) {
    Log("[GL] glVertexAttribPointer(idx: %u, size: %d, type: 0x%X, norm: %u, stride: %d, ptr: %p)",
        index, size, type, normalized, stride, pointer);
    if (t_current) {
        GlVertexAttribPointerArgs args = {
            index, size, type, normalized, {0, 0, 0}, stride,
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pointer))
        };
        t_current->session.Append(GLF_VertexAttribPointer, &args, sizeof(args));
    }
}

void WINAPI glVertexAttribIPointer(unsigned index, int size, unsigned type, int stride, const void* pointer) {
    Log("[GL] glVertexAttribIPointer(idx: %u, size: %d, type: 0x%X, stride: %d, ptr: %p)",
        index, size, type, stride, pointer);
    if (t_current) {
        GlVertexAttribPointerArgs args = {
            index, size, type, 0, {0, 0, 0}, stride,
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pointer))
        };
        t_current->session.Append(GLF_VertexAttribPointer, &args, sizeof(args));
    }
}

void WINAPI glVertexAttribDivisor(unsigned index, unsigned divisor) {
    Log("[GL] glVertexAttribDivisor(idx: %u, divisor: %u)", index, divisor);
    if (t_current) {
        GlVertexAttribDivisorArgs args = {index, divisor};
        t_current->session.Append(GLF_VertexAttribDivisor, &args, sizeof(args));
    }
}

void WINAPI glEnableVertexAttribArray(unsigned index) {
    Log("[GL] glEnableVertexAttribArray(%u)", index);
    if (t_current) {
        GlIdArgs args = {index};
        t_current->session.Append(GLF_EnableVertexAttribArray, &args, sizeof(args));
    }
}

void WINAPI glDisableVertexAttribArray(unsigned index) {
    Log("[GL] glDisableVertexAttribArray(%u)", index);
    if (t_current) {
        GlIdArgs args = {index};
        t_current->session.Append(GLF_DisableVertexAttribArray, &args, sizeof(args));
    }
}

int WINAPI glGetUniformLocation(unsigned program, const char* name) {
    Log("[GL] glGetUniformLocation(prog: %u, name: %s)", program, name ? name : "null");
    return static_cast<int>(g_nextUniformLoc++);
}

int WINAPI glGetAttribLocation(unsigned program, const char* name) {
    Log("[GL] glGetAttribLocation(prog: %u, name: %s)", program, name ? name : "null");
    return 0;
}

void WINAPI glUniform1i(int location, int v0) {
    Log("[GL] glUniform1i(loc: %d, val: %d)", location, v0);
}

void WINAPI glUniform1iv(int location, int count, const int* value) {
    Log("[GL] glUniform1iv(loc: %d, count: %d)", location, count);
}

void WINAPI glUniform1f(int location, float v0) {
    Log("[GL] glUniform1f(loc: %d, val: %f)", location, v0);
}

void WINAPI glUniform1fv(int location, int count, const float* value) {
    Log("[GL] glUniform1fv(loc: %d, count: %d)", location, count);
}

void WINAPI glUniform2f(int location, float v0, float v1) {
    Log("[GL] glUniform2f(loc: %d, %f, %f)", location, v0, v1);
}

void WINAPI glUniform2fv(int location, int count, const float* value) {
    Log("[GL] glUniform2fv(loc: %d, count: %d)", location, count);
}

void WINAPI glUniform3f(int location, float v0, float v1, float v2) {
    Log("[GL] glUniform3f(loc: %d, %f, %f, %f)", location, v0, v1, v2);
}

void WINAPI glUniform3fv(int location, int count, const float* value) {
    Log("[GL] glUniform3fv(loc: %d, count: %d)", location, count);
}

void WINAPI glUniform4f(int location, float v0, float v1, float v2, float v3) {
    Log("[GL] glUniform4f(loc: %d, %f, %f, %f, %f)", location, v0, v1, v2, v3);
}

void WINAPI glUniform4fv(int location, int count, const float* value) {
    Log("[GL] glUniform4fv(loc: %d, count: %d)", location, count);
}

void WINAPI glUniformMatrix2fv(int location, int count, unsigned char transpose, const float* value) {
    Log("[GL] glUniformMatrix2fv(loc: %d, count: %d, trans: %u)", location, count, transpose);
}

void WINAPI glUniformMatrix3fv(int location, int count, unsigned char transpose, const float* value) {
    Log("[GL] glUniformMatrix3fv(loc: %d, count: %d, trans: %u)", location, count, transpose);
}

void WINAPI glUniformMatrix4fv(int location, int count, unsigned char transpose, const float* value) {
    Log("[GL] glUniformMatrix4fv(loc: %d, count: %d, trans: %u)", location, count, transpose);
}

void WINAPI glUniform2i(int location, int v0, int v1) {
    Log("[GL] glUniform2i(loc: %d, %d, %d)", location, v0, v1);
}

void WINAPI glUniform3i(int location, int v0, int v1, int v2) {
    Log("[GL] glUniform3i(loc: %d, %d, %d, %d)", location, v0, v1, v2);
}

void WINAPI glUniform4i(int location, int v0, int v1, int v2, int v3) {
    Log("[GL] glUniform4i(loc: %d, %d, %d, %d, %d)", location, v0, v1, v2, v3);
}

void WINAPI glUniform2iv(int location, int count, const int* value) {
    Log("[GL] glUniform2iv(loc: %d, count: %d)", location, count);
}

void WINAPI glUniform3iv(int location, int count, const int* value) {
    Log("[GL] glUniform3iv(loc: %d, count: %d)", location, count);
}

void WINAPI glUniform4iv(int location, int count, const int* value) {
    Log("[GL] glUniform4iv(loc: %d, count: %d)", location, count);
}

void WINAPI glUniform1ui(int location, unsigned v0) {
    Log("[GL] glUniform1ui(loc: %d, %u)", location, v0);
}

void WINAPI glUniform2ui(int location, unsigned v0, unsigned v1) {
    Log("[GL] glUniform2ui(loc: %d, %u, %u)", location, v0, v1);
}

void WINAPI glUniform3ui(int location, unsigned v0, unsigned v1, unsigned v2) {
    Log("[GL] glUniform3ui(loc: %d, %u, %u, %u)", location, v0, v1, v2);
}

void WINAPI glUniform4ui(int location, unsigned v0, unsigned v1, unsigned v2, unsigned v3) {
    Log("[GL] glUniform4ui(loc: %d, %u, %u, %u, %u)", location, v0, v1, v2, v3);
}

void WINAPI glUniform1uiv(int location, int count, const unsigned* value) {
    Log("[GL] glUniform1uiv(loc: %d, count: %d)", location, count);
}

void WINAPI glUniform2uiv(int location, int count, const unsigned* value) {
    Log("[GL] glUniform2uiv(loc: %d, count: %d)", location, count);
}

void WINAPI glUniform3uiv(int location, int count, const unsigned* value) {
    Log("[GL] glUniform3uiv(loc: %d, count: %d)", location, count);
}

void WINAPI glUniform4uiv(int location, int count, const unsigned* value) {
    Log("[GL] glUniform4uiv(loc: %d, count: %d)", location, count);
}

void WINAPI glGetUniformuiv(unsigned, int, unsigned* params) {
    if (params) *params = 0;
}

static std::map<std::pair<unsigned, std::string>, unsigned> g_uniformBlockIndices;
static unsigned g_nextBlockIndex = 0;
static std::mutex g_blockMutex;

unsigned WINAPI glGetUniformBlockIndex(unsigned program, const char* name) {
    std::string sname = name ? name : "";
    std::lock_guard<std::mutex> lock(g_blockMutex);
    auto it = g_uniformBlockIndices.find({program, sname});
    if (it != g_uniformBlockIndices.end()) {
        return it->second;
    }
    unsigned idx = g_nextBlockIndex++;
    g_uniformBlockIndices[{program, sname}] = idx;
    Log("[GL] glGetUniformBlockIndex(prog: %u, name: %s) -> %u", program, sname.c_str(), idx);
    return idx;
}

void WINAPI glUniformBlockBinding(unsigned program, unsigned uniformBlockIndex, unsigned uniformBlockBinding) {
    Log("[GL] glUniformBlockBinding(prog: %u, index: %u, binding: %u)", program, uniformBlockIndex, uniformBlockBinding);
}

void WINAPI glGetActiveUniformBlockiv(unsigned program, unsigned uniformBlockIndex, unsigned pname, int* params) {
    if (!params) return;
    switch (pname) {
        case 0x8A40: *params = 1024; break; // GL_UNIFORM_BLOCK_DATA_SIZE
        case 0x8A3F: *params = 0; break;    // GL_UNIFORM_BLOCK_BINDING
        case 0x8A43: *params = 0; break;    // GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS
        default:     *params = 0; break;
    }
    Log("[GL] glGetActiveUniformBlockiv(prog: %u, idx: %u, pname: 0x%X) -> %d", program, uniformBlockIndex, pname, *params);
}

void WINAPI glGetActiveUniformBlockName(unsigned, unsigned, int bufSize, int* length, char* uniformBlockName) {
    if (length) *length = 0;
    if (uniformBlockName && bufSize > 0) uniformBlockName[0] = 0;
}

void WINAPI glGetActiveUniformsiv(unsigned, int uniformCount, const unsigned*, unsigned, int* params) {
    if (!params || uniformCount <= 0) return;
    for (int i = 0; i < uniformCount; ++i) params[i] = 0;
}

void WINAPI glGetActiveUniformName(unsigned, unsigned, int bufSize, int* length, char* uniformName) {
    if (length) *length = 0;
    if (uniformName && bufSize > 0) uniformName[0] = 0;
}

void WINAPI glGetUniformIndices(unsigned, int uniformCount, const char* const*, unsigned* uniformIndices) {
    if (!uniformIndices || uniformCount <= 0) return;
    for (int i = 0; i < uniformCount; ++i) uniformIndices[i] = static_cast<unsigned>(i);
}

void WINAPI glGetShaderiv(unsigned shader, unsigned pname, int* params) {
    Log("[GL] glGetShaderiv(shader: %u, pname: 0x%X)", shader, pname);
    if (!params) return;
    if (pname == 0x8B81) *params = 1; // GL_COMPILE_STATUS = TRUE
    else if (pname == 0x8B84) *params = 0; // GL_INFO_LOG_LENGTH = 0
    else *params = 1;
}

void WINAPI glGetProgramiv(unsigned program, unsigned pname, int* params) {
    Log("[GL] glGetProgramiv(program: %u, pname: 0x%X)", program, pname);
    if (!params) return;
    if (pname == 0x8B82) *params = 1; // GL_LINK_STATUS = TRUE
    else if (pname == 0x8B84) *params = 0; // GL_INFO_LOG_LENGTH = 0
    else *params = 1;
}

void WINAPI glGetShaderInfoLog(unsigned, int, int* length, char* infoLog) {
    if (length) *length = 0;
    if (infoLog) infoLog[0] = 0;
}

void WINAPI glGetProgramInfoLog(unsigned, int, int* length, char* infoLog) {
    if (length) *length = 0;
    if (infoLog) infoLog[0] = 0;
}

unsigned WINAPI glCheckFramebufferStatus(unsigned) {
    return 0x8CD5; // GL_FRAMEBUFFER_COMPLETE
}

void WINAPI glBlendFunc(unsigned sfactor, unsigned dfactor) {
    Log("[GL] glBlendFunc(0x%X, 0x%X)", sfactor, dfactor);
    if (t_current) {
        GlBlendFuncSeparateArgs args = {sfactor, dfactor, sfactor, dfactor};
        t_current->session.Append(GLF_BlendFuncSeparate, &args, sizeof(args));
    }
}

void WINAPI glBlendFuncSeparate(unsigned sfactorRGB, unsigned dfactorRGB, unsigned sfactorAlpha, unsigned dfactorAlpha) {
    Log("[GL] glBlendFuncSeparate(0x%X, 0x%X, 0x%X, 0x%X)", sfactorRGB, dfactorRGB, sfactorAlpha, dfactorAlpha);
    if (t_current) {
        GlBlendFuncSeparateArgs args = {sfactorRGB, dfactorRGB, sfactorAlpha, dfactorAlpha};
        t_current->session.Append(GLF_BlendFuncSeparate, &args, sizeof(args));
    }
}

void WINAPI glBlendEquation(unsigned mode) {
    Log("[GL] glBlendEquation(0x%X)", mode);
}

void WINAPI glBlendEquationSeparate(unsigned modeRGB, unsigned modeAlpha) {
    Log("[GL] glBlendEquationSeparate(0x%X, 0x%X)", modeRGB, modeAlpha);
}

void WINAPI glDepthFunc(unsigned func) {
    Log("[GL] glDepthFunc(0x%X)", func);
    if (t_current) {
        GlIdArgs args = {func};
        t_current->session.Append(GLF_DepthFunc, &args, sizeof(args));
    }
}
void WINAPI glDepthMask(unsigned char flag) {
    Log("[GL] glDepthMask(%u)", flag);
    if (t_current) {
        GlIdArgs args = {static_cast<uint32_t>(flag)};
        t_current->session.Append(GLF_DepthMask, &args, sizeof(args));
    }
}
void WINAPI glDepthRange(double nearVal, double farVal) { Log("[GL] glDepthRange(%f, %f)", nearVal, farVal); }
void WINAPI glDepthRangef(float nearVal, float farVal) { glDepthRange(static_cast<double>(nearVal), static_cast<double>(farVal)); }
void WINAPI glCullFace(unsigned mode) { Log("[GL] glCullFace(0x%X)", mode); }

void WINAPI glScissor(int x, int y, int width, int height) {
    Log("[GL] glScissor(%d, %d, %d, %d)", x, y, width, height);
    if (t_current) {
        GlViewportArgs args = {x, y, width, height};
        t_current->session.Append(GLF_Scissor, &args, sizeof(args));
    }
}

void WINAPI glColorMask(unsigned char red, unsigned char green, unsigned char blue, unsigned char alpha) {
    Log("[GL] glColorMask(%u, %u, %u, %u)", red, green, blue, alpha);
    if (t_current) {
        GlColorMaskArgs args = {red, green, blue, alpha};
        t_current->session.Append(GLF_ColorMask, &args, sizeof(args));
    }
}

void WINAPI glColorMaski(unsigned index, unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    Log("[GL] glColorMaski(idx: %u, %u, %u, %u, %u)", index, r, g, b, a);
}

void WINAPI glClearDepth(double depth) {
    Log("[GL] glClearDepth(%f)", depth);
}

void WINAPI glClearDepthf(float depth) {
    glClearDepth(static_cast<double>(depth));
}

void WINAPI glClearStencil(int s) {
    Log("[GL] glClearStencil(%d)", s);
}

void WINAPI glPolygonMode(unsigned face, unsigned mode) {
    Log("[GL] glPolygonMode(0x%X, 0x%X)", face, mode);
    if (t_current) {
        GlPolygonModeArgs args = {face, mode};
        t_current->session.Append(GLF_PolygonMode, &args, sizeof(args));
    }
}

void WINAPI glPolygonOffset(float factor, float units) {
    Log("[GL] glPolygonOffset(%f, %f)", factor, units);
}

void WINAPI glFrontFace(unsigned mode) {
    Log("[GL] glFrontFace(0x%X)", mode);
}

void WINAPI glLineWidth(float width) {
    Log("[GL] glLineWidth(%f)", width);
}

void WINAPI glPointSize(float size) {
    Log("[GL] glPointSize(%f)", size);
}

void WINAPI glStencilFunc(unsigned func, int ref, unsigned mask) {
    Log("[GL] glStencilFunc(0x%X, %d, 0x%X)", func, ref, mask);
}

void WINAPI glStencilMask(unsigned mask) {
    Log("[GL] glStencilMask(0x%X)", mask);
}

void WINAPI glStencilOp(unsigned fail, unsigned zfail, unsigned zpass) {
    Log("[GL] glStencilOp(0x%X, 0x%X, 0x%X)", fail, zfail, zpass);
}

void WINAPI glStencilFuncSeparate(unsigned face, unsigned func, int ref, unsigned mask) {
    Log("[GL] glStencilFuncSeparate(0x%X, 0x%X, %d, 0x%X)", face, func, ref, mask);
}

void WINAPI glStencilMaskSeparate(unsigned face, unsigned mask) {
    Log("[GL] glStencilMaskSeparate(0x%X, 0x%X)", face, mask);
}

void WINAPI glStencilOpSeparate(unsigned face, unsigned fail, unsigned zfail, unsigned zpass) {
    Log("[GL] glStencilOpSeparate(0x%X, 0x%X, 0x%X, 0x%X)", face, fail, zfail, zpass);
}

void WINAPI glBlendColor(float red, float green, float blue, float alpha) {
    Log("[GL] glBlendColor(%f, %f, %f, %f)", red, green, blue, alpha);
}

void WINAPI glClampColor(unsigned target, unsigned clamp) {
    Log("[GL] glClampColor(0x%X, 0x%X)", target, clamp);
}

void WINAPI glProvokingVertex(unsigned mode) {
    Log("[GL] glProvokingVertex(0x%X)", mode);
}

// --- Samplers -----------------------------------------------------------------------------------
static unsigned g_nextSamplerId = 1;

void WINAPI glGenSamplers(int count, unsigned* samplers) {
    Log("[GL] glGenSamplers(%d)", count);
    if (!samplers || count < 0) return;
    for (int i = 0; i < count; ++i) {
        samplers[i] = g_nextSamplerId++;
    }
    if (t_current && count > 0) {
        std::vector<uint8_t> payload(sizeof(GlGenArgs) + count * sizeof(uint32_t));
        GlGenArgs args = {static_cast<uint32_t>(count)};
        std::memcpy(payload.data(), &args, sizeof(args));
        std::memcpy(payload.data() + sizeof(args), samplers, count * sizeof(uint32_t));
        t_current->session.Append(GLF_GenSamplers, payload.data(), static_cast<uint32_t>(payload.size()));
    }
}

void WINAPI glDeleteSamplers(int count, const unsigned* samplers) {
    Log("[GL] glDeleteSamplers(%d)", count);
    if (t_current && samplers && count > 0) {
        std::vector<uint8_t> payload(sizeof(GlGenArgs) + count * sizeof(uint32_t));
        GlGenArgs args = {static_cast<uint32_t>(count)};
        std::memcpy(payload.data(), &args, sizeof(args));
        std::memcpy(payload.data() + sizeof(args), samplers, count * sizeof(uint32_t));
        t_current->session.Append(GLF_DeleteSamplers, payload.data(), static_cast<uint32_t>(payload.size()));
    }
}

unsigned char WINAPI glIsSampler(unsigned id) {
    return id > 0 ? 1 : 0;
}

void WINAPI glBindSampler(unsigned unit, unsigned sampler) {
    Log("[GL] glBindSampler(unit: %u, sampler: %u)", unit, sampler);
    if (t_current) {
        GlBindSamplerArgs args = {unit, sampler};
        t_current->session.Append(GLF_BindSampler, &args, sizeof(args));
    }
}

void WINAPI glSamplerParameteri(unsigned sampler, unsigned pname, int param) {
    Log("[GL] glSamplerParameteri(sampler: %u, pname: 0x%X, param: %d)", sampler, pname, param);
    if (t_current) {
        GlSamplerParameteriArgs args = {sampler, pname, param};
        t_current->session.Append(GLF_SamplerParameteri, &args, sizeof(args));
    }
}

void WINAPI glSamplerParameterf(unsigned sampler, unsigned pname, float param) {
    Log("[GL] glSamplerParameterf(sampler: %u, pname: 0x%X, param: %f)", sampler, pname, param);
    glSamplerParameteri(sampler, pname, static_cast<int>(param));
}

void WINAPI glSamplerParameteriv(unsigned sampler, unsigned pname, const int* params) {
    Log("[GL] glSamplerParameteriv(sampler: %u, pname: 0x%X)", sampler, pname);
    if (params) glSamplerParameteri(sampler, pname, *params);
}

void WINAPI glSamplerParameterfv(unsigned sampler, unsigned pname, const float* params) {
    Log("[GL] glSamplerParameterfv(sampler: %u, pname: 0x%X)", sampler, pname);
    if (params) glSamplerParameteri(sampler, pname, static_cast<int>(*params));
}

void WINAPI glGetSamplerParameteriv(unsigned, unsigned, int* params) {
    if (params) *params = 0;
}

void WINAPI glGetSamplerParameterfv(unsigned, unsigned, float* params) {
    if (params) *params = 0.0f;
}

void WINAPI glSamplerParameterIiv(unsigned sampler, unsigned pname, const int* params) {
    Log("[GL] glSamplerParameterIiv(%u, 0x%X)", sampler, pname);
}

void WINAPI glSamplerParameterIuiv(unsigned sampler, unsigned pname, const unsigned* params) {
    Log("[GL] glSamplerParameterIuiv(%u, 0x%X)", sampler, pname);
}

void WINAPI glGetSamplerParameterIiv(unsigned sampler, unsigned pname, int* params) {
    if (params) *params = 0;
}

void WINAPI glGetSamplerParameterIuiv(unsigned sampler, unsigned pname, unsigned* params) {
    if (params) *params = 0;
}

static unsigned g_nextSyncId = 1;
static std::mutex g_syncMutex;

void* WINAPI glFenceSync(unsigned condition, unsigned flags) {
    std::lock_guard<std::mutex> lock(g_syncMutex);
    unsigned id = g_nextSyncId++;
    Log("[GL] glFenceSync(cond: 0x%X, flags: 0x%X) -> %u", condition, flags, id);
    return reinterpret_cast<void*>(static_cast<uintptr_t>(id));
}

unsigned WINAPI glClientWaitSync(void* sync, unsigned flags, unsigned long long timeout) {
    Log("[GL] glClientWaitSync(sync: %p, flags: 0x%X, timeout: %llu)", sync, flags, timeout);
    return 0x911A; // GL_ALREADY_SIGNALED
}

void WINAPI glWaitSync(void* sync, unsigned flags, unsigned long long timeout) {
    Log("[GL] glWaitSync(sync: %p, flags: 0x%X, timeout: %llu)", sync, flags, timeout);
}

void WINAPI glDeleteSync(void* sync) {
    Log("[GL] glDeleteSync(sync: %p)", sync);
}

unsigned char WINAPI glIsSync(void* sync) {
    return sync != nullptr ? 1 : 0;
}

void WINAPI glGetSynciv(void* sync, unsigned pname, int bufSize, int* length, int* values) {
    if (length) *length = 1;
    if (values && bufSize > 0) {
        if (pname == 0x9114 /*GL_SYNC_STATUS*/) *values = 0x9119 /*GL_SIGNALED*/;
        else if (pname == 0x9113 /*GL_SYNC_CONDITION*/) *values = 0x9118 /*GL_SYNC_GPU_COMMANDS_COMPLETE*/;
        else if (pname == 0x9115 /*GL_SYNC_FLAGS*/) *values = 0;
        else *values = 0;
    }
}

// --- Existing relayed OpenGL functions ---------------------------------------------------------

void WINAPI glClear(unsigned mask) {
    if (!t_current) return;
    const GlClearArgs args = {mask};
    t_current->session.Append(GLF_Clear, &args, sizeof(args));
}

void WINAPI glClearColor(float r, float g, float b, float a) {
    if (!t_current) return;
    const GlClearColorArgs args = {r, g, b, a};
    t_current->session.Append(GLF_ClearColor, &args, sizeof(args));
}

void WINAPI glViewport(int x, int y, int width, int height) {
    if (!t_current) return;
    const GlViewportArgs args = {x, y, width, height};
    t_current->session.Append(GLF_Viewport, &args, sizeof(args));
}

void WINAPI glFlush() {
    if (!t_current) return;
    t_current->session.Append(GLF_Flush, nullptr, 0);
    t_current->session.Flush();
}

void WINAPI glFinish() {
    if (!t_current) return;
    t_current->session.Append(GLF_Finish, nullptr, 0);
    t_current->session.Flush();
}

void WINAPI glBegin(unsigned mode) {
    if (!t_current) return;
    const GlBeginArgs args = {mode};
    t_current->session.Append(GLF_Begin, &args, sizeof(args));
}

void WINAPI glEnd() {
    if (!t_current) return;
    t_current->session.Append(GLF_End, nullptr, 0);
}

void WINAPI glColor3f(float r, float g, float b) {
    if (!t_current) return;
    const GlColor3fArgs args = {r, g, b};
    t_current->session.Append(GLF_Color3f, &args, sizeof(args));
}

void WINAPI glVertex2f(float x, float y) {
    if (!t_current) return;
    const GlVertex2fArgs args = {x, y};
    t_current->session.Append(GLF_Vertex2f, &args, sizeof(args));
}

void WINAPI glGenBuffers(int count, unsigned* buffers) {
    if (!t_current || !buffers || count < 0 || count > 4096) return;
    static unsigned next_id = 1;
    for (int i = 0; i < count; ++i) {
        buffers[i] = next_id++;
        const GlIdArgs args = {buffers[i]};
        t_current->session.Append(GLF_GenBuffer, &args, sizeof(args));
    }
}

void WINAPI glBindBuffer(unsigned target, unsigned buffer) {
    {
        std::lock_guard<std::mutex> lock(g_bufferMutex);
        g_currentBoundBuffers[target] = buffer;
    }
    if (!t_current) return;
    if (IsServerBufferTarget(target)) {
        const GlBindBufferArgs args = {target, buffer};
        t_current->session.Append(GLF_BindBuffer, &args, sizeof(args));
    }
}

void WINAPI glBufferData(unsigned target, ptrdiff_t size, const void* data, unsigned usage) {
    {
        std::lock_guard<std::mutex> lock(g_bufferMutex);
        unsigned boundId = g_currentBoundBuffers[target];
        if (boundId != 0 && size >= 0) {
            auto& buf = g_clientBuffers[boundId];
            buf.storage.resize(static_cast<size_t>(size));
            if (data && size > 0) {
                std::memcpy(buf.storage.data(), data, static_cast<size_t>(size));
            }
        }
    }
    if (!t_current || size < 0 || static_cast<uint64_t>(size) > 64 * 1024 * 1024) return;
    if (IsServerBufferTarget(target)) {
        const GlBufferDataArgs args = {target, usage, static_cast<uint64_t>(size)};
        std::vector<uint8_t> payload(sizeof(args) + static_cast<size_t>(size));
        std::memcpy(payload.data(), &args, sizeof(args));
        if (data && size) std::memcpy(payload.data() + sizeof(args), data, static_cast<size_t>(size));
        t_current->session.Append(GLF_BufferData, payload.data(), static_cast<uint32_t>(payload.size()));
    }
}

void WINAPI glBufferSubData(unsigned target, ptrdiff_t offset, ptrdiff_t size, const void* data) {
    Log("[GL] glBufferSubData(target: 0x%X, offset: %td, size: %td, data: %p)", target, offset, size, data);
    {
        std::lock_guard<std::mutex> lock(g_bufferMutex);
        unsigned boundId = g_currentBoundBuffers[target];
        if (boundId != 0 && data && size > 0 && offset >= 0) {
            auto& buf = g_clientBuffers[boundId];
            if (buf.storage.size() < static_cast<size_t>(offset + size)) {
                buf.storage.resize(static_cast<size_t>(offset + size));
            }
            std::memcpy(buf.storage.data() + offset, data, static_cast<size_t>(size));
        }
    }
    if (t_current && data && size > 0 && offset >= 0 && size <= 64 * 1024 * 1024) {
        const GlBufferSubDataArgs args = {target, 0, static_cast<uint64_t>(offset), static_cast<uint64_t>(size)};
        std::vector<uint8_t> payload(sizeof(args) + static_cast<size_t>(size));
        std::memcpy(payload.data(), &args, sizeof(args));
        std::memcpy(payload.data() + sizeof(args), data, static_cast<size_t>(size));
        t_current->session.Append(GLF_BufferSubData, payload.data(), static_cast<uint32_t>(payload.size()));
    }
}

void WINAPI glGetBufferSubData(unsigned target, ptrdiff_t offset, ptrdiff_t size, void* data) {
    Log("[GL] glGetBufferSubData(target: 0x%X, offset: %td, size: %td, data: %p)", target, offset, size, data);
    std::lock_guard<std::mutex> lock(g_bufferMutex);
    unsigned boundId = g_currentBoundBuffers[target];
    if (boundId != 0 && data && size > 0 && offset >= 0) {
        auto it = g_clientBuffers.find(boundId);
        if (it != g_clientBuffers.end() && it->second.storage.size() >= static_cast<size_t>(offset + size)) {
            std::memcpy(data, it->second.storage.data() + offset, static_cast<size_t>(size));
            return;
        }
    }
    if (data && size > 0) std::memset(data, 0, static_cast<size_t>(size));
}

void WINAPI glDeleteBuffers(int count, const unsigned* buffers) {
    if (count > 0 && buffers) {
        std::lock_guard<std::mutex> lock(g_bufferMutex);
        for (int i = 0; i < count; ++i) {
            g_clientBuffers.erase(buffers[i]);
            for (auto& pair : g_currentBoundBuffers) {
                if (pair.second == buffers[i]) pair.second = 0;
            }
        }
    }
    if (!t_current || !buffers || count < 0 || count > 4096) return;
    for (int i = 0; i < count; ++i) {
        const GlIdArgs args = {buffers[i]};
        t_current->session.Append(GLF_DeleteBuffer, &args, sizeof(args));
    }
}

void* WINAPI glMapBufferRange(unsigned target, ptrdiff_t offset, ptrdiff_t length, unsigned access) {
    Log("[GL] glMapBufferRange(target: 0x%X, offset: %td, len: %td, access: 0x%X)", target, offset, length, access);
    std::lock_guard<std::mutex> lock(g_bufferMutex);
    unsigned boundId = g_currentBoundBuffers[target];
    if (boundId != 0) {
        auto& buf = g_clientBuffers[boundId];
        if (offset < 0 || length < 0) return nullptr;
        if (buf.storage.size() < static_cast<size_t>(offset + length)) {
            buf.storage.resize(static_cast<size_t>(offset + length));
        }
        return buf.storage.data() + offset;
    }
    static std::vector<uint8_t> fallback(16 * 1024 * 1024);
    if (offset >= 0 && length >= 0 && static_cast<size_t>(offset + length) <= fallback.size()) {
        return fallback.data() + offset;
    }
    return fallback.data();
}

unsigned char WINAPI glUnmapBuffer(unsigned target) {
    Log("[GL] glUnmapBuffer(target: 0x%X)", target);
    std::vector<uint8_t> fullBufferCopy;
    {
        std::lock_guard<std::mutex> lock(g_bufferMutex);
        unsigned boundId = g_currentBoundBuffers[target];
        if (boundId != 0 && IsServerBufferTarget(target)) {
            auto it = g_clientBuffers.find(boundId);
            if (it != g_clientBuffers.end()) {
                fullBufferCopy = it->second.storage;
            }
        }
    }
    if (!fullBufferCopy.empty() && t_current && fullBufferCopy.size() <= 64 * 1024 * 1024) {
        const GlBufferDataArgs args = {target, 0x88E4 /*GL_STATIC_DRAW*/, static_cast<uint64_t>(fullBufferCopy.size())};
        std::vector<uint8_t> payload(sizeof(args) + fullBufferCopy.size());
        std::memcpy(payload.data(), &args, sizeof(args));
        std::memcpy(payload.data() + sizeof(args), fullBufferCopy.data(), fullBufferCopy.size());
        t_current->session.Append(GLF_BufferData, payload.data(), static_cast<uint32_t>(payload.size()));
    }
    return 1;
}

void WINAPI glFlushMappedBufferRange(unsigned target, ptrdiff_t offset, ptrdiff_t length) {
    Log("[GL] glFlushMappedBufferRange(target: 0x%X, offset: %td, len: %td)", target, offset, length);
}

void* WINAPI glMapBuffer(unsigned target, unsigned access) {
    Log("[GL] glMapBuffer(target: 0x%X, access: 0x%X)", target, access);
    return glMapBufferRange(target, 0, 64 * 1024, access);
}

void WINAPI glGetBufferPointerv(unsigned target, unsigned pname, void** params) {
    if (!params) return;
    *params = nullptr;
    std::lock_guard<std::mutex> lock(g_bufferMutex);
    unsigned boundId = g_currentBoundBuffers[target];
    if (boundId != 0 && pname == 0x888E /*GL_BUFFER_MAP_POINTER*/) {
        auto it = g_clientBuffers.find(boundId);
        if (it != g_clientBuffers.end() && !it->second.storage.empty()) {
            *params = it->second.storage.data();
        }
    }
}

void WINAPI glBindBufferRange(unsigned target, unsigned index, unsigned buffer, ptrdiff_t offset, ptrdiff_t size) {
    Log("[GL] glBindBufferRange(target: 0x%X, idx: %u, buf: %u, offset: %td, size: %td)", target, index, buffer, offset, size);
    glBindBuffer(target, buffer);
    if (t_current) {
        GlBindBufferRangeArgs args = {target, index, buffer, 0, static_cast<uint64_t>(offset), static_cast<uint64_t>(size)};
        t_current->session.Append(GLF_BindBufferRange, &args, sizeof(args));
    }
}

void WINAPI glBindBufferBase(unsigned target, unsigned index, unsigned buffer) {
    Log("[GL] glBindBufferBase(target: 0x%X, idx: %u, buf: %u)", target, index, buffer);
    glBindBuffer(target, buffer);
    if (t_current) {
        GlBindBufferBaseArgs args = {target, index, buffer};
        t_current->session.Append(GLF_BindBufferBase, &args, sizeof(args));
    }
}

unsigned WINAPI glCreateShader(unsigned type) {
    if (!t_current) return 0;
    static unsigned next_id = 0x10000;
    const unsigned id = next_id++;
    const GlCreateShaderArgs args = {type, id};
    t_current->session.Append(GLF_CreateShader, &args, sizeof(args));
    return id;
}

unsigned WINAPI glCreateProgram() {
    if (!t_current) return 0;
    static unsigned next_id = 0x20000;
    const unsigned id = next_id++;
    const GlIdArgs args = {id};
    t_current->session.Append(GLF_CreateProgram, &args, sizeof(args));
    return id;
}

void WINAPI glShaderSource(unsigned shader, int count, const char* const* strings, const int* lengths) {
    if (!t_current || count != 1 || !strings || !strings[0]) return;
    const size_t len = lengths && lengths[0] >= 0 ? static_cast<size_t>(lengths[0]) : std::strlen(strings[0]);
    if (len > 1024 * 1024) return;
    const GlShaderSourceArgs args = {shader, static_cast<uint32_t>(len)};
    std::vector<uint8_t> payload(sizeof(args) + len);
    std::memcpy(payload.data(), &args, sizeof(args));
    std::memcpy(payload.data() + sizeof(args), strings[0], len);
    t_current->session.Append(GLF_ShaderSource, payload.data(), static_cast<uint32_t>(payload.size()));
}

void WINAPI glCompileShader(unsigned shader) {
    if (!t_current) return;
    const GlIdArgs args = {shader};
    t_current->session.Append(GLF_CompileShader, &args, sizeof(args));
}

void WINAPI glAttachShader(unsigned program, unsigned shader) {
    if (!t_current) return;
    const GlAttachShaderArgs args = {program, shader};
    t_current->session.Append(GLF_AttachShader, &args, sizeof(args));
}

void WINAPI glLinkProgram(unsigned program) {
    if (!t_current) return;
    const GlIdArgs args = {program};
    t_current->session.Append(GLF_LinkProgram, &args, sizeof(args));
}

void WINAPI glUseProgram(unsigned program) {
    if (!t_current) return;
    const GlIdArgs args = {program};
    t_current->session.Append(GLF_UseProgram, &args, sizeof(args));
}

void WINAPI glValidateProgram(unsigned program) {
    Log("[GL] glValidateProgram(%u)", program);
}

void WINAPI glDetachShader(unsigned program, unsigned shader) {
    Log("[GL] glDetachShader(prog: %u, shader: %u)", program, shader);
}

void WINAPI glDrawArrays(unsigned mode, int first, int count) {
    if (!t_current || first < 0 || count < 0 || count > 1 << 20) return;
    const GlDrawArraysArgs args = {mode, static_cast<uint32_t>(first), static_cast<uint32_t>(count)};
    t_current->session.Append(GLF_DrawArrays, &args, sizeof(args));
}

void WINAPI glDrawElements(unsigned mode, int count, unsigned type, const void* indices) {
    Log("[GL] glDrawElements(mode: 0x%X, count: %d, type: 0x%X, indices: %p)", mode, count, type, indices);
    if (!t_current || count <= 0) return;
    GlDrawElementsArgs args = {mode, count, type, 0, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(indices))};
    t_current->session.Append(GLF_DrawElements, &args, sizeof(args));
}

void WINAPI glDrawElementsBaseVertex(unsigned mode, int count, unsigned type, const void* indices, int basevertex) {
    Log("[GL] glDrawElementsBaseVertex(mode: 0x%X, count: %d, type: 0x%X, indices: %p, basevertex: %d)",
        mode, count, type, indices, basevertex);
    if (!t_current || count <= 0) return;
    GlDrawElementsInstancedBaseVertexArgs args = {
        mode, count, type, 0,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(indices)),
        1, basevertex
    };
    t_current->session.Append(GLF_DrawElementsInstancedBaseVertex, &args, sizeof(args));
}

void WINAPI glDrawRangeElements(unsigned mode, unsigned start, unsigned end, int count, unsigned type, const void* indices) {
    Log("[GL] glDrawRangeElements(mode: 0x%X, start: %u, end: %u, count: %d, type: 0x%X, indices: %p)",
        mode, start, end, count, type, indices);
    glDrawElements(mode, count, type, indices);
}

void WINAPI glDrawRangeElementsBaseVertex(unsigned mode, unsigned start, unsigned end, int count, unsigned type, const void* indices, int basevertex) {
    Log("[GL] glDrawRangeElementsBaseVertex(mode: 0x%X, start: %u, end: %u, count: %d, type: 0x%X, indices: %p, basevertex: %d)",
        mode, start, end, count, type, indices, basevertex);
    glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
}

void WINAPI glMultiDrawElements(unsigned mode, const int* count, unsigned type, const void* const* indices, int drawcount) {
    Log("[GL] glMultiDrawElements(mode: 0x%X, type: 0x%X, drawcount: %d)", mode, type, drawcount);
}

void WINAPI glPrimitiveRestartIndex(unsigned index) {
    Log("[GL] glPrimitiveRestartIndex(%u)", index);
}

void WINAPI glDrawArraysInstanced(unsigned mode, int first, int count, int instancecount) {
    Log("[GL] glDrawArraysInstanced(mode: 0x%X, first: %d, count: %d, instances: %d)",
        mode, first, count, instancecount);
    if (!t_current || count <= 0 || instancecount <= 0) return;
    GlDrawArraysInstancedArgs args = {mode, first, count, instancecount};
    t_current->session.Append(GLF_DrawArraysInstanced, &args, sizeof(args));
}

void WINAPI glDrawElementsInstanced(unsigned mode, int count, unsigned type, const void* indices, int instancecount) {
    Log("[GL] glDrawElementsInstanced(mode: 0x%X, count: %d, type: 0x%X, indices: %p, instances: %d)",
        mode, count, type, indices, instancecount);
    if (!t_current || count <= 0 || instancecount <= 0) return;
    GlDrawElementsInstancedBaseVertexArgs args = {
        mode, count, type, 0,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(indices)),
        instancecount, 0
    };
    t_current->session.Append(GLF_DrawElementsInstancedBaseVertex, &args, sizeof(args));
}

unsigned char WINAPI glIsBuffer(unsigned id) { return id > 0 ? 1 : 0; }
unsigned char WINAPI glIsTexture(unsigned id) { return id > 0 ? 1 : 0; }
unsigned char WINAPI glIsFramebuffer(unsigned id) { return id > 0 ? 1 : 0; }
unsigned char WINAPI glIsRenderbuffer(unsigned id) { return id > 0 ? 1 : 0; }
unsigned char WINAPI glIsVertexArray(unsigned id) { return id > 0 ? 1 : 0; }
unsigned char WINAPI glIsProgram(unsigned id) { return id > 0 ? 1 : 0; }
unsigned char WINAPI glIsShader(unsigned id) { return id > 0 ? 1 : 0; }


void WINAPI glBindAttribLocation(unsigned program, unsigned index, const char* name) {
    Log("[GL] glBindAttribLocation(prog: %u, idx: %u, name: %s)", program, index, name ? name : "null");
}

void WINAPI glDeleteShader(unsigned shader) {
    Log("[GL] glDeleteShader(%u)", shader);
}

void WINAPI glDeleteProgram(unsigned program) {
    Log("[GL] glDeleteProgram(%u)", program);
}

void WINAPI glBufferStorage(unsigned target, ptrdiff_t size, const void* data, unsigned flags) {
    Log("[GL] glBufferStorage(target: 0x%X, size: %td, flags: 0x%X)", target, size, flags);
    glBufferData(target, size, data, 0x88E4 /*GL_STATIC_DRAW*/);
}

void WINAPI glCopyBufferSubData(unsigned readTarget, unsigned writeTarget,
                               ptrdiff_t readOffset, ptrdiff_t writeOffset, ptrdiff_t size) {
    Log("[GL] glCopyBufferSubData(0x%X->0x%X, %td->%td, size: %td)",
        readTarget, writeTarget, readOffset, writeOffset, size);
    if (size <= 0 || readOffset < 0 || writeOffset < 0) return;
    std::lock_guard<std::mutex> lock(g_bufferMutex);
    unsigned readId = g_currentBoundBuffers[readTarget];
    unsigned writeId = g_currentBoundBuffers[writeTarget];
    if (readId && writeId && readId != writeId) {
        auto itRead = g_clientBuffers.find(readId);
        auto itWrite = g_clientBuffers.find(writeId);
        if (itRead != g_clientBuffers.end() && itWrite != g_clientBuffers.end()) {
            if (itRead->second.storage.size() >= static_cast<size_t>(readOffset + size)) {
                if (itWrite->second.storage.size() < static_cast<size_t>(writeOffset + size)) {
                    itWrite->second.storage.resize(static_cast<size_t>(writeOffset + size));
                }
                std::memcpy(itWrite->second.storage.data() + writeOffset,
                            itRead->second.storage.data() + readOffset,
                            static_cast<size_t>(size));
            }
        }
    }
}

void WINAPI glClearBufferfv(unsigned buffer, int drawbuffer, const float* value) {
    Log("[GL] glClearBufferfv(buffer: 0x%X, drawbuffer: %d, value: %p)", buffer, drawbuffer, value);
    if (buffer == 0x1800 /*GL_COLOR*/ && value) {
        glClearColor(value[0], value[1], value[2], value[3]);
        glClear(0x4000 /*GL_COLOR_BUFFER_BIT*/);
    } else if (buffer == 0x1801 /*GL_DEPTH*/ && value) {
        glClear(0x0100 /*GL_DEPTH_BUFFER_BIT*/);
    }
}

void WINAPI glClearBufferiv(unsigned buffer, int drawbuffer, const int* value) {
    Log("[GL] glClearBufferiv(buffer: 0x%X, drawbuffer: %d, value: %p)", buffer, drawbuffer, value);
    if (buffer == 0x1802 /*GL_STENCIL*/ && value) {
        glClearStencil(value[0]);
        glClear(0x0400 /*GL_STENCIL_BUFFER_BIT*/);
    }
}

void WINAPI glClearBufferuiv(unsigned buffer, int drawbuffer, const unsigned* value) {
    Log("[GL] glClearBufferuiv(buffer: 0x%X, drawbuffer: %d, value: %p)", buffer, drawbuffer, value);
}

void WINAPI glClearBufferfi(unsigned buffer, int drawbuffer, float depth, int stencil) {
    Log("[GL] glClearBufferfi(buffer: 0x%X, drawbuffer: %d, depth: %f, stencil: %d)", buffer, drawbuffer, depth, stencil);
    glClearDepth(depth);
    glClearStencil(stencil);
    glClear(0x0100 /*GL_DEPTH_BUFFER_BIT*/ | 0x0400 /*GL_STENCIL_BUFFER_BIT*/);
}

void WINAPI glClipControl(unsigned origin, unsigned depth) {
    Log("[GL] glClipControl(origin: 0x%X, depth: 0x%X)", origin, depth);
}

void WINAPI glDisablei(unsigned cap, unsigned index) {
    Log("[GL] glDisablei(cap: 0x%X, index: %u)", cap, index);
}

void WINAPI glEnablei(unsigned cap, unsigned index) {
    Log("[GL] glEnablei(cap: 0x%X, index: %u)", cap, index);
}

void WINAPI glLogicOp(unsigned opcode) {
    Log("[GL] glLogicOp(opcode: 0x%X)", opcode);
}

void WINAPI glReadBuffer(unsigned mode) {
    Log("[GL] glReadBuffer(mode: 0x%X)", mode);
}

void WINAPI glReadPixels(int x, int y, int width, int height, unsigned format, unsigned type, void* pixels) {
    Log("[GL] glReadPixels(%d, %d, %d, %d, fmt: 0x%X, type: 0x%X, pixels: %p)", x, y, width, height, format, type, pixels);
    if (pixels && width > 0 && height > 0) {
        std::memset(pixels, 0, static_cast<size_t>(width) * height * 4);
    }
}

void WINAPI glDrawBuffer(unsigned mode) {
    Log("[GL] glDrawBuffer(mode: 0x%X)", mode);
    if (t_current) {
        GlIdArgs args = {mode};
        t_current->session.Append(GLF_DrawBuffer, &args, sizeof(args));
    }
}

void WINAPI glDrawArraysIndirect(unsigned mode, const void* indirect) {
    Log("[GL] glDrawArraysIndirect(mode: 0x%X, indirect: %p)", mode, indirect);
}

void WINAPI glDrawArraysInstancedBaseInstance(unsigned mode, int first, int count, int instancecount, unsigned baseinstance) {
    Log("[GL] glDrawArraysInstancedBaseInstance(mode: 0x%X, first: %d, count: %d, instances: %d, base: %u)",
        mode, first, count, instancecount, baseinstance);
}

void WINAPI glDrawElementsIndirect(unsigned mode, unsigned type, const void* indirect) {
    Log("[GL] glDrawElementsIndirect(mode: 0x%X, type: 0x%X, indirect: %p)", mode, type, indirect);
}

void WINAPI glDrawElementsInstancedBaseVertex(unsigned mode, int count, unsigned type, const void* indices, int instancecount, int basevertex) {
    Log("[GL] glDrawElementsInstancedBaseVertex(mode: 0x%X, count: %d, type: 0x%X, inst: %d, base: %d)",
        mode, count, type, instancecount, basevertex);
    if (!t_current || count <= 0 || instancecount <= 0) return;
    GlDrawElementsInstancedBaseVertexArgs args = {
        mode, count, type, 0,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(indices)),
        instancecount, basevertex
    };
    t_current->session.Append(GLF_DrawElementsInstancedBaseVertex, &args, sizeof(args));
}

void WINAPI glDrawElementsInstancedBaseVertexBaseInstance(unsigned mode, int count, unsigned type, const void* indices,
                                                         int instancecount, int basevertex, unsigned baseinstance) {
    Log("[GL] glDrawElementsInstancedBaseVertexBaseInstance(mode: 0x%X, count: %d, type: 0x%X, inst: %d, base: %d, baseInst: %u)",
        mode, count, type, instancecount, basevertex, baseinstance);
}

void WINAPI glMultiDrawArrays(unsigned mode, const int* first, const int* count, int drawcount) {
    Log("[GL] glMultiDrawArrays(mode: 0x%X, draws: %d)", mode, drawcount);
}

void WINAPI glMultiDrawArraysIndirect(unsigned mode, const void* indirect, int drawcount, int stride) {
    Log("[GL] glMultiDrawArraysIndirect(mode: 0x%X, draws: %d, stride: %d)", mode, drawcount, stride);
}

void WINAPI glMultiDrawElementsBaseVertex(unsigned mode, const int* count, unsigned type,
                                         const void* const* indices, int drawcount, const int* basevertex) {
    Log("[GL] glMultiDrawElementsBaseVertex(mode: 0x%X, draws: %d)", mode, drawcount);
}

void WINAPI glMultiDrawElementsIndirect(unsigned mode, unsigned type, const void* indirect, int drawcount, int stride) {
    Log("[GL] glMultiDrawElementsIndirect(mode: 0x%X, draws: %d, stride: %d)", mode, drawcount, stride);
}

static unsigned g_nextQueryId = 1;

void WINAPI glGenQueries(int n, unsigned* ids) {
    Log("[GL] glGenQueries(%d)", n);
    if (!ids || n < 0) return;
    for (int i = 0; i < n; ++i) ids[i] = g_nextQueryId++;
}

void WINAPI glDeleteQueries(int n, const unsigned* ids) {
    Log("[GL] glDeleteQueries(%d)", n);
}

void WINAPI glBeginQuery(unsigned target, unsigned id) {
    Log("[GL] glBeginQuery(0x%X, %u)", target, id);
}

void WINAPI glEndQuery(unsigned target) {
    Log("[GL] glEndQuery(0x%X)", target);
}

void WINAPI glGetQueryiv(unsigned target, unsigned pname, int* params) {
    if (!params) return;
    if (pname == 0x8865 /*GL_CURRENT_QUERY*/) *params = 0;
    else if (pname == 0x8864 /*GL_QUERY_COUNTER_BITS*/) *params = 64;
    else *params = 0;
}

unsigned char WINAPI glIsQuery(unsigned id) {
    return id > 0 ? 1 : 0;
}

void WINAPI glQueryCounter(unsigned id, unsigned target) {
    Log("[GL] glQueryCounter(id: %u, target: 0x%X)", id, target);
}

void WINAPI glGetQueryObjectiv(unsigned id, unsigned pname, int* params) {
    if (!params) return;
    if (pname == 0x8867 /*GL_QUERY_RESULT_AVAILABLE*/) *params = 1;
    else if (pname == 0x8866 /*GL_QUERY_RESULT*/) *params = 0;
    else *params = 0;
}

void WINAPI glGetQueryObjectuiv(unsigned id, unsigned pname, unsigned* params) {
    if (!params) return;
    if (pname == 0x8867 /*GL_QUERY_RESULT_AVAILABLE*/) *params = 1;
    else if (pname == 0x8866 /*GL_QUERY_RESULT*/) *params = 0;
    else *params = 0;
}

void WINAPI glGetQueryObjecti64v(unsigned id, unsigned pname, long long* params) {
    if (!params) return;
    if (pname == 0x8867 /*GL_QUERY_RESULT_AVAILABLE*/) *params = 1;
    else if (pname == 0x8866 /*GL_QUERY_RESULT*/) *params = 0;
    else *params = 0;
}

void WINAPI glGetQueryObjectui64v(unsigned id, unsigned pname, unsigned long long* params) {
    if (!params) return;
    if (pname == 0x8867 /*GL_QUERY_RESULT_AVAILABLE*/) *params = 1;
    else if (pname == 0x8866 /*GL_QUERY_RESULT*/) *params = 0;
    else *params = 0;
}

void WINAPI glDebugMessageCallback(void* callback, const void* userParam) {
    Log("[GL] glDebugMessageCallback(%p, %p)", callback, userParam);
}

void WINAPI glDebugMessageCallbackARB(void* callback, const void* userParam) {
    Log("[GL] glDebugMessageCallbackARB(%p, %p)", callback, userParam);
}

void WINAPI glDebugMessageControl(unsigned source, unsigned type, unsigned severity,
                                  int count, const unsigned* ids, unsigned char enabled) {
    Log("[GL] glDebugMessageControl(src: 0x%X, type: 0x%X, sev: 0x%X, count: %d, enabled: %u)",
        source, type, severity, count, enabled);
}

void WINAPI glDebugMessageControlARB(unsigned source, unsigned type, unsigned severity,
                                     int count, const unsigned* ids, unsigned char enabled) {
    Log("[GL] glDebugMessageControlARB(src: 0x%X, type: 0x%X, sev: 0x%X, count: %d, enabled: %u)",
        source, type, severity, count, enabled);
}

void WINAPI glObjectLabel(unsigned identifier, unsigned name, int length, const char* label) {
    Log("[GL] glObjectLabel(id: 0x%X, name: %u, label: %s)", identifier, name, label ? label : "");
}

void WINAPI glLabelObjectEXT(unsigned type, unsigned object, int length, const char* label) {
    Log("[GL] glLabelObjectEXT(type: 0x%X, obj: %u, label: %s)", type, object, label ? label : "");
}

void WINAPI glPushDebugGroup(unsigned source, unsigned id, int length, const char* message) {
    Log("[GL] glPushDebugGroup(src: 0x%X, id: %u, msg: %s)", source, id, message ? message : "");
}

void WINAPI glPopDebugGroup() {
    Log("[GL] glPopDebugGroup()");
}

void WINAPI glBindVertexBuffer(unsigned bindingindex, unsigned buffer, ptrdiff_t offset, int stride) {
    Log("[GL] glBindVertexBuffer(idx: %u, buf: %u, off: %td, stride: %d)", bindingindex, buffer, offset, stride);
}

void WINAPI glVertexAttribBinding(unsigned attribindex, unsigned bindingindex) {
    Log("[GL] glVertexAttribBinding(attrib: %u, binding: %u)", attribindex, bindingindex);
}

void WINAPI glVertexAttribFormat(unsigned attribindex, int size, unsigned type, unsigned char normalized, unsigned relativeoffset) {
    Log("[GL] glVertexAttribFormat(attrib: %u, size: %d, type: 0x%X, norm: %u, off: %u)", attribindex, size, type, normalized, relativeoffset);
}

void WINAPI glVertexAttribIFormat(unsigned attribindex, int size, unsigned type, unsigned relativeoffset) {
    Log("[GL] glVertexAttribIFormat(attrib: %u, size: %d, type: 0x%X, off: %u)", attribindex, size, type, relativeoffset);
}

void WINAPI glVertexBindingDivisor(unsigned bindingindex, unsigned divisor) {
    Log("[GL] glVertexBindingDivisor(binding: %u, divisor: %u)", bindingindex, divisor);
}

void WINAPI glTexBuffer(unsigned target, unsigned internalformat, unsigned buffer) {
    Log("[GL] glTexBuffer(target: 0x%X, internalformat: 0x%X, buffer: %u)", target, internalformat, buffer);
}

void WINAPI glCreateBuffers(int n, unsigned* buffers) {
    Log("[GL] glCreateBuffers(%d)", n);
    glGenBuffers(n, buffers);
}

void WINAPI glNamedBufferData(unsigned buffer, ptrdiff_t size, const void* data, unsigned usage) {
    Log("[GL] glNamedBufferData(buf: %u, size: %td, usage: 0x%X)", buffer, size, usage);
    std::lock_guard<std::mutex> lock(g_bufferMutex);
    auto& buf = g_clientBuffers[buffer];
    buf.storage.resize(size > 0 ? static_cast<size_t>(size) : 0);
    if (data && size > 0) {
        std::memcpy(buf.storage.data(), data, static_cast<size_t>(size));
    }
}

void WINAPI glNamedBufferSubData(unsigned buffer, ptrdiff_t offset, ptrdiff_t size, const void* data) {
    Log("[GL] glNamedBufferSubData(buf: %u, off: %td, size: %td)", buffer, offset, size);
    std::lock_guard<std::mutex> lock(g_bufferMutex);
    auto& buf = g_clientBuffers[buffer];
    if (offset >= 0 && size > 0 && data) {
        if (buf.storage.size() < static_cast<size_t>(offset + size)) {
            buf.storage.resize(static_cast<size_t>(offset + size));
        }
        std::memcpy(buf.storage.data() + offset, data, static_cast<size_t>(size));
    }
}

void WINAPI glNamedBufferStorage(unsigned buffer, ptrdiff_t size, const void* data, unsigned flags) {
    Log("[GL] glNamedBufferStorage(buf: %u, size: %td, flags: 0x%X)", buffer, size, flags);
    glNamedBufferData(buffer, size, data, 0x88E4 /*GL_STATIC_DRAW*/);
}

void WINAPI glCopyNamedBufferSubData(unsigned readBuffer, unsigned writeBuffer,
                                    ptrdiff_t readOffset, ptrdiff_t writeOffset, ptrdiff_t size) {
    Log("[GL] glCopyNamedBufferSubData(%u->%u, %td->%td, size: %td)",
        readBuffer, writeBuffer, readOffset, writeOffset, size);
    if (size <= 0 || readOffset < 0 || writeOffset < 0) return;
    std::lock_guard<std::mutex> lock(g_bufferMutex);
    auto itRead = g_clientBuffers.find(readBuffer);
    auto itWrite = g_clientBuffers.find(writeBuffer);
    if (itRead != g_clientBuffers.end() && itWrite != g_clientBuffers.end()) {
        if (itRead->second.storage.size() >= static_cast<size_t>(readOffset + size)) {
            if (itWrite->second.storage.size() < static_cast<size_t>(writeOffset + size)) {
                itWrite->second.storage.resize(static_cast<size_t>(writeOffset + size));
            }
            std::memcpy(itWrite->second.storage.data() + writeOffset,
                        itRead->second.storage.data() + readOffset,
                        static_cast<size_t>(size));
        }
    }
}

void* WINAPI glMapNamedBufferRange(unsigned buffer, ptrdiff_t offset, ptrdiff_t length, unsigned access) {
    Log("[GL] glMapNamedBufferRange(buf: %u, off: %td, len: %td, access: 0x%X)", buffer, offset, length, access);
    std::lock_guard<std::mutex> lock(g_bufferMutex);
    auto& buf = g_clientBuffers[buffer];
    if (offset < 0 || length < 0) return nullptr;
    if (buf.storage.size() < static_cast<size_t>(offset + length)) {
        buf.storage.resize(static_cast<size_t>(offset + length));
    }
    return buf.storage.data() + offset;
}

unsigned char WINAPI glUnmapNamedBuffer(unsigned buffer) {
    Log("[GL] glUnmapNamedBuffer(buf: %u)", buffer);
    std::vector<uint8_t> fullBufferCopy;
    {
        std::lock_guard<std::mutex> lock(g_bufferMutex);
        auto it = g_clientBuffers.find(buffer);
        if (it != g_clientBuffers.end()) {
            fullBufferCopy = it->second.storage;
        }
    }
    if (!fullBufferCopy.empty() && t_current && fullBufferCopy.size() <= 64 * 1024 * 1024) {
        const GlBufferDataArgs args = {0x8892 /*GL_ARRAY_BUFFER*/, 0x88E4 /*GL_STATIC_DRAW*/, static_cast<uint64_t>(fullBufferCopy.size())};
        std::vector<uint8_t> payload(sizeof(args) + fullBufferCopy.size());
        std::memcpy(payload.data(), &args, sizeof(args));
        std::memcpy(payload.data() + sizeof(args), fullBufferCopy.data(), fullBufferCopy.size());
        t_current->session.Append(GLF_BufferData, payload.data(), static_cast<uint32_t>(payload.size()));
    }
    return 1;
}

void WINAPI glFlushMappedNamedBufferRange(unsigned buffer, ptrdiff_t offset, ptrdiff_t length) {
    Log("[GL] glFlushMappedNamedBufferRange(buf: %u, off: %td, len: %td)", buffer, offset, length);
}

void WINAPI glCreateFramebuffers(int n, unsigned* framebuffers) {
    Log("[GL] glCreateFramebuffers(%d)", n);
    glGenFramebuffers(n, framebuffers);
}

void WINAPI glNamedFramebufferTexture(unsigned framebuffer, unsigned attachment, unsigned texture, int level) {
    Log("[GL] glNamedFramebufferTexture(fb: %u, attach: 0x%X, tex: %u, lvl: %d)", framebuffer, attachment, texture, level);
}

void WINAPI glBlitNamedFramebuffer(unsigned readFramebuffer, unsigned drawFramebuffer,
                                   int srcX0, int srcY0, int srcX1, int srcY1,
                                   int dstX0, int dstY0, int dstX1, int dstY1,
                                   unsigned mask, unsigned filter) {
    Log("[GL] glBlitNamedFramebuffer(%u->%u, mask: 0x%X)", readFramebuffer, drawFramebuffer, mask);
}

// --- Dispatch Table for wglGetProcAddress -------------------------------------------------------

PROC WINAPI wglGetProcAddress(LPCSTR name) {
    if (!name) return nullptr;
    Log("[wglGetProcAddress] Query: %s", name);

    struct Entry { const char* name; PROC fn; };
    static const Entry entries[] = {
        {"wglCreateContextAttribsARB", reinterpret_cast<PROC>(CreateContextAttribsArb)},
        {"wglGetExtensionsStringARB", reinterpret_cast<PROC>(GetExtensionsStringArb)},
        {"wglGetExtensionsStringEXT", reinterpret_cast<PROC>(GetExtensionsStringArb)},
        {"wglSwapIntervalEXT", reinterpret_cast<PROC>(SwapIntervalExt)},
        {"wglGetSwapIntervalEXT", reinterpret_cast<PROC>(GetSwapIntervalExt)},
        {"wglChoosePixelFormatARB", reinterpret_cast<PROC>(wglChoosePixelFormatARB)},
        {"wglGetPixelFormatAttribivARB", reinterpret_cast<PROC>(wglGetPixelFormatAttribivARB)},
        {"wglGetPixelFormatAttribfvARB", reinterpret_cast<PROC>(wglGetPixelFormatAttribfvARB)},
        {"glGetString", reinterpret_cast<PROC>(glGetString)},
        {"glGetStringi", reinterpret_cast<PROC>(glGetStringi)},
        {"glGetIntegerv", reinterpret_cast<PROC>(glGetIntegerv)},
        {"glGetFloatv", reinterpret_cast<PROC>(glGetFloatv)},
        {"glGetDoublev", reinterpret_cast<PROC>(glGetDoublev)},
        {"glGetInteger64v", reinterpret_cast<PROC>(glGetInteger64v)},
        {"glGetInternalformativ", reinterpret_cast<PROC>(glGetInternalformativ)},
        {"glGetTexLevelParameteriv", reinterpret_cast<PROC>(glGetTexLevelParameteriv)},
        {"glPixelStorei", reinterpret_cast<PROC>(glPixelStorei)},
        {"glGetIntegeri_v", reinterpret_cast<PROC>(glGetIntegeri_v)},
        {"glGetBufferParameteriv", reinterpret_cast<PROC>(glGetBufferParameteriv)},
        {"glGetBufferParameteri64v", reinterpret_cast<PROC>(glGetBufferParameteri64v)},
        {"glGetVertexAttribiv", reinterpret_cast<PROC>(glGetVertexAttribiv)},
        {"glGetUniformiv", reinterpret_cast<PROC>(glGetUniformiv)},
        {"glGetUniformfv", reinterpret_cast<PROC>(glGetUniformfv)},
        {"glBindFramebuffer", reinterpret_cast<PROC>(glBindFramebuffer)},
        {"glBindFramebufferEXT", reinterpret_cast<PROC>(glBindFramebuffer)},
        {"glBindRenderbuffer", reinterpret_cast<PROC>(glBindRenderbuffer)},
        {"glBindRenderbufferEXT", reinterpret_cast<PROC>(glBindRenderbuffer)},
        {"glFramebufferTexture2D", reinterpret_cast<PROC>(glFramebufferTexture2D)},
        {"glFramebufferTexture2DEXT", reinterpret_cast<PROC>(glFramebufferTexture2D)},
        {"glFramebufferRenderbuffer", reinterpret_cast<PROC>(glFramebufferRenderbuffer)},
        {"glFramebufferRenderbufferEXT", reinterpret_cast<PROC>(glFramebufferRenderbuffer)},
        {"glGenFramebuffers", reinterpret_cast<PROC>(glGenFramebuffers)},
        {"glGenFramebuffersEXT", reinterpret_cast<PROC>(glGenFramebuffers)},
        {"glDeleteFramebuffers", reinterpret_cast<PROC>(glDeleteFramebuffers)},
        {"glDeleteFramebuffersEXT", reinterpret_cast<PROC>(glDeleteFramebuffers)},
        {"glGenRenderbuffers", reinterpret_cast<PROC>(glGenRenderbuffers)},
        {"glGenRenderbuffersEXT", reinterpret_cast<PROC>(glGenRenderbuffers)},
        {"glDeleteRenderbuffers", reinterpret_cast<PROC>(glDeleteRenderbuffers)},
        {"glDeleteRenderbuffersEXT", reinterpret_cast<PROC>(glDeleteRenderbuffers)},
        {"glRenderbufferStorage", reinterpret_cast<PROC>(glRenderbufferStorage)},
        {"glRenderbufferStorageEXT", reinterpret_cast<PROC>(glRenderbufferStorage)},
        {"glRenderbufferStorageMultisample", reinterpret_cast<PROC>(glRenderbufferStorageMultisample)},
        {"glBlitFramebuffer", reinterpret_cast<PROC>(glBlitFramebuffer)},
        {"glBlitFramebufferEXT", reinterpret_cast<PROC>(glBlitFramebuffer)},
        {"glDrawBuffers", reinterpret_cast<PROC>(glDrawBuffers)},
        {"glDrawBuffersARB", reinterpret_cast<PROC>(glDrawBuffers)},
        {"glGetBooleanv", reinterpret_cast<PROC>(glGetBooleanv)},
        {"glGetError", reinterpret_cast<PROC>(glGetError)},
        {"glGetAttachedShaders", reinterpret_cast<PROC>(glGetAttachedShaders)},
        {"glGetActiveAttrib", reinterpret_cast<PROC>(glGetActiveAttrib)},
        {"glGetActiveUniform", reinterpret_cast<PROC>(glGetActiveUniform)},
        {"glGetShaderSource", reinterpret_cast<PROC>(glGetShaderSource)},
        {"glGetUniformLocationARB", reinterpret_cast<PROC>(glGetUniformLocationARB)},
        {"glEnable", reinterpret_cast<PROC>(glEnable)},
        {"glDisable", reinterpret_cast<PROC>(glDisable)},
        {"glIsEnabled", reinterpret_cast<PROC>(glIsEnabled)},
        {"glClear", reinterpret_cast<PROC>(glClear)},
        {"glClearColor", reinterpret_cast<PROC>(glClearColor)},
        {"glViewport", reinterpret_cast<PROC>(glViewport)},
        {"glFlush", reinterpret_cast<PROC>(glFlush)},
        {"glFinish", reinterpret_cast<PROC>(glFinish)},
        {"glDrawArrays", reinterpret_cast<PROC>(glDrawArrays)},
        {"glDrawElements", reinterpret_cast<PROC>(glDrawElements)},
        {"glDrawElementsBaseVertex", reinterpret_cast<PROC>(glDrawElementsBaseVertex)},
        {"glDrawRangeElements", reinterpret_cast<PROC>(glDrawRangeElements)},
        {"glDrawArraysInstanced", reinterpret_cast<PROC>(glDrawArraysInstanced)},
        {"glDrawArraysInstancedARB", reinterpret_cast<PROC>(glDrawArraysInstanced)},
        {"glDrawElementsInstanced", reinterpret_cast<PROC>(glDrawElementsInstanced)},
        {"glDrawElementsInstancedARB", reinterpret_cast<PROC>(glDrawElementsInstanced)},
        {"glGenBuffers", reinterpret_cast<PROC>(glGenBuffers)},
        {"glGenBuffersARB", reinterpret_cast<PROC>(glGenBuffers)},
        {"glBindBuffer", reinterpret_cast<PROC>(glBindBuffer)},
        {"glBindBufferARB", reinterpret_cast<PROC>(glBindBuffer)},
        {"glBufferData", reinterpret_cast<PROC>(glBufferData)},
        {"glBufferDataARB", reinterpret_cast<PROC>(glBufferData)},
        {"glBufferSubData", reinterpret_cast<PROC>(glBufferSubData)},
        {"glBufferSubDataARB", reinterpret_cast<PROC>(glBufferSubData)},
        {"glGetBufferSubData", reinterpret_cast<PROC>(glGetBufferSubData)},
        {"glGetBufferSubDataARB", reinterpret_cast<PROC>(glGetBufferSubData)},
        {"glDeleteBuffers", reinterpret_cast<PROC>(glDeleteBuffers)},
        {"glDeleteBuffersARB", reinterpret_cast<PROC>(glDeleteBuffers)},
        {"glMapBuffer", reinterpret_cast<PROC>(glMapBuffer)},
        {"glMapBufferARB", reinterpret_cast<PROC>(glMapBuffer)},
        {"glMapBufferRange", reinterpret_cast<PROC>(glMapBufferRange)},
        {"glUnmapBuffer", reinterpret_cast<PROC>(glUnmapBuffer)},
        {"glUnmapBufferARB", reinterpret_cast<PROC>(glUnmapBuffer)},
        {"glFlushMappedBufferRange", reinterpret_cast<PROC>(glFlushMappedBufferRange)},
        {"glBindBufferRange", reinterpret_cast<PROC>(glBindBufferRange)},
        {"glBindBufferBase", reinterpret_cast<PROC>(glBindBufferBase)},
        {"glUniformBlockBinding", reinterpret_cast<PROC>(glUniformBlockBinding)},
        {"glGetUniformBlockIndex", reinterpret_cast<PROC>(glGetUniformBlockIndex)},
        {"glFenceSync", reinterpret_cast<PROC>(glFenceSync)},
        {"glClientWaitSync", reinterpret_cast<PROC>(glClientWaitSync)},
        {"glWaitSync", reinterpret_cast<PROC>(glWaitSync)},
        {"glDeleteSync", reinterpret_cast<PROC>(glDeleteSync)},
        {"glIsSync", reinterpret_cast<PROC>(glIsSync)},
        {"glCreateShader", reinterpret_cast<PROC>(glCreateShader)},
        {"glCreateProgram", reinterpret_cast<PROC>(glCreateProgram)},
        {"glShaderSource", reinterpret_cast<PROC>(glShaderSource)},
        {"glCompileShader", reinterpret_cast<PROC>(glCompileShader)},
        {"glAttachShader", reinterpret_cast<PROC>(glAttachShader)},
        {"glLinkProgram", reinterpret_cast<PROC>(glLinkProgram)},
        {"glUseProgram", reinterpret_cast<PROC>(glUseProgram)},
        {"glGenTextures", reinterpret_cast<PROC>(glGenTextures)},
        {"glBindTexture", reinterpret_cast<PROC>(glBindTexture)},
        {"glDeleteTextures", reinterpret_cast<PROC>(glDeleteTextures)},
        {"glTexParameteri", reinterpret_cast<PROC>(glTexParameteri)},
        {"glTexParameterf", reinterpret_cast<PROC>(glTexParameterf)},
        {"glTexParameteriv", reinterpret_cast<PROC>(glTexParameteriv)},
        {"glTexParameterfv", reinterpret_cast<PROC>(glTexParameterfv)},
        {"glGetTexParameteriv", reinterpret_cast<PROC>(glGetTexParameteriv)},
        {"glGetTexParameterfv", reinterpret_cast<PROC>(glGetTexParameterfv)},
        {"glTexImage1D", reinterpret_cast<PROC>(glTexImage1D)},
        {"glTexImage2D", reinterpret_cast<PROC>(glTexImage2D)},
        {"glTexImage3D", reinterpret_cast<PROC>(glTexImage3D)},
        {"glTexSubImage1D", reinterpret_cast<PROC>(glTexSubImage1D)},
        {"glTexSubImage2D", reinterpret_cast<PROC>(glTexSubImage2D)},
        {"glTexSubImage3D", reinterpret_cast<PROC>(glTexSubImage3D)},
        {"glTexSubImage3DEXT", reinterpret_cast<PROC>(glTexSubImage3D)},
        {"glCopyTexSubImage2D", reinterpret_cast<PROC>(glCopyTexSubImage2D)},
        {"glGenerateMipmap", reinterpret_cast<PROC>(glGenerateMipmap)},
        {"glGenerateMipmapEXT", reinterpret_cast<PROC>(glGenerateMipmap)},
        {"glActiveTexture", reinterpret_cast<PROC>(glActiveTexture)},
        {"glActiveTextureARB", reinterpret_cast<PROC>(glActiveTexture)},
        {"glClientActiveTexture", reinterpret_cast<PROC>(glClientActiveTexture)},
        {"glClientActiveTextureARB", reinterpret_cast<PROC>(glClientActiveTexture)},
        {"glGenVertexArrays", reinterpret_cast<PROC>(glGenVertexArrays)},
        {"glBindVertexArray", reinterpret_cast<PROC>(glBindVertexArray)},
        {"glDeleteVertexArrays", reinterpret_cast<PROC>(glDeleteVertexArrays)},
        {"glVertexAttribPointer", reinterpret_cast<PROC>(glVertexAttribPointer)},
        {"glVertexAttribIPointer", reinterpret_cast<PROC>(glVertexAttribIPointer)},
        {"glVertexAttribDivisor", reinterpret_cast<PROC>(glVertexAttribDivisor)},
        {"glEnableVertexAttribArray", reinterpret_cast<PROC>(glEnableVertexAttribArray)},
        {"glDisableVertexAttribArray", reinterpret_cast<PROC>(glDisableVertexAttribArray)},
        {"glGetUniformLocation", reinterpret_cast<PROC>(glGetUniformLocation)},
        {"glGetAttribLocation", reinterpret_cast<PROC>(glGetAttribLocation)},
        {"glUniform1i", reinterpret_cast<PROC>(glUniform1i)},
        {"glUniform1iARB", reinterpret_cast<PROC>(glUniform1i)},
        {"glUniform1iv", reinterpret_cast<PROC>(glUniform1iv)},
        {"glUniform1ivARB", reinterpret_cast<PROC>(glUniform1iv)},
        {"glUniform1f", reinterpret_cast<PROC>(glUniform1f)},
        {"glUniform1fARB", reinterpret_cast<PROC>(glUniform1f)},
        {"glUniform1fv", reinterpret_cast<PROC>(glUniform1fv)},
        {"glUniform1fvARB", reinterpret_cast<PROC>(glUniform1fv)},
        {"glUniform2f", reinterpret_cast<PROC>(glUniform2f)},
        {"glUniform2fARB", reinterpret_cast<PROC>(glUniform2f)},
        {"glUniform2fv", reinterpret_cast<PROC>(glUniform2fv)},
        {"glUniform2fvARB", reinterpret_cast<PROC>(glUniform2fv)},
        {"glUniform3f", reinterpret_cast<PROC>(glUniform3f)},
        {"glUniform3fARB", reinterpret_cast<PROC>(glUniform3f)},
        {"glUniform3fv", reinterpret_cast<PROC>(glUniform3fv)},
        {"glUniform3fvARB", reinterpret_cast<PROC>(glUniform3fv)},
        {"glUniform4f", reinterpret_cast<PROC>(glUniform4f)},
        {"glUniform4fARB", reinterpret_cast<PROC>(glUniform4f)},
        {"glUniform4fv", reinterpret_cast<PROC>(glUniform4fv)},
        {"glUniform4fvARB", reinterpret_cast<PROC>(glUniform4fv)},
        {"glUniformMatrix2fv", reinterpret_cast<PROC>(glUniformMatrix2fv)},
        {"glUniformMatrix2fvARB", reinterpret_cast<PROC>(glUniformMatrix2fv)},
        {"glUniformMatrix3fv", reinterpret_cast<PROC>(glUniformMatrix3fv)},
        {"glUniformMatrix3fvARB", reinterpret_cast<PROC>(glUniformMatrix3fv)},
        {"glUniformMatrix4fv", reinterpret_cast<PROC>(glUniformMatrix4fv)},
        {"glUniformMatrix4fvARB", reinterpret_cast<PROC>(glUniformMatrix4fv)},
        {"glGetShaderiv", reinterpret_cast<PROC>(glGetShaderiv)},
        {"glGetProgramiv", reinterpret_cast<PROC>(glGetProgramiv)},
        {"glGetShaderInfoLog", reinterpret_cast<PROC>(glGetShaderInfoLog)},
        {"glGetProgramInfoLog", reinterpret_cast<PROC>(glGetProgramInfoLog)},
        {"glCheckFramebufferStatus", reinterpret_cast<PROC>(glCheckFramebufferStatus)},
        {"glCheckFramebufferStatusEXT", reinterpret_cast<PROC>(glCheckFramebufferStatus)},
        {"glBlendFunc", reinterpret_cast<PROC>(glBlendFunc)},
        {"glBlendFuncSeparate", reinterpret_cast<PROC>(glBlendFuncSeparate)},
        {"glBlendFuncSeparateEXT", reinterpret_cast<PROC>(glBlendFuncSeparate)},
        {"glBlendFuncSeparateARB", reinterpret_cast<PROC>(glBlendFuncSeparate)},
        {"glBlendEquation", reinterpret_cast<PROC>(glBlendEquation)},
        {"glBlendEquationEXT", reinterpret_cast<PROC>(glBlendEquation)},
        {"glBlendEquationARB", reinterpret_cast<PROC>(glBlendEquation)},
        {"glBlendEquationSeparate", reinterpret_cast<PROC>(glBlendEquationSeparate)},
        {"glBlendEquationSeparateEXT", reinterpret_cast<PROC>(glBlendEquationSeparate)},
        {"glBlendEquationSeparateARB", reinterpret_cast<PROC>(glBlendEquationSeparate)},
        {"glDepthFunc", reinterpret_cast<PROC>(glDepthFunc)},
        {"glDepthMask", reinterpret_cast<PROC>(glDepthMask)},
        {"glDepthRange", reinterpret_cast<PROC>(glDepthRange)},
        {"glDepthRangef", reinterpret_cast<PROC>(glDepthRangef)},
        {"glCullFace", reinterpret_cast<PROC>(glCullFace)},
        {"glScissor", reinterpret_cast<PROC>(glScissor)},
        {"glColorMask", reinterpret_cast<PROC>(glColorMask)},
        {"glColorMaski", reinterpret_cast<PROC>(glColorMaski)},
        {"glClearDepth", reinterpret_cast<PROC>(glClearDepth)},
        {"glClearDepthf", reinterpret_cast<PROC>(glClearDepthf)},
        {"glClearStencil", reinterpret_cast<PROC>(glClearStencil)},
        {"glPolygonMode", reinterpret_cast<PROC>(glPolygonMode)},
        {"glPolygonOffset", reinterpret_cast<PROC>(glPolygonOffset)},
        {"glGenSamplers", reinterpret_cast<PROC>(glGenSamplers)},
        {"glDeleteSamplers", reinterpret_cast<PROC>(glDeleteSamplers)},
        {"glIsSampler", reinterpret_cast<PROC>(glIsSampler)},
        {"glBindSampler", reinterpret_cast<PROC>(glBindSampler)},
        {"glSamplerParameteri", reinterpret_cast<PROC>(glSamplerParameteri)},
        {"glSamplerParameterf", reinterpret_cast<PROC>(glSamplerParameterf)},
        {"glSamplerParameteriv", reinterpret_cast<PROC>(glSamplerParameteriv)},
        {"glSamplerParameterfv", reinterpret_cast<PROC>(glSamplerParameterfv)},
        {"glGetSamplerParameteriv", reinterpret_cast<PROC>(glGetSamplerParameteriv)},
        {"glGetSamplerParameterfv", reinterpret_cast<PROC>(glGetSamplerParameterfv)},
        {"glIsBuffer", reinterpret_cast<PROC>(glIsBuffer)},
        {"glIsTexture", reinterpret_cast<PROC>(glIsTexture)},
        {"glIsFramebuffer", reinterpret_cast<PROC>(glIsFramebuffer)},
        {"glIsRenderbuffer", reinterpret_cast<PROC>(glIsRenderbuffer)},
        {"glIsVertexArray", reinterpret_cast<PROC>(glIsVertexArray)},
        {"glIsProgram", reinterpret_cast<PROC>(glIsProgram)},
        {"glIsShader", reinterpret_cast<PROC>(glIsShader)},
        {"glBindAttribLocation", reinterpret_cast<PROC>(glBindAttribLocation)},
        {"glBindAttribLocationARB", reinterpret_cast<PROC>(glBindAttribLocation)},
        {"glDeleteShader", reinterpret_cast<PROC>(glDeleteShader)},
        {"glDeleteProgram", reinterpret_cast<PROC>(glDeleteProgram)},
        {"glBufferStorage", reinterpret_cast<PROC>(glBufferStorage)},
        {"glCopyBufferSubData", reinterpret_cast<PROC>(glCopyBufferSubData)},
        {"glClearBufferfv", reinterpret_cast<PROC>(glClearBufferfv)},
        {"glClipControl", reinterpret_cast<PROC>(glClipControl)},
        {"glDisablei", reinterpret_cast<PROC>(glDisablei)},
        {"glEnablei", reinterpret_cast<PROC>(glEnablei)},
        {"glLogicOp", reinterpret_cast<PROC>(glLogicOp)},
        {"glReadBuffer", reinterpret_cast<PROC>(glReadBuffer)},
        {"glReadPixels", reinterpret_cast<PROC>(glReadPixels)},
        {"glDrawBuffer", reinterpret_cast<PROC>(glDrawBuffer)},
        {"glDrawArraysIndirect", reinterpret_cast<PROC>(glDrawArraysIndirect)},
        {"glDrawArraysInstancedBaseInstance", reinterpret_cast<PROC>(glDrawArraysInstancedBaseInstance)},
        {"glDrawElementsIndirect", reinterpret_cast<PROC>(glDrawElementsIndirect)},
        {"glDrawElementsInstancedBaseVertex", reinterpret_cast<PROC>(glDrawElementsInstancedBaseVertex)},
        {"glDrawElementsInstancedBaseVertexBaseInstance", reinterpret_cast<PROC>(glDrawElementsInstancedBaseVertexBaseInstance)},
        {"glMultiDrawArrays", reinterpret_cast<PROC>(glMultiDrawArrays)},
        {"glMultiDrawArraysIndirect", reinterpret_cast<PROC>(glMultiDrawArraysIndirect)},
        {"glMultiDrawElementsBaseVertex", reinterpret_cast<PROC>(glMultiDrawElementsBaseVertex)},
        {"glMultiDrawElementsIndirect", reinterpret_cast<PROC>(glMultiDrawElementsIndirect)},
        {"glGenQueries", reinterpret_cast<PROC>(glGenQueries)},
        {"glGenQueriesARB", reinterpret_cast<PROC>(glGenQueries)},
        {"glDeleteQueries", reinterpret_cast<PROC>(glDeleteQueries)},
        {"glDeleteQueriesARB", reinterpret_cast<PROC>(glDeleteQueries)},
        {"glQueryCounter", reinterpret_cast<PROC>(glQueryCounter)},
        {"glGetQueryObjectiv", reinterpret_cast<PROC>(glGetQueryObjectiv)},
        {"glGetQueryObjectivARB", reinterpret_cast<PROC>(glGetQueryObjectiv)},
        {"glGetQueryObjectuiv", reinterpret_cast<PROC>(glGetQueryObjectuiv)},
        {"glGetQueryObjectuivARB", reinterpret_cast<PROC>(glGetQueryObjectuiv)},
        {"glGetQueryObjecti64v", reinterpret_cast<PROC>(glGetQueryObjecti64v)},
        {"glGetQueryObjectui64v", reinterpret_cast<PROC>(glGetQueryObjectui64v)},
        {"glDebugMessageCallback", reinterpret_cast<PROC>(glDebugMessageCallback)},
        {"glDebugMessageCallbackARB", reinterpret_cast<PROC>(glDebugMessageCallbackARB)},
        {"glDebugMessageControl", reinterpret_cast<PROC>(glDebugMessageControl)},
        {"glDebugMessageControlARB", reinterpret_cast<PROC>(glDebugMessageControlARB)},
        {"glObjectLabel", reinterpret_cast<PROC>(glObjectLabel)},
        {"glLabelObjectEXT", reinterpret_cast<PROC>(glLabelObjectEXT)},
        {"glPushDebugGroup", reinterpret_cast<PROC>(glPushDebugGroup)},
        {"glPopDebugGroup", reinterpret_cast<PROC>(glPopDebugGroup)},
        {"glBindVertexBuffer", reinterpret_cast<PROC>(glBindVertexBuffer)},
        {"glVertexAttribBinding", reinterpret_cast<PROC>(glVertexAttribBinding)},
        {"glVertexAttribFormat", reinterpret_cast<PROC>(glVertexAttribFormat)},
        {"glVertexAttribIFormat", reinterpret_cast<PROC>(glVertexAttribIFormat)},
        {"glVertexBindingDivisor", reinterpret_cast<PROC>(glVertexBindingDivisor)},
        {"glTexBuffer", reinterpret_cast<PROC>(glTexBuffer)},
        {"glTexBufferARB", reinterpret_cast<PROC>(glTexBuffer)},
        {"glCreateBuffers", reinterpret_cast<PROC>(glCreateBuffers)},
        {"glNamedBufferData", reinterpret_cast<PROC>(glNamedBufferData)},
        {"glNamedBufferDataEXT", reinterpret_cast<PROC>(glNamedBufferData)},
        {"glNamedBufferSubData", reinterpret_cast<PROC>(glNamedBufferSubData)},
        {"glNamedBufferSubDataEXT", reinterpret_cast<PROC>(glNamedBufferSubData)},
        {"glNamedBufferStorage", reinterpret_cast<PROC>(glNamedBufferStorage)},
        {"glCopyNamedBufferSubData", reinterpret_cast<PROC>(glCopyNamedBufferSubData)},
        {"glMapNamedBufferRange", reinterpret_cast<PROC>(glMapNamedBufferRange)},
        {"glMapNamedBufferRangeEXT", reinterpret_cast<PROC>(glMapNamedBufferRange)},
        {"glUnmapNamedBuffer", reinterpret_cast<PROC>(glUnmapNamedBuffer)},
        {"glUnmapNamedBufferEXT", reinterpret_cast<PROC>(glUnmapNamedBuffer)},
        {"glFlushMappedNamedBufferRange", reinterpret_cast<PROC>(glFlushMappedNamedBufferRange)},
        {"glFlushMappedNamedBufferRangeEXT", reinterpret_cast<PROC>(glFlushMappedNamedBufferRange)},
        {"glCreateFramebuffers", reinterpret_cast<PROC>(glCreateFramebuffers)},
        {"glNamedFramebufferTexture", reinterpret_cast<PROC>(glNamedFramebufferTexture)},
        {"glNamedFramebufferTextureEXT", reinterpret_cast<PROC>(glNamedFramebufferTexture)},
        {"glBlitNamedFramebuffer", reinterpret_cast<PROC>(glBlitNamedFramebuffer)},
        {"glFramebufferTextureLayer", reinterpret_cast<PROC>(glFramebufferTextureLayer)},
        {"glFramebufferTextureLayerARB", reinterpret_cast<PROC>(glFramebufferTextureLayer)},
        {"glFramebufferTextureLayerEXT", reinterpret_cast<PROC>(glFramebufferTextureLayer)},
        {"glFramebufferTexture", reinterpret_cast<PROC>(glFramebufferTexture)},
        {"glFramebufferTexture1D", reinterpret_cast<PROC>(glFramebufferTexture1D)},
        {"glFramebufferTexture3D", reinterpret_cast<PROC>(glFramebufferTexture3D)},
        {"glGetFramebufferAttachmentParameteriv", reinterpret_cast<PROC>(glGetFramebufferAttachmentParameteriv)},
        {"glGetFramebufferAttachmentParameterivEXT", reinterpret_cast<PROC>(glGetFramebufferAttachmentParameteriv)},
        {"glGetRenderbufferParameteriv", reinterpret_cast<PROC>(glGetRenderbufferParameteriv)},
        {"glGetRenderbufferParameterivEXT", reinterpret_cast<PROC>(glGetRenderbufferParameteriv)},
        {"glBindFragDataLocation", reinterpret_cast<PROC>(glBindFragDataLocation)},
        {"glBindFragDataLocationEXT", reinterpret_cast<PROC>(glBindFragDataLocation)},
        {"glGetFragDataLocation", reinterpret_cast<PROC>(glGetFragDataLocation)},
        {"glGetFragDataLocationEXT", reinterpret_cast<PROC>(glGetFragDataLocation)},
        {"glCopyTexImage1D", reinterpret_cast<PROC>(glCopyTexImage1D)},
        {"glCopyTexImage2D", reinterpret_cast<PROC>(glCopyTexImage2D)},
        {"glCopyTexSubImage1D", reinterpret_cast<PROC>(glCopyTexSubImage1D)},
        {"glCopyTexSubImage3D", reinterpret_cast<PROC>(glCopyTexSubImage3D)},
        {"glPixelStoref", reinterpret_cast<PROC>(glPixelStoref)},
        {"glGetTexLevelParameterfv", reinterpret_cast<PROC>(glGetTexLevelParameterfv)},
        {"glTexParameterIiv", reinterpret_cast<PROC>(glTexParameterIiv)},
        {"glTexParameterIivEXT", reinterpret_cast<PROC>(glTexParameterIiv)},
        {"glTexParameterIuiv", reinterpret_cast<PROC>(glTexParameterIuiv)},
        {"glTexParameterIuivEXT", reinterpret_cast<PROC>(glTexParameterIuiv)},
        {"glGetTexParameterIiv", reinterpret_cast<PROC>(glGetTexParameterIiv)},
        {"glGetTexParameterIivEXT", reinterpret_cast<PROC>(glGetTexParameterIiv)},
        {"glGetTexParameterIuiv", reinterpret_cast<PROC>(glGetTexParameterIuiv)},
        {"glGetTexParameterIuivEXT", reinterpret_cast<PROC>(glGetTexParameterIuiv)},
        {"glUniform2i", reinterpret_cast<PROC>(glUniform2i)},
        {"glUniform2iARB", reinterpret_cast<PROC>(glUniform2i)},
        {"glUniform3i", reinterpret_cast<PROC>(glUniform3i)},
        {"glUniform3iARB", reinterpret_cast<PROC>(glUniform3i)},
        {"glUniform4i", reinterpret_cast<PROC>(glUniform4i)},
        {"glUniform4iARB", reinterpret_cast<PROC>(glUniform4i)},
        {"glUniform2iv", reinterpret_cast<PROC>(glUniform2iv)},
        {"glUniform2ivARB", reinterpret_cast<PROC>(glUniform2iv)},
        {"glUniform3iv", reinterpret_cast<PROC>(glUniform3iv)},
        {"glUniform3ivARB", reinterpret_cast<PROC>(glUniform3iv)},
        {"glUniform4iv", reinterpret_cast<PROC>(glUniform4iv)},
        {"glUniform4ivARB", reinterpret_cast<PROC>(glUniform4iv)},
        {"glUniform1ui", reinterpret_cast<PROC>(glUniform1ui)},
        {"glUniform1uiEXT", reinterpret_cast<PROC>(glUniform1ui)},
        {"glUniform2ui", reinterpret_cast<PROC>(glUniform2ui)},
        {"glUniform2uiEXT", reinterpret_cast<PROC>(glUniform2ui)},
        {"glUniform3ui", reinterpret_cast<PROC>(glUniform3ui)},
        {"glUniform3uiEXT", reinterpret_cast<PROC>(glUniform3ui)},
        {"glUniform4ui", reinterpret_cast<PROC>(glUniform4ui)},
        {"glUniform4uiEXT", reinterpret_cast<PROC>(glUniform4ui)},
        {"glUniform1uiv", reinterpret_cast<PROC>(glUniform1uiv)},
        {"glUniform1uivEXT", reinterpret_cast<PROC>(glUniform1uiv)},
        {"glUniform2uiv", reinterpret_cast<PROC>(glUniform2uiv)},
        {"glUniform2uivEXT", reinterpret_cast<PROC>(glUniform2uiv)},
        {"glUniform3uiv", reinterpret_cast<PROC>(glUniform3uiv)},
        {"glUniform3uivEXT", reinterpret_cast<PROC>(glUniform3uiv)},
        {"glUniform4uiv", reinterpret_cast<PROC>(glUniform4uiv)},
        {"glUniform4uivEXT", reinterpret_cast<PROC>(glUniform4uiv)},
        {"glGetUniformuiv", reinterpret_cast<PROC>(glGetUniformuiv)},
        {"glGetUniformuivEXT", reinterpret_cast<PROC>(glGetUniformuiv)},
        {"glGetActiveUniformBlockiv", reinterpret_cast<PROC>(glGetActiveUniformBlockiv)},
        {"glGetActiveUniformBlockName", reinterpret_cast<PROC>(glGetActiveUniformBlockName)},
        {"glGetActiveUniformsiv", reinterpret_cast<PROC>(glGetActiveUniformsiv)},
        {"glGetActiveUniformName", reinterpret_cast<PROC>(glGetActiveUniformName)},
        {"glGetUniformIndices", reinterpret_cast<PROC>(glGetUniformIndices)},
        {"glFrontFace", reinterpret_cast<PROC>(glFrontFace)},
        {"glLineWidth", reinterpret_cast<PROC>(glLineWidth)},
        {"glPointSize", reinterpret_cast<PROC>(glPointSize)},
        {"glStencilFunc", reinterpret_cast<PROC>(glStencilFunc)},
        {"glStencilMask", reinterpret_cast<PROC>(glStencilMask)},
        {"glStencilOp", reinterpret_cast<PROC>(glStencilOp)},
        {"glStencilFuncSeparate", reinterpret_cast<PROC>(glStencilFuncSeparate)},
        {"glStencilMaskSeparate", reinterpret_cast<PROC>(glStencilMaskSeparate)},
        {"glStencilOpSeparate", reinterpret_cast<PROC>(glStencilOpSeparate)},
        {"glBlendColor", reinterpret_cast<PROC>(glBlendColor)},
        {"glBlendColorEXT", reinterpret_cast<PROC>(glBlendColor)},
        {"glBlendColorARB", reinterpret_cast<PROC>(glBlendColor)},
        {"glClampColor", reinterpret_cast<PROC>(glClampColor)},
        {"glClampColorARB", reinterpret_cast<PROC>(glClampColor)},
        {"glProvokingVertex", reinterpret_cast<PROC>(glProvokingVertex)},
        {"glProvokingVertexEXT", reinterpret_cast<PROC>(glProvokingVertex)},
        {"glClearBufferiv", reinterpret_cast<PROC>(glClearBufferiv)},
        {"glClearBufferuiv", reinterpret_cast<PROC>(glClearBufferuiv)},
        {"glClearBufferfi", reinterpret_cast<PROC>(glClearBufferfi)},
        {"glSamplerParameterIiv", reinterpret_cast<PROC>(glSamplerParameterIiv)},
        {"glSamplerParameterIuiv", reinterpret_cast<PROC>(glSamplerParameterIuiv)},
        {"glGetSamplerParameterIiv", reinterpret_cast<PROC>(glGetSamplerParameterIiv)},
        {"glGetSamplerParameterIuiv", reinterpret_cast<PROC>(glGetSamplerParameterIuiv)},
        {"glBeginQuery", reinterpret_cast<PROC>(glBeginQuery)},
        {"glBeginQueryARB", reinterpret_cast<PROC>(glBeginQuery)},
        {"glEndQuery", reinterpret_cast<PROC>(glEndQuery)},
        {"glEndQueryARB", reinterpret_cast<PROC>(glEndQuery)},
        {"glGetQueryiv", reinterpret_cast<PROC>(glGetQueryiv)},
        {"glGetQueryivARB", reinterpret_cast<PROC>(glGetQueryiv)},
        {"glIsQuery", reinterpret_cast<PROC>(glIsQuery)},
        {"glIsQueryARB", reinterpret_cast<PROC>(glIsQuery)},
        {"glGetBufferPointerv", reinterpret_cast<PROC>(glGetBufferPointerv)},
        {"glGetBufferPointervARB", reinterpret_cast<PROC>(glGetBufferPointerv)},
        {"glGetInteger64i_v", reinterpret_cast<PROC>(glGetInteger64i_v)},
        {"glGetSynciv", reinterpret_cast<PROC>(glGetSynciv)},
        {"glValidateProgram", reinterpret_cast<PROC>(glValidateProgram)},
        {"glDetachShader", reinterpret_cast<PROC>(glDetachShader)},
        {"glGetBooleani_v", reinterpret_cast<PROC>(glGetBooleani_v)},
        {"glIsEnabledi", reinterpret_cast<PROC>(glIsEnabledi)},
        {"glDrawRangeElementsBaseVertex", reinterpret_cast<PROC>(glDrawRangeElementsBaseVertex)},
        {"glMultiDrawElements", reinterpret_cast<PROC>(glMultiDrawElements)},
        {"glPrimitiveRestartIndex", reinterpret_cast<PROC>(glPrimitiveRestartIndex)},
    };
    for (const Entry& e : entries) {
        if (std::strcmp(e.name, name) == 0) return e.fn;
    }

    // Do not return ABI-unsafe generic stubs for unknown functions. LWJGL may call a
    // resolved pointer immediately; a variadic-looking stub can corrupt registers or
    // the stack because OpenGL functions have different signatures. Returning nullptr
    // makes the loader treat the optional entry point as unavailable instead of crashing.
    Log("[wglGetProcAddress -> MISSING] %s", name);
    return nullptr;
}

} // extern "C"

namespace {

void* WINAPI CreateContextAttribsArb(HDC, HGLRC, const int* attribs) {
    constexpr int kMajor = 0x2091, kMinor = 0x2092, kFlags = 0x2094, kProfile = 0x9126;
    GlCreateContextPayload request = {};
    uint32_t major = 1, minor = 0;
    for (const int* a = attribs; a && a[0] != 0; a += 2) {
        switch (a[0]) {
            case kMajor:   major = static_cast<uint32_t>(a[1]); break;
            case kMinor:   minor = static_cast<uint32_t>(a[1]); break;
            case kFlags:   request.flags = static_cast<uint32_t>(a[1]); break;
            case kProfile: request.profile_mask = static_cast<uint32_t>(a[1]); break;
            default: break;
        }
    }
    Log("[WGL] wglCreateContextAttribsARB: requested version %u.%u, flags: 0x%X, profile: 0x%X",
        major, minor, request.flags, request.profile_mask);
    if (major >= 3) { request.major = major; request.minor = minor; }
    return CreateContextInternal(request);
}

const char* WINAPI GetExtensionsStringArb(HDC) {
    return "WGL_ARB_create_context WGL_ARB_create_context_profile WGL_ARB_pixel_format "
           "WGL_ARB_extensions_string WGL_EXT_swap_control WGL_ARB_multisample";
}

BOOL WINAPI SwapIntervalExt(int interval) {
    Log("[WGL] wglSwapIntervalEXT(%d)", interval);
    return TRUE;
}

int WINAPI GetSwapIntervalExt() { return 1; }

BOOL WINAPI wglChoosePixelFormatARB(HDC, const int*, const FLOAT*, UINT nMaxFormats, int* piFormats, UINT* nNumFormats) {
    if (!piFormats || !nNumFormats || nMaxFormats == 0) return FALSE;
    piFormats[0] = kPixelFormat;
    *nNumFormats = 1;
    return TRUE;
}

BOOL WINAPI wglGetPixelFormatAttribivARB(HDC, int, int, UINT nAttributes, const int* piAttributes, int* piValues) {
    if (!piAttributes || !piValues) return FALSE;
    for (UINT i = 0; i < nAttributes; ++i) {
        switch (piAttributes[i]) {
            case 0x2001: piValues[i] = 1; break; // WGL_DRAW_TO_WINDOW_ARB
            case 0x2003: piValues[i] = 0x2027; break; // WGL_ACCELERATION_ARB = WGL_FULL_ACCELERATION_ARB
            case 0x2010: piValues[i] = 1; break; // WGL_SUPPORT_OPENGL_ARB
            case 0x2011: piValues[i] = 1; break; // WGL_DOUBLE_BUFFER_ARB
            case 0x2012: piValues[i] = 0; break; // WGL_STEREO_ARB
            case 0x2013: piValues[i] = 0x202B; break; // WGL_PIXEL_TYPE_ARB = WGL_TYPE_RGBA_ARB
            case 0x2014: piValues[i] = 32; break; // WGL_COLOR_BITS_ARB
            case 0x2015: piValues[i] = 8; break;  // WGL_RED_BITS_ARB
            case 0x2017: piValues[i] = 8; break;  // WGL_GREEN_BITS_ARB
            case 0x2019: piValues[i] = 8; break;  // WGL_BLUE_BITS_ARB
            case 0x201B: piValues[i] = 8; break;  // WGL_ALPHA_BITS_ARB
            case 0x2022: piValues[i] = 24; break; // WGL_DEPTH_BITS_ARB
            case 0x2023: piValues[i] = 8; break;  // WGL_STENCIL_BITS_ARB
            case 0x2042: piValues[i] = 0; break;  // WGL_SAMPLES_ARB
            default:     piValues[i] = 0; break;
        }
    }
    return TRUE;
}

BOOL WINAPI wglGetPixelFormatAttribfvARB(HDC hdc, int iPixelFormat, int iLayerPlane,
                                        UINT nAttributes, const int* piAttributes, FLOAT* pfValues) {
    std::vector<int> ivals(nAttributes);
    if (!wglGetPixelFormatAttribivARB(hdc, iPixelFormat, iLayerPlane, nAttributes, piAttributes, ivals.data())) {
        return FALSE;
    }
    for (UINT i = 0; i < nAttributes; ++i) pfValues[i] = static_cast<FLOAT>(ivals[i]);
    return TRUE;
}

} // namespace
