// Zero-trust tests for the OpenGL server path: a real GlServerDispatcher runs in a thread behind a
// loopback socket and receives well-formed and hostile packets from a raw client.
// Usage: test_gl_server [base_port]   (each case uses its own port)

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include "frame_codec.h"
#include "gl_protocol.h"
#include "gl_server_dispatcher.h"
#include "transport.h"

using namespace RemoteGPU;

namespace {

int g_failures = 0;

void Check(bool ok, const char* label) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++g_failures;
}

// Appends one command (header + args) to a batch.
void AddCommand(std::vector<uint8_t>& batch, uint16_t func, const void* args, uint32_t size, uint16_t reserved = 0,
                uint32_t declared_size = UINT32_MAX) {
    GlCommandHeader h = {func, reserved, declared_size == UINT32_MAX ? size : declared_size};
    const auto* p = reinterpret_cast<const uint8_t*>(&h);
    batch.insert(batch.end(), p, p + sizeof(h));
    const auto* a = static_cast<const uint8_t*>(args);
    batch.insert(batch.end(), a, a + size);
}

// One client session against a freshly started dispatcher.
class Case {
public:
    explicit Case(uint16_t port) : m_port(port) {
        m_listener.BindAndListen(port, "127.0.0.1");
        m_server = std::thread([this] {
            auto sock = m_listener.Accept(10000);
            if (!sock) return;
            PacketHeader first = {};
            std::vector<uint8_t> payload;
            if (!sock->RecvPacket(first, payload)) return;
            GlServerDispatcher dispatcher;
            m_sessionOk = dispatcher.ProcessPacketStream(*sock, first, payload);
            sock->Close();
        });
        m_client.Connect("127.0.0.1", port);
    }
    ~Case() {
        m_client.Close();
        if (m_server.joinable()) m_server.join();
    }

    bool Send(uint16_t opcode, const void* payload, uint32_t size) {
        PacketHeader h = {};
        h.magic = RGPU_MAGIC;
        h.opcode = opcode;
        h.sequence_id = ++m_seq;
        h.payload_length = size;
        return m_client.SendPacket(h, size ? payload : nullptr);
    }
    bool Receive(PacketHeader& h, std::vector<uint8_t>& payload) { return m_client.RecvPacket(h, payload); }

    // `video_kbps` appends a HandshakeVideoPayload asking for that bitrate.
    bool Handshake(uint32_t flags = CLIENT_FLAG_API_OPENGL, uint32_t video_kbps = UINT32_MAX) {
        HandshakeRequestPayload hello = {};
        hello.protocol_version = RGPU_PROTOCOL_VERSION;
        hello.client_flags = flags;
        hello.client_width = 64;
        hello.client_height = 48;
        const HandshakeVideoPayload video = {video_kbps, 0};
        uint8_t bytes[sizeof(hello) + sizeof(video)];
        std::memcpy(bytes, &hello, sizeof(hello));
        std::memcpy(bytes + sizeof(hello), &video, sizeof(video));
        const uint32_t size = static_cast<uint32_t>(sizeof(hello) + (video_kbps != UINT32_MAX ? sizeof(video) : 0));
        PacketHeader h;
        std::vector<uint8_t> p;
        return Send(OP_HANDSHAKE_REQUEST, bytes, size) && Receive(h, p) && h.opcode == OP_HANDSHAKE_RESPONSE;
    }

    // Returns the context response status, or UINT32_MAX when no response came.
    uint32_t CreateContext(uint32_t width, uint32_t height, uint32_t major = 0) {
        GlCreateContextPayload req = {major, 0, major ? 1u : 0u, 0, width, height};
        PacketHeader h;
        std::vector<uint8_t> p;
        if (!Send(OP_GL_CREATE_CONTEXT, &req, sizeof(req)) || !Receive(h, p) || h.opcode != OP_GL_CONTEXT_RESPONSE ||
            p.size() < sizeof(GlContextResponsePayload)) {
            return UINT32_MAX;
        }
        GlContextResponsePayload resp;
        std::memcpy(&resp, p.data(), sizeof(resp));
        return resp.status;
    }

    // True when the server ended the session (the socket reads EOF) without sending anything.
    bool SessionEnded() {
        PacketHeader h;
        std::vector<uint8_t> p;
        return !Receive(h, p);
    }

    bool SessionOk() {
        if (m_server.joinable()) m_server.join();
        return m_sessionOk;
    }

    NetworkSocket m_client;

private:
    uint16_t m_port;
    NetworkSocket m_listener;
    std::thread m_server;
    uint32_t m_seq = 0;
    bool m_sessionOk = true;
};

// Runs one hostile batch on a session that has a valid context and expects it to be dropped.
void ExpectRejected(uint16_t port, const char* label, const std::vector<uint8_t>& batch) {
    Case c(port);
    bool ok = c.Handshake() && c.CreateContext(64, 48) == 0;
    ok = ok && c.Send(OP_GL_COMMANDS, batch.data(), static_cast<uint32_t>(batch.size()));
    // A rejected batch ends the session, so the swap that follows gets no frame back. A server
    // that ignored the batch instead would answer it with a frame, failing the check at once
    // rather than passing on the receive timeout.
    const GlSwapPayload swap = {1};
    c.Send(OP_GL_SWAP, &swap, sizeof(swap));
    ok = ok && c.SessionEnded();
    Check(ok, label);
}

