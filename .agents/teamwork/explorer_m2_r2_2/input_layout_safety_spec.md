# Specification: Input Layout & Resource Creation Memory Safety Spec

**Component**: `ServerDispatcher` (`src/server/server_dispatcher.cpp`, `src/server/server_dispatcher.h`)  
**Target Milestone**: Milestone 2 Remediation (Remote GPU Execution Server)  
**Author**: Explorer M2 Remediation 2 (`explorer_m2_r2_2`)  
**Date**: 2026-10-26  
**Status**: APPROVED SPECIFICATION FOR WORKER IMPLEMENTATION  

---

## 1. Executive Summary

Milestone 2 adversarial stress testing conducted by `challenger_m2_1` exposed vulnerabilities in the command dispatcher when handling network payloads. In addition to pipeline slot and topology crashes, `CreateInputLayout` in `src/server/server_dispatcher.cpp` was identified as having a critical buffer over-read defect (`CWE-125`) during semantic name parsing.

This specification details the root causes and formulates the exact, production-grade C++ remediation patch for:
1. **`HandleCreateInputLayout`**: Enforcing strict `strnlen`-bounded semantic string extraction, guaranteed null-termination, element count bounding against Direct3D 11 architecture limits (`D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT = 32`), and per-element descriptor validation (input slot `< 32`, format enums, input slot class, instance data step rate, and bytecode length).
2. **Resource Creation Routines (`HandleCreateBuffer`, `HandleCreateTexture2D`, `HandleCreateSrv`, `HandleCreateRtv`, `HandleCreateDsv`, `HandleCreateVertexShader`, `HandleCreatePixelShader`, `HandleCreateSampler`, `HandleIASetIndexBuffer`)**:
   - Universal `res_id == 0` validation (rejecting reserved/null resource registration).
   - Buffer byte width and initial data size validation (preventing buffer over-reads where `initial_data_size < byte_width`).
   - Constant buffer 16-byte alignment and size limits (`<= 65536` bytes).
   - Texture 2D dimension constraints (`16384` max dimension, `2048` max array size, `15` max mips).
   - Format enum verification via `IsValidDxgiFormat` (rejecting out-of-range enums before hardware driver invocation).
   - View dimension bounding on SRV (`<= 11`), RTV (`<= 8`), and DSV (`<= 6`).
   - Sampler state parameter validation (`AddressU/V/W` in `[1, 5]`, `ComparisonFunc` in `[1, 8]`, `MaxAnisotropy <= 16`).
   - Index buffer format restriction (`DXGI_FORMAT_R16_UINT`, `DXGI_FORMAT_R32_UINT`, `DXGI_FORMAT_UNKNOWN`).

---

## 2. Vulnerability & Risk Analysis

### 2.1 CWE-125: Buffer Over-Read in `HandleCreateInputLayout` Semantic Name
- **File**: `src/server/server_dispatcher.cpp:592`
- **Verbatim Code**:
  ```cpp
  semantic_names[i] = src_elements[i].semantic_name;
  ```
- **Structure Definition (`src/common/protocol.h:192-200`)**:
  ```cpp
  struct RgpuInputElementDesc {
      char     semantic_name[32];   // Fixed 32-byte array
      uint32_t semantic_index;
      uint32_t format;
      uint32_t input_slot;
      uint32_t aligned_byte_offset;
      uint32_t input_slot_class;
      uint32_t instance_data_step;
  };
  ```
- **Vulnerability Mechanism**: `std::string::operator=(const char*)` invokes `strlen(src_elements[i].semantic_name)`. If a remote client or adversary sends 32 non-null characters (e.g. 32 `'A'` bytes, as in `tests/test_server_stress.cpp:601`), `strlen` reads past the 32-byte boundary into adjacent struct members (`semantic_index`, `format`, etc.). If non-null data continues up to a memory page boundary, this triggers an immediate `0xC0000005` Access Violation. Even if an adjacent null byte is found, heap/stack data is leaked into the D3D11 runtime.
- **Remediation**: Use `strnlen` bounded to `sizeof(src_elements[i].semantic_name)` (32 bytes), followed by `std::string::assign(src_elements[i].semantic_name, name_len)`. This guarantees memory safety and creates a strictly null-terminated C-string for `D3D11_INPUT_ELEMENT_DESC::SemanticName`.

### 2.2 Unbounded Element Count (`num_elements`)
- **File**: `src/server/server_dispatcher.cpp:576, 588`
- **Vulnerability Mechanism**: In D3D11, the input assembler supports a maximum of 32 input elements (`D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT = 32`). If `p->num_elements` is not validated:
  1. Excessive values (e.g. `p->num_elements = 0x40000000`) can cause integer overflow in `elements_size` calculation (`p->num_elements * sizeof(RgpuInputElementDesc)`).
  2. Unbounded heap allocations in `std::vector<D3D11_INPUT_ELEMENT_DESC>` can trigger `std::bad_alloc` or OOM crashes.
  3. Direct3D `CreateInputLayout` driver calls with `> 32` elements can trigger undefined driver behavior or access violations.
