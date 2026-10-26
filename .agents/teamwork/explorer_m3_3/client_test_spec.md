# Client Proxy Test Harness Specification (`tests/test_client_proxy.cpp`)

**Milestone**: Milestone 3 — Client Direct3D Interception & Presentation  
**Document**: Test Suite Design & Verification Specification  
**Author**: M3 Client Test Explorer (`explorer_m3_3`)  
**Target File**: `tests/test_client_proxy.cpp`  
**Date**: 2026-10-26  

---

## 1. Executive Summary & Traceability Matrix

### 1.1 Purpose
The purpose of `tests/test_client_proxy.cpp` is to provide comprehensive, automated, zero-hardware verification of the client Direct3D 11 interception, command serialization, swapchain presentation, and GDI blitting subsystems before end-to-end integration with the remote GPU server.

This test harness guarantees that standard Direct3D 11 application calls (`D3D11CreateDeviceAndSwapChain`, resource creation, pipeline state configuration, dynamic buffer updates, draw calls, and presentation) are intercepted with 100% fidelity, assigned strictly monotonic 32-bit resource identifiers, serialized into valid binary protocol packets matching `src/common/protocol.h`, and that streamed rendered frames are presented locally via Win32 GDI with zero hardware GPU acceleration on the client.

### 1.2 Requirements Traceability

| Requirement | Description | Feature Inventory | Test Suite Coverage |
|---|---|---|---|
| **R1** (Interception Layer) | Intercept Direct3D 11 device creation, pipeline state setup, resource uploads, draw calls without local GPU hardware | Feature 10, 11, 12 | Suite 1 (Device creation), Suite 2 (Resources), Suite 3 (Map/Unmap), Suite 4 (Pipeline State), Suite 5 (Draw/Clear) |
| **R2** (Protocol & Transport) | Binary command stream serialization over TCP sockets with monotonic IDs, 16-byte fixed headers, exact payload framing | Feature 1, 2, 3, 4 | Suite 1 (Handshake), Suite 2–5 (Serialization across opcodes), Suite 8 (Fault tolerance) |
| **R4** (Presentation Pipeline) | Streamed frame reception, 1:1 frame pacing flow control, Win32 GDI zero-GPU blit to application window `HWND` | Feature 13, 14 | Suite 6 (Present flow control & pacing), Suite 7 (Win32 GDI DIB blit & pixel verification) |
| **R5** (Automated Verification) | Automated execution over loopback (127.0.0.1), deterministic pass/fail assertions, exit code 0 | Feature 16, 17 | Entire harness executed via standalone binary with strict pass/fail counters and exit code 0 |

---

## 2. Build Configuration & Linkage Analysis

### 2.1 Authoritative Build Command
```powershell
clang++ -std=c++17 -Wall -Wextra -Wpedantic -O2 -Isrc/common -Isrc/client `
    src/common/transport.cpp `
    src/client/presentation.cpp `
    src/client/proxy_device.cpp `
    src/client/proxy_context.cpp `
    src/client/proxy_swapchain.cpp `
    tests/test_client_proxy.cpp `
    -o build/test_client_proxy.exe `
    -lws2_32 -lgdi32 -ldxgi
```

### 2.2 Linkage Analysis & Critical Invariants
1. **Omission of `-ld3d11`**:
   - `build/test_client_proxy.exe` links `-lws2_32` (Winsock TCP networking), `-lgdi32` (Win32 GDI presentation `SetDIBitsToDevice` / `GetPixel`), and `-ldxgi` (DXGI interfaces).
   - **`-ld3d11` is intentionally NOT linked**. The client proxy classes (`ProxyD3D11Device`, `ProxyD3D11DeviceContext`, `ProxyDXGISwapChain`) implement the Direct3D 11 COM interfaces (`ID3D11Device`, `ID3D11DeviceContext`, `IDXGISwapChain`) directly. They do NOT depend on `d3d11.dll` runtime or native GPU drivers.
   - Therefore, `D3D11CreateDeviceAndSwapChain` is implemented directly inside `src/client/proxy_device.cpp` (or exported via `proxy_device.h`), allowing the test executable to invoke `D3D11CreateDeviceAndSwapChain` directly without unresolved external symbols.
2. **Omission of Server Compilation Units**:
   - Notice that `src/server/gpu_device.cpp` and `src/server/server_dispatcher.cpp` are **NOT** compiled into `test_client_proxy.exe`.
   - Consequently, `tests/test_client_proxy.cpp` **MUST** include its own in-process loopback mock server (`MockRecordingServer`) utilizing the already-compiled `NetworkSocket` from `src/common/transport.cpp`. This decouples the client test suite from the physical GPU requirements of the server and enables deterministic, byte-accurate packet verification.

---

## 3. Test Harness Architecture

