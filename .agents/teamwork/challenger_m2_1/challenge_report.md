# Milestone 2 Adversarial Stress Test & Challenge Report

**Target Components**: `ServerDispatcher`, `HostGpuDevice`, `ServerResourceManager` in `src/server/`  
**Author**: Challenger 1 (Empirical Verification & Adversarial Stress Testing)  
**Date**: 2026-10-26  
**Final Verdict**: `REQUEST_CHANGES`

---

## Challenge Summary

**Overall risk assessment**: **CRITICAL**

While `HostGpuDevice` demonstrates robust headless initialization, clean readback, odd-dimension scanline pitch packing, and `ServerDispatcher` handles abrupt client socket drops and non-existent resource IDs gracefully, **`ServerDispatcher` contains critical input validation defects that allow any remote client to crash the host server daemon with an Access Violation (`0xC0000005`)**.

Direct3D 11 immediate context state-binding calls (`IASetPrimitiveTopology`, `VSSetConstantBuffers`, `PSSetConstantBuffers`, `PSSetShaderResources`, `PSSetSamplers`, `IASetVertexBuffers`) do not validate parameter ranges in retail mode. When an adversarial client transmits out-of-range topology enums or pipeline slot numbers, the Direct3D graphics driver (`igd10iumd64.dll`) performs out-of-bounds indexing in internal driver jump tables/arrays, causing immediate `EXCEPTION_ACCESS_VIOLATION` (`0xC0000005`) that terminates the host daemon. Furthermore, `CreateInputLayout` performs an unbounded string copy of 32-byte semantic names, posing a buffer over-read risk.

---

## Confirmed Vulnerabilities & Challenges

### 1. [CRITICAL] Remote Process Crash via Out-of-Range `OP_IA_SET_TOPOLOGY` Enum
- **Assumption Challenged**: Assumed incoming `IASetTopologyPayload::topology` integer over the network is always a valid `D3D11_PRIMITIVE_TOPOLOGY` enum.
- **Attack Scenario**: An attacker sends `OP_IA_SET_TOPOLOGY` (opcode `0x0033`) with `topology = 0x12345678` or `65` (greater than `D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST`).
- **Verbatim Code (`src/server/server_dispatcher.cpp:710`)**:
  ```cpp
  context->IASetPrimitiveTopology(static_cast<D3D11_PRIMITIVE_TOPOLOGY>(p->topology));
  ```
- **Blast Radius**: Direct3D hardware driver (`igd10iumd64.dll`) accesses out-of-bounds memory in its internal primitive topology dispatch table. The server daemon crashes immediately with `EXCEPTION_ACCESS_VIOLATION` (`0xC0000005`). No recovery is possible.
- **Mitigation**: Validate `p->topology` before calling Direct3D:
  ```cpp
  bool is_valid_topology = 
      (p->topology == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) ||
      (p->topology >= D3D11_PRIMITIVE_TOPOLOGY_POINTLIST && p->topology <= D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) ||
      (p->topology >= D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ && p->topology <= D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ) ||
      (p->topology >= D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST && p->topology <= D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST);
  if (!is_valid_topology) {
      std::cerr << "[ServerDispatcher] Invalid primitive topology: " << p->topology << std::endl;
      return false;
  }
  ```

---

### 2. [CRITICAL] Remote Process Crash via Out-of-Bounds Pipeline Slot Numbers
- **Assumption Challenged**: Assumed `slot` fields in `VSSetConstantBufferPayload`, `PSSetConstantBufferPayload`, `PSSetSrvPayload`, `PSSetSamplerPayload`, and `IASetVertexBufferPayload` are within Direct3D architectural limits.
- **Attack Scenario**: An attacker sends packets with `slot = 32` or `0x12345678`.
- **Verbatim Code (`src/server/server_dispatcher.cpp`)**:
  - `HandleVSSetCB` (line 735): `context->VSSetConstantBuffers(p->slot, 1, &p_cb);` (Max D3D11 constant buffer slot is 13: `D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1`)
  - `HandlePSSetCB` (line 760): `context->PSSetConstantBuffers(p->slot, 1, &p_cb);` (Max slot is 13)
  - `HandlePSSetSrv` (line 773): `context->PSSetShaderResources(p->slot, 1, &p_srv);` (Max slot is 127: `D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1`)
  - `HandlePSSetSampler` (line 786): `context->PSSetSamplers(p->slot, 1, &p_sampler);` (Max slot is 15: `D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1`)
  - `HandleIASetVertexBuffer` (line 687): `context->IASetVertexBuffers(p->slot, 1, &p_buffer, &stride, &offset);` (Max slot is 15/31)