// Full-screen triangle from gl_VertexID (the tests run on legacy contexts, which need no VAO).
const char* const kFullScreenVs =
    "#version 330\n"
    "void main() { vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";

// Appends the commands that create and compile shader `id` from `src`.
void AddShader(std::vector<uint8_t>& batch, uint32_t type, uint32_t id, const char* src) {
    const GlCreateShaderArgs create = {type, id};
    AddCommand(batch, GLF_CreateShader, &create, sizeof(create));
    const GlShaderSourceArgs head = {id, static_cast<uint32_t>(std::strlen(src))};
    std::vector<uint8_t> source(sizeof(head) + head.length);
    std::memcpy(source.data(), &head, sizeof(head));
    std::memcpy(source.data() + sizeof(head), src, head.length);
    AddCommand(batch, GLF_ShaderSource, source.data(), static_cast<uint32_t>(source.size()));
    const GlIdArgs compile = {id};
    AddCommand(batch, GLF_CompileShader, &compile, sizeof(compile));
}

// Puts decoded pixels in RGBA order whatever byte order the server sent.
void ToRgba(std::vector<uint8_t>& pixels, uint32_t format) {
    if (format != FRAME_FORMAT_BGRA8) return;
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
}

// Swaps and decodes the returned frame into top-down RGBA `pixels`.
bool SwapAndDecode(Case& c, uint64_t frame_number, std::vector<uint8_t>& pixels) {
    const GlSwapPayload swap = {frame_number};
    PacketHeader h;
    std::vector<uint8_t> p;
    if (!c.Send(OP_GL_SWAP, &swap, sizeof(swap)) || !c.Receive(h, p) || h.opcode != OP_FRAME_DATA ||
        p.size() < sizeof(FrameDataPayload)) {
        return false;
    }
    FrameDataPayload meta;
    std::memcpy(&meta, p.data(), sizeof(meta));
    const size_t count = static_cast<size_t>(meta.width) * meta.height;
    pixels.assign(count * 4, 0);
    if (h.flags & FLAG_COMPRESSED) {
        if (!FrameCodec::Decode(p.data() + sizeof(meta), p.size() - sizeof(meta), pixels.data(), count, false)) return false;
    } else {
        if (p.size() - sizeof(meta) < pixels.size()) return false;
        std::memcpy(pixels.data(), p.data() + sizeof(meta), pixels.size());
    }
    ToRgba(pixels, meta.format);
    return true;
}

// Sends a batch that clears a width x height viewport to (r, g, b).
bool SendClear(Case& c, float r, float g, float b, int32_t width, int32_t height) {
    std::vector<uint8_t> batch;
    const GlViewportArgs viewport = {0, 0, width, height};
    const GlClearColorArgs color = {r, g, b, 1.0f};
    const GlClearArgs clear = {0x4000};
    AddCommand(batch, GLF_Viewport, &viewport, sizeof(viewport));
    AddCommand(batch, GLF_ClearColor, &color, sizeof(color));
    AddCommand(batch, GLF_Clear, &clear, sizeof(clear));
    return c.Send(OP_GL_COMMANDS, batch.data(), static_cast<uint32_t>(batch.size()));
}

struct VideoFrame {
    uint32_t codec = 0;
    uint32_t mode = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool picture = false;
    std::vector<uint8_t> rgba; // top-down, when `picture`

    bool Near(uint32_t x, uint32_t y, int r, int g, int b) const {
        if (!picture || x >= width || y >= height) return false;
        const uint8_t* p = &rgba[(static_cast<size_t>(y) * width + x) * 4];
        return std::abs(p[0] - r) <= 8 && std::abs(p[1] - g) <= 8 && std::abs(p[2] - b) <= 8;
    }
};

// Swaps and, for a FRAME_CODEC_H264 frame, decodes its access units with `decoder`.
bool SwapVideo(Case& c, uint64_t frame_number, Video::H264Decoder& decoder, VideoFrame& f) {
    const GlSwapPayload swap = {frame_number};
    PacketHeader h;
    std::vector<uint8_t> p;
    if (!c.Send(OP_GL_SWAP, &swap, sizeof(swap)) || !c.Receive(h, p) || h.opcode != OP_FRAME_DATA ||
        !(h.flags & FLAG_COMPRESSED) || p.size() < sizeof(FrameDataPayload) + sizeof(CompressedFrameHeader)) {
        return false;
    }
    FrameDataPayload meta;
    CompressedFrameHeader header;
    std::memcpy(&meta, p.data(), sizeof(meta));
    std::memcpy(&header, p.data() + sizeof(meta), sizeof(header));
    f = VideoFrame{header.codec, header.mode, meta.width, meta.height, false, {}};
    if (header.codec != FRAME_CODEC_H264) return true;
    f.rgba.assign(static_cast<size_t>(meta.width) * meta.height * 4, 0);
    size_t pos = sizeof(meta) + sizeof(header);
    while (pos + 4 <= p.size()) {
        uint32_t length = 0;
        std::memcpy(&length, &p[pos], 4);
        pos += 4;
        bool got = false;
        if (length > p.size() - pos || !decoder.Decode(&p[pos], length, meta.width, meta.height, f.rgba.data(), got)) {
            return false;
        }
        f.picture = f.picture || got;
        pos += length;
    }
    ToRgba(f.rgba, FRAME_FORMAT_BGRA8);
    return pos == p.size();
}

} // namespace