### 3.1 Zero-Dependency Custom Test Framework
Following the proven patterns established in `tests/test_transport.cpp` and `tests/test_server.cpp`, the test runner uses a lightweight, self-contained macro assertion system:

```cpp
static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond) do { \
    g_tests_run++; \
    if (cond) { \
        g_tests_passed++; \
    } else { \
        g_tests_failed++; \
        std::cerr << "  [FAIL] Assertion failed: (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
    } \
} while (0)

#define TEST_ASSERT_MSG(cond, msg) do { \
    g_tests_run++; \
    if (cond) { \
        g_tests_passed++; \
    } else { \
        g_tests_failed++; \
        std::cerr << "  [FAIL] " << msg << ": (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
    } \
} while (0)

#define RUN_TEST_SUITE(suite_func) do { \
    std::cout << "============================================================" << std::endl; \
    std::cout << "[RUNNING ] " #suite_func "..." << std::endl; \
    int start_fails = g_tests_failed; \
    auto t_start = std::chrono::high_resolution_clock::now(); \
    suite_func(); \
    auto t_end = std::chrono::high_resolution_clock::now(); \
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count(); \
    if (g_tests_failed == start_fails) { \
        std::cout << "[PASSED  ] " #suite_func " (" << elapsed_ms << " ms)" << std::endl; \
    } else { \
        std::cout << "[FAILED  ] " #suite_func " (" << elapsed_ms << " ms) - " \
                  << (g_tests_failed - start_fails) << " assertion failure(s)!" << std::endl; \
    } \
} while (0)
```

### 3.2 In-Process Loopback `MockRecordingServer`
To verify network packet serialization without running a full D3D11 hardware server daemon, `tests/test_client_proxy.cpp` implements `MockRecordingServer`:

```cpp
struct RecordedPacket {
    PacketHeader header;
    std::vector<uint8_t> payload;
};

class MockRecordingServer {
public:
    MockRecordingServer();
    ~MockRecordingServer();

    // Lifecycle
    bool Start(uint16_t port = 48889);
    void Stop();

    // Packet Inspection & Synchronization
    bool WaitForPacketCount(size_t count, int timeout_ms = 3000);
    bool WaitForPacket(uint16_t opcode, RecordedPacket& out_packet, int timeout_ms = 3000);
    std::vector<RecordedPacket> GetAllPackets();
    void ClearPackets();

    // Behavior Configuration
    void SetAutoHandshake(bool enable) { m_autoHandshake = enable; }
    void SetAutoFrameData(bool enable) { m_autoFrameData = enable; }
    void SetSyntheticFramePixels(uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void AbruptDisconnectClient();

private:
    void ServerWorker();

    NetworkSocket m_listenSocket;
    std::unique_ptr<NetworkSocket> m_clientSocket;
    std::thread m_workerThread;
    std::atomic<bool> m_running{false};

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<RecordedPacket> m_recordedPackets;

    bool m_autoHandshake = true;
    bool m_autoFrameData = true;
    std::vector<uint8_t> m_syntheticPixels;
    uint32_t m_syntheticWidth = 64;
    uint32_t m_syntheticHeight = 64;
    uint64_t m_serverFrameNumber = 0;
};
```

#### Detailed Working Mechanics:
1. **Background Listening Thread**: The mock server binds to `127.0.0.1:48889` (or dynamically selected port) and accepts the client proxy socket connection.
2. **Packet Reception & Recording**: As client proxy methods execute, all incoming packets (`PacketHeader` + payload bytes) are stored in `m_recordedPackets` protected by `m_mutex`, and `m_cv.notify_all()` is signaled.
3. **Automated Handshake Response**: When `OP_HANDSHAKE_REQUEST` is received:
   - Responds immediately with `OP_HANDSHAKE_RESPONSE`:
     - `protocol_version = RGPU_PROTOCOL_VERSION` (1)
     - `status_code = 0` (OK)
     - `feature_level = D3D_FEATURE_LEVEL_11_0` (0xB000)
     - `frame_width = request.client_width`
     - `frame_height = request.client_height`
     - `frame_format = request.preferred_format`
     - `dedicated_vram = 8ULL * 1024 * 1024 * 1024` (8 GB)
     - `adapter_desc = "Synthetic Mock Dedicated GPU"`
4. **Automated Frame Data Response**: When `OP_PRESENT` is received:
   - Responds immediately with `OP_FRAME_DATA`:
     - `swapchain_id = present.swapchain_id`
     - `width = m_syntheticWidth`, `height = m_syntheticHeight`
     - `format = DXGI_FORMAT_R8G8B8A8_UNORM` (28)
     - `row_pitch = width * 4`
     - `data_size = row_pitch * height`
     - `frame_number = present.frame_number`
     - Sends `m_syntheticPixels` raw bytes.
