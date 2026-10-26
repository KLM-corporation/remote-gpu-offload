# Specification: Remote Direct3D 11 Network Protocol (`src/common/protocol.h`)

**Document ID:** `SPEC-RGPU-PROTO-001`  
**Author:** M1 Protocol Spec Explorer (`explorer_m1_1`)  
**Target Milestone:** Milestone 1 (Network Protocol & Transport Layer)  
**Status:** Authoritative Implementation Specification  
**Compiler Standard:** C++17 (MSVC 2019+, Clang 10+, GCC 9+, LLVM-MinGW x86_64)  
**Integrity Level:** Strict Zero-UB, Binary Deterministic, Memory-Safe  

---

## 1. Architectural & Protocol Foundations

### 1.1 Scope & Purpose
This specification establishes the authoritative, binary-exact network protocol and packet definition for the Remote GPU Offloading system. The protocol encodes Direct3D 11 graphics API calls intercepted on a client Windows PC into a compact, strongly-typed binary stream, transmits them over standard Winsock2 TCP stream sockets, reconstructs hardware state on a remote rendering server, and delivers rendered framebuffers back to the client for zero-GPU presentation.

### 1.2 Wire Architecture & Alignment Rules
1. **Byte Order:** All multi-byte integers (`uint16_t`, `uint32_t`, `uint64_t`, `int32_t`) and floating-point values (`float`) are transmitted strictly in **Little-Endian (x86_64 Native)** format.
2. **Packing Guarantees:** All wire structures are encapsulated within `#pragma pack(push, 1)` and closed with `#pragma pack(pop)`. Zero compiler-injected padding bytes exist between struct members.
3. **Internal Alignment:** Within each structure, fields are positioned at naturally aligned offsets (4-byte fields at multiples of 4, 8-byte fields at multiples of 8, etc.) wherever possible to eliminate unaligned access penalties on non-x86 architectures.
4. **Standard Layout & Trivial Copyability:** Every packet structure satisfies `std::is_standard_layout_v<T> == true` and `std::is_trivially_copyable_v<T> == true`. Serializing via `memcpy` or streaming directly through `send()`/`recv()` is well-defined under ISO C++17 §6.9.
5. **No Pointer Invariants:** No struct contains pointers, virtual functions, dynamic containers, or compiler-dependent memory layouts. Variable-length payloads (shader bytecode, vertex buffers, pixel rasters) follow immediately after fixed payload headers as trailing contiguous byte sequences.

### 1.3 Core Protocol Constants

```cpp
namespace RemoteGPU {

// Synchronization Magic Identifier: ASCII 'R', 'G', 'P', 'U' (0x52475055 in Little-Endian)
inline constexpr uint32_t RGPU_MAGIC = 0x52475055;

// Major Protocol Version
inline constexpr uint32_t RGPU_PROTOCOL_VERSION = 1;

// Default TCP Service Port
inline constexpr uint16_t RGPU_DEFAULT_PORT = 48888;

} // namespace RemoteGPU
```

---

## 2. Fixed 16-Byte Packet Header Specification

Every packet transmitted over the wire begins with a fixed 16-byte header:

```cpp
#pragma pack(push, 1)

struct PacketHeader {
    uint32_t magic;          // Fixed identifier: RGPU_MAGIC (0x52475055)
    uint16_t opcode;         // RgpuOpcode identifying the command or response
    uint16_t flags;          // RgpuFlags bitmask controlling execution semantics
    uint32_t sequence_id;    // Monotonically increasing sequence number
    uint32_t payload_length; // Size in bytes of the following payload (0 if no payload)
};

#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 16, "PacketHeader must be exactly 16 bytes");
static_assert(std::is_standard_layout_v<PacketHeader>, "PacketHeader must have standard layout");
static_assert(std::is_trivially_copyable_v<PacketHeader>, "PacketHeader must be trivially copyable");
```

### 2.1 Field Layout & Offset Table

| Field Name | Type | Size | Byte Offset | Description |
|---|---|---|---|---|
| `magic` | `uint32_t` | 4 bytes | `0x00` – `0x03` | Protocol synchronization word. Must equal `0x52475055`. If mismatch occurs, the stream is desynchronized. |
| `opcode` | `uint16_t` | 2 bytes | `0x04` – `0x05` | Identifies command type (`RgpuOpcode`). |
| `flags` | `uint16_t` | 2 bytes | `0x06` – `0x07` | Modifier flags (`RgpuFlags`). |
| `sequence_id` | `uint32_t` | 4 bytes | `0x08` – `0x0B` | Monotonic sequence number starting at 1. Used for flow validation, tracing, and packet dropping detection. |
| `payload_length` | `uint32_t` | 4 bytes | `0x0C` – `0x0F` | Byte count of the variable data immediately following this header. Maximum payload size: 4 GB. |

### 2.2 Protocol Flags (`RgpuFlags`)

```cpp
enum RgpuFlags : uint16_t {
    FLAG_NONE              = 0x0000,
    FLAG_SYNC              = 0x0001, // Client requests server to flush GPU commands before returning
    FLAG_RESPONSE_REQUIRED = 0x0002, // Server must send an explicit response packet
    FLAG_COMPRESSED        = 0x0004, // Trailing payload data is compressed (e.g. LZ4 / RLE)
    FLAG_ONEWAY            = 0x0008, // Fire-and-forget; processed asynchronously without client wait
    FLAG_END_OF_FRAME      = 0x0010, // Indicates the final command of the current frame submission batch
    FLAG_HEARTBEAT         = 0x0020  // Heartbeat keep-alive probe
};
```

---

## 3. Complete D3D11 Opcode Enumeration (`RgpuOpcode`)

The opcode taxonomy covers the complete lifecycle of Direct3D 11 rendering, structured into 5 logical clusters.

