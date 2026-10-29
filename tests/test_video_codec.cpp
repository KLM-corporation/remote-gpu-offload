// Unit tests for the H.264 video path (src/common/video_codec.*): colour conversions, then a
// moving scene through every encoder this machine offers (hardware first, then
// RGPU_VIDEO_ENCODER=software) and back through the decoder, with timings at 1920x974.
// Build: clang++ -std=c++17 -O2 -static -Isrc/common src/common/video_codec.cpp tests/test_video_codec.cpp -o build/test_video_codec.exe -lole32

#include "video_codec.h"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace RemoteGPU;

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (cond) {                                       \
            std::printf("[PASS] ");                       \
        } else {                                          \
            std::printf("[FAIL] (line %d) ", __LINE__);   \
            ++g_failures;                                 \
        }                                                 \
        std::printf(__VA_ARGS__);                         \
        std::printf("\n");                                \
    } while (0)

using Clock = std::chrono::steady_clock;
using Frame = std::vector<uint8_t>;

double Ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

void Put(Frame& f, uint32_t w, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t* p = &f[(static_cast<size_t>(y) * w + x) * 4];
    p[0] = b;
    p[1] = g;
    p[2] = r;
    p[3] = 255;
}

// A textured backdrop scrolling with `t` (like a turning camera) and three flat squares whose
// colours the decoded frame must keep.
Frame Scene(uint32_t w, uint32_t h, int t) {
    Frame f(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t sx = x + t * 13;
            const uint8_t v = static_cast<uint8_t>(((sx / 8 + y / 8) & 1) ? 90 + (sx * 7 + y * 3) % 40 : 40 + (sx ^ y) % 30);
            Put(f, w, x, y, v, static_cast<uint8_t>(v + 20), static_cast<uint8_t>(v / 2));
        }
    }
    const uint8_t colours[3][3] = {{220, 30, 30}, {30, 200, 40}, {40, 60, 230}};
    for (int s = 0; s < 3; ++s) {
        const uint32_t x0 = w / 8 + s * (w / 4), y0 = h / 3;
        for (uint32_t y = y0; y < y0 + h / 4; ++y) {
            for (uint32_t x = x0; x < x0 + w / 6; ++x) Put(f, w, x, y, colours[s][0], colours[s][1], colours[s][2]);
        }
    }
    return f;
}

double Psnr(const Frame& a, const Frame& b) {
    double se = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            const double d = static_cast<double>(a[i + c]) - b[i + c];
            se += d * d;
            ++n;
        }
    }
    return se == 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / (se / n));
}

// Largest channel difference at the centre of each flat square.
int SquareError(const Frame& a, const Frame& b, uint32_t w, uint32_t h) {
    int worst = 0;
    for (int s = 0; s < 3; ++s) {
        const uint32_t x = w / 8 + s * (w / 4) + w / 12, y = h / 3 + h / 8;
        const size_t i = (static_cast<size_t>(y) * w + x) * 4;
        for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(a[i + c] - b[i + c]));
    }
    return worst;
}

// Splits Encode's output into its length-prefixed access units.
std::vector<std::vector<uint8_t>> Units(const std::vector<uint8_t>& stream) {
    std::vector<std::vector<uint8_t>> units;
    size_t pos = 0;
    while (pos + 4 <= stream.size()) {
        uint32_t len = 0;
        std::memcpy(&len, &stream[pos], 4);
        pos += 4;
        if (len > stream.size() - pos) break;
        units.emplace_back(stream.begin() + pos, stream.begin() + pos + len);
        pos += len;
    }
    return units;
}

