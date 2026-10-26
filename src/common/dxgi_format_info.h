#pragma once
#ifndef REMOTE_GPU_DXGI_FORMAT_INFO_H
#define REMOTE_GPU_DXGI_FORMAT_INFO_H

#include <cstdint>

namespace RemoteGPU {

/**
 * Memory layout of one 2D subresource, shared by client and server so both agree on exactly how
 * many bytes Direct3D reads from initial or update data.
 *
 * row_bytes: bytes of one row of pixels (or one row of 4x4 blocks for BC formats).
 * rows:      rows of pixels (or of blocks) in the subresource.
 * Direct3D reads row_pitch * (rows - 1) + row_bytes bytes.
 *
 * Returns false for formats whose layout is not handled (planar video formats, unknown values);
 * such textures cannot carry initial data through the proxy.
 */
inline bool SurfaceLayout(uint32_t format, uint32_t width, uint32_t height, uint64_t& row_bytes, uint32_t& rows) {
    if (width == 0 || height == 0) return false;
    uint32_t block_bytes = 0;   // BC formats: bytes per 4x4 block
    uint32_t packed_bytes = 0;  // 2x1 packed formats: bytes per 2-pixel group
    uint32_t bits = 0;          // plain formats: bits per pixel

    switch (format) {
        case 1: case 2: case 3: case 4: bits = 128; break;                         // R32G32B32A32
        case 5: case 6: case 7: case 8: bits = 96; break;                          // R32G32B32
        case 9: case 10: case 11: case 12: case 13: case 14:                       // R16G16B16A16
        case 15: case 16: case 17: case 18:                                        // R32G32
        case 19: case 20: case 21: case 22: bits = 64; break;                      // R32G8X24
        case 23: case 24: case 25: case 26:                                        // R10G10B10A2, R11G11B10
        case 27: case 28: case 29: case 30: case 31: case 32:                      // R8G8B8A8
        case 33: case 34: case 35: case 36: case 37: case 38:                      // R16G16
        case 39: case 40: case 41: case 42: case 43:                               // R32, D32
        case 44: case 45: case 46: case 47:                                        // R24G8, D24S8
        case 67:                                                                   // R9G9B9E5
        case 87: case 88: case 89: case 90: case 91: case 92: case 93:             // B8G8R8A8/X8, XR_BIAS
        case 100: case 101: bits = 32; break;                                      // AYUV, Y410
        case 48: case 49: case 50: case 51: case 52:                               // R8G8
        case 53: case 54: case 55: case 56: case 57: case 58: case 59:             // R16, D16
        case 85: case 86: case 114: case 115: bits = 16; break;                    // B5G6R5, B5G5R5A1, A8P8, B4G4R4A4
        case 60: case 61: case 62: case 63: case 64: case 65:                      // R8, A8
        case 111: case 112: case 113: bits = 8; break;                             // AI44, IA44, P8
        case 66: bits = 1; break;                                                  // R1_UNORM
        case 102: bits = 64; break;                                                // Y416
        case 68: case 69: case 107: packed_bytes = 4; break;                       // R8G8_B8G8, G8R8_G8B8, YUY2
        case 108: case 109: packed_bytes = 8; break;                               // Y210, Y216
        case 70: case 71: case 72: case 79: case 80: case 81: block_bytes = 8; break;   // BC1, BC4
        case 73: case 74: case 75: case 76: case 77: case 78:                      // BC2, BC3
        case 82: case 83: case 84:                                                 // BC5
        case 94: case 95: case 96: case 97: case 98: case 99: block_bytes = 16; break;  // BC6H, BC7
        default: return false;                                                     // planar video, unknown
    }

    if (block_bytes) {
        row_bytes = static_cast<uint64_t>((width + 3) / 4) * block_bytes;
        rows = (height + 3) / 4;
    } else if (packed_bytes) {
        row_bytes = static_cast<uint64_t>((width + 1) / 2) * packed_bytes;
        rows = height;
    } else {
        row_bytes = (static_cast<uint64_t>(width) * bits + 7) / 8;
        rows = height;
    }
    return true;
}

// Mip dimension: max(1, size >> level).
inline uint32_t MipSize(uint32_t size, uint32_t level) {
    const uint32_t s = level < 32 ? size >> level : 0;
    return s ? s : 1;
}

// Number of levels in a full mip chain (what MipLevels == 0 means).
inline uint32_t FullMipCount(uint32_t width, uint32_t height) {
    uint32_t largest = width > height ? width : height, levels = 1;
    while (largest > 1) {
        largest >>= 1;
        ++levels;
    }
    return levels;
}

} // namespace RemoteGPU

#endif // REMOTE_GPU_DXGI_FORMAT_INFO_H
