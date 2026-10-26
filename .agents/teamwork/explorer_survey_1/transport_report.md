# Remote Direct3D Command Stream Network Protocol & Transport Specification (R2)

**Author**: Protocol Transport Explorer (`explorer_survey_1`)  
**Date**: 2026-10-26  
**Status**: Complete Technical Specification  
**Target Milestone**: Phase 0 Specification Extraction -> Phase 1 Project Architecture  

---

## 1. Executive Summary & Architectural Overview

The Remote GPU Offloading system enables a client Windows application to issue standard Direct3D 11 graphics API calls that are intercepted, encoded into a high-efficiency binary command stream, transmitted over Winsock2 TCP sockets to a remote rendering server equipped with a dedicated GPU, executed on hardware offscreen render targets, and streamed back as completed framebuffers to the client swapchain for presentation.

This document establishes the authoritative network protocol and transport architecture (**Requirement R2**).

### Core Architectural Decisions:
1. **Transport Protocol**: **TCP (Transmission Control Protocol)** via Windows Winsock2 (`WSASocket` / `socket`).
   - *Rationale*: Graphics state machine commands (shader uploads, buffer creation, pipeline configuration, draw calls) are strictly ordered and loss-intolerant. A single dropped packet causes state desynchronization, corrupted geometry, or GPU hangs. TCP guarantees byte-stream reliability and in-order delivery.
   - *Latency Optimization*: Nagle's algorithm is explicitly disabled via `TCP_NODELAY = 1`, eliminating delayed ACK latency penalties (20ms–200ms).
   - *Throughput Optimization*: Socket send/receive buffers are expanded to 16 MB (`SO_SNDBUF`, `SO_RCVBUF`) to handle multi-megabyte uncompressed frame transfers without TCP window starvation.
   - *Loopback Fast Path*: `SIO_LOOPBACK_FAST_PATH` is enabled via `WSAIoctl` for 127.0.0.1 automated testing, bypassing the standard TCP/IP stack layers for near-zero IPC latency.
2. **Framing Design**: Fixed 16-byte packet header (`PacketHeader`) preceding a variable-length binary payload.
   - 4-byte Magic Number (`0x52475055` / `'RGPU'`).
   - 2-byte Command Opcode (`uint16_t`).
   - 2-byte Protocol Flags (`uint16_t`).
   - 4-byte Monotonic Sequence ID (`uint32_t`).
   - 4-byte Payload Length (`uint32_t`).
3. **Asynchronous Client Resource Allocation**:
   - The client interception layer generates monotonic virtual resource IDs (`uint32_t`) locally.
   - Resource creation commands are pipelined asynchronously (zero client-server round-trips during initialization or draw setups).
4. **Stream Demultiplexing & Framing**:
   - TCP fragmentation and stream coalescing are handled via an exact-byte receiver (`RecvExact`) and a continuous memory ring accumulator.
5. **Flow Control & Frame Pacing**:
   - Natural backpressure is maintained through the `CMD_PRESENT` / `CMD_FRAME_DATA` handshake: the client requests frame completion during `IDXGISwapChain::Present()`, ensuring the command pipeline does not overflow network buffers.

---

## 2. Packet Framing & Wire Specification

### 2.1 Packet Header Layout

Every network packet transmitted over the socket begins with a fixed 16-byte header, strictly packed to 1-byte alignment (`#pragma pack(push, 1)`):

```cpp
#pragma pack(push, 1)

struct PacketHeader {
    uint32_t magic;          // Fixed identifier: 0x52475055 ('R','G','P','U')
    uint16_t opcode;         // Command Opcode (OpcodeType)
    uint16_t flags;          // Command / transport modifier flags
    uint32_t sequence_id;    // Monotonically increasing packet sequence counter
    uint32_t payload_length; // Length in bytes of the following payload (0 if no payload)
};

#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 16, "PacketHeader must be exactly 16 bytes");
```

### 2.2 Wire Field Definitions

| Field | Type | Size | Description |
|---|---|---|---|
| `magic` | `uint32_t` | 4 bytes | Protocol synchronization constant `0x52475055` (ASCII `'R'`, `'G'`, `'P'`, `'U'`). Enables fast stream verification and immediate detection of stream misalignment. |
| `opcode` | `uint16_t` | 2 bytes | Identifies the Direct3D or lifecycle command to execute (see Section 3). Supports up to 65,535 distinct commands. |
| `flags` | `uint16_t` | 2 bytes | Bitmask modifying command behavior and transport handling (see Section 2.3). |
| `sequence_id` | `uint32_t` | 4 bytes | Monotonically increasing sequence number initialized at 1 upon handshake. Used for telemetry, packet ordering verification, and diagnostic tracing. |
| `payload_length` | `uint32_t` | 4 bytes | Byte count of the variable data directly succeeding this header. Maximum payload size: 4,294,967,295 bytes (~4 GB), accommodating 4K uncompressed framebuffers. |

### 2.3 Protocol Flags (`PacketFlags`)

```cpp
enum PacketFlags : uint16_t {
    FLAG_NONE               = 0x0000,
    FLAG_SYNC               = 0x0001, // Client requests server to flush GPU commands before proceeding
    FLAG_RESPONSE_REQUIRED  = 0x0002, // Client requires an explicit acknowledgment or response packet
    FLAG_COMPRESSED         = 0x0004, // Payload is compressed (e.g., LZ4 or run-length)
    FLAG_ONEWAY             = 0x0008, // Fire-and-forget; server processes asynchronously without ack
    FLAG_END_OF_FRAME       = 0x0010, // Indicates all draw commands for the active frame are queued
    FLAG_HEARTBEAT          = 0x0020  // Keep-alive heartbeat message
};
```

### 2.4 Endianness & Memory Guarantees
- Both client and server execute on x86-64 Windows architectures.
- All multi-byte integer and floating-point fields are represented in **Little-Endian** format.
- Structures avoid compiler-specific padding through explicit `#pragma pack(push, 1)` and 4-byte/8-byte member alignment.