5. **Abrupt Disconnection**: Closes `m_clientSocket` immediately to simulate network drops during operations.

### 3.3 Win32 Test Window Helper
Presentation tests require a valid window handle (`HWND`) without requiring user interaction or a physical monitor display:

```cpp
class TestWindowHelper {
public:
    static HWND CreateTestHWND(int width = 256, int height = 256, const char* title = "RGPU Test Window") {
        WNDCLASSA wc = {};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "RgpuClientProxyTestClass";
        RegisterClassA(&wc);

        HWND hwnd = CreateWindowExA(
            0,
            wc.lpszClassName,
            title,
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            width, height,
            nullptr, nullptr, wc.hInstance, nullptr
        );
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
        return hwnd;
    }

    static void DestroyTestHWND(HWND hwnd) {
        if (hwnd) {
            DestroyWindow(hwnd);
            UnregisterClassA("RgpuClientProxyTestClass", GetModuleHandleA(nullptr));
        }
    }
};
```

---

## 4. Comprehensive Test Suite Specifications

### Suite 1: Direct Invocation of `D3D11CreateDeviceAndSwapChain`

#### Objective
Verify that `D3D11CreateDeviceAndSwapChain` successfully connects to the remote host, executes the protocol handshake, creates the proxy device, context, and swapchain COM objects, and sets up the backbuffer.

#### Pre-conditions
- `MockRecordingServer` started on `127.0.0.1:48889`.
- Environment variable set: `_putenv("RGPU_PORT=48889")` and `_putenv("RGPU_HOST=127.0.0.1")`.
- `TestWindowHelper::CreateTestHWND(256, 256)` returns valid `HWND`.

#### Execution Steps
1. Populate `DXGI_SWAP_CHAIN_DESC`:
   - `BufferDesc.Width = 256`, `BufferDesc.Height = 256`
   - `BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM`
   - `SampleDesc.Count = 1`, `SampleDesc.Quality = 0`
   - `BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT`
   - `BufferCount = 1`
   - `OutputWindow = hWnd`
   - `Windowed = TRUE`
2. Call `D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &swapDesc, &pSwapChain, &pDevice, &featureLevel, &pContext)`.

#### Assertions & Verification
1. Return value `hr == S_OK`.
2. Output pointers: `pDevice != nullptr`, `pContext != nullptr`, `pSwapChain != nullptr`.
3. Output feature level: `featureLevel == D3D_FEATURE_LEVEL_11_0`.
4. Mock server handshake validation:
   - Received packet opcode `OP_HANDSHAKE_REQUEST` (0x0001).
   - `header.magic == 0x52475055` (`RGPU_MAGIC`).
   - `payload.protocol_version == 1`.
   - `payload.client_width == 256`, `payload.client_height == 256`.
   - `payload.preferred_format == DXGI_FORMAT_R8G8B8A8_UNORM` (28).
5. COM Interface Query validation:
   - `pDevice->QueryInterface(__uuidof(ID3D11Device), ...)` returns `S_OK`.
   - `pDevice->QueryInterface(__uuidof(IDXGIDevice), ...)` returns `S_OK`.
   - `pContext->QueryInterface(__uuidof(ID3D11DeviceContext), ...)` returns `S_OK`.
   - `pSwapChain->QueryInterface(__uuidof(IDXGISwapChain), ...)` returns `S_OK`.
6. Backbuffer retrieval:
   - `pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer)` returns `S_OK`.
   - `pBackBuffer != nullptr`.
   - COM refcount increments and decrements cleanly (`AddRef() == 2`, `Release() == 1`).

---

### Suite 2: Resource Creation, Monotonic `res_id` Tracking & Packet Serialization

#### Objective
Verify that all Direct3D 11 resource factory methods allocate monotonically increasing 32-bit resource IDs, serialize binary creation packets with exact field layouts, pack initial data, and emit `OP_DESTROY_RESOURCE` on final release.

