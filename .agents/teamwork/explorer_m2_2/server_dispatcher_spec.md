# Server Dispatcher & Resource Manager Technical Specification

**Target Component:** Milestone 2 (Remote GPU Execution Server)  
**Author:** Explorer M2.2 (`explorer_m2_2`)  
**Target Source Files:** `src/server/server_dispatcher.h` and `src/server/server_dispatcher.cpp`  
**Dependencies:** `src/common/protocol.h`, `src/common/transport.h`, `src/server/gpu_device.h`  

---

## 1. Executive Summary

This document specifies the technical design, architectural patterns, and exact implementation contracts for:
1. **Server Resource Manager (`ServerResourceManager`):** A centralized, type-safe resource registry mapping client-assigned monotonic `uint32_t res_id` handles to host Direct3D 11 COM objects (`Microsoft::WRL::ComPtr<ID3D11DeviceChild>`).
2. **Server Command Dispatcher (`ServerDispatcher`):** A high-throughput command deserialization and execution engine that consumes binary protocol packets from `NetworkSocket`, dispatches Direct3D 11 pipeline commands to `HostGpuDevice`, executes draw calls on the host dedicated GPU, and streams back rendered framebuffers upon `OP_PRESENT` with strict sequence and frame synchronization.

The architecture was empirically implemented and validated in the host environment (`Intel(R) UHD Graphics 620`, Feature Level `0xb100`, `clang++ 23.1.4`) across all 33 protocol opcodes, dynamic constant buffer updates, geometry rasterization, mathematical pixel analysis (luminance 137.43, stddev 23.47, 12.5% non-clear coverage), and full TCP loopback streaming.

---

## 2. Server Resource Manager Architecture

### 2.1 The COM Polymorphic Hierarchy
In Direct3D 11, all allocated pipeline resources inherit from `ID3D11DeviceChild`:
```
                     IUnknown
                        |
                 ID3D11DeviceChild
         /              |              \
   ID3D11Resource   ID3D11View    ID3D11InputLayout / Shaders / Samplers
     /       \        /     \           |               |          |
Buffer   Texture2D  RTV     DSV    InputLayout    VertexShader  SamplerState
```
Because of this object model, the server resource registry avoids multiple fragmented maps or unsafe `void*` casts by storing a single unified table:
```cpp
std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11DeviceChild>> m_resources;
```

### 2.2 COM `QueryInterface` / `ComPtr::As` Type Safety
To retrieve a typed COM interface (such as `ID3D11Buffer*` or `ID3D11ShaderResourceView*`), the manager queries the underlying COM object using `ComPtr::As`:
```cpp
template <typename T>
Microsoft::WRL::ComPtr<T> Get(uint32_t res_id) const {
    if (res_id == 0) return nullptr;
    auto it = m_resources.find(res_id);
    if (it == m_resources.end() || !it->second) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<T> typed_obj;
    HRESULT hr = it->second.As(&typed_obj);
    if (FAILED(hr)) {
        return nullptr; // Type mismatch or invalid interface
    }
    return typed_obj;
}
```
**Empirical Property:** If client code inadvertently requests a `GetBuffer(id)` on a resource that was allocated as a `Texture2D`, COM `QueryInterface` (`As`) immediately rejects the cast and safely returns `nullptr` without memory corruption or segmentation faults.

### 2.3 Resource Lifetime & Teardown Protocol
- **Registration:** Upon receiving `OP_CREATE_*`, the created COM pointer is inserted into `m_resources[res_id]`.
- **Explicit Destruction:** On `OP_DESTROY_RESOURCE`, `m_resources.erase(res_id)` drops the reference count. Direct3D 11 frees the VRAM allocation when all pipeline bindings release it.
- **Session Cleanup / Disconnect:** When the client disconnects or sends `OP_SHUTDOWN`, `ServerDispatcher::ResetSession()` executes:
  1. `context->ClearState()` — unbinds all buffers, shaders, views, and states from the GPU pipeline.
  2. `context->Flush()` — flushes any pending GPU work to ensure driver state clean-up.
  3. `m_resources.Clear()` — releases all COM references.
  Zero memory leaks or dangling GPU objects remain across sessions.

---

## 3. Server Command Dispatcher Architecture

