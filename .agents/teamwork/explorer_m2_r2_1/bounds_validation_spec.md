# Milestone 2 Server Bounds Validation Specification
## Hardening Direct3D 11 Host Execution against Remote Adversarial Crashes

**Document Version**: 1.0.0  
**Author**: Explorer M2 Remediation 1 (`explorer_m2_r2_1`)  
**Target File**: `src/server/server_dispatcher.cpp`  
**Governing Documents**: `ORIGINAL_REQUEST.md`, `PROJECT.md`, `challenge_report.md`  
**Date**: 2026-10-26  

---

## 1. Executive Summary & Root Cause Analysis

### 1.1 The Vulnerability
During Milestone 2 gate adversarial testing (`tests/test_server_stress.cpp`), Challenger 1 identified that `ServerDispatcher` forwarded raw network-deserialized parameters directly to Direct3D 11 Immediate Context methods (`IASetPrimitiveTopology`, `VSSetConstantBuffers`, `PSSetConstantBuffers`, `PSSetShaderResources`, `PSSetSamplers`, `IASetVertexBuffers`).

In retail Direct3D 11 drivers (such as Intel UHD Graphics `igd10iumd64.dll`), immediate context methods do not perform defensive perimeter checks. When supplied with out-of-range enum values or pipeline slot numbers exceeding hardware register limits:
1. The driver performs out-of-bounds indexing in internal state jump tables and array structures.
2. The operating system kernel intercepts the unhandled hardware memory fault as an **Access Violation (`0xC0000005`)**.
3. Because the driver state is corrupted, even if caught by Structured Exception Handling (`__except`), subsequent context cleanups (`context->ClearState()`, `context->Flush()`, or session reset) trigger secondary fatal memory faults that terminate the entire host daemon process.

Furthermore, `HandleCreateInputLayout` performed unbounded string construction from 32-byte semantic name buffers, creating a heap/stack buffer over-read hazard if client data was not null-terminated.

### 1.2 The Remediation Mandate
The server dispatcher must act as a **zero-trust hardware defense boundary**. Every opcode handler must validate:
1. **Packet Payload Length**: Header length vs struct size, including non-overflowing variable-length payload offsets.
2. **Enum Ranges**: Only documented, standard Direct3D 11 / DXGI enumerations may be cast and forwarded.
3. **Hardware Architectural Slots**: Slot indices must be strictly validated against Direct3D 11 limits prior to invoking COM methods.
4. **Pointer & Buffer Bounds**: String copies must be bounded via `strnlen`; buffer write offsets must not exceed target resource capacities.

Any packet failing validation must be rejected immediately by returning `false`, logging a diagnostic warning without invoking any context methods, ensuring the host GPU driver remains 100% uncorrupted.

---

## 2. Direct3D 11 Architectural Limits & Invariants Reference

The following table documents the authoritative architectural limits defined by Microsoft Direct3D 11 headers (`d3d11.h`, `d3dcommon.h`, `dxgiformat.h`) and enforced by GPU hardware:

| Parameter Category | Constant Name | Architectural Limit | Direct3D Context Method | Impact of Breach |
|---|---|:---:|---|---|
| **Constant Buffer Slots** | `D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT` | **14** (Slots `0..13`) | `VSSetConstantBuffers`, `PSSetConstantBuffers` | Driver memory AV `0xC0000005` at `slot >= 14` |
| **Sampler Slots** | `D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT` | **16** (Slots `0..15`) | `PSSetSamplers` | Driver memory AV `0xC0000005` at `slot >= 16` |
| **Shader Resource Slots** | `D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT` | **128** (Slots `0..127`) | `PSSetShaderResources` | Driver memory AV `0xC0000005` at `slot >= 128` |
| **Vertex Buffer Slots** | `D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT` | **32** (Slots `0..31`) | `IASetVertexBuffers` | Driver memory AV `0xC0000005` at `slot >= 32` |
| **Input Layout Elements** | `D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT` | **32** (Elements `0..31`) | `CreateInputLayout` | Driver memory fault or `E_INVALIDARG` |
| **Simultaneous Render Targets** | `D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT` | **8** (Views `0..7`) | `OMSetRenderTargets` | Driver memory AV `0xC0000005` at `num_rt > 8` |
| **Pipeline Viewports** | `D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE` | **16** (Viewports `0..15`) | `RSSetViewports` | Driver crash or `E_INVALIDARG` at `num_vp > 16` |
| **Texture 2D Dimensions** | `D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION` | **16,384** pixels | `CreateTexture2D`, `CreateOffscreenTargets` | Allocation failure, device removed |
| **Texture 2D Array Axis** | `D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION` | **2,048** slices | `CreateTexture2D` | `E_INVALIDARG` |
| **Texture Mip Levels** | `D3D11_REQ_MIP_LEVELS` | **15** levels | `CreateTexture2D` | `E_INVALIDARG` |
| **Constant Buffer ByteWidth** | Multiple of **16** bytes, max **65,536** B | 64 KB, 16B aligned | `CreateBuffer` | `E_INVALIDARG` |
| **Primitive Topology** | Valid `D3D11_PRIMITIVE_TOPOLOGY` enum | Set $\{0, [1..5], [10..13], [33..64]\}$ | `IASetPrimitiveTopology` | Immediate driver jump table AV `0xC0000005` |
| **Index Buffer Format** | `DXGI_FORMAT_R16_UINT` (57), `R32_UINT` (42), `UNKNOWN` (0) | Only formats 57, 42, 0 | `IASetIndexBuffer` | Driver crash or `E_INVALIDARG` |

---

## 3. Opcode-by-Opcode Bounds Validation Specification

Every opcode processed in `ServerDispatcher::DispatchCommand` is categorized and analyzed below with exact validation rules.

### 3.1 Category 1: Session & Lifecycle (Opcodes `0x0001` - `0x000F`)

#### Opcode `0x0001`: `OP_HANDSHAKE_REQUEST`
- **Payload Struct**: `HandshakeRequestPayload` (20 bytes)
- **Minimum Payload Size**: `sizeof(HandshakeRequestPayload)` (20 bytes).
- **Validation Rules**:
  1. `size < sizeof(HandshakeRequestPayload)`: Return `false`.
  2. Protocol Version: `req->protocol_version == RGPU_PROTOCOL_VERSION` (1). If mismatch, send error response (`status_code = 1`) and return `false`.
  3. Window Dimensions: If `req->client_width > 16384 || req->client_height > 16384`: Reject with `status_code = 2` (GPU Init Failed) or return `false`.
  4. Offscreen Allocation: `m_gpuDevice.CreateOffscreenTargets(req->client_width, req->client_height)`. If creation fails, return `false`.

#### Opcode `0x0002`: `OP_HANDSHAKE_RESPONSE`
- **Server Role**: Server generates this packet; incoming packets with this opcode on server are ignored/no-op (`return true`).