#### Monotonic Sequence Verification Plan
| Step | Resource Created | API Invocations | Expected `res_id` | Wire Opcode | Expected Payload Fields |
|---|---|---|:---:|:---:|---|
| Initial | Virtual Backbuffer | Created during swapchain init | 1 | (Init) | Width=256, Height=256 |
| 1 | Vertex Buffer | `CreateBuffer` (Default, Bind Vertex) | 2 | `OP_CREATE_BUFFER` (0x0010) | `byte_width=36`, `usage=0`, `bind_flags=1`, `has_initial_data=1` |
| 2 | Index Buffer | `CreateBuffer` (Immutable, Bind Index) | 3 | `OP_CREATE_BUFFER` (0x0010) | `byte_width=6`, `usage=1`, `bind_flags=2`, `has_initial_data=1` |
| 3 | Dynamic Constant Buffer | `CreateBuffer` (Dynamic, Bind CB) | 4 | `OP_CREATE_BUFFER` (0x0010) | `byte_width=64`, `usage=2`, `bind_flags=4`, `has_initial_data=0` |
| 4 | Texture 2D | `CreateTexture2D` (64x64 RGBA) | 5 | `OP_CREATE_TEXTURE2D` (0x0011) | `width=64`, `height=64`, `format=28`, `bind_flags=8` |
| 5 | Shader Resource View | `CreateShaderResourceView` (pTex) | 6 | `OP_CREATE_SRV` (0x0012) | `resource_id=5`, `format=28`, `view_dimension=4` |
| 6 | Render Target View | `CreateRenderTargetView` (pBackBuffer) | 7 | `OP_CREATE_RTV` (0x0013) | `resource_id=1`, `format=28`, `view_dimension=4` |
| 7 | Depth Stencil Texture | `CreateTexture2D` (256x256 D24S8) | 8 | `OP_CREATE_TEXTURE2D` (0x0011) | `width=256`, `height=256`, `format=45`, `bind_flags=64` |
| 8 | Depth Stencil View | `CreateDepthStencilView` (pDepthTex) | 9 | `OP_CREATE_DSV` (0x0014) | `resource_id=8`, `format=45`, `view_dimension=3` |
| 9 | Vertex Shader | `CreateVertexShader` (synthetic DXBC) | 10 | `OP_CREATE_VERTEX_SHADER` (0x0015)| `bytecode_length=32`, followed by 32 bytes bytecode |
| 10 | Pixel Shader | `CreatePixelShader` (synthetic DXBC) | 11 | `OP_CREATE_PIXEL_SHADER` (0x0016) | `bytecode_length=32`, followed by 32 bytes bytecode |
| 11 | Input Layout | `CreateInputLayout` (2 elements) | 12 | `OP_CREATE_INPUT_LAYOUT` (0x0017) | `num_elements=2`, followed by 2x `RgpuInputElementDesc` |
| 12 | Sampler State | `CreateSamplerState` (Linear Wrap) | 13 | `OP_CREATE_SAMPLER` (0x0019) | `filter=0x15`, `address_u=1`, `address_v=1` |

#### Resource Lifecycle & Destruction Check
- Release sampler state: `pSampler->Release()`.
- Wait for mock server packet:
  - Opcode == `OP_DESTROY_RESOURCE` (0x0018).
  - Payload struct `DestroyResourcePayload { res_id: 13 }`.
  - Assert `payload.res_id == 13`.

---

### Suite 3: Dynamic Buffer Map/Unmap Lifecycle & WRITE_DISCARD Updates

#### Objective
Verify that `ID3D11DeviceContext::Map` on a dynamic buffer returns a valid CPU shadow memory pointer without immediate network transmission, and `ID3D11DeviceContext::Unmap` serializes `OP_WRITE_BUFFER_DISCARD` with the exact updated payload.

#### Execution Steps & Assertions
1. **Dynamic Constant Buffer Inspection**:
   - Dynamic buffer `pCB` (`res_id = 4`, size = 64 bytes).
2. **Execute Map**:
   - `D3D11_MAPPED_SUBRESOURCE mapped = {};`
   - `HRESULT hr = pContext->Map(pCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);`
   - `TEST_ASSERT(hr == S_OK);`
   - `TEST_ASSERT(mapped.pData != nullptr);`
   - `TEST_ASSERT(mapped.RowPitch >= 64);`
3. **Shadow Buffer In-Place Modification**:
   - Write 16 floating point values representing a transformation matrix:
     `float test_matrix[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  10.5f, 20.5f, 30.5f, 1 };`
   - `std::memcpy(mapped.pData, test_matrix, sizeof(test_matrix));`
4. **Pre-Unmap Wire Silence Check**:
   - Mock server packet count is checked before `Unmap`.
   - Assert NO `OP_WRITE_BUFFER_DISCARD` packet was transmitted while buffer was mapped (CPU-side accumulation).
5. **Execute Unmap**:
   - `pContext->Unmap(pCB, 0);`
6. **Post-Unmap Wire Verification**:
   - Mock server receives `OP_WRITE_BUFFER_DISCARD` (0x0051).
   - Header payload length == `sizeof(WriteBufferDiscardPayload) + 64` (76 bytes).
   - Payload struct `WriteBufferDiscardPayload`:
     - `res_id == 4`
     - `byte_offset == 0`
     - `byte_width == 64`
   - Trailing 64 bytes match `test_matrix` bit-for-bit (`std::memcmp == 0`).
7. **Second Consecutive Update Cycle**:
   - Map again, write updated values (`{ 2, 0, 0, 0, ... }`), call Unmap.
   - Verify second `OP_WRITE_BUFFER_DISCARD` packet received with updated contents.
