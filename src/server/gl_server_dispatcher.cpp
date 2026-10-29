#include "gl_server_dispatcher.h"

#include <GL/gl.h>

#include <cstring>
#include <iostream>
#include <string>

#include "frame_codec.h"

namespace RemoteGPU {

using GLintptr = std::ptrdiff_t;
using GLsizeiptr = std::ptrdiff_t;

using GenBuffersFn = void(WINAPI*)(GLsizei, GLuint*);
using BindBufferFn = void(WINAPI*)(GLenum, GLuint);
using BufferDataFn = void(WINAPI*)(GLenum, std::ptrdiff_t, const void*, GLenum);
using BufferSubDataFn = void(WINAPI*)(GLenum, std::ptrdiff_t, std::ptrdiff_t, const void*);
using BindBufferRangeFn = void(WINAPI*)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr);
using BindBufferBaseFn = void(WINAPI*)(GLenum, GLuint, GLuint);
using DeleteBuffersFn = void(WINAPI*)(GLsizei, const GLuint*);
using CopyBufferSubDataFn = void(WINAPI*)(GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr);

using CreateShaderFn = GLuint(WINAPI*)(GLenum);
using ShaderSourceFn = void(WINAPI*)(GLuint, GLsizei, const char* const*, const GLint*);
using CompileShaderFn = void(WINAPI*)(GLuint);
using CreateProgramFn = GLuint(WINAPI*)();
using AttachShaderFn = void(WINAPI*)(GLuint, GLuint);
using LinkProgramFn = void(WINAPI*)(GLuint);
using UseProgramFn = void(WINAPI*)(GLuint);

using ActiveTextureFn = void(WINAPI*)(GLenum);
using GenerateMipmapFn = void(WINAPI*)(GLenum);

using GenSamplersFn = void(WINAPI*)(GLsizei, GLuint*);
using BindSamplerFn = void(WINAPI*)(GLuint, GLuint);
using DeleteSamplersFn = void(WINAPI*)(GLsizei, const GLuint*);
using SamplerParameteriFn = void(WINAPI*)(GLuint, GLenum, GLint);

