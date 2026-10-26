## 2026-10-26T17:57:21Z
You are Worker M2 (Iteration 2 Remediation) for Milestone 2: Remote GPU Execution Server.
Your Working Directory: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\worker_m2_2
Authoritative User Request: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\ORIGINAL_REQUEST.md
Scope Document: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\PROJECT.md

Exclusive Write Ownership:
- `src/server/server_dispatcher.h`
- `src/server/server_dispatcher.cpp`
You have exclusive write ownership over these 2 files. Do not modify files outside your ownership.

Remediation Specifications to Implement:
1. Bounds validation for topology and shader pipeline slots:
   Read: `C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m2_r2_1\bounds_validation_spec.md`
   Implement the exact drop-in checks:
   - `OP_IA_SET_TOPOLOGY`: validate topology enum is in valid D3D11 range (`1 <= topology <= 64` excluding reserved/undefined gaps per spec). Return false or log error and do not call `IASetPrimitiveTopology` on invalid values.
   - `OP_IA_SET_VERTEX_BUFFER`: validate `slot < 32` (`D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT`).
   - `OP_IA_SET_INDEX_BUFFER`: validate `format == DXGI_FORMAT_R16_UINT || format == DXGI_FORMAT_R32_UINT`.
   - `OP_VS_SET_CB` and `OP_PS_SET_CB`: validate `start_slot < 14` (`D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT`).
   - `OP_PS_SET_SRV`: validate `start_slot < 128` (`D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT`).
   - `OP_PS_SET_SAMPLER`: validate `start_slot < 16` (`D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT`).
   - `OP_OM_SET_RENDER_TARGETS`: validate target count <= 8 (`D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT`).
   - `OP_RS_SET_VIEWPORT`: validate `num_viewports <= 16` (`D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE`).
2. Input layout memory safety:
   Read: `C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m2_r2_2\input_layout_safety_spec.md`
   Implement `strnlen`-bounded semantic name copy, element count validation (<= 32), and format validation.
3. Verification instructions:
   Read: `C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m2_r2_3\stress_test_analysis.md`

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

Verification Requirements:
1. Compile and run adversarial stress test:
   `clang++ -std=c++17 -fms-extensions -O2 -Isrc/common -Isrc/server src/common/transport.cpp src/server/gpu_device.cpp src/server/server_dispatcher.cpp tests/test_server_stress.cpp -o build/test_server_stress.exe -ld3d11 -ldxgi -lws2_32 -ld3dcompiler -lpsapi`
   `.\build\test_server_stress.exe`
   Ensure all 181 assertions pass with 0 failures, 0 crashes, and exit code 0.
2. Compile and run standard unit test:
   `clang++ -std=c++17 -Wall -Wextra -Wpedantic -O2 -Isrc/common -Isrc/server src/common/transport.cpp src/server/gpu_device.cpp src/server/server_dispatcher.cpp tests/test_server.cpp -o build/test_server.exe -ld3d11 -ldxgi -lws2_32 -ld3dcompiler`
   `.\build\test_server.exe`
   Ensure all 81 assertions pass with exit code 0.
3. Compile standalone server daemon:
   `clang++ -std=c++17 -Wall -Wextra -Wpedantic -O2 -Isrc/common -Isrc/server src/common/transport.cpp src/server/gpu_device.cpp src/server/server_dispatcher.cpp src/server/main_server.cpp -o build/server.exe -ld3d11 -ldxgi -lws2_32 -ld3dcompiler`
4. Write your handoff report to `C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\worker_m2_2\handoff.md` with full outputs.
5. Maintain `progress.md` in your working directory.
When finished, send a message to orchestrator.
