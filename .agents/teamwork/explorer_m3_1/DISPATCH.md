## 2026-10-26T18:24:23Z
You are M3 Proxy COM Objects Explorer for Milestone 3 (Client Direct3D Interception & Presentation).
Your Working Directory: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m3_1
Authoritative User Request: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\ORIGINAL_REQUEST.md
Scope Document: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\PROJECT.md
Prior Mined Spec: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\spec_miner_survey_1\spec_report.md
Protocol Header: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\src\common\protocol.h
Transport Header: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\src\common\transport.h

Scope:
Design the C++ COM proxy classes for `src/client/proxy_device.h`, `src/client/proxy_device.cpp`, `src/client/proxy_context.h`, `src/client/proxy_context.cpp`:
1. `ProxyD3D11Device`:
   - Implements `ID3D11Device` (IUnknown QueryInterface, AddRef, Release).
   - Generates monotonic client-side 32-bit `res_id = ++m_nextResourceId`.
   - Serializes resource creation opcodes (`OP_CREATE_BUFFER`, `OP_CREATE_TEXTURE2D`, `OP_CREATE_SRV`, `OP_CREATE_RTV`, `OP_CREATE_DSV`, `OP_CREATE_VERTEX_SHADER`, `OP_CREATE_PIXEL_SHADER`, `OP_CREATE_INPUT_LAYOUT`) over `NetworkSocket`.
   - Wraps resources in proxy wrapper classes (`ProxyBuffer`, `ProxyTexture2D`, `ProxyShader`, etc.) storing `res_id`.
   - Handles `CreateBuffer` with initial data and dynamic usage.
2. `ProxyD3D11DeviceContext`:
   - Implements `ID3D11DeviceContext`.
   - Serializes pipeline state methods (`IASetInputLayout`, `IASetVertexBuffers`, `IASetIndexBuffer`, `IASetPrimitiveTopology`, `VSSetShader`, `VSSetConstantBuffers`, `PSSetShader`, `PSSetConstantBuffers`, `PSSetShaderResources`, `RSSetViewports`, `OMSetRenderTargets`).
   - Serializes draw calls (`Draw`, `DrawIndexed`, `ClearRenderTargetView`).
   - Dynamic buffer mapping: implements `Map(pResource, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)` by returning a pointer to client CPU shadow memory; on `Unmap()`, serializes `OP_WRITE_BUFFER_DISCARD` with the shadow buffer bytes.

Write your specification to:
`C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m3_1\proxy_com_spec.md`
and self-contained handoff to:
`C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m3_1\handoff.md`.
Maintain progress.md. Send completion message to orchestrator.