---

## 3. Complete Command Opcode Taxonomy & Payload Structures

The opcodes cover the full Direct3D 11 graphics lifecycle required to create devices, allocate buffers and textures, compile and bind shaders, configure the input assembler and rasterizer, execute draw calls, and synchronize presentation.

```cpp
enum Opcode : uint16_t {
    // 0x0001 - 0x001F: Session & Connection Lifecycle
    CMD_HANDSHAKE_REQ               = 0x0001,
    CMD_HANDSHAKE_RESP              = 0x0002,
    CMD_INIT_DEVICE                 = 0x0003,
    CMD_INIT_DEVICE_RESP            = 0x0004,
    CMD_PING                        = 0x0005,
    CMD_PONG                        = 0x0006,
    CMD_SHUTDOWN                    = 0x0007,
    CMD_SHUTDOWN_ACK                = 0x0008,
    CMD_RESET_SESSION               = 0x0009,

    // 0x0020 - 0x005F: Resource Allocation & Destruction
    CMD_CREATE_BUFFER               = 0x0020,
    CMD_CREATE_TEXTURE2D            = 0x0021,
    CMD_CREATE_SHADER_RESOURCE_VIEW = 0x0022,
    CMD_CREATE_RENDER_TARGET_VIEW   = 0x0023,
    CMD_CREATE_DEPTH_STENCIL_VIEW   = 0x0024,
    CMD_CREATE_SAMPLER_STATE        = 0x0025,
    CMD_CREATE_RASTERIZER_STATE     = 0x0026,
    CMD_CREATE_DEPTH_STENCIL_STATE  = 0x0027,
    CMD_CREATE_BLEND_STATE          = 0x0028,
    CMD_RELEASE_RESOURCE            = 0x002F,

    // 0x0060 - 0x007F: Shader Pipeline Creation
    CMD_CREATE_VERTEX_SHADER        = 0x0060,
    CMD_CREATE_PIXEL_SHADER         = 0x0061,
    CMD_CREATE_INPUT_LAYOUT         = 0x0062,

    // 0x0080 - 0x009F: Input Assembler (IA) Stage Setup
    CMD_IA_SET_INPUT_LAYOUT         = 0x0080,
    CMD_IA_SET_VERTEX_BUFFERS       = 0x0081,
    CMD_IA_SET_INDEX_BUFFER         = 0x0082,
    CMD_IA_SET_PRIMITIVE_TOPOLOGY   = 0x0083,

    // 0x00A0 - 0x00CF: Shader Stages & Resource Binding
    CMD_VS_SET_SHADER               = 0x00A0,
    CMD_VS_SET_CONSTANT_BUFFERS     = 0x00A1,
    CMD_VS_SET_SAMPLERS             = 0x00A2,
    CMD_VS_SET_SHADER_RESOURCES     = 0x00A3,

    CMD_PS_SET_SHADER               = 0x00B0,
    CMD_PS_SET_CONSTANT_BUFFERS     = 0x00B1,
    CMD_PS_SET_SAMPLERS             = 0x00B2,
    CMD_PS_SET_SHADER_RESOURCES     = 0x00B3,

    // 0x00D0 - 0x00EF: Rasterizer & Output Merger Setup
    CMD_RS_SET_VIEWPORTS            = 0x00D0,
    CMD_RS_SET_SCISSOR_RECTS        = 0x00D1,
    CMD_RS_SET_STATE                = 0x00D2,
    CMD_OM_SET_RENDER_TARGETS       = 0x00E0,
    CMD_OM_SET_DEPTH_STENCIL_STATE  = 0x00E1,
    CMD_OM_SET_BLEND_STATE          = 0x00E2,

    // 0x00F0 - 0x010F: Resource Updates & Memory Transfers
    CMD_UPDATE_SUBRESOURCE          = 0x00F0,
    CMD_MAP_BUFFER                  = 0x00F1,
    CMD_UNMAP_BUFFER                = 0x00F2,
    CMD_WRITE_BUFFER_DISCARD        = 0x00F3,

    // 0x0110 - 0x012F: Clearing & Draw Calls
    CMD_CLEAR_RENDER_TARGET_VIEW    = 0x0110,
    CMD_CLEAR_DEPTH_STENCIL_VIEW    = 0x0111,
    CMD_DRAW                        = 0x0120,
    CMD_DRAW_INDEXED                = 0x0121,
    CMD_DRAW_INSTANCED              = 0x0122,
    CMD_DRAW_INDEXED_INSTANCED      = 0x0123,

    // 0x0130 - 0x014F: Presentation & Frame Streaming
    CMD_PRESENT                     = 0x0130,
    CMD_FRAME_DATA                  = 0x0131,
    CMD_FRAME_ACK                   = 0x0132,

    // 0x0150 - 0x017F: Diagnostic & Error Management
    CMD_ERROR                       = 0x0150,
    CMD_DEVICE_LOST                 = 0x0151
};
```

---

### 3.1 Detailed Command Payload Structures

All payloads are serialized immediately following `PacketHeader`.

#### 3.1.1 Session Handshake

```cpp
#pragma pack(push, 1)

// CMD_HANDSHAKE_REQ (Client -> Server)
struct HandshakeReqPayload {
    uint32_t protocol_version;   // 0x00010000 (v1.0)
    uint32_t client_flags;       // Feature flags
    uint32_t client_width;       // Client window width (e.g., 1280)
    uint32_t client_height;      // Client window height (e.g., 720)
    uint32_t preferred_format;   // DXGI_FORMAT_R8G8B8A8_UNORM (28)
};

// CMD_HANDSHAKE_RESP (Server -> Client)
struct HandshakeRespPayload {
    uint32_t protocol_version;   // Supported version
    uint32_t status_code;        // 0: OK, 1: VersionMismatch, 2: GpuInitFailed, 3: FormatUnsupported
    uint32_t feature_level;      // D3D_FEATURE_LEVEL_11_0 (0xb000)
    uint32_t frame_width;        // Confirmed render target width
    uint32_t frame_height;       // Confirmed render target height
    uint32_t frame_format;       // Confirmed DXGI_FORMAT
    uint64_t dedicated_vram;     // Server GPU dedicated video memory in bytes
    char adapter_desc[128];      // GPU description (e.g., "Intel(R) UHD Graphics 620")
};

#pragma pack(pop)
```