- **Remediation**: Enforce `if (p->num_elements > D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT) return false;`.

### 2.3 Layout Element Descriptor Validation
- In `HandleCreateInputLayout`, incoming layout elements must be validated prior to calling `device->CreateInputLayout`:
  - `src_elements[i].input_slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT` (32).
  - `src_elements[i].input_slot_class <= D3D11_INPUT_PER_INSTANCE_DATA` (1).
  - `src_elements[i].format != DXGI_FORMAT_UNKNOWN && IsValidDxgiFormat(src_elements[i].format)`.
  - Empty semantic name check (`name_len == 0` is invalid in D3D11).
  - `p->bytecode_length > 0`: Direct3D requires a valid vertex shader signature to create an input layout.

### 2.4 Buffer Creation Initial Data Over-Read
- **File**: `src/server/server_dispatcher.cpp:292-302`
- **Vulnerability Mechanism**: When `p->has_initial_data == 1`, Direct3D `device->CreateBuffer(&desc, &init_data, &buffer)` assumes `init_data.pSysMem` contains at least `desc.ByteWidth` bytes. If an attacker sends `has_initial_data = 1` with `initial_data_size = 4` but `byte_width = 1048576`, Direct3D reads 1 MB from `pSysMem`, reading out of bounds of the packet payload and leaking or faulting memory.
- **Remediation**: Validate that `p->initial_data_size >= p->byte_width` whenever `p->has_initial_data` is non-zero, and verify `p->initial_data_size <= size - sizeof(CreateBufferPayload)`.

### 2.5 Out-of-Range Format and Dimension Enums in Resource Creation
- Direct3D 11 hardware drivers maintain internal dispatch tables indexed by enums such as `DXGI_FORMAT`, `D3D11_SRV_DIMENSION`, `D3D11_RTV_DIMENSION`, `D3D11_DSV_DIMENSION`, and `D3D11_FILTER`. Supplying out-of-range integer values over the network bypasses compile-time type safety and causes `0xC0000005` in driver binaries.
- All creation handlers must validate enums against architectural maximums before invoking COM methods.

---

## 3. Precise Patch Specification

### 3.1 Format Validation Helper
Worker M2 will add the following helper function at the top of `src/server/server_dispatcher.cpp` (in anonymous namespace or file-static scope):

```cpp
namespace {

/**
 * @brief Validates that a uint32_t value is within the recognized DXGI_FORMAT range.
 * Standard DXGI formats range from 1 (DXGI_FORMAT_R32G32B32A32_TYPELESS) to 
 * 115 (DXGI_FORMAT_B4G4R4A4_UNORM), with extended video/compressed formats extending to 191.
 */
inline bool IsValidDxgiFormat(uint32_t format) {
    return (format >= 1 && format <= 191);
}

} // anonymous namespace
```

---

### 3.2 `HandleCreateInputLayout` Replacement Patch

**Target Lines**: `src/server/server_dispatcher.cpp:569-613`