#### Opcode `0x0003`: `OP_SHUTDOWN`
- **Payload Struct**: `ShutdownPayload` (4 bytes)
- **Validation Rules**:
  1. If `size < sizeof(ShutdownPayload)`: Set exit code 0, cleanly call `ResetSession()`, return `true`.
  2. If `size >= sizeof(ShutdownPayload)`: Extract `exit_code`, call `ResetSession()`, return `true`.

---

### 3.2 Category 2: Resource Management (Opcodes `0x0010` - `0x002F`)

#### Opcode `0x0010`: `OP_CREATE_BUFFER`
- **Payload Struct**: `CreateBufferPayload` (36 bytes) + optional initial data.
- **Validation Rules**:
  1. Payload Header: `size < sizeof(CreateBufferPayload)` $\rightarrow$ return `false`.
  2. Resource ID: `p->res_id == 0` $\rightarrow$ return `false` (Resource ID 0 is reserved for unbinding/null).
  3. Byte Width: `p->byte_width == 0 || p->byte_width > 0x7FFFFFFF` $\rightarrow$ return `false` (Direct3D rejects 0-byte or > 2GB buffers).
  4. Constant Buffer Invariant: If `(p->bind_flags & 4)` (`D3D11_BIND_CONSTANT_BUFFER`):
     - `p->byte_width % 16 != 0`: Direct3D requires 16-byte alignment. Return `false`.
     - `p->byte_width > 65536`: Direct3D maximum constant buffer size is 64 KB. Return `false`.
  5. Usage Enum: `p->usage > 3` (`D3D11_USAGE_STAGING`) $\rightarrow$ return `false`.
  6. Initial Data Bounds & Overflow Prevention:
     - If `p->has_initial_data != 0`:
       - `p->initial_data_size == 0` $\rightarrow$ return `false`.
       - Non-overflowing size check: `if (size - sizeof(CreateBufferPayload) < p->initial_data_size)` $\rightarrow$ return `false`.
     - If `p->usage == 1` (`D3D11_USAGE_IMMUTABLE`):
       - Immutable buffers MUST have initial data: `if (!p->has_initial_data || p->initial_data_size < p->byte_width)` $\rightarrow$ return `false`.
  7. Creation Failure: If `FAILED(device->CreateBuffer(...))` $\rightarrow$ return `false`.

#### Opcode `0x0011`: `OP_CREATE_TEXTURE2D`
- **Payload Struct**: `CreateTexture2DPayload` (64 bytes) + optional initial data.
- **Validation Rules**:
  1. Payload Header: `size < sizeof(CreateTexture2DPayload)` $\rightarrow$ return `false`.
  2. Resource ID: `p->res_id == 0` $\rightarrow$ return `false`.
  3. Dimensions:
     - `p->width == 0 || p->height == 0` $\rightarrow$ return `false`.
     - `p->width > 16384 || p->height > 16384` (`D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION`) $\rightarrow$ return `false`.
  4. Array Size & Mips:
     - `p->array_size == 0 || p->array_size > 2048` (`D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION`) $\rightarrow$ return `false`.
     - `p->mips > 15` (`D3D11_REQ_MIP_LEVELS`) $\rightarrow$ return `false`.
  5. Usage Enum: `p->usage > 3` $\rightarrow$ return `false`.
  6. Format: `p->format == 0 || p->format > 190` $\rightarrow$ return `false`.
  7. Initial Data Bounds & Overflow Prevention:
     - If `p->has_initial_data != 0`:
       - `p->initial_data_size == 0 || p->sys_mem_pitch == 0` $\rightarrow$ return `false`.
       - Non-overflowing size check: `if (size - sizeof(CreateTexture2DPayload) < p->initial_data_size)` $\rightarrow$ return `false`.
  8. Creation Failure: If `FAILED(device->CreateTexture2D(...))` $\rightarrow$ return `false`.

#### Opcode `0x0012`: `OP_CREATE_SRV`
- **Payload Struct**: `CreateSrvPayload` (24 bytes)
- **Validation Rules**:
  1. Payload Header: `size < sizeof(CreateSrvPayload)` $\rightarrow$ return `false`.
  2. Resource IDs: `p->res_id == 0 || p->resource_id == 0` $\rightarrow$ return `false`.
  3. Resource Resolution: `resource = m_resources.Get<ID3D11Resource>(p->resource_id)`. If `!resource` $\rightarrow$ return `false`.
  4. View Dimension: `p->view_dimension > 11` (`D3D11_SRV_DIMENSION_BUFFEREX`) $\rightarrow$ return `false`.
  5. Format: `p->format > 190` $\rightarrow$ return `false`.
  6. Creation Failure: If `FAILED(device->CreateShaderResourceView(...))` $\rightarrow$ return `false`.

#### Opcode `0x0013`: `OP_CREATE_RTV`
- **Payload Struct**: `CreateRtvPayload` (20 bytes)
- **Validation Rules**:
  1. Payload Header: `size < sizeof(CreateRtvPayload)` $\rightarrow$ return `false`.
  2. Resource ID: `p->res_id == 0` $\rightarrow$ return `false`.
  3. View Dimension: `p->view_dimension > 8` (`D3D11_RTV_DIMENSION_TEXTURE3D`) $\rightarrow$ return `false`.
  4. Resource Resolution:
     - If `p->resource_id == 0`: Target default offscreen backbuffer.
     - If `p->resource_id != 0`: Look up via `m_resources.Get<ID3D11Resource>(p->resource_id)`. If missing and default texture available, fallback; otherwise return `false`.
  5. Creation Failure: If `FAILED(device->CreateRenderTargetView(...))` $\rightarrow$ return `false`.

#### Opcode `0x0014`: `OP_CREATE_DSV`
- **Payload Struct**: `CreateDsvPayload` (24 bytes)
- **Validation Rules**:
  1. Payload Header: `size < sizeof(CreateDsvPayload)` $\rightarrow$ return `false`.
  2. Resource ID: `p->res_id == 0` $\rightarrow$ return `false`.
  3. View Dimension: `p->view_dimension > 6` (`D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY`) $\rightarrow$ return `false`.
  4. Resource Resolution:
     - If `p->resource_id == 0`: Target default offscreen depth buffer.
     - If `p->resource_id != 0`: Look up via `m_resources.Get<ID3D11Resource>(p->resource_id)`. If missing, fallback to default depth texture; if still missing, return `false`.
  5. Creation Failure: If `FAILED(device->CreateDepthStencilView(...))` $\rightarrow$ return `false`.

