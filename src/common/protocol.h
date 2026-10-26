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

// Synchronization Magic Identifier: ASCII 'R', 'G', 'P', 'U' (0x52475055 in Little-Endian)
inline constexpr uint32_t RGPU_MAGIC = 0x52475055;

// Major Protocol Version
inline constexpr uint32_t RGPU_PROTOCOL_VERSION = 1;

// Default TCP Service Port
inline constexpr uint16_t RGPU_DEFAULT_PORT = 48888;

enum RgpuFlags : uint16_t {
    FLAG_NONE              = 0x0000,
    FLAG_SYNC              = 0x0001, // Client requests server to flush GPU commands before returning
    FLAG_RESPONSE_REQUIRED = 0x0002, // Server must send an explicit response packet
    FLAG_COMPRESSED        = 0x0004, // Trailing payload data is compressed
    FLAG_ONEWAY            = 0x0008, // Fire-and-forget; processed asynchronously without client wait
    FLAG_END_OF_FRAME      = 0x0010, // Final command of the current frame submission batch
    FLAG_HEARTBEAT         = 0x0020  // Heartbeat keep-alive probe
};

// HandshakeRequestPayload::client_flags bits. Unknown bits are ignored by the server, so a
// client that sets none (older clients) keeps receiving uncompressed frames.
enum RgpuClientFlags : uint32_t {
    CLIENT_FLAG_NONE              = 0x0000,
    CLIENT_FLAG_FRAME_COMPRESSION = 0x0001  // Client can decode OP_FRAME_DATA sent with FLAG_COMPRESSED
};

// Frame codecs used when OP_FRAME_DATA carries FLAG_COMPRESSED.
enum RgpuFrameCodec : uint32_t {
    FRAME_CODEC_RLE_DELTA = 1  // XOR prediction + zero-run/literal tokens (see frame_codec.h)
};

enum RgpuFrameMode : uint32_t {
    FRAME_MODE_KEY   = 0,  // Each pixel predicted from its left neighbour; needs no previous frame
    FRAME_MODE_DELTA = 1   // Each pixel predicted from the previous decoded frame
};

// =============================================================================
// Direct3D 11 Command Opcodes (33 Total)
// =============================================================================

enum RgpuOpcode : uint16_t {
    // 1. Session & Lifecycle (0x0001 - 0x000F)
    OP_HANDSHAKE_REQUEST    = 0x0001, // Client -> Server: Initiate session & feature negotiation
    OP_HANDSHAKE_RESPONSE   = 0x0002, // Server -> Client: Confirm capabilities & adapter info
    OP_SHUTDOWN             = 0x0003, // Bidirectional: Graceful session termination & cleanup

    // 2. Resource Management (0x0010 - 0x002F)
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
    OP_CREATE_BLEND_STATE   = 0x001A, // Client -> Server: Create Blend State (transparency)
    OP_CREATE_DEPTH_STENCIL_STATE = 0x001B, // Client -> Server: Create Depth-Stencil State
    OP_CREATE_RASTERIZER_STATE    = 0x001C, // Client -> Server: Create Rasterizer State (culling, fill, scissor)

    // 3. Pipeline State & Binding (0x0030 - 0x004F)
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
    OP_OM_SET_BLEND_STATE   = 0x003C, // Client -> Server: Bind Blend State (+ blend factor, sample mask)
    OP_OM_SET_DEPTH_STENCIL_STATE = 0x003D, // Client -> Server: Bind Depth-Stencil State (+ stencil ref)
    OP_RS_SET_STATE         = 0x003E, // Client -> Server: Bind Rasterizer State
    OP_RS_SET_SCISSOR       = 0x003F, // Client -> Server: Set scissor rectangle

    // 4. Updates & Draw Calls (0x0050 - 0x006F)
    OP_UPDATE_SUBRESOURCE   = 0x0050, // Client -> Server: Update subresource memory
    OP_WRITE_BUFFER_DISCARD = 0x0051, // Client -> Server: High-speed dynamic buffer update
    OP_CLEAR_RTV            = 0x0052, // Client -> Server: Clear Render Target with RGBA float
    OP_CLEAR_DSV            = 0x0053, // Client -> Server: Clear Depth Stencil View
    OP_DRAW                 = 0x0054, // Client -> Server: Draw non-indexed primitives
    OP_DRAW_INDEXED         = 0x0055, // Client -> Server: Draw indexed primitives

