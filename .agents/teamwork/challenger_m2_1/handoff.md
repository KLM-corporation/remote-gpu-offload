# Handoff Report — Challenger M2

**Role**: EMPIRICAL CHALLENGER (critic, specialist)  
**Milestone**: Milestone 2 — Remote GPU Execution Server  
**Recipient**: Orchestrator / Worker M2  
**Verdict**: `REQUEST_CHANGES`

---

## 1. Observation

1. **Compilation & Execution Command**:
   - Stress test harness compiled with:
     ```powershell
     clang++ -std=c++17 -fms-extensions -O2 -Isrc/common -Isrc/server src/common/transport.cpp src/server/gpu_device.cpp src/server/server_dispatcher.cpp tests/test_server_stress.cpp -o build/test_server_stress.exe -ld3d11 -ldxgi -lws2_32 -ld3dcompiler -lpsapi
     ```
   - Execution command:
     ```powershell
     .\build\test_server_stress.exe
     ```
   - Execution Output Summary:
     ```
     Total Assertions Run : 181
     Passed               : 178
     Failed               : 4
     Total Duration       : 983 ms
     VERDICT: REQUEST_CHANGES (4 failure(s))
     ```
2. **Crash Defects Observed During Stress Testing**:
   - `OP_IA_SET_TOPOLOGY` with `topology = 0x12345678` triggered:
     ```
     [CRASH DEFECT] Opcode 0x33 crashed on 4-byte payload with code 0xc0000005
     ```
   - Fuzz testing with 150 pseudorandom packets triggered:
     ```
     [CRASH DEFECT] Fuzz packet 90 (op 0x33) crashed with code 0xc0000005
     [CRASH DEFECT] Fuzz packet 133 (op 0x37) crashed with code 0xc0000005
     [CRASH DEFECT] Fuzz packet 149 (op 0x37) crashed with code 0xc0000005
     [FAIL] Fuzz test completed: detected 8 hardware/driver crash(es)
     ```
   - Parameter boundary tests triggered:
     ```
     [CRASH DEFECT] OP_IA_SET_TOPOLOGY with bad topology=305419896 crashed with code 0xc0000005
     [CRASH DEFECT] OP_VS_SET_CB with slot=32 crashed with 0xc0000005
     [CRASH DEFECT] OP_PS_SET_CB with slot=32 crashed with 0xc0000005
     [CRASH DEFECT] OP_PS_SET_SRV with slot=32 crashed with 0xc0000005
     [CRASH DEFECT] OP_PS_SET_SAMPLER with slot=32 crashed with 0xc0000005
     [CRASH DEFECT] OP_PS_SET_SRV with slot=128 crashed with 0xc0000005
     [CRASH DEFECT] OP_PS_SET_SAMPLER with slot=128 crashed with 0xc0000005
     [CRASH DEFECT] OP_VS_SET_CB with slot=305419896 crashed with 0xc0000005
     [CRASH DEFECT] OP_PS_SET_CB with slot=305419896 crashed with 0xc0000005
     [CRASH DEFECT] OP_PS_SET_SRV with slot=305419896 crashed with 0xc0000005
     [CRASH DEFECT] OP_PS_SET_SAMPLER with slot=305419896 crashed with 0xc0000005
     [FAIL] Parameter validation stress: detected 11 crash(es)
     ```
   - Corrupted driver state led to unhandled secondary access violation during cleanup:
     ```
     [FATAL SUITE CRASH] Stress_MalformedOpcodes_And_BoundaryAttacks terminated with unhandled driver exception 0xc0000005
     ```
3. **Verbatim Code Locations**:
   - `src/server/server_dispatcher.cpp:710`:
     ```cpp
     context->IASetPrimitiveTopology(static_cast<D3D11_PRIMITIVE_TOPOLOGY>(p->topology));
     ```
   - `src/server/server_dispatcher.cpp:735`:
     ```cpp
     context->VSSetConstantBuffers(p->slot, 1, &p_cb);
     ```
   - `src/server/server_dispatcher.cpp:760`:
     ```cpp
     context->PSSetConstantBuffers(p->slot, 1, &p_cb);
     ```
   - `src/server/server_dispatcher.cpp:773`:
     ```cpp
     context->PSSetShaderResources(p->slot, 1, &p_srv);
     ```
   - `src/server/server_dispatcher.cpp:786`:
     ```cpp
     context->PSSetSamplers(p->slot, 1, &p_sampler);
     ```
   - `src/server/server_dispatcher.cpp:687`:
     ```cpp
     context->IASetVertexBuffers(p->slot, 1, &p_buffer, &stride, &offset);
     ```
   - `src/server/server_dispatcher.cpp:592`:
     ```cpp
     semantic_names[i] = src_elements[i].semantic_name;
     ```
