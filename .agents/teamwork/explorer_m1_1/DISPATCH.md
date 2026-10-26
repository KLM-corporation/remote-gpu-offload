## 2026-10-26T16:18:29Z
You are M1 Protocol Spec Explorer for Milestone 1 (Network Protocol & Transport Layer).
Your Working Directory: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m1_1
Authoritative User Request: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\ORIGINAL_REQUEST.md
Scope Document: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\PROJECT.md
Prior Survey Report: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_survey_1\transport_report.md

Milestone 1 Scope:
Create `src/common/protocol.h`, `src/common/transport.h`, `src/common/transport.cpp`, and unit test `tests/test_transport.cpp`.

Your Task:
Investigate and produce the precise implementation specification for `src/common/protocol.h`:
1. All 32 D3D11 opcode enum values, flags (FLAG_NONE, FLAG_ONEWAY, FLAG_COMPRESSED).
2. Fixed 16-byte PacketHeader struct definition with `#pragma pack(push, 1)`.
3. Every payload struct:
   - HandshakeRequest / HandshakeResponse
   - CreateBufferPayload (res_id, byte_width, usage, bind_flags, cpu_access_flags, misc_flags, has_initial_data)
   - CreateTexture2DPayload (res_id, width, height, mips, array_size, format, sample_count, sample_quality, usage, bind_flags, cpu_access_flags, misc_flags)
   - CreateShaderPayload (res_id, bytecode_length)
   - CreateInputLayoutPayload (res_id, num_elements, bytecode_length)
   - UpdateSubresourcePayload (res_id, subresource, dst_box, row_pitch, depth_pitch, data_size)
   - WriteBufferDiscardPayload (res_id, byte_offset, byte_width)
   - DrawPayload (vertex_count, start_vertex_location)
   - DrawIndexedPayload (index_count, start_index_location, base_vertex_location)
   - ClearRtvPayload (res_id, color[4])
   - PresentPayload (swapchain_id, sync_interval, flags)
   - FrameDataPayload (swapchain_id, width, height, format, row_pitch, data_size)
4. Ensure 100% C++17 compatibility, standard integer types (`<cstdint>`), Windows header compatibility, and zero undefined behavior.

Write your report to:
`C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m1_1\protocol_spec.md`
and self-contained handoff to:
`C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m1_1\handoff.md`.
Maintain progress.md in your working directory. Send message to orchestrator upon completion.
