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
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "gl_protocol.h"
#include "presentation.h"
#include "transport.h"

namespace RemoteGPU {

/**
 * @brief Client side of one remote OpenGL context: connection, command batching and the
 * SwapBuffers round trip that returns the rendered frame.
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

    // Sends the batch plus the swap, waits for the frame and shows it in `hwnd`.
    bool Swap(HWND hwnd);

    // Resizes the host framebuffer to the client area of `hwnd` (pending commands are flushed first).
    void ResizeToWindow(HWND hwnd);

    bool IsConnected() const noexcept { return m_socket && m_socket->IsValid() && !m_broken; }
    uint32_t Width() const noexcept { return m_width; }
    uint32_t Height() const noexcept { return m_height; }
    const std::string& Renderer() const noexcept { return m_renderer; }

private:
    bool SendPacket(uint16_t opcode, const void* payload, uint32_t size);
    bool ReceiveFrame();

    std::unique_ptr<NetworkSocket> m_socket;
    GdiPresenter m_presenter;
    std::vector<uint8_t> m_batch;
    std::vector<uint8_t> m_frame;     // last decoded frame, also the delta reference
    uint32_t m_frameWidth = 0;
    uint32_t m_frameHeight = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_sequence = 0;
    uint64_t m_frameNumber = 0;
    bool m_broken = false;
    std::string m_renderer;
};

} // namespace RemoteGPU

#endif // REMOTE_GPU_GL_SESSION_H