4. **Successful Observations**:
   - Client disconnect mid-packet and abrupt TCP reset (`SO_LINGER {1,0}`) detected cleanly, zero server hang.
   - 20 sequential adversarial client drops did not destabilize the server; subsequent legitimate client connected, rendered, and read back pristine framebuffers.
   - Lookups for non-existent resource IDs (`0xDEADBEEF`) safely handled across all opcodes without null dereference.
   - Resource leak check verified 1,000 buffers (allocated and destroyed twice), 1,000 textures, session reset, and 500 rapid interleaved cycles. COM refcounts released cleanly and process memory stabilized at 143 MB with only 2 MB growth after initial pool warm-up.

---

## 2. Logic Chain

1. **Premise 1**: In Direct3D 11 retail mode (without debug layer runtime interception), immediate context methods (`ID3D11DeviceContext::*`) pass slot indices and enum values directly into hardware driver export functions (`igd10iumd64.dll`) without checking that indices are below driver table limits.
2. **Premise 2**: In `src/server/server_dispatcher.cpp`, methods `HandleIASetTopology`, `HandleVSSetCB`, `HandlePSSetCB`, `HandlePSSetSrv`, `HandlePSSetSampler`, and `HandleIASetVertexBuffer` cast uint32 integers directly from incoming network payloads (`p->topology`, `p->slot`) and forward them into `m_context->*` calls without range validation.
3. **Inference 1**: When an adversarial client transmits network packets with `p->topology > 64` or `p->slot >= API limits`, the underlying Direct3D driver attempts to write/read beyond its internal static dispatch arrays.
4. **Empirical Fact**: Executing `test_server_stress.exe` directly triggered 19 `EXCEPTION_ACCESS_VIOLATION` (`0xC0000005`) crashes inside the hardware driver DLL.
5. **Inference 2**: These access violations corrupt the graphics driver internal memory state, causing subsequent session cleanup (`context->ClearState()` / `context->Flush()`) to fail catastrophically and terminate the host daemon.
6. **Deduction**: Because the server daemon is exposed to untrusted network traffic, lack of input boundary validation constitutes a critical remote denial-of-service / server crash vulnerability.

---

## 3. Caveats

- Tests were run on the active hardware GPU adapter: `Intel(R) UHD Graphics 620` (VendorId: `0x8086`, DeviceId: `0x3ea0`). Driver behavior on NVIDIA or AMD hardware may exhibit different failure modes (e.g. silent corruption or driver device reset rather than immediate SEH access violation), but out-of-range indexing in driver tables remains undefined behavior across all hardware vendors.
- No modifications were made to implementation code under `src/` in adherence to the review-only constraint.

---

## 4. Conclusion

**Verdict: REQUEST_CHANGES**

Worker M2 must remediate `src/server/server_dispatcher.cpp` by adding defensive parameter validation before calling `ID3D11DeviceContext` methods:
1. Reject invalid `D3D11_PRIMITIVE_TOPOLOGY` values in `HandleIASetTopology`.
2. Reject `slot >= 14` in `HandleVSSetCB` and `HandlePSSetCB`.
3. Reject `slot >= 128` in `HandlePSSetSrv`.
4. Reject `slot >= 16` in `HandlePSSetSampler`.
5. Reject `slot >= 32` in `HandleIASetVertexBuffer`.
6. Reject invalid `DXGI_FORMAT` in `HandleIASetIndexBuffer`.
7. Bound `src_elements[i].semantic_name` copying with `strnlen` in `HandleCreateInputLayout`.

---

## 5. Verification Method

1. Compile the adversarial stress harness:
   ```powershell
   clang++ -std=c++17 -fms-extensions -O2 -Isrc/common -Isrc/server src/common/transport.cpp src/server/gpu_device.cpp src/server/server_dispatcher.cpp tests/test_server_stress.cpp -o build/test_server_stress.exe -ld3d11 -ldxgi -lws2_32 -ld3dcompiler -lpsapi
   ```
2. Run the adversarial stress harness:
   ```powershell
   .\build\test_server_stress.exe
   ```
3. Invalidation Conditions:
   - If `build/test_server_stress.exe` exits with code 0 (`VERDICT: APPROVE (ALL ADVERSARIAL STRESS TESTS PASSED)`), changes have successfully eliminated the vulnerabilities.
   - If any `[CRASH DEFECT]` or `EXCEPTION_ACCESS_VIOLATION` is triggered, the fix is incomplete.