using GenFramebuffersFn = void(WINAPI*)(GLsizei, GLuint*);
using BindFramebufferFn = void(WINAPI*)(GLenum, GLuint);
using DeleteFramebuffersFn = void(WINAPI*)(GLsizei, const GLuint*);
using FramebufferTexture2DFn = void(WINAPI*)(GLenum, GLenum, GLenum, GLuint, GLint);
using BlitFramebufferFn = void(WINAPI*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
using DrawBuffersFn = void(WINAPI*)(GLsizei, const GLenum*);

using GenVertexArraysFn = void(WINAPI*)(GLsizei, GLuint*);
using BindVertexArrayFn = void(WINAPI*)(GLuint);
using DeleteVertexArraysFn = void(WINAPI*)(GLsizei, const GLuint*);
using EnableVertexAttribArrayFn = void(WINAPI*)(GLuint);
using DisableVertexAttribArrayFn = void(WINAPI*)(GLuint);
using VertexAttribPointerFn = void(WINAPI*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
using VertexAttribDivisorFn = void(WINAPI*)(GLuint, GLuint);

using DrawElementsInstancedFn = void(WINAPI*)(GLenum, GLsizei, GLenum, const void*, GLsizei);
using DrawElementsInstancedBaseVertexFn = void(WINAPI*)(GLenum, GLsizei, GLenum, const void*, GLsizei, GLint);
using DrawArraysInstancedFn = void(WINAPI*)(GLenum, GLint, GLsizei, GLsizei);
using BlendFuncSeparateFn = void(WINAPI*)(GLenum, GLenum, GLenum, GLenum);

using BindAttribLocationFn = void(WINAPI*)(GLuint, GLuint, const char*);
using GetUniformBlockIndexFn = GLuint(WINAPI*)(GLuint, const char*);
using UniformBlockBindingFn = void(WINAPI*)(GLuint, GLuint, GLuint);
using GetUniformLocationFn = GLint(WINAPI*)(GLuint, const char*);
using UniformVFn = void(WINAPI*)(GLint, GLsizei, const void*);
using UniformMatrixFn = void(WINAPI*)(GLint, GLsizei, GLboolean, const void*);
using VertexAttribIPointerFn = void(WINAPI*)(GLuint, GLint, GLenum, GLsizei, const void*);
using BlendEquationSeparateFn = void(WINAPI*)(GLenum, GLenum);
using GenRenderbuffersFn = void(WINAPI*)(GLsizei, GLuint*);
using BindRenderbufferFn = void(WINAPI*)(GLenum, GLuint);
using DeleteRenderbuffersFn = void(WINAPI*)(GLsizei, const GLuint*);
using RenderbufferStorageFn = void(WINAPI*)(GLenum, GLenum, GLsizei, GLsizei);
using FramebufferRenderbufferFn = void(WINAPI*)(GLenum, GLenum, GLenum, GLuint);
using TexImage3DFn = void(WINAPI*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
using TexSubImage3DFn = void(WINAPI*)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void*);
using GetivFn = void(WINAPI*)(GLuint, GLenum, GLint*);
using GetInfoLogFn = void(WINAPI*)(GLuint, GLsizei, GLsizei*, char*);

using ClipControlFn = void(WINAPI*)(GLenum, GLenum);
using DrawArraysIndirectFn = void(WINAPI*)(GLenum, const void*);
using DrawElementsIndirectFn = void(WINAPI*)(GLenum, GLenum, const void*);
using MultiDrawArraysIndirectFn = void(WINAPI*)(GLenum, const void*, GLsizei, GLsizei);
using MultiDrawElementsIndirectFn = void(WINAPI*)(GLenum, GLenum, const void*, GLsizei, GLsizei);
using DrawArraysInstancedBaseInstanceFn = void(WINAPI*)(GLenum, GLint, GLsizei, GLsizei, GLuint);
using DrawElementsInstancedBaseVertexBaseInstanceFn = void(WINAPI*)(GLenum, GLsizei, GLenum, const void*, GLsizei, GLint, GLuint);

struct GlDispatchApi {
    BindAttribLocationFn bindAttribLocation = nullptr;
    GetUniformBlockIndexFn getUniformBlockIndex = nullptr;
    UniformBlockBindingFn uniformBlockBinding = nullptr;
    GetUniformLocationFn getUniformLocation = nullptr;
    UniformVFn uniformV[3][4] = {};
    UniformMatrixFn uniformMatrix[3] = {};
    VertexAttribIPointerFn vertexAttribIPointer = nullptr;
    BlendEquationSeparateFn blendEquationSeparate = nullptr;
    GenRenderbuffersFn genRenderbuffers = nullptr;
    BindRenderbufferFn bindRenderbuffer = nullptr;
    DeleteRenderbuffersFn deleteRenderbuffers = nullptr;
    RenderbufferStorageFn renderbufferStorage = nullptr;
    FramebufferRenderbufferFn framebufferRenderbuffer = nullptr;
    TexImage3DFn texImage3D = nullptr;
    TexSubImage3DFn texSubImage3D = nullptr;
    GetivFn getShaderiv = nullptr;
    GetivFn getProgramiv = nullptr;
    GetInfoLogFn getShaderInfoLog = nullptr;
    GetInfoLogFn getProgramInfoLog = nullptr;

    ClipControlFn clipControl = nullptr;
    DrawArraysIndirectFn drawArraysIndirect = nullptr;
    DrawElementsIndirectFn drawElementsIndirect = nullptr;
    MultiDrawArraysIndirectFn multiDrawArraysIndirect = nullptr;
    MultiDrawElementsIndirectFn multiDrawElementsIndirect = nullptr;
    DrawArraysInstancedBaseInstanceFn drawArraysInstancedBaseInstance = nullptr;
    DrawElementsInstancedBaseVertexBaseInstanceFn drawElementsInstancedBaseVertexBaseInstance = nullptr;

    GenBuffersFn genBuffers = nullptr;
    BindBufferFn bindBuffer = nullptr;
    BufferDataFn bufferData = nullptr;
    BufferSubDataFn bufferSubData = nullptr;
    BindBufferRangeFn bindBufferRange = nullptr;
    BindBufferBaseFn bindBufferBase = nullptr;
    DeleteBuffersFn deleteBuffers = nullptr;
    CopyBufferSubDataFn copyBufferSubData = nullptr;

    CreateShaderFn createShader = nullptr;
    ShaderSourceFn shaderSource = nullptr;
    CompileShaderFn compileShader = nullptr;
    CreateProgramFn createProgram = nullptr;
    AttachShaderFn attachShader = nullptr;
    LinkProgramFn linkProgram = nullptr;
    UseProgramFn useProgram = nullptr;

    ActiveTextureFn activeTexture = nullptr;
    GenerateMipmapFn generateMipmap = nullptr;

    GenSamplersFn genSamplers = nullptr;
    BindSamplerFn bindSampler = nullptr;
    DeleteSamplersFn deleteSamplers = nullptr;
    SamplerParameteriFn samplerParameteri = nullptr;

    GenFramebuffersFn genFramebuffers = nullptr;
    BindFramebufferFn bindFramebuffer = nullptr;
    DeleteFramebuffersFn deleteFramebuffers = nullptr;
    FramebufferTexture2DFn framebufferTexture2D = nullptr;
    BlitFramebufferFn blitFramebuffer = nullptr;
    DrawBuffersFn drawBuffers = nullptr;

    GenVertexArraysFn genVertexArrays = nullptr;
    BindVertexArrayFn bindVertexArray = nullptr;
    DeleteVertexArraysFn deleteVertexArrays = nullptr;
    EnableVertexAttribArrayFn enableVertexAttribArray = nullptr;
    DisableVertexAttribArrayFn disableVertexAttribArray = nullptr;
    VertexAttribPointerFn vertexAttribPointer = nullptr;
    VertexAttribDivisorFn vertexAttribDivisor = nullptr;

    DrawElementsInstancedFn drawElementsInstanced = nullptr;
    DrawElementsInstancedBaseVertexFn drawElementsInstancedBaseVertex = nullptr;
    DrawArraysInstancedFn drawArraysInstanced = nullptr;
    BlendFuncSeparateFn blendFuncSeparate = nullptr;
};

namespace {

constexpr uint32_t kClearMaskAll = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_ACCUM_BUFFER_BIT;
constexpr int32_t  kMaxViewportDim = 32768;
constexpr GLenum kArrayBuffer = 0x8892;
constexpr GLenum kElementArrayBuffer = 0x8893;
constexpr GLenum kUniformBuffer = 0x8A11;

inline bool IsValidBufferTarget(GLenum target) {
    return target == 0x8892 /*GL_ARRAY_BUFFER*/ ||
           target == 0x8893 /*GL_ELEMENT_ARRAY_BUFFER*/ ||
           target == 0x88EC /*GL_PIXEL_UNPACK_BUFFER*/ ||
           target == 0x88ED /*GL_PIXEL_PACK_BUFFER*/ ||
           target == 0x8A11 /*GL_UNIFORM_BUFFER*/ ||
           target == 0x8C2C /*GL_TEXTURE_BUFFER*/ ||
           target == 0x8C8E /*GL_TRANSFORM_FEEDBACK_BUFFER*/ ||
           target == 0x8F36 /*GL_COPY_READ_BUFFER*/ ||
           target == 0x8F37 /*GL_COPY_WRITE_BUFFER*/ ||
           target == 0x8F3F /*GL_DRAW_INDIRECT_BUFFER*/ ||
           target == 0x90D2 /*GL_SHADER_STORAGE_BUFFER*/;
}

// Client FBO 0 is the host offscreen FBO, where the default-framebuffer enums are invalid.
inline GLenum TranslateDrawBuffer(GLenum mode, bool isDefault) {
    if (isDefault && mode >= 0x0400 && mode <= 0x0408) return 0x8CE0; // GL_COLOR_ATTACHMENT0
    return mode;
}

template <typename T> bool LoadGl(T& fn, const char* name) {
    fn = reinterpret_cast<T>(reinterpret_cast<void*>(wglGetProcAddress(name)));
    return fn != nullptr;
}

bool ValidSize(uint32_t width, uint32_t height) {
    return width >= 1 && height >= 1 && width <= GL_MAX_FRAMEBUFFER_DIM && height <= GL_MAX_FRAMEBUFFER_DIM;
}

template <typename T>
T ReadArgs(const uint8_t* p) {
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}

bool SendReply(NetworkSocket& socket, uint16_t opcode, uint32_t sequence, const void* payload, uint32_t size) {
    PacketHeader h = {};
    h.magic = RGPU_MAGIC;
    h.opcode = opcode;
    h.sequence_id = sequence;
    h.payload_length = size;
    return socket.SendPacket(h, payload);
}

} // namespace

GlServerDispatcher::GlServerDispatcher() : m_gl(std::make_unique<GlDispatchApi>()) {}
GlServerDispatcher::~GlServerDispatcher() = default;
GlServerDispatcher::GlServerDispatcher(GlServerDispatcher&&) noexcept = default;
GlServerDispatcher& GlServerDispatcher::operator=(GlServerDispatcher&&) noexcept = default;

bool GlServerDispatcher::LoadGlApi() {
    if (m_gl && m_gl->genBuffers) return true;
    if (!m_gl) m_gl = std::make_unique<GlDispatchApi>();
    bool base = LoadGl(m_gl->genBuffers, "glGenBuffers") && LoadGl(m_gl->bindBuffer, "glBindBuffer") &&
                LoadGl(m_gl->bufferData, "glBufferData") && LoadGl(m_gl->createShader, "glCreateShader") &&
                LoadGl(m_gl->shaderSource, "glShaderSource") && LoadGl(m_gl->compileShader, "glCompileShader") &&
                LoadGl(m_gl->createProgram, "glCreateProgram") && LoadGl(m_gl->attachShader, "glAttachShader") &&
                LoadGl(m_gl->linkProgram, "glLinkProgram") && LoadGl(m_gl->useProgram, "glUseProgram");
    if (!base) return false;

    LoadGl(m_gl->bufferSubData, "glBufferSubData");
    LoadGl(m_gl->copyBufferSubData, "glCopyBufferSubData");
    LoadGl(m_gl->bindBufferRange, "glBindBufferRange");
    LoadGl(m_gl->bindBufferBase, "glBindBufferBase");
    LoadGl(m_gl->deleteBuffers, "glDeleteBuffers");

    LoadGl(m_gl->activeTexture, "glActiveTexture");
    LoadGl(m_gl->generateMipmap, "glGenerateMipmap");

    LoadGl(m_gl->genSamplers, "glGenSamplers");
    LoadGl(m_gl->bindSampler, "glBindSampler");
    LoadGl(m_gl->deleteSamplers, "glDeleteSamplers");
    LoadGl(m_gl->samplerParameteri, "glSamplerParameteri");

    LoadGl(m_gl->genFramebuffers, "glGenFramebuffers");
    LoadGl(m_gl->bindFramebuffer, "glBindFramebuffer");
    LoadGl(m_gl->deleteFramebuffers, "glDeleteFramebuffers");
    LoadGl(m_gl->framebufferTexture2D, "glFramebufferTexture2D");
    LoadGl(m_gl->blitFramebuffer, "glBlitFramebuffer");
    LoadGl(m_gl->drawBuffers, "glDrawBuffers");

    LoadGl(m_gl->genVertexArrays, "glGenVertexArrays");
    LoadGl(m_gl->bindVertexArray, "glBindVertexArray");
    LoadGl(m_gl->deleteVertexArrays, "glDeleteVertexArrays");
    LoadGl(m_gl->enableVertexAttribArray, "glEnableVertexAttribArray");
    LoadGl(m_gl->disableVertexAttribArray, "glDisableVertexAttribArray");
    LoadGl(m_gl->vertexAttribPointer, "glVertexAttribPointer");
    LoadGl(m_gl->vertexAttribDivisor, "glVertexAttribDivisor");

    LoadGl(m_gl->drawElementsInstanced, "glDrawElementsInstanced");
    LoadGl(m_gl->drawElementsInstancedBaseVertex, "glDrawElementsInstancedBaseVertex");
    LoadGl(m_gl->drawArraysInstanced, "glDrawArraysInstanced");
    LoadGl(m_gl->blendFuncSeparate, "glBlendFuncSeparate");

    LoadGl(m_gl->bindAttribLocation, "glBindAttribLocation");
    LoadGl(m_gl->getUniformBlockIndex, "glGetUniformBlockIndex");
    LoadGl(m_gl->uniformBlockBinding, "glUniformBlockBinding");
    LoadGl(m_gl->getUniformLocation, "glGetUniformLocation");
    LoadGl(m_gl->uniformV[0][0], "glUniform1fv");
    LoadGl(m_gl->uniformV[0][1], "glUniform2fv");
    LoadGl(m_gl->uniformV[0][2], "glUniform3fv");
    LoadGl(m_gl->uniformV[0][3], "glUniform4fv");
    LoadGl(m_gl->uniformV[1][0], "glUniform1iv");
    LoadGl(m_gl->uniformV[1][1], "glUniform2iv");
    LoadGl(m_gl->uniformV[1][2], "glUniform3iv");
    LoadGl(m_gl->uniformV[1][3], "glUniform4iv");
    LoadGl(m_gl->uniformV[2][0], "glUniform1uiv");
    LoadGl(m_gl->uniformV[2][1], "glUniform2uiv");
    LoadGl(m_gl->uniformV[2][2], "glUniform3uiv");
    LoadGl(m_gl->uniformV[2][3], "glUniform4uiv");
    LoadGl(m_gl->uniformMatrix[0], "glUniformMatrix2fv");
    LoadGl(m_gl->uniformMatrix[1], "glUniformMatrix3fv");
    LoadGl(m_gl->uniformMatrix[2], "glUniformMatrix4fv");
    LoadGl(m_gl->vertexAttribIPointer, "glVertexAttribIPointer");
    LoadGl(m_gl->blendEquationSeparate, "glBlendEquationSeparate");
    LoadGl(m_gl->genRenderbuffers, "glGenRenderbuffers");
    LoadGl(m_gl->bindRenderbuffer, "glBindRenderbuffer");
    LoadGl(m_gl->deleteRenderbuffers, "glDeleteRenderbuffers");
    LoadGl(m_gl->renderbufferStorage, "glRenderbufferStorage");
    LoadGl(m_gl->framebufferRenderbuffer, "glFramebufferRenderbuffer");
    LoadGl(m_gl->texImage3D, "glTexImage3D");
    LoadGl(m_gl->texSubImage3D, "glTexSubImage3D");
    LoadGl(m_gl->getShaderiv, "glGetShaderiv");
    LoadGl(m_gl->getProgramiv, "glGetProgramiv");
    LoadGl(m_gl->getShaderInfoLog, "glGetShaderInfoLog");
    LoadGl(m_gl->getProgramInfoLog, "glGetProgramInfoLog");

    LoadGl(m_gl->clipControl, "glClipControl");
    LoadGl(m_gl->drawArraysIndirect, "glDrawArraysIndirect");
    LoadGl(m_gl->drawElementsIndirect, "glDrawElementsIndirect");
    LoadGl(m_gl->multiDrawArraysIndirect, "glMultiDrawArraysIndirect");
    LoadGl(m_gl->multiDrawElementsIndirect, "glMultiDrawElementsIndirect");
    LoadGl(m_gl->drawArraysInstancedBaseInstance, "glDrawArraysInstancedBaseInstance");
    LoadGl(m_gl->drawElementsInstancedBaseVertexBaseInstance, "glDrawElementsInstancedBaseVertexBaseInstance");

    return true;
}

bool GlServerDispatcher::ProcessPacketStream(NetworkSocket& socket, const PacketHeader& first,
                                             const std::vector<uint8_t>& first_payload) {
    PacketHeader header = first;
    std::vector<uint8_t> payload = first_payload;
    bool have_packet = true;

    while (socket.IsValid()) {
        if (!have_packet && !socket.RecvPacket(header, payload)) return false;
        have_packet = false;

        if (header.magic != RGPU_MAGIC) {
            std::cerr << "[GlServer] Invalid magic 0x" << std::hex << header.magic << std::dec << std::endl;
            return false;
        }
        if (header.opcode == OP_SHUTDOWN) return true;
        if (!Dispatch(header, payload.data(), payload.size(), socket)) {
            std::cerr << "[GlServer] Command failed (opcode 0x" << std::hex << header.opcode << std::dec
                      << "); ending the session." << std::endl;
            return false;
        }
    }
    return false;
}

bool GlServerDispatcher::Dispatch(const PacketHeader& header, const uint8_t* payload, size_t size,
                                  NetworkSocket& socket) {
    // Only the handshake may precede it; everything else needs a negotiated GL session.
    if (header.opcode == OP_HANDSHAKE_REQUEST) return HandleHandshake(header, payload, size, socket);
    if (!m_handshakeDone) return false;

    if (header.opcode == OP_GL_CREATE_CONTEXT) return HandleCreateContext(header, payload, size, socket);
    if (!m_device.IsValid()) return false;

    switch (header.opcode) {
        case OP_GL_COMMANDS: return ExecuteCommands(payload, size);
        case OP_GL_RESIZE:   return HandleResize(payload, size);
        case OP_GL_SWAP:     return HandleSwap(header, payload, size, socket);
        default:
            return false;
    }
}

bool GlServerDispatcher::HandleHandshake(const PacketHeader& header, const uint8_t* payload, size_t size,
                                         NetworkSocket& socket) {
    if (size < sizeof(HandshakeRequestPayload)) return false;
    const auto req = ReadArgs<HandshakeRequestPayload>(payload);
    if ((req.client_flags & CLIENT_FLAG_API_OPENGL) == 0) return false;

    m_compression = (req.client_flags & CLIENT_FLAG_FRAME_COMPRESSION) != 0;
    m_initialWidth = req.client_width;
    m_initialHeight = req.client_height;

    HandshakeResponsePayload resp = {};
    resp.protocol_version = RGPU_PROTOCOL_VERSION;
    resp.status_code = req.protocol_version == RGPU_PROTOCOL_VERSION ? 0 : 1;
    resp.frame_width = req.client_width;
    resp.frame_height = req.client_height;
    resp.frame_format = 28; // DXGI_FORMAT_R8G8B8A8_UNORM, the byte order glReadPixels produces
    std::strncpy(resp.adapter_desc, "OpenGL host", sizeof(resp.adapter_desc) - 1);

    if (!SendReply(socket, OP_HANDSHAKE_RESPONSE, header.sequence_id, &resp, sizeof(resp))) return false;
    m_handshakeDone = resp.status_code == 0;
    return m_handshakeDone;
}

bool GlServerDispatcher::HandleCreateContext(const PacketHeader& header, const uint8_t* payload, size_t size,
                                             NetworkSocket& socket) {
    if (size < sizeof(GlCreateContextPayload) || m_device.IsValid()) return false;
    const auto req = ReadArgs<GlCreateContextPayload>(payload);

    GlContextResponsePayload resp = {};
    if (!ValidSize(req.width, req.height)) {
        resp.status = 1;
    } else if (!m_device.Create(req.major, req.minor, req.profile_mask, req.flags, req.width, req.height)) {
        resp.status = 2;
    } else {
        resp.major = m_device.Major();
        resp.minor = m_device.Minor();
        std::strncpy(resp.renderer, m_device.Renderer().c_str(), sizeof(resp.renderer) - 1);
        std::cout << "[GlServer] Context " << resp.major << "." << resp.minor << " on " << resp.renderer
                  << " (" << req.width << "x" << req.height << ")" << std::endl;
    }
    m_previousFrame.clear();
    if (!SendReply(socket, OP_GL_CONTEXT_RESPONSE, header.sequence_id, &resp, sizeof(resp))) return false;
    return resp.status == 0;
}

bool GlServerDispatcher::HandleResize(const uint8_t* payload, size_t size) {
    if (size < sizeof(GlResizePayload)) return false;
    const auto req = ReadArgs<GlResizePayload>(payload);
    if (!ValidSize(req.width, req.height)) return false;
    m_previousFrame.clear(); // the next frame has a new size, so it must be a key frame
    return m_device.Resize(req.width, req.height);
}

bool GlServerDispatcher::ExecuteCommands(const uint8_t* data, size_t size) {
    if (!m_device.IsValid()) return false; // no context to run on
    if (!LoadGlApi()) return false;
    size_t pos = 0;
    while (pos < size) {
        if (size - pos < sizeof(GlCommandHeader)) return false;
        const auto cmd = ReadArgs<GlCommandHeader>(data + pos);
        pos += sizeof(GlCommandHeader);
        if (cmd.reserved != 0 || cmd.size > size - pos) return false;
        const uint8_t* args = data + pos;
        pos += cmd.size;
        ++m_commands;

        switch (cmd.func) {
            case GLF_Clear: {
                if (cmd.size != sizeof(GlClearArgs)) return false;
                const auto a = ReadArgs<GlClearArgs>(args);
                if ((a.mask & ~kClearMaskAll) != 0) return false;
                glClear(a.mask);
                break;
            }
            case GLF_ClearColor: {
                if (cmd.size != sizeof(GlClearColorArgs)) return false;
                const auto a = ReadArgs<GlClearColorArgs>(args);
                glClearColor(a.r, a.g, a.b, a.a);
                break;
            }
            case GLF_Viewport: {
                if (cmd.size != sizeof(GlViewportArgs)) return false;
                const auto a = ReadArgs<GlViewportArgs>(args);
                if (a.width < 0 || a.height < 0 || a.width > kMaxViewportDim || a.height > kMaxViewportDim) return false;
                glViewport(a.x, a.y, a.width, a.height);
                break;
            }
            case GLF_Flush:
                if (cmd.size != 0) return false;
                glFlush();
                break;
            case GLF_Finish:
                if (cmd.size != 0) return false;
                glFinish();
                break;
            case GLF_Begin: {
                if (cmd.size != sizeof(GlBeginArgs)) return false;
                const auto a = ReadArgs<GlBeginArgs>(args);
                if (a.mode != GL_POINTS && a.mode != GL_LINES && a.mode != GL_LINE_STRIP &&
                    a.mode != GL_TRIANGLES && a.mode != GL_TRIANGLE_STRIP && a.mode != GL_TRIANGLE_FAN &&
                    a.mode != GL_QUADS) return false;
                glBegin(a.mode);
                break;
            }
            case GLF_End:
                if (cmd.size != 0) return false;
                glEnd();
                break;
            case GLF_Color3f: {
                if (cmd.size != sizeof(GlColor3fArgs)) return false;
                const auto a = ReadArgs<GlColor3fArgs>(args);
                glColor3f(a.r, a.g, a.b);
                break;
            }
            case GLF_Vertex2f: {
                if (cmd.size != sizeof(GlVertex2fArgs)) return false;
                const auto a = ReadArgs<GlVertex2fArgs>(args);
                glVertex2f(a.x, a.y);
                break;
            }
            case GLF_GenBuffer: {
                if (cmd.size != sizeof(GlIdArgs)) return false;
                const auto a = ReadArgs<GlIdArgs>(args);
                if (a.id == 0 || m_buffers.count(a.id)) return false;
                Buffer buffer;
                m_gl->genBuffers(1, &buffer.gl_id);
                if (!buffer.gl_id) return false;
                m_buffers.emplace(a.id, std::move(buffer));
                break;
            }
            case GLF_BindBuffer: {
                if (cmd.size != sizeof(GlBindBufferArgs)) return false;
                const auto a = ReadArgs<GlBindBufferArgs>(args);
                if (!IsValidBufferTarget(a.target)) return false;
                if (a.id != 0 && !m_buffers.count(a.id)) return false;
                m_boundBuffers[a.target] = a.id;
                if (a.target == kArrayBuffer) m_boundArrayBuffer = a.id;
                m_gl->bindBuffer(a.target, a.id ? m_buffers[a.id].gl_id : 0);
                break;
            }
            case GLF_BufferData: {
                if (cmd.size < sizeof(GlBufferDataArgs)) return false;
                const auto a = ReadArgs<GlBufferDataArgs>(args);
                if (!IsValidBufferTarget(a.target) || a.size > 64 * 1024 * 1024) return false;
                const bool hasData = (cmd.size == sizeof(a) + a.size);
                if (!hasData && cmd.size != sizeof(a)) return false;
                auto itBound = m_boundBuffers.find(a.target);
                if (itBound == m_boundBuffers.end() || itBound->second == 0) return false;
                auto itBuf = m_buffers.find(itBound->second);
                if (itBuf == m_buffers.end()) return false;
                auto& buffer = itBuf->second;
                buffer.target = a.target;
                buffer.gpu_size = a.size;
                if (hasData) {
                    buffer.data.assign(args + sizeof(a), args + cmd.size);
                } else {
                    buffer.data.clear();
                }
                m_gl->bindBuffer(a.target, buffer.gl_id);
                m_gl->bufferData(a.target, static_cast<std::ptrdiff_t>(a.size),
                                 hasData ? buffer.data.data() : nullptr, a.usage);
                break;
            }
            case GLF_DeleteBuffer: {
                if (cmd.size != sizeof(GlIdArgs)) return false;
                const auto a = ReadArgs<GlIdArgs>(args);
                m_buffers.erase(a.id);
                if (m_boundArrayBuffer == a.id) m_boundArrayBuffer = 0;
                for (auto& pair : m_boundBuffers) {
                    if (pair.second == a.id) {
                        pair.second = 0;
                    }
                }
                break;
            }
            case GLF_CreateShader: {
                if (cmd.size != sizeof(GlCreateShaderArgs)) return false;
                const auto a = ReadArgs<GlCreateShaderArgs>(args);
                if (a.id == 0 || m_shaders.count(a.id)) return false;
                const GLuint shader = m_gl->createShader(a.type);
                if (!shader) return false;
                m_shaders.emplace(a.id, shader);
                break;
            }
            case GLF_CreateProgram: {
                if (cmd.size != sizeof(GlIdArgs)) return false;
                const auto a = ReadArgs<GlIdArgs>(args);
                if (a.id == 0 || m_programs.count(a.id)) return false;
                const GLuint program = m_gl->createProgram();
                if (!program) return false;
                m_programs.emplace(a.id, program);
                break;
            }
            case GLF_ShaderSource: {
                if (cmd.size < sizeof(GlShaderSourceArgs)) return false;
                const auto a = ReadArgs<GlShaderSourceArgs>(args);
                auto it = m_shaders.find(a.id);
                if (it == m_shaders.end() || a.length != cmd.size - sizeof(a)) return false;
                const char* source = reinterpret_cast<const char*>(args + sizeof(a));
                const GLint source_length = static_cast<GLint>(a.length);
                m_gl->shaderSource(it->second, 1, &source, &source_length);
                break;
            }
            case GLF_CompileShader: {
                if (cmd.size != sizeof(GlIdArgs)) return false;
                const auto a = ReadArgs<GlIdArgs>(args);
                auto it = m_shaders.find(a.id);
                if (it == m_shaders.end()) return false;
                m_gl->compileShader(it->second);
                if (m_gl->getShaderiv && m_gl->getShaderInfoLog) {
                    GLint ok = 1;
                    m_gl->getShaderiv(it->second, 0x8B81 /*GL_COMPILE_STATUS*/, &ok);
                    if (!ok) {
                        char log[2048] = {};
                        m_gl->getShaderInfoLog(it->second, sizeof(log) - 1, nullptr, log);
                        std::cerr << "[GlServer] Shader " << a.id << " compile FAILED:\n" << log << std::endl;
                    }
                }
                break;
            }
            case GLF_AttachShader: {
                if (cmd.size != sizeof(GlAttachShaderArgs)) return false;
                const auto a = ReadArgs<GlAttachShaderArgs>(args);
                auto p = m_programs.find(a.program), s = m_shaders.find(a.shader);
                if (p == m_programs.end() || s == m_shaders.end()) return false;
                m_gl->attachShader(p->second, s->second);
                break;
            }
            case GLF_LinkProgram: {
                if (cmd.size != sizeof(GlIdArgs)) return false;
                const auto a = ReadArgs<GlIdArgs>(args);
                auto it = m_programs.find(a.id);
                if (it == m_programs.end()) return false;
                m_gl->linkProgram(it->second);
                if (m_gl->getProgramiv && m_gl->getProgramInfoLog) {
                    GLint ok = 1;
                    m_gl->getProgramiv(it->second, 0x8B82 /*GL_LINK_STATUS*/, &ok);
                    if (!ok) {
                        char log[2048] = {};
                        m_gl->getProgramInfoLog(it->second, sizeof(log) - 1, nullptr, log);
                        std::cerr << "[GlServer] Program " << a.id << " link FAILED:\n" << log << std::endl;
                    }
                }
                break;
            }
            case GLF_UseProgram: {
                if (cmd.size != sizeof(GlIdArgs)) return false;
                const auto a = ReadArgs<GlIdArgs>(args);
                if (a.id != 0 && !m_programs.count(a.id)) return false;
                m_gl->useProgram(a.id ? m_programs[a.id] : 0);
                break;
            }
            case GLF_DrawArrays: {
                if (cmd.size != sizeof(GlDrawArraysArgs)) return false;
                const auto a = ReadArgs<GlDrawArraysArgs>(args);
                if (a.mode != GL_TRIANGLES && a.mode != GL_POINTS && a.mode != GL_LINES &&
                    a.mode != GL_LINE_STRIP && a.mode != GL_TRIANGLE_STRIP && a.mode != GL_TRIANGLE_FAN &&
                    a.mode != GL_QUADS) return false;
                glDrawArrays(a.mode, a.first, a.count);
                break;
            }
            case GLF_GenTextures: {
                if (cmd.size < sizeof(GlGenArgs)) return false;
                const auto a = ReadArgs<GlGenArgs>(args);
                if (cmd.size != sizeof(GlGenArgs) + a.count * sizeof(uint32_t)) return false;
                const uint32_t* ids = reinterpret_cast<const uint32_t*>(args + sizeof(GlGenArgs));
                for (uint32_t i = 0; i < a.count; ++i) {
                    GLuint tex = 0;
                    glGenTextures(1, &tex);
                    m_textures[ids[i]] = tex;
                }
                break;
            }
            case GLF_BindTexture: {
                if (cmd.size != sizeof(GlBindTextureArgs)) return false;
                const auto a = ReadArgs<GlBindTextureArgs>(args);
                GLuint tex = 0;
                if (a.id != 0) {
                    auto it = m_textures.find(a.id);
                    if (it != m_textures.end()) tex = it->second;
                }
                glBindTexture(a.target, tex);
                break;
            }
            case GLF_DeleteTextures: {
                if (cmd.size < sizeof(GlGenArgs)) return false;
                const auto a = ReadArgs<GlGenArgs>(args);
                if (cmd.size != sizeof(GlGenArgs) + a.count * sizeof(uint32_t)) return false;
                const uint32_t* ids = reinterpret_cast<const uint32_t*>(args + sizeof(GlGenArgs));
                for (uint32_t i = 0; i < a.count; ++i) {
                    auto it = m_textures.find(ids[i]);
                    if (it != m_textures.end()) {
                        glDeleteTextures(1, &it->second);
                        m_textures.erase(it);
                    }
                }
                break;
            }
            case GLF_TexParameteri: {
                if (cmd.size != sizeof(GlTexParameteriArgs)) return false;
                const auto a = ReadArgs<GlTexParameteriArgs>(args);
                glTexParameteri(a.target, a.pname, a.param);
                break;
            }
            case GLF_TexImage2D: {
                if (cmd.size < sizeof(GlTexImage2DArgs)) return false;
                const auto a = ReadArgs<GlTexImage2DArgs>(args);
                if (cmd.size != sizeof(GlTexImage2DArgs) + a.data_size) return false;
                const void* pixels = a.data_size ? (args + sizeof(GlTexImage2DArgs)) : reinterpret_cast<const void*>(static_cast<uintptr_t>(a.pixel_offset));
                glTexImage2D(a.target, a.level, a.internalformat, a.width, a.height, a.border, a.format, a.type, pixels);
                break;
            }
            case GLF_TexSubImage2D: {
                if (cmd.size < sizeof(GlTexSubImage2DArgs)) return false;
                const auto a = ReadArgs<GlTexSubImage2DArgs>(args);
                if (cmd.size != sizeof(GlTexSubImage2DArgs) + a.data_size) return false;
                const void* pixels = a.data_size ? (args + sizeof(GlTexSubImage2DArgs)) : reinterpret_cast<const void*>(static_cast<uintptr_t>(a.pixel_offset));
                glTexSubImage2D(a.target, a.level, a.xoffset, a.yoffset, a.width, a.height, a.format, a.type, pixels);
                break;
            }
            case GLF_ActiveTexture: {
                if (cmd.size != sizeof(GlIdArgs)) return false;
                const auto a = ReadArgs<GlIdArgs>(args);
                if (m_gl->activeTexture) m_gl->activeTexture(a.id);
                break;
            }
            case GLF_GenerateMipmap: {
                if (cmd.size != sizeof(GlIdArgs)) return false;
                const auto a = ReadArgs<GlIdArgs>(args);
                if (m_gl->generateMipmap) m_gl->generateMipmap(a.id);
                break;
            }
            case GLF_GenSamplers: {
                if (cmd.size < sizeof(GlGenArgs)) return false;
                const auto a = ReadArgs<GlGenArgs>(args);
                if (cmd.size != sizeof(GlGenArgs) + a.count * sizeof(uint32_t)) return false;
                const uint32_t* ids = reinterpret_cast<const uint32_t*>(args + sizeof(GlGenArgs));
                for (uint32_t i = 0; i < a.count; ++i) {
                    GLuint s = 0;
                    if (m_gl->genSamplers) m_gl->genSamplers(1, &s);
                    m_samplers[ids[i]] = s;
                }
                break;
            }
            case GLF_BindSampler: {
                if (cmd.size != sizeof(GlBindSamplerArgs)) return false;
                const auto a = ReadArgs<GlBindSamplerArgs>(args);
                GLuint s = 0;
                if (a.sampler != 0) {
                    auto it = m_samplers.find(a.sampler);
                    if (it != m_samplers.end()) s = it->second;
                }
                if (m_gl->bindSampler) m_gl->bindSampler(a.unit, s);
                break;
            }
            case GLF_DeleteSamplers: {
                if (cmd.size < sizeof(GlGenArgs)) return false;
                const auto a = ReadArgs<GlGenArgs>(args);
                if (cmd.size != sizeof(GlGenArgs) + a.count * sizeof(uint32_t)) return false;
                const uint32_t* ids = reinterpret_cast<const uint32_t*>(args + sizeof(GlGenArgs));
                for (uint32_t i = 0; i < a.count; ++i) {
                    auto it = m_samplers.find(ids[i]);
                    if (it != m_samplers.end()) {
                        if (m_gl->deleteSamplers) m_gl->deleteSamplers(1, &it->second);
                        m_samplers.erase(it);
                    }
                }
                break;
            }
            case GLF_SamplerParameteri: {
                if (cmd.size != sizeof(GlSamplerParameteriArgs)) return false;
                const auto a = ReadArgs<GlSamplerParameteriArgs>(args);
                auto it = m_samplers.find(a.sampler);
                if (it != m_samplers.end() && m_gl->samplerParameteri) {
                    m_gl->samplerParameteri(it->second, a.pname, a.param);
                }
                break;
            }
            case GLF_GenFramebuffers: {
                if (cmd.size < sizeof(GlGenArgs)) return false;
                const auto a = ReadArgs<GlGenArgs>(args);
                if (cmd.size != sizeof(GlGenArgs) + a.count * sizeof(uint32_t)) return false;
                const uint32_t* ids = reinterpret_cast<const uint32_t*>(args + sizeof(GlGenArgs));
                for (uint32_t i = 0; i < a.count; ++i) {
                    GLuint fbo = 0;
                    if (m_gl->genFramebuffers) m_gl->genFramebuffers(1, &fbo);
                    m_framebuffers[ids[i]] = fbo;
                }
                break;
            }
            case GLF_BindFramebuffer: {
                if (cmd.size != sizeof(GlBindFramebufferArgs)) return false;
                const auto a = ReadArgs<GlBindFramebufferArgs>(args);
                GLuint fbo = m_device.Fbo(); // Client FBO 0 redirects to host offscreen FBO
                if (a.id != 0) {
                    auto it = m_framebuffers.find(a.id);
                    if (it != m_framebuffers.end()) fbo = it->second;
                }
                if (m_gl->bindFramebuffer) m_gl->bindFramebuffer(a.target, fbo);
                {
                    const bool isDef = (fbo == m_device.Fbo());
                    if (a.target == 0x8D40 /*GL_FRAMEBUFFER*/) { m_drawFboDefault = m_readFboDefault = isDef; }
                    else if (a.target == 0x8CA9 /*GL_DRAW_FRAMEBUFFER*/) m_drawFboDefault = isDef;
                    else if (a.target == 0x8CA8 /*GL_READ_FRAMEBUFFER*/) m_readFboDefault = isDef;
                }
                break;
            }
            case GLF_DeleteFramebuffers: {
                if (cmd.size < sizeof(GlGenArgs)) return false;
                const auto a = ReadArgs<GlGenArgs>(args);
                if (cmd.size != sizeof(GlGenArgs) + a.count * sizeof(uint32_t)) return false;
                const uint32_t* ids = reinterpret_cast<const uint32_t*>(args + sizeof(GlGenArgs));
                for (uint32_t i = 0; i < a.count; ++i) {
                    auto it = m_framebuffers.find(ids[i]);
                    if (it != m_framebuffers.end()) {
                        if (m_gl->deleteFramebuffers) m_gl->deleteFramebuffers(1, &it->second);
                        m_framebuffers.erase(it);
                    }
                }
                break;
            }
            case GLF_FramebufferTexture2D: {
                if (cmd.size != sizeof(GlFramebufferTexture2DArgs)) return false;
                const auto a = ReadArgs<GlFramebufferTexture2DArgs>(args);
                GLuint tex = 0;
                if (a.texture != 0) {
                    auto it = m_textures.find(a.texture);
                    if (it != m_textures.end()) tex = it->second;
                }
                if (m_gl->framebufferTexture2D) m_gl->framebufferTexture2D(a.target, a.attachment, a.textarget, tex, a.level);
                break;
            }
            case GLF_BlitFramebuffer: {
                if (cmd.size != sizeof(GlBlitFramebufferArgs)) return false;
                const auto a = ReadArgs<GlBlitFramebufferArgs>(args);
                if (m_gl->blitFramebuffer) {
                    m_gl->blitFramebuffer(a.srcX0, a.srcY0, a.srcX1, a.srcY1,
                                          a.dstX0, a.dstY0, a.dstX1, a.dstY1,
                                          a.mask, a.filter);
                }
                break;
            }
            case GLF_DrawBuffer: {
                if (cmd.size != sizeof(GlIdArgs)) return false;
                const auto a = ReadArgs<GlIdArgs>(args);
                glDrawBuffer(TranslateDrawBuffer(a.id, m_drawFboDefault));
                break;
            }
            case GLF_DrawBuffers: {
                if (cmd.size < sizeof(GlDrawBuffersArgs)) return false;
                const auto a = ReadArgs<GlDrawBuffersArgs>(args);
                if (cmd.size != sizeof(GlDrawBuffersArgs) + a.n * sizeof(uint32_t)) return false;
                const uint32_t* bufs = reinterpret_cast<const uint32_t*>(args + sizeof(GlDrawBuffersArgs));
                if (m_gl->drawBuffers) {
                    std::vector<GLenum> tr(a.n);
                    for (uint32_t i = 0; i < a.n; ++i) tr[i] = TranslateDrawBuffer(bufs[i], m_drawFboDefault);
                    m_gl->drawBuffers(static_cast<GLsizei>(a.n), tr.data());
                }
                break;
            }
            case GLF_GenVertexArrays: {
                if (cmd.size < sizeof(GlGenArgs)) return false;
                const auto a = ReadArgs<GlGenArgs>(args);
                if (cmd.size != sizeof(GlGenArgs) + a.count * sizeof(uint32_t)) return false;
                const uint32_t* ids = reinterpret_cast<const uint32_t*>(args + sizeof(GlGenArgs));
                for (uint32_t i = 0; i < a.count; ++i) {
                    GLuint vao = 0;
                    if (m_gl->genVertexArrays) m_gl->genVertexArrays(1, &vao);
                    m_vaos[ids[i]] = vao;
                }
                break;
            }
            case GLF_BindVertexArray: {
                if (cmd.size != sizeof(GlIdArgs)) return false;
                const auto a = ReadArgs<GlIdArgs>(args);
                GLuint vao = 0;
                if (a.id != 0) {
                    auto it = m_vaos.find(a.id);
                    if (it != m_vaos.end()) vao = it->second;
                }
                if (m_gl->bindVertexArray) m_gl->bindVertexArray(vao);
                break;
            }
            case GLF_DeleteVertexArrays: {
                if (cmd.size < sizeof(GlGenArgs)) return false;
                const auto a = ReadArgs<GlGenArgs>(args);
                if (cmd.size != sizeof(GlGenArgs) + a.count * sizeof(uint32_t)) return false;
                const uint32_t* ids = reinterpret_cast<const uint32_t*>(args + sizeof(GlGenArgs));
                for (uint32_t i = 0; i < a.count; ++i) {
                    auto it = m_vaos.find(ids[i]);
                    if (it != m_vaos.end()) {
                        if (m_gl->deleteVertexArrays) m_gl->deleteVertexArrays(1, &it->second);
                        m_vaos.erase(it);
                    }
                }
                break;
            }
            case GLF_EnableVertexAttribArray: {
                if (cmd.size != sizeof(GlIdArgs)) return false;
                const auto a = ReadArgs<GlIdArgs>(args);
                if (m_gl->enableVertexAttribArray) m_gl->enableVertexAttribArray(a.id);
                break;
            }
            case GLF_DisableVertexAttribArray: {
                if (cmd.size != sizeof(GlIdArgs)) return false;
                const auto a = ReadArgs<GlIdArgs>(args);
                if (m_gl->disableVertexAttribArray) m_gl->disableVertexAttribArray(a.id);
                break;
            }
            case GLF_VertexAttribPointer: {
                if (cmd.size != sizeof(GlVertexAttribPointerArgs)) return false;
                const auto a = ReadArgs<GlVertexAttribPointerArgs>(args);
                if (a.reserved[0] == 1 && m_gl->vertexAttribIPointer) {
                    m_gl->vertexAttribIPointer(a.index, a.size, a.type, a.stride,
                                               reinterpret_cast<const void*>(static_cast<uintptr_t>(a.pointer)));
                } else if (m_gl->vertexAttribPointer) {
                    m_gl->vertexAttribPointer(a.index, a.size, a.type, a.normalized, a.stride,
                                              reinterpret_cast<const void*>(static_cast<uintptr_t>(a.pointer)));
                }
                break;
            }
            case GLF_VertexAttribDivisor: {
                if (cmd.size != sizeof(GlVertexAttribDivisorArgs)) return false;
                const auto a = ReadArgs<GlVertexAttribDivisorArgs>(args);
                if (m_gl->vertexAttribDivisor) m_gl->vertexAttribDivisor(a.index, a.divisor);
                break;
            }
            case GLF_BindBufferRange: {
                if (cmd.size != sizeof(GlBindBufferRangeArgs)) return false;
                const auto a = ReadArgs<GlBindBufferRangeArgs>(args);
                GLuint buf = 0;
                if (a.buffer != 0) {
                    auto it = m_buffers.find(a.buffer);
                    if (it != m_buffers.end()) buf = it->second.gl_id;
                }
                if (m_gl->bindBufferRange) {
                    m_gl->bindBufferRange(a.target, a.index, buf,
                                          static_cast<GLintptr>(a.offset),
                                          static_cast<GLsizeiptr>(a.size));
                }
                break;
            }
            case GLF_BindBufferBase: {
                if (cmd.size != sizeof(GlBindBufferBaseArgs)) return false;
                const auto a = ReadArgs<GlBindBufferBaseArgs>(args);
                GLuint buf = 0;
                if (a.buffer != 0) {
                    auto it = m_buffers.find(a.buffer);
                    if (it != m_buffers.end()) buf = it->second.gl_id;
                }
                if (m_gl->bindBufferBase) m_gl->bindBufferBase(a.target, a.index, buf);
                break;
            }
            case GLF_BufferSubData: {
                if (cmd.size < sizeof(GlBufferSubDataArgs)) return false;
                const auto a = ReadArgs<GlBufferSubDataArgs>(args);
                if (!IsValidBufferTarget(a.target)) return false;
                if (cmd.size != sizeof(GlBufferSubDataArgs) + a.size) return false;
                const void* data = args + sizeof(GlBufferSubDataArgs);
                {
                    auto itB = m_boundBuffers.find(a.target);
                    if (itB != m_boundBuffers.end() && itB->second != 0) {
                        auto itBuf = m_buffers.find(itB->second);
                        if (itBuf != m_buffers.end() && a.offset + a.size > itBuf->second.gpu_size) {
                            itBuf->second.gpu_size = a.offset + a.size;
                            m_gl->bufferData(a.target, static_cast<std::ptrdiff_t>(itBuf->second.gpu_size), nullptr, 0x88E8 /*GL_DYNAMIC_DRAW*/);
                        }
                    }
                }
                if (m_gl->bufferSubData) {
                    m_gl->bufferSubData(a.target, static_cast<std::ptrdiff_t>(a.offset),
                                        static_cast<std::ptrdiff_t>(a.size), data);
                }
                break;
            }
            case GLF_DrawElements: {
                if (cmd.size != sizeof(GlDrawElementsArgs)) return false;
                const auto a = ReadArgs<GlDrawElementsArgs>(args);
                glDrawElements(a.mode, a.count, a.type, reinterpret_cast<const void*>(static_cast<uintptr_t>(a.indices)));
                break;
            }
            case GLF_DrawArraysInstanced: {
                if (cmd.size != sizeof(GlDrawArraysInstancedArgs)) return false;
                const auto a = ReadArgs<GlDrawArraysInstancedArgs>(args);
                if (m_gl->drawArraysInstanced) {
                    m_gl->drawArraysInstanced(a.mode, a.first, a.count, a.instancecount);
                } else {
                    glDrawArrays(a.mode, a.first, a.count);
                }
                break;
            }
            case GLF_DrawElementsInstancedBaseVertex: {
                if (cmd.size != sizeof(GlDrawElementsInstancedBaseVertexArgs)) return false;
                const auto a = ReadArgs<GlDrawElementsInstancedBaseVertexArgs>(args);
                if (m_gl->drawElementsInstancedBaseVertex) {
                    m_gl->drawElementsInstancedBaseVertex(a.mode, a.count, a.type,
                                                          reinterpret_cast<const void*>(static_cast<uintptr_t>(a.indices)),
                                                          a.instancecount, a.basevertex);
                } else if (m_gl->drawElementsInstanced) {
                    m_gl->drawElementsInstanced(a.mode, a.count, a.type,
                                                reinterpret_cast<const void*>(static_cast<uintptr_t>(a.indices)),
                                                a.instancecount);
                } else {
                    glDrawElements(a.mode, a.count, a.type, reinterpret_cast<const void*>(static_cast<uintptr_t>(a.indices)));
                }
                break;
            }
            case GLF_Enable: {
                if (cmd.size != sizeof(GlIdArgs)) return false;
                const auto a = ReadArgs<GlIdArgs>(args);
                glEnable(a.id);
                break;
            }
            case GLF_Disable: {
                if (cmd.size != sizeof(GlIdArgs)) return false;
                const auto a = ReadArgs<GlIdArgs>(args);
                glDisable(a.id);
                break;
            }
            case GLF_Scissor: {
                if (cmd.size != sizeof(GlViewportArgs)) return false;
                const auto a = ReadArgs<GlViewportArgs>(args);
                glScissor(a.x, a.y, a.width, a.height);
                break;
            }
            case GLF_BlendFuncSeparate: {
                if (cmd.size != sizeof(GlBlendFuncSeparateArgs)) return false;
                const auto a = ReadArgs<GlBlendFuncSeparateArgs>(args);
                if (m_gl->blendFuncSeparate) m_gl->blendFuncSeparate(a.srcRGB, a.dstRGB, a.srcAlpha, a.dstAlpha);
                else glBlendFunc(a.srcRGB, a.dstRGB);
                break;
            }
            case GLF_DepthFunc: {
                if (cmd.size != sizeof(GlIdArgs)) return false;
                const auto a = ReadArgs<GlIdArgs>(args);
                glDepthFunc(a.id);
                break;
            }
            case GLF_DepthMask: {
                if (cmd.size != sizeof(GlIdArgs)) return false;
                const auto a = ReadArgs<GlIdArgs>(args);
                glDepthMask(static_cast<GLboolean>(a.id != 0));
                break;
            }
            case GLF_ColorMask: {
                if (cmd.size != sizeof(GlColorMaskArgs)) return false;
                const auto a = ReadArgs<GlColorMaskArgs>(args);
                glColorMask(a.r, a.g, a.b, a.a);
                break;
            }
            case GLF_PolygonMode: {
                if (cmd.size != sizeof(GlPolygonModeArgs)) return false;
                const auto a = ReadArgs<GlPolygonModeArgs>(args);
                glPolygonMode(a.face, a.mode);
                break;
            }
            case GLF_CopyBufferSubData: {
                if (cmd.size != sizeof(GlCopyBufferSubDataArgs)) return false;
                const auto a = ReadArgs<GlCopyBufferSubDataArgs>(args);
                if (!IsValidBufferTarget(a.readTarget) || !IsValidBufferTarget(a.writeTarget)) return false;
                if (m_gl->copyBufferSubData) {
                    m_gl->copyBufferSubData(a.readTarget, a.writeTarget,
                                            static_cast<std::ptrdiff_t>(a.readOffset),
                                            static_cast<std::ptrdiff_t>(a.writeOffset),
                                            static_cast<std::ptrdiff_t>(a.size));
                }
                break;
            }
            case GLF_PixelStorei: {
                if (cmd.size != sizeof(GlPixelStoreiArgs)) return false;
                const auto a = ReadArgs<GlPixelStoreiArgs>(args);
                glPixelStorei(a.pname, a.param);
                break;
            }
            case GLF_BindAttribLocation: {
                if (cmd.size < sizeof(GlBindAttribLocationArgs)) return false;
                const auto a = ReadArgs<GlBindAttribLocationArgs>(args);
                if (cmd.size != sizeof(a) + a.name_len || a.name_len > 256) return false;
                auto it = m_programs.find(a.program);
                if (it != m_programs.end()) {
                    std::string name(reinterpret_cast<const char*>(args + sizeof(a)), a.name_len);
                    if (m_gl->bindAttribLocation) m_gl->bindAttribLocation(it->second, a.index, name.c_str());
                }
                break;
            }
            case GLF_UniformBlockBinding: {
                if (cmd.size < sizeof(GlUniformBlockBindingArgs)) return false;
                const auto a = ReadArgs<GlUniformBlockBindingArgs>(args);
                if (cmd.size != sizeof(a) + a.name_len || a.name_len > 256) return false;
                auto it = m_programs.find(a.program);
                if (it != m_programs.end()) {
                    std::string name(reinterpret_cast<const char*>(args + sizeof(a)), a.name_len);
                    if (m_gl->getUniformBlockIndex && m_gl->uniformBlockBinding) {
                        const GLuint idx = m_gl->getUniformBlockIndex(it->second, name.c_str());
                        if (idx != 0xFFFFFFFFu) m_gl->uniformBlockBinding(it->second, idx, a.binding);
                    }
                }
                break;
            }
            case GLF_RegisterUniformLocation: {
                if (cmd.size < sizeof(GlRegisterUniformLocationArgs)) return false;
                const auto a = ReadArgs<GlRegisterUniformLocationArgs>(args);
                if (cmd.size != sizeof(a) + a.name_len || a.name_len > 256) return false;
                auto it = m_programs.find(a.program);
                if (it != m_programs.end()) {
                    std::string name(reinterpret_cast<const char*>(args + sizeof(a)), a.name_len);
                    m_uniformLocs[a.client_loc] = m_gl->getUniformLocation ? m_gl->getUniformLocation(it->second, name.c_str()) : -1;
                } else {
                    m_uniformLocs[a.client_loc] = -1;
                }
                break;
            }
            case GLF_Uniform: {
                if (cmd.size < sizeof(GlUniformArgs)) return false;
                const auto a = ReadArgs<GlUniformArgs>(args);
                if (cmd.size != sizeof(a) + a.data_size || a.type > 2 || a.comps < 1 || a.comps > 4 || a.count <= 0) return false;
                const size_t elems = a.matrix ? static_cast<size_t>(a.comps) * a.comps : a.comps;
                if (a.data_size != elems * 4 * static_cast<size_t>(a.count)) return false;
                auto it = m_uniformLocs.find(a.client_loc);
                if (it == m_uniformLocs.end() || it->second < 0) break;
                const void* data = args + sizeof(a);
                if (a.matrix) {
                    if (a.type != 0 || a.comps < 2) return false;
                    auto fn = m_gl->uniformMatrix[a.comps - 2];
                    if (fn) fn(it->second, a.count, a.transpose ? GL_TRUE : GL_FALSE, data);
                } else {
                    auto fn = m_gl->uniformV[a.type][a.comps - 1];
                    if (fn) fn(it->second, a.count, data);
                }
                break;
            }
            case GLF_BlendEquationSeparate: {
                if (cmd.size != sizeof(GlBlendEquationSeparateArgs)) return false;
                const auto a = ReadArgs<GlBlendEquationSeparateArgs>(args);
                if (m_gl->blendEquationSeparate) m_gl->blendEquationSeparate(a.modeRGB, a.modeAlpha);
                break;
            }
            case GLF_CullFace: {
                if (cmd.size != sizeof(GlIdArgs)) return false;
                glCullFace(ReadArgs<GlIdArgs>(args).id);
                break;
            }
            case GLF_FrontFace: {
                if (cmd.size != sizeof(GlIdArgs)) return false;
                glFrontFace(ReadArgs<GlIdArgs>(args).id);
                break;
            }
            case GLF_PolygonOffset: {
                if (cmd.size != sizeof(GlPolygonOffsetArgs)) return false;
                const auto a = ReadArgs<GlPolygonOffsetArgs>(args);
                glPolygonOffset(a.factor, a.units);
                break;
            }
            case GLF_ClearDepth: {
                if (cmd.size != sizeof(GlClearDepthArgs)) return false;
                glClearDepth(ReadArgs<GlClearDepthArgs>(args).depth);
                break;
            }
            case GLF_ReadBuffer: {
                if (cmd.size != sizeof(GlIdArgs)) return false;
                glReadBuffer(TranslateDrawBuffer(ReadArgs<GlIdArgs>(args).id, m_readFboDefault));
                break;
            }
            case GLF_GenRenderbuffers: {
                if (cmd.size < sizeof(GlGenArgs)) return false;
                const auto a = ReadArgs<GlGenArgs>(args);
                if (cmd.size != sizeof(GlGenArgs) + a.count * sizeof(uint32_t)) return false;
                const uint32_t* ids = reinterpret_cast<const uint32_t*>(args + sizeof(GlGenArgs));
                for (uint32_t i = 0; i < a.count; ++i) {
                    GLuint rb = 0;
                    if (m_gl->genRenderbuffers) m_gl->genRenderbuffers(1, &rb);
                    m_renderbuffers[ids[i]] = rb;
                }
                break;
            }
            case GLF_BindRenderbuffer: {
                if (cmd.size != sizeof(GlBindRenderbufferArgs)) return false;
                const auto a = ReadArgs<GlBindRenderbufferArgs>(args);
                GLuint rb = 0;
                if (a.id != 0) {
                    auto it = m_renderbuffers.find(a.id);
                    if (it != m_renderbuffers.end()) rb = it->second;
                }
                if (m_gl->bindRenderbuffer) m_gl->bindRenderbuffer(a.target, rb);
                break;
            }
            case GLF_DeleteRenderbuffers: {
                if (cmd.size < sizeof(GlGenArgs)) return false;
                const auto a = ReadArgs<GlGenArgs>(args);
                if (cmd.size != sizeof(GlGenArgs) + a.count * sizeof(uint32_t)) return false;
                const uint32_t* ids = reinterpret_cast<const uint32_t*>(args + sizeof(GlGenArgs));
                for (uint32_t i = 0; i < a.count; ++i) {
                    auto it = m_renderbuffers.find(ids[i]);
                    if (it != m_renderbuffers.end()) {
                        if (m_gl->deleteRenderbuffers) m_gl->deleteRenderbuffers(1, &it->second);
                        m_renderbuffers.erase(it);
                    }
                }
                break;
            }
            case GLF_RenderbufferStorage: {
                if (cmd.size != sizeof(GlRenderbufferStorageArgs)) return false;
                const auto a = ReadArgs<GlRenderbufferStorageArgs>(args);
                if (a.width < 0 || a.height < 0 || a.width > kMaxViewportDim || a.height > kMaxViewportDim) return false;
                if (m_gl->renderbufferStorage) m_gl->renderbufferStorage(a.target, a.internalformat, a.width, a.height);
                break;
            }
            case GLF_FramebufferRenderbuffer: {
                if (cmd.size != sizeof(GlFramebufferRenderbufferArgs)) return false;
                const auto a = ReadArgs<GlFramebufferRenderbufferArgs>(args);
                GLuint rb = 0;
                if (a.renderbuffer != 0) {
                    auto it = m_renderbuffers.find(a.renderbuffer);
                    if (it != m_renderbuffers.end()) rb = it->second;
                }
                if (m_gl->framebufferRenderbuffer) m_gl->framebufferRenderbuffer(a.target, a.attachment, a.rbtarget, rb);
                break;
            }
            case GLF_TexImage3D: {
                if (cmd.size < sizeof(GlTexImage3DArgs)) return false;
                const auto a = ReadArgs<GlTexImage3DArgs>(args);
                if (cmd.size != sizeof(a) + a.data_size) return false;
                const void* pixels = a.data_size ? static_cast<const void*>(args + sizeof(a))
                                                 : reinterpret_cast<const void*>(static_cast<uintptr_t>(a.pixel_offset));
                if (m_gl->texImage3D) m_gl->texImage3D(a.target, a.level, a.internalformat, a.width, a.height, a.depth,
                                                       a.border, a.format, a.type, pixels);
                break;
            }
            case GLF_TexSubImage3D: {
                if (cmd.size < sizeof(GlTexSubImage3DArgs)) return false;
                const auto a = ReadArgs<GlTexSubImage3DArgs>(args);
                if (cmd.size != sizeof(a) + a.data_size) return false;
                const void* pixels = a.data_size ? static_cast<const void*>(args + sizeof(a))
                                                 : reinterpret_cast<const void*>(static_cast<uintptr_t>(a.pixel_offset));
                if (m_gl->texSubImage3D) m_gl->texSubImage3D(a.target, a.level, a.xoffset, a.yoffset, a.zoffset,
                                                             a.width, a.height, a.depth, a.format, a.type, pixels);
                break;
            }
            case GLF_ClipControl: {
                if (cmd.size != sizeof(GlClipControlArgs)) return false;
                const auto a = ReadArgs<GlClipControlArgs>(args);
                if (m_gl->clipControl) m_gl->clipControl(a.origin, a.depth);
                break;
            }
            case GLF_DrawArraysIndirect: {
                if (cmd.size != sizeof(GlDrawArraysIndirectArgs)) return false;
                const auto a = ReadArgs<GlDrawArraysIndirectArgs>(args);
                if (m_gl->drawArraysIndirect) {
                    m_gl->drawArraysIndirect(a.mode, reinterpret_cast<const void*>(static_cast<uintptr_t>(a.indirect)));
                }
                break;
            }
            case GLF_DrawElementsIndirect: {
                if (cmd.size != sizeof(GlDrawElementsIndirectArgs)) return false;
                const auto a = ReadArgs<GlDrawElementsIndirectArgs>(args);
                if (m_gl->drawElementsIndirect) {
                    m_gl->drawElementsIndirect(a.mode, a.type, reinterpret_cast<const void*>(static_cast<uintptr_t>(a.indirect)));
                }
                break;
            }
            case GLF_MultiDrawArraysIndirect: {
                if (cmd.size != sizeof(GlMultiDrawArraysIndirectArgs)) return false;
                const auto a = ReadArgs<GlMultiDrawArraysIndirectArgs>(args);
                if (m_gl->multiDrawArraysIndirect) {
                    m_gl->multiDrawArraysIndirect(a.mode, reinterpret_cast<const void*>(static_cast<uintptr_t>(a.indirect)),
                                                  a.drawcount, a.stride);
                }
                break;
            }
            case GLF_MultiDrawElementsIndirect: {
                if (cmd.size != sizeof(GlMultiDrawElementsIndirectArgs)) return false;
                const auto a = ReadArgs<GlMultiDrawElementsIndirectArgs>(args);
                if (m_gl->multiDrawElementsIndirect) {
                    m_gl->multiDrawElementsIndirect(a.mode, a.type,
                                                    reinterpret_cast<const void*>(static_cast<uintptr_t>(a.indirect)),
                                                    a.drawcount, a.stride);
                }
                break;
            }
            case GLF_DrawArraysInstancedBaseInstance: {
                if (cmd.size != sizeof(GlDrawArraysInstancedBaseInstanceArgs)) return false;
                const auto a = ReadArgs<GlDrawArraysInstancedBaseInstanceArgs>(args);
                if (m_gl->drawArraysInstancedBaseInstance) {
                    m_gl->drawArraysInstancedBaseInstance(a.mode, a.first, a.count, a.instancecount, a.baseinstance);
                } else if (m_gl->drawArraysInstanced) {
                    m_gl->drawArraysInstanced(a.mode, a.first, a.count, a.instancecount);
                } else {
                    glDrawArrays(a.mode, a.first, a.count);
                }
                break;
            }
            case GLF_DrawElementsInstancedBaseVertexBaseInstance: {
                if (cmd.size != sizeof(GlDrawElementsInstancedBaseVertexBaseInstanceArgs)) return false;
                const auto a = ReadArgs<GlDrawElementsInstancedBaseVertexBaseInstanceArgs>(args);
                if (m_gl->drawElementsInstancedBaseVertexBaseInstance) {
                    m_gl->drawElementsInstancedBaseVertexBaseInstance(
                        a.mode, a.count, a.type,
                        reinterpret_cast<const void*>(static_cast<uintptr_t>(a.indices)),
                        a.instancecount, a.basevertex, a.baseinstance);
                } else if (m_gl->drawElementsInstancedBaseVertex) {
                    m_gl->drawElementsInstancedBaseVertex(
                        a.mode, a.count, a.type,
                        reinterpret_cast<const void*>(static_cast<uintptr_t>(a.indices)),
                        a.instancecount, a.basevertex);
                } else if (m_gl->drawElementsInstanced) {
                    m_gl->drawElementsInstanced(
                        a.mode, a.count, a.type,
                        reinterpret_cast<const void*>(static_cast<uintptr_t>(a.indices)),
                        a.instancecount);
                } else {
                    glDrawElements(a.mode, a.count, a.type,
                                   reinterpret_cast<const void*>(static_cast<uintptr_t>(a.indices)));
                }
                break;
            }
            default:
                return false;
        }
    }
    return true;
}

bool GlServerDispatcher::HandleSwap(const PacketHeader& header, const uint8_t* payload, size_t size,
                                    NetworkSocket& socket) {
    if (size < sizeof(GlSwapPayload)) return false;
    const auto swap = ReadArgs<GlSwapPayload>(payload);

    if (!m_device.ReadbackFrame(m_frame)) {
        std::cerr << "[GlServer] Frame readback failed." << std::endl;
        return false;
    }
    const uint32_t width = m_device.Width();
    const uint32_t height = m_device.Height();
    const size_t pixel_count = static_cast<size_t>(width) * height;

    // Same wire format as the Direct3D path: compressed when it saves bytes, raw otherwise, and
    // either way the client ends up holding m_frame, the reference for the next delta frame.
    bool compressed = false;
    if (m_compression) {
        const bool has_reference = m_previousFrame.size() == m_frame.size();
        FrameCodec::Encode(m_frame.data(), has_reference ? m_previousFrame.data() : nullptr, pixel_count, m_encoded);
        compressed = m_encoded.size() < m_frame.size();
    }
    const std::vector<uint8_t>& body = compressed ? m_encoded : m_frame;

    PacketHeader reply = {};
    reply.magic = RGPU_MAGIC;
    reply.opcode = OP_FRAME_DATA;
    reply.flags = compressed ? FLAG_COMPRESSED : FLAG_NONE;
    reply.sequence_id = header.sequence_id;
    reply.payload_length = static_cast<uint32_t>(sizeof(FrameDataPayload) + body.size());

    FrameDataPayload meta = {};
    meta.width = width;
    meta.height = height;
    meta.format = 28;
    meta.row_pitch = width * 4;
    meta.data_size = static_cast<uint32_t>(body.size());
    meta.frame_number = swap.frame_number;

    if (!socket.SendExact(&reply, sizeof(reply)) || !socket.SendExact(&meta, sizeof(meta)) ||
        !socket.SendExact(body.data(), body.size())) {
        return false;
    }

    m_rawBytes += m_frame.size();
    m_sentBytes += body.size();
    if (m_compression) m_previousFrame.swap(m_frame);
    ++m_frames;
    return true;
}

} // namespace RemoteGPU