#### Opcode `0x0015`: `OP_CREATE_VERTEX_SHADER`
- **Payload Struct**: `CreateShaderPayload` (8 bytes) + bytecode
- **Validation Rules**:
  1. Payload Header: `size < sizeof(CreateShaderPayload)` $\rightarrow$ return `false`.
  2. Resource ID: `p->res_id == 0` $\rightarrow$ return `false`.
  3. Bytecode Length: `p->bytecode_length == 0 || p->bytecode_length > 67108864` (64 MB sanity limit) $\rightarrow$ return `false`.
  4. Bounds & Overflow: `if (size - sizeof(CreateShaderPayload) < p->bytecode_length)` $\rightarrow$ return `false`.
  5. Creation Failure: If `FAILED(device->CreateVertexShader(...))` $\rightarrow$ return `false`.

#### Opcode `0x0016`: `OP_CREATE_PIXEL_SHADER`
- **Payload Struct**: `CreateShaderPayload` (8 bytes) + bytecode
- **Validation Rules**:
  1. Payload Header: `size < sizeof(CreateShaderPayload)` $\rightarrow$ return `false`.
  2. Resource ID: `p->res_id == 0` $\rightarrow$ return `false`.
  3. Bytecode Length: `p->bytecode_length == 0 || p->bytecode_length > 67108864` $\rightarrow$ return `false`.
  4. Bounds & Overflow: `if (size - sizeof(CreateShaderPayload) < p->bytecode_length)` $\rightarrow$ return `false`.
  5. Creation Failure: If `FAILED(device->CreatePixelShader(...))` $\rightarrow$ return `false`.

#### Opcode `0x0017`: `OP_CREATE_INPUT_LAYOUT`
- **Payload Struct**: `CreateInputLayoutPayload` (12 bytes) + `num_elements * RgpuInputElementDesc` (56 bytes each) + bytecode
- **Validation Rules**:
  1. Payload Header: `size < sizeof(CreateInputLayoutPayload)` $\rightarrow$ return `false`.
  2. Resource ID: `p->res_id == 0` $\rightarrow$ return `false`.
  3. Element Count: `p->num_elements == 0 || p->num_elements > 32` (`D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT`) $\rightarrow$ return `false`.
  4. Bytecode Length: `p->bytecode_length == 0 || p->bytecode_length > 67108864` $\rightarrow$ return `false`.
  5. Compound Bounds Check:
     ```cpp
     size_t elements_size = static_cast<size_t>(p->num_elements) * sizeof(RgpuInputElementDesc);
     if (size < sizeof(CreateInputLayoutPayload) + elements_size) return false;
     if (size - sizeof(CreateInputLayoutPayload) - elements_size < p->bytecode_length) return false;
     ```
  6. Per-Element Field Bounds:
     - **Semantic Name Buffer Over-Read Prevention**:
       ```cpp
       size_t name_len = strnlen(src_elements[i].semantic_name, sizeof(src_elements[i].semantic_name));
       if (name_len == 0) return false; // Empty semantic name invalid
       semantic_names[i].assign(src_elements[i].semantic_name, name_len);
       ```
     - **Input Slot**: `if (src_elements[i].input_slot >= 32)` (`D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT`) $\rightarrow$ return `false`.
     - **Input Slot Class**: `if (src_elements[i].input_slot_class > 1)` (`D3D11_INPUT_PER_INSTANCE_DATA`) $\rightarrow$ return `false`.
     - **Format**: `if (src_elements[i].format == 0 || src_elements[i].format > 190)` $\rightarrow$ return `false`.
  7. Creation Failure: If `FAILED(device->CreateInputLayout(...))` $\rightarrow$ return `false`.

#### Opcode `0x0019`: `OP_CREATE_SAMPLER`
- **Payload Struct**: `CreateSamplerPayload` (56 bytes)
- **Validation Rules**:
  1. Payload Header: `size < sizeof(CreateSamplerPayload)` $\rightarrow$ return `false`.
  2. Resource ID: `p->res_id == 0` $\rightarrow$ return `false`.
  3. Address Modes: `if (p->address_u < 1 || p->address_u > 5 || p->address_v < 1 || p->address_v > 5 || p->address_w < 1 || p->address_w > 5)` $\rightarrow$ return `false`.
  4. Comparison Func: `if (p->comparison_func < 1 || p->comparison_func > 8)` $\rightarrow$ return `false`.
  5. Anisotropy Bound: Clamp or validate `p->max_anisotropy <= 16`.
  6. Creation Failure: If `FAILED(device->CreateSamplerState(...))` $\rightarrow$ return `false`.

#### Opcode `0x0018`: `OP_DESTROY_RESOURCE`
- **Payload Struct**: `DestroyResourcePayload` (4 bytes)
- **Validation Rules**:
  1. Payload Header: `size < sizeof(DestroyResourcePayload)` $\rightarrow$ return `false`.
  2. Resource ID: `p->res_id == 0` $\rightarrow$ return `false`.
  3. Destruction Result: `return m_resources.Destroy(p->res_id);` (returns `false` cleanly if non-existent).

---

### 3.3 Category 3: Pipeline State & Binding (Opcodes `0x0030` - `0x004F`)

#### Opcode `0x0030`: `OP_IA_SET_INPUT_LAYOUT`
- **Payload Struct**: `IASetInputLayoutPayload` (4 bytes)
- **Validation Rules**:
  1. `size < sizeof(IASetInputLayoutPayload)` $\rightarrow$ return `false`.
  2. Safe Unbind: If `p->res_id == 0`, pass `nullptr` to `context->IASetInputLayout(nullptr)` $\rightarrow$ return `true`.
  3. Resolution: Look up via `m_resources.GetInputLayout(p->res_id)`. If not found, unbinds safely (`nullptr`).

#### Opcode `0x0031`: `OP_IA_SET_VERTEX_BUFFER`
- **Payload Struct**: `IASetVertexBufferPayload` (16 bytes)
- **Validation Rules**:
  1. `size < sizeof(IASetVertexBufferPayload)` $\rightarrow$ return `false`.
  2. **Hardware Slot Bound**:
     ```cpp
     if (p->slot >= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) { // 32
         std::cerr << "[ServerDispatcher] Invalid vertex buffer slot: " << p->slot << std::endl;
         return false;
     }
     ```
  3. Safe Unbind: If `p->res_id == 0`, buffer pointer is `nullptr` $\rightarrow$ unbinds slot.

#### Opcode `0x0032`: `OP_IA_SET_INDEX_BUFFER`
- **Payload Struct**: `IASetIndexBufferPayload` (12 bytes)
- **Validation Rules**:
  1. `size < sizeof(IASetIndexBufferPayload)` $\rightarrow$ return `false`.
  2. **Format Bound**:
     ```cpp
     if (p->format != DXGI_FORMAT_R16_UINT && 
         p->format != DXGI_FORMAT_R32_UINT && 
         p->format != DXGI_FORMAT_UNKNOWN) {
         std::cerr << "[ServerDispatcher] Invalid index buffer format: " << p->format << std::endl;
         return false;
     }
     ```
  3. Safe Unbind: If `p->res_id == 0`, buffer pointer is `nullptr`.

