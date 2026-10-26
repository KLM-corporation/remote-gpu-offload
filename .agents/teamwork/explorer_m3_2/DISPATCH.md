## 2026-10-26T18:24:23Z
You are M3 DLL Wrapper & Presentation Explorer for Milestone 3 (Client Direct3D Interception & Presentation).
Your Working Directory: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m3_2
Authoritative User Request: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\ORIGINAL_REQUEST.md
Scope Document: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\PROJECT.md
Prior Mined Spec: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\spec_miner_survey_1\spec_report.md

Scope:
Design DLL export wrapping and zero-hardware presentation for `src/client/d3d11_wrapper.cpp`, `src/client/dxgi_wrapper.cpp`, `src/client/proxy_swapchain.h`, `src/client/proxy_swapchain.cpp`, `src/client/presentation.h`, `src/client/presentation.cpp`:
1. Drop-in wrapper DLL entrypoints:
   - `d3d11.dll`: export `D3D11CreateDevice` and `D3D11CreateDeviceAndSwapChain`. Forward any other export dynamically to `C:\Windows\System32\d3d11.dll`.
   - `dxgi.dll`: export `CreateDXGIFactory`, `CreateDXGIFactory1`, `CreateDXGIFactory2`.
   - Connecting to remote GPU server on port 48888 (configurable via environment variable `RGPU_HOST` / `RGPU_PORT`, defaulting to `127.0.0.1:48888`).
2. `ProxyDXGISwapChain`:
   - Implements `IDXGISwapChain`. Captures window `HWND` from `DXGI_SWAP_CHAIN_DESC::OutputWindow`.
   - On `Present()`: sends `OP_PRESENT` packet to server; synchronously waits for `OP_FRAME_DATA` response (enforcing 1:1 frame pacing); passes received RGBA pixel buffer to `GdiPresenter`.
   - On `GetBuffer()`: returns a dummy/proxy `ID3D11Texture2D` backbuffer handle so client can call `CreateRenderTargetView`.
3. `GdiPresenter`:
   - Zero-GPU presentation via Win32 GDI `SetDIBitsToDevice` onto `GetDC(hWnd)` or `StretchDIBits` for window resizing. Requires zero hardware graphics acceleration on client (fulfilling R1 & R4).

Write your specification to:
`C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m3_2\wrapper_presentation_spec.md`
and self-contained handoff to:
`C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m3_2\handoff.md`.
Maintain progress.md. Send completion message to orchestrator.