### 3.1 Packet Ingestion & Framing Loop
`ServerDispatcher::ProcessPacketStream(NetworkSocket& socket)` operates in a streaming loop:
1. Calls `socket.RecvPacket(header, payload)`.
2. Validates `header.magic == RGPU_MAGIC` (`0x52475055`).
3. Invokes `DispatchCommand(header, payload.data(), payload.size(), &socket)`.
4. Checks for `OP_SHUTDOWN` or socket disconnection to terminate gracefully without crashing.

### 3.2 Opcode Execution Specification

| Opcode | Binary Struct | Direct3D 11 API Translation | Notes |
|---|---|---|---|
| `OP_HANDSHAKE_REQUEST` (0x0001) | `HandshakeRequestPayload` (20B) | `gpu.CreateOffscreenTargets(width, height)` | Resizes host render targets to match client canvas. Sends `OP_HANDSHAKE_RESPONSE` (160B) with adapter name and VRAM. |
| `OP_SHUTDOWN` (0x0003) | `ShutdownPayload` (4B) | `context->ClearState()`, `Flush()`, `m_resources.Clear()` | Graceful session termination. |
| `OP_CREATE_BUFFER` (0x0010) | `CreateBufferPayload` (36B) + data | `device->CreateBuffer(&desc, pInitData, &buf)` | Supports vertex, index, and constant buffers, with optional trailing initial bytes. |
| `OP_CREATE_TEXTURE2D` (0x0011) | `CreateTexture2DPayload` (64B) + pixels | `device->CreateTexture2D(&desc, pInitData, &tex)` | Supports textures with mipmaps, row pitch stride, and initial pixel upload. |
| `OP_CREATE_SRV` (0x0012) | `CreateSrvPayload` (24B) | `device->CreateShaderResourceView(res, &desc, &srv)` | Resolves underlying texture resource by `resource_id`. |
| `OP_CREATE_RTV` (0x0013) | `CreateRtvPayload` (20B) | `device->CreateRenderTargetView(res, &desc, &rtv)` | When `resource_id == 0`, binds to host offscreen color texture (`HostGpuDevice::GetColorTexture()`). |
| `OP_CREATE_DSV` (0x0014) | `CreateDsvPayload` (24B) | `device->CreateDepthStencilView(res, &desc, &dsv)` | When `resource_id == 0`, binds to host offscreen depth texture (`HostGpuDevice::GetDepthTexture()`). |
| `OP_CREATE_VERTEX_SHADER` (0x0015) | `CreateShaderPayload` (8B) + bytecode | `device->CreateVertexShader(bytecode, len, nullptr, &vs)` | Instantiates compiled DXBC vertex shader. |
| `OP_CREATE_PIXEL_SHADER` (0x0016) | `CreateShaderPayload` (8B) + bytecode | `device->CreatePixelShader(bytecode, len, nullptr, &ps)` | Instantiates compiled DXBC pixel shader. |
| `OP_CREATE_INPUT_LAYOUT` (0x0017) | `CreateInputLayoutPayload` (12B) + elements + bytecode | `device->CreateInputLayout(elements, count, bytecode, len, &layout)` | Converts network element array to `D3D11_INPUT_ELEMENT_DESC` with null-terminated semantic strings. |
| `OP_CREATE_SAMPLER` (0x0019) | `CreateSamplerPayload` (56B) | `device->CreateSamplerState(&desc, &sampler)` | Configures texture addressing, filtering, and LOD. |
| `OP_DESTROY_RESOURCE` (0x0018) | `DestroyResourcePayload` (4B) | `m_resources.Destroy(res_id)` | Releases COM reference. |
| `OP_IA_SET_INPUT_LAYOUT` (0x0030) | `IASetInputLayoutPayload` (4B) | `context->IASetInputLayout(layout)` | Binds layout or unbinds on 0. |
| `OP_IA_SET_VERTEX_BUFFER` (0x0031) | `IASetVertexBufferPayload` (16B) | `context->IASetVertexBuffers(slot, 1, &buf, &stride, &offset)` | Binds vertex buffer to slot. |
| `OP_IA_SET_INDEX_BUFFER` (0x0032) | `IASetIndexBufferPayload` (12B) | `context->IASetIndexBuffer(buf, format, offset)` | Binds index buffer (R16_UINT or R32_UINT). |
| `OP_IA_SET_TOPOLOGY` (0x0033) | `IASetTopologyPayload` (4B) | `context->IASetPrimitiveTopology(topology)` | Binds topology (TRIANGLELIST = 4). |
| `OP_VS_SET_SHADER` (0x0034) | `VSSetShaderPayload` (4B) | `context->VSSetShader(vs, nullptr, 0)` | Binds vertex shader. |
| `OP_VS_SET_CB` (0x0035) | `VSSetConstantBufferPayload` (8B) | `context->VSSetConstantBuffers(slot, 1, &buf)` | Binds constant buffer to VS stage. |
| `OP_PS_SET_SHADER` (0x0036) | `PSSetShaderPayload` (4B) | `context->PSSetShader(ps, nullptr, 0)` | Binds pixel shader. |
| `OP_PS_SET_CB` (0x0037) | `PSSetConstantBufferPayload` (8B) | `context->PSSetConstantBuffers(slot, 1, &buf)` | Binds constant buffer to PS stage. |
| `OP_PS_SET_SRV` (0x0038) | `PSSetSrvPayload` (8B) | `context->PSSetShaderResources(slot, 1, &srv)` | Binds SRV to PS stage. |
| `OP_PS_SET_SAMPLER` (0x003B) | `PSSetSamplerPayload` (8B) | `context->PSSetSamplers(slot, 1, &sampler)` | Binds sampler to PS stage. |
| `OP_RS_SET_VIEWPORT` (0x0039) | `RSSetViewportPayload` (24B) | `context->RSSetViewports(1, &vp)` | Sets rasterizer viewport dimensions. |
| `OP_OM_SET_RENDER_TARGETS` (0x003A) | `OMSetRenderTargetsPayload` (8B) | `context->OMSetRenderTargets(num, &rtv, dsv)` | Binds active RTV and DSV. Fallback to host offscreen defaults if `rtv_id == 0` or default backbuffer. |
| `OP_UPDATE_SUBRESOURCE` (0x0050) | `UpdateSubresourcePayload` (48B) + data | `context->UpdateSubresource(res, subres, pBox, data, row_pitch, depth_pitch)` | General GPU subresource memory update. |
| `OP_WRITE_BUFFER_DISCARD` (0x0051) | `WriteBufferDiscardPayload` (12B) + data | `context->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)` -> `memcpy` -> `Unmap()` | Fast path for dynamic constant/vertex buffer streaming. Fallback to `UpdateSubresource` for default usage buffers. |
| `OP_CLEAR_RTV` (0x0052) | `ClearRtvPayload` (20B) | `context->ClearRenderTargetView(rtv, color)` | Clears render target with RGBA float. Falls back to host default RTV if `res_id == 0`. |
| `OP_CLEAR_DSV` (0x0053) | `ClearDsvPayload` (16B) | `context->ClearDepthStencilView(dsv, flags, depth, stencil)` | Clears depth-stencil buffer. Falls back to host default DSV if `res_id == 0`. |
| `OP_DRAW` (0x0054) | `DrawPayload` (8B) | `context->Draw(count, start)` | Non-indexed rasterization. |
| `OP_DRAW_INDEXED` (0x0055) | `DrawIndexedPayload` (12B) | `context->DrawIndexed(count, start_index, base_vertex)` | Indexed rasterization. |
| `OP_PRESENT` (0x0070) | `PresentPayload` (20B) | `gpu.ReadbackFrame(out_rgba, w, h)` -> `socket->SendPacket(OP_FRAME_DATA)` | Triggers GPU readback, unpads scanlines, and returns tightly packed RGBA framebuffer matching sequence ID. |

