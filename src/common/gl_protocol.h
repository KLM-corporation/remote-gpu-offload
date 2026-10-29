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
    GLF_DrawArrays = 21, // GlDrawArraysArgs

    // Textures & Samplers
    GLF_GenTextures = 22,       // GlGenArgs + uint32_t ids
    GLF_BindTexture = 23,       // GlBindTextureArgs
    GLF_DeleteTextures = 24,    // GlGenArgs + uint32_t ids
    GLF_TexParameteri = 25,     // GlTexParameteriArgs
    GLF_TexImage2D = 26,        // GlTexImage2DArgs + pixel bytes
    GLF_TexSubImage2D = 27,     // GlTexSubImage2DArgs + pixel bytes
    GLF_ActiveTexture = 28,     // GlIdArgs
    GLF_GenerateMipmap = 29,    // GlIdArgs
    GLF_GenSamplers = 30,       // GlGenArgs + uint32_t ids
    GLF_BindSampler = 31,       // GlBindSamplerArgs
    GLF_DeleteSamplers = 32,    // GlGenArgs + uint32_t ids
    GLF_SamplerParameteri = 33, // GlSamplerParameteriArgs

    // Framebuffers
    GLF_GenFramebuffers = 34,        // GlGenArgs + uint32_t ids
    GLF_BindFramebuffer = 35,        // GlBindFramebufferArgs
    GLF_DeleteFramebuffers = 36,     // GlGenArgs + uint32_t ids
    GLF_FramebufferTexture2D = 37,   // GlFramebufferTexture2DArgs
    GLF_BlitFramebuffer = 38,        // GlBlitFramebufferArgs
    GLF_DrawBuffer = 39,             // GlIdArgs
    GLF_DrawBuffers = 40,            // GlDrawBuffersArgs + uint32_t buffers

    // Vertex Array Objects & Attributes
    GLF_GenVertexArrays = 41,        // GlGenArgs + uint32_t ids
    GLF_BindVertexArray = 42,        // GlIdArgs
    GLF_DeleteVertexArrays = 43,     // GlGenArgs + uint32_t ids
    GLF_EnableVertexAttribArray = 44, // GlIdArgs
    GLF_DisableVertexAttribArray = 45,// GlIdArgs
    GLF_VertexAttribPointer = 46,    // GlVertexAttribPointerArgs
    GLF_VertexAttribDivisor = 47,    // GlVertexAttribDivisorArgs

    // Buffer range / UBO
    GLF_BindBufferRange = 48,        // GlBindBufferRangeArgs
    GLF_BindBufferBase = 49,         // GlBindBufferBaseArgs
    GLF_BufferSubData = 50,          // GlBufferSubDataArgs + bytes

    // Extended Draw Calls
    GLF_DrawElements = 51,           // GlDrawElementsArgs
    GLF_DrawArraysInstanced = 52,    // GlDrawArraysInstancedArgs
    GLF_DrawElementsInstancedBaseVertex = 53, // GlDrawElementsInstancedBaseVertexArgs

    // Pipeline & Raster State
    GLF_Enable = 54,                 // GlIdArgs
    GLF_Disable = 55,                // GlIdArgs
    GLF_Scissor = 56,                // GlViewportArgs
    GLF_BlendFuncSeparate = 57,      // GlBlendFuncSeparateArgs
    GLF_DepthFunc = 58,              // GlIdArgs
    GLF_DepthMask = 59,              // GlIdArgs
    GLF_ColorMask = 60,              // GlColorMaskArgs
    GLF_PolygonMode = 61,            // GlPolygonModeArgs
    GLF_CopyBufferSubData = 62,      // GlCopyBufferSubDataArgs

    // Shaders, Uniforms, Renderbuffers & Extended State
    GLF_PixelStorei = 63,            // GlPixelStoreiArgs
    GLF_BindAttribLocation = 64,     // GlBindAttribLocationArgs + name
    GLF_UniformBlockBinding = 65,    // GlUniformBlockBindingArgs + name
    GLF_RegisterUniformLocation = 66,// GlRegisterUniformLocationArgs + name
    GLF_Uniform = 67,                // GlUniformArgs + data
    GLF_BlendEquationSeparate = 68,  // GlBlendEquationSeparateArgs
    GLF_CullFace = 69,               // GlIdArgs
    GLF_FrontFace = 70,              // GlIdArgs
    GLF_PolygonOffset = 71,          // GlPolygonOffsetArgs
    GLF_ClearDepth = 72,             // GlClearDepthArgs
    GLF_ReadBuffer = 73,             // GlIdArgs
    GLF_GenRenderbuffers = 74,       // GlGenArgs + uint32_t ids
    GLF_BindRenderbuffer = 75,       // GlBindRenderbufferArgs
    GLF_DeleteRenderbuffers = 76,    // GlGenArgs + uint32_t ids
    GLF_RenderbufferStorage = 77,    // GlRenderbufferStorageArgs
    GLF_FramebufferRenderbuffer = 78,// GlFramebufferRenderbufferArgs
    GLF_TexImage3D = 79,             // GlTexImage3DArgs + pixel bytes
    GLF_TexSubImage3D = 80,          // GlTexSubImage3DArgs + pixel bytes

    // Clip Control & Indirect Draw
    GLF_ClipControl = 81,            // GlClipControlArgs
    GLF_DrawArraysIndirect = 82,     // GlDrawArraysIndirectArgs
    GLF_DrawElementsIndirect = 83,   // GlDrawElementsIndirectArgs
    GLF_MultiDrawArraysIndirect = 84,// GlMultiDrawArraysIndirectArgs
    GLF_MultiDrawElementsIndirect = 85, // GlMultiDrawElementsIndirectArgs
    GLF_DrawArraysInstancedBaseInstance = 86, // GlDrawArraysInstancedBaseInstanceArgs
    GLF_DrawElementsInstancedBaseVertexBaseInstance = 87 // GlDrawElementsInstancedBaseVertexBaseInstanceArgs
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