8. **Negative Cases**:
   - Calling `Map` with `D3D11_MAP_READ` returns error `E_INVALIDARG` or fails gracefully without crashing.
   - Calling `Map` on non-dynamic buffer returns `E_INVALIDARG`.

---

### Suite 4: Pipeline State Setup & Binding Serialization

#### Objective
Verify that all Direct3D 11 pipeline stage binding calls on `ID3D11DeviceContext` correctly serialize their respective opcodes and encode resource IDs.

#### Method Invocations & Assertions

```cpp
// 1. Input Layout
pContext->IASetInputLayout(pLayout);
// Assert OP_IA_SET_INPUT_LAYOUT (0x0030), payload.res_id == 12

// 2. Vertex Buffers
UINT stride = sizeof(ColoredVertex);
UINT offset = 0;
pContext->IASetVertexBuffers(0, 1, &pVB, &stride, &offset);
// Assert OP_IA_SET_VERTEX_BUFFER (0x0031), slot == 0, res_id == 2, stride == 28, offset == 0

// 3. Index Buffer
pContext->IASetIndexBuffer(pIB, DXGI_FORMAT_R16_UINT, 0);
// Assert OP_IA_SET_INDEX_BUFFER (0x0032), res_id == 3, format == 57, offset == 0

// 4. Primitive Topology
pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
// Assert OP_IA_SET_TOPOLOGY (0x0033), topology == 4 (D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)

// 5. Vertex Shader & Constant Buffer
pContext->VSSetShader(pVS, nullptr, 0);
// Assert OP_VS_SET_SHADER (0x0034), res_id == 10
pContext->VSSetConstantBuffers(0, 1, &pCB);
// Assert OP_VS_SET_CB (0x0035), slot == 0, res_id == 4

// 6. Pixel Shader, Constant Buffer, SRV, Sampler
pContext->PSSetShader(pPS, nullptr, 0);
// Assert OP_PS_SET_SHADER (0x0036), res_id == 11
pContext->PSSetConstantBuffers(0, 1, &pCB);
// Assert OP_PS_SET_CB (0x0037), slot == 0, res_id == 4
pContext->PSSetShaderResources(0, 1, &pSRV);
// Assert OP_PS_SET_SRV (0x0038), slot == 0, res_id == 6
pContext->PSSetSamplers(0, 1, &pSampler);
// Assert OP_PS_SET_SAMPLER (0x003B), slot == 0, res_id == 13

// 7. Rasterizer Viewport
D3D11_VIEWPORT vp = { 0.0f, 0.0f, 256.0f, 256.0f, 0.0f, 1.0f };
pContext->RSSetViewports(1, &vp);
// Assert OP_RS_SET_VIEWPORT (0x0039), top_left_x=0.0f, top_left_y=0.0f, width=256.0f, height=256.0f, min_depth=0.0f, max_depth=1.0f

// 8. Output Merger Render Targets & DSV
pContext->OMSetRenderTargets(1, &pRTV, pDSV);
// Assert OP_OM_SET_RENDER_TARGETS (0x003A), rtv_id == 7, dsv_id == 9

// 9. Unbinding Verification
pContext->IASetInputLayout(nullptr);
// Assert OP_IA_SET_INPUT_LAYOUT (0x0030), res_id == 0
pContext->OMSetRenderTargets(0, nullptr, nullptr);
// Assert OP_OM_SET_RENDER_TARGETS (0x003A), rtv_id == 0, dsv_id == 0
```

---

### Suite 5: Clears & Draw Calls

#### Objective
Verify that `ClearRenderTargetView`, `ClearDepthStencilView`, `Draw`, and `DrawIndexed` serialize their parameters accurately to the network socket.

#### Execution Steps & Assertions
1. **ClearRenderTargetView**:
   - Float clear color: `FLOAT clearColor[4] = { 0.1f, 0.2f, 0.3f, 1.0f };`
   - `pContext->ClearRenderTargetView(pRTV, clearColor);`
   - Mock server receives `OP_CLEAR_RTV` (0x0052).
   - Payload struct `ClearRtvPayload`:
     - `res_id == 7`
     - `color[0] == 0.1f`, `color[1] == 0.2f`, `color[2] == 0.3f`, `color[3] == 1.0f`.
2. **ClearDepthStencilView**:
   - `pContext->ClearDepthStencilView(pDSV, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);`
   - Mock server receives `OP_CLEAR_DSV` (0x0053).
   - Payload struct `ClearDsvPayload`:
     - `res_id == 9`
     - `clear_flags == 3` (DEPTH | STENCIL)
     - `depth == 1.0f`
     - `stencil == 0`
3. **Draw (Non-indexed)**:
   - `pContext->Draw(3, 0);`
   - Mock server receives `OP_DRAW` (0x0054).
   - Payload struct `DrawPayload`:
     - `vertex_count == 3`
     - `start_vertex_location == 0`
