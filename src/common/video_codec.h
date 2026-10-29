#pragma once
#ifndef REMOTE_GPU_VIDEO_CODEC_H
#define REMOTE_GPU_VIDEO_CODEC_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace RemoteGPU {
namespace Video {

/**
 * FRAME_CODEC_H264: lossy video for frames that change too much for FRAME_CODEC_RLE_DELTA (a
 * moving camera turns a 7 MB frame into 5 MB of RLE tokens but under 100 KB of H.264).
 *
 * Everything goes through Media Foundation, loaded at run time so that a system without it
 * (Windows N) still runs the lossless codec. The server encodes with the GPU's encoder (NVENC,
 * Quick Sync, AMF) when the driver registers one, otherwise with Microsoft's software encoder;
 * the client decodes with Microsoft's decoder.
 *
 * Frames are coded at their size rounded up to a multiple of 16 (edge pixels repeated into the
 * padding), as full-range BT.709 NV12, without B-frames: every input frame normally comes out
 * as one Annex-B access unit before the next one goes in.
 */

// True when Media Foundation can be loaded on this system.
bool IsAvailable();

// Coded size of a frame dimension.
inline uint32_t CodedSize(uint32_t n) { return (n + 15u) & ~15u; }

// Converts width x height BGRA pixels whose rows are `stride` bytes apart (negative for
// bottom-up rows) into a coded_width x coded_height NV12 image (luma plane followed by the
// interleaved chroma plane, both coded_width bytes per row).
void BgraToNv12(const uint8_t* bgra, ptrdiff_t stride, uint32_t width, uint32_t height,
                uint8_t* nv12, uint32_t coded_width, uint32_t coded_height);

// Converts the top-left width x height of an NV12 image (rows `pitch` bytes apart in both
// planes) into top-down BGRA with opaque alpha.
void Nv12ToBgra(const uint8_t* luma, const uint8_t* chroma, size_t pitch, uint32_t width, uint32_t height,
                uint8_t* bgra);

class H264Encoder {
public:
    H264Encoder();
    ~H264Encoder();
    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    // Creates an encoder for width x height frames. RGPU_VIDEO_ENCODER=software skips the
    // hardware encoders. Must be called on the thread that encodes and destroys.
    bool Create(uint32_t width, uint32_t height, uint32_t bitrate_kbps);
    void Destroy();

    // Encodes one frame of the created size (rows as for BgraToNv12) and appends each access unit
    // the encoder produced, normally just this frame's, to `out` as a 32-bit little-endian length
    // followed by its Annex-B bytes. `key` is set when one of them is an IDR picture (which
    // always carries its parameter sets).
    bool Encode(const uint8_t* bgra, ptrdiff_t stride, std::vector<uint8_t>& out, bool& key);

    bool IsValid() const noexcept;
    uint32_t Width() const noexcept;
    uint32_t Height() const noexcept;
    const std::string& Name() const noexcept; // the encoder's friendly name

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();
    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    // Decodes on the GPU (DXVA) when possible, unless RGPU_VIDEO_DECODER=software. Must be called
    // on the thread that decodes and destroys.
    bool Create();
    void Destroy();
    bool IsHardware() const noexcept;

    // Decodes one access unit (Annex-B bytes). When a picture comes out, its top-left width x
    // height is written to `bgra` (width * height * 4 bytes) and `picture` is set. False on a
    // stream the decoder rejects or a picture smaller than width x height.
    bool Decode(const uint8_t* data, size_t size, uint32_t width, uint32_t height, uint8_t* bgra, bool& picture);

    bool IsValid() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace Video
} // namespace RemoteGPU

#endif // REMOTE_GPU_VIDEO_CODEC_H
