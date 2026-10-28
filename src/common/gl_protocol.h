#pragma once
#ifndef REMOTE_GPU_GL_PROTOCOL_H
#define REMOTE_GPU_GL_PROTOCOL_H

#include <cstdint>
#include <type_traits>

#include "protocol.h"

namespace RemoteGPU {

// Largest host framebuffer an OpenGL session may request, per side.
inline constexpr uint32_t GL_MAX_FRAMEBUFFER_DIM = 8192;

// Function ids carried by OP_GL_COMMANDS. Each command is a GlCommandHeader followed by
// `size` bytes of arguments whose layout is fixed per function.
enum GlFunc : uint16_t {
    GLF_Clear      = 1,  // GlClearArgs
    GLF_ClearColor = 2,  // GlClearColorArgs
    GLF_Viewport   = 3,  // GlViewportArgs
    GLF_Flush      = 4,  // no arguments
    GLF_Finish     = 5,  // no arguments
    GLF_Begin      = 6,  // GlBeginArgs
    GLF_End        = 7,  // no arguments
    GLF_Color3f    = 8,  // GlColor3fArgs
    GLF_Vertex2f   = 9,  // GlVertex2fArgs
    GLF_GenBuffer   = 10, // GlIdArgs
    GLF_BindBuffer  = 11, // GlBindBufferArgs
    GLF_BufferData  = 12, // GlBufferDataArgs + bytes
    GLF_DeleteBuffer = 13, // GlIdArgs
    GLF_CreateShader = 14, // GlCreateShaderArgs
    GLF_CreateProgram = 15, // GlIdArgs
    GLF_ShaderSource = 16, // GlShaderSourceArgs + source
    GLF_CompileShader = 17, // GlIdArgs
    GLF_AttachShader = 18, // GlAttachShaderArgs
    GLF_LinkProgram = 19, // GlIdArgs
    GLF_UseProgram = 20, // GlIdArgs
    GLF_DrawArrays = 21 // GlDrawArraysArgs
};

#pragma pack(push, 1)

struct GlCreateContextPayload {
    uint32_t major;         // 0 = legacy context (highest compatibility profile the driver offers)
    uint32_t minor;
    uint32_t profile_mask;  // WGL_CONTEXT_*_PROFILE_BIT_ARB
    uint32_t flags;         // WGL_CONTEXT_*_BIT_ARB
    uint32_t width;
    uint32_t height;
};

struct GlContextResponsePayload {
    uint32_t status;        // 0: OK, 1: invalid size, 2: context creation failed
    uint32_t major;         // Version of the context that was actually created
    uint32_t minor;
    char     renderer[128]; // GL_RENDERER of the server GPU
};

struct GlResizePayload {
    uint32_t width;
    uint32_t height;
};

struct GlSwapPayload {
    uint64_t frame_number;
};

struct GlCommandHeader {
    uint16_t func;          // GlFunc
    uint16_t reserved;      // Must be zero
    uint32_t size;          // Argument bytes following this header
};

struct GlClearArgs      { uint32_t mask; };
struct GlClearColorArgs { float r, g, b, a; };
struct GlViewportArgs   { int32_t x, y, width, height; };
struct GlBeginArgs      { uint32_t mode; };
struct GlColor3fArgs    { float r, g, b; };
struct GlVertex2fArgs   { float x, y; };
struct GlIdArgs          { uint32_t id; };
struct GlBindBufferArgs  { uint32_t target, id; };
struct GlBufferDataArgs  { uint32_t target, usage; uint64_t size; };
struct GlCreateShaderArgs { uint32_t type, id; };
struct GlShaderSourceArgs { uint32_t id; uint32_t length; };
struct GlAttachShaderArgs { uint32_t program, shader; };
struct GlDrawArraysArgs  { uint32_t mode, first, count; };

#pragma pack(pop)

static_assert(sizeof(GlCreateContextPayload) == 24);
static_assert(sizeof(GlContextResponsePayload) == 140);
static_assert(sizeof(GlResizePayload) == 8);
static_assert(sizeof(GlSwapPayload) == 8);
static_assert(sizeof(GlCommandHeader) == 8);
static_assert(sizeof(GlClearArgs) == 4);
static_assert(sizeof(GlClearColorArgs) == 16);
static_assert(sizeof(GlViewportArgs) == 16);
static_assert(sizeof(GlBeginArgs) == 4);
static_assert(sizeof(GlColor3fArgs) == 12);
static_assert(sizeof(GlVertex2fArgs) == 8);
static_assert(sizeof(GlIdArgs) == 4);
static_assert(sizeof(GlBindBufferArgs) == 8);
static_assert(sizeof(GlBufferDataArgs) == 16);
static_assert(sizeof(GlCreateShaderArgs) == 8);
static_assert(sizeof(GlShaderSourceArgs) == 8);
static_assert(sizeof(GlAttachShaderArgs) == 8);
static_assert(sizeof(GlDrawArraysArgs) == 12);

} // namespace RemoteGPU

#endif // REMOTE_GPU_GL_PROTOCOL_H