#### Opcode `0x0033`: `OP_IA_SET_TOPOLOGY`
- **Payload Struct**: `IASetTopologyPayload` (4 bytes)
- **Validation Rules**:
  1. `size < sizeof(IASetTopologyPayload)` $\rightarrow$ return `false`.
  2. **Primitive Topology Range Bound**:
     Direct3D 11 hardware primitive topologies are strictly partitioned:
     - `D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED` (0)
     - `POINTLIST` (1) through `TRIANGLESTRIP` (5)
     - `LINELIST_ADJ` (10) through `TRIANGLESTRIP_ADJ` (13)
     - `1_CONTROL_POINT_PATCHLIST` (33) through `32_CONTROL_POINT_PATCHLIST` (64)
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

#### Opcode `0x0034`: `OP_VS_SET_SHADER`
- **Payload Struct**: `VSSetShaderPayload` (4 bytes)
- **Validation Rules**:
  1. `size < sizeof(VSSetShaderPayload)` $\rightarrow$ return `false`.
  2. Safe Unbind: If `p->res_id == 0`, pass `nullptr` to `context->VSSetShader(nullptr, nullptr, 0)`.

#### Opcode `0x0035`: `OP_VS_SET_CB`
- **Payload Struct**: `VSSetConstantBufferPayload` (8 bytes)
- **Validation Rules**:
  1. `size < sizeof(VSSetConstantBufferPayload)` $\rightarrow$ return `false`.
  2. **Hardware Slot Bound**:
     ```cpp
     if (p->slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) { // 14
         std::cerr << "[ServerDispatcher] Invalid VS constant buffer slot: " << p->slot << std::endl;
         return false;
     }
     ```
  3. Safe Unbind: If `p->res_id == 0`, buffer pointer is `nullptr` $\rightarrow$ unbinds slot.

#### Opcode `0x0036`: `OP_PS_SET_SHADER`
- **Payload Struct**: `PSSetShaderPayload` (4 bytes)
- **Validation Rules**:
  1. `size < sizeof(PSSetShaderPayload)` $\rightarrow$ return `false`.
  2. Safe Unbind: If `p->res_id == 0`, pass `nullptr` to `context->PSSetShader(nullptr, nullptr, 0)`.

#### Opcode `0x0037`: `OP_PS_SET_CB`
- **Payload Struct**: `PSSetConstantBufferPayload` (8 bytes)
- **Validation Rules**:
  1. `size < sizeof(PSSetConstantBufferPayload)` $\rightarrow$ return `false`.
  2. **Hardware Slot Bound**:
     ```cpp
     if (p->slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) { // 14
         std::cerr << "[ServerDispatcher] Invalid PS constant buffer slot: " << p->slot << std::endl;
         return false;
     }
     ```
  3. Safe Unbind: If `p->res_id == 0`, buffer pointer is `nullptr` $\rightarrow$ unbinds slot.

#### Opcode `0x0038`: `OP_PS_SET_SRV`
- **Payload Struct**: `PSSetSrvPayload` (8 bytes)
- **Validation Rules**:
  1. `size < sizeof(PSSetSrvPayload)` $\rightarrow$ return `false`.
  2. **Hardware Slot Bound**:
     ```cpp
     if (p->slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) { // 128
         std::cerr << "[ServerDispatcher] Invalid PS SRV slot: " << p->slot << std::endl;
         return false;
     }
     ```
  3. Safe Unbind: If `p->res_id == 0`, SRV pointer is `nullptr` $\rightarrow$ unbinds slot.

#### Opcode `0x003B`: `OP_PS_SET_SAMPLER`
- **Payload Struct**: `PSSetSamplerPayload` (8 bytes)
- **Validation Rules**:
  1. `size < sizeof(PSSetSamplerPayload)` $\rightarrow$ return `false`.
  2. **Hardware Slot Bound**:
     ```cpp
     if (p->slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) { // 16
         std::cerr << "[ServerDispatcher] Invalid PS sampler slot: " << p->slot << std::endl;
         return false;
     }
     ```
  3. Safe Unbind: If `p->res_id == 0`, sampler pointer is `nullptr` $\rightarrow$ unbinds slot.

#### Opcode `0x0039`: `OP_RS_SET_VIEWPORT`
- **Payload Struct**: `RSSetViewportPayload` (24 bytes)
- **Validation Rules**:
  1. `size < sizeof(RSSetViewportPayload)` $\rightarrow$ return `false`.
  2. **Viewport Count Limit**: If payload holds multiple viewports (`size / sizeof(RSSetViewportPayload) > 1`):
     ```cpp
     UINT num_viewports = static_cast<UINT>(size / sizeof(RSSetViewportPayload));
     if (num_viewports > D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) { // 16
         std::cerr << "[ServerDispatcher] Viewport count exceeds limit: " << num_viewports << std::endl;
         return false;
     }
     ```
  3. **Floating Point Sanitization**:
     Ensure `p->width`, `p->height`, `p->top_left_x`, `p->top_left_y`, `p->min_depth`, and `p->max_depth` are finite (not NaN or $\pm\infty$):
     ```cpp
     if (!std::isfinite(p->width) || !std::isfinite(p->height) ||
         !std::isfinite(p->top_left_x) || !std::isfinite(p->top_left_y) ||
         !std::isfinite(p->min_depth) || !std::isfinite(p->max_depth)) {
         return false;
     }
     if (p->width < 0.0f || p->height < 0.0f || p->width > 32767.0f || p->height > 32767.0f) {
         return false;
     }
     if (p->min_depth < 0.0f || p->max_depth > 1.0f || p->min_depth > p->max_depth) {
         return false;
     }
     ```

#### Opcode `0x003A`: `OP_OM_SET_RENDER_TARGETS`
- **Payload Struct**: `OMSetRenderTargetsPayload` (8 bytes)
- **Validation Rules**:
  1. `size < sizeof(OMSetRenderTargetsPayload)` $\rightarrow$ return `false`.
  2. **Simultaneous Render Target Count Limit**:
     The number of simultaneous RTVs must never exceed `D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT` (8).
     In single-RTV dispatch, `num_rtvs = p_rtv ? 1 : 0` satisfies `num_rtvs <= 8`.
  3. Target Resolution & Safe Fallback:
     - If `p->rtv_id != 0`: Look up RTV. If missing, fallback to default offscreen RTV.
     - If `p->dsv_id != 0`: Look up DSV. If missing, fallback to default offscreen DSV.
     - Call: `context->OMSetRenderTargets(p_rtv ? 1 : 0, p_rtv ? &p_rtv : nullptr, dsv.Get());`

---

### 3.4 Category 4: Updates & Draw Calls (Opcodes `0x0050` - `0x006F`)