    // 5. Presentation & Streaming (0x0070 - 0x007F)
    OP_PRESENT              = 0x0070, // Client -> Server: End-of-frame present & readback trigger
    OP_FRAME_DATA           = 0x0071, // Server -> Client: Return rendered framebuffer pixels
    OP_RESIZE_BUFFERS       = 0x0072  // Client -> Server: Recreate the host back buffer at a new size
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
    uint32_t protocol_version;   // Client protocol version (RGPU_PROTOCOL_VERSION)
    uint32_t client_flags;       // Feature request flags
    uint32_t client_width;       // Initial client window width (e.g. 1280 or 800)
    uint32_t client_height;      // Initial client window height (e.g. 720 or 600)
    uint32_t preferred_format;   // DXGI_FORMAT enum (e.g. 28 = DXGI_FORMAT_R8G8B8A8_UNORM)
};
using HandshakeRequest = HandshakeRequestPayload;

struct HandshakeResponsePayload {
    uint32_t protocol_version;   // Confirmed server protocol version
    uint32_t status_code;        // 0: OK, 1: Version Mismatch, 2: GPU Init Failed, 3: Format Unsupported
    uint32_t feature_level;      // Direct3D Feature Level (e.g. 0xB000 for D3D_FEATURE_LEVEL_11_0)
    uint32_t frame_width;        // Confirmed offscreen render target width
    uint32_t frame_height;       // Confirmed offscreen render target height
    uint32_t frame_format;       // Confirmed DXGI_FORMAT
    uint64_t dedicated_vram;     // Server GPU dedicated video memory in bytes
    char     adapter_desc[128];  // Hardware description (e.g. "Intel(R) UHD Graphics 620")
};
using HandshakeResponse = HandshakeResponsePayload;

struct ShutdownPayload {
    uint32_t exit_code;          // 0: Normal graceful disconnect, 1: Error abort
};

// =============================================================================
// Resource Creation Payloads
// =============================================================================

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
};

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
};

struct CreateSrvPayload {
    uint32_t res_id;              // Monotonic ID for this SRV
    uint32_t resource_id;         // Underlying texture/buffer res_id
    uint32_t format;              // DXGI_FORMAT enum (0 to inherit resource format)
    uint32_t view_dimension;      // D3D11_SRV_DIMENSION (4 = D3D11_SRV_DIMENSION_TEXTURE2D)
    uint32_t most_detailed_mip;   // Most detailed mip slice
    uint32_t mip_levels;          // Number of mip levels (0xFFFFFFFF for all)
};

struct CreateRtvPayload {
    uint32_t res_id;              // Monotonic ID for this RTV
    uint32_t resource_id;         // Underlying texture res_id
    uint32_t format;              // DXGI_FORMAT enum
    uint32_t view_dimension;      // D3D11_RTV_DIMENSION (4 = D3D11_RTV_DIMENSION_TEXTURE2D)
    uint32_t mip_slice;           // Mip slice index
};

struct CreateDsvPayload {
    uint32_t res_id;              // Monotonic ID for this DSV
    uint32_t resource_id;         // Underlying depth texture res_id
    uint32_t format;              // DXGI_FORMAT enum
    uint32_t view_dimension;      // D3D11_DSV_DIMENSION (3 = D3D11_DSV_DIMENSION_TEXTURE2D)
    uint32_t mip_slice;           // Mip slice index
    uint32_t flags;               // D3D11_DSV_FLAG
};

struct CreateShaderPayload {
    uint32_t res_id;              // Monotonic shader ID (vertex or pixel shader)
    uint32_t bytecode_length;     // Byte size of shader bytecode that immediately follows
};

struct RgpuInputElementDesc {
    char     semantic_name[32];   // e.g. "POSITION", "TEXCOORD", "NORMAL", "COLOR"
    uint32_t semantic_index;      // Semantic index (0, 1, ...)
    uint32_t format;              // DXGI_FORMAT enum (e.g. 6 = DXGI_FORMAT_R32G32B32_FLOAT)
    uint32_t input_slot;          // Vertex buffer input slot (0..15)
    uint32_t aligned_byte_offset; // Byte offset or 0xFFFFFFFF (D3D11_APPEND_ALIGNED_ELEMENT)
    uint32_t input_slot_class;    // 0: PER_VERTEX_DATA, 1: PER_INSTANCE_DATA
    uint32_t instance_data_step;  // Step rate (0 for vertex data)
};
using SerializedInputElementDesc = RgpuInputElementDesc;

struct CreateInputLayoutPayload {
    uint32_t res_id;              // Monotonic Input Layout ID
    uint32_t num_elements;        // Number of RgpuInputElementDesc elements
    uint32_t bytecode_length;     // Shader bytecode length for signature validation
};

struct DestroyResourcePayload {
    uint32_t res_id;              // Monotonic ID of resource to destroy
};

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

// Fixed-function state objects. Fields carry the D3D11 enum values unchanged.
struct RgpuRenderTargetBlend {
    uint32_t blend_enable;
    uint32_t src_blend;           // D3D11_BLEND (1..19)
    uint32_t dest_blend;
    uint32_t blend_op;            // D3D11_BLEND_OP (1..5)
    uint32_t src_blend_alpha;
    uint32_t dest_blend_alpha;
    uint32_t blend_op_alpha;
    uint32_t write_mask;          // D3D11_COLOR_WRITE_ENABLE bits (0..15)
};