#### 3.1.2 Buffer Creation (`CMD_CREATE_BUFFER`)

Direct3D vertex buffers, index buffers, and constant buffers are created with this command:

```cpp
#pragma pack(push, 1)

struct CreateBufferPayload {
    uint32_t resource_id;         // Monotonic ID generated by client
    uint32_t byte_width;          // Size of buffer in bytes
    uint32_t usage;               // D3D11_USAGE (DEFAULT=0, IMMUTABLE=1, DYNAMIC=2, STAGING=3)
    uint32_t bind_flags;          // D3D11_BIND_FLAG bitmask (VERTEX=1, INDEX=2, CONSTANT=4, etc.)
    uint32_t cpu_access_flags;    // D3D11_CPU_ACCESS_FLAG (WRITE=0x10000, READ=0x20000)
    uint32_t misc_flags;          // D3D11_RESOURCE_MISC_FLAG
    uint32_t structure_byte_stride; // Stride for structured buffers
    uint32_t has_initial_data;    // 1 if initial data is appended, 0 otherwise
    uint32_t initial_data_size;   // Size of initial data in trailing bytes
    // Followed immediately by: uint8_t initial_data[initial_data_size]
};

#pragma pack(pop)
```

#### 3.1.3 Texture2D Creation (`CMD_CREATE_TEXTURE2D`)

```cpp
#pragma pack(push, 1)

struct CreateTexture2DPayload {
    uint32_t resource_id;         // Monotonic texture resource ID
    uint32_t width;               // Texture width in pixels
    uint32_t height;              // Texture height in pixels
    uint32_t mip_levels;          // Number of mip levels
    uint32_t array_size;           // Texture array slice count
    uint32_t format;              // DXGI_FORMAT enum
    uint32_t sample_count;        // SampleDesc.Count (usually 1)
    uint32_t sample_quality;      // SampleDesc.Quality (usually 0)
    uint32_t usage;               // D3D11_USAGE
    uint32_t bind_flags;          // D3D11_BIND_FLAG (e.g. SHADER_RESOURCE = 8)
    uint32_t cpu_access_flags;    // D3D11_CPU_ACCESS_FLAG
    uint32_t misc_flags;          // D3D11_RESOURCE_MISC_FLAG
    uint32_t has_initial_data;    // 1 if subresource data follows, 0 otherwise
    uint32_t sys_mem_pitch;       // Pitch in bytes for initial data
    uint32_t sys_mem_slice_pitch; // Slice pitch
    uint32_t initial_data_size;   // Byte count of initial pixel data
    // Followed immediately by: uint8_t initial_data[initial_data_size]
};

#pragma pack(pop)
```

#### 3.1.4 Views & States

```cpp
#pragma pack(push, 1)

struct CreateShaderResourceViewPayload {
    uint32_t srv_id;              // Monotonic ID for SRV
    uint32_t resource_id;         // Target texture/buffer ID
    uint32_t format;              // DXGI_FORMAT
    uint32_t view_dimension;      // D3D11_SRV_DIMENSION (TEXTURE2D = 4)
    uint32_t most_detailed_mip;
    uint32_t mip_levels;
};

struct CreateRenderTargetViewPayload {
    uint32_t rtv_id;              // Monotonic ID for RTV
    uint32_t resource_id;         // Target texture ID
    uint32_t format;              // DXGI_FORMAT
    uint32_t view_dimension;      // D3D11_RTV_DIMENSION (TEXTURE2D = 4)
    uint32_t mip_slice;
};

struct CreateDepthStencilViewPayload {
    uint32_t dsv_id;              // Monotonic ID for DSV
    uint32_t resource_id;         // Target depth texture ID
    uint32_t format;              // DXGI_FORMAT
    uint32_t view_dimension;      // D3D11_DSV_DIMENSION
    uint32_t flags;
};

struct CreateSamplerStatePayload {
    uint32_t sampler_id;
    uint32_t filter;              // D3D11_FILTER (e.g. MIN_MAG_MIP_LINEAR = 0x15)
    uint32_t address_u;           // D3D11_TEXTURE_ADDRESS_MODE (CLAMP = 3, WRAP = 1)
    uint32_t address_v;
    uint32_t address_w;
    float mip_lod_bias;
    uint32_t max_anisotropy;
    uint32_t comparison_func;
    float border_color[4];
    float min_lod;
    float max_lod;
};

struct CreateRasterizerStatePayload {
    uint32_t rasterizer_id;
    uint32_t fill_mode;           // D3D11_FILL_MODE (SOLID = 3, WIREFRAME = 2)
    uint32_t cull_mode;           // D3D11_CULL_MODE (NONE = 1, FRONT = 2, BACK = 3)
    int32_t front_counter_clockwise;
    int32_t depth_bias;
    float depth_bias_clamp;
    float slope_scaled_depth_bias;
    int32_t depth_clip_enable;
    int32_t scissor_enable;
    int32_t multisample_enable;
    int32_t antialiased_line_enable;
};

struct ReleaseResourcePayload {
    uint32_t resource_id;         // ID of buffer, texture, view, or shader to release
};

#pragma pack(pop)
```

#### 3.1.5 Shaders & Input Layout