4. **DrawIndexed**:
   - `pContext->DrawIndexed(36, 0, 0);`
   - Mock server receives `OP_DRAW_INDEXED` (0x0055).
   - Payload struct `DrawIndexedPayload`:
     - `index_count == 36`
     - `start_index_location == 0`
     - `base_vertex_location == 0`

---

### Suite 6: SwapChain Present & Loopback Frame Streaming

#### Objective
Verify that `IDXGISwapChain::Present` sends `OP_PRESENT`, enforces 1:1 frame pacing by synchronously waiting for `OP_FRAME_DATA` response, receives the streamed pixel buffer, and passes it to presentation.

#### Execution Steps & Assertions
1. **Single Frame Present**:
   - `HRESULT hr = pSwapChain->Present(1, 0);`
   - Mock server receives `OP_PRESENT` (0x0070).
   - Payload struct `PresentPayload`:
     - `swapchain_id == 0`
     - `sync_interval == 1`
     - `flags == 0`
     - `frame_number == 1`
   - Mock server automatically returns `OP_FRAME_DATA` (0x0071) with synthetic 256x256 pixel buffer.
   - `Present` unblocks and returns `S_OK`.
2. **Multi-Frame Pacing Loop (5 Frames)**:
   - Execute 5 consecutive frames:
     ```cpp
     for (int f = 2; f <= 6; ++f) {
         pContext->ClearRenderTargetView(pRTV, clearColor);
         pContext->DrawIndexed(36, 0, 0);
         HRESULT pres_hr = pSwapChain->Present(0, 0);
         TEST_ASSERT(pres_hr == S_OK);
     }
     ```
   - Mock server verifies that `present.frame_number` increments monotonically: 2, 3, 4, 5, 6.
   - Frame pacing invariant: The client never submits `frame N+1` until `frame N`'s `OP_FRAME_DATA` has been received, preventing socket bufferbloat.

---

### Suite 7: Zero-Hardware Win32 GDI Presentation & Pixel Verification

#### Objective
Verify direct Win32 GDI presentation via `GdiPresenter` onto a test `HWND` window using `SetDIBitsToDevice` / `StretchDIBits`, ensuring zero GPU acceleration requirements and pixel-level display accuracy.

#### Execution Steps & Assertions
1. **Direct `GdiPresenter` Test**:
   - Instantiate `GdiPresenter presenter;`
   - Call `presenter.Initialize(hWnd, 64, 64);`
   - Prepare synthetic 64x64 RGBA pixel array with a distinctive solid test color:
     - Red = 210, Green = 105, Blue = 30, Alpha = 255.
     - `std::vector<uint8_t> test_pixels(64 * 64 * 4);`
     - Fill each pixel with `{ 210, 105, 30, 255 }`.
2. **Blit Frame**:
   - `bool blit_ok = presenter.PresentFrame(hWnd, test_pixels.data(), 64, 64, 64 * 4);`
   - `TEST_ASSERT_MSG(blit_ok, "GdiPresenter::PresentFrame succeeded");`
3. **Programmatic Pixel Verification via Win32 GDI Readback**:
   - Query client window device context:
     `HDC hdc = GetDC(hWnd);`
     `TEST_ASSERT(hdc != nullptr);`
   - Sample center pixel at (32, 32):
     `COLORREF pixel = GetPixel(hdc, 32, 32);`
     `uint8_t r = GetRValue(pixel);`
     `uint8_t g = GetGValue(pixel);`
     `uint8_t b = GetBValue(pixel);`
   - Verify color components within GDI color tolerance ($\pm 3$ LSB):
     - `TEST_ASSERT(std::abs(static_cast<int>(r) - 210) <= 3);`
     - `TEST_ASSERT(std::abs(static_cast<int>(g) - 105) <= 3);`
     - `TEST_ASSERT(std::abs(static_cast<int>(b) - 30) <= 3);`
   - Release DC: `ReleaseDC(hWnd, hdc);`
4. **Window Resizing / Stretched Blit**:
   - Blit 64x64 pixels into 128x128 window bounds:
     `bool stretch_ok = presenter.PresentFrame(hWnd, test_pixels.data(), 64, 64, 64 * 4);`
     `TEST_ASSERT_MSG(stretch_ok, "Stretched GdiPresenter blit succeeded");`
5. **Clean Shutdown**:
   - `presenter.Shutdown();`
   - Verify zero GDI resource leaks.

---

### Suite 8: Network Fault Tolerance & Error Handling

#### Objective
Verify that unexpected network disconnection or connection drops are caught cleanly without crashes or segmentation faults, returning appropriate Direct3D COM error codes.