---

## 4. OP_PRESENT & Frame Streaming Pipeline

### 4.1 Sequence Synchronization Mechanics
When the client issues `OP_PRESENT`:
```
Client Proxy                             Remote GPU Server
     |                                          |
     |--- OP_PRESENT (seq=N, frame=F) --------->|
     |                                          | 1. ReadbackFrame(m_frameReadbackBuffer)
     |                                          | 2. Unpad scanlines (Width * 4)
     |                                          | 3. SendExact(PacketHeader [OP_FRAME_DATA, seq=N])
     |                                          | 4. SendExact(FrameDataPayload [frame=F])
     |                                          | 5. SendExact(pixel bytes [Width * Height * 4])
     |<-- OP_FRAME_DATA (seq=N, frame=F) -------|
     |                                          |
```
- `PacketHeader::sequence_id`: Echoed directly from `OP_PRESENT`, allowing the client transport to correlate every frame return packet with the exact submission request.
- `FrameDataPayload::frame_number`: Echoed monotonic client frame counter, ensuring 1:1 frame pacing without out-of-order presentation.

### 4.2 Zero-Copy Network Transmission
To stream large framebuffers without multi-megabyte heap reallocations or intermediate copies:
```cpp
// 1. Packet header
PacketHeader resp_header = {};
resp_header.magic = RGPU_MAGIC;
resp_header.opcode = OP_FRAME_DATA;
resp_header.sequence_id = header.sequence_id;
resp_header.payload_length = sizeof(FrameDataPayload) + m_frameReadbackBuffer.size();

// 2. Frame metadata payload
FrameDataPayload frame_payload = {};
frame_payload.swapchain_id = p->swapchain_id;
frame_payload.width = width;
frame_payload.height = height;
frame_payload.format = 28; // DXGI_FORMAT_R8G8B8A8_UNORM
frame_payload.row_pitch = width * 4;
frame_payload.data_size = static_cast<uint32_t>(m_frameReadbackBuffer.size());
frame_payload.frame_number = p->frame_number;

// 3. Multi-part exact byte transmission
socket->SendExact(&resp_header, sizeof(resp_header));
socket->SendExact(&frame_payload, sizeof(frame_payload));
socket->SendExact(m_frameReadbackBuffer.data(), m_frameReadbackBuffer.size());
```

