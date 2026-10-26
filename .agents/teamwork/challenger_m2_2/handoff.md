# Handoff Report — Challenger 2 (Milestone 2)

**Agent**: Challenger 2 (`challenger_m2_2`)  
**Role**: Empirical Challenger (Adversarial Critic & Specialist)  
**Parent Conversation ID**: `ed6dd465-ec42-4bab-b054-c76327c6c969`  
**Milestone**: Milestone 2 (Remote GPU Execution Server)  
**Date**: 2026-10-26T17:28:30Z  
**Verdict**: **`APPROVE`**  

---

## 1. Observation

### Codebase Observations
1. `src/server/gpu_device.h` lines 44-85: `HostGpuDevice` class interface defining `InitializeDedicatedGpu()`, `CreateOffscreenTargets(width, height)`, and `ReadbackFrame(std::vector<uint8_t>&, uint32_t&, uint32_t&)`.
2. `src/server/gpu_device.cpp` lines 375-422: `HostGpuDevice::ReadbackFrame` copies offscreen color render target to staging texture (`CopyResource`), maps staging texture with `D3D11_MAP_READ`, and handles row pitch unpadding:
   ```cpp
   if (mapped.RowPitch == tight_row_bytes) {
       std::memcpy(p_dst, p_src, total_packed_bytes);
   } else {
       for (uint32_t y = 0; y < m_height; ++y) {
           std::memcpy(p_dst + (static_cast<size_t>(y) * tight_row_bytes),
                       p_src + (static_cast<size_t>(y) * mapped.RowPitch),
                       tight_row_bytes);
       }
   }
   ```
3. `tests/test_server_benchmark.cpp`: Independent empirical benchmark harness created by Challenger 2 implementing 5 stress suites (high-res profiling, 100 continuous frames profiling, prime/odd row-pitch skew oracle, dynamic resolution resizing, and mathematical luminance/bounding box distribution oracle).

### Tool Execution & Quantitative Observations
1. **Compilation Command**:
   `clang++ -std=c++17 -O2 -Isrc/common -Isrc/server tests/test_server_benchmark.cpp src/common/transport.cpp src/server/gpu_device.cpp -o build/test_server_benchmark.exe -ld3d11 -ldxgi -lws2_32 -ld3dcompiler`
   Result: Exited code 0 with 0 compiler warnings.
2. **Benchmark Execution (`.\build\test_server_benchmark.exe`)**:
   - Hardware: `Intel(R) UHD Graphics 620 | VendorId: 0x8086 | DeviceId: 0x3ea0 | Dedicated VRAM: 128 MB | Feature Level: 0xb100`.
   - High-Res Throughput:
     - 800x600: Render=0.154 ms, Readback=1.306 ms, Total=1.460 ms, Throughput=**684.8 FPS**, Bandwidth=1.37 GB/s.
     - 1280x720: Render=0.110 ms, Readback=2.323 ms, Total=2.434 ms, Throughput=**410.9 FPS**, Bandwidth=1.48 GB/s.
     - 1920x1080: Render=0.117 ms, Readback=4.601 ms, Total=4.719 ms (P50=4.659 ms, P95=5.835 ms), Throughput=**211.9 FPS**, Bandwidth=1.68 GB/s.
     - 2560x1440: Render=0.138 ms, Readback=8.429 ms, Total=8.567 ms, Throughput=**116.7 FPS**, Bandwidth=1.63 GB/s.
   - 100 Continuous Frames Profile:
     - 800x600 (100 frames): Mean=1.584 ms (StdDev=1.048 ms), P50=1.283 ms, Max=7.554 ms, Sustained=**631.4 FPS**, Drops >16.6ms=**0/100 (0.0%)**, Drops >33.3ms=**0/100 (0.0%)**.
     - 1920x1080 (100 frames): Mean=5.772 ms (StdDev=3.719 ms), P50=4.474 ms, Sustained=**173.3 FPS**, Drops >16.6ms=**4/100 (4.0%)**, Drops >33.3ms=**0/100 (0.0%)**.
   - Row-Pitch Prime/Odd Comb Skew Oracle (1,136,245 total pixels verified across 333x221, 511x511, 137x89, 1023x767, 77x61 with padding 4 to 220 bytes/scanline):
     - Skew Errors Detected: **0 (0.000%)**.
   - Dynamic Resolution Reallocation: 24 cycles across 7 resolutions completed in 95-111 ms (3.9 - 4.6 ms/reallocation) with **0 memory leaks and 0 crashes**.
   - Mathematical Area & Luminance Oracle:
     - 800x600 non-clear ratio: **60,000 / 480,000 = 12.500%** (exact match with theoretical NDC triangle area $0.5 / 4.0 = 12.500\%$).
     - 1920x1080 non-clear ratio: **259,200 / 2,073,600 = 12.500%** (exact match with theoretical NDC triangle area).
     - Bounding box matching viewport NDC projection within $\le 1$ pixel.
3. **Existing Server Unit Tests (`.\build\test_server.exe`)**:
   - 81 assertions run, 81 passed, 0 failed in 249 ms.