```cpp
bool ServerDispatcher::HandleCreateInputLayout(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateInputLayoutPayload)) {
        std::cerr << "[ServerDispatcher] CreateInputLayout payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const CreateInputLayoutPayload*>(payload);
    if (p->res_id == 0) {
        std::cerr << "[ServerDispatcher] CreateInputLayout: invalid res_id=0" << std::endl;
        return false;
    }

    // Direct3D 11 architecture enforces a maximum of 32 input elements
    if (p->num_elements > D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT) {
        std::cerr << "[ServerDispatcher] CreateInputLayout: num_elements (" << p->num_elements 
                  << ") exceeds D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT (" 
                  << D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT << ")" << std::endl;
        return false;
    }

    size_t elements_size = static_cast<size_t>(p->num_elements) * sizeof(RgpuInputElementDesc);
    size_t expected_total = sizeof(CreateInputLayoutPayload) + elements_size + static_cast<size_t>(p->bytecode_length);

    if (size < expected_total) {
        std::cerr << "[ServerDispatcher] CreateInputLayout payload truncated (expected at least " 
                  << expected_total << ", got " << size << ")" << std::endl;
        return false;
    }

    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    const auto* src_elements = reinterpret_cast<const RgpuInputElementDesc*>(payload + sizeof(CreateInputLayoutPayload));
    std::vector<D3D11_INPUT_ELEMENT_DESC> d3d_elements(p->num_elements);
    std::vector<std::string> semantic_names(p->num_elements);

    for (uint32_t i = 0; i < p->num_elements; ++i) {
        // Safe bounded string copy: prevent buffer over-read if semantic name is not null-terminated
        size_t name_len = strnlen(src_elements[i].semantic_name, sizeof(src_elements[i].semantic_name));
        if (name_len == 0) {
            std::cerr << "[ServerDispatcher] CreateInputLayout: empty semantic name at element " << i << std::endl;
            return false;
        }
        semantic_names[i].assign(src_elements[i].semantic_name, name_len);
        d3d_elements[i].SemanticName = semantic_names[i].c_str();

        d3d_elements[i].SemanticIndex = src_elements[i].semantic_index;

        // Format enum validation
        if (src_elements[i].format == DXGI_FORMAT_UNKNOWN || !IsValidDxgiFormat(src_elements[i].format)) {
            std::cerr << "[ServerDispatcher] CreateInputLayout: invalid format " << src_elements[i].format 
                      << " at element " << i << std::endl;
            return false;
        }
        d3d_elements[i].Format = static_cast<DXGI_FORMAT>(src_elements[i].format);

        // Vertex buffer input slot validation (< 32)
        if (src_elements[i].input_slot >= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) {
            std::cerr << "[ServerDispatcher] CreateInputLayout: input_slot " << src_elements[i].input_slot 
                      << " exceeds D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT at element " << i << std::endl;
            return false;
        }
        d3d_elements[i].InputSlot = src_elements[i].input_slot;

        d3d_elements[i].AlignedByteOffset = src_elements[i].aligned_byte_offset;

        // Input classification validation (0: PER_VERTEX_DATA, 1: PER_INSTANCE_DATA)
        if (src_elements[i].input_slot_class > D3D11_INPUT_PER_INSTANCE_DATA) {
            std::cerr << "[ServerDispatcher] CreateInputLayout: invalid input_slot_class " 
                      << src_elements[i].input_slot_class << " at element " << i << std::endl;
            return false;
        }
        d3d_elements[i].InputSlotClass = static_cast<D3D11_INPUT_CLASSIFICATION>(src_elements[i].input_slot_class);
        d3d_elements[i].InstanceDataStepRate = src_elements[i].instance_data_step;
    }

    // Direct3D 11 requires non-empty shader bytecode with input signature to create layout
    if (p->bytecode_length == 0) {
        std::cerr << "[ServerDispatcher] CreateInputLayout: bytecode_length is 0" << std::endl;
        return false;
    }

    const void* bytecode = payload + sizeof(CreateInputLayoutPayload) + elements_size;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;
    const D3D11_INPUT_ELEMENT_DESC* p_descs = (p->num_elements > 0) ? d3d_elements.data() : nullptr;
    HRESULT hr = device->CreateInputLayout(p_descs, p->num_elements, bytecode, p->bytecode_length, &layout);
    if (FAILED(hr)) {
        std::cerr << "[ServerDispatcher] CreateInputLayout failed: 0x" << std::hex << hr << std::dec
                  << " (res_id=" << p->res_id << ")" << std::endl;
        return false;
    }

    m_resources.Register(p->res_id, layout);
    return true;
}
```

---

### 3.3 `HandleCreateBuffer` Replacement Patch

**Target Lines**: `src/server/server_dispatcher.cpp:271-312`

