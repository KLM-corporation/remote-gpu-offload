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
| 20 | DXGI Factory & Adapter Proxies | `IDXGIFactory2` / `IDXGIAdapter1` proxies (device `GetAdapter`, drop-in `dxgi.dll`), `IDXGISwapChain1`, `CreateSwapChainForHwnd`, real `ResizeBuffers` via `OP_RESIZE_BUFFERS` | Post-M5 | Real apps |
| 19 | Frame Compression | Lossless `FRAME_CODEC_RLE_DELTA` (XOR prediction vs previous frame or left pixel, zero-run/literal tokens) negotiated with `CLIENT_FLAG_FRAME_COMPRESSION`; raw fallback when it would not save bytes | Post-M5 | Bandwidth |

---

## Milestones

| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| M1 | Network Protocol & Transport Layer | Common protocol header (`protocol.h`), Winsock2 TCP transport class (`transport.h`, `transport.cpp`), packet serialization, handshake, and transport unit tests (`test_transport.cpp`) | none | DONE |
| M2 | Remote GPU Execution Server | Dedicated adapter selection, headless D3D11 device creation, server resource manager, command execution dispatcher, staging framebuffer readback, and server unit tests (`test_server.cpp`) | M1 | DONE |
| M3 | Client Direct3D Interception & Presentation | Drop-in `d3d11.dll`/`dxgi.dll`, COM proxies (`ProxyDevice`, `ProxyContext`, `ProxySwapChain`), dynamic buffer shadow mapper, Win32 GDI zero-GPU presentation, and client proxy unit tests (`test_client_proxy.cpp`) | M1, M2 | DONE (`dxgi.dll` wrapper deferred) |
| M4 | 3D Sample App & Automated Verification Suite | Standalone 3D dynamic textured mesh sample application (`sample_cube.exe`), automated loopback test runner (`run_loopback_test.py`), and mathematical pixel verifier (`verify_pixels.py`) | M1, M2, M3 | DONE |
| M5 | E2E Integration, Hardening & Final Gate | Full end-to-end integration running client proxy with sample app against server on 127.0.0.1 for >= 60 frames, pixel verification, network disconnect test, and clean exit code 0 | M1, M2, M3, M4 | DONE |

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

### 1b. Frame Compression (`src/common/frame_codec.h`)
- The client sets `CLIENT_FLAG_FRAME_COMPRESSION` in `HandshakeRequestPayload::client_flags` (disable with `RGPU_COMPRESSION=0`). Clients that send 0 keep receiving raw frames.
- A compressed `OP_FRAME_DATA` carries `FLAG_COMPRESSED`; its body is `CompressedFrameHeader {codec, mode, raw_size}` followed by tokens, and `FrameDataPayload::data_size` counts both.
- `FRAME_MODE_KEY` (first frame of a session or after a size change) predicts each pixel from its left neighbour; `FRAME_MODE_DELTA` from the previous frame. Both sides keep the last frame the client holds, whether it was sent compressed or raw.
- Measured on the sample cube (60 frames, 640x480): 70.3 MB rendered, 1.7 MB sent (2.4%). 1280x720 over 200 frames: 703 MB -> 11.3 MB (1.6%). Codec cost at 1080p: about 2 ms encode, 1 ms decode.

### 1c. LAN Validation (2026-10-26)
Server: desktop with NVIDIA GeForce RTX 4070, wired Ethernet. Client: Lenovo laptop (Intel UHD 620, not used for rendering), Wi-Fi Intel AC 9560 (866 Mb/s link). Sample cube, 300 frames per run, two runs per row, `RGPU_SERVER=192.168.1.17`.

| Resolution | Compression | FPS | Received per frame | Wi-Fi throughput |
|---|---|---|---|---|
| 640x480 | off | 41 | 1203 KB | ~400 Mb/s |
| 640x480 | on | 135 | 28 KB | ~31 Mb/s |
| 1280x720 | off | 16 | 3606 KB | ~480 Mb/s |
| 1280x720 | on | 91 | 60 KB | ~45 Mb/s |

- Two-machine LAN operation (acceptance criterion) verified; the captured frame passes `verify_pixels.py` and is byte-identical with and without compression.
- Without compression the Wi-Fi link is saturated. With compression bandwidth is no longer the limit: the 1:1 Present round trip (one frame in flight) is, which frame pipelining would address.

