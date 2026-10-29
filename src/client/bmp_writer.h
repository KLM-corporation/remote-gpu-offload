#pragma once
#ifndef REMOTE_GPU_BMP_WRITER_H
#define REMOTE_GPU_BMP_WRITER_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace RemoteGPU {

// Writes a top-down 32-bit BMP from an RGBA buffer, or a BGRA one when `bgra` is set (used by the
// automated pixel verifier).
inline bool WriteBmp(const char* path, const uint8_t* rgba, uint32_t width, uint32_t height, bool bgra = false) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const uint32_t image_size = width * height * 4;
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    fh.bfType = 0x4D42; // 'BM'
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + image_size;
    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(width);
    ih.biHeight = -static_cast<LONG>(height);
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = image_size;
    std::fwrite(&fh, sizeof(fh), 1, f);
    std::fwrite(&ih, sizeof(ih), 1, f);
    std::vector<uint8_t> row(width * 4);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* src = rgba + static_cast<size_t>(y) * width * 4;
        if (bgra) {
            std::fwrite(src, row.size(), 1, f); // already the BMP byte order
            continue;
        }
        for (uint32_t x = 0; x < width; ++x) {
            row[x * 4 + 0] = src[x * 4 + 2];
            row[x * 4 + 1] = src[x * 4 + 1];
            row[x * 4 + 2] = src[x * 4 + 0];
            row[x * 4 + 3] = src[x * 4 + 3];
        }
        std::fwrite(row.data(), row.size(), 1, f);
    }
    std::fclose(f);
    return true;
}

} // namespace RemoteGPU

#endif // REMOTE_GPU_BMP_WRITER_H
