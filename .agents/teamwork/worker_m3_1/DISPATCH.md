## 2026-10-26T18:34:42Z
You are worker_m3_1, a versatile worker tasked with implementing Milestone 3: Client Direct3D Interception & Presentation Layer for the remote GPU offload system.

## 🔒 Your Identity & Working Directory
- Archetype: teamwork_preview_worker
- Working directory: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\worker_m3_1
- Parent Orchestrator ID: ed6dd465-ec42-4bab-b054-c76327c6c969

## Authoritative Documentation to Read First
1. ORIGINAL_REQUEST.md: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\ORIGINAL_REQUEST.md
2. PROJECT.md: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\PROJECT.md
3. Explorer M3_1 Spec (Proxy COM Objects): C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m3_1\proxy_com_spec.md
4. Explorer M3_2 Spec (DLL Wrapper & Presentation): C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m3_2\wrapper_presentation_spec.md
5. Explorer M3_3 Spec (Client Proxy Test Harness): C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m3_3\client_test_spec.md
6. Core headers: src/common/protocol.h, src/common/transport.h, src/server/server_dispatcher.h

## Exclusive File Ownership
You have exclusive write ownership of these files:
- src/client/proxy_resources.h
- src/client/proxy_device.h
- src/client/proxy_device.cpp
- src/client/proxy_context.h
- src/client/proxy_context.cpp
- src/client/proxy_swapchain.h
- src/client/proxy_swapchain.cpp
- src/client/presentation.h
- src/client/presentation.cpp
- src/client/d3d11_wrapper.cpp
- src/client/dxgi_wrapper.cpp
- tests/test_client_proxy.cpp

## Mandatory Implementation Requirements
1. Proxy COM Hierarchy & Refcounting:
   - Implement IUnknown and COM refcounting across all proxy objects.
   - Implement ProxyResourceBase and IProxyResource interface (with GetResourceId()).
   - Proxy resource wrappers for ID3D11Buffer, ID3D11Texture2D, ID3D11ShaderResourceView, ID3D11RenderTargetView, ID3D11DepthStencilView, ID3D11VertexShader, ID3D11PixelShader, ID3D11InputLayout, ID3D11SamplerState.
2. ProxyD3D11Device:
   - Monotonic atomic resource ID counter (res_id).
   - Serialization of creation opcodes: OP_CREATE_BUFFER, OP_CREATE_TEXTURE2D, OP_CREATE_SRV, OP_CREATE_RTV, OP_CREATE_DSV, OP_CREATE_VERTEX_SHADER, OP_CREATE_PIXEL_SHADER, OP_CREATE_INPUT_LAYOUT, OP_CREATE_SAMPLER.
   - Resource destruction notification: when refcount drops to 0, transmit OP_DESTROY_RESOURCE packet over NetworkSocket.
3. ProxyD3D11DeviceContext:
   - Serialization of pipeline state bindings: IASetInputLayout, IASetVertexBuffers, IASetIndexBuffer, IASetPrimitiveTopology, VSSetShader, VSSetConstantBuffers, PSSetShader, PSSetConstantBuffers, PSSetShaderResources, PSSetSamplers, RSSetViewports, OMSetRenderTargets.
   - Serialization of draw and clear calls: Draw, DrawIndexed, ClearRenderTargetView, ClearDepthStencilView, UpdateSubresource.
   - Dynamic buffer Map/Unmap: Return CPU shadow buffer memory in Map(..., D3D11_MAP_WRITE_DISCARD, ...). On Unmap(), serialize OP_WRITE_BUFFER_DISCARD with updated bytes matching ServerDispatcher payload expectations.
4. ProxyDXGISwapChain & 1:1 Frame Pacing:
   - Implements IDXGISwapChain. GetBuffer(0, ...) returns proxy ID3D11Texture2D backbuffer with res_id = 0 (routing to host offscreen backbuffer).
   - Present() sends OP_PRESENT packet with monotonic frame number, then synchronously blocks receiving OP_FRAME_DATA response.
   - Dispatches received pixel buffer to GdiPresenter.
5. Zero-Hardware GDI Presentation Engine (GdiPresenter):
   - Blits received pixels into target HWND client area using SetDIBitsToDevice (1:1 direct blit) or StretchDIBits (with COLORONCOLOR when scaled).
   - Top-down 32-bit DIB (biHeight = -height), BI_BITFIELDS RGBA or RGB/BGR fallback. Strict GetDC/ReleaseDC pairing.
6. DLL Wrapper Entrypoints:
   - d3d11_wrapper.cpp: Exports D3D11CreateDevice and D3D11CreateDeviceAndSwapChain. Parses RGPU_HOST/RGPU_PORT (defaulting to 127.0.0.1:48888). Performs OP_INIT_SESSION handshake. Forwards unhandled exports to System32\\d3d11.dll.
   - dxgi_wrapper.cpp: Exports CreateDXGIFactory, CreateDXGIFactory1, CreateDXGIFactory2. Returns ProxyDXGIFactory. Forwards unhandled exports to System32\\dxgi.dll.
7. Automated Client Proxy Test Suite (tests/test_client_proxy.cpp):
   - In-process loopback MockRecordingServer on ephemeral port.
   - Implements 8 comprehensive test suites verifying creation, packet serialization, dynamic buffer Map/Unmap, pipeline state setup, draw/clear commands, swapchain Present 1:1 pacing, Win32 GDI presentation with GetPixel verification on a test window, and network disconnect handling.

## Build and Test Verification
Execute the following commands to verify:
1. Compile client proxy DLL:
   clang++ -shared -std=c++17 -O2 -Isrc/common -Isrc/client src/common/transport.cpp src/client/presentation.cpp src/client/proxy_device.cpp src/client/proxy_context.cpp src/client/proxy_swapchain.cpp src/client/d3d11_wrapper.cpp src/client/dxgi_wrapper.cpp -o build/d3d11.dll -lws2_32 -lgdi32 -ldxgi
2. Compile and run client test suite:
   clang++ -std=c++17 -Wall -Wextra -Wpedantic -O2 -Isrc/common -Isrc/client src/common/transport.cpp src/client/presentation.cpp src/client/proxy_device.cpp src/client/proxy_context.cpp src/client/proxy_swapchain.cpp tests/test_client_proxy.cpp -o build/test_client_proxy.exe -lws2_32 -lgdi32 -ldxgi
   .\\build\\test_client_proxy.exe
All assertions must pass cleanly with exit code 0.