#### Execution Steps & Assertions
1. **Abrupt Socket Disconnect during Present**:
   - While client is running, mock server forcibly closes client connection:
     `mockServer.AbruptDisconnectClient();`
   - Client calls `pSwapChain->Present(0, 0);`
   - Assert returned `HRESULT` indicates device loss:
     - `TEST_ASSERT(hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET);`
2. **Context Operations After Disconnection**:
   - Invoking `pContext->Draw(3, 0);` or `pContext->ClearRenderTargetView(...)` after connection drop must not cause an unhandled exception or crash. The proxy silently records error status or returns safely.
3. **Graceful Disconnect via OP_SHUTDOWN**:
   - In a fresh session, client releasing device and context transmits `OP_SHUTDOWN` (0x0003) with `exit_code == 0`.
   - Mock server receives clean shutdown packet.

---

## 5. Interface Contracts & Class Signatures for `src/client/*`

To ensure perfect alignment between the explorer specifications and the implementer worker, the following class interfaces are assumed by `test_client_proxy.cpp`:

### 5.1 `src/client/proxy_device.h`
```cpp
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <memory>
#include <atomic>
#include "transport.h"

namespace RemoteGPU {

class ProxyD3D11DeviceContext;

class ProxyD3D11Device : public ID3D11Device, public IDXGIDevice {
public:
    explicit ProxyD3D11Device(std::shared_ptr<NetworkSocket> socket);
    virtual ~ProxyD3D11Device();

    // Resource ID Generator
    uint32_t AllocateResourceId() { return ++m_nextResourceId; }
    std::shared_ptr<NetworkSocket> GetSocket() const { return m_socket; }
    ProxyD3D11DeviceContext* GetProxyContext() const { return m_immediateContext.get(); }
    void SetProxyContext(std::shared_ptr<ProxyD3D11DeviceContext> ctx) { m_immediateContext = ctx; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ID3D11Device Methods (Key Subset)
    HRESULT STDMETHODCALLTYPE CreateBuffer(const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Buffer** ppBuffer) override;
    HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D) override;
    HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D11Resource* pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc, ID3D11ShaderResourceView** ppSRV) override;
    HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D11Resource* pResource, const D3D11_RENDER_TARGET_VIEW_DESC* pDesc, ID3D11RenderTargetView** ppRTView) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D11Resource* pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc, ID3D11DepthStencilView** ppDepthStencilView) override;
    HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* pInputElementDescs, UINT NumElements, const void* pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength, ID3D11InputLayout** ppInputLayout) override;
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11VertexShader** ppVertexShader) override;
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11PixelShader** ppPixelShader) override;
    HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D11_SAMPLER_DESC* pSamplerDesc, ID3D11SamplerState** ppSamplerState) override;
    void STDMETHODCALLTYPE GetImmediateContext(ID3D11DeviceContext** ppImmediateContext) override;
    HRESULT STDMETHODCALLTYPE CheckFormatSupport(DXGI_FORMAT Format, UINT* pFormatSupport) override;

    // Factory Entrypoint
    static HRESULT CreateProxyDeviceAndSwapChain(
        const char* host, uint16_t port,
        const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
        IDXGISwapChain** ppSwapChain,
        ID3D11Device** ppDevice,
        D3D_FEATURE_LEVEL* pFeatureLevel,
        ID3D11DeviceContext** ppImmediateContext
    );

private:
    std::atomic<ULONG> m_refCount{1};
    std::atomic<uint32_t> m_nextResourceId{0};
    std::shared_ptr<NetworkSocket> m_socket;
    std::shared_ptr<ProxyD3D11DeviceContext> m_immediateContext;
};

} // namespace RemoteGPU

// Standard D3D11 Entrypoint (implemented in proxy_device.cpp)
extern "C" HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* pAdapter,
    D3D_DRIVER_TYPE DriverType,
    HMODULE Software,
    UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
    IDXGISwapChain** ppSwapChain,
    ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext
);
```

