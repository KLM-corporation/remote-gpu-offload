## 2026-10-26T16:53:14Z
From: ed6dd465-ec42-4bab-b054-c76327c6c969 (Orchestrator)

You are M2 Server Dispatcher Explorer for Milestone 2 (Remote GPU Execution Server).
Your Working Directory: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m2_2
Authoritative User Request: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\ORIGINAL_REQUEST.md
Scope Document: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\PROJECT.md
Protocol Header: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\src\common\protocol.h
Transport Header: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\src\common\transport.h

Scope:
Design `src/server/server_dispatcher.h` and `src/server/server_dispatcher.cpp`:
1. Server Resource Manager:
   - `std::unordered_map<uint32_t, ComPtr<ID3D11DeviceChild>>` mapping client monotonic `res_id` to actual D3D11 COM objects (`ID3D11Buffer`, `ID3D11Texture2D`, `ID3D11ShaderResourceView`, `ID3D11RenderTargetView`, `ID3D11DepthStencilView`, `ID3D11VertexShader`, `ID3D11PixelShader`, `ID3D11InputLayout`).
2. Server Command Dispatcher:
   - Handling `OP_HANDSHAKE_REQUEST` -> validating protocol version and responding with `OP_HANDSHAKE_RESPONSE`.
   - Handling resource creation opcodes (`OP_CREATE_BUFFER`, `OP_CREATE_TEXTURE2D`, `OP_CREATE_SRV`, `OP_CREATE_VERTEX_SHADER`, `OP_CREATE_PIXEL_SHADER`, `OP_CREATE_INPUT_LAYOUT`, `OP_DESTROY_RESOURCE`).
   - Handling pipeline state opcodes (`OP_IA_SET_INPUT_LAYOUT`, `OP_IA_SET_VERTEX_BUFFER`, `OP_IA_SET_INDEX_BUFFER`, `OP_IA_SET_TOPOLOGY`, `OP_VS_SET_SHADER`, `OP_VS_SET_CB`, `OP_PS_SET_SHADER`, `OP_PS_SET_CB`, `OP_PS_SET_SRV`, `OP_RS_SET_VIEWPORT`, `OP_OM_SET_RENDER_TARGETS`).
   - Handling updates & draw calls (`OP_UPDATE_SUBRESOURCE`, `OP_WRITE_BUFFER_DISCARD`, `OP_CLEAR_RTV`, `OP_CLEAR_DSV`, `OP_DRAW`, `OP_DRAW_INDEXED`).
   - Handling `OP_PRESENT`: capturing framebuffer via `HostGpuDevice::ReadbackFrame` and sending `OP_FRAME_DATA` back to client socket with sequence synchronization.

Write your specification to:
`C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m2_2\server_dispatcher_spec.md`
and self-contained handoff to:
`C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m2_2\handoff.md`.
Maintain progress.md. Send completion message to orchestrator.