struct GlGenArgs { uint32_t count; };
struct GlBindTextureArgs { uint32_t target, id; };
struct GlTexParameteriArgs { uint32_t target, pname; int32_t param; };
struct GlTexImage2DArgs {
    uint32_t target;
    int32_t  level;
    int32_t  internalformat;
    int32_t  width;
    int32_t  height;
    int32_t  border;
    uint32_t format;
    uint32_t type;
    uint32_t data_size;
    uint32_t reserved;
    uint64_t pixel_offset;
};
struct GlTexSubImage2DArgs {
    uint32_t target;
    int32_t  level;
    int32_t  xoffset;
    int32_t  yoffset;
    int32_t  width;
    int32_t  height;
    uint32_t format;
    uint32_t type;
    uint32_t data_size;
    uint32_t reserved;
    uint64_t pixel_offset;
};
struct GlBindSamplerArgs { uint32_t unit, sampler; };
struct GlSamplerParameteriArgs { uint32_t sampler, pname; int32_t param; };

struct GlBindFramebufferArgs { uint32_t target, id; };
struct GlFramebufferTexture2DArgs { uint32_t target, attachment, textarget, texture; int32_t level; };
struct GlBlitFramebufferArgs {
    int32_t  srcX0, srcY0, srcX1, srcY1;
    int32_t  dstX0, dstY0, dstX1, dstY1;
    uint32_t mask;
    uint32_t filter;
};
struct GlDrawBuffersArgs { uint32_t n; };

struct GlVertexAttribPointerArgs {
    uint32_t index;
    int32_t  size;
    uint32_t type;
    uint8_t  normalized;
    uint8_t  reserved[3];
    int32_t  stride;
    uint64_t pointer;
};
struct GlVertexAttribDivisorArgs { uint32_t index, divisor; };

struct GlBindBufferRangeArgs {
    uint32_t target;
    uint32_t index;
    uint32_t buffer;
    uint32_t reserved;
    uint64_t offset;
    uint64_t size;
};
struct GlBindBufferBaseArgs { uint32_t target, index, buffer; };
struct GlBufferSubDataArgs {
    uint32_t target;
    uint32_t reserved;
    uint64_t offset;
    uint64_t size;
};

struct GlDrawElementsArgs { uint32_t mode; int32_t count; uint32_t type; uint32_t reserved; uint64_t indices; };
struct GlDrawArraysInstancedArgs { uint32_t mode; int32_t first, count, instancecount; };
struct GlDrawElementsInstancedBaseVertexArgs {
    uint32_t mode;
    int32_t  count;
    uint32_t type;
    uint32_t reserved;
    uint64_t indices;
    int32_t  instancecount;
    int32_t  basevertex;
};

struct GlBlendFuncSeparateArgs { uint32_t srcRGB, dstRGB, srcAlpha, dstAlpha; };
struct GlColorMaskArgs { uint8_t r, g, b, a; };
struct GlPolygonModeArgs { uint32_t face, mode; };
struct GlCopyBufferSubDataArgs {
    uint32_t readTarget;
    uint32_t writeTarget;
    uint64_t readOffset;
    uint64_t writeOffset;
    uint64_t size;
};