void TestConversions() {
    std::printf("-- colour conversions\n");
    // Flat 2x2 blocks survive BGRA -> NV12 -> BGRA almost exactly.
    const uint32_t w = 37, h = 21; // odd sizes: padding columns and rows
    const uint32_t cw = Video::CodedSize(w), ch = Video::CodedSize(h);
    Frame src(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t block = (x / 2) * 31 + (y / 2) * 17;
            Put(src, w, x, y, static_cast<uint8_t>(block * 7), static_cast<uint8_t>(block * 13 + 50), static_cast<uint8_t>(255 - block * 5));
        }
    std::vector<uint8_t> nv12(static_cast<size_t>(cw) * ch * 3 / 2);
    Video::BgraToNv12(src.data(), static_cast<ptrdiff_t>(w) * 4, w, h, nv12.data(), cw, ch);
    Frame back(src.size());
    Video::Nv12ToBgra(nv12.data(), nv12.data() + static_cast<size_t>(cw) * ch, cw, w, h, back.data());
    int worst = 0;
    for (uint32_t y = 0; y < (h & ~1u); ++y)
        for (uint32_t x = 0; x < (w & ~1u); ++x)
            for (int c = 0; c < 3; ++c) {
                const size_t i = (static_cast<size_t>(y) * w + x) * 4 + c;
                worst = std::max(worst, std::abs(src[i] - back[i]));
            }
    CHECK(worst <= 3, "flat 2x2 blocks round-trip within 3 levels (worst %d)", worst);
    CHECK(back[3] == 255 && back[back.size() - 1] == 255, "decoded alpha is opaque");

    // Bottom-up rows (negative stride) give the same image as top-down ones.
    Frame flipped(src.size());
    for (uint32_t y = 0; y < h; ++y)
        std::memcpy(&flipped[static_cast<size_t>(h - 1 - y) * w * 4], &src[static_cast<size_t>(y) * w * 4], w * 4);
    std::vector<uint8_t> nv12_flipped(nv12.size());
    Video::BgraToNv12(flipped.data() + static_cast<size_t>(h - 1) * w * 4, -static_cast<ptrdiff_t>(w) * 4, w, h,
                      nv12_flipped.data(), cw, ch);
    CHECK(nv12_flipped == nv12, "bottom-up rows convert like top-down ones");

    // The SSE2 loop (8 pixels per step) and the scalar tail produce identical pixels.
    const uint32_t wide = 24, narrow = 7, rows = 4;
    std::vector<uint8_t> luma(wide * rows), chroma(wide * rows / 2);
    for (size_t i = 0; i < luma.size(); ++i) luma[i] = static_cast<uint8_t>(i * 37 + 11);
    for (size_t i = 0; i < chroma.size(); ++i) chroma[i] = static_cast<uint8_t>(i * 53 + 3);
    Frame simd(wide * rows * 4), scalar(narrow * rows * 4);
    Video::Nv12ToBgra(luma.data(), chroma.data(), wide, wide, rows, simd.data());
    Video::Nv12ToBgra(luma.data(), chroma.data(), wide, narrow, rows, scalar.data());
    bool same = true;
    for (uint32_t y = 0; y < rows; ++y)
        same = same && std::memcmp(&simd[y * wide * 4], &scalar[y * narrow * 4], narrow * 4) == 0;
    CHECK(same, "SSE2 and scalar NV12 conversions agree");

    // Same for BGRA -> NV12: a 24-pixel row goes through the SSE2 loop, a 7-pixel one through the
    // scalar loops; the luma of columns 0-6 and the chroma of blocks 0-2 must match.
    Frame bgra_wide(wide * rows * 4), bgra_narrow(narrow * rows * 4);
    for (uint32_t y = 0; y < rows; ++y)
        for (uint32_t x = 0; x < wide; ++x) {
            const uint32_t v = y * 97 + x * 41;
            Put(bgra_wide, wide, x, y, static_cast<uint8_t>(v * 3), static_cast<uint8_t>(v * 7 + 1), static_cast<uint8_t>(v * 11 + 2));
            if (x < narrow) Put(bgra_narrow, narrow, x, y, static_cast<uint8_t>(v * 3), static_cast<uint8_t>(v * 7 + 1), static_cast<uint8_t>(v * 11 + 2));
        }
    const uint32_t cwide = Video::CodedSize(wide), cnarrow = Video::CodedSize(narrow), crows = Video::CodedSize(rows);
    std::vector<uint8_t> nv_wide(static_cast<size_t>(cwide) * crows * 3 / 2), nv_narrow(static_cast<size_t>(cnarrow) * crows * 3 / 2);
    Video::BgraToNv12(bgra_wide.data(), wide * 4, wide, rows, nv_wide.data(), cwide, crows);
    Video::BgraToNv12(bgra_narrow.data(), narrow * 4, narrow, rows, nv_narrow.data(), cnarrow, crows);
    same = true;
    for (uint32_t y = 0; y < rows; ++y) same = same && std::memcmp(&nv_wide[y * cwide], &nv_narrow[y * cnarrow], narrow) == 0;
    for (uint32_t y = 0; y < rows / 2; ++y)
        same = same && std::memcmp(&nv_wide[cwide * crows + y * cwide], &nv_narrow[cnarrow * crows + y * cnarrow], 6) == 0;
    CHECK(same, "SSE2 and scalar BGRA conversions agree");

    // Speed at the game's window size.
    const uint32_t bw = 1920, bh = 974, bcw = Video::CodedSize(bw), bch = Video::CodedSize(bh);
    const Frame big = Scene(bw, bh, 0);
    std::vector<uint8_t> big_nv12(static_cast<size_t>(bcw) * bch * 3 / 2);
    Frame big_back(big.size());
    const int n = 20;
    auto t0 = Clock::now();
    for (int i = 0; i < n; ++i) Video::BgraToNv12(big.data(), bw * 4, bw, bh, big_nv12.data(), bcw, bch);
    auto t1 = Clock::now();
    for (int i = 0; i < n; ++i)
        Video::Nv12ToBgra(big_nv12.data(), big_nv12.data() + static_cast<size_t>(bcw) * bch, bcw, bw, bh, big_back.data());
    auto t2 = Clock::now();
    std::printf("[INFO] 1920x974: BGRA->NV12 %.2f ms, NV12->BGRA %.2f ms\n", Ms(t0, t1) / n, Ms(t1, t2) / n);
}

