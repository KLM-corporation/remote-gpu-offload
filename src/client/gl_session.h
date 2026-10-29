#pragma once
#ifndef REMOTE_GPU_GL_SESSION_H
#define REMOTE_GPU_GL_SESSION_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gl_protocol.h"
#include "presentation.h"
#include "transport.h"
#include "video_codec.h"

namespace RemoteGPU {

/**
 * @brief Client side of one remote OpenGL context: connection, command batching and the
 * SwapBuffers round trip that returns the rendered frame.
 *
 * Frames are pipelined: a receiver thread decodes them while the application builds the next
 * ones, and Swap only waits once RGPU_FRAME_LATENCY frames (default 2, 1 = fully synchronous)
 * are queued on the server.
 *
 * Frames come as H.264 video when Media Foundation is available, unless RGPU_VIDEO=0 asks for
 * lossless frames only; RGPU_VIDEO_BITRATE sets the bitrate in Mbit/s (server default 40).
 */
class GlSession {
public:
    GlSession() = default;
    ~GlSession();

    GlSession(const GlSession&) = delete;
    GlSession& operator=(const GlSession&) = delete;

    // Connects (RGPU_SERVER / RGPU_PORT), negotiates the session and creates the host context.
    bool Connect(const GlCreateContextPayload& request);

    // Appends one call to the pending batch; nothing is sent until Flush or Swap.
    void Append(GlFunc func, const void* args, uint32_t size);
    bool Flush();

    // Sends the batch plus the swap and shows the newest frame received in `hwnd`, waiting only
    // while too many frames are in flight.
    bool Swap(HWND hwnd);

    // A one-line throughput summary once every few seconds, empty in between.
    std::string TakePerfReport();

    // Resizes the host framebuffer to the client area of `hwnd` (pending commands are flushed first).
    void ResizeToWindow(HWND hwnd);

    bool IsConnected() const noexcept { return m_socket && m_socket->IsValid() && !m_broken; }
    uint32_t Width() const noexcept { return m_width; }
    uint32_t Height() const noexcept { return m_height; }
    const std::string& Renderer() const noexcept { return m_renderer; }

private:
    using Clock = std::chrono::steady_clock;

    bool SendPacket(uint16_t opcode, const void* payload, uint32_t size);
    void ReceiverLoop();
    // Decodes one OP_FRAME_DATA into m_spare; `picture` is false when the frame carried no new
    // picture (an H.264 encoder still busy).
    bool DecodeFrame(const PacketHeader& header, const std::vector<uint8_t>& payload, bool& picture);
    bool DecodeVideo(const FrameDataPayload& meta, const uint8_t* units, size_t size, bool& picture);

    std::unique_ptr<NetworkSocket> m_socket;
    GdiPresenter m_presenter;
    std::vector<uint8_t> m_batch;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_sequence = 0;
    uint64_t m_frameNumber = 0;
    std::atomic<bool> m_broken{false};
    std::string m_renderer;

    // Receiver thread only.
    std::thread m_receiver;
    std::vector<uint8_t> m_reference;  // last lossless frame, the reference for the next delta frame
    std::unique_ptr<Video::H264Decoder> m_decoder; // created on the first H.264 frame of a size
    uint32_t m_decoderWidth = 0;
    uint32_t m_decoderHeight = 0;
    std::vector<uint8_t> m_spare;      // newest decoded picture, handed over as m_latest
    uint32_t m_spareWidth = 0;
    uint32_t m_spareHeight = 0;
    uint32_t m_spareFormat = 0;

    // Shared with the receiver, under m_frameMutex.
    std::mutex m_frameMutex;
    std::condition_variable m_frameCv;
    std::vector<uint8_t> m_latest;     // newest decoded frame not yet shown
    uint32_t m_latestWidth = 0;
    uint32_t m_latestHeight = 0;
    uint32_t m_latestFormat = 0;
    bool m_hasNewFrame = false;
    bool m_receiverDone = false;
    uint64_t m_framesReceived = 0;
    uint64_t m_bytesReceived = 0;
    double m_decodeMs = 0.0;           // total time spent decoding frames
    const char* m_codec = "none";      // how the last frame arrived

    // Application thread.
    std::vector<uint8_t> m_frame;      // frame on screen
    uint32_t m_frameWidth = 0;
    uint32_t m_frameHeight = 0;
    uint32_t m_frameFormat = 0;        // FRAME_FORMAT_*
    uint64_t m_framesSent = 0;
    uint32_t m_maxInFlight = 2;

    // Throughput counters since the last report.
    Clock::time_point m_perfStart = Clock::now();
    uint64_t m_perfFrames = 0;
    uint64_t m_perfBytesSent = 0;
    uint64_t m_perfBytesReceivedAtStart = 0;
    uint64_t m_perfFramesReceivedAtStart = 0;
    double m_perfDecodeMsAtStart = 0.0;
    double m_perfWaitMs = 0.0;
    double m_perfPresentMs = 0.0;
};

} // namespace RemoteGPU

#endif // REMOTE_GPU_GL_SESSION_H
