#pragma once
#ifndef REMOTE_GPU_GL_HOST_DEVICE_H
#define REMOTE_GPU_GL_HOST_DEVICE_H

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

namespace RemoteGPU {

struct GlApi;

/**
 * @brief Server-side OpenGL context bound to the thread that created it.
 *
 * The context lives on a hidden window. Everything is rendered into an offscreen framebuffer
 * object that stands in for the window's default framebuffer, so the result does not depend on
 * window visibility or pixel ownership. The session thread must call every method.
 */
class GlHostDevice {
public:
    GlHostDevice();
    ~GlHostDevice();

    GlHostDevice(const GlHostDevice&) = delete;
    GlHostDevice& operator=(const GlHostDevice&) = delete;
    GlHostDevice(GlHostDevice&&) noexcept;
    GlHostDevice& operator=(GlHostDevice&&) noexcept;

    // major == 0 requests a legacy context; otherwise a versioned one (falling back to legacy
    // when the driver refuses the attributes).
    bool Create(uint32_t major, uint32_t minor, uint32_t profile_mask, uint32_t flags,
                uint32_t width, uint32_t height);
    void Destroy();

    // Recreates the offscreen framebuffer attachments at a new size.
    bool Resize(uint32_t width, uint32_t height);

    // Reads the framebuffer as top-down BGRA8 (width * height * 4 bytes, the byte order GDI blits
    // without conversion), or bottom-up as OpenGL stores it (one copy less) without `top_down`,
    // leaving the application's pixel-pack and framebuffer bindings untouched.
    bool ReadbackFrame(std::vector<uint8_t>& out, bool top_down = true);

    bool IsValid() const noexcept { return m_glrc != nullptr; }
    uint32_t Width() const noexcept { return m_width; }
    uint32_t Height() const noexcept { return m_height; }
    uint32_t Major() const noexcept { return m_major; }
    uint32_t Minor() const noexcept { return m_minor; }
    unsigned Fbo() const noexcept { return m_fbo; }
    const std::string& Renderer() const noexcept { return m_renderer; }

private:
    bool CreateAttachments(uint32_t width, uint32_t height);
    void DestroyAttachments();
    bool LoadApi();

    HWND  m_hwnd = nullptr;
    HDC   m_hdc = nullptr;
    HGLRC m_glrc = nullptr;

    unsigned m_fbo = 0;
    unsigned m_colorRb = 0;
    unsigned m_depthRb = 0;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_major = 0;
    uint32_t m_minor = 0;
    std::string m_renderer;
    std::vector<uint8_t> m_bottomUp;

    std::unique_ptr<GlApi> m_api;
};

} // namespace RemoteGPU

#endif // REMOTE_GPU_GL_HOST_DEVICE_H