struct CreateBlendStatePayload {
    uint32_t res_id;
    uint32_t alpha_to_coverage;
    uint32_t independent_blend;
    RgpuRenderTargetBlend render_target[8];
};

struct RgpuStencilOpDesc {
    uint32_t fail_op;             // D3D11_STENCIL_OP (1..8)
    uint32_t depth_fail_op;
    uint32_t pass_op;
    uint32_t func;                // D3D11_COMPARISON_FUNC (1..8)
};

struct CreateDepthStencilStatePayload {
    uint32_t res_id;
    uint32_t depth_enable;
    uint32_t depth_write_mask;    // 0: ZERO, 1: ALL
    uint32_t depth_func;          // D3D11_COMPARISON_FUNC (1..8)
    uint32_t stencil_enable;
    uint32_t stencil_read_mask;   // low 8 bits used
    uint32_t stencil_write_mask;  // low 8 bits used
    RgpuStencilOpDesc front_face;
    RgpuStencilOpDesc back_face;
};

struct CreateRasterizerStatePayload {
    uint32_t res_id;
    uint32_t fill_mode;           // 2: WIREFRAME, 3: SOLID
    uint32_t cull_mode;           // 1: NONE, 2: FRONT, 3: BACK
    uint32_t front_counter_clockwise;
    int32_t  depth_bias;
    float    depth_bias_clamp;
    float    slope_scaled_depth_bias;
    uint32_t depth_clip_enable;
    uint32_t scissor_enable;
    uint32_t multisample_enable;
    uint32_t antialiased_line_enable;
};

// =============================================================================
// Pipeline State & Binding Payloads
// =============================================================================

struct OMSetBlendStatePayload {
    uint32_t res_id;              // Blend State ID (0: default state)
    float    blend_factor[4];
    uint32_t sample_mask;
};

struct OMSetDepthStencilStatePayload {
    uint32_t res_id;              // Depth-Stencil State ID (0: default state)
    uint32_t stencil_ref;
};

struct RSSetStatePayload {
    uint32_t res_id;              // Rasterizer State ID (0: default state)
};

