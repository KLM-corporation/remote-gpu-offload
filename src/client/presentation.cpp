#include "presentation.h"

#include <iostream>
#include <chrono>
#include <cstring>

namespace RemoteGPU {

namespace {

#pragma pack(push, 1)
struct DibInfoBitfields {
    BITMAPINFOHEADER bmiHeader;
    DWORD            bmiColors[3];
};
#pragma pack(pop)

} // anonymous namespace

GdiPresenter::GdiPresenter() = default;

GdiPresenter::~GdiPresenter() {
    Shutdown();
}

bool GdiPresenter::Initialize(HWND hwnd, uint32_t width, uint32_t height) {
    m_hwnd = hwnd;
    m_width = width;
    m_height = height;
    return true;
}

void GdiPresenter::Shutdown() {
    m_hwnd = nullptr;
    m_width = 0;
    m_height = 0;
    m_conversionBuffer.clear();
}

void GdiPresenter::ConvertRgbaToBgra(const uint8_t* src, uint8_t* dst, size_t pixel_count) {
    const auto* src32 = reinterpret_cast<const uint32_t*>(src);
    auto* dst32 = reinterpret_cast<uint32_t*>(dst);

    for (size_t i = 0; i < pixel_count; ++i) {
        uint32_t p = src32[i];
        // RGBA (Little Endian in memory: R at byte 0, G at byte 1, B at byte 2, A at byte 3)
        // -> BGRA (B at byte 0, G at byte 1, R at byte 2, A at byte 3)
        dst32[i] = (p & 0xFF00FF00u) | ((p & 0x00FF0000u) >> 16) | ((p & 0x000000FFu) << 16);
    }
}

bool GdiPresenter::PresentFrame(HWND hwnd, const uint8_t* rgba_data, uint32_t width, uint32_t height, uint32_t /*row_pitch*/) {
    if (!hwnd || !IsWindow(hwnd) || !rgba_data || width == 0 || height == 0) {
        return false;
    }

    auto t_start = std::chrono::high_resolution_clock::now();

    HDC hdc = GetDC(hwnd);
    if (!hdc) {
        return false;
    }

    RECT clientRect = {};
    GetClientRect(hwnd, &clientRect);
    int clientWidth = clientRect.right - clientRect.left;
    int clientHeight = clientRect.bottom - clientRect.top;

    bool blit_result = false;
    const uint8_t* pPixelsToBlit = rgba_data;

    if (m_forceBgraConversion) {
        size_t totalBytes = static_cast<size_t>(width) * height * 4;
        if (m_conversionBuffer.size() < totalBytes) {
            m_conversionBuffer.resize(totalBytes);
        }
        ConvertRgbaToBgra(rgba_data, m_conversionBuffer.data(), static_cast<size_t>(width) * height);
        pPixelsToBlit = m_conversionBuffer.data();

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = static_cast<LONG>(width);
        bi.bmiHeader.biHeight = -static_cast<LONG>(height); // Top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bi.bmiHeader.biSizeImage = static_cast<DWORD>(totalBytes);

        if (clientWidth == static_cast<int>(width) && clientHeight == static_cast<int>(height)) {
            int lines = SetDIBitsToDevice(
                hdc,
                0, 0,
                width, height,
                0, 0,
                0, height,
                pPixelsToBlit,
                &bi,
                DIB_RGB_COLORS
            );
            blit_result = (lines > 0);
        } else {
            SetStretchBltMode(hdc, COLORONCOLOR);
            int lines = StretchDIBits(
                hdc,
                0, 0, clientWidth, clientHeight,
                0, 0, width, height,
                pPixelsToBlit,
                &bi,
                DIB_RGB_COLORS,
                SRCCOPY
            );
            blit_result = (lines > 0);
        }
    } else {
        DibInfoBitfields dib = {};
        dib.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        dib.bmiHeader.biWidth = static_cast<LONG>(width);
        dib.bmiHeader.biHeight = -static_cast<LONG>(height); // Top-down DIB
        dib.bmiHeader.biPlanes = 1;
        dib.bmiHeader.biBitCount = 32;
        dib.bmiHeader.biCompression = BI_BITFIELDS;
        dib.bmiHeader.biSizeImage = width * height * 4;

        // DXGI_FORMAT_R8G8B8A8_UNORM channel bitmasks:
        dib.bmiColors[0] = 0x000000FF; // Red
        dib.bmiColors[1] = 0x0000FF00; // Green
        dib.bmiColors[2] = 0x00FF0000; // Blue

        if (clientWidth == static_cast<int>(width) && clientHeight == static_cast<int>(height)) {
            int lines = SetDIBitsToDevice(
                hdc,
                0, 0,
                width, height,
                0, 0,
                0, height,
                pPixelsToBlit,
                reinterpret_cast<const BITMAPINFO*>(&dib),
                DIB_RGB_COLORS
            );
            blit_result = (lines > 0);
        } else {
            SetStretchBltMode(hdc, COLORONCOLOR);
            int lines = StretchDIBits(
                hdc,
                0, 0, clientWidth, clientHeight,
                0, 0, width, height,
                pPixelsToBlit,
                reinterpret_cast<const BITMAPINFO*>(&dib),
                DIB_RGB_COLORS,
                SRCCOPY
            );
            blit_result = (lines > 0);
        }
    }

    ReleaseDC(hwnd, hdc);

    auto t_end = std::chrono::high_resolution_clock::now();
    m_lastPresentMs = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    if (blit_result) {
        m_presentedFrames++;
    }

    return blit_result;
}

bool GdiPresenter::Present(HWND hwnd, const uint8_t* rgba_data, uint32_t width, uint32_t height, uint32_t /*format*/) {
    return PresentFrame(hwnd, rgba_data, width, height, width * 4);
}

} // namespace RemoteGPU