struct GlPixelStoreiArgs { uint32_t pname; int32_t param; };
struct GlBindAttribLocationArgs { uint32_t program, index, name_len; };
struct GlUniformBlockBindingArgs { uint32_t program, block_index, binding, name_len; };
struct GlRegisterUniformLocationArgs { uint32_t program; int32_t client_loc; uint32_t name_len; };
// type: 0=float 1=int 2=uint; comps: 1..4 (vector size) or matrix dim 2..4 when matrix != 0
struct GlUniformArgs {
    int32_t  client_loc;
    int32_t  count;
    uint8_t  type;
    uint8_t  comps;
    uint8_t  matrix;
    uint8_t  transpose;
    uint32_t data_size;
};
struct GlBlendEquationSeparateArgs { uint32_t modeRGB, modeAlpha; };
struct GlPolygonOffsetArgs { float factor, units; };
struct GlClearDepthArgs { double depth; };
struct GlBindRenderbufferArgs { uint32_t target, id; };
struct GlRenderbufferStorageArgs { uint32_t target, internalformat; int32_t width, height; };
struct GlFramebufferRenderbufferArgs { uint32_t target, attachment, rbtarget, renderbuffer; };
struct GlTexImage3DArgs {
    uint32_t target;
    int32_t  level, internalformat, width, height, depth, border;
    uint32_t format, type, data_size, reserved;
    uint64_t pixel_offset;
};
struct GlTexSubImage3DArgs {
    uint32_t target;
    int32_t  level, xoffset, yoffset, zoffset, width, height, depth;
    uint32_t format, type, data_size, reserved;
    uint64_t pixel_offset;
};

struct GlClipControlArgs {
    uint32_t origin;
    uint32_t depth;
};

struct GlDrawArraysIndirectArgs {
    uint32_t mode;
    uint32_t reserved;
    uint64_t indirect;
};

struct GlDrawElementsIndirectArgs {
    uint32_t mode;
    uint32_t type;
    uint64_t indirect;
};

struct GlMultiDrawArraysIndirectArgs {
    uint32_t mode;
    int32_t  drawcount;
    int32_t  stride;
    uint32_t reserved;
    uint64_t indirect;
};

struct GlMultiDrawElementsIndirectArgs {
    uint32_t mode;
    uint32_t type;
    int32_t  drawcount;
    int32_t  stride;
    uint64_t indirect;
};

struct GlDrawArraysInstancedBaseInstanceArgs {
    uint32_t mode;
    int32_t  first;
    int32_t  count;
    int32_t  instancecount;
    uint32_t baseinstance;
    uint32_t reserved;
};

struct GlDrawElementsInstancedBaseVertexBaseInstanceArgs {
    uint32_t mode;
    int32_t  count;
    uint32_t type;
    uint32_t reserved;
    uint64_t indices;
    int32_t  instancecount;
    int32_t  basevertex;
    uint32_t baseinstance;
    uint32_t reserved2;
};

#pragma pack(pop)

static_assert(sizeof(GlClipControlArgs) == 8);
static_assert(sizeof(GlDrawArraysIndirectArgs) == 16);
static_assert(sizeof(GlDrawElementsIndirectArgs) == 16);
static_assert(sizeof(GlMultiDrawArraysIndirectArgs) == 24);
static_assert(sizeof(GlMultiDrawElementsIndirectArgs) == 24);
static_assert(sizeof(GlDrawArraysInstancedBaseInstanceArgs) == 24);
static_assert(sizeof(GlDrawElementsInstancedBaseVertexBaseInstanceArgs) == 40);
static_assert(sizeof(GlUniformArgs) == 16);
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
static_assert(sizeof(GlGenArgs) == 4);
static_assert(sizeof(GlBindTextureArgs) == 8);
static_assert(sizeof(GlTexParameteriArgs) == 12);
static_assert(sizeof(GlTexImage2DArgs) == 48);
static_assert(sizeof(GlTexSubImage2DArgs) == 48);
static_assert(sizeof(GlBindSamplerArgs) == 8);
static_assert(sizeof(GlSamplerParameteriArgs) == 12);
static_assert(sizeof(GlBindFramebufferArgs) == 8);
static_assert(sizeof(GlFramebufferTexture2DArgs) == 20);
static_assert(sizeof(GlBlitFramebufferArgs) == 40);
static_assert(sizeof(GlDrawBuffersArgs) == 4);
static_assert(sizeof(GlVertexAttribPointerArgs) == 28);
static_assert(sizeof(GlVertexAttribDivisorArgs) == 8);
static_assert(sizeof(GlBindBufferRangeArgs) == 32);
static_assert(sizeof(GlBindBufferBaseArgs) == 12);
static_assert(sizeof(GlBufferSubDataArgs) == 24);
static_assert(sizeof(GlCopyBufferSubDataArgs) == 32);
static_assert(sizeof(GlDrawElementsArgs) == 24);
static_assert(sizeof(GlDrawArraysInstancedArgs) == 16);
static_assert(sizeof(GlDrawElementsInstancedBaseVertexArgs) == 32);
static_assert(sizeof(GlBlendFuncSeparateArgs) == 16);
static_assert(sizeof(GlColorMaskArgs) == 4);
static_assert(sizeof(GlPolygonModeArgs) == 8);

} // namespace RemoteGPU

#endif // REMOTE_GPU_GL_PROTOCOL_H