int main(int argc, char** argv) {
    uint16_t port = argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : 48990;
    if (!NetworkSocket::InitializeNetwork()) return 2;

    std::printf("[gl_server] valid session\n");
    {
        Case c(port++);
        bool ok = c.Handshake() && c.CreateContext(64, 48) == 0;
        Check(ok, "handshake and context creation");
        if (ok) {
            std::vector<uint8_t> batch;
            const GlClearColorArgs color = {0.0f, 1.0f, 0.0f, 1.0f};
            const GlClearArgs clear = {0x4000};
            const GlViewportArgs viewport = {0, 0, 64, 48};
            AddCommand(batch, GLF_Viewport, &viewport, sizeof(viewport));
            AddCommand(batch, GLF_ClearColor, &color, sizeof(color));
            AddCommand(batch, GLF_Clear, &clear, sizeof(clear));
            AddCommand(batch, GLF_Flush, nullptr, 0);
            const GlSwapPayload swap = {7};
            PacketHeader h;
            std::vector<uint8_t> p;
            ok = c.Send(OP_GL_COMMANDS, batch.data(), static_cast<uint32_t>(batch.size())) &&
                 c.Send(OP_GL_SWAP, &swap, sizeof(swap)) && c.Receive(h, p) && h.opcode == OP_FRAME_DATA &&
                 p.size() >= sizeof(FrameDataPayload);
            Check(ok, "commands then swap return a frame");
            if (ok) {
                FrameDataPayload meta;
                std::memcpy(&meta, p.data(), sizeof(meta));
                Check(meta.width == 64 && meta.height == 48 && meta.frame_number == 7, "frame metadata matches the context");
                std::vector<uint8_t> pixels(64 * 48 * 4, 0);
                const bool decoded = (h.flags & FLAG_COMPRESSED)
                    ? FrameCodec::Decode(p.data() + sizeof(meta), p.size() - sizeof(meta), pixels.data(), 64 * 48, false)
                    : (std::memcpy(pixels.data(), p.data() + sizeof(meta), pixels.size()), true);
                ToRgba(pixels, meta.format);
                const uint8_t* mid = &pixels[(24 * 64 + 32) * 4];
                Check(decoded && mid[0] == 0 && mid[1] == 255 && mid[2] == 0, "centre pixel is the clear colour");
            }
        }
        Check(c.m_client.IsValid(), "client connection stayed open");
    }

    std::printf("[gl_server] uniform buffer lifecycle\n");
    {
        Case c(port++);
        bool ok = c.Handshake() && c.CreateContext(64, 48) == 0;
        if (ok) {
            std::vector<uint8_t> batch;
            const GlIdArgs gen = {1};
            AddCommand(batch, GLF_GenBuffer, &gen, sizeof(gen));

            const GlBindBufferArgs bind = {0x8A11 /*GL_UNIFORM_BUFFER*/, 1};
            AddCommand(batch, GLF_BindBuffer, &bind, sizeof(bind));

            std::vector<uint8_t> bufDataCmd;
            const GlBufferDataArgs bda = {0x8A11 /*GL_UNIFORM_BUFFER*/, 0x88E4 /*GL_STATIC_DRAW*/, 64};
            bufDataCmd.resize(sizeof(bda) + 64, 0xAB);
            std::memcpy(bufDataCmd.data(), &bda, sizeof(bda));
            AddCommand(batch, GLF_BufferData, bufDataCmd.data(), static_cast<uint32_t>(bufDataCmd.size()));

            const GlIdArgs del = {1};
            AddCommand(batch, GLF_DeleteBuffer, &del, sizeof(del));
            AddCommand(batch, GLF_Flush, nullptr, 0);

            ok = c.Send(OP_GL_COMMANDS, batch.data(), static_cast<uint32_t>(batch.size()));
            Check(ok && c.m_client.IsValid(), "uniform buffer lifecycle accepted");
        } else {
            Check(false, "uniform buffer lifecycle accepted");
        }
    }

    std::printf("[gl_server] texture buffer object read by a shader\n");
    {
        // Minecraft's terrain shaders read chunk data with texelFetch(samplerBuffer): the buffer
        // goes through GL_TEXTURE_BUFFER and is attached with glTexBuffer.
        Case c(port++);
        bool ok = c.Handshake() && c.CreateContext(64, 48) == 0;
        if (ok) {
            std::vector<uint8_t> batch;
            const GlIdArgs gen = {1};
            AddCommand(batch, GLF_GenBuffer, &gen, sizeof(gen));
            const GlBindBufferArgs bind = {0x8C2A /*GL_TEXTURE_BUFFER*/, 1};
            AddCommand(batch, GLF_BindBuffer, &bind, sizeof(bind));
            const GlBufferDataArgs bda = {0x8C2A, 0x88E4 /*GL_STATIC_DRAW*/, 8};
            const uint8_t texels[8] = {255, 0, 0, 255, 0, 255, 0, 255}; // texel 0 red, texel 1 green
            std::vector<uint8_t> data(sizeof(bda) + sizeof(texels));
            std::memcpy(data.data(), &bda, sizeof(bda));
            std::memcpy(data.data() + sizeof(bda), texels, sizeof(texels));
            AddCommand(batch, GLF_BufferData, data.data(), static_cast<uint32_t>(data.size()));

            const uint32_t gen_tex[2] = {1, 5}; // GlGenArgs{count = 1} followed by the id
            AddCommand(batch, GLF_GenTextures, gen_tex, sizeof(gen_tex));
            const GlBindTextureArgs bind_tex = {0x8C2A, 5};
            AddCommand(batch, GLF_BindTexture, &bind_tex, sizeof(bind_tex));
            const GlTexBufferArgs tex_buffer = {0x8C2A, 0x8058 /*GL_RGBA8*/, 1};
            AddCommand(batch, GLF_TexBuffer, &tex_buffer, sizeof(tex_buffer));

            // The fragment colour is texel 1 of the buffer.
            AddShader(batch, 0x8B31 /*GL_VERTEX_SHADER*/, 10, kFullScreenVs);
            AddShader(batch, 0x8B30 /*GL_FRAGMENT_SHADER*/, 11,
                      "#version 330\n"
                      "uniform samplerBuffer tbo;\n"
                      "out vec4 color;\n"
                      "void main() { color = texelFetch(tbo, 1); }\n");
            const GlIdArgs program = {20};
            AddCommand(batch, GLF_CreateProgram, &program, sizeof(program));
            const GlAttachShaderArgs attach_vs = {20, 10}, attach_fs = {20, 11};
            AddCommand(batch, GLF_AttachShader, &attach_vs, sizeof(attach_vs));
            AddCommand(batch, GLF_AttachShader, &attach_fs, sizeof(attach_fs));
            AddCommand(batch, GLF_LinkProgram, &program, sizeof(program));
            AddCommand(batch, GLF_UseProgram, &program, sizeof(program));

            // Indexed blend toggles are forwarded too; blending ends up off for the draw.
            const GlCapIndexArgs blend0 = {0x0BE2 /*GL_BLEND*/, 0};
            AddCommand(batch, GLF_Enablei, &blend0, sizeof(blend0));
            AddCommand(batch, GLF_Disablei, &blend0, sizeof(blend0));

            const GlViewportArgs viewport = {0, 0, 64, 48};
            const GlClearColorArgs black = {0.0f, 0.0f, 0.0f, 1.0f};
            const GlClearArgs clear = {0x4000};
            const GlDrawArraysArgs draw = {GL_TRIANGLES, 0, 3};
            AddCommand(batch, GLF_Viewport, &viewport, sizeof(viewport));
            AddCommand(batch, GLF_ClearColor, &black, sizeof(black));
            AddCommand(batch, GLF_Clear, &clear, sizeof(clear));
            AddCommand(batch, GLF_DrawArrays, &draw, sizeof(draw));

            const GlSwapPayload swap = {3};
            PacketHeader h;
            std::vector<uint8_t> p;
            ok = c.Send(OP_GL_COMMANDS, batch.data(), static_cast<uint32_t>(batch.size())) &&
                 c.Send(OP_GL_SWAP, &swap, sizeof(swap)) && c.Receive(h, p) && h.opcode == OP_FRAME_DATA &&
                 p.size() >= sizeof(FrameDataPayload);
            Check(ok, "TBO commands accepted and a frame returned");
            if (ok) {
                FrameDataPayload meta;
                std::memcpy(&meta, p.data(), sizeof(meta));
                std::vector<uint8_t> pixels(64 * 48 * 4, 0);
                const bool decoded = (h.flags & FLAG_COMPRESSED)
                    ? FrameCodec::Decode(p.data() + sizeof(meta), p.size() - sizeof(meta), pixels.data(), 64 * 48, false)
                    : (std::memcpy(pixels.data(), p.data() + sizeof(meta), pixels.size()), true);
                ToRgba(pixels, meta.format);
                const uint8_t* mid = &pixels[(24 * 64 + 32) * 4];
                char label[96];
                std::snprintf(label, sizeof(label), "shader reads texel 1 of the buffer (centre %u,%u,%u)",
                              mid[0], mid[1], mid[2]);
                Check(decoded && mid[0] == 0 && mid[1] == 255 && mid[2] == 0, label);
            }
        } else {
            Check(false, "TBO commands accepted and a frame returned");
        }
    }

    std::printf("[gl_server] buffer larger than 64 MB keeps every upload\n");
    {
        // Minecraft 26.3 allocates 128 MB chunk heaps without data, then writes sections anywhere
        // in them. A later write further in must not reallocate (and so wipe) the buffer.
        Case c(port++);
        bool ok = c.Handshake() && c.CreateContext(64, 48) == 0;
        std::vector<uint8_t> batch;
        const GlIdArgs gen = {1};
        AddCommand(batch, GLF_GenBuffer, &gen, sizeof(gen));
        const GlBindBufferArgs bind = {0x8C2A /*GL_TEXTURE_BUFFER*/, 1};
        AddCommand(batch, GLF_BindBuffer, &bind, sizeof(bind));
        const uint64_t heap = 100ull << 20, far_offset = 96ull << 20;
        const GlBufferDataArgs alloc = {0x8C2A, 0x88E8 /*GL_DYNAMIC_DRAW*/, heap};
        AddCommand(batch, GLF_BufferData, &alloc, sizeof(alloc));
        auto write_texel = [&](uint64_t offset, uint8_t r, uint8_t g, uint8_t b) {
            const GlBufferSubDataArgs w = {0x8C2A, 0, offset, 4};
            uint8_t cmd[sizeof(w) + 4];
            std::memcpy(cmd, &w, sizeof(w));
            const uint8_t rgba[4] = {r, g, b, 255};
            std::memcpy(cmd + sizeof(w), rgba, sizeof(rgba));
            AddCommand(batch, GLF_BufferSubData, cmd, sizeof(cmd));
        };
        write_texel(0, 0, 255, 0);
        write_texel(far_offset, 0, 0, 255);
        write_texel(heap, 255, 0, 0); // past the end: skipped, the buffer is neither grown nor wiped

        const uint32_t gen_tex[2] = {1, 5};
        AddCommand(batch, GLF_GenTextures, gen_tex, sizeof(gen_tex));
        const GlBindTextureArgs bind_tex = {0x8C2A, 5};
        AddCommand(batch, GLF_BindTexture, &bind_tex, sizeof(bind_tex));
        const GlTexBufferArgs tex_buffer = {0x8C2A, 0x8058 /*GL_RGBA8*/, 1};
        AddCommand(batch, GLF_TexBuffer, &tex_buffer, sizeof(tex_buffer));
        char fs[256];
        std::snprintf(fs, sizeof(fs),
                      "#version 330\nuniform samplerBuffer tbo;\nout vec4 color;\n"
                      "void main() { color = texelFetch(tbo, gl_FragCoord.x < 32.0 ? 0 : %u); }\n",
                      static_cast<unsigned>(far_offset / 4));
        AddShader(batch, 0x8B31 /*GL_VERTEX_SHADER*/, 10, kFullScreenVs);
        AddShader(batch, 0x8B30 /*GL_FRAGMENT_SHADER*/, 11, fs);
        const GlIdArgs program = {20};
        AddCommand(batch, GLF_CreateProgram, &program, sizeof(program));
        const GlAttachShaderArgs attach_vs = {20, 10}, attach_fs = {20, 11};
        AddCommand(batch, GLF_AttachShader, &attach_vs, sizeof(attach_vs));
        AddCommand(batch, GLF_AttachShader, &attach_fs, sizeof(attach_fs));
        AddCommand(batch, GLF_LinkProgram, &program, sizeof(program));
        AddCommand(batch, GLF_UseProgram, &program, sizeof(program));
        const GlViewportArgs viewport = {0, 0, 64, 48};
        const GlDrawArraysArgs draw = {GL_TRIANGLES, 0, 3};
        AddCommand(batch, GLF_Viewport, &viewport, sizeof(viewport));
        AddCommand(batch, GLF_DrawArrays, &draw, sizeof(draw));

        std::vector<uint8_t> pixels;
        ok = ok && c.Send(OP_GL_COMMANDS, batch.data(), static_cast<uint32_t>(batch.size())) && SwapAndDecode(c, 9, pixels);
        const uint8_t* left = ok ? &pixels[(24 * 64 + 8) * 4] : nullptr;
        const uint8_t* right = ok ? &pixels[(24 * 64 + 56) * 4] : nullptr;
        char label[160];
        std::snprintf(label, sizeof(label), "100 MB allocation accepted, first and far writes both kept (%u,%u,%u | %u,%u,%u)",
                      left ? left[0] : 0, left ? left[1] : 0, left ? left[2] : 0,
                      right ? right[0] : 0, right ? right[1] : 0, right ? right[2] : 0);
        Check(ok && left[1] == 255 && left[2] == 0 && right[2] == 255 && right[1] == 0, label);
    }

    std::printf("[gl_server] client newer than the server\n");
    {
        // A function id this server does not know is skipped; the session and the frame survive.
        Case c(port++);
        bool ok = c.Handshake() && c.CreateContext(64, 48) == 0;
        std::vector<uint8_t> batch;
        const uint8_t opaque[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        AddCommand(batch, 9999, opaque, sizeof(opaque));
        const GlClearColorArgs blue = {0.0f, 0.0f, 1.0f, 1.0f};
        const GlClearArgs clear = {0x4000};
        AddCommand(batch, GLF_ClearColor, &blue, sizeof(blue));
        AddCommand(batch, GLF_Clear, &clear, sizeof(clear));
        const GlSwapPayload swap = {4};
        PacketHeader h;
        std::vector<uint8_t> p;
        ok = ok && c.Send(OP_GL_COMMANDS, batch.data(), static_cast<uint32_t>(batch.size())) &&
             c.Send(OP_GL_SWAP, &swap, sizeof(swap)) && c.Receive(h, p) && h.opcode == OP_FRAME_DATA &&
             p.size() >= sizeof(FrameDataPayload);
        bool blue_frame = false;
        if (ok) {
            FrameDataPayload meta;
            std::memcpy(&meta, p.data(), sizeof(meta));
            std::vector<uint8_t> pixels(64 * 48 * 4, 0);
            const bool decoded = (h.flags & FLAG_COMPRESSED)
                ? FrameCodec::Decode(p.data() + sizeof(meta), p.size() - sizeof(meta), pixels.data(), 64 * 48, false)
                : (std::memcpy(pixels.data(), p.data() + sizeof(meta), pixels.size()), true);
            ToRgba(pixels, meta.format);
            const uint8_t* mid = &pixels[(24 * 64 + 32) * 4];
            blue_frame = decoded && mid[0] == 0 && mid[1] == 0 && mid[2] == 255;
        }
        Check(ok && blue_frame, "unknown function skipped, the rest of the batch still renders");
    }

    std::printf("[gl_server] H.264 video frames\n");
    if (!Video::IsAvailable()) {
        std::printf("  [SKIP] Media Foundation is not available on this system\n");
    } else {
        constexpr uint32_t kVideoFlags = CLIENT_FLAG_API_OPENGL | CLIENT_FLAG_FRAME_COMPRESSION | CLIENT_FLAG_VIDEO_H264;
        {
            Case c(port++);
            bool ok = c.Handshake(kVideoFlags, 12000) && c.CreateContext(320, 240) == 0;
            Check(ok, "video handshake and context creation");
            Video::H264Decoder decoder;
            Check(decoder.Create(), "H.264 decoder created");
            VideoFrame f;
            ok = ok && SendClear(c, 1.0f, 0.5f, 0.0f, 320, 240) && SwapVideo(c, 1, decoder, f);
            Check(ok && f.codec == FRAME_CODEC_H264 && f.mode == FRAME_MODE_KEY, "first frame is an H.264 key frame");
            Check(ok && f.Near(160, 120, 255, 128, 0) && f.Near(2, 2, 255, 128, 0), "orange decoded within 8 levels");
            ok = ok && SendClear(c, 0.0f, 0.5f, 0.5f, 320, 240) && SwapVideo(c, 2, decoder, f);
            Check(ok && f.codec == FRAME_CODEC_H264 && f.mode == FRAME_MODE_DELTA && f.Near(160, 120, 0, 128, 128),
                  "next frame is a predicted picture in the new colour");
            // A new size goes out losslessly until it has held for a while, then starts a new
            // stream, which a fresh decoder reads from its first frame.
            const GlResizePayload resize = {400, 300};
            Video::H264Decoder resized;
            ok = ok && resized.Create() && c.Send(OP_GL_RESIZE, &resize, sizeof(resize));
            int lossless = 0;
            for (uint64_t n = 3; ok && n < 60; ++n) {
                ok = SendClear(c, 0.2f, 0.4f, 1.0f, 400, 300) && SwapVideo(c, n, resized, f);
                if (f.codec == FRAME_CODEC_H264) break;
                lossless += f.codec == FRAME_CODEC_RLE_DELTA ? 1 : 100;
            }
            Check(ok && lossless >= 20 && lossless < 40, "a new size is sent losslessly until it holds (30 frames)");
            Check(ok && f.codec == FRAME_CODEC_H264 && f.width == 400 && f.height == 300 && f.mode == FRAME_MODE_KEY &&
                      f.Near(399, 299, 51, 102, 255),
                  "then it restarts the stream with a key frame");
        }
        {
            Case c(port++);
            bool ok = c.Handshake(kVideoFlags) && c.CreateContext(48, 32) == 0;
            std::vector<uint8_t> pixels;
            ok = ok && SendClear(c, 0.0f, 1.0f, 0.0f, 48, 32) && SwapAndDecode(c, 1, pixels);
            Check(ok && pixels[0] == 0 && pixels[1] == 255 && pixels[2] == 0, "frames under 64 pixels stay lossless");
        }
    }

    std::printf("[gl_server] hostile input\n");
    {
        std::vector<uint8_t> b;

        b.clear();
        const GlTexBufferArgs tbo_wrong_target = {0x0DE1 /*GL_TEXTURE_2D*/, 0x8058, 0};
        AddCommand(b, GLF_TexBuffer, &tbo_wrong_target, sizeof(tbo_wrong_target));
        ExpectRejected(port++, "glTexBuffer on a target other than GL_TEXTURE_BUFFER", b);

        b.clear();
        const GlTexBufferArgs tbo_unknown = {0x8C2A, 0x8058, 77};
        AddCommand(b, GLF_TexBuffer, &tbo_unknown, sizeof(tbo_unknown));
        ExpectRejected(port++, "glTexBuffer with a buffer the client never created", b);

        b.clear();
        const GlIdArgs gen_huge = {1};
        AddCommand(b, GLF_GenBuffer, &gen_huge, sizeof(gen_huge));
        const GlBindBufferArgs bind_huge = {0x8892 /*GL_ARRAY_BUFFER*/, 1};
        AddCommand(b, GLF_BindBuffer, &bind_huge, sizeof(bind_huge));
        const GlBufferDataArgs huge = {0x8892, 0x88E8, 3ull << 30};
        AddCommand(b, GLF_BufferData, &huge, sizeof(huge));
        ExpectRejected(port++, "glBufferData allocation over 2 GB", b);

        b.clear();
        const GlIdArgs short_enablei = {0x0BE2};
        AddCommand(b, GLF_Enablei, &short_enablei, sizeof(short_enablei));
        ExpectRejected(port++, "glEnablei with a truncated argument block", b);

        b = {1, 0, 0};  // shorter than a command header
        ExpectRejected(port++, "truncated command header", b);

        b.clear();
        AddCommand(b, GLF_Clear, nullptr, 0, 0, 4096);  // declares more argument bytes than the batch holds
        ExpectRejected(port++, "command size beyond the batch", b);

        b.clear();
        const uint8_t two[2] = {0, 0};
        AddCommand(b, GLF_Clear, two, 2);
        ExpectRejected(port++, "wrong argument size for glClear", b);

        b.clear();
        const GlClearArgs clear = {0x4000};
        AddCommand(b, GLF_Clear, &clear, sizeof(clear), 1);
        ExpectRejected(port++, "non-zero reserved field", b);

        b.clear();
        const GlClearArgs bad_mask = {0x80000000u};
        AddCommand(b, GLF_Clear, &bad_mask, sizeof(bad_mask));
        ExpectRejected(port++, "undefined glClear mask bits", b);

        b.clear();
        const GlViewportArgs negative = {0, 0, -5, 10};
        AddCommand(b, GLF_Viewport, &negative, sizeof(negative));
        ExpectRejected(port++, "negative viewport size", b);

        b.clear();
        AddCommand(b, GLF_Flush, nullptr, 0);
        const uint8_t four[4] = {0, 0, 0, 0};
        AddCommand(b, GLF_Flush, four, 4);  // second command has a size no flush accepts
        ExpectRejected(port++, "flush with arguments after a valid command", b);

        b.clear();
        const GlBindBufferArgs bad_bind = {0x9999, 1};
        AddCommand(b, GLF_BindBuffer, &bad_bind, sizeof(bad_bind));
        ExpectRejected(port++, "unsupported buffer target in glBindBuffer", b);

        b.clear();
        const GlBufferDataArgs bad_bda = {0x8A11, 0x88E4, 16};
        std::vector<uint8_t> bad_ubo(sizeof(bad_bda) + 16, 0);
        std::memcpy(bad_ubo.data(), &bad_bda, sizeof(bad_bda));
        AddCommand(b, GLF_BufferData, bad_ubo.data(), static_cast<uint32_t>(bad_ubo.size()));
        ExpectRejected(port++, "buffer data on unbound uniform buffer target", b);
    }

    std::printf("[gl_server] protocol order and limits\n");
    {
        Case c(port++);
        const GlSwapPayload swap = {1};
        bool ok = c.Handshake() && c.Send(OP_GL_COMMANDS, &swap, sizeof(swap)) && c.SessionEnded();
        Check(ok, "commands before a context are refused");
    }
    {
        Case c(port++);
        const GlSwapPayload swap = {1};
        Check(c.Send(OP_GL_SWAP, &swap, sizeof(swap)) && c.SessionEnded(), "GL packets before the handshake are refused");
    }
    {
        Case c(port++);
        Check(c.Handshake(CLIENT_FLAG_NONE) == false, "handshake without the OpenGL flag is refused");
    }
    {
        Case c(port++);
        Check(c.Handshake() && c.CreateContext(0, 0) == 1, "zero-sized context is refused");
    }
    {
        Case c(port++);
        Check(c.Handshake() && c.CreateContext(GL_MAX_FRAMEBUFFER_DIM + 1, 64) == 1, "oversized context is refused");
    }
    {
        Case c(port++);
        bool ok = c.Handshake() && c.CreateContext(32, 32) == 0;
        GlResizePayload huge = {100000, 100000};
        ok = ok && c.Send(OP_GL_RESIZE, &huge, sizeof(huge)) && c.SessionEnded();
        Check(ok, "oversized resize ends the session");
    }
    {
        Case c(port++);
        bool ok = c.Handshake() && c.CreateContext(32, 32) == 0;
        ok = ok && c.Send(0x01FF, nullptr, 0) && c.SessionEnded();
        Check(ok, "unknown opcode ends the session");
    }
    {
        Case c(port++);
        bool ok = c.Handshake() && c.CreateContext(32, 32) == 0;
        GlResizePayload size = {48, 40};
        const GlSwapPayload swap = {2};
        PacketHeader h;
        std::vector<uint8_t> p;
        ok = ok && c.Send(OP_GL_RESIZE, &size, sizeof(size)) && c.Send(OP_GL_SWAP, &swap, sizeof(swap)) &&
             c.Receive(h, p) && h.opcode == OP_FRAME_DATA && p.size() >= sizeof(FrameDataPayload);
        FrameDataPayload meta = {};
        if (ok) std::memcpy(&meta, p.data(), sizeof(meta));
        Check(ok && meta.width == 48 && meta.height == 40, "resize changes the next frame's size");
    }

    std::printf("[gl_server] concurrent sessions\n");
    {
        uint16_t conc_port = port++;
        NetworkSocket listener;
        listener.BindAndListen(conc_port, "127.0.0.1");

        std::atomic<bool> s1_ok{false}, s2_ok{false};
        std::thread s1_worker([&] {
            auto sock = listener.Accept(10000);
            if (!sock) return;
            PacketHeader first = {};
            std::vector<uint8_t> payload;
            if (!sock->RecvPacket(first, payload)) return;
            GlServerDispatcher d1;
            s1_ok = d1.ProcessPacketStream(*sock, first, payload);
            sock->Close();
        });
        std::thread s2_worker([&] {
            auto sock = listener.Accept(10000);
            if (!sock) return;
            PacketHeader first = {};
            std::vector<uint8_t> payload;
            if (!sock->RecvPacket(first, payload)) return;
            GlServerDispatcher d2;
            s2_ok = d2.ProcessPacketStream(*sock, first, payload);
            sock->Close();
        });

        auto run_client = [](uint16_t p, uint32_t w, uint32_t h, float r, float g, float b, uint8_t er, uint8_t eg, uint8_t eb) -> bool {
            NetworkSocket c;
            if (!c.Connect("127.0.0.1", p, 10000)) return false;
            uint32_t seq = 0;
            auto send_pkt = [&](uint16_t op, const void* data, uint32_t sz) {
                PacketHeader hdr = {RGPU_MAGIC, op, FLAG_NONE, ++seq, sz};
                return c.SendPacket(hdr, sz ? data : nullptr);
            };

            HandshakeRequestPayload hello = {};
            hello.protocol_version = RGPU_PROTOCOL_VERSION;
            hello.client_flags = CLIENT_FLAG_API_OPENGL;
            hello.client_width = w;
            hello.client_height = h;
            PacketHeader reply = {};
            std::vector<uint8_t> pld;
            if (!send_pkt(OP_HANDSHAKE_REQUEST, &hello, sizeof(hello)) || !c.RecvPacket(reply, pld) || reply.opcode != OP_HANDSHAKE_RESPONSE) return false;

            GlCreateContextPayload ctx_req = {0, 0, 0, 0, w, h};
            if (!send_pkt(OP_GL_CREATE_CONTEXT, &ctx_req, sizeof(ctx_req)) || !c.RecvPacket(reply, pld) || reply.opcode != OP_GL_CONTEXT_RESPONSE) return false;
            GlContextResponsePayload ctx_resp = {};
            std::memcpy(&ctx_resp, pld.data(), sizeof(ctx_resp));
            if (ctx_resp.status != 0) return false;

            std::vector<uint8_t> batch;
            const GlViewportArgs vp = {0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)};
            const GlClearColorArgs clr = {r, g, b, 1.0f};
            const GlClearArgs msk = {0x4000};
            AddCommand(batch, GLF_Viewport, &vp, sizeof(vp));
            AddCommand(batch, GLF_ClearColor, &clr, sizeof(clr));
            AddCommand(batch, GLF_Clear, &msk, sizeof(msk));
            AddCommand(batch, GLF_Flush, nullptr, 0);

            const GlSwapPayload swap = {101};
            if (!send_pkt(OP_GL_COMMANDS, batch.data(), static_cast<uint32_t>(batch.size())) ||
                !send_pkt(OP_GL_SWAP, &swap, sizeof(swap)) ||
                !c.RecvPacket(reply, pld) || reply.opcode != OP_FRAME_DATA || pld.size() < sizeof(FrameDataPayload)) return false;

            FrameDataPayload meta = {};
            std::memcpy(&meta, pld.data(), sizeof(meta));
            if (meta.width != w || meta.height != h) return false;

            std::vector<uint8_t> pixels(w * h * 4, 0);
            const bool decoded = (reply.flags & FLAG_COMPRESSED)
                ? FrameCodec::Decode(pld.data() + sizeof(meta), pld.size() - sizeof(meta), pixels.data(), w * h, false)
                : (std::memcpy(pixels.data(), pld.data() + sizeof(meta), pixels.size()), true);
            ToRgba(pixels, meta.format);
            if (!decoded) return false;

            const uint8_t* center = &pixels[((h / 2) * w + (w / 2)) * 4];
            if (center[0] != er || center[1] != eg || center[2] != eb) return false;

            ShutdownPayload shut = {};
            send_pkt(OP_SHUTDOWN, &shut, sizeof(shut));
            c.Close();
            return true;
        };

        std::atomic<bool> c1_ok{false}, c2_ok{false};
        std::thread t1([&] { c1_ok = run_client(conc_port, 64, 48, 0.0f, 1.0f, 0.0f, 0, 255, 0); });
        std::thread t2([&] { c2_ok = run_client(conc_port, 40, 30, 0.0f, 0.0f, 1.0f, 0, 0, 255); });
        t1.join();
        t2.join();
        s1_worker.join();
        s2_worker.join();

        Check(c1_ok && s1_ok, "concurrent session 1 (green 64x48) rendered correctly");
        Check(c2_ok && s2_ok, "concurrent session 2 (blue 40x30) rendered correctly");
    }

    std::printf("\n%s\n", g_failures ? "GL SERVER TEST: FAILED" : "GL SERVER TEST: PASSED");
    return g_failures ? 1 : 0;
}
