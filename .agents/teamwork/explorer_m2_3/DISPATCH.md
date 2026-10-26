## 2026-10-26T16:53:14Z
You are M2 Server Main & Test Explorer for Milestone 2 (Remote GPU Execution Server).
Your Working Directory: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m2_3
Authoritative User Request: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\ORIGINAL_REQUEST.md
Scope Document: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\PROJECT.md

Scope:
Design `src/server/main_server.cpp` and `tests/test_server.cpp`:
1. `src/server/main_server.cpp`:
   - Standalone server executable accepting CLI arguments (`--port <port>`, `--adapter <index>`).
   - Initializes `HostGpuDevice`, binds to dedicated adapter, prints active GPU description.
   - Listens on `NetworkSocket`, accepts client connection, instantiates `ServerDispatcher`, processes commands in a loop, handles clean disconnect without crashing, and exits cleanly.
2. `tests/test_server.cpp`:
   - Comprehensive unit test: initializes `HostGpuDevice`, creates offscreen 256x256 render target, creates a simple colored triangle pipeline, issues draw call, executes staging readback.
   - Verifies pixel buffer output: verifies background clear color, verifies rasterized triangle pixels (non-clear, non-zero luminance), verifies row pitch packing.
   - Verifies server dispatcher loopback handling with synthetic packet sequence.
   - Build command: `clang++ -std=c++17 -O2 -Isrc/common -Isrc/server src/common/transport.cpp src/server/gpu_device.cpp src/server/server_dispatcher.cpp tests/test_server.cpp -o build/test_server.exe -ld3d11 -ldxgi -lws2_32 -ld3dcompiler`.
   - Exit code 0 on success.

Write your specification to:
`C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m2_3\server_main_test_spec.md`
and self-contained handoff to:
`C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\.agents\teamwork\explorer_m2_3\handoff.md`.
Maintain progress.md. Send completion message to orchestrator.
