# Project: Networked GPU Offloading System

## Architecture
A high-performance remote GPU offloading framework enabling Windows clients without dedicated graphics hardware to execute full Direct3D 11 rendering workloads remotely on a dedicated GPU server and present the streamed framebuffers locally.

### Data & Control Flow Architecture
```
+----------------------------------------------------------------------------------+
| CLIENT PC (Zero-GPU Requirement)                                                 |
|                                                                                  |
| [3D Application / Sample App]                                                    |
|           | (standard D3D11CreateDeviceAndSwapChain calls)                       |
|           v                                                                      |
| [d3d11.dll / dxgi.dll Proxy Wrapper]                                             |
|           |                                                                      |
|           +--> ProxyD3D11Device (assigns monotonic res_id)                       |
|           +--> ProxyD3D11DeviceContext (serializes pipeline commands)            |
|           +--> ProxyDXGISwapChain (captures OutputWindow HWND, presents frames)  |
|                         |                                                        |
|                         v                                                        |
|           [Winsock2 TCP Client Transport (TCP_NODELAY, SIO_LOOPBACK_FAST_PATH)]  |
+-------------------------|--------------------------------------------------------+
                          | Network Socket (127.0.0.1 or LAN IP)
                          v
+----------------------------------------------------------------------------------+
| HOST GPU SERVER (Dedicated Graphics Hardware)                                    |
|                                                                                  |
| [Winsock2 TCP Server Transport (16 MB Socket Buffers)]                            |
|           |                                                                      |
|           v                                                                      |
| [Server Command Dispatcher & Deserializer]                                       |
|           | (maps res_id to real COM objects)                                    |
|           v                                                                      |
| [Direct3D 11 Dedicated Hardware Device (D3D_DRIVER_TYPE_UNKNOWN)]                |
|   - Adapter: Intel(R) UHD Graphics 620 (or discrete GPU)                         |
|   - Offscreen Render Target (Headless, no window required)                       |
|   - Executes: Shaders, Textures, Vertex/Index Buffers, DrawIndexed               |
|           |                                                                      |
|           v                                                                      |
| [Staging Readback Pipeline (D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ)]        |
|   - CopyResource -> Map -> Scanline Pitch Alignment Packing                      |
|           |                                                                      |
|           +--> Streams CMD_FRAME_DATA back to Client ProxyDXGISwapChain          |
+----------------------------------------------------------------------------------+
```

---

## Feature Inventory
Every feature identified during Phase 0 Survey is inventoried and assigned to an exact milestone:

| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | Socket Transport & Framing | Winsock2 TCP socket abstraction, `TCP_NODELAY`, 16 MB buffers, `SIO_LOOPBACK_FAST_PATH`, `SendExact`/`RecvExact` stream boundary handling | M1 | Survey (R2) |
| 2 | Packet Serialization | 16-byte fixed packet header (`0x52475055`), `#pragma pack(push, 1)` binary structs for 32 graphics opcodes, zero-copy buffer packing | M1 | Survey (R2) |
| 3 | Handshake & Lifecycle | Session handshake (`CMD_HANDSHAKE`), version check, resolution/format negotiation, graceful disconnect (`CMD_SHUTDOWN`), loopback/LAN support | M1 | Survey (R2) |
| 4 | Network Fault Tolerance | Socket timeout detection, disconnect recovery, non-crashing shutdown on either party closing connection | M1 | Survey (R2) |
| 5 | DXGI Adapter Selection | Enumerate adapters, filter software rasterizers (`0x1414`), select dedicated GPU with highest VRAM, log adapter description | M2 | Survey (R3) |
| 6 | Headless D3D11 Device | Create hardware D3D11 device with `D3D_DRIVER_TYPE_UNKNOWN` and explicit `IDXGIAdapter*` without requiring window handles | M2 | Survey (R3) |
| 7 | Host GPU Resource Manager | Server-side lookup map translating client `res_id` to actual `ID3D11Buffer`, `ID3D11Texture2D`, shaders, input layouts | M2 | Survey (R3) |
| 8 | Server Command Execution | Command dispatcher executing pipeline calls: `ClearRenderTargetView`, `IASet*`, `VSSet*`, `PSSet*`, `DrawIndexed`, `Draw` | M2 | Survey (R3) |
| 9 | Framebuffer Readback | Headless staging texture (`D3D11_USAGE_STAGING`), `CopyResource`, `Map`/`Unmap`, row-pitch packing eliminating scanline shearing | M2 | Survey (R4) |
| 10 | Client Interception DLLs | Drop-in `d3d11.dll` and `dxgi.dll` exporting `D3D11CreateDevice`, `D3D11CreateDeviceAndSwapChain`, forwarding unintercepted calls | M3 | Survey (R1) |
| 11 | COM Proxy Object Machine | `ProxyD3D11Device`, `ProxyD3D11DeviceContext`, `ProxyDXGISwapChain` intercepting API calls, assigning monotonic client `res_id` | M3 | Survey (R1) |
| 12 | Dynamic Buffer Mapping | Client CPU shadow memory for `Map(D3D11_MAP_WRITE_DISCARD)`, serializing updated buffer deltas on `Unmap()` | M3 | Survey (R1) |
| 13 | Zero-Hardware Presentation | Window `HWND` presentation via Win32 GDI `SetDIBitsToDevice` on `GetDC(hWnd)`, requiring zero hardware acceleration on client | M3 | Survey (R4) |
| 14 | Frame Flow Control | 1:1 frame pacing at `IDXGISwapChain::Present()`, blocking client present until server signals frame completion, preventing stream bufferbloat | M3 | Survey (R2, R4) |
| 15 | Standalone 3D Sample App | Complete Direct3D 11 application rendering rotating textured 3D mesh (cube with vertex colors, textured faces, camera matrices) | M4 | Survey (R5) |
| 16 | Loopback Test Runner | Automated Python/PowerShell test harness running server and client on 127.0.0.1, executing >= 60 frames, clean exit code 0 | M4 | Survey (R5) |
| 17 | Programmatic Pixel Verifier | Mathematical validation analyzing captured frame: mean luminance, standard deviation, non-clear ratio, multi-color clusters | M4 | Survey (R5) |
| 18 | E2E System Integration | Full integration of client proxy DLL with 3D sample app and remote server, verified over loopback with zero memory leaks | M5 | Acceptance Criteria |

---

## Milestones

| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| M1 | Network Protocol & Transport Layer | Common protocol header (`protocol.h`), Winsock2 TCP transport class (`transport.h`, `transport.cpp`), packet serialization, handshake, and transport unit tests (`test_transport.cpp`) | none | DONE |
| M2 | Remote GPU Execution Server | Dedicated adapter selection, headless D3D11 device creation, server resource manager, command execution dispatcher, staging framebuffer readback, and server unit tests (`test_server.cpp`) | M1 | DONE |
| M3 | Client Direct3D Interception & Presentation | Drop-in `d3d11.dll`/`dxgi.dll`, COM proxies (`ProxyDevice`, `ProxyContext`, `ProxySwapChain`), dynamic buffer shadow mapper, Win32 GDI zero-GPU presentation, and client proxy unit tests (`test_client_proxy.cpp`) | M1, M2 | IN_PROGRESS |
| M4 | 3D Sample App & Automated Verification Suite | Standalone 3D dynamic textured mesh sample application (`sample_cube.exe`), automated loopback test runner (`run_loopback_test.py`), and mathematical pixel verifier (`verify_pixels.py`) | M1, M2, M3 | PLANNED |
| M5 | E2E Integration, Hardening & Final Gate | Full end-to-end integration running client proxy with sample app against server on 127.0.0.1 for >= 60 frames, pixel verification, network disconnect test, and clean exit code 0 | M1, M2, M3, M4 | PLANNED |

