# Milestone 3 Client Proxy Test Harness Handoff Report

**Author**: M3 Client Test Explorer (`explorer_m3_3`)  
**Target Component**: `tests/test_client_proxy.cpp`  
**Specification Document**: `C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m3_3\client_test_spec.md`  
**Date**: 2026-10-26  

---

## 1. Observation

1. **Compilation Command Invariants**:
   The authoritative build command specified for `tests/test_client_proxy.cpp` is:
   ```powershell
   clang++ -std=c++17 -Wall -Wextra -Wpedantic -O2 -Isrc/common -Isrc/client `
       src/common/transport.cpp src/client/presentation.cpp src/client/proxy_device.cpp `
       src/client/proxy_context.cpp src/client/proxy_swapchain.cpp tests/test_client_proxy.cpp `
       -o build/test_client_proxy.exe -lws2_32 -lgdi32 -ldxgi
   ```
   Directly observed:
   - `-ld3d11` is NOT linked.
   - `src/server/gpu_device.cpp` and `src/server/server_dispatcher.cpp` are NOT compiled.
   - Headers included: `-Isrc/common -Isrc/client`.

2. **Protocol and Opcode Definitions**:
   In `src/common/protocol.h`:
   - Fixed header: `PacketHeader` (16 bytes, magic = `0x52475055`).
   - Lifecycle: `OP_HANDSHAKE_REQUEST` (0x0001), `OP_HANDSHAKE_RESPONSE` (0x0002), `OP_SHUTDOWN` (0x0003).
   - Resource Creation: `OP_CREATE_BUFFER` (0x0010), `OP_CREATE_TEXTURE2D` (0x0011), `OP_CREATE_SRV` (0x0012), `OP_CREATE_RTV` (0x0013), `OP_CREATE_DSV` (0x0014), `OP_CREATE_VERTEX_SHADER` (0x0015), `OP_CREATE_PIXEL_SHADER` (0x0016), `OP_CREATE_INPUT_LAYOUT` (0x0017), `OP_DESTROY_RESOURCE` (0x0018), `OP_CREATE_SAMPLER` (0x0019).
   - Pipeline State: `OP_IA_SET_INPUT_LAYOUT` (0x0030), `OP_IA_SET_VERTEX_BUFFER` (0x0031), `OP_IA_SET_INDEX_BUFFER` (0x0032), `OP_IA_SET_TOPOLOGY` (0x0033), `OP_VS_SET_SHADER` (0x0034), `OP_VS_SET_CB` (0x0035), `OP_PS_SET_SHADER` (0x0036), `OP_PS_SET_CB` (0x0037), `OP_PS_SET_SRV` (0x0038), `OP_RS_SET_VIEWPORT` (0x0039), `OP_OM_SET_RENDER_TARGETS` (0x003A), `OP_PS_SET_SAMPLER` (0x003B).
   - Updates & Draws: `OP_WRITE_BUFFER_DISCARD` (0x0051), `OP_CLEAR_RTV` (0x0052), `OP_CLEAR_DSV` (0x0053), `OP_DRAW` (0x0054), `OP_DRAW_INDEXED` (0x0055).
   - Presentation: `OP_PRESENT` (0x0070), `OP_FRAME_DATA` (0x0071).

3. **Peer Explorer Scopes**:
   - `explorer_m3_1/DISPATCH.md`: Specifies `ProxyD3D11Device` (assigning monotonic client 32-bit `res_id = ++m_nextResourceId`, serializing resource creation opcodes) and `ProxyD3D11DeviceContext` (serializing pipeline state, draw calls, dynamic buffer Map/Unmap shadow memory).
   - `explorer_m3_2/DISPATCH.md`: Specifies `ProxyDXGISwapChain` (capturing HWND, sending `OP_PRESENT`, synchronous wait for `OP_FRAME_DATA` for 1:1 frame pacing, returning backbuffer `res_id = 1`) and `GdiPresenter` (zero-GPU Win32 GDI `SetDIBitsToDevice` / `StretchDIBits` onto `GetDC(hWnd)`).

4. **Testing Conventions**:
   In `tests/test_transport.cpp` and `tests/test_server.cpp`:
   - Lightweight zero-dependency test framework using `TEST_ASSERT`, `TEST_ASSERT_MSG`, and `RUN_TEST_SUITE`.
   - Global counters `g_tests_run`, `g_tests_passed`, `g_tests_failed`, returning 0 on all tests passing, and 1 on failure.

---

## 2. Logic Chain

1. From Observation 1, because `-ld3d11` is not linked, `D3D11CreateDeviceAndSwapChain` must be implemented directly inside the client proxy code (`src/client/proxy_device.cpp`). When `test_client_proxy.cpp` calls `D3D11CreateDeviceAndSwapChain`, it resolves directly to the proxy implementation.
2. From Observation 1, because `src/server/*` is not linked into `test_client_proxy.exe`, the test harness cannot rely on `ServerDispatcher` or `HostGpuDevice`. Therefore, an in-process `MockRecordingServer` must be implemented directly inside `tests/test_client_proxy.cpp` using `NetworkSocket` from `src/common/transport.cpp`.
3. From Observation 2 and Observation 3, `MockRecordingServer` running on a background thread over loopback TCP can intercept every packet sent by `ProxyD3D11Device`, `ProxyD3D11DeviceContext`, and `ProxyDXGISwapChain`. It can assert byte-exact header and payload compliance for every opcode, auto-respond to `OP_HANDSHAKE_REQUEST`, and return synthetic `OP_FRAME_DATA` on `OP_PRESENT`.
4. From Observation 3, `ProxyD3D11Device` allocates monotonic `res_id` values starting with the backbuffer (`res_id = 1`), followed by vertex buffers (2), index buffers (3), constant buffers (4), textures (5), etc. The test harness can strictly verify this sequence.
5. From Observation 3, dynamic buffer `Map` returns a CPU shadow buffer pointer and emits zero network packets until `Unmap()`, which emits `OP_WRITE_BUFFER_DISCARD`. The test harness can verify wire silence while mapped, followed by exact payload delivery on `Unmap()`.
6. From Observation 3, `ProxyDXGISwapChain::Present` blocks until `OP_FRAME_DATA` is returned, enforcing 1:1 frame pacing, and passes the pixel buffer to `GdiPresenter`. The test harness can verify that presenting into a Win32 test `HWND` successfully updates the device context pixels (verified via `GetDC` / `GetPixel`), confirming zero-GPU presentation without hardware graphics requirements.

---

## 3. Caveats

1. **GDI Color Roundoff**: Win32 GDI `SetDIBitsToDevice` and `GetPixel` may undergo 24-bit to 32-bit color rounding depending on display color bit-depth; assertions allow a tolerance of $\pm 3$ LSB for RGB values.
2. **Port Collisions**: To prevent port conflicts when running tests in parallel with a live server daemon on default port 48888, the test harness configures `RGPU_PORT=48889` via environment variables for loopback testing.
3. No other caveats.

---

## 4. Conclusion

The specification document `client_test_spec.md` provides a complete, self-contained blueprint for `tests/test_client_proxy.cpp`. It defines 8 structured test suites covering:
1. `D3D11CreateDeviceAndSwapChain` invocation and handshake negotiation.
2. Monotonic `res_id` allocation and binary packet serialization across 10 resource types and destruction.
3. Dynamic buffer CPU shadow memory `Map` / `Unmap` lifecycle and `OP_WRITE_BUFFER_DISCARD` payload delivery.
4. Complete pipeline state binding serialization (`IASet*`, `VSSet*`, `PSSet*`, `RSSetViewports`, `OMSetRenderTargets`).
5. Clear and Draw call serialization (`ClearRenderTargetView`, `ClearDepthStencilView`, `Draw`, `DrawIndexed`).
6. Swapchain `Present()` execution with loopback 1:1 frame pacing and `OP_FRAME_DATA` synchronization.
7. Win32 GDI zero-GPU presentation to test `HWND` and programmatic `GetPixel` validation.
8. Network fault tolerance, graceful shutdown, and disconnect recovery.

Implementing `tests/test_client_proxy.cpp` according to this specification will guarantee full validation of Milestone 3 requirements R1, R2, and R4.

---

## 5. Verification Method

Once implemented by the worker agent:

1. **Compilation Command**:
   ```powershell
   clang++ -std=c++17 -Wall -Wextra -Wpedantic -O2 -Isrc/common -Isrc/client `
       src/common/transport.cpp src/client/presentation.cpp src/client/proxy_device.cpp `
       src/client/proxy_context.cpp src/client/proxy_swapchain.cpp tests/test_client_proxy.cpp `
       -o build/test_client_proxy.exe -lws2_32 -lgdi32 -ldxgi
   ```
   - Must compile with zero warnings and zero errors.

2. **Execution Command**:
   ```powershell
   .\build\test_client_proxy.exe
   ```

3. **Pass Criteria**:
   - Exit code 0.
   - All 8 test suites report `[PASSED]`.
   - Greater than 80 total assertions run, 0 failures.
   - Total execution duration $< 2000$ ms.
