#include "gl_session.h"

#include <algorithm>
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
    if (m_receiver.joinable()) {
        // Queued data (OP_SHUTDOWN) is still delivered; the blocked recv in the receiver returns.
        if (m_socket && m_socket->IsValid()) ::shutdown(m_socket->GetRawSocket(), SD_BOTH);
        m_receiver.join();
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
    m_perfBytesSent += sizeof(h) + size;
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
    HandshakeVideoPayload video = {};
    const bool want_video = EnvVar("RGPU_VIDEO") != "0" && Video::IsAvailable();
    if (want_video) {
        hello.client_flags |= CLIENT_FLAG_VIDEO_H264;
        const long mbps = std::strtol(EnvVar("RGPU_VIDEO_BITRATE").c_str(), nullptr, 10);
        if (mbps > 0 && mbps <= 200) video.bitrate_kbps = static_cast<uint32_t>(mbps) * 1000;
    }
    uint8_t request_bytes[sizeof(hello) + sizeof(video)];
    std::memcpy(request_bytes, &hello, sizeof(hello));
    std::memcpy(request_bytes + sizeof(hello), &video, sizeof(video));
    const uint32_t request_size = static_cast<uint32_t>(sizeof(hello) + (want_video ? sizeof(video) : 0));

    PacketHeader h = {};
    std::vector<uint8_t> payload;
    if (!SendPacket(OP_HANDSHAKE_REQUEST, request_bytes, request_size) || !m_socket->RecvPacket(h, payload) ||
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

    // RGPU_FRAME_LATENCY: frames allowed in flight (1 = wait for every frame before returning).
    const std::string latency = EnvVar("RGPU_FRAME_LATENCY");
    if (!latency.empty()) {
        const long value = std::strtol(latency.c_str(), nullptr, 10);
        if (value >= 1 && value <= 8) m_maxInFlight = static_cast<uint32_t>(value);
    }
    // An older server sends RGBA frames: convert them here rather than through slow GDI masks.
    m_presenter.SetForceBgraConversion(true);
    // From here on the server only sends frames.
    m_receiver = std::thread(&GlSession::ReceiverLoop, this);
    m_perfStart = Clock::now();
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

bool GlSession::DecodeFrame(const PacketHeader& h, const std::vector<uint8_t>& payload, bool& picture) {
    picture = false;
    if (h.opcode != OP_FRAME_DATA || payload.size() < sizeof(FrameDataPayload)) return false;

    FrameDataPayload meta;
    std::memcpy(&meta, payload.data(), sizeof(meta));
    if (meta.width == 0 || meta.height == 0 || meta.width > GL_MAX_FRAMEBUFFER_DIM || meta.height > GL_MAX_FRAMEBUFFER_DIM) {
        return false;
    }
    const size_t pixel_count = static_cast<size_t>(meta.width) * meta.height;
    const size_t expected = pixel_count * 4;
    const uint8_t* body = payload.data() + sizeof(meta);
    const size_t body_size = payload.size() - sizeof(meta);

    CompressedFrameHeader codec = {};
    const bool compressed = (h.flags & FLAG_COMPRESSED) != 0;
    if (compressed && body_size >= sizeof(codec)) std::memcpy(&codec, body, sizeof(codec));
    if (compressed && codec.codec == FRAME_CODEC_H264) {
        if (meta.data_size != body_size || codec.raw_size != expected ||
            !DecodeVideo(meta, body + sizeof(codec), body_size - sizeof(codec), picture)) {
            std::cerr << "[RemoteGPU] Corrupt video frame " << meta.frame_number << std::endl;
            return false;
        }
        m_reference.clear(); // the server sends a key frame before any lossless delta
        return true;
    }

    bool ok;
    if (compressed) {
        // Frames arrive in order, so the reference is always the previous decoded frame.
        const bool has_reference = m_reference.size() == expected;
        if (!has_reference) m_reference.assign(expected, 0);
        ok = meta.data_size == body_size && FrameCodec::Decode(body, body_size, m_reference.data(), pixel_count, has_reference);
    } else {
        ok = meta.data_size >= expected && body_size >= expected;
        if (ok) m_reference.assign(body, body + expected);
    }
    if (!ok) {
        std::cerr << "[RemoteGPU] Corrupt frame " << meta.frame_number << std::endl;
        return false;
    }
    // Copied, since the reference must stay intact for the next delta frame.
    m_spare.assign(m_reference.begin(), m_reference.end());
    m_spareWidth = meta.width;
    m_spareHeight = meta.height;
    m_spareFormat = meta.format;
    picture = true;
    return true;
}

// Decodes the length-prefixed access units of a FRAME_CODEC_H264 frame into m_spare.
bool GlSession::DecodeVideo(const FrameDataPayload& meta, const uint8_t* units, size_t size, bool& picture) {
    if (!m_decoder || meta.width != m_decoderWidth || meta.height != m_decoderHeight) {
        // A new size means a new stream (the server starts it with an IDR picture).
        if (!m_decoder) m_decoder = std::make_unique<Video::H264Decoder>();
        if (!m_decoder->Create()) {
            std::cerr << "[RemoteGPU] No H.264 decoder on this system; set RGPU_VIDEO=0" << std::endl;
            return false;
        }
        m_decoderWidth = meta.width;
        m_decoderHeight = meta.height;
    }
    m_spare.resize(static_cast<size_t>(meta.width) * meta.height * 4);
    size_t pos = 0;
    while (pos < size) {
        uint32_t length = 0;
        if (size - pos < sizeof(length)) return false;
        std::memcpy(&length, units + pos, sizeof(length));
        pos += sizeof(length);
        if (length == 0 || length > size - pos) return false;
        bool decoded = false;
        if (!m_decoder->Decode(units + pos, length, meta.width, meta.height, m_spare.data(), decoded)) return false;
        picture = picture || decoded;
        pos += length;
    }
    if (picture) {
        m_spareWidth = meta.width;
        m_spareHeight = meta.height;
        m_spareFormat = FRAME_FORMAT_BGRA8;
    }
    return true;
}

void GlSession::ReceiverLoop() {
    PacketHeader h = {};
    std::vector<uint8_t> payload;
    while (m_socket->RecvPacket(h, payload)) {
        if (h.opcode == OP_SHUTDOWN) break;
        const Clock::time_point decode_start = Clock::now();
        bool picture = false;
        if (!DecodeFrame(h, payload, picture)) {
            m_broken = true;
            break;
        }
        const double decode_ms = std::chrono::duration<double, std::milli>(Clock::now() - decode_start).count();
        const char* codec = !(h.flags & FLAG_COMPRESSED) ? "raw"
                            : !m_reference.empty()       ? "lossless" // video frames clear the reference
                            : m_decoder->IsHardware()    ? "h264 gpu"
                                                         : "h264 cpu";
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            if (picture) {
                m_latest.swap(m_spare);
                m_latestWidth = m_spareWidth;
                m_latestHeight = m_spareHeight;
                m_latestFormat = m_spareFormat;
                m_hasNewFrame = true;
            }
            ++m_framesReceived; // a frame without a picture still answers its swap
            m_bytesReceived += sizeof(h) + payload.size();
            m_decodeMs += decode_ms;
            m_codec = codec;
        }
        m_frameCv.notify_all();
    }
    m_decoder.reset(); // on the thread that created it (COM)
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_receiverDone = true;
    }
    m_frameCv.notify_all();
}