```cpp
#pragma pack(push, 1)

// CMD_CREATE_VERTEX_SHADER & CMD_CREATE_PIXEL_SHADER
struct CreateShaderPayload {
    uint32_t shader_id;           // Monotonic ID for shader
    uint32_t bytecode_length;     // Bytecode size in bytes
    // Followed immediately by: uint8_t bytecode[bytecode_length]
};

// Input element descriptor serialized cleanly with zero pointer dependencies
struct SerializedInputElementDesc {
    char semantic_name[32];       // e.g. "POSITION", "TEXCOORD", "NORMAL", "COLOR"
    uint32_t semantic_index;      // 0, 1, ...
    uint32_t format;              // DXGI_FORMAT enum (e.g. DXGI_FORMAT_R32G32B32_FLOAT = 6)
    uint32_t input_slot;          // Slot 0..15
    uint32_t aligned_byte_offset; // Byte offset or D3D11_APPEND_ALIGNED_ELEMENT (0xffffffff)
    uint32_t input_slot_class;    // D3D11_INPUT_PER_VERTEX_DATA = 0
    uint32_t instance_data_step;  // Step rate
};

// CMD_CREATE_INPUT_LAYOUT
struct CreateInputLayoutPayload {
    uint32_t layout_id;           // Monotonic ID for input layout
    uint32_t num_elements;        // Number of SerializedInputElementDesc structs
    uint32_t bytecode_length;     // Shader bytecode length for signature validation
    // Followed immediately by:
    // SerializedInputElementDesc elements[num_elements]
    // uint8_t shader_bytecode[bytecode_length]
};

#pragma pack(pop)
```

#### 3.1.6 Pipeline Bindings & Input Assembler

```cpp
#pragma pack(push, 1)

// CMD_IA_SET_INPUT_LAYOUT
struct IASetInputLayoutPayload {
    uint32_t layout_id;           // 0 for null, or valid layout ID
};

// CMD_IA_SET_VERTEX_BUFFERS
struct IASetVertexBuffersPayload {
    uint32_t start_slot;
    uint32_t num_buffers;
    // Followed by parallel arrays:
    // uint32_t buffer_ids[num_buffers]
    // uint32_t strides[num_buffers]
    // uint32_t offsets[num_buffers]
};

// CMD_IA_SET_INDEX_BUFFER
struct IASetIndexBufferPayload {
    uint32_t buffer_id;           // Index buffer ID
    uint32_t format;              // DXGI_FORMAT_R16_UINT (57) or DXGI_FORMAT_R32_UINT (42)
    uint32_t offset;              // Offset in bytes
};

// CMD_IA_SET_PRIMITIVE_TOPOLOGY
struct IASetPrimitiveTopologyPayload {
    uint32_t topology;            // D3D11_PRIMITIVE_TOPOLOGY (TRIANGLELIST = 4)
};

// CMD_VS_SET_SHADER / CMD_PS_SET_SHADER
struct SetShaderPayload {
    uint32_t shader_id;           // 0 for unbind, or valid shader ID
};

// CMD_VS_SET_CONSTANT_BUFFERS / CMD_PS_SET_CONSTANT_BUFFERS
struct SetConstantBuffersPayload {
    uint32_t start_slot;
    uint32_t num_buffers;
    // Followed by: uint32_t buffer_ids[num_buffers]
};

// CMD_PS_SET_SHADER_RESOURCES
struct SetShaderResourcesPayload {
    uint32_t start_slot;
    uint32_t num_views;
    // Followed by: uint32_t srv_ids[num_views]
};

// CMD_PS_SET_SAMPLERS
struct SetSamplersPayload {
    uint32_t start_slot;
    uint32_t num_samplers;
    // Followed by: uint32_t sampler_ids[num_samplers]
};

#pragma pack(pop)
```

#### 3.1.7 Viewports, Render Targets, and Output Merger

```cpp
#pragma pack(push, 1)

struct SerializedViewport {
    float top_left_x;
    float top_left_y;
    float width;
    float height;
    float min_depth;
    float max_depth;
};

// CMD_RS_SET_VIEWPORTS
struct RSSetViewportsPayload {
    uint32_t num_viewports;
    // Followed by: SerializedViewport viewports[num_viewports]
};

// CMD_OM_SET_RENDER_TARGETS
struct OMSetRenderTargetsPayload {
    uint32_t num_views;
    uint32_t dsv_id;              // 0 if none, or DepthStencilView ID
    // Followed by: uint32_t rtv_ids[num_views]
};

#pragma pack(pop)
```

#### 3.1.8 Resource Updates & Data Streaming

```cpp
#pragma pack(push, 1)

// Box descriptor for partial resource updates
struct SerializedBox {
    uint32_t left;
    uint32_t top;
    uint32_t front;
    uint32_t right;
    uint32_t bottom;
    uint32_t back;
};

// CMD_UPDATE_SUBRESOURCE
struct UpdateSubresourcePayload {
    uint32_t resource_id;
    uint32_t dst_subresource;
    uint32_t has_box;             // 1 if custom box provided, 0 for entire resource
    SerializedBox box;
    uint32_t src_row_pitch;
    uint32_t src_depth_pitch;
    uint32_t data_size;
    // Followed immediately by: uint8_t data[data_size]
};

// CMD_WRITE_BUFFER_DISCARD (High-performance dynamic buffer update)
struct WriteBufferDiscardPayload {
    uint32_t buffer_id;
    uint32_t data_size;
    // Followed immediately by: uint8_t data[data_size]
};

#pragma pack(pop)
```

#### 3.1.9 Draw Calls & Clears

```cpp
#pragma pack(push, 1)

// CMD_CLEAR_RENDER_TARGET_VIEW
struct ClearRenderTargetViewPayload {
    uint32_t rtv_id;
    float color_rgba[4];
};

// CMD_CLEAR_DEPTH_STENCIL_VIEW
struct ClearDepthStencilViewPayload {
    uint32_t dsv_id;
    uint32_t clear_flags;         // D3D11_CLEAR_DEPTH (1) | D3D11_CLEAR_STENCIL (2)
    float depth;                  // Typically 1.0f
    uint8_t stencil;              // Typically 0
    uint8_t padding[3];           // Explicit 4-byte struct alignment
};

// CMD_DRAW
struct DrawPayload {
    uint32_t vertex_count;
    uint32_t start_vertex_location;
};

// CMD_DRAW_INDEXED
struct DrawIndexedPayload {
    uint32_t index_count;
    uint32_t start_index_location;
    int32_t base_vertex_location;
};

#pragma pack(pop)
```