4. **Total Benchmark & Stress Suite Assertions**:
   - 530 assertions run, 530 passed, 0 failed.

---

## 2. Logic Chain

1. **Premise 1 (Throughput)**: The user request and Milestone 2 specification require the server to maintain real-time performance on the host GPU. Real-time rendering requires exceeding 30 FPS (< 33.3 ms/frame) for baseline interactivity and 60 FPS (< 16.67 ms/frame) for smooth display.
2. **Observation Step**: In Benchmark 1 & 2, Full HD 1080p offscreen rendering and readback sustained 173.3 - 211.9 FPS with a median latency of 4.47 - 4.66 ms, and 800x600 sustained 631.4 - 684.8 FPS with a median latency of 1.28 - 1.40 ms.
3. **Inference 1**: The host GPU readback pipeline operates well above the real-time threshold, utilizing only 27% of the 16.67 ms 60 FPS frame budget at 1080p and 9% at 800x600.
4. **Premise 2 (Scanline Alignment)**: Direct3D 11 drivers enforce row-pitch padding to 256-byte multiples. Prime or odd dimensions have row bytes that are not multiples of 256 (e.g. 333 width = 1332 bytes, padded to 1536 bytes, leaving 204 bytes padding per row). Any flaw in unpadding shifts subsequent scanlines, producing diagonal shearing.
5. **Observation Step**: In Benchmark 3, a fullscreen quad was rendered using a pixel shader encoding screen coordinates in an 8-pixel alternating comb pattern. 1,136,245 individual pixels across 5 prime/odd dimensions were tested against the coordinate oracle.
6. **Inference 2**: Zero scanline shift errors were observed across all 1,136,245 pixels. The row-pitch unpadding algorithm is mathematically sound and guarantees zero pixel skew.
7. **Premise 3 (Rasterization Integrity)**: Mathematical projection of an NDC triangle with vertices $(0, 0.5)$, $(0.5, -0.5)$, $(-0.5, -0.5)$ has an exact theoretical area of 0.5 in an NDC viewport of 4.0, predicting a non-clear coverage ratio of $12.500\%$.
8. **Observation Step**: At 800x600 (60,000 non-clear pixels) and 1920x1080 (259,200 non-clear pixels), the observed non-clear ratio was exactly $12.500\%$, and bounding boxes matched predicted NDC bounds within 1 pixel.
9. **Inference 3**: Headless Direct3D 11 device configuration, viewport transformation, rasterization, and staging transfer preserve exact mathematical geometric integrity.
10. **Conclusion Step**: All criteria are satisfied; Milestone 2 is approved.

---

## 3. Caveats

1. **Single-Staging Synchronous Map**: `ReadbackFrame` relies on a single staging texture with synchronous `Map(D3D11_MAP_READ, 0)`. Under 1080p this achieves > 170 FPS, but at 4K (3840x2160) or under extreme GPU load, synchronous mapping serializes CPU readback with GPU execution. For future milestones targeting 4K streaming, double-buffering staging textures is recommended.
2. **Uncompressed Frame Delivery**: Staging readback outputs uncompressed RGBA8 (8.29 MB per 1080p frame). Network streaming performance across high-latency WAN links will depend on socket bandwidth or downstream compression (Milestone 3/4).
3. **Hardware Platform**: Tests were executed on the host's primary hardware GPU (`Intel(R) UHD Graphics 620`, Feature Level `0xb100`). Behavior on multi-GPU SLI/Crossfire systems was not evaluated.

---

## 4. Conclusion

**Verdict**: **`APPROVE`**  
The `HostGpuDevice` offscreen rendering and staging readback implementation in `src/server/` meets and exceeds all performance, throughput, row-pitch alignment, and mathematical accuracy requirements for Milestone 2. No defects or regressions were detected. Milestone 2 Remote GPU Execution Server is approved to proceed to Milestone 3.

---

## 5. Verification Method

To independently verify these findings on the host machine:

1. **Run Milestone 2 Server Unit Tests**:
   ```powershell
   .\build\test_server.exe
   ```
   *Expected Result*: 81 assertions passed, 0 failures, exit code 0.

2. **Recompile and Run Empirical Benchmark & Stress Suite**:
   ```powershell
   clang++ -std=c++17 -O2 -Isrc/common -Isrc/server tests/test_server_benchmark.cpp src/common/transport.cpp src/server/gpu_device.cpp -o build/test_server_benchmark.exe -ld3d11 -ldxgi -lws2_32 -ld3dcompiler
   .\build\test_server_benchmark.exe
   ```
   *Expected Result*: 530 assertions passed, 0 failures, exit code 0.

3. **Invalidation Conditions**:
   - Sustained 1080p frame rate drops below 30 FPS.
   - Any scanline skew error (> 0) on odd dimensions (333x221 or 511x511).
   - Non-clear pixel ratio on 800x600 deviates from $12.500\% \pm 1.0\%$.
   - Any crash or unhandled exception during dynamic target resizing.