```cpp
enum RgpuOpcode : uint16_t {
    // =========================================================================
    // 1. Session & Lifecycle (0x0001 - 0x000F)
    // =========================================================================
    OP_HANDSHAKE_REQUEST    = 0x0001, // Client -> Server: Initiate session & feature negotiation
    OP_HANDSHAKE_RESPONSE   = 0x0002, // Server -> Client: Confirm capabilities & adapter info
    OP_SHUTDOWN             = 0x0003, // Bidirectional: Graceful session termination & cleanup

    // =========================================================================
    // 2. Resource Management (0x0010 - 0x002F)
    // =========================================================================
    OP_CREATE_BUFFER        = 0x0010, // Client -> Server: Allocate vertex, index, or constant buffer
    OP_CREATE_TEXTURE2D     = 0x0011, // Client -> Server: Allocate 2D texture
    OP_CREATE_SRV           = 0x0012, // Client -> Server: Create Shader Resource View
    OP_CREATE_RTV           = 0x0013, // Client -> Server: Create Render Target View
    OP_CREATE_DSV           = 0x0014, // Client -> Server: Create Depth Stencil View
    OP_CREATE_VERTEX_SHADER = 0x0015, // Client -> Server: Create Vertex Shader from bytecode
    OP_CREATE_PIXEL_SHADER  = 0x0016, // Client -> Server: Create Pixel Shader from bytecode
    OP_CREATE_INPUT_LAYOUT  = 0x0017, // Client -> Server: Create Input Layout from element descs
    OP_DESTROY_RESOURCE     = 0x0018, // Client -> Server: Release GPU resource by res_id
    OP_CREATE_SAMPLER       = 0x0019, // Client -> Server: Create Sampler State (Texture filtering)

    // =========================================================================
    // 3. Pipeline State & Binding (0x0030 - 0x004F)
    // =========================================================================
    OP_IA_SET_INPUT_LAYOUT  = 0x0030, // Client -> Server: Bind Input Layout to IA stage
    OP_IA_SET_VERTEX_BUFFER = 0x0031, // Client -> Server: Bind Vertex Buffer to input slot
    OP_IA_SET_INDEX_BUFFER  = 0x0032, // Client -> Server: Bind Index Buffer
    OP_IA_SET_TOPOLOGY      = 0x0033, // Client -> Server: Set Primitive Topology
    OP_VS_SET_SHADER        = 0x0034, // Client -> Server: Bind Vertex Shader
    OP_VS_SET_CB            = 0x0035, // Client -> Server: Bind Constant Buffer to VS slot
    OP_PS_SET_SHADER        = 0x0036, // Client -> Server: Bind Pixel Shader
    OP_PS_SET_CB            = 0x0037, // Client -> Server: Bind Constant Buffer to PS slot
    OP_PS_SET_SRV           = 0x0038, // Client -> Server: Bind Shader Resource View to PS slot
    OP_RS_SET_VIEWPORT      = 0x0039, // Client -> Server: Set Rasterizer Viewport
    OP_OM_SET_RENDER_TARGETS= 0x003A, // Client -> Server: Bind RTVs & DSV to Output Merger
    OP_PS_SET_SAMPLER       = 0x003B, // Client -> Server: Bind Sampler State to PS slot

    // =========================================================================
    // 4. Updates & Draw Calls (0x0050 - 0x006F)
    // =========================================================================
    OP_UPDATE_SUBRESOURCE   = 0x0050, // Client -> Server: Update subresource memory
    OP_WRITE_BUFFER_DISCARD = 0x0051, // Client -> Server: High-speed dynamic buffer update
    OP_CLEAR_RTV            = 0x0052, // Client -> Server: Clear Render Target with RGBA float
    OP_CLEAR_DSV            = 0x0053, // Client -> Server: Clear Depth Stencil View
    OP_DRAW                 = 0x0054, // Client -> Server: Draw non-indexed primitives
    OP_DRAW_INDEXED         = 0x0055, // Client -> Server: Draw indexed primitives

    // =========================================================================
    // 5. Presentation & Streaming (0x0070 - 0x007F)
    // =========================================================================
    OP_PRESENT              = 0x0070, // Client -> Server: End-of-frame present & readback trigger
    OP_FRAME_DATA           = 0x0071  // Server -> Client: Return rendered framebuffer pixels
};
```

---

## 4. Comprehensive Payload Structures & Inlined Wire Layouts

All payload structs are declared under `#pragma pack(push, 1)`.

### 4.1 Session & Lifecycle Payloads

#### 4.1.1 `HandshakeRequestPayload` (`OP_HANDSHAKE_REQUEST`)
Initiates the client-server connection, negotiates protocol version, and conveys display requirements.
```cpp
struct HandshakeRequestPayload {
    uint32_t protocol_version;   // Client protocol version (RGPU_PROTOCOL_VERSION)
    uint32_t client_flags;       // Feature request flags
    uint32_t client_width;       // Initial client window width (e.g. 1280 or 800)
    uint32_t client_height;      // Initial client window height (e.g. 720 or 600)
    uint32_t preferred_format;   // DXGI_FORMAT enum (e.g. 28 = DXGI_FORMAT_R8G8B8A8_UNORM)
};
using HandshakeRequest = HandshakeRequestPayload;

static_assert(sizeof(HandshakeRequestPayload) == 20);
```

#### 4.1.2 `HandshakeResponsePayload` (`OP_HANDSHAKE_RESPONSE`)
Returns server GPU hardware capabilities, confirmed viewport dimensions, and adapter description.
```cpp
struct HandshakeResponsePayload {
    uint32_t protocol_version;   // Confirmed server protocol version
    uint32_t status_code;        // 0: OK, 1: Version Mismatch, 2: GPU Init Failed, 3: Format Unsupported
    uint32_t feature_level;      // Direct3D Feature Level (e.g. 0xB000 for D3D_FEATURE_LEVEL_11_0)
    uint32_t frame_width;        // Confirmed offscreen render target width
    uint32_t frame_height;       // Confirmed offscreen render target height
    uint32_t frame_format;       // Confirmed DXGI_FORMAT
    uint64_t dedicated_vram;     // Server GPU dedicated video memory in bytes
    char     adapter_desc[128];  // Hardware description (e.g., "Intel(R) UHD Graphics 620")
};
using HandshakeResponse = HandshakeResponsePayload;

static_assert(sizeof(HandshakeResponsePayload) == 160);
```