```cpp
bool ServerDispatcher::HandleCreateBuffer(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateBufferPayload)) {
        std::cerr << "[ServerDispatcher] CreateBuffer payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const CreateBufferPayload*>(payload);
    if (p->res_id == 0) {
        std::cerr << "[ServerDispatcher] CreateBuffer: invalid res_id=0" << std::endl;
        return false;
    }

    // Direct3D 11 buffer requirements
    constexpr uint32_t MAX_BUFFER_BYTES = 2048u * 1024u * 1024u; // 2 GB D3D11 limit
    if (p->byte_width == 0 || p->byte_width > MAX_BUFFER_BYTES) {
        std::cerr << "[ServerDispatcher] CreateBuffer: invalid byte_width: " << p->byte_width << std::endl;
        return false;
    }

    // Validate Usage enum (0: DEFAULT, 1: IMMUTABLE, 2: DYNAMIC, 3: STAGING)
    if (p->usage > D3D11_USAGE_STAGING) {
        std::cerr << "[ServerDispatcher] CreateBuffer: invalid usage: " << p->usage << std::endl;
        return false;
    }

    // Constant buffer specific alignment and size rules
    if (p->bind_flags & D3D11_BIND_CONSTANT_BUFFER) {
        if ((p->byte_width % 16) != 0 || p->byte_width > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16) {
            std::cerr << "[ServerDispatcher] CreateBuffer: constant buffer byte_width must be 16-byte aligned and <= 65536, got "
                      << p->byte_width << std::endl;
            return false;
        }
    }

    // Initial data bounds validation
    if (p->has_initial_data && p->initial_data_size > 0) {
        if (p->initial_data_size < p->byte_width) {
            std::cerr << "[ServerDispatcher] CreateBuffer: initial_data_size (" << p->initial_data_size 
                      << ") < byte_width (" << p->byte_width << ")" << std::endl;
            return false;
        }
        if (p->initial_data_size > size - sizeof(CreateBufferPayload)) {
            std::cerr << "[ServerDispatcher] CreateBuffer payload missing initial data" << std::endl;
            return false;
        }
    } else if (p->usage == D3D11_USAGE_IMMUTABLE) {
        std::cerr << "[ServerDispatcher] CreateBuffer: IMMUTABLE buffer requires initial data" << std::endl;
        return false;
    }

    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = p->byte_width;
    desc.Usage = static_cast<D3D11_USAGE>(p->usage);
    desc.BindFlags = p->bind_flags;
    desc.CPUAccessFlags = p->cpu_access_flags;
    desc.MiscFlags = p->misc_flags;
    desc.StructureByteStride = p->structure_byte_stride;

    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    HRESULT hr = S_OK;

    if (p->has_initial_data && p->initial_data_size > 0) {
        D3D11_SUBRESOURCE_DATA init_data = {};
        init_data.pSysMem = payload + sizeof(CreateBufferPayload);
        hr = device->CreateBuffer(&desc, &init_data, &buffer);
    } else {
        hr = device->CreateBuffer(&desc, nullptr, &buffer);
    }

    if (FAILED(hr)) {
        std::cerr << "[ServerDispatcher] CreateBuffer failed: 0x" << std::hex << hr << std::dec
                  << " (res_id=" << p->res_id << ")" << std::endl;
        return false;
    }

    m_resources.Register(p->res_id, buffer);
    return true;
}
```

---

### 3.4 `HandleCreateTexture2D` Replacement Patch

**Target Lines**: `src/server/server_dispatcher.cpp:314-362`

```cpp
bool ServerDispatcher::HandleCreateTexture2D(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateTexture2DPayload)) {
        std::cerr << "[ServerDispatcher] CreateTexture2D payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const CreateTexture2DPayload*>(payload);
    if (p->res_id == 0) {
        std::cerr << "[ServerDispatcher] CreateTexture2D: invalid res_id=0" << std::endl;
        return false;
    }

    // Direct3D 11 Texture2D architectural limits
    if (p->width == 0 || p->width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        p->height == 0 || p->height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        std::cerr << "[ServerDispatcher] CreateTexture2D: invalid dimensions " << p->width << "x" << p->height << std::endl;
        return false;
    }

    if (p->array_size == 0 || p->array_size > D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION) {
        std::cerr << "[ServerDispatcher] CreateTexture2D: invalid array_size: " << p->array_size << std::endl;
        return false;
    }

    if (p->mips > D3D11_REQ_MIP_LEVELS) {
        std::cerr << "[ServerDispatcher] CreateTexture2D: invalid mips: " << p->mips << std::endl;
        return false;
    }

    if (p->format == DXGI_FORMAT_UNKNOWN || !IsValidDxgiFormat(p->format)) {
        std::cerr << "[ServerDispatcher] CreateTexture2D: invalid format: " << p->format << std::endl;
        return false;
    }

    if (p->usage > D3D11_USAGE_STAGING) {
        std::cerr << "[ServerDispatcher] CreateTexture2D: invalid usage: " << p->usage << std::endl;
        return false;
    }

    if (p->has_initial_data && p->initial_data_size > 0) {
        if (p->initial_data_size > size - sizeof(CreateTexture2DPayload)) {
            std::cerr << "[ServerDispatcher] CreateTexture2D payload missing initial data" << std::endl;
            return false;
        }
    } else if (p->usage == D3D11_USAGE_IMMUTABLE) {
        std::cerr << "[ServerDispatcher] CreateTexture2D: IMMUTABLE texture requires initial data" << std::endl;
        return false;
    }

    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = p->width;
    desc.Height = p->height;
    desc.MipLevels = p->mips;
    desc.ArraySize = p->array_size;
    desc.Format = static_cast<DXGI_FORMAT>(p->format);
    desc.SampleDesc.Count = (p->sample_count > 0) ? p->sample_count : 1;
    desc.SampleDesc.Quality = p->sample_quality;
    desc.Usage = static_cast<D3D11_USAGE>(p->usage);
    desc.BindFlags = p->bind_flags;
    desc.CPUAccessFlags = p->cpu_access_flags;
    desc.MiscFlags = p->misc_flags;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = S_OK;

    if (p->has_initial_data && p->initial_data_size > 0) {
        D3D11_SUBRESOURCE_DATA init_data = {};
        init_data.pSysMem = payload + sizeof(CreateTexture2DPayload);
        init_data.SysMemPitch = p->sys_mem_pitch;
        init_data.SysMemSlicePitch = p->sys_mem_slice_pitch;
        hr = device->CreateTexture2D(&desc, &init_data, &texture);
    } else {
        hr = device->CreateTexture2D(&desc, nullptr, &texture);
    }

    if (FAILED(hr)) {
        std::cerr << "[ServerDispatcher] CreateTexture2D failed: 0x" << std::hex << hr << std::dec
                  << " (res_id=" << p->res_id << ")" << std::endl;
        return false;
    }

    m_resources.Register(p->res_id, texture);
    return true;
}
```