- **Blast Radius**: The driver indexes its fixed-size constant buffer, sampler, or SRV table out-of-bounds, triggering immediate `0xC0000005` access violation and crashing the server process.
- **Mitigation**: Add bounds checks before issuing context calls:
  ```cpp
  if (p->slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) return false; // 14
  if (p->slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) return false;             // 16
  if (p->slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) return false;     // 128
  if (p->slot >= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) return false;        // 32
  ```

---

### 3. [HIGH] Memory Corrupted State Leading to Secondary Crash during Cleanup
- **Assumption Challenged**: Assumed that if an exception occurs or invalid state is dispatched, `ResetSession()` and `m_gpuDevice.Reset()` can cleanly recover.
- **Attack Scenario**: After invalid slots/topologies are processed, the driver internal state is poisoned. During session cleanup (`~ServerDispatcher()` or `ResetSession()`), calling `context->ClearState()` and `context->Flush()` triggers an unhandled `EXCEPTION_ACCESS_VIOLATION` (`0xC0000005`).
- **Blast Radius**: The entire host execution daemon crashes fatally on disconnect or error recovery.
- **Mitigation**: Defend the boundary: never allow out-of-range parameters to reach `context->*` methods.

---

### 4. [MEDIUM] Buffer Over-Read Risk in `OP_CREATE_INPUT_LAYOUT` Semantic Name
- **Assumption Challenged**: Assumed `src_elements[i].semantic_name` is always null-terminated within its fixed 32-byte array.
- **Attack Scenario**: An attacker sends an element desc where all 32 bytes are non-zero (e.g. 32 `'A'` bytes).
- **Verbatim Code (`src/server/server_dispatcher.cpp:592`)**:
  ```cpp
  semantic_names[i] = src_elements[i].semantic_name;
  ```
- **Blast Radius**: `std::string::operator=(const char*)` invokes `strlen()`, which reads past the 32-byte boundary in `payload` until encountering a zero byte in adjacent memory, causing potential buffer over-read or access violation.
- **Mitigation**: Use bounded string construction:
  ```cpp
  size_t name_len = strnlen(src_elements[i].semantic_name, sizeof(src_elements[i].semantic_name));
  semantic_names[i].assign(src_elements[i].semantic_name, name_len);
  ```

---

## Stress Test Results

Executed via independent test harness `build/test_server_stress.exe` (181 assertions total):

