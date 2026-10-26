# Worker M2 (Iteration 2 Remediation) Handoff Report

## 1. Observation
Prior to this remediation, running `tests/test_server_stress.cpp` against the baseline `ServerDispatcher` produced 4 failures and 15 Direct3D 11 hardware/driver memory access violations (`0xC0000005`) in `Stress_MalformedOpcodes_And_BoundaryAttacks`:
```
  [CRASH DEFECT] Opcode 0x33 crashed on 4-byte payload with code 0xc0000005
  [FAIL] Fuzz test completed: detected 9 hardware/driver crash(es): (fuzz_crashes == 0) at tests/test_server_stress.cpp:576
  [FAIL] Parameter validation stress: detected 11 crash(es): (param_crashes == 0) at tests/test_server_stress.cpp:671
  [FATAL SUITE CRASH] Stress_MalformedOpcodes_And_BoundaryAttacks terminated with unhandled driver exception 0xc0000005
[FAILED  ] Stress_MalformedOpcodes_And_BoundaryAttacks (36 ms) - 4 assertion failure(s)!
============================================================
  ADVERSARIAL STRESS TEST SUMMARY
============================================================
  Total Assertions Run : 181
  Passed               : 178
  Failed               : 4
============================================================
  VERDICT: REQUEST_CHANGES (4 failure(s))
```

Inspection of `src/server/server_dispatcher.cpp` confirmed:
1. In `HandleIASetTopology` (formerly line 710):
   ```cpp
   context->IASetPrimitiveTopology(static_cast<D3D11_PRIMITIVE_TOPOLOGY>(p->topology));
   ```
   No validation was performed on `p->topology`. When an invalid value such as `0x12345678`, `65`, `0xFFFFFFFF`, `6`, or `255` was supplied, retail Intel graphics drivers (`igd10iumd64.dll`) indexed out of bounds in their internal state tables, raising `EXCEPTION_ACCESS_VIOLATION` (`0xC0000005`).
2. In `HandleIASetVertexBuffer`, `HandleVSSetCB`, `HandlePSSetCB`, `HandlePSSetSrv`, and `HandlePSSetSampler`:
   Raw slot numbers from network payloads were forwarded directly to Direct3D 11 Immediate Context methods without verifying they were below architectural limits (`D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT` = 32, `D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT` = 14, `D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT` = 128, `D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT` = 16).
3. In `HandleCreateInputLayout` (formerly line 592):
   ```cpp
   semantic_names[i] = src_elements[i].semantic_name;
   ```
   If client data contained 32 non-null bytes without `\0`, standard `strlen` resulted in a buffer over-read (CWE-125).
4. In `HandleWriteBufferDiscard`, `HandleRSSetViewport`, `HandleOMSetRenderTargets`, `HandleClearRtv`, and `HandleClearDsv`:
   Missing bounds checks on buffer write offsets, viewport counts / non-finite floats, render target counts, and clear masks.

Following our modifications to `src/server/server_dispatcher.cpp`:
1. `build/test_server_stress.exe` execution output:
```
============================================================
  ADVERSARIAL STRESS TEST SUMMARY
============================================================
  Total Assertions Run : 181
  Passed               : 181
  Failed               : 0
  Total Duration       : 993 ms
============================================================
  VERDICT: APPROVE (ALL ADVERSARIAL STRESS TESTS PASSED)
============================================================
```
Exit code: `0`. 0 crashes, 0 failures.

2. `build/test_server.exe` execution output:
```
============================================================
  TEST SUMMARY
============================================================
  Total Assertions Run : 81
  Passed               : 81
  Failed               : 0
  Total Duration       : 201 ms
============================================================
  OVERALL STATUS: ALL TESTS PASSED (EXIT CODE 0)
============================================================
```
Exit code: `0`. 81/81 assertions passed.

3. `build/server.exe` compiled cleanly with `clang++ -std=c++17 -Wall -Wextra -Wpedantic -O2`:
Exit code: `0`, zero warnings.

4. `build/test_server_benchmark.exe` execution output:
```
============================================================
  BENCHMARK & STRESS SUMMARY
============================================================
  Total Assertions Run : 530
  Passed               : 530
  Failed               : 0
  Total Duration       : 1424 ms
============================================================
  OVERALL VERDICT: ALL BENCHMARKS & STRESS TESTS PASSED
============================================================
```
Exit code: `0`. 530/530 assertions passed.

5. `build/test_transport_stress.exe` execution output:
```
============================================================
  STRESS TEST SUMMARY
============================================================
  Total Assertions Run : 1635
  Passed               : 1635
  Failed               : 0
  Total Duration       : 17686 ms
============================================================
  OVERALL STATUS: ALL STRESS TESTS PASSED (EXIT CODE 0)
============================================================
```
Exit code: `0`. 1635/1635 assertions passed.