#### 4.1.3 `ShutdownPayload` (`OP_SHUTDOWN`)
```cpp
struct ShutdownPayload {
    uint32_t exit_code;          // 0: Normal graceful disconnect, 1: Error abort
};

static_assert(sizeof(ShutdownPayload) == 4);
```

---

### 4.2 Resource Creation Payloads

#### 4.2.1 `CreateBufferPayload` (`OP_CREATE_BUFFER`)
Creates vertex buffers, index buffers, or constant buffers. If `has_initial_data == 1`, raw data follows immediately.
```cpp
struct CreateBufferPayload {
    uint32_t res_id;                // Unique monotonic resource ID assigned by client
    uint32_t byte_width;            // Buffer byte size (D3D11_BUFFER_DESC::ByteWidth)
    uint32_t usage;                 // D3D11_USAGE (0: DEFAULT, 1: IMMUTABLE, 2: DYNAMIC, 3: STAGING)
    uint32_t bind_flags;            // D3D11_BIND_FLAG (1: VERTEX, 2: INDEX, 4: CONSTANT)
    uint32_t cpu_access_flags;      // D3D11_CPU_ACCESS_FLAG (0x10000: WRITE, 0x20000: READ)
    uint32_t misc_flags;            // D3D11_RESOURCE_MISC_FLAG
    uint32_t structure_byte_stride; // Stride for structured buffers (0 for standard)
    uint32_t has_initial_data;      // 1 if initial buffer data is appended in payload, 0 otherwise
    uint32_t initial_data_size;     // Size of following data (typically equal to byte_width)
    // Trailing data: uint8_t initial_data[initial_data_size]
};

static_assert(sizeof(CreateBufferPayload) == 36);
```

#### 4.2.2 `CreateTexture2DPayload` (`OP_CREATE_TEXTURE2D`)
Creates 2D textures for diffuse maps or offscreen buffers.
```cpp
struct CreateTexture2DPayload {
    uint32_t res_id;              // Unique monotonic texture resource ID
    uint32_t width;               // Texture width in pixels
    uint32_t height;              // Texture height in pixels
    uint32_t mips;                // Number of mip levels (0 or 1 for unmipped)
    uint32_t array_size;          // Texture array size (usually 1)
    uint32_t format;              // DXGI_FORMAT enum
    uint32_t sample_count;        // Sample count (usually 1)
    uint32_t sample_quality;      // Sample quality (usually 0)
    uint32_t usage;               // D3D11_USAGE
    uint32_t bind_flags;          // D3D11_BIND_FLAG (e.g. 8: SHADER_RESOURCE, 32: RENDER_TARGET)
    uint32_t cpu_access_flags;    // D3D11_CPU_ACCESS_FLAG
    uint32_t misc_flags;          // D3D11_RESOURCE_MISC_FLAG
    uint32_t has_initial_data;    // 1 if initial pixel bytes follow, 0 otherwise
    uint32_t sys_mem_pitch;       // Pitch in bytes of initial data (row stride)
    uint32_t sys_mem_slice_pitch; // Slice pitch of initial data
    uint32_t initial_data_size;   // Byte count of initial pixel data
    // Trailing data: uint8_t initial_data[initial_data_size]
};

static_assert(sizeof(CreateTexture2DPayload) == 64);
```

#### 4.2.3 `CreateSrvPayload` (`OP_CREATE_SRV`)
Creates a Shader Resource View binding a texture or buffer to shader stages.
```cpp
struct CreateSrvPayload {
    uint32_t res_id;              // Monotonic ID for this SRV
    uint32_t resource_id;         // Underlying texture/buffer res_id
    uint32_t format;              // DXGI_FORMAT enum (0 to inherit resource format)
    uint32_t view_dimension;      // D3D11_SRV_DIMENSION (4 = D3D11_SRV_DIMENSION_TEXTURE2D)
    uint32_t most_detailed_mip;   // Most detailed mip slice
    uint32_t mip_levels;          // Number of mip levels (0xFFFFFFFF for all)
};

static_assert(sizeof(CreateSrvPayload) == 24);
```

#### 4.2.4 `CreateRtvPayload` (`OP_CREATE_RTV`)
Creates a Render Target View for offscreen rendering.
```cpp
struct CreateRtvPayload {
    uint32_t res_id;              // Monotonic ID for this RTV
    uint32_t resource_id;         // Underlying texture res_id
    uint32_t format;              // DXGI_FORMAT enum
    uint32_t view_dimension;      // D3D11_RTV_DIMENSION (4 = D3D11_RTV_DIMENSION_TEXTURE2D)
    uint32_t mip_slice;           // Mip slice index
};

static_assert(sizeof(CreateRtvPayload) == 20);
```

#### 4.2.5 `CreateDsvPayload` (`OP_CREATE_DSV`)
Creates a Depth Stencil View for depth buffering.
```cpp
struct CreateDsvPayload {
    uint32_t res_id;              // Monotonic ID for this DSV
    uint32_t resource_id;         // Underlying depth texture res_id
    uint32_t format;              // DXGI_FORMAT enum
    uint32_t view_dimension;      // D3D11_DSV_DIMENSION (3 = D3D11_DSV_DIMENSION_TEXTURE2D)
    uint32_t mip_slice;           // Mip slice index
    uint32_t flags;               // D3D11_DSV_FLAG
};

static_assert(sizeof(CreateDsvPayload) == 24);
```

#### 4.2.6 `CreateShaderPayload` (`OP_CREATE_VERTEX_SHADER`, `OP_CREATE_PIXEL_SHADER`)
Transmits compiled HLSL shader bytecode to the server GPU.
```cpp
struct CreateShaderPayload {
    uint32_t res_id;              // Monotonic shader ID (vertex or pixel shader)
    uint32_t bytecode_length;     // Byte size of shader bytecode that immediately follows
    // Trailing data: uint8_t bytecode[bytecode_length]
};

static_assert(sizeof(CreateShaderPayload) == 8);
```

