# Multi-API Remoting Architecture: D3D11, D3D12, Vulkan & OpenGL

This document details how the Remote GPU Offload (RGPU) architecture scales from Direct3D 11 to Direct3D 12, Vulkan, and OpenGL.

---

## 1. Protocol Opcode Space Partitioning

The RGPU binary protocol reserves 16-bit opcodes partitioned by API domain:

| Opcode Range | API Target | Status | Characteristics |
| :--- | :--- | :--- | :--- |
| `0x0001` - `0x000F` | Core Session & Control | ✅ Active | Handshake, capabilities, shutdown, ping |
| `0x0010` - `0x007F` | Direct3D 11 Immediate | ✅ Active | Immediate context state, draw calls, staging readback |
| `0x0100` - `0x01FF` | Direct3D 12 Explicit | 📋 Planned | Command lists, descriptor heaps, barriers |
| `0x0200` - `0x02FF` | Vulkan Explicit | 📋 Planned | VkCommandBuffer, pipeline layouts, queue submissions |
| `0x0300` - `0x03FF` | OpenGL State Machine | 📋 Planned | WGL contexts, display lists, shader programs |

---

## 2. API-by-API Deep Dive

### A. Direct3D 11 (Immediate Mode)
- **Status:** **Implemented & Live Tested.**
- **Interception Mechanism:** Proxy `d3d11.dll` exposing `D3D11CreateDevice` and `D3D11CreateDeviceAndSwapChain`.
- **Key Characteristics:** 
  - Centralized immediate context (`ID3D11DeviceContext`).
  - Monotonic resource IDs allocated by client and registered on server.
  - Automatic offscreen target allocation matching client window dimensions.
  - Staging texture readback with row pitch alignment handling.

### B. Direct3D 12 (Command List Architecture)
- **Status:** **Architecture Drafted.**
- **Interception Mechanism:** Proxy `d3d12.dll` exposing `D3D12CreateDevice`.
- **Why D3D12 is Well-Suited to Networking:**
  - In D3D11, calls are submitted immediately, requiring frequent synchronization.
  - In D3D12, applications record hundreds of commands into `ID3D12GraphicsCommandList` objects locally without GPU interaction.
  - Network transmission only occurs upon `ID3D12CommandQueue::ExecuteCommandLists()`, dramatically reducing per-call network overhead.
- **Key Challenges:**
  - Descriptor heaps and GPU virtual addresses (GPUVA) must be translated to relative offsets across machines.
  - Explicit synchronization (`ID3D12Fence`) must be communicated across the network.

### C. Vulkan (Layer-Based Architecture)
- **Status:** **Architecture Drafted.**
- **Interception Mechanism:** Official Khronos Vulkan Layer (`VK_LAYER_remote_gpu`).
- **Advantage Over DLL Hooking:**
  - Unlike DirectX and OpenGL which require DLL proxying or memory detours, Vulkan officially specifies API layers.
  - Placing a JSON manifest in the Windows registry automatically injects the remoting interceptor into **any Vulkan game or application** (e.g., Doom Eternal, Baldur's Gate 3, Red Dead Redemption 2).
- **Execution Flow:**
  - `vkQueueSubmit` serializes compiled command buffers into opcode `0x0250` packets.
  - `vkQueuePresentKHR` serializes swapchain presentation and triggers frame readback.

### D. OpenGL (State Machine Architecture)
- **Status:** **Architecture Drafted.**
- **Interception Mechanism:** Proxy `opengl32.dll` exposing `wglCreateContext`, `wglMakeCurrent`, and `wglGetProcAddress`.
- **Key Challenges:**
  - OpenGL has massive legacy surface area (`glBegin`/`glEnd`, display lists).
  - Modern OpenGL (Core Profile 3.3+) operates cleanly with VBOs/VAOs and GLSL shaders, which map directly to our buffer/shader protocol opcodes.

---

## 3. High-Performance Frame Compression Roadmap

Streaming raw 32-bit RGBA frames requires significant bandwidth:
- `512 x 512` @ 60 FPS = `60 MB/s` (~480 Mbps)
- `1920 x 1080` @ 60 FPS = `474 MB/s` (~3.8 Gbps)

To achieve 60 FPS at 1080p and 4K, Milestone 4+ introduces:
1. **LZ4 / Zstandard Fast Compression:** 3-5x bandwidth reduction with < 1 ms CPU overhead.
2. **NVENC Hardware H.264 / AV1 Encoding:** Encodes directly on the RTX 4070 NVENC silicon, reducing a 1080p frame from 8 MB to 50 KB (< 20 Mbps), unlocking smooth 60 FPS streaming over standard Wi-Fi.
