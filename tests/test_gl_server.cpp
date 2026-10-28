// Zero-trust tests for the OpenGL server path: a real GlServerDispatcher runs in a thread behind a
// loopback socket and receives well-formed and hostile packets from a raw client.
// Usage: test_gl_server [base_port]   (each case uses its own port)

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
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

    bool Handshake(uint32_t flags = CLIENT_FLAG_API_OPENGL) {
        HandshakeRequestPayload hello = {};
        hello.protocol_version = RGPU_PROTOCOL_VERSION;
        hello.client_flags = flags;
        hello.client_width = 64;
        hello.client_height = 48;
        PacketHeader h;
        std::vector<uint8_t> p;
        return Send(OP_HANDSHAKE_REQUEST, &hello, sizeof(hello)) && Receive(h, p) && h.opcode == OP_HANDSHAKE_RESPONSE;
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
    ok = ok && c.SessionEnded();
    Check(ok, label);
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
                const uint8_t* mid = &pixels[(24 * 64 + 32) * 4];
                Check(decoded && mid[0] == 0 && mid[1] == 255 && mid[2] == 0, "centre pixel is the clear colour");
            }
        }
        Check(c.m_client.IsValid(), "client connection stayed open");
    }

    std::printf("[gl_server] hostile input\n");
    {
        std::vector<uint8_t> b;

        b = {1, 0, 0};  // shorter than a command header
        ExpectRejected(port++, "truncated command header", b);

        b.clear();
        AddCommand(b, GLF_Clear, nullptr, 0, 0, 4096);  // declares more argument bytes than the batch holds
        ExpectRejected(port++, "command size beyond the batch", b);

        b.clear();
        AddCommand(b, 9999, nullptr, 0);
        ExpectRejected(port++, "unknown function id", b);

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