---

## Interface Contracts

### 1. Packet Protocol Contract (`src/common/protocol.h`)
```cpp
#pragma pack(push, 1)

constexpr uint32_t RGPU_MAGIC = 0x52475055; // 'RGPU'
constexpr uint32_t RGPU_PROTOCOL_VERSION = 1;
constexpr uint16_t RGPU_DEFAULT_PORT = 48888;

struct PacketHeader {
    uint32_t magic;           // RGPU_MAGIC
    uint16_t opcode;          // RgpuOpcode
    uint16_t flags;           // RgpuFlags
    uint32_t sequence_id;     // Monotonic packet sequence number
    uint32_t payload_length;  // Size of following payload in bytes
};

enum RgpuOpcode : uint16_t {
    // Session & Lifecycle (0x0001 - 0x000F)
    OP_HANDSHAKE_REQUEST   = 0x0001,
    OP_HANDSHAKE_RESPONSE  = 0x0002,
    OP_SHUTDOWN            = 0x0003,

    // Resource Management (0x0010 - 0x002F)
    OP_CREATE_BUFFER       = 0x0010,
    OP_CREATE_TEXTURE2D    = 0x0011,
    OP_CREATE_SRV          = 0x0012,
    OP_CREATE_RTV          = 0x0013,
    OP_CREATE_DSV          = 0x0014,
    OP_CREATE_VERTEX_SHADER= 0x0015,
    OP_CREATE_PIXEL_SHADER = 0x0016,
    OP_CREATE_INPUT_LAYOUT = 0x0017,
    OP_DESTROY_RESOURCE    = 0x0018,

    // Pipeline State & Binding (0x0030 - 0x004F)
    OP_IA_SET_INPUT_LAYOUT = 0x0030,
    OP_IA_SET_VERTEX_BUFFER= 0x0031,
    OP_IA_SET_INDEX_BUFFER = 0x0032,
    OP_IA_SET_TOPOLOGY     = 0x0033,
    OP_VS_SET_SHADER       = 0x0034,
    OP_VS_SET_CB           = 0x0035,
    OP_PS_SET_SHADER       = 0x0036,
    OP_PS_SET_CB           = 0x0037,
    OP_PS_SET_SRV          = 0x0038,
    OP_RS_SET_VIEWPORT     = 0x0039,
    OP_OM_SET_RENDER_TARGETS=0x003A,

    // Updates & Draw Calls (0x0050 - 0x006F)
    OP_UPDATE_SUBRESOURCE  = 0x0050,
    OP_WRITE_BUFFER_DISCARD= 0x0051,
    OP_CLEAR_RTV           = 0x0052,
    OP_CLEAR_DSV           = 0x0053,
    OP_DRAW                = 0x0054,
    OP_DRAW_INDEXED        = 0x0055,

    // Presentation & Streaming (0x0070 - 0x007F)
    OP_PRESENT             = 0x0070,
    OP_FRAME_DATA          = 0x0071
};

#pragma pack(pop)
```

### 2. Transport Contract (`src/common/transport.h`)
```cpp
class NetworkSocket {
public:
    static bool InitializeNetwork();
    static void ShutdownNetwork();

    bool Connect(const char* host, uint16_t port);
    bool BindAndListen(uint16_t port);
    std::unique_ptr<NetworkSocket> Accept();

    bool SendExact(const void* data, size_t length);
    bool RecvExact(void* data, size_t length);
    void Close();
    bool IsValid() const;
};
```

### 3. Server Execution Contract (`src/server/gpu_device.h`)
```cpp
class HostGpuDevice {
public:
    bool InitializeDedicatedGpu(int adapter_index = -1);
    bool CreateOffscreenTargets(uint32_t width, uint32_t height);
    bool ReadbackFrame(std::vector<uint8_t>& out_rgba, uint32_t& out_width, uint32_t& out_height);
    ID3D11Device* GetDevice() const;
    ID3D11DeviceContext* GetContext() const;
    const std::string& GetAdapterDescription() const;
};
```

