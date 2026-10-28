#include "gl_session.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "bmp_writer.h"
#include "frame_codec.h"

namespace RemoteGPU {

namespace {

// Reads through the Win32 API: this DLL is linked statically, so std::getenv would only see the
// environment as it was when the DLL loaded, not variables the application sets afterwards.
std::string EnvVar(const char* name) {
    char buffer[512];
    const DWORD n = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    return (n > 0 && n < sizeof(buffer)) ? std::string(buffer, n) : std::string();
}

std::string ServerHost() {
    std::string host = EnvVar("RGPU_SERVER");
    if (!host.empty()) return host;

    const char* ini_paths[] = {
        "rgpu.ini",
        "C:\\Users\\LHRIC\\curseforge\\minecraft\\Instances\\test\\rgpu.ini",
        "C:\\Users\\LHRIC\\curseforge\\minecraft\\Install\\java\\Jre_21\\bin\\rgpu.ini"
    };
    for (const char* p : ini_paths) {
        FILE* f = std::fopen(p, "r");
        if (f) {
            char buf[128] = {};
            if (std::fgets(buf, sizeof(buf), f)) {
                char* e = std::strchr(buf, '\r'); if (e) *e = 0;
                e = std::strchr(buf, '\n'); if (e) *e = 0;
                if (buf[0]) {
                    host = buf;
                    std::fclose(f);
                    return host;
                }
            }
            std::fclose(f);
        }
    }

    // Default to the dedicated GPU server for Minecraft/Java processes, or loopback for generic tests
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    std::string exe_str = exe;
    for (char& c : exe_str) c = (char)std::tolower(c);
    if (exe_str.find("java") != std::string::npos || exe_str.find("minecraft") != std::string::npos) {
        return "192.168.1.17";
    }

    return "127.0.0.1";
}

uint16_t ServerPort() {
    const std::string port = EnvVar("RGPU_PORT");
    if (!port.empty()) {
        const long value = std::strtol(port.c_str(), nullptr, 10);
        if (value > 0 && value <= 65535) return static_cast<uint16_t>(value);
    }

    // Allow the per-instance rgpu.ini to carry the port on its second line.
    // Environment variables still take precedence for tests and explicit launches.
    const char* ini_paths[] = {
        "rgpu.ini",
        "C:\\Users\\LHRIC\\curseforge\\minecraft\\Instances\\test\\rgpu.ini",
        "C:\\Users\\LHRIC\\curseforge\\minecraft\\Install\\java\\Jre_21\\bin\\rgpu.ini"
    };
    for (const char* p : ini_paths) {
        FILE* f = std::fopen(p, "r");
        if (!f) continue;
        char line[128] = {};
        std::fgets(line, sizeof(line), f);
        if (std::fgets(line, sizeof(line), f)) {
            char* e = std::strpbrk(line, "\r\n");
            if (e) *e = 0;
            const long value = std::strtol(line, nullptr, 10);
            std::fclose(f);
            if (value > 0 && value <= 65535) return static_cast<uint16_t>(value);
        } else {
            std::fclose(f);
        }
    }
    return RGPU_DEFAULT_PORT;
}

// A batch this large is sent without waiting for the swap.
constexpr size_t kBatchLimit = 256 * 1024;

} // namespace

GlSession::~GlSession() {
    if (IsConnected()) {
        ShutdownPayload p = {};
        SendPacket(OP_SHUTDOWN, &p, sizeof(p));
    }
    m_presenter.Shutdown();
}

bool GlSession::SendPacket(uint16_t opcode, const void* payload, uint32_t size) {
    PacketHeader h = {};
    h.magic = RGPU_MAGIC;
    h.opcode = opcode;
    h.sequence_id = ++m_sequence;
    h.payload_length = size;
    if (!m_socket->SendPacket(h, size ? payload : nullptr)) {
        m_broken = true;
        return false;
    }
    return true;
}

bool GlSession::Connect(const GlCreateContextPayload& request) {
    if (!NetworkSocket::InitializeNetwork()) return false;
    auto socket = std::make_unique<NetworkSocket>();
    if (!socket->Connect(ServerHost().c_str(), ServerPort())) {
        std::cerr << "[RemoteGPU] Cannot reach the GPU server at " << ServerHost() << ":" << ServerPort() << std::endl;
        return false;
    }
    m_socket = std::move(socket);
    m_socket->SetTimeouts(10000, 0);
    m_socket->EnableKeepAlive(5000, 1000);

    HandshakeRequestPayload hello = {};
    hello.protocol_version = RGPU_PROTOCOL_VERSION;
    hello.client_flags = CLIENT_FLAG_API_OPENGL;
    if (EnvVar("RGPU_COMPRESSION") != "0") hello.client_flags |= CLIENT_FLAG_FRAME_COMPRESSION;
    hello.client_width = request.width;
    hello.client_height = request.height;
    hello.preferred_format = 28;

    PacketHeader h = {};
    std::vector<uint8_t> payload;
    if (!SendPacket(OP_HANDSHAKE_REQUEST, &hello, sizeof(hello)) || !m_socket->RecvPacket(h, payload) ||
        h.opcode != OP_HANDSHAKE_RESPONSE || payload.size() < sizeof(HandshakeResponsePayload)) {
        m_broken = true;
        return false;
    }
    HandshakeResponsePayload hs;
    std::memcpy(&hs, payload.data(), sizeof(hs));
    if (hs.status_code != 0) {
        std::cerr << "[RemoteGPU] Server refused the session (status " << hs.status_code << ")" << std::endl;
        m_broken = true;
        return false;
    }

    if (!SendPacket(OP_GL_CREATE_CONTEXT, &request, sizeof(request)) || !m_socket->RecvPacket(h, payload) ||
        h.opcode != OP_GL_CONTEXT_RESPONSE || payload.size() < sizeof(GlContextResponsePayload)) {
        m_broken = true;
        return false;
    }
    GlContextResponsePayload ctx;
    std::memcpy(&ctx, payload.data(), sizeof(ctx));
    if (ctx.status != 0) {
        std::cerr << "[RemoteGPU] Server could not create the OpenGL context (status " << ctx.status << ")" << std::endl;
        m_broken = true;
        return false;
    }
    ctx.renderer[sizeof(ctx.renderer) - 1] = '\0';
    m_renderer = ctx.renderer;
    m_width = request.width;
    m_height = request.height;
    return true;
}

void GlSession::Append(GlFunc func, const void* args, uint32_t size) {
    if (m_broken) return;
    GlCommandHeader cmd = {};
    cmd.func = func;
    cmd.size = size;
    const auto* h = reinterpret_cast<const uint8_t*>(&cmd);
    m_batch.insert(m_batch.end(), h, h + sizeof(cmd));
    if (size) {
        const auto* a = static_cast<const uint8_t*>(args);
        m_batch.insert(m_batch.end(), a, a + size);
    }
    if (m_batch.size() >= kBatchLimit) Flush();
}

bool GlSession::Flush() {
    if (m_broken) return false;
    if (m_batch.empty()) return true;
    const bool ok = SendPacket(OP_GL_COMMANDS, m_batch.data(), static_cast<uint32_t>(m_batch.size()));
    m_batch.clear();
    return ok;
}

bool GlSession::ReceiveFrame() {
    PacketHeader h = {};
    std::vector<uint8_t> payload;
    if (!m_socket->RecvPacket(h, payload)) { m_broken = true; return false; }
    if (h.opcode != OP_FRAME_DATA || payload.size() < sizeof(FrameDataPayload)) { m_broken = true; return false; }

    FrameDataPayload meta;
    std::memcpy(&meta, payload.data(), sizeof(meta));
    if (meta.width == 0 || meta.height == 0 || meta.width > GL_MAX_FRAMEBUFFER_DIM || meta.height > GL_MAX_FRAMEBUFFER_DIM) {
        m_broken = true;
        return false;
    }
    const size_t pixel_count = static_cast<size_t>(meta.width) * meta.height;
    const size_t expected = pixel_count * 4;
    const uint8_t* body = payload.data() + sizeof(meta);
    const size_t body_size = payload.size() - sizeof(meta);

    bool ok;
    if (h.flags & FLAG_COMPRESSED) {
        const bool has_reference = m_frame.size() == expected;
        if (!has_reference) m_frame.assign(expected, 0);
        ok = meta.data_size == body_size && FrameCodec::Decode(body, body_size, m_frame.data(), pixel_count, has_reference);
    } else {
        ok = meta.data_size >= expected && body_size >= expected;
        if (ok) m_frame.assign(body, body + expected);
    }
    if (!ok) {
        std::cerr << "[RemoteGPU] Corrupt frame " << meta.frame_number << std::endl;
        m_broken = true;
        return false;
    }
    m_frameWidth = meta.width;
    m_frameHeight = meta.height;
    return true;
}

void GlSession::ResizeToWindow(HWND hwnd) {
    RECT rc = {};
    if (!hwnd || !GetClientRect(hwnd, &rc)) return;
    const uint32_t width = static_cast<uint32_t>(rc.right - rc.left);
    const uint32_t height = static_cast<uint32_t>(rc.bottom - rc.top);
    if (width == 0 || height == 0 || width > GL_MAX_FRAMEBUFFER_DIM || height > GL_MAX_FRAMEBUFFER_DIM) return;
    if (width == m_width && height == m_height) return;
    GlResizePayload p = {width, height};
    if (SendPacket(OP_GL_RESIZE, &p, sizeof(p))) {
        m_width = width;
        m_height = height;
    }
}

bool GlSession::Swap(HWND hwnd) {
    if (!IsConnected()) return false;
    if (!Flush()) return false;

    GlSwapPayload swap = {++m_frameNumber};
    if (!SendPacket(OP_GL_SWAP, &swap, sizeof(swap))) return false;
    if (!ReceiveFrame()) return false;

    if (hwnd) m_presenter.PresentFrame(hwnd, m_frame.data(), m_frameWidth, m_frameHeight);
    const std::string capture = EnvVar("RGPU_CAPTURE_FILE");
    if (!capture.empty()) WriteBmp(capture.c_str(), m_frame.data(), m_frameWidth, m_frameHeight);
    ResizeToWindow(hwnd);
    return true;
}

} // namespace RemoteGPU