struct RSSetScissorPayload {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

struct IASetInputLayoutPayload {
    uint32_t res_id;              // Input Layout ID (0 to unbind)
};

struct IASetVertexBufferPayload {
    uint32_t slot;                // Input slot (0..15)
    uint32_t res_id;              // Vertex Buffer ID (0 to unbind)
    uint32_t stride;              // Stride in bytes between consecutive vertices
    uint32_t offset;              // Starting byte offset in the buffer
};

struct IASetIndexBufferPayload {
    uint32_t res_id;              // Index Buffer ID (0 to unbind)
    uint32_t format;              // DXGI_FORMAT_R16_UINT (57) or DXGI_FORMAT_R32_UINT (42)
    uint32_t offset;              // Byte offset from buffer start
};

struct IASetTopologyPayload {
    uint32_t topology;            // D3D11_PRIMITIVE_TOPOLOGY (4 = TRIANGLELIST)
};

struct VSSetShaderPayload {
    uint32_t res_id;              // Vertex Shader ID (0 to unbind)
};

struct VSSetConstantBufferPayload {
    uint32_t slot;                // Constant buffer slot (0..13)
    uint32_t res_id;              // Constant Buffer ID (0 to unbind)
};

struct PSSetShaderPayload {
    uint32_t res_id;              // Pixel Shader ID (0 to unbind)
};

struct PSSetConstantBufferPayload {
    uint32_t slot;                // Constant buffer slot (0..13)
    uint32_t res_id;              // Constant Buffer ID (0 to unbind)
};

struct PSSetSrvPayload {
    uint32_t slot;                // SRV slot (0..127)
    uint32_t res_id;              // Shader Resource View ID (0 to unbind)
};

struct PSSetSamplerPayload {
    uint32_t slot;                // Sampler slot (0..15)
    uint32_t res_id;              // Sampler State ID (0 to unbind)
};

struct RSSetViewportPayload {
    float top_left_x;             // Top-left X coordinate
    float top_left_y;             // Top-left Y coordinate
    float width;                  // Viewport width
    float height;                 // Viewport height
    float min_depth;              // Minimum depth (typically 0.0f)
    float max_depth;              // Maximum depth (typically 1.0f)
};

struct OMSetRenderTargetsPayload {
    uint32_t rtv_id;              // Render Target View ID (0 to unbind)
    uint32_t dsv_id;              // Depth Stencil View ID (0 to unbind)
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
    uint32_t res_id;              // Target buffer or texture ID
    uint32_t subresource;         // Subresource index (0 for simple resources)
    uint32_t has_dst_box;         // 1 if custom destination box is provided, 0 otherwise
    RgpuBox  dst_box;             // Destination box coordinates
    uint32_t row_pitch;           // Source data row pitch in bytes
    uint32_t depth_pitch;         // Source data depth pitch in bytes
    uint32_t data_size;           // Total byte size of following data
};

struct WriteBufferDiscardPayload {
    uint32_t res_id;              // Buffer resource ID
    uint32_t byte_offset;         // Byte offset into buffer (usually 0)
    uint32_t byte_width;          // Number of updated bytes that follow
};

struct ClearRtvPayload {
    uint32_t res_id;              // Render Target View ID
    float    color[4];            // RGBA float clear color values
};
using ClearRenderTargetViewPayload = ClearRtvPayload;

struct ClearDsvPayload {
    uint32_t res_id;              // Depth Stencil View ID
    uint32_t clear_flags;         // D3D11_CLEAR_DEPTH (1) | D3D11_CLEAR_STENCIL (2)
    float    depth;               // Depth clear value (typically 1.0f)
    uint8_t  stencil;             // Stencil clear value (typically 0)
    uint8_t  padding[3];          // Explicit padding maintaining 4-byte struct alignment
};
using ClearDepthStencilViewPayload = ClearDsvPayload;

struct DrawPayload {
    uint32_t vertex_count;        // Number of vertices to draw
    uint32_t start_vertex_location; // Index of the first vertex
};

struct DrawIndexedPayload {
    uint32_t index_count;         // Number of indices to draw
    uint32_t start_index_location;// Location of the first index in index buffer
    int32_t  base_vertex_location;// Value added to each index before reading vertex
};

// =============================================================================
// Presentation & Frame Return Payloads
// =============================================================================

struct PresentPayload {
    uint32_t swapchain_id;        // Monotonic SwapChain ID (0 for default window swapchain)
    uint32_t sync_interval;       // 0: Unthrottled immediate presentation, 1: VSync
    uint32_t flags;               // DXGI_PRESENT flags
    uint64_t frame_number;        // Monotonic client frame counter
};

struct FrameDataPayload {
    uint32_t swapchain_id;        // Matching SwapChain ID
    uint32_t width;               // Frame width in pixels
    uint32_t height;              // Frame height in pixels
    uint32_t format;              // DXGI_FORMAT enum (e.g. 28 = DXGI_FORMAT_R8G8B8A8_UNORM)
    uint32_t row_pitch;           // Stride in bytes (e.g. width * 4)
    uint32_t data_size;           // Total byte size of following pixel array (row_pitch * height)
    uint64_t frame_number;        // Monotonic server frame number matching PresentPayload
};

// Sent when a swap chain is created at, or resized to, a size other than the handshake size.
// As with IDXGISwapChain::ResizeBuffers, views created on the old back buffer (resource_id 0)
// keep pointing at the old texture; the client must recreate them.
struct ResizeBuffersPayload {
    uint32_t width;               // New back buffer width in pixels
    uint32_t height;              // New back buffer height in pixels
};

// With FLAG_COMPRESSED, FrameDataPayload is followed by this header and the codec token stream;
// FrameDataPayload::data_size then counts both. Without the flag, raw pixels follow as before.
struct CompressedFrameHeader {
    uint32_t codec;               // RgpuFrameCodec
    uint32_t mode;                // RgpuFrameMode
    uint32_t raw_size;            // Decoded byte count (width * height * 4)
};

#pragma pack(pop)

// =============================================================================
// Compile-Time Layout & Compatibility Invariants
// =============================================================================

static_assert(sizeof(PacketHeader) == 16, "PacketHeader must be exactly 16 bytes");
static_assert(std::is_standard_layout_v<PacketHeader>, "PacketHeader must have standard layout");
static_assert(std::is_trivially_copyable_v<PacketHeader>, "PacketHeader must be trivially copyable");

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
static_assert(sizeof(RgpuRenderTargetBlend) == 32);
static_assert(sizeof(CreateBlendStatePayload) == 268);
static_assert(sizeof(RgpuStencilOpDesc) == 16);
static_assert(sizeof(CreateDepthStencilStatePayload) == 60);
static_assert(sizeof(CreateRasterizerStatePayload) == 44);
static_assert(sizeof(OMSetBlendStatePayload) == 24);
static_assert(sizeof(OMSetDepthStencilStatePayload) == 8);
static_assert(sizeof(RSSetStatePayload) == 4);
static_assert(sizeof(RSSetScissorPayload) == 16);

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
static_assert(sizeof(CompressedFrameHeader) == 12);
static_assert(sizeof(ResizeBuffersPayload) == 8);

} // namespace RemoteGPU

#endif // REMOTE_GPU_PROTOCOL_H
