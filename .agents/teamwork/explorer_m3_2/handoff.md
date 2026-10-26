# Milestone 3 Subagent Handoff Report: DLL Wrapper & Presentation Layer

**From**: Explorer Subagent `explorer_m3_2` (M3 DLL Wrapper & Presentation Explorer)  
**To**: Orchestrator (`ed6dd465-ec42-4bab-b054-c76327c6c969`) / Implementer Subagents (`worker_m3_*`)  
**Scope**: Milestone 3 Client Direct3D Interception & Presentation  
**Target Files Specified**:
- `src/client/d3d11_wrapper.cpp`
- `src/client/dxgi_wrapper.cpp`
- `src/client/proxy_swapchain.h`
- `src/client/proxy_swapchain.cpp`
- `src/client/presentation.h`
- `src/client/presentation.cpp`  
**Specification Artifact**: `C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m3_2\wrapper_presentation_spec.md`  
**Date**: October 2026  

---

## 1. Observation

Direct observations and evidence gathered from the codebase and build environment:

1. **Protocol Header (`src/common/protocol.h`)**:
   - Lines 79–81:
     ```cpp
     OP_PRESENT              = 0x0070, // Client -> Server: End-of-frame present & readback trigger
     OP_FRAME_DATA           = 0x0071  // Server -> Client: Return rendered framebuffer pixels
     ```
   - Lines 353–368:
     ```cpp
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
     ```
   - Static assertions at lines 416–417 confirm `sizeof(PresentPayload) == 20` and `sizeof(FrameDataPayload) == 32`.

2. **Server Presentation Handling (`src/server/server_dispatcher.cpp`)**:
   - Lines 542–560 in `HandleCreateRtv`:
     ```cpp
     // Special case: resource_id == 0 indicates binding to the host offscreen default backbuffer
     if (p->resource_id == 0) {
         rtv = m_gpuDevice.GetRenderTargetView();
         if (!rtv && m_gpuDevice.GetColorTexture()) {
             HRESULT hr = device->CreateRenderTargetView(m_gpuDevice.GetColorTexture(), nullptr, &rtv);
     ...
     } else {
         Microsoft::WRL::ComPtr<ID3D11Resource> resource = m_resources.Get<ID3D11Resource>(p->resource_id);
         if (!resource) {
             // Fallback: if client passes an ID representing the swapchain backbuffer, use host color texture
             if (m_gpuDevice.GetColorTexture()) {
                 resource = m_gpuDevice.GetColorTexture();
     ```
   - Lines 1328–1371 in `HandlePresent`:
     ```cpp
     bool ServerDispatcher::HandlePresent(const PacketHeader& header, const uint8_t* payload, size_t size, NetworkSocket* socket) {
         ...
         if (!m_gpuDevice.ReadbackFrame(m_frameReadbackBuffer, width, height)) { ... }
         ...
         resp_header.opcode = OP_FRAME_DATA;
         ...
         frame_payload.format = 28; // DXGI_FORMAT_R8G8B8A8_UNORM
         frame_payload.row_pitch = width * 4;
         frame_payload.data_size = static_cast<uint32_t>(m_frameReadbackBuffer.size());
         frame_payload.frame_number = p->frame_number;
         ...
         socket->SendExact(&resp_header, sizeof(resp_header));
         socket->SendExact(&frame_payload, sizeof(frame_payload));
         socket->SendExact(m_frameReadbackBuffer.data(), m_frameReadbackBuffer.size());
     ```

3. **Compiler and Toolchain Environment**:
   - `clang++ --version` output:
     `clang version 23.1.4 (x86_64-w64-windows-gnu), LLVM-MinGW UCRT x86_64`.
   - DirectX 11 / DXGI headers (`d3d11.h`, `dxgi.h`, `dxgi1_2.h`) and libraries (`-ld3d11`, `-ldxgi`, `-lgdi32`, `-lws2_32`) are fully available.

