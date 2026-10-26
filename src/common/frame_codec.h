#pragma once
#ifndef REMOTE_GPU_FRAME_CODEC_H
#define REMOTE_GPU_FRAME_CODEC_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "protocol.h"

namespace RemoteGPU {
namespace FrameCodec {

/**
 * FRAME_CODEC_RLE_DELTA: lossless codec for 32-bit pixel frames, no external dependency.
 *
 * Each pixel is XORed with a prediction: the same pixel of the previous frame (delta mode)
 * or the pixel to its left (key mode). Unchanged areas and flat colours become zeros.
 * The residual words are written as tokens: LEB128 varint of (count << 1 | is_literal),
 * followed by count raw 32-bit words for literals; zero runs carry no data.
 *
 * Output layout: CompressedFrameHeader followed by the token stream.
 */

// Encodes `pixel_count` pixels of `current`. `reference` is the previous frame (same size) for a
// delta frame, or nullptr for a key frame. Replaces the contents of `out`.
void Encode(const uint8_t* current, const uint8_t* reference, size_t pixel_count, std::vector<uint8_t>& out);

// Decodes `in` (header + tokens) into `frame` in place. For a delta frame, `frame` must hold the
// previous decoded frame and `has_reference` must be true. Returns false on any malformed or
// mismatching input; `frame` may then be partially overwritten.
bool Decode(const uint8_t* in, size_t in_len, uint8_t* frame, size_t pixel_count, bool has_reference);

} // namespace FrameCodec
} // namespace RemoteGPU

#endif // REMOTE_GPU_FRAME_CODEC_H