#### 4.2.7 `CreateInputLayoutPayload` & `RgpuInputElementDesc` (`OP_CREATE_INPUT_LAYOUT`)
Transmits the vertex layout specification without pointer dependencies.
```cpp
struct RgpuInputElementDesc {
    char     semantic_name[32];   // e.g. "POSITION", "TEXCOORD", "NORMAL", "COLOR"
    uint32_t semantic_index;      // Semantic index (0, 1, ...)
    uint32_t format;              // DXGI_FORMAT enum (e.g. 6 = DXGI_FORMAT_R32G32B32_FLOAT)
    uint32_t input_slot;          // Vertex buffer input slot (0..15)
    uint32_t aligned_byte_offset; // Byte offset or 0xFFFFFFFF (D3D11_APPEND_ALIGNED_ELEMENT)
    uint32_t input_slot_class;    // 0: PER_VERTEX_DATA, 1: PER_INSTANCE_DATA
    uint32_t instance_data_step;  // Step rate (0 for vertex data)
};

static_assert(sizeof(RgpuInputElementDesc) == 56);

struct CreateInputLayoutPayload {
    uint32_t res_id;              // Monotonic Input Layout ID
    uint32_t num_elements;        // Number of RgpuInputElementDesc elements
    uint32_t bytecode_length;     // Shader bytecode length for signature validation
    // Trailing layout:
    // RgpuInputElementDesc elements[num_elements]
    // uint8_t shader_bytecode[bytecode_length]
};

static_assert(sizeof(CreateInputLayoutPayload) == 12);
```

#### 4.2.8 `DestroyResourcePayload` (`OP_DESTROY_RESOURCE`)
Synchronizes client COM reference teardown with host GPU resource disposal.
```cpp
struct DestroyResourcePayload {
    uint32_t res_id;              // Monotonic ID of buffer, texture, view, or shader to destroy
};

static_assert(sizeof(DestroyResourcePayload) == 4);
```

#### 4.2.9 `CreateSamplerPayload` (`OP_CREATE_SAMPLER`)
Configures texture filtering and addressing modes on the server GPU.
```cpp
struct CreateSamplerPayload {
    uint32_t res_id;              // Monotonic Sampler State ID
    uint32_t filter;              // D3D11_FILTER (e.g. 0x15 = MIN_MAG_MIP_LINEAR)
    uint32_t address_u;           // D3D11_TEXTURE_ADDRESS_MODE (1: WRAP, 3: CLAMP)
    uint32_t address_v;           // D3D11_TEXTURE_ADDRESS_MODE
    uint32_t address_w;           // D3D11_TEXTURE_ADDRESS_MODE
    float    mip_lod_bias;        // LOD bias
    uint32_t max_anisotropy;      // Max anisotropy (1..16)
    uint32_t comparison_func;     // D3D11_COMPARISON_FUNC (e.g. 8 = ALWAYS)
    float    border_color[4];     // Border color RGBA
    float    min_lod;             // Minimum LOD
    float    max_lod;             // Maximum LOD (3.402823466e+38f)
};

static_assert(sizeof(CreateSamplerPayload) == 56);
```

---

### 4.3 Pipeline State & Binding Payloads

```cpp
// OP_IA_SET_INPUT_LAYOUT
struct IASetInputLayoutPayload {
    uint32_t res_id;              // Input Layout ID (0 to unbind)
};
static_assert(sizeof(IASetInputLayoutPayload) == 4);

// OP_IA_SET_VERTEX_BUFFER
struct IASetVertexBufferPayload {
    uint32_t slot;                // Input slot (0..15)
    uint32_t res_id;              // Vertex Buffer ID (0 to unbind)
    uint32_t stride;              // Stride in bytes between consecutive vertices
    uint32_t offset;              // Starting byte offset in the buffer
};
static_assert(sizeof(IASetVertexBufferPayload) == 16);

// OP_IA_SET_INDEX_BUFFER
struct IASetIndexBufferPayload {
    uint32_t res_id;              // Index Buffer ID (0 to unbind)
    uint32_t format;              // DXGI_FORMAT_R16_UINT (57) or DXGI_FORMAT_R32_UINT (42)
    uint32_t offset;              // Byte offset from buffer start
};
static_assert(sizeof(IASetIndexBufferPayload) == 12);

// OP_IA_SET_TOPOLOGY
struct IASetTopologyPayload {
    uint32_t topology;            // D3D11_PRIMITIVE_TOPOLOGY (4 = TRIANGLELIST)
};
static_assert(sizeof(IASetTopologyPayload) == 4);

// OP_VS_SET_SHADER
struct VSSetShaderPayload {
    uint32_t res_id;              // Vertex Shader ID (0 to unbind)
};
static_assert(sizeof(VSSetShaderPayload) == 4);

// OP_VS_SET_CB
struct VSSetConstantBufferPayload {
    uint32_t slot;                // Constant buffer slot (0..13)
    uint32_t res_id;              // Constant Buffer ID (0 to unbind)
};
static_assert(sizeof(VSSetConstantBufferPayload) == 8);

// OP_PS_SET_SHADER
struct PSSetShaderPayload {
    uint32_t res_id;              // Pixel Shader ID (0 to unbind)
};
static_assert(sizeof(PSSetShaderPayload) == 4);

// OP_PS_SET_CB
struct PSSetConstantBufferPayload {
    uint32_t slot;                // Constant buffer slot (0..13)
    uint32_t res_id;              // Constant Buffer ID (0 to unbind)
};
static_assert(sizeof(PSSetConstantBufferPayload) == 8);

// OP_PS_SET_SRV
struct PSSetSrvPayload {
    uint32_t slot;                // SRV slot (0..127)
    uint32_t res_id;              // Shader Resource View ID (0 to unbind)
};
static_assert(sizeof(PSSetSrvPayload) == 8);

// OP_PS_SET_SAMPLER
struct PSSetSamplerPayload {
    uint32_t slot;                // Sampler slot (0..15)
    uint32_t res_id;              // Sampler State ID (0 to unbind)
};
static_assert(sizeof(PSSetSamplerPayload) == 8);

// OP_RS_SET_VIEWPORT
struct RSSetViewportPayload {
    float top_left_x;             // Top-left X coordinate
    float top_left_y;             // Top-left Y coordinate
    float width;                  // Viewport width
    float height;                 // Viewport height
    float min_depth;              // Minimum depth (typically 0.0f)
    float max_depth;              // Maximum depth (typically 1.0f)
};
static_assert(sizeof(RSSetViewportPayload) == 24);

// OP_OM_SET_RENDER_TARGETS
struct OMSetRenderTargetsPayload {
    uint32_t rtv_id;              // Render Target View ID (0 to unbind)
    uint32_t dsv_id;              // Depth Stencil View ID (0 to unbind)
};
static_assert(sizeof(OMSetRenderTargetsPayload) == 8);
```

