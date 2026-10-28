#include "gl_server_dispatcher.h"

#include <GL/gl.h>

#include <cstring>
#include <iostream>

#include "frame_codec.h"

namespace RemoteGPU {

using GenBuffersFn = void(WINAPI*)(GLsizei, GLuint*);
using BindBufferFn = void(WINAPI*)(GLenum, GLuint);
using BufferDataFn = void(WINAPI*)(GLenum, std::ptrdiff_t, const void*, GLenum);
using CreateShaderFn = GLuint(WINAPI*)(GLenum);
using ShaderSourceFn = void(WINAPI*)(GLuint, GLsizei, const char* const*, const GLint*);
using CompileShaderFn = void(WINAPI*)(GLuint);
using CreateProgramFn = GLuint(WINAPI*)();
using AttachShaderFn = void(WINAPI*)(GLuint, GLuint);
using LinkProgramFn = void(WINAPI*)(GLuint);
using UseProgramFn = void(WINAPI*)(GLuint);

struct GlDispatchApi {
    GenBuffersFn genBuffers = nullptr;
    BindBufferFn bindBuffer = nullptr;
    BufferDataFn bufferData = nullptr;
    CreateShaderFn createShader = nullptr;
    ShaderSourceFn shaderSource = nullptr;
    CompileShaderFn compileShader = nullptr;
    CreateProgramFn createProgram = nullptr;
    AttachShaderFn attachShader = nullptr;
    LinkProgramFn linkProgram = nullptr;
    UseProgramFn useProgram = nullptr;
};

namespace {

constexpr uint32_t kClearMaskAll = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_ACCUM_BUFFER_BIT;
constexpr int32_t  kMaxViewportDim = 32768;
constexpr GLenum kArrayBuffer = 0x8892;
constexpr GLenum kElementArrayBuffer = 0x8893;
constexpr GLenum kUniformBuffer = 0x8A11;

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
    return LoadGl(m_gl->genBuffers, "glGenBuffers") && LoadGl(m_gl->bindBuffer, "glBindBuffer") &&
           LoadGl(m_gl->bufferData, "glBufferData") && LoadGl(m_gl->createShader, "glCreateShader") &&
           LoadGl(m_gl->shaderSource, "glShaderSource") && LoadGl(m_gl->compileShader, "glCompileShader") &&
           LoadGl(m_gl->createProgram, "glCreateProgram") && LoadGl(m_gl->attachShader, "glAttachShader") &&
           LoadGl(m_gl->linkProgram, "glLinkProgram") && LoadGl(m_gl->useProgram, "glUseProgram");
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
                if (a.target != kArrayBuffer && a.target != kElementArrayBuffer && a.target != kUniformBuffer) return false;
                if (a.id != 0 && !m_buffers.count(a.id)) return false;
                m_boundBuffers[a.target] = a.id;
                if (a.target == kArrayBuffer) m_boundArrayBuffer = a.id;
                m_gl->bindBuffer(a.target, a.id ? m_buffers[a.id].gl_id : 0);
                break;
            }
            case GLF_BufferData: {
                if (cmd.size < sizeof(GlBufferDataArgs)) return false;
                const auto a = ReadArgs<GlBufferDataArgs>(args);
                if ((a.target != kArrayBuffer && a.target != kElementArrayBuffer && a.target != kUniformBuffer) ||
                    a.size > 64 * 1024 * 1024 || a.size != cmd.size - sizeof(a)) return false;
                auto itBound = m_boundBuffers.find(a.target);
                if (itBound == m_boundBuffers.end() || itBound->second == 0) return false;
                auto itBuf = m_buffers.find(itBound->second);
                if (itBuf == m_buffers.end()) return false;
                auto& buffer = itBuf->second;
                buffer.target = a.target;
                buffer.data.assign(args + sizeof(a), args + cmd.size);
                m_gl->bindBuffer(a.target, buffer.gl_id);
                m_gl->bufferData(a.target, static_cast<std::ptrdiff_t>(a.size),
                                 buffer.data.empty() ? nullptr : buffer.data.data(), a.usage);
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
                if (a.mode != GL_TRIANGLES && a.mode != GL_POINTS && a.mode != GL_LINES) return false;
                glDrawArrays(a.mode, a.first, a.count);
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
