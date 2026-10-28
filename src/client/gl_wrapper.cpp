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
    if (count <= 5 || (count % 250) == 0) {
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

void WINAPI glGetTexLevelParameteriv(unsigned, int, unsigned pname, int* params) {
    if (!params) return;
    Log("[GL] glGetTexLevelParameteriv(0x%X)", pname);
    *params = 0;
}

void WINAPI glPixelStorei(unsigned pname, int param) {
    Log("[GL] glPixelStorei(0x%X, %d)", pname, param);
}

void WINAPI glGetIntegeri_v(unsigned pname, unsigned index, int* params) {
    if (!params) return;
    Log("[GL] glGetIntegeri_v(0x%X, %u)", pname, index);
    glGetIntegerv(pname, params);
}

void WINAPI glGetBufferParameteri64v(unsigned, unsigned, long long* params) {
    if (params) *params = 0;
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
}

void WINAPI glBindRenderbuffer(unsigned target, unsigned renderbuffer) {
    Log("[GL] glBindRenderbuffer(0x%X, %u)", target, renderbuffer);
}

void WINAPI glFramebufferTexture2D(unsigned, unsigned, unsigned, unsigned, int) {}
void WINAPI glFramebufferRenderbuffer(unsigned, unsigned, unsigned, unsigned) {}
void WINAPI glGenFramebuffers(int count, unsigned* ids) {
    static unsigned next_id = 1;
    if (!ids || count < 0) return;
    for (int i = 0; i < count; ++i) ids[i] = next_id++;
}
void WINAPI glDeleteFramebuffers(int, const unsigned*) {}

void WINAPI glGetBooleanv(unsigned pname, unsigned char* params) {
    if (!params) return;
    switch (pname) {
        case 0x0C10: *params = 1; break; // GL_DOUBLEBUFFER
        case 0x0C11: *params = 0; break; // GL_STEREO
        default:     *params = 0; break;
    }
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

void WINAPI glEnable(unsigned cap) { Log("[GL] glEnable(0x%X)", cap); }
void WINAPI glDisable(unsigned cap) { Log("[GL] glDisable(0x%X)", cap); }
unsigned char WINAPI glIsEnabled(unsigned) { return 1; }

// --- Textures & VAO Stubs/Trackers --------------------------------------------------------------
static unsigned g_nextTextureId = 1;
static unsigned g_nextVaoId = 1;
static unsigned g_nextUniformLoc = 1;

void WINAPI glGenTextures(int count, unsigned* textures) {
    Log("[GL] glGenTextures(%d)", count);
    if (!textures || count < 0) return;
    for (int i = 0; i < count; ++i) textures[i] = g_nextTextureId++;
}

void WINAPI glBindTexture(unsigned target, unsigned texture) {
    Log("[GL] glBindTexture(target: 0x%X, id: %u)", target, texture);
}

void WINAPI glDeleteTextures(int count, const unsigned*) {
    Log("[GL] glDeleteTextures(%d)", count);
}

void WINAPI glTexParameteri(unsigned target, unsigned pname, int param) {
    Log("[GL] glTexParameteri(0x%X, 0x%X, %d)", target, pname, param);
}

void WINAPI glTexImage2D(unsigned target, int level, int internalformat, int width, int height,
                         int border, unsigned format, unsigned type, const void* pixels) {
    Log("[GL] glTexImage2D(0x%X, level: %d, internal: %d, %dx%d, pixels: %p)",
        target, level, internalformat, width, height, pixels);
}

void WINAPI glActiveTexture(unsigned texture) {
    Log("[GL] glActiveTexture(0x%X)", texture);
}

void WINAPI glGenVertexArrays(int count, unsigned* arrays) {
    Log("[GL] glGenVertexArrays(%d)", count);
    if (!arrays || count < 0) return;
    for (int i = 0; i < count; ++i) arrays[i] = g_nextVaoId++;
}

void WINAPI glBindVertexArray(unsigned array) {
    Log("[GL] glBindVertexArray(%u)", array);
}

void WINAPI glDeleteVertexArrays(int count, const unsigned*) {
    Log("[GL] glDeleteVertexArrays(%d)", count);
}

void WINAPI glVertexAttribPointer(unsigned index, int size, unsigned type, unsigned char normalized,
                                  int stride, const void* pointer) {
    Log("[GL] glVertexAttribPointer(idx: %u, size: %d, type: 0x%X, norm: %u, stride: %d, ptr: %p)",
        index, size, type, normalized, stride, pointer);
}

void WINAPI glEnableVertexAttribArray(unsigned index) {
    Log("[GL] glEnableVertexAttribArray(%u)", index);
}

void WINAPI glDisableVertexAttribArray(unsigned index) {
    Log("[GL] glDisableVertexAttribArray(%u)", index);
}

int WINAPI glGetUniformLocation(unsigned program, const char* name) {
    Log("[GL] glGetUniformLocation(prog: %u, name: %s)", program, name ? name : "null");
    return static_cast<int>(g_nextUniformLoc++);
}

int WINAPI glGetAttribLocation(unsigned program, const char* name) {
    Log("[GL] glGetAttribLocation(prog: %u, name: %s)", program, name ? name : "null");
    return 0;
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
}

void WINAPI glDepthFunc(unsigned func) { Log("[GL] glDepthFunc(0x%X)", func); }
void WINAPI glDepthMask(unsigned char flag) { Log("[GL] glDepthMask(%u)", flag); }
void WINAPI glCullFace(unsigned mode) { Log("[GL] glCullFace(0x%X)", mode); }

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
    if (!t_current) return;
    const GlBindBufferArgs args = {target, buffer};
    t_current->session.Append(GLF_BindBuffer, &args, sizeof(args));
}

void WINAPI glBufferData(unsigned target, ptrdiff_t size, const void* data, unsigned usage) {
    if (!t_current || size < 0 || static_cast<uint64_t>(size) > 64 * 1024 * 1024) return;
    const GlBufferDataArgs args = {target, usage, static_cast<uint64_t>(size)};
    std::vector<uint8_t> payload(sizeof(args) + static_cast<size_t>(size));
    std::memcpy(payload.data(), &args, sizeof(args));
    if (data && size) std::memcpy(payload.data() + sizeof(args), data, static_cast<size_t>(size));
    t_current->session.Append(GLF_BufferData, payload.data(), static_cast<uint32_t>(payload.size()));
}

void WINAPI glDeleteBuffers(int count, const unsigned* buffers) {
    if (!t_current || !buffers || count < 0 || count > 4096) return;
    for (int i = 0; i < count; ++i) {
        const GlIdArgs args = {buffers[i]};
        t_current->session.Append(GLF_DeleteBuffer, &args, sizeof(args));
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

void WINAPI glDrawArrays(unsigned mode, int first, int count) {
    if (!t_current || first < 0 || count < 0 || count > 1 << 20) return;
    const GlDrawArraysArgs args = {mode, static_cast<uint32_t>(first), static_cast<uint32_t>(count)};
    t_current->session.Append(GLF_DrawArrays, &args, sizeof(args));
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
        {"glGetInteger64v", reinterpret_cast<PROC>(glGetInteger64v)},
        {"glGetInternalformativ", reinterpret_cast<PROC>(glGetInternalformativ)},
        {"glGetTexLevelParameteriv", reinterpret_cast<PROC>(glGetTexLevelParameteriv)},
        {"glPixelStorei", reinterpret_cast<PROC>(glPixelStorei)},
        {"glGetIntegeri_v", reinterpret_cast<PROC>(glGetIntegeri_v)},
        {"glGetBufferParameteri64v", reinterpret_cast<PROC>(glGetBufferParameteri64v)},
        {"glGetVertexAttribiv", reinterpret_cast<PROC>(glGetVertexAttribiv)},
        {"glGetUniformiv", reinterpret_cast<PROC>(glGetUniformiv)},
        {"glGetUniformfv", reinterpret_cast<PROC>(glGetUniformfv)},
        {"glBindFramebuffer", reinterpret_cast<PROC>(glBindFramebuffer)},
        {"glBindRenderbuffer", reinterpret_cast<PROC>(glBindRenderbuffer)},
        {"glFramebufferTexture2D", reinterpret_cast<PROC>(glFramebufferTexture2D)},
        {"glFramebufferRenderbuffer", reinterpret_cast<PROC>(glFramebufferRenderbuffer)},
        {"glGenFramebuffers", reinterpret_cast<PROC>(glGenFramebuffers)},
        {"glDeleteFramebuffers", reinterpret_cast<PROC>(glDeleteFramebuffers)},
        {"glGetBooleanv", reinterpret_cast<PROC>(glGetBooleanv)},
        {"glGetError", reinterpret_cast<PROC>(glGetError)},
        {"glGetAttachedShaders", reinterpret_cast<PROC>(glGetAttachedShaders)},
        {"glGetActiveAttrib", reinterpret_cast<PROC>(glGetActiveAttrib)},
        {"glGetActiveUniform", reinterpret_cast<PROC>(glGetActiveUniform)},
        {"glGetShaderSource", reinterpret_cast<PROC>(glGetShaderSource)},
        {"glGetUniformLocationARB", reinterpret_cast<PROC>(glGetUniformLocationARB)},
        {"glGetDoublev", reinterpret_cast<PROC>(glGetDoublev)},
        {"glEnable", reinterpret_cast<PROC>(glEnable)},
        {"glDisable", reinterpret_cast<PROC>(glDisable)},
        {"glIsEnabled", reinterpret_cast<PROC>(glIsEnabled)},
        {"glClear", reinterpret_cast<PROC>(glClear)},
        {"glClearColor", reinterpret_cast<PROC>(glClearColor)},
        {"glViewport", reinterpret_cast<PROC>(glViewport)},
        {"glFlush", reinterpret_cast<PROC>(glFlush)},
        {"glFinish", reinterpret_cast<PROC>(glFinish)},
        {"glDrawArrays", reinterpret_cast<PROC>(glDrawArrays)},
        {"glGenBuffers", reinterpret_cast<PROC>(glGenBuffers)},
        {"glBindBuffer", reinterpret_cast<PROC>(glBindBuffer)},
        {"glBufferData", reinterpret_cast<PROC>(glBufferData)},
        {"glDeleteBuffers", reinterpret_cast<PROC>(glDeleteBuffers)},
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
        {"glTexImage2D", reinterpret_cast<PROC>(glTexImage2D)},
        {"glActiveTexture", reinterpret_cast<PROC>(glActiveTexture)},
        {"glGenVertexArrays", reinterpret_cast<PROC>(glGenVertexArrays)},
        {"glBindVertexArray", reinterpret_cast<PROC>(glBindVertexArray)},
        {"glDeleteVertexArrays", reinterpret_cast<PROC>(glDeleteVertexArrays)},
        {"glVertexAttribPointer", reinterpret_cast<PROC>(glVertexAttribPointer)},
        {"glEnableVertexAttribArray", reinterpret_cast<PROC>(glEnableVertexAttribArray)},
        {"glDisableVertexAttribArray", reinterpret_cast<PROC>(glDisableVertexAttribArray)},
        {"glGetUniformLocation", reinterpret_cast<PROC>(glGetUniformLocation)},
        {"glGetAttribLocation", reinterpret_cast<PROC>(glGetAttribLocation)},
        {"glGetShaderiv", reinterpret_cast<PROC>(glGetShaderiv)},
        {"glGetProgramiv", reinterpret_cast<PROC>(glGetProgramiv)},
        {"glGetShaderInfoLog", reinterpret_cast<PROC>(glGetShaderInfoLog)},
        {"glGetProgramInfoLog", reinterpret_cast<PROC>(glGetProgramInfoLog)},
        {"glCheckFramebufferStatus", reinterpret_cast<PROC>(glCheckFramebufferStatus)},
        {"glBlendFunc", reinterpret_cast<PROC>(glBlendFunc)},
        {"glDepthFunc", reinterpret_cast<PROC>(glDepthFunc)},
        {"glDepthMask", reinterpret_cast<PROC>(glDepthMask)},
        {"glCullFace", reinterpret_cast<PROC>(glCullFace)},
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