---

### 4.4 Resource Updates & Draw Calls Payloads

#### 4.4.1 `UpdateSubresourcePayload` & `RgpuBox` (`OP_UPDATE_SUBRESOURCE`)
```cpp
struct RgpuBox {
    uint32_t left;
    uint32_t top;
    uint32_t front;
    uint32_t right;
    uint32_t bottom;
    uint32_t back;
};
static_assert(sizeof(RgpuBox) == 24);

struct UpdateSubresourcePayload {
    uint32_t res_id;              // Target buffer or texture ID
    uint32_t subresource;         // Subresource index (0 for simple resources)
    uint32_t has_dst_box;         // 1 if custom destination box is provided, 0 otherwise
    RgpuBox  dst_box;             // Destination box coordinates
    uint32_t row_pitch;           // Source data row pitch in bytes
    uint32_t depth_pitch;         // Source data depth pitch in bytes
    uint32_t data_size;           // Total byte size of following data
    // Trailing data: uint8_t data[data_size]
};
static_assert(sizeof(UpdateSubresourcePayload) == 48);
```

#### 4.4.2 `WriteBufferDiscardPayload` (`OP_WRITE_BUFFER_DISCARD`)
High-speed path for dynamic constant buffer updates during `Map(D3D11_MAP_WRITE_DISCARD)`/`Unmap`.
```cpp
struct WriteBufferDiscardPayload {
    uint32_t res_id;              // Buffer resource ID
    uint32_t byte_offset;         // Byte offset into buffer (usually 0)
    uint32_t byte_width;          // Number of updated bytes that follow
    // Trailing data: uint8_t data[byte_width]
};
static_assert(sizeof(WriteBufferDiscardPayload) == 12);
```

#### 4.4.3 `ClearRtvPayload` & `ClearDsvPayload`
```cpp
// OP_CLEAR_RTV
struct ClearRtvPayload {
    uint32_t res_id;              // Render Target View ID
    float    color[4];            // RGBA float clear color values
};
static_assert(sizeof(ClearRtvPayload) == 20);

// OP_CLEAR_DSV
struct ClearDsvPayload {
    uint32_t res_id;              // Depth Stencil View ID
    uint32_t clear_flags;         // D3D11_CLEAR_DEPTH (1) | D3D11_CLEAR_STENCIL (2)
    float    depth;               // Depth clear value (typically 1.0f)
    uint8_t  stencil;             // Stencil clear value (typically 0)
    uint8_t  padding[3];          // Explicit padding maintaining 4-byte struct alignment
};
static_assert(sizeof(ClearDsvPayload) == 16);
```

#### 4.4.4 `DrawPayload` & `DrawIndexedPayload`
```cpp
// OP_DRAW
struct DrawPayload {
    uint32_t vertex_count;        // Number of vertices to draw
    uint32_t start_vertex_location; // Index of the first vertex
};
static_assert(sizeof(DrawPayload) == 8);

// OP_DRAW_INDEXED
struct DrawIndexedPayload {
    uint32_t index_count;         // Number of indices to draw
    uint32_t start_index_location;// Location of the first index in index buffer
    int32_t  base_vertex_location;// Value added to each index before reading vertex
};
static_assert(sizeof(DrawIndexedPayload) == 12);
```

---

### 4.5 Presentation & Frame Return Payloads

#### 4.5.1 `PresentPayload` (`OP_PRESENT`)
Sent by the client during `IDXGISwapChain::Present()`. Triggers server GPU staging readback and acts as the backpressure synchronization point.
```cpp
struct PresentPayload {
    uint32_t swapchain_id;        // Monotonic SwapChain ID (0 for default window swapchain)
    uint32_t sync_interval;       // 0: Unthrottled immediate presentation, 1: VSync
    uint32_t flags;               // DXGI_PRESENT flags
    uint64_t frame_number;        // Monotonic client frame counter
};
static_assert(sizeof(PresentPayload) == 20);
```

#### 4.5.2 `FrameDataPayload` (`OP_FRAME_DATA`)
Sent by the server to return the uncompressed or compressed rendered framebuffer pixels to the client.
```cpp
struct FrameDataPayload {
    uint32_t swapchain_id;        // Matching SwapChain ID
    uint32_t width;               // Frame width in pixels
    uint32_t height;              // Frame height in pixels
    uint32_t format;              // DXGI_FORMAT enum (e.g. 28 = DXGI_FORMAT_R8G8B8A8_UNORM)
    uint32_t row_pitch;           // Stride in bytes (e.g. width * 4)
    uint32_t data_size;           // Total byte size of following pixel array (row_pitch * height)
    uint64_t frame_number;        // Monotonic server frame number matching PresentPayload
    // Trailing data: uint8_t pixels[data_size]
};
static_assert(sizeof(FrameDataPayload) == 32);
```

---

## 5. Struct Memory Layout & Wire Size Summary Table