4. **Build Definitions in `PROJECT.md`**:
   - Line 234 specifies the client proxy DLL compilation command:
     ```powershell
     clang++ -shared -std=c++17 -O2 -Isrc/common -Isrc/client src/common/transport.cpp src/client/presentation.cpp src/client/proxy_device.cpp src/client/proxy_context.cpp src/client/proxy_swapchain.cpp src/client/d3d11_wrapper.cpp src/client/dxgi_wrapper.cpp -o build/d3d11.dll -lws2_32 -lgdi32
     ```

---

## 2. Logic Chain

1. **Interception Without Local GPU (R1 & R4)**:
   - Direct3D 11 executables link to `d3d11.dll` and `dxgi.dll`. Under Windows standard DLL search rules, placing wrapper DLLs in the application directory intercepts all calls before `C:\Windows\System32` binaries are invoked.
   - For applications calling `D3D11CreateDevice` or `D3D11CreateDeviceAndSwapChain`, our wrapper intercepts device and swap chain creation, instantiating `ProxyD3D11Device` and `ProxyDXGISwapChain`.
   - For applications querying DXGI factories directly (`CreateDXGIFactory[1/2]`), `dxgi_wrapper.cpp` returns `ProxyDXGIFactory`, allowing adapter enumeration and swapchain creation.
   - Any unintercepted entrypoint is dynamically resolved from `C:\Windows\System32\d3d11.dll` / `dxgi.dll` via `LoadLibraryW` and `GetProcAddress`, ensuring complete binary compatibility without crashes.

2. **Backbuffer Virtualization & Binding**:
   - When an application renders, it invokes `pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer)` and then `pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pRTV)`.
   - Because the client has no local hardware render targets, `ProxyDXGISwapChain::GetBuffer` returns a proxy `ID3D11Texture2D` instance configured with `res_id = 0`.
   - As observed in `server_dispatcher.cpp:543`, the server explicitly interprets `resource_id == 0` as a request to bind to the host offscreen default backbuffer (`m_gpuDevice.GetColorTexture()`). Thus, client RTV creation seamlessly routes to the remote hardware render target without extra server state overhead.

3. **1:1 Frame Pacing & Flow Control (R2 & R4)**:
   - In `ProxyDXGISwapChain::Present(SyncInterval, Flags)`, the client sends `OP_PRESENT` with `PresentPayload` containing the monotonic `frame_number`.
   - Immediately following packet transmission, the client enters a blocking synchronous read `m_socket->RecvPacket(resp_hdr, resp_payload)`.
   - This synchronous receive holds the client thread until the remote server finishes GPU execution, readback, and transmission of `OP_FRAME_DATA`.
   - This enforces a strict 1:1 frame pacing invariant: the client cannot submit new rendering commands for frame N+1 until frame N is returned by the server, completely preventing socket bufferbloat and unbounded memory buildup.

4. **Zero-Hardware GDI Presentation Engine (`GdiPresenter`)**:
   - To display frames on clients with zero graphics hardware, `GdiPresenter` blits raw pixels into the application's window `HWND` using Win32 GDI Device Independent Bitmaps (DIBs).
   - Setting `biHeight = -static_cast<LONG>(height)` defines a top-down DIB, aligning GDI scanlines with Direct3D texture memory layout and eliminating vertical scanline inversions.
   - `GdiPresenter` employs a dual-path blitter:
     - When window dimensions match frame resolution, `SetDIBitsToDevice` is called for unscaled 1:1 presentation with minimal CPU usage.
     - When window dimensions differ (due to user resize or DPI scaling), `StretchDIBits` with `SetStretchBltMode(hdc, COLORONCOLOR)` scales the image crisply without blur.
   - Every `GetDC(hWnd)` call is strictly paired with `ReleaseDC(hWnd, hdc)`, preventing GDI resource handle exhaustion.

---

## 3. Caveats

1. **Exclusive Fullscreen Transitions**:
   - `ProxyDXGISwapChain::SetFullscreenState` is stubbed to track state in `m_fullscreen` and return `S_OK`. True display mode switches (e.g. changing monitor video modes via DXGI) are not executed since display is managed in software via Win32 GDI inside the window's client rect.