---

## Code Layout
```
remote_gpu_offload/
├── CMakeLists.txt
├── src/
│   ├── common/
│   │   ├── protocol.h            # Packet header, opcodes, structs
│   │   ├── transport.h           # Winsock TCP socket wrapper
│   │   └── transport.cpp         # Socket implementation & error handling
│   ├── server/
│   │   ├── gpu_device.h          # Adapter enumeration, D3D11 headless device
│   │   ├── gpu_device.cpp        # Staging texture & readback pipeline
│   │   ├── server_dispatcher.h   # Opcode dispatcher & resource manager
│   │   ├── server_dispatcher.cpp # Reconstructing pipeline state
│   │   └── main_server.cpp       # Standalone server daemon entrypoint
│   ├── client/
│   │   ├── proxy_device.h        # ID3D11Device implementation
│   │   ├── proxy_device.cpp      # Monotonic res_id assignment & packet dispatch
│   │   ├── proxy_context.h       # ID3D11DeviceContext implementation
│   │   ├── proxy_context.cpp     # Serializing state and draw commands
│   │   ├── proxy_swapchain.h     # IDXGISwapChain implementation
│   │   ├── proxy_swapchain.cpp   # Frame receipt & presentation
│   │   ├── presentation.h        # Zero-GPU Win32 GDI presentation
│   │   ├── presentation.cpp      # SetDIBitsToDevice blit
│   │   ├── d3d11_wrapper.cpp     # D3D11CreateDevice[AndSwapChain] exports
│   │   └── dxgi_wrapper.cpp      # CreateDXGIFactory[1/2] exports
│   └── sample_app/
│       ├── main_sample.cpp       # Standalone 3D rotating textured cube app
│       ├── cube_geometry.h       # Cube vertices, UVs, indices
│       └── shaders.h             # Embedded compiled/source HLSL
├── tests/
│   ├── test_transport.cpp        # Transport loopback unit tests
│   ├── test_server.cpp           # Headless GPU & readback unit tests
│   ├── test_client_proxy.cpp     # Proxy serialization unit tests
│   └── verify_pixels.py          # Mathematical pixel validation script
├── scripts/
│   ├── build.ps1                 # Automation build script
│   └── run_loopback_test.py      # Automated loopback E2E test runner
└── .agents/teamwork/             # Teamwork metadata only
```

---

## Build and Run Commands
- **Compiler**: `clang++` (LLVM-MinGW x86_64) or CMake with Ninja
- **Build Server**:
  ```powershell
  clang++ -std=c++17 -O2 -Isrc/common -Isrc/server src/common/transport.cpp src/server/gpu_device.cpp src/server/server_dispatcher.cpp src/server/main_server.cpp -o build/server.exe -ld3d11 -ldxgi -lws2_32 -ld3dcompiler
  ```
- **Build Client Proxy DLLs**:
  ```powershell
  clang++ -shared -std=c++17 -O2 -Isrc/common -Isrc/client src/common/transport.cpp src/client/presentation.cpp src/client/proxy_device.cpp src/client/proxy_context.cpp src/client/proxy_swapchain.cpp src/client/d3d11_wrapper.cpp src/client/dxgi_wrapper.cpp -o build/d3d11.dll -lws2_32 -lgdi32
  ```
- **Build 3D Sample App**:
  ```powershell
  clang++ -std=c++17 -O2 -Isrc/sample_app src/sample_app/main_sample.cpp -o build/sample_cube.exe -ld3d11 -ldxgi -ld3dcompiler
  ```
- **Run Automated Loopback Test**:
  ```powershell
  python scripts/run_loopback_test.py --port 48888 --frames 60
  ```