#### Opcode `0x0050`: `OP_UPDATE_SUBRESOURCE`
- **Payload Struct**: `UpdateSubresourcePayload` (48 bytes) + data
- **Validation Rules**:
  1. `size < sizeof(UpdateSubresourcePayload)` $\rightarrow$ return `false`.
  2. `p->res_id == 0 || p->data_size == 0` $\rightarrow$ return `false`.
  3. Non-overflowing Data Bounds: `if (size - sizeof(UpdateSubresourcePayload) < p->data_size)` $\rightarrow$ return `false`.
  4. Resource Resolution: `res = m_resources.Get<ID3D11Resource>(p->res_id)`. If `!res` $\rightarrow$ return `false`.
  5. Bounding Box Sanitization: If `p->has_dst_box != 0`:
     `if (p->dst_box.right <= p->dst_box.left || p->dst_box.bottom <= p->dst_box.top || p->dst_box.back <= p->dst_box.front)` $\rightarrow$ return `false`.

#### Opcode `0x0051`: `OP_WRITE_BUFFER_DISCARD`
- **Payload Struct**: `WriteBufferDiscardPayload` (12 bytes) + data
- **Validation Rules**:
  1. `size < sizeof(WriteBufferDiscardPayload)` $\rightarrow$ return `false`.
  2. `p->res_id == 0 || p->byte_width == 0` $\rightarrow$ return `false`.
  3. Non-overflowing Data Bounds: `if (size - sizeof(WriteBufferDiscardPayload) < p->byte_width)` $\rightarrow$ return `false`.
  4. Resource Resolution: `buf = m_resources.GetBuffer(p->res_id)`. If `!buf` $\rightarrow$ return `false`.
  5. **Buffer Capacity Bounds**:
     Query buffer capacity via `buf->GetDesc(&desc)`.
     `if (p->byte_offset > desc.ByteWidth || (desc.ByteWidth - p->byte_offset < p->byte_width))` $\rightarrow$ return `false`.

#### Opcode `0x0052`: `OP_CLEAR_RTV`
- **Payload Struct**: `ClearRtvPayload` (20 bytes)
- **Validation Rules**:
  1. `size < sizeof(ClearRtvPayload)` $\rightarrow$ return `false`.
  2. Floating Point Check: Ensure all 4 color components are finite (`std::isfinite`).
  3. RTV Resolution: If `p->res_id == 0`, clear default offscreen target; if non-zero, clear looked-up target or default if not found.

#### Opcode `0x0053`: `OP_CLEAR_DSV`
- **Payload Struct**: `ClearDsvPayload` (16 bytes)
- **Validation Rules**:
  1. `size < sizeof(ClearDsvPayload)` $\rightarrow$ return `false`.
  2. **Clear Flags Mask**: `valid_mask = D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL` (0x03).
     `if (p->clear_flags == 0 || (p->clear_flags & ~valid_mask) != 0)` $\rightarrow$ return `false`.
  3. Depth Sanitization: `if (!std::isfinite(p->depth) || p->depth < 0.0f || p->depth > 1.0f)` $\rightarrow$ return `false`.
  4. DSV Resolution: If `p->res_id == 0`, clear default offscreen DSV; if non-zero, clear looked-up DSV or default.

#### Opcode `0x0054`: `OP_DRAW`
- **Payload Struct**: `DrawPayload` (8 bytes)
- **Validation Rules**:
  1. `size < sizeof(DrawPayload)` $\rightarrow$ return `false`.
  2. If `p->vertex_count == 0`: Safe no-op, return `true`.

#### Opcode `0x0055`: `OP_DRAW_INDEXED`
- **Payload Struct**: `DrawIndexedPayload` (12 bytes)
- **Validation Rules**:
  1. `size < sizeof(DrawIndexedPayload)` $\rightarrow$ return `false`.
  2. If `p->index_count == 0`: Safe no-op, return `true`.

---

### 3.5 Category 5: Presentation (Opcodes `0x0070` - `0x0071`)

#### Opcode `0x0070`: `OP_PRESENT`
- **Payload Struct**: `PresentPayload` (20 bytes)
- **Validation Rules**:
  1. `size < sizeof(PresentPayload)` $\rightarrow$ return `false`.
  2. Readback Execution: `m_gpuDevice.ReadbackFrame(...)`. If readback fails, return `false`.
  3. Network Transmission: Handle socket disconnect cleanly without throw.

---

## 4. Concrete Implementation Blueprint for `src/server/server_dispatcher.cpp`

The following code snippets show exact, drop-in before $\rightarrow$ after modifications for `server_dispatcher.cpp`.

### 4.1 Topology Validation Helper & `HandleIASetTopology`

**Target File**: `src/server/server_dispatcher.cpp` (Lines ~703-713)

**Before**:
```cpp
bool ServerDispatcher::HandleIASetTopology(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(IASetTopologyPayload)) return false;
    const auto* p = reinterpret_cast<const IASetTopologyPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    context->IASetPrimitiveTopology(static_cast<D3D11_PRIMITIVE_TOPOLOGY>(p->topology));
    return true;
}
```

**After**:
```cpp
static inline bool IsValidD3D11PrimitiveTopology(uint32_t topology) {
    return (topology == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) ||
           (topology >= D3D11_PRIMITIVE_TOPOLOGY_POINTLIST && topology <= D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) ||
           (topology >= D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ && topology <= D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ) ||
           (topology >= D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST && topology <= D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST);
}

bool ServerDispatcher::HandleIASetTopology(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(IASetTopologyPayload)) return false;
    const auto* p = reinterpret_cast<const IASetTopologyPayload*>(payload);

    if (!IsValidD3D11PrimitiveTopology(p->topology)) {
        std::cerr << "[ServerDispatcher] Invalid primitive topology: " << p->topology << std::endl;
        return false;
    }

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    context->IASetPrimitiveTopology(static_cast<D3D11_PRIMITIVE_TOPOLOGY>(p->topology));
    return true;
}
```

---

### 4.2 Pipeline Slot Validation: Vertex Buffers, Constant Buffers, SRVs, Samplers

**Target File**: `src/server/server_dispatcher.cpp` (Lines ~675-789)

**Before (`HandleIASetVertexBuffer`)**:
```cpp
bool ServerDispatcher::HandleIASetVertexBuffer(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(IASetVertexBufferPayload)) return false;
    const auto* p = reinterpret_cast<const IASetVertexBufferPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer = (p->res_id == 0) ? nullptr : m_resources.GetBuffer(p->res_id);
    ID3D11Buffer* p_buffer = buffer.Get();
    UINT stride = p->stride;
    UINT offset = p->offset;

    context->IASetVertexBuffers(p->slot, 1, &p_buffer, &stride, &offset);
    return true;
}
```