---

### 3.5 `HandleCreateSrv` Replacement Patch

**Target Lines**: `src/server/server_dispatcher.cpp:364-401`

```cpp
bool ServerDispatcher::HandleCreateSrv(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateSrvPayload)) {
        std::cerr << "[ServerDispatcher] CreateSrv payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const CreateSrvPayload*>(payload);
    if (p->res_id == 0 || p->resource_id == 0) {
        std::cerr << "[ServerDispatcher] CreateSrv: invalid res_id or resource_id (res_id=" 
                  << p->res_id << ", resource_id=" << p->resource_id << ")" << std::endl;
        return false;
    }

    bool use_desc = (p->format != 0 || p->view_dimension != 0);
    if (use_desc) {
        if (p->format != 0 && !IsValidDxgiFormat(p->format)) {
            std::cerr << "[ServerDispatcher] CreateSrv: invalid format: " << p->format << std::endl;
            return false;
        }
        if (p->view_dimension > D3D11_SRV_DIMENSION_BUFFEREX) {
            std::cerr << "[ServerDispatcher] CreateSrv: invalid view_dimension: " << p->view_dimension << std::endl;
            return false;
        }
    }

    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    Microsoft::WRL::ComPtr<ID3D11Resource> resource = m_resources.Get<ID3D11Resource>(p->resource_id);
    if (!resource) {
        std::cerr << "[ServerDispatcher] CreateSrv: underlying resource not found: " << p->resource_id << std::endl;
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
    if (use_desc) {
        desc.Format = static_cast<DXGI_FORMAT>(p->format);
        desc.ViewDimension = static_cast<D3D11_SRV_DIMENSION>(p->view_dimension);
        if (desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
            desc.Texture2D.MostDetailedMip = p->most_detailed_mip;
            desc.Texture2D.MipLevels = p->mip_levels;
        }
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = device->CreateShaderResourceView(resource.Get(), use_desc ? &desc : nullptr, &srv);
    if (FAILED(hr)) {
        std::cerr << "[ServerDispatcher] CreateShaderResourceView failed: 0x" << std::hex << hr << std::dec
                  << " (res_id=" << p->res_id << ")" << std::endl;
        return false;
    }

    m_resources.Register(p->res_id, srv);
    return true;
}
```

---

### 3.6 `HandleCreateRtv` Replacement Patch

**Target Lines**: `src/server/server_dispatcher.cpp:403-456`

```cpp
bool ServerDispatcher::HandleCreateRtv(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateRtvPayload)) {
        std::cerr << "[ServerDispatcher] CreateRtv payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const CreateRtvPayload*>(payload);
    if (p->res_id == 0) {
        std::cerr << "[ServerDispatcher] CreateRtv: invalid res_id=0" << std::endl;
        return false;
    }

    bool use_desc = (p->format != 0);
    if (use_desc) {
        if (!IsValidDxgiFormat(p->format)) {
            std::cerr << "[ServerDispatcher] CreateRtv: invalid format: " << p->format << std::endl;
            return false;
        }
        if (p->view_dimension > D3D11_RTV_DIMENSION_TEXTURE3D) {
            std::cerr << "[ServerDispatcher] CreateRtv: invalid view_dimension: " << p->view_dimension << std::endl;
            return false;
        }
    }

    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;

    // Special case: resource_id == 0 indicates binding to the host offscreen default backbuffer
    if (p->resource_id == 0) {
        rtv = m_gpuDevice.GetRenderTargetView();
        if (!rtv && m_gpuDevice.GetColorTexture()) {
            HRESULT hr = device->CreateRenderTargetView(m_gpuDevice.GetColorTexture(), nullptr, &rtv);
            if (FAILED(hr)) {
                std::cerr << "[ServerDispatcher] CreateRenderTargetView on default backbuffer failed: 0x"
                          << std::hex << hr << std::dec << std::endl;
                return false;
            }
        }
    } else {
        Microsoft::WRL::ComPtr<ID3D11Resource> resource = m_resources.Get<ID3D11Resource>(p->resource_id);
        if (!resource) {
            if (m_gpuDevice.GetColorTexture()) {
                resource = m_gpuDevice.GetColorTexture();
            } else {
                std::cerr << "[ServerDispatcher] CreateRtv: resource not found: " << p->resource_id << std::endl;
                return false;
            }
        }

        D3D11_RENDER_TARGET_VIEW_DESC desc = {};
        if (use_desc) {
            desc.Format = static_cast<DXGI_FORMAT>(p->format);
            desc.ViewDimension = static_cast<D3D11_RTV_DIMENSION>(p->view_dimension);
            desc.Texture2D.MipSlice = p->mip_slice;
        }

        HRESULT hr = device->CreateRenderTargetView(resource.Get(), use_desc ? &desc : nullptr, &rtv);
        if (FAILED(hr)) {
            std::cerr << "[ServerDispatcher] CreateRenderTargetView failed: 0x" << std::hex << hr << std::dec
                      << " (res_id=" << p->res_id << ")" << std::endl;
            return false;
        }
    }

    m_resources.Register(p->res_id, rtv);
    return true;
}
```

