# Handoff Report: CreateInputLayout & Resource Creation Safety Specification

**Agent**: Explorer M2 Remediation 2 (`explorer_m2_r2_2`)  
**Recipient**: Orchestrator & Worker M2  
**Working Directory**: `C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m2_r2_2`  
**Specification Document**: `C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m2_r2_2\input_layout_safety_spec.md`  
**Date**: 2026-10-26  

---

## 1. Observation

1. **Semantic Name Unbounded Copy in `HandleCreateInputLayout`**:
   - File: `src/server/server_dispatcher.cpp:591-594`
   - Verbatim Code:
     ```cpp
     for (uint32_t i = 0; i < p->num_elements; ++i) {
         semantic_names[i] = src_elements[i].semantic_name;
         d3d_elements[i].SemanticName = semantic_names[i].c_str();
     ```
   - Structure: `src/common/protocol.h:192-200`:
     ```cpp
     struct RgpuInputElementDesc {
         char     semantic_name[32];   // e.g. "POSITION", "TEXCOORD", "NORMAL", "COLOR"
         uint32_t semantic_index;
         uint32_t format;
         uint32_t input_slot;
         uint32_t aligned_byte_offset;
         uint32_t input_slot_class;
         uint32_t instance_data_step;
     };
     ```
   - Observation: `src_elements[i].semantic_name` is an un-terminated 32-byte char array. Passing it directly to `std::string::operator=` triggers `strlen()`, which performs an unbounded read past the 32-byte boundary when all 32 bytes are non-null.

2. **Challenger Adversarial Stress Test Reproduction**:
   - File: `tests/test_server_stress.cpp:597-614`
   - Test Scenario:
     ```cpp
     CreateInputLayoutPayload ilp{ 9990, 1, 0 };
     RgpuInputElementDesc elem{};
     std::memset(elem.semantic_name, 'A', sizeof(elem.semantic_name)); // 32 bytes of 'A', NO '\0'
     elem.format = DXGI_FORMAT_R32G32B32_FLOAT;
     std::vector<uint8_t> pl(sizeof(ilp) + sizeof(elem));
     std::memcpy(pl.data(), &ilp, sizeof(ilp));
     std::memcpy(pl.data() + sizeof(ilp), &elem, sizeof(elem));
     PacketHeader h{ RGPU_MAGIC, OP_CREATE_INPUT_LAYOUT, FLAG_NONE, 402, static_cast<uint32_t>(pl.size()) };
     DWORD exc = 0;
     SafeDispatchCommand(dispatcher, h, pl.data(), pl.size(), nullptr, &exc);
     ```
   - Observation: When 32 non-null characters are transmitted without trailing zero byte, `strlen` reads past the element boundary into adjacent memory. In random packet fuzzing (Test 2.5, lines 550-577), 8 access violations were triggered.

3. **Unchecked Direct3D 11 Element Count Limit**:
   - File: `src/server/server_dispatcher.cpp:576, 588`
   - Verbatim Code:
     ```cpp
     size_t elements_size = static_cast<size_t>(p->num_elements) * sizeof(RgpuInputElementDesc);
     size_t expected_total = sizeof(CreateInputLayoutPayload) + elements_size + p->bytecode_length;
     ...
     std::vector<D3D11_INPUT_ELEMENT_DESC> d3d_elements(p->num_elements);
     ```
   - Observation: Direct3D 11 defines `D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT = 32`. A remote packet with `num_elements > 32` causes unchecked vector allocation and invokes `device->CreateInputLayout` with out-of-spec element counts.

4. **Resource Creation Missing Parameter Bounds**:
   - `HandleCreateBuffer` (`src/server/server_dispatcher.cpp:292-298`):
     ```cpp
     if (p->has_initial_data && p->initial_data_size > 0) {
         if (size < sizeof(CreateBufferPayload) + p->initial_data_size) {
     ```
     Observation: If `p->initial_data_size < p->byte_width`, `device->CreateBuffer` reads `p->byte_width` bytes from `pSysMem`, reading out-of-bounds of the transmitted initial data.
   - `HandleCreateTexture2D` (`src/server/server_dispatcher.cpp:325-333`):
     Observation: Width, height, array size, mip counts, and `p->format` are not validated against Direct3D 11 limits (`16384` dimension, `2048` array size, `15` mips).
   - `HandleCreateSrv`, `HandleCreateRtv`, `HandleCreateDsv` (`src/server/server_dispatcher.cpp:384, 442, 496`):
     Observation: View dimensions (`view_dimension`) and format enums are cast directly without checking against D3D11 view dimension bounds (SRV $\le 11$, RTV $\le 8$, DSV $\le 6$).
   - `HandleCreateSampler` (`src/server/server_dispatcher.cpp:627-632`):
     Observation: `address_u/v/w` and `comparison_func` are cast without verifying range `[1, 5]` and `[1, 8]`.
   - `HandleIASetIndexBuffer` (`src/server/server_dispatcher.cpp:699`):
     Observation: Format is cast to `DXGI_FORMAT` without ensuring it is `DXGI_FORMAT_R16_UINT` (57), `DXGI_FORMAT_R32_UINT` (42), or `DXGI_FORMAT_UNKNOWN` (0).
   - `HandleWriteBufferDiscard` (`src/server/server_dispatcher.cpp:905`):
     Observation: `std::memcpy(mapped.pData + p->byte_offset, p_data, p->byte_width)` does not check `byte_offset + byte_width <= desc.ByteWidth`.