## 2. Logic Chain
1. Root cause identification: Retail Direct3D 11 driver binaries trust API callers and do not guard against out-of-bounds parameters. Forwarding unvalidated remote values triggers hardware memory access violations that poison driver state, causing secondary crashes during `context->ClearState()` in `ResetSession()`.
2. Topology validation: `IsValidD3D11PrimitiveTopology()` ensures only the exact defined ranges (`D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED` (0), `POINTLIST..TRIANGLESTRIP` (1..5), `LINELIST_ADJ..TRIANGLESTRIP_ADJ` (10..13), `1_CONTROL_POINT_PATCHLIST..32_CONTROL_POINT_PATCHLIST` (33..64)) are forwarded to `IASetPrimitiveTopology`. Invalid values return `false` without touching the driver.
3. Hardware register slot validation:
   - `slot < 32` for `HandleIASetVertexBuffer`
   - `slot < 14` for `HandleVSSetCB` and `HandlePSSetCB`
   - `slot < 128` for `HandlePSSetSrv`
   - `slot < 16` for `HandlePSSetSampler`
   Values equal to or above these limits are rejected with `false` and diagnostic logging.
4. Memory safety in `HandleCreateInputLayout`:
   - `strnlen(src_elements[i].semantic_name, sizeof(src_elements[i].semantic_name))` guarantees at most 32 bytes are read.
   - `semantic_names[i].assign(src_elements[i].semantic_name, name_len)` creates a guaranteed null-terminated C++ string.
   - `p->num_elements <= 32` prevents vector allocation or driver descriptor overflows.
   - Per-element `input_slot < 32`, `format != DXGI_FORMAT_UNKNOWN && IsValidDxgiFormat(...)`, and `input_slot_class <= 1` are validated.
5. Resource creation hardening:
   - Universal rejection of `p->res_id == 0`.
   - `IsValidDxgiFormat` checks across textures, SRVs, RTVs, DSVs, and input layout elements.
   - Buffer byte width, 16-byte constant buffer alignment, and buffer initial data bounds (`initial_data_size >= byte_width`).
   - Comparison function validation in `HandleCreateSampler` allowing `0` (defaulted to `D3D11_COMPARISON_NEVER`) and `1..8`, while rejecting invalid functions `> 8`.
   - Non-finite float checks (`std::isfinite`) and dimension bounds on viewports and clear colors.
   - Target count bounding (`num_rtvs <= 8`, `num_viewports <= 16`).
   - Dynamic buffer discard capacity bounding (`byte_offset + byte_width <= desc.ByteWidth`).

## 3. Caveats
- Direct3D 11 feature level was tested against hardware adapter Intel UHD Graphics 620 (`Feature Level 0xb100`). All architectural limits validated are standard across Feature Level 11_0, 11_1, and 12+.
- Exclusive write ownership was respected: only `src/server/server_dispatcher.cpp` was modified. `server_dispatcher.h` already contained all necessary method declarations.

## 4. Conclusion
All bounds validation and memory safety remediations specified by the orchestrator and explorer specs have been fully implemented in `src/server/server_dispatcher.cpp`.
All 181 adversarial stress test assertions pass with 0 failures, 0 crashes, and exit code 0.
All 81 standard unit test assertions pass with exit code 0.
The standalone server daemon compiles cleanly with zero warnings under `-Wall -Wextra -Wpedantic`.
Milestone 2 Remote GPU Execution Server is now hardened against remote adversarial attacks.

## 5. Verification Method
To independently verify:
```powershell
# 1. Adversarial Stress Suite (must show 181/181 assertions passed, 0 crashes, exit code 0)
clang++ -std=c++17 -fms-extensions -O2 -Isrc/common -Isrc/server src/common/transport.cpp src/server/gpu_device.cpp src/server/server_dispatcher.cpp tests/test_server_stress.cpp -o build/test_server_stress.exe -ld3d11 -ldxgi -lws2_32 -ld3dcompiler -lpsapi
.\build\test_server_stress.exe

# 2. Standard Unit Test Suite (must show 81/81 assertions passed, exit code 0)
clang++ -std=c++17 -Wall -Wextra -Wpedantic -O2 -Isrc/common -Isrc/server src/common/transport.cpp src/server/gpu_device.cpp src/server/server_dispatcher.cpp tests/test_server.cpp -o build/test_server.exe -ld3d11 -ldxgi -lws2_32 -ld3dcompiler
.\build\test_server.exe

# 3. Standalone Daemon Compilation (must compile with 0 warnings)
clang++ -std=c++17 -Wall -Wextra -Wpedantic -O2 -Isrc/common -Isrc/server src/common/transport.cpp src/server/gpu_device.cpp src/server/server_dispatcher.cpp src/server/main_server.cpp -o build/server.exe -ld3d11 -ldxgi -lws2_32 -ld3dcompiler
```
Invalidation conditions:
- Any occurrence of `0xC0000005` in `test_server_stress.exe`.
- Any assertion failure in `test_server.exe` or `test_server_stress.exe`.
- Any compilation error or warning with `-Wall -Wextra -Wpedantic`.