void GlSession::ResizeToWindow(HWND hwnd) {
    RECT rc = {};
    if (!hwnd || !GetClientRect(hwnd, &rc)) return;
    const uint32_t width = static_cast<uint32_t>(rc.right - rc.left);
    const uint32_t height = static_cast<uint32_t>(rc.bottom - rc.top);
    if (width == 0 || height == 0 || width > GL_MAX_FRAMEBUFFER_DIM || height > GL_MAX_FRAMEBUFFER_DIM) return;
    if (width == m_width && height == m_height) return;
    Flush();
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
    ++m_framesSent;

    // Wait only while m_maxInFlight frames are still being rendered or transferred, then take the
    // newest frame that arrived (with a latency of 1 that is the frame of this very swap).
    const Clock::time_point wait_start = Clock::now();
    bool show = false;
    {
        std::unique_lock<std::mutex> lock(m_frameMutex);
        m_frameCv.wait(lock, [this] { return m_framesSent - m_framesReceived < m_maxInFlight || m_receiverDone; });
        if (m_framesSent - m_framesReceived >= m_maxInFlight) {
            m_broken = true; // the receiver stopped: connection lost or corrupt frame
            return false;
        }
        if (m_hasNewFrame) {
            m_frame.swap(m_latest);
            m_frameWidth = m_latestWidth;
            m_frameHeight = m_latestHeight;
            m_frameFormat = m_latestFormat;
            m_hasNewFrame = false;
            show = true;
        }
    }
    const Clock::time_point present_start = Clock::now();

    if (show) {
        if (hwnd) m_presenter.Present(hwnd, m_frame.data(), m_frameWidth, m_frameHeight, m_frameFormat);
        const std::string capture = EnvVar("RGPU_CAPTURE_FILE");
        if (!capture.empty()) {
            WriteBmp(capture.c_str(), m_frame.data(), m_frameWidth, m_frameHeight, m_frameFormat == FRAME_FORMAT_BGRA8);
        }
    }
    ResizeToWindow(hwnd);

    const Clock::time_point end = Clock::now();
    m_perfWaitMs += std::chrono::duration<double, std::milli>(present_start - wait_start).count();
    m_perfPresentMs += std::chrono::duration<double, std::milli>(end - present_start).count();
    ++m_perfFrames;
    return true;
}

std::string GlSession::TakePerfReport() {
    const Clock::time_point now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - m_perfStart).count();
    if (seconds < 5.0 || m_perfFrames == 0) return std::string();

    uint64_t received = 0;
    uint64_t frames_received = 0;
    double decode_ms = 0.0;
    const char* codec = "";
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        received = m_bytesReceived;
        frames_received = m_framesReceived;
        decode_ms = m_decodeMs;
        codec = m_codec;
    }
    const double frames = static_cast<double>(m_perfFrames);
    const double decoded = std::max(1.0, static_cast<double>(frames_received - m_perfFramesReceivedAtStart));
    char line[320];
    std::snprintf(line, sizeof(line),
                  "perf: %.1f fps at %ux%u, up %.0f KB/frame, down %.0f KB/frame (%s), decode %.1f ms/frame, "
                  "waiting for frames %.1f ms/frame, present %.1f ms/frame, latency %u",
                  frames / seconds, m_width, m_height, m_perfBytesSent / frames / 1024.0,
                  (received - m_perfBytesReceivedAtStart) / frames / 1024.0, codec,
                  (decode_ms - m_perfDecodeMsAtStart) / decoded, m_perfWaitMs / frames, m_perfPresentMs / frames,
                  m_maxInFlight);
    m_perfStart = now;
    m_perfFrames = 0;
    m_perfBytesSent = 0;
    m_perfBytesReceivedAtStart = received;
    m_perfFramesReceivedAtStart = frames_received;
    m_perfDecodeMsAtStart = decode_ms;
    m_perfWaitMs = 0.0;
    m_perfPresentMs = 0.0;
    return line;
}

} // namespace RemoteGPU