**After (`HandleIASetVertexBuffer`)**:
```cpp
bool ServerDispatcher::HandleIASetVertexBuffer(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(IASetVertexBufferPayload)) return false;
    const auto* p = reinterpret_cast<const IASetVertexBufferPayload*>(payload);

    if (p->slot >= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) {
        std::cerr << "[ServerDispatcher] Invalid vertex buffer slot: " << p->slot 
                  << " (limit: " << D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT << ")" << std::endl;
        return false;
    }

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer = (p->res_id == 0) ? nullptr : m_resources.GetBuffer(p->res_id);
    ID3D11Buffer* p_buffer = buffer.Get();
    UINT stride = p->stride;
    UINT offset = p->offset;

    context->IASetVertexBuffers(p->slot, 1, &p_buffer, &stride, &offset);
    return true;
}
```

**Before (`HandleIASetIndexBuffer`)**:
```cpp
bool ServerDispatcher::HandleIASetIndexBuffer(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(IASetIndexBufferPayload)) return false;
    const auto* p = reinterpret_cast<const IASetIndexBufferPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer = (p->res_id == 0) ? nullptr : m_resources.GetBuffer(p->res_id);
    context->IASetIndexBuffer(buffer.Get(), static_cast<DXGI_FORMAT>(p->format), p->offset);
    return true;
}
```

**After (`HandleIASetIndexBuffer`)**:
```cpp
bool ServerDispatcher::HandleIASetIndexBuffer(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(IASetIndexBufferPayload)) return false;
    const auto* p = reinterpret_cast<const IASetIndexBufferPayload*>(payload);

    if (p->format != DXGI_FORMAT_R16_UINT && 
        p->format != DXGI_FORMAT_R32_UINT && 
        p->format != DXGI_FORMAT_UNKNOWN) {
        std::cerr << "[ServerDispatcher] Invalid index buffer format: " << p->format << std::endl;
        return false;
    }

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer = (p->res_id == 0) ? nullptr : m_resources.GetBuffer(p->res_id);
    context->IASetIndexBuffer(buffer.Get(), static_cast<DXGI_FORMAT>(p->format), p->offset);
    return true;
}
```

**Before (`HandleVSSetCB`)**:
```cpp
bool ServerDispatcher::HandleVSSetCB(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(VSSetConstantBufferPayload)) return false;
    const auto* p = reinterpret_cast<const VSSetConstantBufferPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11Buffer> cb = (p->res_id == 0) ? nullptr : m_resources.GetBuffer(p->res_id);
    ID3D11Buffer* p_cb = cb.Get();
    context->VSSetConstantBuffers(p->slot, 1, &p_cb);
    return true;
}
```

**After (`HandleVSSetCB`)**:
```cpp
bool ServerDispatcher::HandleVSSetCB(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(VSSetConstantBufferPayload)) return false;
    const auto* p = reinterpret_cast<const VSSetConstantBufferPayload*>(payload);

    if (p->slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) {
        std::cerr << "[ServerDispatcher] Invalid VS constant buffer slot: " << p->slot 
                  << " (limit: " << D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT << ")" << std::endl;
        return false;
    }

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11Buffer> cb = (p->res_id == 0) ? nullptr : m_resources.GetBuffer(p->res_id);
    ID3D11Buffer* p_cb = cb.Get();
    context->VSSetConstantBuffers(p->slot, 1, &p_cb);
    return true;
}
```

**Before (`HandlePSSetCB`)**:
```cpp
bool ServerDispatcher::HandlePSSetCB(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(PSSetConstantBufferPayload)) return false;
    const auto* p = reinterpret_cast<const PSSetConstantBufferPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11Buffer> cb = (p->res_id == 0) ? nullptr : m_resources.GetBuffer(p->res_id);
    ID3D11Buffer* p_cb = cb.Get();
    context->PSSetConstantBuffers(p->slot, 1, &p_cb);
    return true;
}
```

**After (`HandlePSSetCB`)**:
```cpp
bool ServerDispatcher::HandlePSSetCB(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(PSSetConstantBufferPayload)) return false;
    const auto* p = reinterpret_cast<const PSSetConstantBufferPayload*>(payload);

    if (p->slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) {
        std::cerr << "[ServerDispatcher] Invalid PS constant buffer slot: " << p->slot 
                  << " (limit: " << D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT << ")" << std::endl;
        return false;
    }

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11Buffer> cb = (p->res_id == 0) ? nullptr : m_resources.GetBuffer(p->res_id);
    ID3D11Buffer* p_cb = cb.Get();
    context->PSSetConstantBuffers(p->slot, 1, &p_cb);
    return true;
}
```

**Before (`HandlePSSetSrv`)**:
```cpp
bool ServerDispatcher::HandlePSSetSrv(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(PSSetSrvPayload)) return false;
    const auto* p = reinterpret_cast<const PSSetSrvPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv = (p->res_id == 0) ? nullptr : m_resources.GetSRV(p->res_id);
    ID3D11ShaderResourceView* p_srv = srv.Get();
    context->PSSetShaderResources(p->slot, 1, &p_srv);
    return true;
}
```

**After (`HandlePSSetSrv`)**:
```cpp
bool ServerDispatcher::HandlePSSetSrv(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(PSSetSrvPayload)) return false;
    const auto* p = reinterpret_cast<const PSSetSrvPayload*>(payload);

    if (p->slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        std::cerr << "[ServerDispatcher] Invalid PS SRV slot: " << p->slot 
                  << " (limit: " << D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT << ")" << std::endl;
        return false;
    }

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv = (p->res_id == 0) ? nullptr : m_resources.GetSRV(p->res_id);
    ID3D11ShaderResourceView* p_srv = srv.Get();
    context->PSSetShaderResources(p->slot, 1, &p_srv);
    return true;
}
```

**Before (`HandlePSSetSampler`)**:
```cpp
bool ServerDispatcher::HandlePSSetSampler(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(PSSetSamplerPayload)) return false;
    const auto* p = reinterpret_cast<const PSSetSamplerPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler = (p->res_id == 0) ? nullptr : m_resources.GetSamplerState(p->res_id);
    ID3D11SamplerState* p_sampler = sampler.Get();
    context->PSSetSamplers(p->slot, 1, &p_sampler);
    return true;
}
```

**After (`HandlePSSetSampler`)**:
```cpp
bool ServerDispatcher::HandlePSSetSampler(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(PSSetSamplerPayload)) return false;
    const auto* p = reinterpret_cast<const PSSetSamplerPayload*>(payload);

    if (p->slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        std::cerr << "[ServerDispatcher] Invalid PS sampler slot: " << p->slot 
                  << " (limit: " << D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT << ")" << std::endl;
        return false;
    }

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler = (p->res_id == 0) ? nullptr : m_resources.GetSamplerState(p->res_id);
    ID3D11SamplerState* p_sampler = sampler.Get();
    context->PSSetSamplers(p->slot, 1, &p_sampler);
    return true;
}
```

---

### 4.3 Viewport & Render Target Bounds Validation

**Target File**: `src/server/server_dispatcher.cpp` (Lines ~790-836)