---

## 2. Logic Chain

1. From **Observation 1**, `std::string::operator=(const char*)` assumes the input is null-terminated. Because network packets can contain arbitrary byte streams, assuming null-termination is an invalid trust assumption.
2. From **Observation 2**, when an adversary sends 32 non-null bytes, `strlen()` reads past the buffer end. Bounding the read using `strnlen(src_elements[i].semantic_name, sizeof(src_elements[i].semantic_name))` strictly prevents reading beyond 32 bytes, and `std::string::assign()` guarantees null-termination for `d3d_elements[i].SemanticName = semantic_names[i].c_str()`.
3. From **Observation 3**, Direct3D 11 hardware architectures cap input assembly elements at 32 (`D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT`). Validating `p->num_elements <= 32` prevents integer multiplication overflow in `elements_size` and prevents unbounded allocations.
4. From **Observation 4**, D3D11 device routines delegate to hardware driver DLLs (`igd10iumd64.dll`, `nvldumdx.dll`, etc.). Drivers utilize jump tables indexed by enum values. Providing unvalidated integers over the network leads directly to `EXCEPTION_ACCESS_VIOLATION` (`0xC0000005`).
5. Validating all format enums with `IsValidDxgiFormat` (1..191), clamping/rejecting out-of-range view dimensions and sampler address modes, and verifying `initial_data_size >= byte_width` before buffer creation closes all identified attack vectors without impacting valid application workloads.

---

## 3. Caveats

1. Direct3D 11 extended format IDs exist up to 191 (`DXGI_FORMAT_A4B4G4R4_UNORM`). Checking `format >= 1 && format <= 191` accepts all current and legacy DXGI formats while blocking arbitrary large integers.
2. For `HandleCreateInputLayout`, if `p->num_elements == 0`, passing `nullptr` for `pInputElementDescs` is allowed by Direct3D for vertex shaders using `SV_VertexID`. The specification handles `num_elements == 0` cleanly.
3. For `HandleWriteBufferDiscard`, dynamic buffers mapped with `D3D11_MAP_WRITE_DISCARD` discard the entire subresource, but `p->byte_offset + p->byte_width` must remain within `desc.ByteWidth`.
4. State-binding opcode slot checks (constant buffer slot $< 14$, sampler slot $< 16$, SRV slot $< 128$) and primitive topology enum checks are covered in parallel by Explorer 1 (`explorer_m2_r2_1`). Both specifications are complementary and non-conflicting.

---

## 4. Conclusion

The buffer over-read risk in `CreateInputLayout` and the driver crash risks across resource creation routines are fully characterized and remediated.

The remediation requires applying the precise C++ patches documented in:
`C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m2_r2_2\input_layout_safety_spec.md`

Specifically:
- In `HandleCreateInputLayout`: bound semantic name copy with `strnlen`, enforce `p->num_elements <= 32`, validate `format != UNKNOWN`, validate `input_slot < 32`, and validate `input_slot_class <= 1`.
- In `HandleCreateBuffer`: validate `byte_width > 0 && <= 2048 MB`, `usage <= 3`, `initial_data_size >= byte_width`, and constant buffer 16-byte alignment.
- In `HandleCreateTexture2D`: validate dimensions $\le 16384$, array size $\le 2048$, mips $\le 15$, and format.
- In `HandleCreateSrv`, `HandleCreateRtv`, `HandleCreateDsv`: validate format enums and view dimension limits.
- In `HandleCreateSampler`: validate address modes $\in [1, 5]$, comparison func $\in [1, 8]$, max anisotropy $\le 16$.
- In `HandleIASetIndexBuffer`: validate format $\in \{\text{R16\_UINT}, \text{R32\_UINT}, \text{UNKNOWN}\}$.
- In `HandleWriteBufferDiscard`: validate `byte_offset + byte_width <= desc.ByteWidth`.
- Reject `res_id == 0` across all creation handlers.

---

## 5. Verification Method

1. **Compile Stress Test Harness**:
   ```powershell
   clang++ -std=c++17 -fms-extensions -O2 -Isrc/common -Isrc/server src/common/transport.cpp src/server/gpu_device.cpp src/server/server_dispatcher.cpp tests/test_server_stress.cpp -o build/test_server_stress.exe -ld3d11 -ldxgi -lws2_32 -ld3dcompiler -lpsapi
   ```
2. **Execute Stress Suite**:
   ```powershell
   .\build\test_server_stress.exe
   ```
3. **Verify Existing Server Unit Tests**:
   ```powershell
   clang++ -std=c++17 -O2 -Isrc/common -Isrc/server src/common/transport.cpp src/server/gpu_device.cpp src/server/server_dispatcher.cpp tests/test_server.cpp -o build/test_server.exe -ld3d11 -ldxgi -lws2_32 -ld3dcompiler -lpsapi
   .\build\test_server.exe
   ```
4. **Acceptance Criteria**:
   - `build/test_server_stress.exe` exits with code 0.
   - Zero `EXCEPTION_ACCESS_VIOLATION` (`0xC0000005`) crashes.
   - All 181 stress test assertions pass.
   - All 15 server unit tests in `test_server.exe` pass.