#### 3.1.10 Presentation & Framebuffer Return Stream

```cpp
#pragma pack(push, 1)

// CMD_PRESENT (Client -> Server: triggers render completion and frame capture)
struct PresentPayload {
    uint32_t sync_interval;       // 0 for unthrottled, 1 for VSync
    uint32_t present_flags;       // DXGI_PRESENT flags
    uint64_t frame_number;        // Monotonic client frame counter
};

// CMD_FRAME_DATA (Server -> Client: rendered framebuffer pixel payload)
struct FrameDataPayload {
    uint64_t frame_number;        // Matching frame number
    uint32_t width;               // Frame width in pixels
    uint32_t height;              // Frame height in pixels
    uint32_t format;              // DXGI_FORMAT (DXGI_FORMAT_R8G8B8A8_UNORM = 28)
    uint32_t row_pitch;           // Stride in bytes (width * 4)
    uint32_t data_size;           // Total byte size of uncompressed pixel array
    uint32_t compression_type;    // 0: Uncompressed RGBA32, 1: LZ4
    // Followed immediately by: uint8_t pixel_bytes[data_size]
};

// CMD_FRAME_ACK (Client -> Server: optional frame acknowledgment)
struct FrameAckPayload {
    uint64_t frame_number;
    uint64_t client_timestamp_us;
};

#pragma pack(pop)
```

---

## 4. Binary Serialization of Variable-Sized Buffers

### 4.1 Asynchronous Monotonic Resource IDs

Traditional client-server graphics architectures often suffer from round-trip latency when creating resources:
```
Client: CreateBuffer() ---> Server creates buffer ---> returns ServerID ---> Client receives ServerID
```
In our protocol, the client **locally assigns** monotonic identifiers (`uint32_t resource_id = ++next_id;`) and immediately associates the local COM proxy object with this ID. The `CMD_CREATE_BUFFER` packet is sent asynchronously down the TCP socket. The server registers the real `ID3D11Buffer*` inside a flat hash table or direct lookup array (`std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11Resource>>`).

**Advantage**:
- **Zero round-trip latency during initialization and rendering setup**. The client submits mesh data, vertex buffers, index buffers, shader binaries, and textures without waiting for server responses.

### 4.2 Inlined Variable Payloads

To maximize throughput and prevent serialization overhead, payloads use an **inlined dynamic structure pattern**:
```
+-------------------+----------------------------+-----------------------------+
| PacketHeader (16B)| Fixed Payload Struct (N B) | Dynamic Binary Data (M B)   |
+-------------------+----------------------------+-----------------------------+
```
`PacketHeader::payload_length = sizeof(FixedPayloadStruct) + M`.

#### Serialization Implementation (Client):
```cpp
void CommandWriter::SendCreateBuffer(uint32_t id, const D3D11_BUFFER_DESC& desc, const void* initData) {
    CreateBufferPayload payload{};
    payload.resource_id = id;
    payload.byte_width = desc.ByteWidth;
    payload.usage = desc.Usage;
    payload.bind_flags = desc.BindFlags;
    payload.cpu_access_flags = desc.CPUAccessFlags;
    payload.misc_flags = desc.MiscFlags;
    payload.structure_byte_stride = desc.StructureByteStride;
    payload.has_initial_data = (initData != nullptr) ? 1 : 0;
    payload.initial_data_size = (initData != nullptr) ? desc.ByteWidth : 0;

    PacketHeader hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.opcode = CMD_CREATE_BUFFER;
    hdr.flags = FLAG_NONE;
    hdr.sequence_id = ++m_sequenceId;
    hdr.payload_length = sizeof(payload) + payload.initial_data_size;

    // Use scatter-gather (WSASend) or contiguous serialization
    WSABUF buffers[3];
    buffers[0].buf = (char*)&hdr;
    buffers[0].len = sizeof(hdr);
    buffers[1].buf = (char*)&payload;
    buffers[1].len = sizeof(payload);

    DWORD bytesSent = 0;
    if (payload.has_initial_data) {
        buffers[2].buf = (char*)initData;
        buffers[2].len = payload.initial_data_size;
        WSASend(m_socket, buffers, 3, &bytesSent, 0, nullptr, nullptr);
    } else {
        WSASend(m_socket, buffers, 2, &bytesSent, 0, nullptr, nullptr);
    }
}
```
Using `WSASend` with multiple `WSABUF` segments performs **zero-copy kernel transmission**, eliminating the need to allocate intermediate temporary memory buffers or copy multi-megabyte vertex buffers in user-space!

---

## 5. Winsock2 Transport Architecture & Implementation

### 5.1 Protocol Selection: Why TCP is Required for Commands

| Metric | TCP | UDP |
|---|---|---|
| **Packet Delivery** | Guaranteed in-order, lossless | Unreliable, packet drops |
| **Pipeline Safety** | 100% reliable state machine | Dropped Draw or Buffer packet ruins frame state |
| **Congestion Control** | Built-in windowing | Requires custom flow control & ARQ |
| **Overhead** | Minimal with `TCP_NODELAY` | High development complexity for reliable layer |
| **Loopback Performance** | ~0.05ms with `SIO_LOOPBACK_FAST_PATH` | Negligible advantage over localhost |

**Conclusion**: For remote graphics command streams, TCP is vastly superior. UDP without an exhaustive ARQ layer (such as QUIC or reliable UDP) would cause fatal state desynchronization. With `TCP_NODELAY`, TCP delivers sub-millisecond command submission.