---

### 3.7 `HandleCreateDsv` Replacement Patch

**Target Lines**: `src/server/server_dispatcher.cpp:458-511`

```cpp
bool ServerDispatcher::HandleCreateDsv(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateDsvPayload)) {
        std::cerr << "[ServerDispatcher] CreateDsv payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const CreateDsvPayload*>(payload);
    if (p->res_id == 0) {
        std::cerr << "[ServerDispatcher] CreateDsv: invalid res_id=0" << std::endl;
        return false;
    }

    bool use_desc = (p->format != 0);
    if (use_desc) {
        if (!IsValidDxgiFormat(p->format)) {
            std::cerr << "[ServerDispatcher] CreateDsv: invalid format: " << p->format << std::endl;
            return false;
        }
        if (p->view_dimension > D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY) {
            std::cerr << "[ServerDispatcher] CreateDsv: invalid view_dimension: " << p->view_dimension << std::endl;
            return false;
        }
    }

    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;

    // Special case: resource_id == 0 indicates host offscreen depth-stencil buffer
    if (p->resource_id == 0) {
        dsv = m_gpuDevice.GetDepthStencilView();
        if (!dsv && m_gpuDevice.GetDepthTexture()) {
            HRESULT hr = device->CreateDepthStencilView(m_gpuDevice.GetDepthTexture(), nullptr, &dsv);
            if (FAILED(hr)) {
                std::cerr << "[ServerDispatcher] CreateDepthStencilView on default depth buffer failed: 0x"
                          << std::hex << hr << std::dec << std::endl;
                return false;
            }
        }
    } else {
        Microsoft::WRL::ComPtr<ID3D11Resource> resource = m_resources.Get<ID3D11Resource>(p->resource_id);
        if (!resource) {
            if (m_gpuDevice.GetDepthTexture()) {
                resource = m_gpuDevice.GetDepthTexture();
            } else {
                std::cerr << "[ServerDispatcher] CreateDsv: resource not found: " << p->resource_id << std::endl;
                return false;
            }
        }

        D3D11_DEPTH_STENCIL_VIEW_DESC desc = {};
        if (use_desc) {
            desc.Format = static_cast<DXGI_FORMAT>(p->format);
            desc.ViewDimension = static_cast<D3D11_DSV_DIMENSION>(p->view_dimension);
            desc.Flags = p->flags;
            desc.Texture2D.MipSlice = p->mip_slice;
        }

        HRESULT hr = device->CreateDepthStencilView(resource.Get(), use_desc ? &desc : nullptr, &dsv);
        if (FAILED(hr)) {
            std::cerr << "[ServerDispatcher] CreateDepthStencilView failed: 0x" << std::hex << hr << std::dec
                      << " (res_id=" << p->res_id << ")" << std::endl;
            return false;
        }
    }

    m_resources.Register(p->res_id, dsv);
    return true;
}
```

---

### 3.8 `HandleCreateVertexShader` & `HandleCreatePixelShader` Replacement Patch

**Target Lines**: `src/server/server_dispatcher.cpp:513-567`

