#include "frame_codec.h"

#include <cstring>

namespace RemoteGPU {
namespace FrameCodec {

namespace {

inline uint32_t Load(const uint8_t* p, size_t i) {
    uint32_t v;
    std::memcpy(&v, p + i * 4, 4);
    return v;
}

inline void Store(uint8_t* p, size_t i, uint32_t v) {
    std::memcpy(p + i * 4, &v, 4);
}

void PutVarint(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<uint8_t>(v | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<uint8_t>(v));
}

bool GetVarint(const uint8_t*& p, const uint8_t* end, uint64_t& v) {
    v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (p >= end) return false;
        const uint8_t byte = *p++;
        v |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if (!(byte & 0x80)) return true;
    }
    return false;
}

} // namespace

void Encode(const uint8_t* current, const uint8_t* reference, size_t pixel_count, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(sizeof(CompressedFrameHeader) + pixel_count + 64);

    CompressedFrameHeader header = {};
    header.codec = FRAME_CODEC_RLE_DELTA;
    header.mode = reference ? FRAME_MODE_DELTA : FRAME_MODE_KEY;
    header.raw_size = static_cast<uint32_t>(pixel_count * 4);
    out.resize(sizeof(header));
    std::memcpy(out.data(), &header, sizeof(header));

    auto residual = [&](size_t i) -> uint32_t {
        const uint32_t prediction = reference ? Load(reference, i) : (i ? Load(current, i - 1) : 0u);
        return Load(current, i) ^ prediction;
    };

    size_t i = 0;
    while (i < pixel_count) {
        size_t j = i;
        if (residual(i) == 0) {
            while (j < pixel_count && residual(j) == 0) ++j;
            PutVarint(out, static_cast<uint64_t>(j - i) << 1);
        } else {
            while (j < pixel_count && residual(j) != 0) ++j;
            PutVarint(out, (static_cast<uint64_t>(j - i) << 1) | 1);
            const size_t base = out.size();
            out.resize(base + (j - i) * 4);
            for (size_t k = i; k < j; ++k) Store(out.data() + base, k - i, residual(k));
        }
        i = j;
    }
}

bool Decode(const uint8_t* in, size_t in_len, uint8_t* frame, size_t pixel_count, bool has_reference) {
    if (!in || (!frame && pixel_count) || in_len < sizeof(CompressedFrameHeader)) return false;

    CompressedFrameHeader header;
    std::memcpy(&header, in, sizeof(header));
    if (header.codec != FRAME_CODEC_RLE_DELTA || header.raw_size != pixel_count * 4) return false;
    if (header.mode != FRAME_MODE_KEY && header.mode != FRAME_MODE_DELTA) return false;
    const bool delta = (header.mode == FRAME_MODE_DELTA);
    if (delta && !has_reference) return false;

    const uint8_t* p = in + sizeof(header);
    const uint8_t* end = in + in_len;
    size_t i = 0;
    while (i < pixel_count) {
        uint64_t token;
        if (!GetVarint(p, end, token)) return false;
        const uint64_t count = token >> 1;
        if (count == 0 || count > pixel_count - i) return false;

        if (token & 1) {
            if (static_cast<uint64_t>(end - p) < count * 4) return false;
            for (uint64_t k = 0; k < count; ++k, ++i) {
                const uint32_t prediction = delta ? Load(frame, i) : (i ? Load(frame, i - 1) : 0u);
                Store(frame, i, Load(p, k) ^ prediction);
            }
            p += count * 4;
        } else if (delta) {
            i += count; // unchanged since the previous frame
        } else {
            for (uint64_t k = 0; k < count; ++k, ++i) Store(frame, i, i ? Load(frame, i - 1) : 0u);
        }
    }
    return p == end; // trailing bytes mean a corrupt or mismatched stream
}

} // namespace FrameCodec
} // namespace RemoteGPU