### 5.2 Winsock2 Socket Initialization & Tuning

The following socket options must be applied immediately after socket creation on both client and server:

```cpp
bool ConfigureSocketForOffloading(SOCKET sock) {
    // 1. Disable Nagle's Algorithm (CRITICAL for low-latency command streams)
    int nodelay = 1;
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay)) != 0) {
        return false;
    }

    // 2. Expand Socket Send & Receive Buffers to 16 MB
    // (Prevents stalling when streaming 1080p uncompressed RGBA frames: 1920*1080*4 = 8.29 MB)
    int bufSize = 16 * 1024 * 1024; // 16 MB
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char*)&bufSize, sizeof(bufSize));
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&bufSize, sizeof(bufSize));

    // 3. Set Socket Timeouts (5 seconds to detect hung or dropped connections)
    DWORD timeoutMs = 5000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

    // 4. Windows Loopback Fast Path (Windows 8+)
    #ifndef SIO_LOOPBACK_FAST_PATH
    #define SIO_LOOPBACK_FAST_PATH _WSAIOW(IOC_VENDOR, 16)
    #endif
    int optionVal = 1;
    DWORD bytesReturned = 0;
    WSAIoctl(sock, SIO_LOOPBACK_FAST_PATH, &optionVal, sizeof(optionVal), nullptr, 0, &bytesReturned, nullptr, nullptr);

    return true;
}
```

### 5.3 Handling TCP Fragmentation and Stream Coalescing

In TCP, `recv()` returns arbitrary chunk sizes. A single `recv()` call may return:
1. Partial header (e.g. 7 bytes instead of 16).
2. Complete header + partial payload.
3. Multiple complete packets coalesced together.

To handle this cleanly, the transport layer implements two distinct, robust patterns:

#### Pattern A: Synchronous Blocking `RecvExact` (Preferred for Dedicated Receiver Thread)

```cpp
// Receives exactly `totalBytes` into `destBuffer`.
// Returns true on success, false on socket error or connection closure.
bool RecvExact(SOCKET sock, void* destBuffer, size_t totalBytes) {
    size_t bytesReceived = 0;
    char* target = static_cast<char*>(destBuffer);

    while (bytesReceived < totalBytes) {
        int result = recv(sock, target + bytesReceived, static_cast<int>(totalBytes - bytesReceived), 0);
        if (result <= 0) {
            // result == 0 indicates graceful close by peer
            // result == SOCKET_ERROR indicates network error / timeout
            return false;
        }
        bytesReceived += static_cast<size_t>(result);
    }
    return true;
}

// Packet receiver loop
bool ReceiveNextPacket(SOCKET sock, PacketHeader& outHeader, std::vector<uint8_t>& outPayload) {
    // 1. Read exact 16-byte header
    if (!RecvExact(sock, &outHeader, sizeof(PacketHeader))) {
        return false;
    }

    // 2. Verify Protocol Magic identifier
    if (outHeader.magic != PROTOCOL_MAGIC) {
        // Stream corrupted or desynchronized!
        return false;
    }

    // 3. Read variable payload
    outPayload.resize(outHeader.payload_length);
    if (outHeader.payload_length > 0) {
        if (!RecvExact(sock, outPayload.data(), outHeader.payload_length)) {
            return false;
        }
    }

    return true;
}
```

#### Pattern B: Continuous Memory Ring / Accumulation Buffer (for Non-Blocking I/O)

For non-blocking architectures with `select()`:
```cpp
class StreamAccumulator {
public:
    StreamAccumulator(size_t initialCapacity = 4 * 1024 * 1024) {
        m_buffer.resize(initialCapacity);
    }

    // Append raw bytes received from non-blocking recv()
    void Append(const void* data, size_t len) {
        if (m_writeOffset + len > m_buffer.size()) {
            Compact();
            if (m_writeOffset + len > m_buffer.size()) {
                m_buffer.resize((m_writeOffset + len) * 2);
            }
        }
        memcpy(m_buffer.data() + m_writeOffset, data, len);
        m_writeOffset += len;
    }

    // Attempt to extract the next complete packet from the buffer
    bool TryPopPacket(PacketHeader& outHeader, std::vector<uint8_t>& outPayload) {
        size_t available = m_writeOffset - m_readOffset;
        if (available < sizeof(PacketHeader)) {
            return false; // Header incomplete
        }

        const PacketHeader* hdr = reinterpret_cast<const PacketHeader*>(m_buffer.data() + m_readOffset);
        if (hdr->magic != PROTOCOL_MAGIC) {
            // Corrupted stream: search forward for magic byte synchronization
            Resynchronize();
            return false;
        }

        size_t totalPacketSize = sizeof(PacketHeader) + hdr->payload_length;
        if (available < totalPacketSize) {
            return false; // Payload incomplete, wait for more bytes
        }

        outHeader = *hdr;
        outPayload.resize(hdr->payload_length);
        if (hdr->payload_length > 0) {
            memcpy(outPayload.data(), m_buffer.data() + m_readOffset + sizeof(PacketHeader), hdr->payload_length);
        }

        m_readOffset += totalPacketSize;
        if (m_readOffset == m_writeOffset) {
            m_readOffset = 0;
            m_writeOffset = 0;
        }
        return true;
    }

private:
    void Compact() {
        if (m_readOffset > 0) {
            size_t remaining = m_writeOffset - m_readOffset;
            if (remaining > 0) {
                memmove(m_buffer.data(), m_buffer.data() + m_readOffset, remaining);
            }
            m_writeOffset = remaining;
            m_readOffset = 0;
        }
    }

    void Resynchronize() {
        // Scan for 0x52475055 magic marker in remaining stream
        const uint8_t* ptr = m_buffer.data() + m_readOffset + 1;
        size_t remaining = m_writeOffset - (m_readOffset + 1);
        for (size_t i = 0; i + 4 <= remaining; ++i) {
            uint32_t val;
            memcpy(&val, ptr + i, 4);
            if (val == PROTOCOL_MAGIC) {
                m_readOffset += 1 + i;
                return;
            }
        }
        // No magic found, discard corrupted buffer
        m_readOffset = 0;
        m_writeOffset = 0;
    }

    std::vector<uint8_t> m_buffer;
    size_t m_readOffset = 0;
    size_t m_writeOffset = 0;
};
```