```cpp
bool ServerDispatcher::HandleCreateVertexShader(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateShaderPayload)) {
        std::cerr << "[ServerDispatcher] CreateVertexShader payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const CreateShaderPayload*>(payload);
    if (p->res_id == 0) {
        std::cerr << "[ServerDispatcher] CreateVertexShader: invalid res_id=0" << std::endl;
        return false;
    }

    if (p->bytecode_length == 0 || p->bytecode_length > 64 * 1024 * 1024) {
        std::cerr << "[ServerDispatcher] CreateVertexShader: invalid bytecode_length: " << p->bytecode_length << std::endl;
        return false;
    }

    if (p->bytecode_length > size - sizeof(CreateShaderPayload)) {
        std::cerr << "[ServerDispatcher] CreateVertexShader bytecode truncated" << std::endl;
        return false;
    }

    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    const void* bytecode = payload + sizeof(CreateShaderPayload);
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
    HRESULT hr = device->CreateVertexShader(bytecode, p->bytecode_length, nullptr, &vs);
    if (FAILED(hr)) {
        std::cerr << "[ServerDispatcher] CreateVertexShader failed: 0x" << std::hex << hr << std::dec
                  << " (res_id=" << p->res_id << ")" << std::endl;
        return false;
    }

    m_resources.Register(p->res_id, vs);
    return true;
}

bool ServerDispatcher::HandleCreatePixelShader(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateShaderPayload)) {
        std::cerr << "[ServerDispatcher] CreatePixelShader payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const CreateShaderPayload*>(payload);
    if (p->res_id == 0) {
        std::cerr << "[ServerDispatcher] CreatePixelShader: invalid res_id=0" << std::endl;
        return false;
    }

    if (p->bytecode_length == 0 || p->bytecode_length > 64 * 1024 * 1024) {
        std::cerr << "[ServerDispatcher] CreatePixelShader: invalid bytecode_length: " << p->bytecode_length << std::endl;
        return false;
    }

    if (p->bytecode_length > size - sizeof(CreateShaderPayload)) {
        std::cerr << "[ServerDispatcher] CreatePixelShader bytecode truncated" << std::endl;
        return false;
    }

    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    const void* bytecode = payload + sizeof(CreateShaderPayload);
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
    HRESULT hr = device->CreatePixelShader(bytecode, p->bytecode_length, nullptr, &ps);
    if (FAILED(hr)) {
        std::cerr << "[ServerDispatcher] CreatePixelShader failed: 0x" << std::hex << hr << std::dec
                  << " (res_id=" << p->res_id << ")" << std::endl;
        return false;
    }

    m_resources.Register(p->res_id, ps);
    return true;
}
```

---

### 3.9 `HandleCreateSampler` Replacement Patch

**Target Lines**: `src/server/server_dispatcher.cpp:615-647`

```cpp
bool ServerDispatcher::HandleCreateSampler(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(CreateSamplerPayload)) {
        std::cerr << "[ServerDispatcher] CreateSampler payload too small: " << size << std::endl;
        return false;
    }

    const auto* p = reinterpret_cast<const CreateSamplerPayload*>(payload);
    if (p->res_id == 0) {
        std::cerr << "[ServerDispatcher] CreateSampler: invalid res_id=0" << std::endl;
        return false;
    }

    // Direct3D 11 texture address modes: 1 (WRAP) to 5 (MIRROR_ONCE)
    if (p->address_u < 1 || p->address_u > 5 ||
        p->address_v < 1 || p->address_v > 5 ||
        p->address_w < 1 || p->address_w > 5) {
        std::cerr << "[ServerDispatcher] CreateSampler: invalid address mode (u=" 
                  << p->address_u << ", v=" << p->address_v << ", w=" << p->address_w << ")" << std::endl;
        return false;
    }

    // Direct3D 11 comparison functions: 1 (NEVER) to 8 (ALWAYS)
    if (p->comparison_func < 1 || p->comparison_func > 8) {
        std::cerr << "[ServerDispatcher] CreateSampler: invalid comparison_func: " << p->comparison_func << std::endl;
        return false;
    }

    ID3D11Device* device = m_gpuDevice.GetDevice();
    if (!device) return false;

    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = static_cast<D3D11_FILTER>(p->filter);
    desc.AddressU = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(p->address_u);
    desc.AddressV = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(p->address_v);
    desc.AddressW = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(p->address_w);
    desc.MipLODBias = p->mip_lod_bias;
    desc.MaxAnisotropy = (std::min)(p->max_anisotropy, 16u);
    desc.ComparisonFunc = static_cast<D3D11_COMPARISON_FUNC>(p->comparison_func);
    std::memcpy(desc.BorderColor, p->border_color, sizeof(desc.BorderColor));
    desc.MinLOD = p->min_lod;
    desc.MaxLOD = p->max_lod;

    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
    HRESULT hr = device->CreateSamplerState(&desc, &sampler);
    if (FAILED(hr)) {
        std::cerr << "[ServerDispatcher] CreateSamplerState failed: 0x" << std::hex << hr << std::dec
                  << " (res_id=" << p->res_id << ")" << std::endl;
        return false;
    }

    m_resources.Register(p->res_id, sampler);
    return true;
}
```

---

### 3.10 `HandleIASetIndexBuffer` Replacement Patch

