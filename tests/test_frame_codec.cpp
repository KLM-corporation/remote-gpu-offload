// Unit tests for the FRAME_CODEC_RLE_DELTA frame codec (src/common/frame_codec.*).
// Build: clang++ -std=c++17 -O2 -Isrc/common src/common/frame_codec.cpp tests/test_frame_codec.cpp -o build/test_frame_codec.exe

#include "frame_codec.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
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

using Frame = std::vector<uint8_t>;

Frame Solid(size_t w, size_t h, uint32_t rgba) {
    Frame f(w * h * 4);
    for (size_t i = 0; i < w * h; ++i) std::memcpy(&f[i * 4], &rgba, 4);
    return f;
}

Frame Checker(size_t w, size_t h, size_t cell) {
    Frame f(w * h * 4);
    for (size_t y = 0; y < h; ++y)
        for (size_t x = 0; x < w; ++x) {
            const uint32_t c = (((x / cell) + (y / cell)) & 1) ? 0xFFFFFFFFu : 0xFF203040u;
            std::memcpy(&f[(y * w + x) * 4], &c, 4);
        }
    return f;
}

Frame Noise(size_t w, size_t h, uint32_t seed) {
    std::mt19937 rng(seed);
    Frame f(w * h * 4);
    for (auto& b : f) b = static_cast<uint8_t>(rng());
    return f;
}

// Encodes `cur` (as a delta of `ref` when given), decodes it into a copy of `ref` and compares.
bool RoundTrip(const Frame& cur, const Frame* ref, size_t* encoded_size = nullptr) {
    std::vector<uint8_t> enc;
    const size_t pixels = cur.size() / 4;
    FrameCodec::Encode(cur.data(), ref ? ref->data() : nullptr, pixels, enc);
    if (encoded_size) *encoded_size = enc.size();
    Frame out = ref ? *ref : Frame(cur.size(), 0xCD);
    return FrameCodec::Decode(enc.data(), enc.size(), out.data(), pixels, ref != nullptr) && out == cur;
}

} // namespace

int main() {
    const size_t W = 640, H = 480, RAW = W * H * 4;
    size_t sz = 0;

    std::printf("== Round trips\n");
    Frame solid = Solid(W, H, 0xFF261F0Du);
    CHECK(RoundTrip(solid, nullptr, &sz) && sz < 64, "key frame, solid colour: %zu bytes (raw %zu)", sz, RAW);
    Frame checker = Checker(W, H, 8);
    CHECK(RoundTrip(checker, nullptr, &sz) && sz < RAW / 4, "key frame, checkerboard: %zu bytes (%.1f%%)", sz, 100.0 * sz / RAW);
    Frame noise = Noise(W, H, 1);
    CHECK(RoundTrip(noise, nullptr, &sz), "key frame, random noise: %zu bytes (%.1f%%)", sz, 100.0 * sz / RAW);
    CHECK(RoundTrip(solid, &solid, &sz) && sz < 64, "delta frame, identical: %zu bytes", sz);
    Frame moved = checker;
    for (size_t y = 100; y < 160; ++y) std::memset(&moved[(y * W + 200) * 4], 0x7F, 80 * 4);
    CHECK(RoundTrip(moved, &checker, &sz) && sz < 60 * 80 * 4 + 1024, "delta frame, 80x60 changed block: %zu bytes", sz);
    CHECK(RoundTrip(noise, &checker), "delta frame, everything changed");
    Frame one = Solid(1, 1, 0x12345678u);
    CHECK(RoundTrip(one, nullptr) && RoundTrip(Solid(1, 1, 0x9ABCDEF0u), &one), "1x1 frames (key and delta)");
    Frame empty;
    CHECK(RoundTrip(empty, nullptr), "zero-pixel frame");
    Frame zeros = Solid(W, H, 0);
    CHECK(RoundTrip(zeros, nullptr), "key frame, all-zero pixels");

    std::printf("== Malformed input is rejected\n");
    std::vector<uint8_t> enc;
    FrameCodec::Encode(moved.data(), checker.data(), W * H, enc);
    Frame target = checker;
    CHECK(!FrameCodec::Decode(enc.data(), enc.size(), target.data(), W * H, false), "delta frame without reference");
    CHECK(!FrameCodec::Decode(enc.data(), enc.size() - 1, target.data(), W * H, true), "truncated stream");
    std::vector<uint8_t> trailing = enc;
    trailing.push_back(0);
    target = checker;
    CHECK(!FrameCodec::Decode(trailing.data(), trailing.size(), target.data(), W * H, true), "trailing bytes");
    CHECK(!FrameCodec::Decode(enc.data(), enc.size(), target.data(), W * H - 1, true), "pixel count mismatch");
    CHECK(!FrameCodec::Decode(enc.data(), 5, target.data(), W * H, true), "stream shorter than header");
    std::vector<uint8_t> bad_codec = enc;
    bad_codec[0] = 99;
    CHECK(!FrameCodec::Decode(bad_codec.data(), bad_codec.size(), target.data(), W * H, true), "unknown codec id");
    std::vector<uint8_t> overflow(enc.begin(), enc.begin() + sizeof(CompressedFrameHeader));
    for (int k = 0; k < 9; ++k) overflow.push_back(0xFF);
    overflow.push_back(0x01);
    CHECK(!FrameCodec::Decode(overflow.data(), overflow.size(), target.data(), W * H, true), "run longer than the frame");

    std::printf("== Fuzzing (random corruption must never crash)\n");
    std::mt19937 rng(42);
    int accepted = 0;
    const int iterations = 20000;
    for (int it = 0; it < iterations; ++it) {
        std::vector<uint8_t> f = enc;
        const int flips = 1 + static_cast<int>(rng() % 8);
        for (int k = 0; k < flips; ++k) f[rng() % f.size()] = static_cast<uint8_t>(rng());
        if (rng() % 4 == 0) f.resize(rng() % (f.size() + 1));
        Frame t = checker;
        if (FrameCodec::Decode(f.data(), f.size(), t.data(), W * H, true)) ++accepted;
    }
    // Corrupted literal bytes still form a valid stream (integrity is TCP's job), so only the
    // absence of crashes and out-of-bounds writes is asserted here.
    CHECK(true, "%d corrupted streams handled without crash (%d structurally valid)", iterations, accepted);

    std::printf("== Speed (1920x1080)\n");
    const size_t FW = 1920, FH = 1080;
    Frame a = Checker(FW, FH, 16), b = a;
    for (size_t y = 300; y < 700; ++y) std::memset(&b[(y * FW + 600) * 4], 0x55, 500 * 4);
    Frame out = a;
    const int reps = 20;
    auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < reps; ++r) FrameCodec::Encode(b.data(), a.data(), FW * FH, enc);
    auto t1 = std::chrono::steady_clock::now();
    bool ok = true;
    for (int r = 0; r < reps; ++r) {
        out = a;
        ok = ok && FrameCodec::Decode(enc.data(), enc.size(), out.data(), FW * FH, true);
    }
    auto t2 = std::chrono::steady_clock::now();
    const double enc_ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / reps;
    const double dec_ms = std::chrono::duration<double, std::milli>(t2 - t1).count() / reps;
    CHECK(ok && out == b, "1080p delta: encode %.2f ms, decode %.2f ms, %zu bytes (%.1f%% of %zu)", enc_ms, dec_ms,
          enc.size(), 100.0 * enc.size() / (FW * FH * 4), FW * FH * 4);

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