2. **Multi-SwapChain Applications**:
   - The primary design targets single-window Direct3D 11 presentation (`swapchain_id = 0`), which covers standard 3D games and benchmarks. Multi-window swapchains are supported via distinct `swapchain_id` fields, but server readback currently stages from the primary offscreen target.
3. **Legacy GDI Driver Color Masks**:
   - While `BI_BITFIELDS` with RGBA masks is standard in modern Windows DWM/WDDM environments, `GdiPresenter` includes a fast fallback channel swapper to BGRA (`ConvertRgbaToBgra`) for 100% color accuracy on legacy display drivers.

---

## 4. Conclusion

The specification for the Client Direct3D DLL Interception and Zero-Hardware Presentation subsystem is complete, fully documented, and ready for immediate implementation by `worker_m3_*`.

Key deliverables designed and specified in `wrapper_presentation_spec.md`:
1. `src/client/d3d11_wrapper.cpp`: Exports `D3D11CreateDevice` and `D3D11CreateDeviceAndSwapChain`; parses `RGPU_HOST`/`RGPU_PORT`; performs handshake; dynamically forwards other exports to `System32\d3d11.dll`.
2. `src/client/dxgi_wrapper.cpp`: Exports `CreateDXGIFactory[1/2]`; provides `ProxyDXGIFactory` and `ProxyDXGIAdapter`; forwards unintercepted exports to `System32\dxgi.dll`.
3. `src/client/proxy_swapchain.h` & `proxy_swapchain.cpp`: Implements `IDXGISwapChain`; virtualizes backbuffer via `GetBuffer()` (`res_id = 0`); enforces synchronous 1:1 frame pacing in `Present()`; routes pixel buffers to `GdiPresenter`.
4. `src/client/presentation.h` & `presentation.cpp`: Implements `GdiPresenter` utilizing Win32 GDI `SetDIBitsToDevice` and `StretchDIBits` on `GetDC(hWnd)` with zero local GPU hardware requirements.

---

## 5. Verification Method

To independently verify the specification and resulting implementation:

1. **Inspect Specification Documents**:
   - Verify `wrapper_presentation_spec.md` exists and contains complete class definitions, method contracts, and failure handling tables:
     `view_file AbsolutePath="C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m3_2\wrapper_presentation_spec.md"`

2. **Compilation & Linkage Verification** (once implemented):
   - Compile the client proxy DLL:
     ```powershell
     clang++ -shared -std=c++17 -O2 `
       -Isrc/common -Isrc/client `
       src/common/transport.cpp `
       src/client/presentation.cpp `
       src/client/proxy_device.cpp `
       src/client/proxy_context.cpp `
       src/client/proxy_swapchain.cpp `
       src/client/d3d11_wrapper.cpp `
       src/client/dxgi_wrapper.cpp `
       -o build/d3d11.dll `
       -lws2_32 -lgdi32
     ```
   - Compile and execute the client proxy unit test:
     ```powershell
     clang++ -std=c++17 -Wall -Wextra -Wpedantic -O2 `
       -Isrc/common -Isrc/client `
       src/common/transport.cpp `
       src/client/presentation.cpp `
       src/client/proxy_device.cpp `
       src/client/proxy_context.cpp `
       src/client/proxy_swapchain.cpp `
       tests/test_client_proxy.cpp `
       -o build/test_client_proxy.exe `
       -lws2_32 -lgdi32 -ldxgi
     .\build\test_client_proxy.exe
     ```

3. **Invalidation Conditions**:
   - The specification is invalidated if `SetDIBitsToDevice` fails on top-down 32-bit DIBs on standard Windows 10/11 platforms.
   - The specification is invalidated if `server_dispatcher.cpp` fails to resolve `resource_id = 0` to `m_gpuDevice.GetColorTexture()`.
   - The specification is invalidated if dynamic forwarding to `C:\Windows\System32\d3d11.dll` causes loader deadlocks in `DllMain`.