**Target Lines**: `src/server/server_dispatcher.cpp:691-701`

```cpp
bool ServerDispatcher::HandleIASetIndexBuffer(const PacketHeader& /*header*/, const uint8_t* payload, size_t size) {
    if (size < sizeof(IASetIndexBufferPayload)) return false;
    const auto* p = reinterpret_cast<const IASetIndexBufferPayload*>(payload);

    // Direct3D 11 restricts index buffer format to 16-bit uint, 32-bit uint, or 0 (unbind)
    if (p->format != DXGI_FORMAT_UNKNOWN && 
        p->format != DXGI_FORMAT_R16_UINT && 
        p->format != DXGI_FORMAT_R32_UINT) {
        std::cerr << "[ServerDispatcher] IASetIndexBuffer: invalid index format: " << p->format << std::endl;
        return false;
    }

    ID3D11DeviceContext* context = m_gpuDevice.GetContext();
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer = (p->res_id == 0) ? nullptr : m_resources.GetBuffer(p->res_id);
    context->IASetIndexBuffer(buffer.Get(), static_cast<DXGI_FORMAT>(p->format), p->offset);
    return true;
}
```

---

### 3.11 `HandleWriteBufferDiscard` Buffer Bounds Patch

**Target Lines**: `src/server/server_dispatcher.cpp:901-908`

```cpp
    if ((desc.Usage == D3D11_USAGE_DYNAMIC) && (desc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE)) {
        if (static_cast<uint64_t>(p->byte_offset) + static_cast<uint64_t>(p->byte_width) > desc.ByteWidth) {
            std::cerr << "[ServerDispatcher] WriteBufferDiscard: write range exceeds buffer byte width ("
                      << (p->byte_offset + p->byte_width) << " > " << desc.ByteWidth << ")" << std::endl;
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = context->Map(buf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            std::memcpy(static_cast<uint8_t*>(mapped.pData) + p->byte_offset, p_data, p->byte_width);
            context->Unmap(buf.Get(), 0);
            return true;
        }
    }
```

---

## 4. Invariant & Safety Guarantees

| Invariant | Mechanism | Defect Prevented |
|-----------|-----------|------------------|
| **String Null-Termination** | `strnlen` bounds copy to at most 32 bytes; `assign` allocates and null-terminates string. | Buffer over-read into adjacent memory / unmapped pages (CWE-125). |
| **Element Count Upper Bound** | `num_elements <= 32` (`D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT`). | Integer multiplication overflow, excessive vector allocation, D3D11 driver table overflow. |
| **Input Slot Bounds** | `input_slot < 32` (`D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT`). | Driver IA stage slot array out-of-bounds indexing (`0xC0000005`). |
| **Format Enum Safety** | `IsValidDxgiFormat(format)` checks `[1, 191]`. | Driver pixel/vertex unpack table jump out-of-bounds. |
| **Buffer Initial Data Range** | `initial_data_size >= byte_width` check before reading payload. | Buffer over-read of network packet memory into GPU buffer. |
| **Constant Buffer Alignment** | `byte_width % 16 == 0` and `byte_width <= 65536`. | D3D11 constant buffer allocation failure / driver fault. |
| **View Dimension Range** | SRV $\le 11$, RTV $\le 8$, DSV $\le 6$. | Driver view dispatch table out-of-bounds index. |
| **Sampler Parameter Range** | AddressU/V/W $\in [1, 5]$, ComparisonFunc $\in [1, 8]$, MaxAnisotropy $\le 16$. | Driver sampler state register array fault. |
| **Index Buffer Format** | Restricted to `R16_UINT` (57), `R32_UINT` (42), or `UNKNOWN` (0). | Driver IA index fetcher crash. |
| **Resource ID Zero Safety** | Explicit rejection of `res_id == 0` across all create handlers. | Inconsistent state and memory pollution in resource manager. |

---

## 5. Implementation & Verification Matrix

### Compilation Command:
```powershell
clang++ -std=c++17 -fms-extensions -O2 -Isrc/common -Isrc/server src/common/transport.cpp src/server/gpu_device.cpp src/server/server_dispatcher.cpp tests/test_server_stress.cpp -o build/test_server_stress.exe -ld3d11 -ldxgi -lws2_32 -ld3dcompiler -lpsapi
```

### Execution & Pass Criteria:
```powershell
.\build\test_server_stress.exe
```
- **Test 2.2**: Non-null-terminated 32-byte semantic names safely handled without access violation (`exc == 0`).
- **Test 2.5**: 150 pseudorandom fuzzed packets safely rejected with zero driver access violations.
- **Suite 4**: 5,500 resource allocations/destructions complete with stable memory.
- **Overall**: 181/181 stress test assertions pass with exit code 0 (`VERDICT: APPROVE`).