// Encodes `frames` frames of the moving scene and decodes them back; checks quality and that
// each frame comes out right away. Returns false when no encoder could be created.
bool RunCodec(const char* label, uint32_t w, uint32_t h, int frames, uint32_t kbps) {
    Video::H264Encoder encoder;
    const auto c0 = Clock::now();
    if (!encoder.Create(w, h, kbps)) {
        std::printf("[INFO] %s: no encoder for %ux%u\n", label, w, h);
        return false;
    }
    const auto c1 = Clock::now();
    Video::H264Decoder decoder;
    CHECK(decoder.Create(), "%s: decoder created", label);
    std::printf("[INFO] %s: \"%s\" at %ux%u, created in %.1f ms, %s decoder\n", label, encoder.Name().c_str(), w, h,
                Ms(c0, c1), decoder.IsHardware() ? "hardware" : "software");

    Frame decoded(static_cast<size_t>(w) * h * 4);
    double encode_ms = 0, decode_ms = 0;
    size_t bytes = 0, units_total = 0;
    int late = 0, pictures = 0, keys = 0;
    bool all_ok = true;
    double psnr_last = 0;
    int square_last = 255;
    for (int t = 0; t < frames; ++t) {
        const Frame src = Scene(w, h, t);
        std::vector<uint8_t> stream;
        bool key = false;
        const auto e0 = Clock::now();
        // Bottom-up rows, as the server reads them back from OpenGL.
        Frame flipped(src.size());
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(&flipped[static_cast<size_t>(h - 1 - y) * w * 4], &src[static_cast<size_t>(y) * w * 4], w * 4);
        const auto e1 = Clock::now();
        all_ok = encoder.Encode(flipped.data() + static_cast<size_t>(h - 1) * w * 4, -static_cast<ptrdiff_t>(w) * 4, stream, key) && all_ok;
        const auto e2 = Clock::now();
        if (t >= 5) encode_ms += Ms(e1, e2); // steady state: the first frames warm the encoder up
        (void)e0;
        bytes += stream.size();
        keys += key ? 1 : 0;
        const auto units = Units(stream);
        units_total += units.size();
        if (units.empty()) ++late;
        bool picture = false;
        const auto d0 = Clock::now();
        for (const auto& u : units) {
            bool got = false;
            all_ok = decoder.Decode(u.data(), u.size(), w, h, decoded.data(), got) && all_ok;
            picture = picture || got;
        }
        if (t >= 5) decode_ms += Ms(d0, Clock::now());
        if (picture) {
            ++pictures;
            psnr_last = Psnr(src, decoded);
            square_last = SquareError(src, decoded, w, h);
        }
    }
    CHECK(all_ok, "%s: every frame encoded and decoded", label);
    CHECK(keys >= 1, "%s: the stream starts with an IDR picture (%d key frames)", label, keys);
    CHECK(late <= 1 && pictures >= frames - 1, "%s: frames come out at once (%d of %d without output, %d pictures)", label,
          late, frames, pictures);
    CHECK(psnr_last > 26.0, "%s: last frame PSNR %.1f dB", label, psnr_last);
    CHECK(square_last <= 12, "%s: flat colours kept within 12 levels (worst %d)", label, square_last);
    std::printf("[INFO] %s: %.1f KB/frame, after warm-up encode %.2f ms/frame, decode+convert %.2f ms/frame, %zu units\n",
                label, bytes / 1024.0 / frames, encode_ms / (frames - 5), decode_ms / (frames - 5), units_total);
    return true;
}

} // namespace

int main() {
    if (!Video::IsAvailable()) {
        std::printf("[SKIP] Media Foundation is not available on this system\n");
        return 0;
    }
    TestConversions();

    std::printf("-- default encoder (hardware when available)\n");
    SetEnvironmentVariableA("RGPU_VIDEO_ENCODER", nullptr);
    CHECK(RunCodec("default", 1920, 974, 60, 40000), "an encoder exists for 1920x974");
    RunCodec("default odd size", 1283, 701, 20, 20000);

    std::printf("-- software encoder\n");
    SetEnvironmentVariableA("RGPU_VIDEO_ENCODER", "software");
    CHECK(RunCodec("software", 1920, 974, 30, 40000), "the software encoder exists");
    RunCodec("software odd size", 643, 481, 10, 10000);
    SetEnvironmentVariableA("RGPU_VIDEO_ENCODER", nullptr);

    std::printf("-- software decoder\n");
    SetEnvironmentVariableA("RGPU_VIDEO_DECODER", "software");
    RunCodec("software decoder", 1283, 701, 15, 20000);
    SetEnvironmentVariableA("RGPU_VIDEO_DECODER", nullptr);

    // A decoder fed garbage reports failure instead of crashing.
    Video::H264Decoder decoder;
    if (decoder.Create()) {
        std::vector<uint8_t> junk(4096);
        for (size_t i = 0; i < junk.size(); ++i) junk[i] = static_cast<uint8_t>(i * 131 + 7);
        Frame out(64 * 64 * 4);
        bool picture = false;
        decoder.Decode(junk.data(), junk.size(), 64, 64, out.data(), picture);
        CHECK(!picture, "garbage input produces no picture");
    }

    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