---

## 6. Connection & Session Lifecycle

### 6.1 Handshake Sequence Diagram

```
Client (127.0.0.1 or LAN)                                Server (Dedicated GPU)
       |                                                            |
       |--------------- 1. TCP Connect (SYN, ACK) ----------------->|
       |                                                            |
       |========== ConfigureSocketForOffloading(sock) =============|
       |                                                            |
       |-- 2. CMD_HANDSHAKE_REQ (Ver: 1.0, 1280x720, RGBA8) ------->|
       |                                                            |
       |                                     [EnumAdapters: Select Dedicated GPU]
       |                                     [Create D3D11Device on Selected GPU]
       |                                     [Allocate Offscreen Framebuffers]
       |                                                            |
       |<- 3. CMD_HANDSHAKE_RESP (Status: OK, Feature: 11_0, Desc) -|
       |                                                            |
       |========== Handshake Complete: Session Active ==============|
```

### 6.2 IP Address Configuration: Loopback vs LAN

- **Loopback (`127.0.0.1`)**:
  - The client and server run on the same Windows host.
  - Sockets bind to `INADDR_LOOPBACK` (`inet_addr("127.0.0.1")`).
  - Automated tests (Requirement R5) launch server and client on `127.0.0.1:48888`.
  - With `SIO_LOOPBACK_FAST_PATH`, loopback data transfers avoid TCP/IP network driver stack traversal.
- **LAN Deployment (`192.168.x.x` / Configurable IP)**:
  - The server binds to `INADDR_ANY` (`0.0.0.0`) on a configurable port (default: `48888`).
  - The client reads the target IP from an environment variable (`REMOTE_GPU_HOST`) or configuration file (`remote_gpu.ini`).
  - Windows Defender Firewall rules must allow inbound TCP on port `48888` for the server executable.

### 6.3 Frame Pacing & Backpressure Control

A critical risk in remoted graphics is **pipeline starvation or runaway command queuing**: if the client produces 500 frames per second but the server GPU renders at 60 FPS, the TCP socket buffers would overflow, buffering hundreds of megabytes of draw commands.

**Flow Control Mechanism**:
1. When the client application calls `IDXGISwapChain::Present()`, the client sends `CMD_PRESENT`.
2. The client then **blocks** on `RecvExact` awaiting `CMD_FRAME_DATA` from the server.
3. The server executes all queued draw commands for the frame, reads back the rendered framebuffer from its offscreen staging texture, and sends `CMD_FRAME_DATA` back to the client.
4. The client receives `CMD_FRAME_DATA`, copies the pixel bytes into its local swapchain or window DC, and returns from `Present()`.
5. This 1:1 synchronization provides **automatic backpressure**: the client cannot get ahead of the server GPU rendering rate.

### 6.4 Graceful Teardown & Clean Disconnect

```
Client                                                   Server
   |                                                        |
   |-- 1. CMD_SHUTDOWN ------------------------------------>|
   |                                                        |
   |                                 [Flush GPU context]
   |                                 [Release all D3D11 resources]
   |                                 [Destroy offscreen render targets]
   |                                                        |
   |<- 2. CMD_SHUTDOWN_ACK ---------------------------------|
   |                                                        |
   |-- 3. shutdown(s, SD_BOTH)                             |-- 3. shutdown(s, SD_BOTH)
   |-- 4. closesocket(s)                                    |-- 4. closesocket(s)
   |                                                        |
```

### 6.5 Fault Detection & Error Recovery

1. **Unexpected Socket Drop**:
   - If `recv()` returns `0` or `SOCKET_ERROR` with `WSAGetLastError() == WSAECONNRESET`:
     - **Server**: Logs client disconnection, flushes and resets all tracked D3D resources in its resource map (`map.clear()`), restores default pipeline state, and loops back to `accept()` for subsequent connections. The server daemon **never crashes** upon client drop.
     - **Client**: Logs network failure, marks the virtual device as lost (`DXGI_ERROR_DEVICE_REMOVED`), preventing crashes in the calling application.
2. **Heartbeats / Ping-Pong**:
   - If no commands are transmitted for 2 seconds (e.g. idle application), the client sends `CMD_PING`. The server responds with `CMD_PONG`. If a response is not received within `timeoutMs` (5s), the connection is cleanly aborted.

---

## 7. Performance, Latency, and Bandwidth Analysis

### 7.1 Bandwidth Calculation

At 60 Frames Per Second:
1. **Command Stream (Client -> Server)**:
   - Dynamic 3D mesh rendering typically requires ~20 to 50 commands per frame:
     - Clear Render Target: 32 bytes
     - Set Constant Buffers / Matrices: ~80 bytes
     - Update Dynamic Uniform Buffer: ~128 bytes
     - Bind Vertex/Index Buffers: ~48 bytes
     - DrawIndexed: ~32 bytes
     - Present: 32 bytes
   - Total command stream bandwidth: ~5 KB to 20 KB per frame -> **0.3 MB/s to 1.2 MB/s**. This is completely negligible over modern Ethernet and localhost!
2. **Framebuffer Stream (Server -> Client)**:
   - For 1280x720 at 60 FPS uncompressed RGBA8:
     - Per frame: `1280 * 720 * 4 = 3,686,400 bytes` (~3.51 MB).
     - At 60 FPS: `3.51 MB * 60 = 210.6 MB/s` (~1.68 Gbps).
   - For 800x600 (automated test harness standard):
     - Per frame: `800 * 600 * 4 = 1,920,000 bytes` (~1.83 MB).
     - At 60 FPS: `1.83 MB * 60 = 109.8 MB/s` (~878 Mbps).
   - Over `127.0.0.1` loopback with `SIO_LOOPBACK_FAST_PATH`, memory transfer bandwidth on modern Windows exceeds **5,000 MB/s (5 GB/s)**, easily sustaining 60+ FPS without compression.
   - For 1 Gbps LAN deployments, simple Run-Length Encoding (RLE) or LZ4 compression can be toggled via `FLAG_COMPRESSED`.

