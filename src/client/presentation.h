#pragma once
#ifndef REMOTE_GPU_PRESENTATION_H
#define REMOTE_GPU_PRESENTATION_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdint>
#include <vector>

namespace RemoteGPU {

/**
 * @brief High-performance Win32 GDI Presentation Engine.
 * Blits uncompressed RGBA/BGRA framebuffers directly into application HWND
 * using GDI Device Independent Bitmaps (DIBs), requiring ZERO hardware 3D
 * acceleration on the client.
 */
class GdiPresenter {
public:
    GdiPresenter();
    ~GdiPresenter();

    GdiPresenter(const GdiPresenter&) = delete;
    GdiPresenter& operator=(const GdiPresenter&) = delete;

    /**
     * @brief Optional initialization for caching default window handle and initial target dimensions.
     */
    bool Initialize(HWND hwnd, uint32_t width, uint32_t height);

    /**
     * @brief Blits incoming frame pixels to target window HWND.
     * @param hwnd Target Win32 window handle.
     * @param rgba_data Pointer to raw pixel buffer.
     * @param width Frame width in pixels.
     * @param height Frame height in pixels.
     * @param row_pitch Row stride in bytes (if 0, defaults to width * 4).
     * @return true on successful blit; false on invalid HWND or GDI error.
     */
    bool PresentFrame(HWND hwnd, const uint8_t* rgba_data, uint32_t width, uint32_t height, uint32_t row_pitch = 0);

    /**
     * @brief Alternative presentation signature accepting DXGI format.
     */
    bool Present(HWND hwnd, const uint8_t* rgba_data, uint32_t width, uint32_t height, uint32_t format = 28);

    /**
     * @brief Releases any cached presentation resources.
     */
    void Shutdown();

    // Diagnostics & Metrics
    uint64_t GetPresentedFrames() const noexcept { return m_presentedFrames; }
    double GetLastPresentDurationMs() const noexcept { return m_lastPresentMs; }

    // Color conversion mode toggle
    void SetForceBgraConversion(bool enable) noexcept { m_forceBgraConversion = enable; }
    bool GetForceBgraConversion() const noexcept { return m_forceBgraConversion; }

private:
    static void ConvertRgbaToBgra(const uint8_t* src, uint8_t* dst, size_t pixel_count);

    HWND                 m_hwnd{nullptr};
    uint32_t             m_width{0};
    uint32_t             m_height{0};
    std::vector<uint8_t> m_conversionBuffer;
    uint64_t             m_presentedFrames{0};
    double               m_lastPresentMs{0.0};
    bool                 m_forceBgraConversion{false};
};

} // namespace RemoteGPU

#endif // REMOTE_GPU_PRESENTATION_H
