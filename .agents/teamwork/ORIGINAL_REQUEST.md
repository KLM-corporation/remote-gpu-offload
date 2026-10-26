# Original User Request

## 2026-10-26T16:04:32Z

A networked GPU offloading system that intercepts Direct3D 11 / Direct3D 12 graphics API calls on a client Windows PC, transmits the command stream over a network socket to a remote Windows host equipped with a dedicated GPU, executes rendering on the host GPU, and streams back rendered frames to the client display.

Working directory: C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload
Integrity mode: development

## Requirements

### R1. Client Direct3D Interception Layer
A client-side proxy or hooking layer that intercepts Direct3D 11/12 device creation, pipeline state setup, resource/buffer uploads, and draw calls from a target Windows executable without requiring local hardware acceleration.

### R2. Command Stream Network Protocol & Transport
A network protocol and serialization pipeline that efficiently packs Direct3D commands, shader bytecode, vertex/index data, and textures, delivering them over standard network sockets (supporting configurable remote IP addresses as well as 127.0.0.1 loopback) with flow control and synchronization.

### R3. Remote GPU Execution Server
A server daemon designed to run on the machine with the target GPU, which binds to the host's dedicated graphics adapter, reconstructs the Direct3D device/context state from incoming command streams, executes all rendering commands to offscreen render targets, and handles client session lifecycle.

### R4. Framebuffer Streaming and Client Presentation
A readback and transfer pipeline that captures completed framebuffers from the host GPU, transmits the frame data back to the client over the network, and presents the received images into the client application's window swapchain.

### R5. Automated Test Harness & Verification Suite
A standalone Direct3D 11/12 3D sample application (e.g., rendering a dynamic textured 3D mesh) and an automated test runner script that can execute server and client instances in loopback mode (127.0.0.1), objectively verifying end-to-end command serialization, execution, and non-blank frame presentation without requiring human intervention.

## Acceptance Criteria

### Network & Command Protocol
- [ ] Server and client establish a reliable network handshake over TCP or UDP with configurable target port and host address.
- [ ] Direct3D resource allocation, shader compilation/binding, and draw calls are serialized on the client and successfully parsed by the server without stream desynchronization.
- [ ] System operates correctly over 127.0.0.1 loopback for automated self-testing as well as across distinct LAN IP addresses.

### Graphics Rendering & GPU Offloading
- [ ] The remote server explicitly enumerates and binds to the host's dedicated GPU adapter, logging the active adapter's hardware description string.
- [ ] Offscreen render targets on the host GPU accurately execute the submitted vertex, geometry/rasterization, and pixel shader operations.
- [ ] Client receives returned frame buffers and presents them in its window at a measurable frame rate.

### Automated Verification & Robustness
- [ ] Running the automated test harness launches both server and client in loopback mode, executes at least 60 rendered frames, and exits cleanly with return code 0.
- [ ] The test harness programmatically captures at least one rendered frame and verifies that the output contains valid 3D render pixels (non-empty, non-zero luminance).
- [ ] Graceful error handling and shutdown occur if the network connection is dropped or closed by either party.