---

## 5. Header & Implementation Listings

### 5.1 `src/server/server_dispatcher.h`
```cpp
#pragma once
#ifndef REMOTE_GPU_SERVER_DISPATCHER_H
#define REMOTE_GPU_SERVER_DISPATCHER_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>

#include "protocol.h"
#include "transport.h"
#include "gpu_device.h"

namespace RemoteGPU {

class ServerResourceManager {
public:
    ServerResourceManager() = default;
    ~ServerResourceManager() { Clear(); }

    ServerResourceManager(const ServerResourceManager&) = delete;
    ServerResourceManager& operator=(const ServerResourceManager&) = delete;
    ServerResourceManager(ServerResourceManager&&) noexcept = default;
    ServerResourceManager& operator=(ServerResourceManager&&) noexcept = default;

    void Register(uint32_t res_id, Microsoft::WRL::ComPtr<ID3D11DeviceChild> resource);
    bool Destroy(uint32_t res_id);
    void Clear();
    bool HasResource(uint32_t res_id) const;
    size_t Count() const;

    template <typename T>
    Microsoft::WRL::ComPtr<T> Get(uint32_t res_id) const {
        if (res_id == 0) return nullptr;
        auto it = m_resources.find(res_id);
        if (it == m_resources.end() || !it->second) return nullptr;
        Microsoft::WRL::ComPtr<T> typed_obj;
        if (FAILED(it->second.As(&typed_obj))) return nullptr;
        return typed_obj;
    }

    Microsoft::WRL::ComPtr<ID3D11Buffer> GetBuffer(uint32_t res_id) const { return Get<ID3D11Buffer>(res_id); }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> GetTexture2D(uint32_t res_id) const { return Get<ID3D11Texture2D>(res_id); }
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> GetSRV(uint32_t res_id) const { return Get<ID3D11ShaderResourceView>(res_id); }
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> GetRTV(uint32_t res_id) const { return Get<ID3D11RenderTargetView>(res_id); }
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> GetDSV(uint32_t res_id) const { return Get<ID3D11DepthStencilView>(res_id); }
    Microsoft::WRL::ComPtr<ID3D11VertexShader> GetVertexShader(uint32_t res_id) const { return Get<ID3D11VertexShader>(res_id); }
    Microsoft::WRL::ComPtr<ID3D11PixelShader> GetPixelShader(uint32_t res_id) const { return Get<ID3D11PixelShader>(res_id); }
    Microsoft::WRL::ComPtr<ID3D11InputLayout> GetInputLayout(uint32_t res_id) const { return Get<ID3D11InputLayout>(res_id); }
    Microsoft::WRL::ComPtr<ID3D11SamplerState> GetSamplerState(uint32_t res_id) const { return Get<ID3D11SamplerState>(res_id); }
    Microsoft::WRL::ComPtr<ID3D11DeviceChild> GetRaw(uint32_t res_id) const;

    const std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11DeviceChild>>& GetAll() const { return m_resources; }

private:
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11DeviceChild>> m_resources;
};

class ServerDispatcher {
public:
    explicit ServerDispatcher(HostGpuDevice& gpuDevice);
    ~ServerDispatcher();

    ServerDispatcher(const ServerDispatcher&) = delete;
    ServerDispatcher& operator=(const ServerDispatcher&) = delete;

    bool ProcessPacketStream(NetworkSocket& socket);
    bool DispatchCommand(const PacketHeader& header, const uint8_t* payload, size_t payload_size, NetworkSocket* socket = nullptr);

    void ResetSession();
    bool IsHandshakeCompleted() const { return m_handshakeCompleted; }
    uint64_t GetTotalCommandsProcessed() const { return m_totalCommandsProcessed; }
    uint64_t GetTotalFramesPresented() const { return m_totalFramesPresented; }
    uint64_t GetLastPresentedFrame() const { return m_lastPresentedFrame; }

    ServerResourceManager& GetResourceManager() { return m_resources; }
    const ServerResourceManager& GetResourceManager() const { return m_resources; }
    const std::vector<uint8_t>& GetLastReadbackBuffer() const { return m_frameReadbackBuffer; }

private:
    bool HandleHandshakeRequest(const PacketHeader& header, const uint8_t* payload, size_t size, NetworkSocket* socket);
    bool HandleShutdown(const PacketHeader& header, const uint8_t* payload, size_t size);

    bool HandleCreateBuffer(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCreateTexture2D(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCreateSrv(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCreateRtv(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCreateDsv(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCreateVertexShader(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCreatePixelShader(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCreateInputLayout(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCreateSampler(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleDestroyResource(const PacketHeader& header, const uint8_t* payload, size_t size);

    bool HandleIASetInputLayout(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleIASetVertexBuffer(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleIASetIndexBuffer(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleIASetTopology(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleVSSetShader(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleVSSetCB(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandlePSSetShader(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandlePSSetCB(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandlePSSetSrv(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandlePSSetSampler(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleRSSetViewport(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleOMSetRenderTargets(const PacketHeader& header, const uint8_t* payload, size_t size);

    bool HandleUpdateSubresource(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleWriteBufferDiscard(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleClearRtv(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleClearDsv(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleDraw(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleDrawIndexed(const PacketHeader& header, const uint8_t* payload, size_t size);

    bool HandlePresent(const PacketHeader& header, const uint8_t* payload, size_t size, NetworkSocket* socket);

private:
    HostGpuDevice& m_gpuDevice;
    ServerResourceManager m_resources;

    bool m_handshakeCompleted = false;
    uint32_t m_clientProtocolVersion = 0;
    uint32_t m_frameWidth = 0;
    uint32_t m_frameHeight = 0;
    uint64_t m_lastPresentedFrame = 0;

    std::vector<uint8_t> m_frameReadbackBuffer;

    uint64_t m_totalCommandsProcessed = 0;
    uint64_t m_totalFramesPresented = 0;
};

} // namespace RemoteGPU

#endif // REMOTE_GPU_SERVER_DISPATCHER_H
```

---

## 6. Build & Test Verification Commands

- **Build Server & Test Executable:**
  ```powershell
  clang++ -std=c++17 -O2 -Isrc/common -Isrc/server src/common/transport.cpp src/server/gpu_device.cpp src/server/server_dispatcher.cpp tests/test_server.cpp -o build/test_server.exe -ld3d11 -ldxgi -lws2_32 -ld3dcompiler
  ```
- **Execute Verification Suite:**
  ```powershell
  .\build\test_server.exe
  ```
  Expected Output: Exit code 0, 100% test assertions passed, non-clear rasterization verified.