**Before (`HandleRSSetViewport`)**:
```cpp
bool ServerDispatcher::HandleRSSetViewport(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(RSSetViewportPayload)) return false;
    const auto* p = reinterpret_cast<const RSSetViewportPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = p->top_left_x;
    vp.TopLeftY = p->top_left_y;
    vp.Width = p->width;
    vp.Height = p->height;
    vp.MinDepth = p->min_depth;
    vp.MaxDepth = p->max_depth;

    context->RSSetViewports(1, &vp);
    return true;
}
```

**After (`HandleRSSetViewport`)**:
```cpp
bool ServerDispatcher::HandleRSSetViewport(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(RSSetViewportPayload)) return false;
    const auto* p = reinterpret_cast<const RSSetViewportPayload*>(payload);

    // Validate floating point sanity
    if (!std::isfinite(p->width) || !std::isfinite(p->height) ||
        !std::isfinite(p->top_left_x) || !std::isfinite(p->top_left_y) ||
        !std::isfinite(p->min_depth) || !std::isfinite(p->max_depth)) {
        std::cerr << "[ServerDispatcher] Viewport contains non-finite float values" << std::endl;
        return false;
    }

    if (p->width < 0.0f || p->height < 0.0f || p->width > 32767.0f || p->height > 32767.0f) {
        std::cerr << "[ServerDispatcher] Viewport dimensions out of bounds: " << p->width << "x" << p->height << std::endl;
        return false;
    }

    if (p->min_depth < 0.0f || p->max_depth > 1.0f || p->min_depth > p->max_depth) {
        std::cerr << "[ServerDispatcher] Viewport depth range invalid: [" << p->min_depth << ".." << p->max_depth << "]" << std::endl;
        return false;
    }

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = p->top_left_x;
    vp.TopLeftY = p->top_left_y;
    vp.Width = p->width;
    vp.Height = p->height;
    vp.MinDepth = p->min_depth;
    vp.MaxDepth = p->max_depth;

    context->RSSetViewports(1, &vp);
    return true;
}
```

**Before (`HandleOMSetRenderTargets`)**:
```cpp
bool ServerDispatcher::HandleOMSetRenderTargets(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(OMSetRenderTargetsPayload)) return false;
    const auto* p = reinterpret_cast<const OMSetRenderTargetsPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    if (p->rtv_id != 0) {
        rtv = m_resources.GetRTV(p->rtv_id);
    }
    // Fallback to host default offscreen render target if not explicitly in map
    if (!rtv) {
        rtv = m_gpuDevice.GetRenderTargetView();
    }

    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
    if (p->dsv_id != 0) {
        dsv = m_resources.GetDSV(p->dsv_id);
    }
    if (!dsv) {
        dsv = m_gpuDevice.GetDepthStencilView();
    }

    ID3D11RenderTargetView* p_rtv = rtv.Get();
    context->OMSetRenderTargets(p_rtv ? 1 : 0, p_rtv ? &p_rtv : nullptr, dsv.Get());
    return true;
}
```

**After (`HandleOMSetRenderTargets`)**:
```cpp
bool ServerDispatcher::HandleOMSetRenderTargets(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(OMSetRenderTargetsPayload)) return false;
    const auto* p = reinterpret_cast<const OMSetRenderTargetsPayload*>(payload);

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    if (p->rtv_id != 0) {
        rtv = m_resources.GetRTV(p->rtv_id);
    }
    if (!rtv) {
        rtv = m_gpuDevice.GetRenderTargetView();
    }

    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
    if (p->dsv_id != 0) {
        dsv = m_resources.GetDSV(p->dsv_id);
    }
    if (!dsv) {
        dsv = m_gpuDevice.GetDepthStencilView();
    }

    ID3D11RenderTargetView* p_rtv = rtv.Get();
    UINT num_rtvs = p_rtv ? 1 : 0;
    if (num_rtvs > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT) {
        std::cerr << "[ServerDispatcher] Render target count exceeds limit: " << num_rtvs << std::endl;
        return false;
    }

    context->OMSetRenderTargets(num_rtvs, p_rtv ? &p_rtv : nullptr, dsv.Get());
    return true;
}
```

---

### 4.4 Bounded Input Layout Semantic Name & Element Validation

**Target File**: `src/server/server_dispatcher.cpp` (Lines ~569-613)

**Before**:
```cpp
bool ServerDispatcher::HandleCreateInputLayout(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateInputLayoutPayload)) {
        std::cerr << "[ServerDispatcher] CreateInputLayout payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const CreateInputLayoutPayload*>(payload);
    size_t elements_size = static_cast<size_t>(p->num_elements) * sizeof(RgpuInputElementDesc);
    size_t expected_total = sizeof(CreateInputLayoutPayload) + elements_size + p->bytecode_length;

    if (size < expected_total) {
        std::cerr << "[ServerDispatcher] CreateInputLayout payload truncated" << std::endl;
        return false;
    }

    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    const auto* src_elements = reinterpret_cast<const RgpuInputElementDesc*>(payload + sizeof(CreateInputLayoutPayload));
    std::vector<D3D11_INPUT_ELEMENT_DESC> d3d_elements(p->num_elements);
    std::vector<std::string> semantic_names(p->num_elements);

    for (uint32_t i = 0; i < p->num_elements; ++i) {
        semantic_names[i] = src_elements[i].semantic_name;
        d3d_elements[i].SemanticName = semantic_names[i].c_str();
        d3d_elements[i].SemanticIndex = src_elements[i].semantic_index;
        d3d_elements[i].Format = static_cast<DXGI_FORMAT>(src_elements[i].format);
        d3d_elements[i].InputSlot = src_elements[i].input_slot;
        d3d_elements[i].AlignedByteOffset = src_elements[i].aligned_byte_offset;
        d3d_elements[i].InputSlotClass = static_cast<D3D11_INPUT_CLASSIFICATION>(src_elements[i].input_slot_class);
        d3d_elements[i].InstanceDataStepRate = src_elements[i].instance_data_step;
    }
...
```