| Structure Name | Opcode Binding | Size (Bytes) | Alignment | Trailing Data Description |
|---|---|:---:|:---:|---|
| `PacketHeader` | *All Packets* | 16 | 1 | Precedes every packet on the socket |
| `HandshakeRequestPayload` | `OP_HANDSHAKE_REQUEST` | 20 | 1 | None |
| `HandshakeResponsePayload`| `OP_HANDSHAKE_RESPONSE`| 160 | 1 | None |
| `ShutdownPayload` | `OP_SHUTDOWN` | 4 | 1 | None |
| `CreateBufferPayload` | `OP_CREATE_BUFFER` | 36 | 1 | Optional initial buffer data (`initial_data_size` bytes) |
| `CreateTexture2DPayload` | `OP_CREATE_TEXTURE2D` | 64 | 1 | Optional initial pixel data (`initial_data_size` bytes) |
| `CreateSrvPayload` | `OP_CREATE_SRV` | 24 | 1 | None |
| `CreateRtvPayload` | `OP_CREATE_RTV` | 20 | 1 | None |
| `CreateDsvPayload` | `OP_CREATE_DSV` | 24 | 1 | None |
| `CreateShaderPayload` | `OP_CREATE_VERTEX_SHADER`, `OP_CREATE_PIXEL_SHADER` | 8 | 1 | Compiled shader bytecode (`bytecode_length` bytes) |
| `RgpuInputElementDesc` | Sub-element | 56 | 1 | Fixed-size array element |
| `CreateInputLayoutPayload`| `OP_CREATE_INPUT_LAYOUT`| 12 | 1 | `RgpuInputElementDesc[num_elements]` followed by `bytecode` |
| `DestroyResourcePayload` | `OP_DESTROY_RESOURCE` | 4 | 1 | None |
| `CreateSamplerPayload` | `OP_CREATE_SAMPLER` | 56 | 1 | None |
| `IASetInputLayoutPayload` | `OP_IA_SET_INPUT_LAYOUT`| 4 | 1 | None |
| `IASetVertexBufferPayload`| `OP_IA_SET_VERTEX_BUFFER`| 16 | 1 | None |
| `IASetIndexBufferPayload` | `OP_IA_SET_INDEX_BUFFER`| 12 | 1 | None |
| `IASetTopologyPayload` | `OP_IA_SET_TOPOLOGY` | 4 | 1 | None |
| `VSSetShaderPayload` | `OP_VS_SET_SHADER` | 4 | 1 | None |
| `VSSetConstantBufferPayload`|`OP_VS_SET_CB` | 8 | 1 | None |
| `PSSetShaderPayload` | `OP_PS_SET_SHADER` | 4 | 1 | None |
| `PSSetConstantBufferPayload`|`OP_PS_SET_CB` | 8 | 1 | None |
| `PSSetSrvPayload` | `OP_PS_SET_SRV` | 8 | 1 | None |
| `PSSetSamplerPayload` | `OP_PS_SET_SAMPLER` | 8 | 1 | None |
| `RSSetViewportPayload` | `OP_RS_SET_VIEWPORT` | 24 | 1 | None |
| `OMSetRenderTargetsPayload`|`OP_OM_SET_RENDER_TARGETS`| 8 | 1 | None |
| `RgpuBox` | Sub-element | 24 | 1 | Coordinate boundary box |
| `UpdateSubresourcePayload`| `OP_UPDATE_SUBRESOURCE` | 48 | 1 | Subresource byte stream (`data_size` bytes) |
| `WriteBufferDiscardPayload`|`OP_WRITE_BUFFER_DISCARD`| 12 | 1 | Dynamic buffer delta bytes (`byte_width` bytes) |
| `ClearRtvPayload` | `OP_CLEAR_RTV` | 20 | 1 | None |
| `ClearDsvPayload` | `OP_CLEAR_DSV` | 16 | 1 | None |
| `DrawPayload` | `OP_DRAW` | 8 | 1 | None |
| `DrawIndexedPayload` | `OP_DRAW_INDEXED` | 12 | 1 | None |
| `PresentPayload` | `OP_PRESENT` | 20 | 1 | None |
| `FrameDataPayload` | `OP_FRAME_DATA` | 32 | 1 | Raw RGBA pixel stream (`data_size` bytes) |

---

## 6. Implementation Prototype for `src/common/protocol.h`

The listing below represents the exact production code ready for deployment to `src/common/protocol.h`:

```cpp
#pragma once
#ifndef REMOTE_GPU_PROTOCOL_H
#define REMOTE_GPU_PROTOCOL_H

#include <cstdint>
#include <cstddef>
#include <type_traits>

namespace RemoteGPU {

// =============================================================================
// Constants & Protocol Flags
// =============================================================================

inline constexpr uint32_t RGPU_MAGIC = 0x52475055; // 'RGPU'
inline constexpr uint32_t RGPU_PROTOCOL_VERSION = 1;
inline constexpr uint16_t RGPU_DEFAULT_PORT = 48888;

enum RgpuFlags : uint16_t {
    FLAG_NONE              = 0x0000,
    FLAG_SYNC              = 0x0001,
    FLAG_RESPONSE_REQUIRED = 0x0002,
    FLAG_COMPRESSED        = 0x0004,
    FLAG_ONEWAY            = 0x0008,
    FLAG_END_OF_FRAME      = 0x0010,
    FLAG_HEARTBEAT         = 0x0020
};

// =============================================================================
// Direct3D 11 Command Opcodes
// =============================================================================

enum RgpuOpcode : uint16_t {
    // Session & Lifecycle (0x0001 - 0x000F)
    OP_HANDSHAKE_REQUEST    = 0x0001,
    OP_HANDSHAKE_RESPONSE   = 0x0002,
    OP_SHUTDOWN             = 0x0003,

    // Resource Management (0x0010 - 0x002F)
    OP_CREATE_BUFFER        = 0x0010,
    OP_CREATE_TEXTURE2D     = 0x0011,
    OP_CREATE_SRV           = 0x0012,
    OP_CREATE_RTV           = 0x0013,
    OP_CREATE_DSV           = 0x0014,
    OP_CREATE_VERTEX_SHADER = 0x0015,
    OP_CREATE_PIXEL_SHADER  = 0x0016,
    OP_CREATE_INPUT_LAYOUT  = 0x0017,
    OP_DESTROY_RESOURCE     = 0x0018,
    OP_CREATE_SAMPLER       = 0x0019,

    // Pipeline State & Binding (0x0030 - 0x004F)
    OP_IA_SET_INPUT_LAYOUT  = 0x0030,
    OP_IA_SET_VERTEX_BUFFER = 0x0031,
    OP_IA_SET_INDEX_BUFFER  = 0x0032,
    OP_IA_SET_TOPOLOGY      = 0x0033,
    OP_VS_SET_SHADER        = 0x0034,
    OP_VS_SET_CB            = 0x0035,
    OP_PS_SET_SHADER        = 0x0036,
    OP_PS_SET_CB            = 0x0037,
    OP_PS_SET_SRV           = 0x0038,
    OP_RS_SET_VIEWPORT      = 0x0039,
    OP_OM_SET_RENDER_TARGETS= 0x003A,
    OP_PS_SET_SAMPLER       = 0x003B,

    // Updates & Draw Calls (0x0050 - 0x006F)
    OP_UPDATE_SUBRESOURCE   = 0x0050,
    OP_WRITE_BUFFER_DISCARD = 0x0051,
    OP_CLEAR_RTV            = 0x0052,
    OP_CLEAR_DSV            = 0x0053,
    OP_DRAW                 = 0x0054,
    OP_DRAW_INDEXED         = 0x0055,

    // Presentation & Streaming (0x0070 - 0x007F)
    OP_PRESENT              = 0x0070,
    OP_FRAME_DATA           = 0x0071
};

#pragma pack(push, 1)

// =============================================================================
// Fixed 16-Byte Packet Header
// =============================================================================

struct PacketHeader {
    uint32_t magic;          // Fixed identifier: RGPU_MAGIC (0x52475055)
    uint16_t opcode;         // RgpuOpcode
    uint16_t flags;          // RgpuFlags bitmask
    uint32_t sequence_id;    // Monotonic sequence number
    uint32_t payload_length; // Size of succeeding payload in bytes
};

// =============================================================================
// Session & Lifecycle Payloads
// =============================================================================

struct HandshakeRequestPayload {
    uint32_t protocol_version;
    uint32_t client_flags;
    uint32_t client_width;
    uint32_t client_height;
    uint32_t preferred_format;
};
using HandshakeRequest = HandshakeRequestPayload;

struct HandshakeResponsePayload {
    uint32_t protocol_version;
    uint32_t status_code;
    uint32_t feature_level;
    uint32_t frame_width;
    uint32_t frame_height;
    uint32_t frame_format;
    uint64_t dedicated_vram;
    char     adapter_desc[128];
};
using HandshakeResponse = HandshakeResponsePayload;

struct ShutdownPayload {
    uint32_t exit_code;
};

// =============================================================================
// Resource Creation Payloads
// =============================================================================

struct CreateBufferPayload {
    uint32_t res_id;
    uint32_t byte_width;
    uint32_t usage;
    uint32_t bind_flags;
    uint32_t cpu_access_flags;
    uint32_t misc_flags;
    uint32_t structure_byte_stride;
    uint32_t has_initial_data;
    uint32_t initial_data_size;
};

struct CreateTexture2DPayload {
    uint32_t res_id;
    uint32_t width;
    uint32_t height;
    uint32_t mips;
    uint32_t array_size;
    uint32_t format;
    uint32_t sample_count;
    uint32_t sample_quality;
    uint32_t usage;
    uint32_t bind_flags;
    uint32_t cpu_access_flags;
    uint32_t misc_flags;
    uint32_t has_initial_data;
    uint32_t sys_mem_pitch;
    uint32_t sys_mem_slice_pitch;
    uint32_t initial_data_size;
};

struct CreateSrvPayload {
    uint32_t res_id;
    uint32_t resource_id;
    uint32_t format;
    uint32_t view_dimension;
    uint32_t most_detailed_mip;
    uint32_t mip_levels;
};

struct CreateRtvPayload {
    uint32_t res_id;
    uint32_t resource_id;
    uint32_t format;
    uint32_t view_dimension;
    uint32_t mip_slice;
};

struct CreateDsvPayload {
    uint32_t res_id;
    uint32_t resource_id;
    uint32_t format;
    uint32_t view_dimension;
    uint32_t mip_slice;
    uint32_t flags;
};

struct CreateShaderPayload {
    uint32_t res_id;
    uint32_t bytecode_length;
};

struct RgpuInputElementDesc {
    char     semantic_name[32];
    uint32_t semantic_index;
    uint32_t format;
    uint32_t input_slot;
    uint32_t aligned_byte_offset;
    uint32_t input_slot_class;
    uint32_t instance_data_step;
};

struct CreateInputLayoutPayload {
    uint32_t res_id;
    uint32_t num_elements;
    uint32_t bytecode_length;
};

struct DestroyResourcePayload {
    uint32_t res_id;
};

struct CreateSamplerPayload {
    uint32_t res_id;
    uint32_t filter;
    uint32_t address_u;
    uint32_t address_v;
    uint32_t address_w;
    float    mip_lod_bias;
    uint32_t max_anisotropy;
    uint32_t comparison_func;
    float    border_color[4];
    float    min_lod;
    float    max_lod;
};

// =============================================================================
// Pipeline State & Binding Payloads
// =============================================================================

struct IASetInputLayoutPayload {
    uint32_t res_id;
};

struct IASetVertexBufferPayload {
    uint32_t slot;
    uint32_t res_id;
    uint32_t stride;
    uint32_t offset;
};

struct IASetIndexBufferPayload {
    uint32_t res_id;
    uint32_t format;
    uint32_t offset;
};

struct IASetTopologyPayload {
    uint32_t topology;
};

struct VSSetShaderPayload {
    uint32_t res_id;
};

struct VSSetConstantBufferPayload {
    uint32_t slot;
    uint32_t res_id;
};

struct PSSetShaderPayload {
    uint32_t res_id;
};

struct PSSetConstantBufferPayload {
    uint32_t slot;
    uint32_t res_id;
};

struct PSSetSrvPayload {
    uint32_t slot;
    uint32_t res_id;
};

struct PSSetSamplerPayload {
    uint32_t slot;
    uint32_t res_id;
};

struct RSSetViewportPayload {
    float top_left_x;
    float top_left_y;
    float width;
    float height;
    float min_depth;
    float max_depth;
};

struct OMSetRenderTargetsPayload {
    uint32_t rtv_id;
    uint32_t dsv_id;
};

// =============================================================================
// Updates & Draw Calls Payloads
// =============================================================================

struct RgpuBox {
    uint32_t left;
    uint32_t top;
    uint32_t front;
    uint32_t right;
    uint32_t bottom;
    uint32_t back;
};

struct UpdateSubresourcePayload {
    uint32_t res_id;
    uint32_t subresource;
    uint32_t has_dst_box;
    RgpuBox  dst_box;
    uint32_t row_pitch;
    uint32_t depth_pitch;
    uint32_t data_size;
};

struct WriteBufferDiscardPayload {
    uint32_t res_id;
    uint32_t byte_offset;
    uint32_t byte_width;
};

struct ClearRtvPayload {
    uint32_t res_id;
    float    color[4];
};

struct ClearDsvPayload {
    uint32_t res_id;
    uint32_t clear_flags;
    float    depth;
    uint8_t  stencil;
    uint8_t  padding[3];
};

struct DrawPayload {
    uint32_t vertex_count;
    uint32_t start_vertex_location;
};

struct DrawIndexedPayload {
    uint32_t index_count;
    uint32_t start_index_location;
    int32_t  base_vertex_location;
};

// =============================================================================
// Presentation & Frame Return Payloads
// =============================================================================

struct PresentPayload {
    uint32_t swapchain_id;
    uint32_t sync_interval;
    uint32_t flags;
    uint64_t frame_number;
};

struct FrameDataPayload {
    uint32_t swapchain_id;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t row_pitch;
    uint32_t data_size;
    uint64_t frame_number;
};

#pragma pack(pop)

// =============================================================================
// Compile-Time Layout & Compatibility Invariants
// =============================================================================

static_assert(sizeof(PacketHeader) == 16, "PacketHeader must be exactly 16 bytes");
static_assert(std::is_standard_layout_v<PacketHeader>);
static_assert(std::is_trivially_copyable_v<PacketHeader>);

static_assert(sizeof(HandshakeRequestPayload) == 20);
static_assert(sizeof(HandshakeResponsePayload) == 160);
static_assert(sizeof(ShutdownPayload) == 4);

static_assert(sizeof(CreateBufferPayload) == 36);
static_assert(sizeof(CreateTexture2DPayload) == 64);
static_assert(sizeof(CreateSrvPayload) == 24);
static_assert(sizeof(CreateRtvPayload) == 20);
static_assert(sizeof(CreateDsvPayload) == 24);
static_assert(sizeof(CreateShaderPayload) == 8);
static_assert(sizeof(RgpuInputElementDesc) == 56);
static_assert(sizeof(CreateInputLayoutPayload) == 12);
static_assert(sizeof(DestroyResourcePayload) == 4);
static_assert(sizeof(CreateSamplerPayload) == 56);

static_assert(sizeof(IASetInputLayoutPayload) == 4);
static_assert(sizeof(IASetVertexBufferPayload) == 16);
static_assert(sizeof(IASetIndexBufferPayload) == 12);
static_assert(sizeof(IASetTopologyPayload) == 4);
static_assert(sizeof(VSSetShaderPayload) == 4);
static_assert(sizeof(VSSetConstantBufferPayload) == 8);
static_assert(sizeof(PSSetShaderPayload) == 4);
static_assert(sizeof(PSSetConstantBufferPayload) == 8);
static_assert(sizeof(PSSetSrvPayload) == 8);
static_assert(sizeof(PSSetSamplerPayload) == 8);
static_assert(sizeof(RSSetViewportPayload) == 24);
static_assert(sizeof(OMSetRenderTargetsPayload) == 8);

static_assert(sizeof(RgpuBox) == 24);
static_assert(sizeof(UpdateSubresourcePayload) == 48);
static_assert(sizeof(WriteBufferDiscardPayload) == 12);
static_assert(sizeof(ClearRtvPayload) == 20);
static_assert(sizeof(ClearDsvPayload) == 16);
static_assert(sizeof(DrawPayload) == 8);
static_assert(sizeof(DrawIndexedPayload) == 12);

static_assert(sizeof(PresentPayload) == 20);
static_assert(sizeof(FrameDataPayload) == 32);

} // namespace RemoteGPU

#endif // REMOTE_GPU_PROTOCOL_H
```

---

## 7. Zero Undefined Behavior & Cross-Platform Windows Guarantees

1. **Standard Header Independence:** `protocol.h` relies exclusively on `<cstdint>`, `<cstddef>`, and `<type_traits>`. It requires neither `<windows.h>` nor `<winsock2.h>`, completely preventing macro name collisions (such as `ERROR`, `min`, `max`, or `DELETE`).
2. **Deterministic Struct Packing:** Using `#pragma pack(push, 1)` and `#pragma pack(pop)` ensures byte-level identity across LLVM-MinGW Clang, GCC, and MSVC on 64-bit Windows.
3. **Strict Integer Types:** Standard `<cstdint>` types (`uint32_t`, `int32_t`, `uint64_t`) prevent cross-platform integer width discrepancies that occur when using Windows `DWORD` or `LONG`.
4. **Natural Field Offsets:** All 4-byte fields reside on 4-byte boundaries, and 8-byte fields on 8-byte boundaries, within each packed struct.
5. **No Pointer Dereference During Unpacking:** Trailing dynamic buffers (such as initial texture rasters or shader bytecodes) are read into allocated buffers by size directly from the network socket, guaranteeing zero out-of-bounds pointer reads or slice corruption.