| # | Test Scenario | Expected Behavior | Actual Behavior | Result |
|---|---------------|-------------------|-----------------|:------:|
| 1 | Partial packet disconnect (16B header, 0B payload) | Server drops socket cleanly, loop exits without hang | Exited cleanly in 12 ms, zero hang | **PASS** |
| 2 | Hard TCP RST mid-stream (`SO_LINGER {1,0}`) | Server catches `WSAECONNRESET` without exception | Caught cleanly, no crash | **PASS** |
| 3 | Disconnect during `OP_PRESENT` readback stream | Server handles write error on broken pipe gracefully | Handled cleanly by `SendExact`, zero hang | **PASS** |
| 4 | Rapid multi-client churn (20 adversarial drops + 1 legitimate) | Server survives all churn; 21st client renders/presents | Cleanly accepted 20 drops, rendered perfect frame | **PASS** |
| 5 | Bogus opcodes (0x0000, 0x0004, 0xFFFF, etc.) | Safely rejected with `false` | All 15 bogus opcodes rejected | **PASS** |
| 6 | Truncated zero-length / 1-byte / 4-byte payloads | Safely rejected without buffer over-read | 32 opcodes rejected; opcode 0x33 crashed driver | **FAIL** |
| 7 | Corrupted variable payloads (initial data, shaders, layouts) | Payload length mismatches safely rejected | All variable-length mismatches rejected | **PASS** |
| 8 | Random pseudorandom bitstream fuzzing (150 packets) | Server discards malformed packets without crash | 8 driver crashes triggered by unvalidated slots/topo | **FAIL** |
| 9 | Out-of-bounds parameter stress (topology, slots) | Server rejects invalid slots and topologies | 11 direct driver access violations (`0xC0000005`) | **FAIL** |
| 10 | Non-existent resource ID lookups (16 opcodes with `0xDEADBEEF`) | Safe no-op or unbind, zero null dereference | Handled 100% cleanly without crash | **PASS** |
| 11 | Resource ID 0 (Null/Unbind/Default) lookups | Unbinds resource or binds default offscreen target | Correctly bound/unbound cleanly | **PASS** |
| 12 | Cross-type COM QueryInterface type confusion | Buffer passed as shader returns `nullptr` via `ComPtr::As` | `ComPtr::As` prevented all type confusion | **PASS** |
| 13 | Use-after-free and double-destroy resistance | Second destroy and subsequent write return `false` | Both safely rejected without crash | **PASS** |
| 14 | Repeated 1,000 buffer allocation & destruction (x2) | COM refcounts released, memory stabilized | 1,000 buffers freed, count=0 | **PASS** |
| 15 | Repeated 1,000 texture allocation & destruction | COM refcounts released, memory stabilized | 1,000 textures freed, count=0 | **PASS** |
| 16 | Session Reset with 1,000 active allocations | Releases all 1,000 tracked COM objects | All cleared, count=0 | **PASS** |
| 17 | Interleaved 500 rapid create/use/destroy cycles | Zero memory leak across 4,500 total allocations | Memory plateaued at 143 MB (only 2 MB growth) | **PASS** |
| 18 | `HostGpuDevice` invalid dimensions (0x0, 0x512, 512x0, 65536) | Safely rejected without crash | All rejected with `false` | **PASS** |
| 19 | `HostGpuDevice` readback without targets or post-reset | Safely returns `false` | Returned `false` cleanly | **PASS** |
| 20 | Rapid target resizing churn (64x64 up to 1080p and odd 137x89) | Reallocates offscreen & staging textures cleanly | Readback pixel buffers match all sizes | **PASS** |

---

## Areas Verified Robust

1. **Network Fault Tolerance & Clean Socket Drops**:
   `ServerDispatcher::ProcessPacketStream` and `NetworkSocket` handle abrupt client disconnects, missing packet payloads, abortive resets (`SO_LINGER`), and broken pipe writes during `OP_PRESENT` flawlessly. No socket hangs or memory leaks occur during churn.
2. **Server Resource Manager & COM Type Safety**:
   `ServerResourceManager` provides robust encapsulation. The template method `Get<T>()` properly uses `ComPtr::As()` to invoke COM `QueryInterface()`, completely mitigating cross-resource type confusion attacks.
3. **GPU Memory Leak Resistance**:
   Across over 5,500 total resource allocations and destructions (including two rounds of 1,000 64-KB buffers, 1,000 textures, session resets, and rapid interleaved cycles), COM refcounts are completely released, and process memory stabilizes at 143 MB with only 2 MB of growth after driver pool warm-up.
4. **HostGpuDevice Headless Pipeline & Readback**:
   Adapter enumeration, headless D3D11 device creation, offscreen targets, and row-pitch packing for non-standard dimensions (137x89) operate with 100% mathematical fidelity.

---

## Required Remediation for Worker M2

To achieve `APPROVE`, Worker M2 must implement input boundary validation in `src/server/server_dispatcher.cpp`:

1. **In `HandleIASetTopology`**:
   Validate `p->topology` against valid `D3D11_PRIMITIVE_TOPOLOGY` enum ranges before calling `context->IASetPrimitiveTopology`. Return `false` if invalid.
2. **In `HandleVSSetCB` and `HandlePSSetCB`**:
   Validate `p->slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT` (14). Return `false` if out of bounds.
3. **In `HandlePSSetSrv`**:
   Validate `p->slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT` (128). Return `false` if out of bounds.
4. **In `HandlePSSetSampler`**:
   Validate `p->slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT` (16). Return `false` if out of bounds.
5. **In `HandleIASetVertexBuffer`**:
   Validate `p->slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT` (32). Return `false` if out of bounds.
6. **In `HandleIASetIndexBuffer`**:
   Validate `p->format == DXGI_FORMAT_R16_UINT || p->format == DXGI_FORMAT_R32_UINT || p->format == DXGI_FORMAT_UNKNOWN`. Return `false` if invalid.
7. **In `HandleCreateInputLayout`**:
   Use `strnlen` to safely bound the copy of `semantic_name` into `std::string`.