**After**:
```cpp
bool ServerDispatcher::HandleCreateInputLayout(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateInputLayoutPayload)) {
        std::cerr << "[ServerDispatcher] CreateInputLayout payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const CreateInputLayoutPayload*>(payload);
    if (p->res_id == 0) {
        std::cerr << "[ServerDispatcher] CreateInputLayout: invalid res_id 0" << std::endl;
        return false;
    }

    if (p->num_elements == 0 || p->num_elements > D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT) { // 32
        std::cerr << "[ServerDispatcher] CreateInputLayout: invalid num_elements " << p->num_elements << std::endl;
        return false;
    }

    if (p->bytecode_length == 0 || p->bytecode_length > 67108864) {
        std::cerr << "[ServerDispatcher] CreateInputLayout: invalid bytecode length " << p->bytecode_length << std::endl;
        return false;
    }

    size_t elements_size = static_cast<size_t>(p->num_elements) * sizeof(RgpuInputElementDesc);
    if (size < sizeof(CreateInputLayoutPayload) + elements_size) {
        std::cerr << "[ServerDispatcher] CreateInputLayout elements truncated" << std::endl;
        return false;
    }
    if (size - sizeof(CreateInputLayoutPayload) - elements_size < p->bytecode_length) {
        std::cerr << "[ServerDispatcher] CreateInputLayout bytecode truncated" << std::endl;
        return false;
    }

    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    const auto* src_elements = reinterpret_cast<const RgpuInputElementDesc*>(payload + sizeof(CreateInputLayoutPayload));
    std::vector<D3D11_INPUT_ELEMENT_DESC> d3d_elements(p->num_elements);
    std::vector<std::string> semantic_names(p->num_elements);

    for (uint32_t i = 0; i < p->num_elements; ++i) {
        // Safe bounded string copy: prevents buffer over-read on non-null-terminated input
        size_t name_len = strnlen(src_elements[i].semantic_name, sizeof(src_elements[i].semantic_name));
        semantic_names[i].assign(src_elements[i].semantic_name, name_len);

        if (src_elements[i].input_slot >= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) {
            std::cerr << "[ServerDispatcher] CreateInputLayout element " << i 
                      << " invalid input_slot: " << src_elements[i].input_slot << std::endl;
            return false;
        }

        if (src_elements[i].input_slot_class > 1) {
            std::cerr << "[ServerDispatcher] CreateInputLayout element " << i 
                      << " invalid input_slot_class: " << src_elements[i].input_slot_class << std::endl;
            return false;
        }

        d3d_elements[i].SemanticName = semantic_names[i].c_str();
        d3d_elements[i].SemanticIndex = src_elements[i].semantic_index;
        d3d_elements[i].Format = static_cast<DXGI_FORMAT>(src_elements[i].format);
        d3d_elements[i].InputSlot = src_elements[i].input_slot;
        d3d_elements[i].AlignedByteOffset = src_elements[i].aligned_byte_offset;
        d3d_elements[i].InputSlotClass = static_cast<D3D11_INPUT_CLASSIFICATION>(src_elements[i].input_slot_class);
        d3d_elements[i].InstanceDataStepRate = src_elements[i].instance_data_step;
    }
...
```

---

## 5. Adversarial Stress Suite Defense Matrix

Mapping the remediations directly to the 20 test scenarios defined in `tests/test_server_stress.cpp`:

| Test # | Stress Scenario | Vulnerability in Base Code | Validation Defense Implemented | Expected Post-Fix Outcome |
|:---:|---|---|---|:---:|
| **1** | Partial packet disconnect | None (already robust) | `ProcessPacketStream` handles socket read error cleanly | **PASS** |
| **2** | Hard TCP RST mid-stream (`SO_LINGER {1,0}`) | None (already robust) | `NetworkSocket::RecvExact` catches `WSAECONNRESET` cleanly | **PASS** |
| **3** | Broken pipe during `OP_PRESENT` readback | None (already robust) | `SendExact` detects socket failure without throw | **PASS** |
| **4** | 20 adversarial drops + 1 legitimate client | None (already robust) | Session state reset cleanly between clients | **PASS** |
| **5** | 15 bogus opcodes (`0x0000`, `0xFFFF`, etc.) | None (already robust) | `DispatchCommand` switch `default:` safely returns `false` | **PASS** |
| **6** | Truncated 0B/1B/4B payloads | `0x0033` crashed driver on 4B payload (`0x12345678`) | `IsValidD3D11PrimitiveTopology` catches `0x12345678`, returns `false` | **PASS (0 crashes)** |
| **7** | Truncated variable payloads (shaders, layouts) | Layout element count over-read risk | Compound payload size arithmetic with underflow guards | **PASS** |
| **8** | Pseudorandom bitstream fuzzing (150 packets) | 8 driver crashes from fuzz packets hitting unvalidated slots/topos | All slots and topology enums bounds-checked; zero invalid values reach driver | **PASS (0 crashes)** |
| **9** | Parameter range stress (bad slots, bad topos) | 11 driver access violations (`0xC0000005`) | Explicit slot < limit checks on all 5 binding opcodes | **PASS (0 crashes)** |
| **10**| Non-existent resource ID lookup (`0xDEADBEEF`) | None (already robust) | `ServerResourceManager::Get<T>()` returns `nullptr`; unbinds safely | **PASS** |
| **11**| Resource ID 0 (Null/Unbind/Default) lookups | None (already robust) | Explicit `res_id == 0` guards in all setters | **PASS** |
| **12**| Cross-type COM QueryInterface confusion | None (already robust) | `ComPtr::As` validates interface type via COM QueryInterface | **PASS** |
| **13**| Use-after-free and double-destroy resistance | None (already robust) | `Destroy` erases entry; subsequent access returns `nullptr` | **PASS** |
| **14**| 1,000 buffer churn (x2) | None (already robust) | Clean COM refcount release, zero leak | **PASS** |
| **15**| 1,000 texture churn | None (already robust) | Clean COM refcount release, zero leak | **PASS** |
| **16**| Session reset with 1,000 active allocations | None (already robust) | `m_resources.Clear()` and `ClearState()` release all COM refs | **PASS** |
| **17**| Interleaved 500 rapid create/use/destroy cycles | None (already robust) | Process private memory stabilizes | **PASS** |
| **18**| `HostGpuDevice` invalid dimensions (0x0, 65536) | None (already robust) | Dimension check in `CreateOffscreenTargets` | **PASS** |
| **19**| Readback without targets or post-reset | None (already robust) | Guarded against null context/staging | **PASS** |
| **20**| Rapid target resizing churn (64x64 up to 1080p) | None (already robust) | Direct3D offscreen targets reallocated cleanly | **PASS** |

---

## 6. Verification and Acceptance Criteria for Worker M2

Worker M2 must implement these exact boundary validations and verify that:
1. Compilation cleanly succeeds with zero warnings:
   ```powershell
   clang++ -std=c++17 -Wall -Wextra -Wpedantic -O2 -Isrc/common -Isrc/server src/common/transport.cpp src/server/gpu_device.cpp src/server/server_dispatcher.cpp tests/test_server.cpp -o build/test_server.exe -ld3d11 -ldxgi -lws2_32 -ld3dcompiler
   ```
2. The adversarial stress test suite passes 100% of assertions with zero hardware/driver crashes:
   ```powershell
   clang++ -std=c++17 -fms-extensions -O2 -Isrc/common -Isrc/server src/common/transport.cpp src/server/gpu_device.cpp src/server/server_dispatcher.cpp tests/test_server_stress.cpp -o build/test_server_stress.exe -ld3d11 -ldxgi -lws2_32 -ld3dcompiler -lpsapi
   .\build\test_server_stress.exe
   ```
   **Acceptance Standard**: 181 / 181 assertions passed, 0 crashes, verdict: `APPROVE`.