---

## 8. Interface Header Specifications (`protocol.h` Prototype)

Below is the complete C++ interface header designed for implementation in `src/common/protocol.h`:

```cpp
#pragma once
#ifndef REMOTE_GPU_PROTOCOL_H
#define REMOTE_GPU_PROTOCOL_H

#include <cstdint>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace RemoteGPU {

// Synchronization Magic Identifier: 'RGPU' (0x52475055 in Little-Endian)
constexpr uint32_t PROTOCOL_MAGIC = 0x52475055;
constexpr uint32_t PROTOCOL_VERSION = 0x00010000; // v1.0
constexpr uint16_t DEFAULT_PORT = 48888;

#pragma pack(push, 1)

struct PacketHeader {
    uint32_t magic;          // PROTOCOL_MAGIC
    uint16_t opcode;         // Opcode enum
    uint16_t flags;          // PacketFlags bitmask
    uint32_t sequence_id;    // Monotonic sequence number
    uint32_t payload_length; // Size of succeeding payload
};

enum PacketFlags : uint16_t {
    FLAG_NONE              = 0x0000,
    FLAG_SYNC              = 0x0001,
    FLAG_RESPONSE_REQUIRED = 0x0002,
    FLAG_COMPRESSED        = 0x0004,
    FLAG_ONEWAY            = 0x0008,
    FLAG_END_OF_FRAME      = 0x0010,
    FLAG_HEARTBEAT         = 0x0020
};

enum Opcode : uint16_t {
    CMD_HANDSHAKE_REQ               = 0x0001,
    CMD_HANDSHAKE_RESP              = 0x0002,
    CMD_INIT_DEVICE                 = 0x0003,
    CMD_INIT_DEVICE_RESP            = 0x0004,
    CMD_PING                        = 0x0005,
    CMD_PONG                        = 0x0006,
    CMD_SHUTDOWN                    = 0x0007,
    CMD_SHUTDOWN_ACK                = 0x0008,

    CMD_CREATE_BUFFER               = 0x0020,
    CMD_CREATE_TEXTURE2D            = 0x0021,
    CMD_CREATE_SHADER_RESOURCE_VIEW = 0x0022,
    CMD_CREATE_RENDER_TARGET_VIEW   = 0x0023,
    CMD_CREATE_DEPTH_STENCIL_VIEW   = 0x0024,
    CMD_CREATE_SAMPLER_STATE        = 0x0025,
    CMD_CREATE_RASTERIZER_STATE     = 0x0026,
    CMD_RELEASE_RESOURCE            = 0x002F,

    CMD_CREATE_VERTEX_SHADER        = 0x0060,
    CMD_CREATE_PIXEL_SHADER         = 0x0061,
    CMD_CREATE_INPUT_LAYOUT         = 0x0062,

    CMD_IA_SET_INPUT_LAYOUT         = 0x0080,
    CMD_IA_SET_VERTEX_BUFFERS       = 0x0081,
    CMD_IA_SET_INDEX_BUFFER         = 0x0082,
    CMD_IA_SET_PRIMITIVE_TOPOLOGY   = 0x0083,

    CMD_VS_SET_SHADER               = 0x00A0,
    CMD_VS_SET_CONSTANT_BUFFERS     = 0x00A1,
    CMD_PS_SET_SHADER               = 0x00B0,
    CMD_PS_SET_CONSTANT_BUFFERS     = 0x00B1,
    CMD_PS_SET_SHADER_RESOURCES     = 0x00B3,
    CMD_PS_SET_SAMPLERS             = 0x00B2,

    CMD_RS_SET_VIEWPORTS            = 0x00D0,
    CMD_OM_SET_RENDER_TARGETS       = 0x00E0,

    CMD_UPDATE_SUBRESOURCE          = 0x00F0,
    CMD_WRITE_BUFFER_DISCARD        = 0x00F3,

    CMD_CLEAR_RENDER_TARGET_VIEW    = 0x0110,
    CMD_CLEAR_DEPTH_STENCIL_VIEW    = 0x0111,
    CMD_DRAW                        = 0x0120,
    CMD_DRAW_INDEXED                = 0x0121,

    CMD_PRESENT                     = 0x0130,
    CMD_FRAME_DATA                  = 0x0131,
    CMD_FRAME_ACK                   = 0x0132,

    CMD_ERROR                       = 0x0150
};

#pragma pack(pop)

} // namespace RemoteGPU

#endif // REMOTE_GPU_PROTOCOL_H
```

---

## 9. Conclusion & Recommendations for Implementer

1. **Header File Placement**: Implement the structures in `src/common/protocol.h` and helper network functions in `src/common/transport.cpp` / `src/common/transport.h`.
2. **Use Scatter-Gather (`WSASend`)**: For commands with trailing payload blobs (e.g. `CMD_CREATE_BUFFER`, `CMD_UPDATE_SUBRESOURCE`, `CMD_CREATE_VERTEX_SHADER`), pass two `WSABUF` segments to avoid memory copies.
3. **Use Dedicated Threads**:
   - **Client**: Calling application thread pushes commands; `Present()` receives frames and presents to swapchain.
   - **Server**: Network thread receives command packets via `RecvExact()` and executes them directly on the dedicated GPU `ID3D11DeviceContext`.
4. **Automated Verification Target**:
   - The transport layer can be verified independently with a standalone unit test that transmits 1,000 synthetic commands over loopback (`127.0.0.1`), verifying payload integrity and zero frame loss.