### 1d. Real-Application Compatibility
- Test corpus: Microsoft's Direct3D 11 tutorials 02, 04, 05, 06, 07 (github.com/walbourn/directx-sdk-samples, MIT), unmodified. `python scripts/run_compat_tests.py --fetch` downloads them (~1.5 MB, sparse clone into `build/third_party`), builds them with `tests/compat/shim` (DirectXMath subset missing from LLVM-MinGW), runs each through the proxy and checks the last frame. All five render correctly.
- `OP_RESIZE_BUFFERS` (0x0072, `ResizeBuffersPayload {width, height}`) recreates the host back buffer; sent when a swap chain is created at a size other than the handshake size and on `ResizeBuffers`. As in D3D11, views on the old back buffer must be recreated.
- A factory created before any device (`CreateDXGIFactory*`) opens one short probe session to learn the remote adapter name and VRAM.
- Fixed-function states are forwarded: `OP_CREATE_BLEND_STATE` (0x001A), `OP_CREATE_DEPTH_STENCIL_STATE` (0x001B), `OP_CREATE_RASTERIZER_STATE` (0x001C), `OP_OM_SET_BLEND_STATE` (0x003C, with blend factor and sample mask), `OP_OM_SET_DEPTH_STENCIL_STATE` (0x003D, with stencil ref), `OP_RS_SET_STATE` (0x003E), `OP_RS_SET_SCISSOR` (0x003F, first rectangle). Res_id 0 binds the D3D11 default state. The client normalises fields D3D11 ignores (render targets 1-7 without independent blending, disabled blend/stencil) and the server validates every enum. `tests/test_render_states.cpp` checks culling, front-face winding, wireframe, scissor, alpha blending, depth on/off and stencil + colour write mask on the rendered pixels.
- Texture subresources: `CreateTexture2D` initial data carries every subresource (mip levels x array slices) as `has_initial_data = TEXTURE_INITIAL_DATA_SUBRESOURCES` (2): `uint32 count`, `RgpuSubresourceData[count]` {row_pitch, slice_pitch, offset, size}, then the data. `src/common/dxgi_format_info.h` gives client and server the exact bytes Direct3D reads per subresource (BC formats in 4x4 blocks); the server rejects any entry, legacy single entry or `UpdateSubresource` (pitch, box, subresource index) that would make the driver read past the received packet. SRVs created without a description let the host derive the view (2D arrays, cubes); explicit array/cube views send their slice range in an optional `CreateSrvArrayRange` trailer. Covered by `tests/test_textures.cpp` (pixel checks) and `Test_ServerDispatcher_TextureDataBounds` in `tests/test_server.cpp`.
- RTX 4070 over Wi-Fi (2026-10-26, server `192.168.1.17`): `test_client_proxy` (17 checks), `test_dxgi_proxy` (19; the adapter reports "RemoteGPU: NVIDIA GeForce RTX 4070", 12012 MB), `test_render_states` (12) and `test_textures` (20) all pass, and the five tutorials render correctly. Sample cube: 130 FPS at 640x480 and 78 FPS at 1280x720 with compression, 40 and 16 FPS without.
- Instancing and pipeline coverage: `OP_DRAW_INSTANCED` (0x0056), `OP_DRAW_INDEXED_INSTANCED` (0x0057), `OP_OM_SET_RENDER_TARGETS_MRT` (0x0040: up to 8 RTVs, 0 RTVs = depth-only, id 0 = empty slot; the client keeps the original single-RTV opcode for one target), `OP_VS_SET_SRV` (0x0041) and `OP_VS_SET_SAMPLER` (0x0042). SRV descriptions of every dimension (buffers, typed and structured, 1D/2D/3D, arrays, cubes, multisampled) travel as the four UINTs of the D3D11 union. `tests/test_pipeline_features.cpp` checks MRT, DrawInstanced, DrawIndexedInstanced with StartInstanceLocation, VS Buffer<float4>, VS texture sampling, PS StructuredBuffer and a draw with no render target bound, all on pixels.
- Not forwarded yet: RTV/DSV array slices, unordered access views and compute shaders, GS/HS/DS stages, VS/PS constant buffer ranges (`*SetConstantBuffers1`), `ID3D11Device1` beyond constant buffers, instanced draws, multiple render targets, `ID3D11Device1`.

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
│   │   ├── dxgi_format_info.h    # Bytes per row/rows per DXGI format (shared)
│   │   ├── protocol.h            # Packet header, opcodes, structs
│   │   ├── transport.h           # Winsock TCP socket wrapper
│   │   ├── frame_codec.h/.cpp    # Lossless RLE/delta frame compression
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
│   │   ├── proxy_dxgi.h/.cpp     # IDXGIFactory2 / IDXGIAdapter1 proxies
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
  clang++ -std=c++17 -O2 -Isrc/common -Isrc/server src/common/transport.cpp src/common/frame_codec.cpp src/server/gpu_device.cpp src/server/server_dispatcher.cpp src/server/main_server.cpp -o build/server.exe -ld3d11 -ldxgi -lws2_32 -ld3dcompiler
  ```
- **Build Client Proxy DLLs**:
  ```powershell
  clang++ -shared -static -std=c++17 -O2 -Isrc/common -Isrc/client src/common/transport.cpp src/common/frame_codec.cpp src/client/presentation.cpp src/client/proxy_device.cpp src/client/proxy_context.cpp src/client/proxy_swapchain.cpp src/client/proxy_dxgi.cpp src/client/d3d11_wrapper.cpp src/client/d3d11.def -o build/client/d3d11.dll -lws2_32 -lgdi32 -luuid -ldxguid
  clang++ -shared -static -std=c++17 -O2 src/client/dxgi_wrapper.cpp src/client/dxgi.def -o build/client/dxgi.dll
  ```
  Deploy both DLLs next to the application's .exe. `dxgi.dll` forwards `CreateDXGIFactory/1/2` to the `d3d11.dll` in its own folder.
  The proxy DLL must live in `build/client/`, never next to `server.exe`: Windows loads a `d3d11.dll` from the executable's folder first, so the server would load the proxy instead of the real driver.
  `src/client/generated/*.inc` is produced by `python tools/gen_context_stubs.py <llvm-mingw>/include/d3d11.h src/client/generated`.
  Server address for the proxy: `RGPU_SERVER` (default `127.0.0.1`) and `RGPU_PORT` (default `48888`). `RGPU_CAPTURE_FILE=<path.bmp>` saves the last presented frame.
- **Client Proxy End-to-End Test** (server must be running):
  ```powershell
  clang++ -std=c++17 -O2 tests/test_client_proxy.cpp -o build/test_client_proxy.exe -ld3dcompiler -luser32 -lgdi32 -ldxguid
  build/test_client_proxy.exe build/client/d3d11.dll 60
  ```
- **Build 3D Sample App**:
  ```powershell
  clang++ -std=c++17 -O2 -static -Isrc/sample_app src/sample_app/main_sample.cpp -o build/client/sample_cube.exe -ld3d11 -ld3dcompiler -luser32 -ldxguid
  ```
- **Run Automated Loopback Test**:
  ```powershell
  python scripts/run_loopback_test.py --port 48888 --frames 60 --build
  ```
  `--build` compiles the server, proxy DLL and sample first. Artifacts (server/client logs, `last_frame.bmp`/`.png`, `pixel_metrics.json`) go to `build/loopback_artifacts/`.
  Pixel check alone: `python tests/verify_pixels.py <frame.bmp> [--png out.png] [--json out.json]` (exit code 0 = pass, 1 = criteria failed, 2 = unreadable frame).
- **Network Fault Tests** (no server, server killed, client killed, 15 s idle client, 6-session memory check):
  ```powershell
  python scripts/run_fault_tests.py [--only server_killed client_killed ...]
  ```
- **Real-Application Compatibility Tests** (Microsoft D3D11 tutorials through the proxy):
  ```powershell
  python scripts/run_compat_tests.py --fetch
  ```
- **M5 Final Gate** (build + all unit, proxy, loopback and fault tests; exit code 0 = pass):
  ```powershell
  python scripts/run_final_gate.py
  ```
  Server sessions use no receive timeout (an idle client is legitimate) and TCP keepalive (5 s, then 1 s probes) to detect silently dropped peers. A session is reported `Normal` only when the client sent `OP_SHUTDOWN`.