### 5.2 `src/client/proxy_context.h`
```cpp
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <memory>
#include <atomic>
#include "transport.h"

namespace RemoteGPU {

class ProxyD3D11Device;

class ProxyD3D11DeviceContext : public ID3D11DeviceContext {
public:
    ProxyD3D11DeviceContext(ProxyD3D11Device* device, std::shared_ptr<NetworkSocket> socket);
    virtual ~ProxyD3D11DeviceContext();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ID3D11DeviceChild
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** ppDevice) override;

    // ID3D11DeviceContext Methods
    void STDMETHODCALLTYPE IASetInputLayout(ID3D11InputLayout* pInputLayout) override;
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppVertexBuffers, const UINT* pStrides, const UINT* pOffsets) override;
    void STDMETHODCALLTYPE IASetIndexBuffer(ID3D11Buffer* pIndexBuffer, DXGI_FORMAT Format, UINT Offset) override;
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology) override;
    void STDMETHODCALLTYPE VSSetShader(ID3D11VertexShader* pVertexShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override;
    void STDMETHODCALLTYPE VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override;
    void STDMETHODCALLTYPE PSSetShader(ID3D11PixelShader* pPixelShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override;
    void STDMETHODCALLTYPE PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override;
    void STDMETHODCALLTYPE PSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override;
    void STDMETHODCALLTYPE PSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override;
    void STDMETHODCALLTYPE RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT* pViewports) override;
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT NumViews, ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView) override;
    void STDMETHODCALLTYPE ClearRenderTargetView(ID3D11RenderTargetView* pRenderTargetView, const FLOAT ColorRGBA[4]) override;
    void STDMETHODCALLTYPE ClearDepthStencilView(ID3D11DepthStencilView* pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil) override;
    void STDMETHODCALLTYPE Draw(UINT VertexCount, UINT StartVertexLocation) override;
    void STDMETHODCALLTYPE DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) override;
    HRESULT STDMETHODCALLTYPE Map(ID3D11Resource* pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource) override;
    void STDMETHODCALLTYPE Unmap(ID3D11Resource* pResource, UINT Subresource) override;
    void STDMETHODCALLTYPE Flush() override;

private:
    std::atomic<ULONG> m_refCount{1};
    ProxyD3D11Device* m_device;
    std::shared_ptr<NetworkSocket> m_socket;
};

} // namespace RemoteGPU
```

### 5.3 `src/client/proxy_swapchain.h`
```cpp
#pragma once
#include <windows.h>
#include <dxgi.h>
#include <d3d11.h>
#include <memory>
#include <atomic>
#include "transport.h"
#include "presentation.h"

namespace RemoteGPU {

class ProxyD3D11Device;

class ProxyDXGISwapChain : public IDXGISwapChain {
public:
    ProxyDXGISwapChain(ProxyD3D11Device* device, std::shared_ptr<NetworkSocket> socket, const DXGI_SWAP_CHAIN_DESC& desc);
    virtual ~ProxyDXGISwapChain();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) override;
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) override;

private:
    std::atomic<ULONG> m_refCount{1};
    ProxyD3D11Device* m_device;
    std::shared_ptr<NetworkSocket> m_socket;
    DXGI_SWAP_CHAIN_DESC m_desc;
    HWND m_hwnd;
    uint64_t m_frameNumber = 0;
    GdiPresenter m_presenter;
    ID3D11Texture2D* m_backBuffer = nullptr;
};

} // namespace RemoteGPU
```

### 5.4 `src/client/presentation.h`
```cpp
#pragma once
#include <windows.h>
#include <cstdint>

namespace RemoteGPU {

class GdiPresenter {
public:
    GdiPresenter();
    ~GdiPresenter();

    bool Initialize(HWND hwnd, uint32_t width, uint32_t height);
    bool PresentFrame(HWND hwnd, const uint8_t* rgba_data, uint32_t width, uint32_t height, uint32_t row_pitch);
    void Shutdown();

private:
    HWND m_hwnd = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    BITMAPINFO m_bmi = {};
};

} // namespace RemoteGPU
```

---

## 6. Pass / Fail Semantics & Verification Criteria

| Metric | Target | Fail Condition |
|---|---|---|
| **Total Test Assertions** | $\ge 80$ assertions across 8 suites | $< 50$ assertions |
| **Pass Rate** | 100% | Any assertion failure (`g_tests_failed > 0`) |
| **Process Returncode** | `0` | Any non-zero exit code |
| **Execution Duration** | $< 2000$ ms for full suite run | $> 5000$ ms (indicates deadlocks or stalled sockets) |
| **GDI Resource Leaks** | 0 leaked DCs, Bitmaps, or handles | Unreleased DCs detected |
| **Memory Leaks** | 0 COM leaks on clean shutdown | Destructor counts mismatch allocation counts |

---

## 7. Recommended Implementation Ordering for Worker Agent

1. **Step 1**: Implement `MockRecordingServer` and `TestWindowHelper` classes at the top of `tests/test_client_proxy.cpp`.
2. **Step 2**: Implement `Suite 1` (Device & SwapChain creation) and `Suite 2` (Resource Creation & Monotonic `res_id` checks).
3. **Step 3**: Implement `Suite 3` (Dynamic buffer Map/Unmap) and `Suite 4` (Pipeline state bindings).
4. **Step 4**: Implement `Suite 5` (Clears and Draw calls).
5. **Step 5**: Implement `Suite 6` (SwapChain Present & loopback streaming) and `Suite 7` (GDI presentation & pixel verification).
6. **Step 6**: Implement `Suite 8` (Network fault tolerance & shutdown).
7. **Step 7**: Build with authoritative `clang++` command and execute `build/test_client_proxy.exe`, confirming exit code 0.
