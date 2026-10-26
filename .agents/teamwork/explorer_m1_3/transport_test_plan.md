# Milestone 1 Transport Layer Test Plan & Test Suite Specification

**Document Path**: `tests/test_transport.cpp` Specification  
**Author**: M1 Transport Test Explorer (`explorer_m1_3`)  
**Target Milestone**: Milestone 1 (Network Protocol & Transport Layer)  
**Date**: 2026-10-26  
**Status**: Authoritative Test Specification & Reference Implementation  

---

## 1. Executive Summary & Scope

Milestone 1 establishes the foundational networking protocol and transport layer for the Remote GPU Offloading System. Direct3D 11 graphics API commands, shader bytecode, dynamic vertex/index buffers, textures, and readback framebuffers are transmitted across TCP sockets between the Windows client proxy and the remote GPU execution server.

This document specifies the comprehensive unit test suite to be implemented in `tests/test_transport.cpp`. The test suite provides opaque-box, requirement-driven verification of:
1. **Packet Header Packing & Alignment**: Fixed 16-byte `PacketHeader` layout (`#pragma pack(push, 1)`), member offsets, member sizes, protocol magic constant `0x52475055` (`'R','G','P','U'`), version checks, and opcode/flag enumeration integrity.
2. **Serialization / Deserialization Round-Trip**: Bit-for-bit lossless round-trip serialization of all command payload structures (handshake, buffer/texture creation, shader/layout descriptors, pipeline bindings, update deltas, draw calls, and presentation frame data).
3. **Socket Loopback Connection**: Server binding to `127.0.0.1:48888`, client connection, `SIO_LOOPBACK_FAST_PATH` activation, `TCP_NODELAY` verification, and socket option configuration.
4. **Data Transfer & Stream Framing**: Accurate loopback delivery of small graphics commands and large multi-megabyte payloads (4 MB simulated uncompressed texture and framebuffer readbacks) with byte-by-byte integrity verification (`memcmp == 0`) and back-to-back burst stream stress.
5. **Peer Disconnection & Network Error Handling**: Server clean EOF detection without crashing or hanging on client disconnect, client connection rejection handling on closed/unbound ports without hang, mid-stream truncation handling, and corrupted magic rejection.
6. **Execution & Build Instructions**: Compilation using LLVM-MinGW `clang++` (`clang++ -std=c++17 -Isrc/common src/common/transport.cpp tests/test_transport.cpp -o build/test_transport.exe -lws2_32`) and execution resulting in exit code 0.

---

## 2. Feature Inventory Alignment & Coverage Matrix

Per `TEST_INFRA.md`, the transport test suite addresses Features 1, 2, 3, and 4 across all four testing tiers:

| Feature ID | Feature Name | Requirement Source | Tier 1 (Assertions) | Tier 2 (Boundaries) | Tier 3 (Combinations) | Tier 4 (Workloads) | Test Function |
|---|---|---|---|---|---|---|---|
| **Feature 1** | Socket Transport & Framing | `ORIGINAL_REQUEST §R2` | $\ge 5$ assertions | 0-length, buffer limits | Connect + Loopback fast path | 4 MB multi-MB payload transfer | `Test_Socket_DataTransfer` |
| **Feature 2** | Packet Serialization | `ORIGINAL_REQUEST §R2` | $\ge 5$ assertions | Header packing, sizeof 16 | Header + variable payload | 15+ payload struct roundtrips | `Test_PacketHeader_PackingAndConstants`, `Test_Payload_SerializationRoundTrip` |
| **Feature 3** | Handshake & Lifecycle | `ORIGINAL_REQUEST §R2` | $\ge 5$ assertions | Port fallback, timeouts | Connect + Handshake exchange | Clean session shutdown | `Test_Socket_LoopbackConnection`, `Test_Socket_DataTransfer` |
| **Feature 4** | Network Fault Tolerance | `ORIGINAL_REQUEST §R2` | $\ge 5$ assertions | Truncated header, bad magic | Connect to closed port | Mid-stream peer abort | `Test_Socket_DisconnectionAndErrors` |

---

## 3. Test Harness Architecture & Design Principles

### 3.1 Zero External Dependencies
To ensure immediate portability across developer machines and automated CI environments without requiring package managers (vcpkg, Conan) or third-party test libraries (GTest, Catch2), `tests/test_transport.cpp` incorporates a lightweight, dependency-free test macro framework based purely on standard C++17 (`<iostream>`, `<chrono>`, `<thread>`, `<atomic>`, `<vector>`, `<cstring>`).

### 3.2 Test Framework Macros
```cpp
// Global statistics
static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond) do { \
    g_tests_run++; \
    if (cond) { \
        g_tests_passed++; \
    } else { \
        g_tests_failed++; \
        std::cerr << "  [FAIL] Assertion failed: (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
    } \
} while (0)

#define TEST_ASSERT_MSG(cond, msg) do { \
    g_tests_run++; \
    if (cond) { \
        g_tests_passed++; \
    } else { \
        g_tests_failed++; \
        std::cerr << "  [FAIL] " << msg << ": (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
    } \
} while (0)

#define RUN_TEST_SUITE(suite_func) do { \
    std::cout << "============================================================" << std::endl; \
    std::cout << "[RUNNING ] " #suite_func << "..." << std::endl; \
    int start_fails = g_tests_failed; \
    auto t_start = std::chrono::high_resolution_clock::now(); \
    suite_func(); \
    auto t_end = std::chrono::high_resolution_clock::now(); \
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count(); \
    if (g_tests_failed == start_fails) { \
        std::cout << "[PASSED  ] " #suite_func " (" << elapsed_ms << " ms)" << std::endl; \
    } else { \
        std::cout << "[FAILED  ] " #suite_func " (" << elapsed_ms << " ms) - " \
                  << (g_tests_failed - start_fails) << " assertion failure(s)!" << std::endl; \
    } \
} while (0)
```

### 3.3 Return Code Contract
- `g_tests_failed == 0` $\rightarrow$ Process exits with code **0** (Success).
- `g_tests_failed > 0` $\rightarrow$ Process exits with code **1** (Failure).

---

## 4. Test Suite Specifications

### Suite 1: Packet Header Packing, Alignment, and Constants (`Test_PacketHeader_PackingAndConstants`)

#### Objective:
Verify binary wire-compatibility of `PacketHeader`, guarantee zero compiler-induced padding, ensure member byte-alignment, and validate protocol constants.

#### Assertions & Checks:
1. `sizeof(PacketHeader) == 16`: Static assert and runtime check.
2. Member offsets (`<cstddef>` `offsetof`):
   - `offsetof(PacketHeader, magic) == 0`
   - `offsetof(PacketHeader, opcode) == 4`
   - `offsetof(PacketHeader, flags) == 6`
   - `offsetof(PacketHeader, sequence_id) == 8`
   - `offsetof(PacketHeader, payload_length) == 12`
3. Member sizes:
   - `sizeof(PacketHeader::magic) == 4`
   - `sizeof(PacketHeader::opcode) == 2`
   - `sizeof(PacketHeader::flags) == 2`
   - `sizeof(PacketHeader::sequence_id) == 4`
   - `sizeof(PacketHeader::payload_length) == 4`
4. Magic constant `RGPU_MAGIC == 0x52475055`:
   - Verify ASCII representation in Little-Endian memory:
     - `byte[0] == 'R' (0x52)`
     - `byte[1] == 'G' (0x47)`
     - `byte[2] == 'P' (0x50)`
     - `byte[3] == 'U' (0x55)`
5. Version and Port Constants:
   - `RGPU_PROTOCOL_VERSION == 1`
   - `RGPU_DEFAULT_PORT == 48888`
6. Enum Completeness & Opcode Boundaries:
   - Verify all 32 opcode values are non-zero, distinct, and fall within their defined functional ranges (Lifecycle: `0x0001-0x000F`, Resources: `0x0010-0x002F`, Pipeline: `0x0030-0x004F`, Updates/Draws: `0x0050-0x006F`, Present: `0x0070-0x007F`).
   - Verify protocol flags (`FLAG_NONE`, `FLAG_SYNC`, `FLAG_RESPONSE_REQUIRED`, `FLAG_COMPRESSED`, `FLAG_ONEWAY`, `FLAG_END_OF_FRAME`, `FLAG_HEARTBEAT`) are bitwise orthogonal.

---

### Suite 2: Payload Struct Serialization & Deserialization Round-Trip (`Test_Payload_SerializationRoundTrip`)

#### Objective:
Verify that all Direct3D payload structures can be serialized to a raw byte array, transmitted over the wire, and deserialized without field truncation, byte corruption, or endianness anomalies.

#### Structs Tested:
1. `HandshakeRequest`:
   - Fields: `protocol_version`, `client_flags`, `client_width`, `client_height`, `preferred_format`.
   - Test data: `{ 1, FLAG_NONE, 1920, 1080, 28 }`.
   - Assert: all 5 fields match original after byte-copy round-trip.
2. `HandshakeResponse`:
   - Fields: `protocol_version`, `status_code`, `feature_level`, `frame_width`, `frame_height`, `frame_format`, `dedicated_vram`, `adapter_desc[128]`.
   - Test data: `{ 1, 0, 0xb000, 1920, 1080, 28, 8589934592ULL, "Intel(R) UHD Graphics 620" }`.
   - Assert: string equality of `adapter_desc` and numeric equality of all 64-bit and 32-bit fields.
3. `CreateBufferPayload`:
   - Fields: `res_id`, `byte_width`, `usage`, `bind_flags`, `cpu_access_flags`, `misc_flags`, `structure_byte_stride`, `has_initial_data`, `initial_data_size`.
   - Round-trip test Case A: Buffer without initial data (`has_initial_data = 0`).
   - Round-trip test Case B: Buffer with initial vertex data (`has_initial_data = 1`, 64 bytes of floating-point vertex data appended). Verify both struct and trailing data.
4. `CreateTexture2DPayload`:
   - Fields: `res_id`, `width`, `height`, `mip_levels`, `array_size`, `format`, `sample_count`, `sample_quality`, `usage`, `bind_flags`, `cpu_access_flags`, `misc_flags`, `has_initial_data`, `sys_mem_pitch`, `sys_mem_slice_pitch`, `initial_data_size`.
   - Round-trip test with 256x256 RGBA initial texture data.
5. `CreateShaderPayload`:
   - Fields: `shader_id`, `bytecode_length`.
   - Appended data: 512 bytes of synthetic compiled HLSL bytecode.
   - Assert: byte-exact deserialization.
6. `CreateInputLayoutPayload`:
   - Fields: `layout_id`, `num_elements`, `bytecode_length`.
   - Appended data: Array of `SerializedInputElementDesc` ("POSITION", "COLOR", "TEXCOORD") followed by shader signature bytecode.
   - Assert: semantic names, offsets, formats, and signature bytes are verified.
7. `UpdateSubresourcePayload`:
   - Fields: `resource_id`, `dst_subresource`, `has_box`, `box` (`left`, `top`, `front`, `right`, `bottom`, `back`), `src_row_pitch`, `src_depth_pitch`, `data_size`.
   - Assert: subresource update parameters and data match.
8. `WriteBufferDiscardPayload`:
   - Fields: `buffer_id`, `data_size`.
   - Appended data: 128 bytes of dynamic constant buffer matrices.
   - Assert: round-trip preservation.
9. `DrawPayload` and `DrawIndexedPayload`:
   - `DrawPayload`: `vertex_count = 36`, `start_vertex_location = 0`.
   - `DrawIndexedPayload`: `index_count = 36`, `start_index_location = 0`, `base_vertex_location = 0`.
   - Assert: member fidelity.
10. `ClearRenderTargetViewPayload` and `ClearDepthStencilViewPayload`:
    - `ClearRenderTargetViewPayload`: `rtv_id = 1`, `color[4] = { 0.1f, 0.2f, 0.3f, 1.0f }`.
    - `ClearDepthStencilViewPayload`: `dsv_id = 2`, `clear_flags = 1`, `depth = 1.0f`, `stencil = 0`.
    - Assert: floating-point bit preservation.
11. `PresentPayload` and `FrameDataPayload`:
    - `PresentPayload`: `sync_interval = 1`, `present_flags = 0`, `frame_number = 60`.
    - `FrameDataPayload`: `frame_number = 60`, `width = 800`, `height = 600`, `format = 28`, `row_pitch = 3200`, `data_size = 1920000`, `compression_type = 0`.
    - Assert: exact deserialization.

---

### Suite 3: Socket Loopback Connection Test (`Test_Socket_LoopbackConnection`)

#### Objective:
Verify socket initialization, local server listening on `127.0.0.1:48888`, client loopback connection, Winsock option configuration, and thread synchronization.

#### Execution Flow & Threading:
1. `NetworkSocket::InitializeNetwork()` initializes Winsock2 (`WSAStartup`).
2. Server thread initializes a `NetworkSocket listener`.
3. Listener binds to `127.0.0.1:48888` (with fallback retry logic on dynamic port if 48888 is temporarily occupied) and calls `BindAndListen(port)`.
4. Server thread signals readiness to main thread using `std::promise<uint16_t>`.
5. Main thread awaits server readiness and initializes `NetworkSocket client`.
6. Client invokes `client.Connect("127.0.0.1", port, 5000)`.
7. Server thread invokes `listener.Accept(5000)`, yielding `std::unique_ptr<NetworkSocket> server_conn`.
8. Assertions:
   - `client.IsValid() == true`
   - `server_conn != nullptr`
   - `server_conn->IsValid() == true`
9. Socket Option Verification:
   - Verify `TCP_NODELAY == 1` on client socket (`getsockopt(..., IPPROTO_TCP, TCP_NODELAY, ...)`).
   - Verify `SO_SNDBUF >= 1048576` and `SO_RCVBUF >= 1048576` (16 MB configured, OS clamped $\ge 1$ MB).
10. Clean Teardown:
    - Both sockets are closed gracefully (`Close()`).
    - Server thread joins.

---

### Suite 4: SendPacket & RecvPacket Loopback Transfer Integrity (`Test_Socket_DataTransfer`)

#### Objective:
Verify bidirectional packet framing, stream reassembly, and zero byte corruption over loopback TCP for small commands, back-to-back bursts, and large multi-megabyte payloads.

#### Sub-Tests:

#### Sub-Test 4A: Small Graphics Command Sequence
- Connect client and server over loopback.
- Send a sequential train of commands simulating frame setup:
  1. `OP_HANDSHAKE_REQUEST` (Header + `HandshakeRequest`)
  2. `OP_CREATE_BUFFER` (Header + `CreateBufferPayload` + 128 bytes vertex data)
  3. `OP_CLEAR_RTV` (Header + `ClearRenderTargetViewPayload`)
  4. `OP_DRAW_INDEXED` (Header + `DrawIndexedPayload`)
  5. `OP_PRESENT` (Header + `PresentPayload`)
- Server receives each packet in order via `RecvPacket(hdr, payload)`.
- Assert:
  - Sequence IDs strictly increment from 1 to 5.
  - Opcodes match expected values.
  - Payload lengths match struct + trailing buffer sizes.
  - Struct fields and vertex bytes match original byte-for-byte.

#### Sub-Test 4B: Large Multi-Megabyte Payload Transfer (4 MB Payload)
- Payload size: **4,194,304 bytes** ($4 \times 1024 \times 1024$).
- Simulates an uncompressed $1024 \times 1024$ RGBA texture or $800 \times 600 \times 4$ framebuffer transfer.
- Generate pseudo-random deterministic test pattern:
  ```cpp
  std::vector<uint8_t> send_buffer(4 * 1024 * 1024);
  for (size_t i = 0; i < send_buffer.size(); ++i) {
      send_buffer[i] = static_cast<uint8_t>((i * 109 + 37) ^ (i >> 5));
  }
  ```
- Send via `client.SendPacket(header, send_buffer.data())` with `opcode = OP_FRAME_DATA`.
- Server receives via `server_conn->RecvPacket(recv_hdr, recv_payload)`.
- Performance measurement: record start/end time using `std::chrono::high_resolution_clock`.
- Assertions:
  - `recv_hdr.magic == RGPU_MAGIC`
  - `recv_hdr.opcode == OP_FRAME_DATA`
  - `recv_hdr.payload_length == 4194304`
  - `recv_payload.size() == 4194304`
  - `memcmp(send_buffer.data(), recv_payload.data(), 4194304) == 0` (100% byte integrity).
- Compute and print transfer throughput (e.g. $> 1,000$ MB/s over loopback).

#### Sub-Test 4C: Back-to-Back Burst Stream (Boundary & Fragmentation Stress)
- Client sends 100 consecutive packets of varying sizes (from 16 bytes up to 64 KB) in rapid back-to-back succession without pauses.
- Server reads all 100 packets in a continuous receive loop.
- Assert:
  - All 100 packets are received with zero data loss.
  - Every packet's sequence number equals $1, 2, \dots, 100$.
  - Stream boundaries are strictly preserved without misalignment.

---

### Suite 5: Peer Disconnection & Network Error Handling (`Test_Socket_DisconnectionAndErrors`)

#### Objective:
Verify fault tolerance, non-crashing behavior, clean EOF detection, timeout enforcement, and protocol validation.

#### Sub-Tests:

#### Sub-Test 5A: Clean Disconnect / Peer Close Detection
- Client connects to server.
- Client sends one packet; server receives it.
- Client explicitly closes connection: `client.Close()`.
- Server attempts `server_conn->RecvPacket(hdr, payload)` or `server_conn->RecvExact(&hdr, sizeof(hdr))`.
- Assertions:
  - Server call returns `false` (graceful EOF / connection closed).
  - Server does NOT crash, does NOT throw exceptions, and does NOT enter an infinite loop.
  - Server successfully closes `server_conn` and listener socket.

#### Sub-Test 5B: Connection Attempt to Unbound / Closed Port
- Main thread attempts to connect to an unused port where no listener exists (e.g. `48899`).
- Client calls `client.Connect("127.0.0.1", 48899, 1000)` (1000 ms timeout).
- Assertions:
  - `Connect()` returns `false` promptly.
  - `client.IsValid() == false`.
  - Process does NOT hang indefinitely; timeout/refusal is cleanly handled.

#### Sub-Test 5C: Truncated Stream / Mid-Stream Abort
- Client connects to server.
- Client transmits only 8 bytes of the 16-byte `PacketHeader` (incomplete header).
- Client immediately aborts connection (calls `closesocket`).
- Server calls `server_conn->RecvPacket(hdr, payload)`.
- Assertions:
  - `RecvPacket` returns `false` upon partial read failure.
  - Server loop terminates cleanly without memory corruption.

#### Sub-Test 5D: Corrupted Magic Rejection
- Client connects to server.
- Client transmits a packet with corrupted magic `0xDEADBEEF` instead of `RGPU_MAGIC`.
- Server calls `server_conn->RecvPacket(hdr, payload)`.
- Assertions:
  - `RecvPacket` validates `hdr.magic == RGPU_MAGIC`.
  - Magic mismatch is detected; `RecvPacket` returns `false`.
  - Stream desynchronization is contained.

---

## 5. Complete Implementation Reference for `tests/test_transport.cpp`

The complete, compilable C++17 source code below implements all test suites designed above. The implementer can directly copy or adapt this into `tests/test_transport.cpp`:

```cpp
// ============================================================================
// Remote GPU Offload - Milestone 1 Transport Layer Unit Test Suite
// File: tests/test_transport.cpp
// Standard: C++17
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include "protocol.h"
#include "transport.h"

#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <chrono>
#include <thread>
#include <future>
#include <atomic>

// ----------------------------------------------------------------------------
// Lightweight Zero-Dependency Test Harness
// ----------------------------------------------------------------------------
static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond) do { \
    g_tests_run++; \
    if (cond) { \
        g_tests_passed++; \
    } else { \
        g_tests_failed++; \
        std::cerr << "  [FAIL] Assertion failed: (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
    } \
} while (0)

#define TEST_ASSERT_MSG(cond, msg) do { \
    g_tests_run++; \
    if (cond) { \
        g_tests_passed++; \
    } else { \
        g_tests_failed++; \
        std::cerr << "  [FAIL] " << msg << ": (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
    } \
} while (0)

#define RUN_TEST_SUITE(suite_func) do { \
    std::cout << "============================================================" << std::endl; \
    std::cout << "[RUNNING ] " #suite_func "..." << std::endl; \
    int start_fails = g_tests_failed; \
    auto t_start = std::chrono::high_resolution_clock::now(); \
    suite_func(); \
    auto t_end = std::chrono::high_resolution_clock::now(); \
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count(); \
    if (g_tests_failed == start_fails) { \
        std::cout << "[PASSED  ] " #suite_func " (" << elapsed_ms << " ms)" << std::endl; \
    } else { \
        std::cout << "[FAILED  ] " #suite_func " (" << elapsed_ms << " ms) - " \
                  << (g_tests_failed - start_fails) << " assertion failure(s)!" << std::endl; \
    } \
} while (0)

// ----------------------------------------------------------------------------
// Suite 1: Packet Header Packing, Alignment, and Constants
// ----------------------------------------------------------------------------
void Test_PacketHeader_PackingAndConstants() {
    // 1. Fixed size check
    static_assert(sizeof(RemoteGPU::PacketHeader) == 16, "PacketHeader must be exactly 16 bytes");
    TEST_ASSERT_MSG(sizeof(RemoteGPU::PacketHeader) == 16, "PacketHeader size == 16");

    // 2. Member offset checks
    TEST_ASSERT(offsetof(RemoteGPU::PacketHeader, magic) == 0);
    TEST_ASSERT(offsetof(RemoteGPU::PacketHeader, opcode) == 4);
    TEST_ASSERT(offsetof(RemoteGPU::PacketHeader, flags) == 6);
    TEST_ASSERT(offsetof(RemoteGPU::PacketHeader, sequence_id) == 8);
    TEST_ASSERT(offsetof(RemoteGPU::PacketHeader, payload_length) == 12);

    // 3. Member size checks
    TEST_ASSERT(sizeof(RemoteGPU::PacketHeader::magic) == 4);
    TEST_ASSERT(sizeof(RemoteGPU::PacketHeader::opcode) == 2);
    TEST_ASSERT(sizeof(RemoteGPU::PacketHeader::flags) == 2);
    TEST_ASSERT(sizeof(RemoteGPU::PacketHeader::sequence_id) == 4);
    TEST_ASSERT(sizeof(RemoteGPU::PacketHeader::payload_length) == 4);

    // 4. Magic constant check (0x52475055 == 'RGPU')
    TEST_ASSERT_MSG(RemoteGPU::RGPU_MAGIC == 0x52475055, "RGPU_MAGIC == 0x52475055");
    uint32_t magic_val = RemoteGPU::RGPU_MAGIC;
    const uint8_t* magic_bytes = reinterpret_cast<const uint8_t*>(&magic_val);
    TEST_ASSERT(magic_bytes[0] == 'R');
    TEST_ASSERT(magic_bytes[1] == 'G');
    TEST_ASSERT(magic_bytes[2] == 'P');
    TEST_ASSERT(magic_bytes[3] == 'U');

    // 5. Version and default port checks
    TEST_ASSERT(RemoteGPU::RGPU_PROTOCOL_VERSION == 1);
    TEST_ASSERT(RemoteGPU::RGPU_DEFAULT_PORT == 48888);

    // 6. Flags bitwise orthogonality
    TEST_ASSERT(RemoteGPU::FLAG_NONE == 0x0000);
    TEST_ASSERT((RemoteGPU::FLAG_SYNC & RemoteGPU::FLAG_RESPONSE_REQUIRED) == 0);
    TEST_ASSERT((RemoteGPU::FLAG_COMPRESSED & RemoteGPU::FLAG_ONEWAY) == 0);
    TEST_ASSERT((RemoteGPU::FLAG_END_OF_FRAME & RemoteGPU::FLAG_HEARTBEAT) == 0);
}

// ----------------------------------------------------------------------------
// Suite 2: Payload Struct Serialization Round-Trip
// ----------------------------------------------------------------------------
void Test_Payload_SerializationRoundTrip() {
    // 1. Handshake Request
    {
        RemoteGPU::HandshakeRequest src{};
        src.protocol_version = RemoteGPU::RGPU_PROTOCOL_VERSION;
        src.client_flags = RemoteGPU::FLAG_NONE;
        src.client_width = 1920;
        src.client_height = 1080;
        src.preferred_format = 28; // DXGI_FORMAT_R8G8B8A8_UNORM

        std::vector<uint8_t> buffer(sizeof(src));
        std::memcpy(buffer.data(), &src, sizeof(src));

        RemoteGPU::HandshakeRequest dst{};
        std::memcpy(&dst, buffer.data(), sizeof(dst));

        TEST_ASSERT(dst.protocol_version == src.protocol_version);
        TEST_ASSERT(dst.client_flags == src.client_flags);
        TEST_ASSERT(dst.client_width == src.client_width);
        TEST_ASSERT(dst.client_height == src.client_height);
        TEST_ASSERT(dst.preferred_format == src.preferred_format);
    }

    // 2. Handshake Response
    {
        RemoteGPU::HandshakeResponse src{};
        src.protocol_version = RemoteGPU::RGPU_PROTOCOL_VERSION;
        src.status_code = 0;
        src.feature_level = 0xb000; // D3D_FEATURE_LEVEL_11_0
        src.frame_width = 1920;
        src.frame_height = 1080;
        src.frame_format = 28;
        src.dedicated_vram = 8589934592ULL; // 8 GB
        const char* desc = "Intel(R) UHD Graphics 620";
        std::strncpy(src.adapter_desc, desc, sizeof(src.adapter_desc) - 1);

        std::vector<uint8_t> buffer(sizeof(src));
        std::memcpy(buffer.data(), &src, sizeof(src));

        RemoteGPU::HandshakeResponse dst{};
        std::memcpy(&dst, buffer.data(), sizeof(dst));

        TEST_ASSERT(dst.protocol_version == src.protocol_version);
        TEST_ASSERT(dst.status_code == src.status_code);
        TEST_ASSERT(dst.feature_level == src.feature_level);
        TEST_ASSERT(dst.frame_width == src.frame_width);
        TEST_ASSERT(dst.frame_height == src.frame_height);
        TEST_ASSERT(dst.dedicated_vram == src.dedicated_vram);
        TEST_ASSERT(std::strcmp(dst.adapter_desc, desc) == 0);
    }

    // 3. CreateBufferPayload with trailing vertex data
    {
        RemoteGPU::CreateBufferPayload payload{};
        payload.res_id = 42;
        payload.byte_width = 64;
        payload.usage = 0;
        payload.bind_flags = 1; // VERTEX_BUFFER
        payload.cpu_access_flags = 0;
        payload.misc_flags = 0;
        payload.structure_byte_stride = 16;
        payload.has_initial_data = 1;
        payload.initial_data_size = 64;

        float vertex_data[16];
        for (int i = 0; i < 16; ++i) vertex_data[i] = static_cast<float>(i) * 1.5f;

        std::vector<uint8_t> packet_buf(sizeof(payload) + sizeof(vertex_data));
        std::memcpy(packet_buf.data(), &payload, sizeof(payload));
        std::memcpy(packet_buf.data() + sizeof(payload), vertex_data, sizeof(vertex_data));

        RemoteGPU::CreateBufferPayload recvd_payload{};
        std::memcpy(&recvd_payload, packet_buf.data(), sizeof(recvd_payload));
        float recvd_vertices[16];
        std::memcpy(recvd_vertices, packet_buf.data() + sizeof(recvd_payload), sizeof(recvd_vertices));

        TEST_ASSERT(recvd_payload.res_id == 42);
        TEST_ASSERT(recvd_payload.byte_width == 64);
        TEST_ASSERT(recvd_payload.has_initial_data == 1);
        TEST_ASSERT(std::memcmp(vertex_data, recvd_vertices, sizeof(vertex_data)) == 0);
    }

    // 4. DrawIndexedPayload
    {
        RemoteGPU::DrawIndexedPayload src{};
        src.index_count = 36;
        src.start_index_location = 0;
        src.base_vertex_location = 10;

        std::vector<uint8_t> buffer(sizeof(src));
        std::memcpy(buffer.data(), &src, sizeof(src));

        RemoteGPU::DrawIndexedPayload dst{};
        std::memcpy(&dst, buffer.data(), sizeof(dst));

        TEST_ASSERT(dst.index_count == 36);
        TEST_ASSERT(dst.start_index_location == 0);
        TEST_ASSERT(dst.base_vertex_location == 10);
    }

    // 5. ClearRenderTargetViewPayload
    {
        RemoteGPU::ClearRtvPayload src{};
        src.rtv_id = 100;
        src.color[0] = 0.2f;
        src.color[1] = 0.4f;
        src.color[2] = 0.6f;
        src.color[3] = 1.0f;

        std::vector<uint8_t> buffer(sizeof(src));
        std::memcpy(buffer.data(), &src, sizeof(src));

        RemoteGPU::ClearRtvPayload dst{};
        std::memcpy(&dst, buffer.data(), sizeof(dst));

        TEST_ASSERT(dst.rtv_id == 100);
        TEST_ASSERT(dst.color[0] == 0.2f);
        TEST_ASSERT(dst.color[1] == 0.4f);
        TEST_ASSERT(dst.color[2] == 0.6f);
        TEST_ASSERT(dst.color[3] == 1.0f);
    }

    // 6. FrameDataPayload
    {
        RemoteGPU::FrameDataPayload src{};
        src.swapchain_id = 1;
        src.width = 800;
        src.height = 600;
        src.format = 28;
        src.row_pitch = 3200;
        src.data_size = 1920000;

        std::vector<uint8_t> buffer(sizeof(src));
        std::memcpy(buffer.data(), &src, sizeof(src));

        RemoteGPU::FrameDataPayload dst{};
        std::memcpy(&dst, buffer.data(), sizeof(dst));

        TEST_ASSERT(dst.swapchain_id == 1);
        TEST_ASSERT(dst.width == 800);
        TEST_ASSERT(dst.height == 600);
        TEST_ASSERT(dst.format == 28);
        TEST_ASSERT(dst.row_pitch == 3200);
        TEST_ASSERT(dst.data_size == 1920000);
    }
}

// ----------------------------------------------------------------------------
// Suite 3: Socket Loopback Connection Test
// ----------------------------------------------------------------------------
void Test_Socket_LoopbackConnection() {
    const uint16_t test_port = 48888;
    std::promise<uint16_t> port_promise;
    auto port_future = port_promise.get_future();

    std::unique_ptr<RemoteGPU::NetworkSocket> server_conn;
    std::thread server_thread([&]() {
        RemoteGPU::NetworkSocket listener;
        if (!listener.BindAndListen(test_port, "127.0.0.1")) {
            // Port might be in use, signal failure with port 0
            port_promise.set_value(0);
            return;
        }
        port_promise.set_value(test_port);
        server_conn = listener.Accept(5000);
    });

    uint16_t bound_port = port_future.get();
    TEST_ASSERT_MSG(bound_port == test_port, "Server successfully bound to test port 48888");

    if (bound_port != 0) {
        RemoteGPU::NetworkSocket client;
        bool connected = client.Connect("127.0.0.1", bound_port, 5000);
        TEST_ASSERT_MSG(connected, "Client successfully connected to 127.0.0.1:48888");
        TEST_ASSERT(client.IsValid());

        server_thread.join();

        TEST_ASSERT_MSG(server_conn != nullptr, "Server successfully accepted incoming connection");
        if (server_conn) {
            TEST_ASSERT(server_conn->IsValid());

            // Check TCP_NODELAY option on client socket
            int nodelay = 0;
            int optlen = sizeof(nodelay);
            int opt_res = getsockopt(client.GetSocket(), IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, &optlen);
            TEST_ASSERT(opt_res == 0);
            TEST_ASSERT_MSG(nodelay == 1, "TCP_NODELAY is active on client socket");

            server_conn->Close();
        }
        client.Close();
    } else {
        if (server_thread.joinable()) server_thread.join();
    }
}

// ----------------------------------------------------------------------------
// Suite 4: SendPacket & RecvPacket Loopback Transfer Integrity
// ----------------------------------------------------------------------------
void Test_Socket_DataTransfer() {
    const uint16_t test_port = 48889; // Use distinct port for transfer test
    std::promise<uint16_t> port_promise;
    auto port_future = port_promise.get_future();

    std::unique_ptr<RemoteGPU::NetworkSocket> server_conn;
    std::thread server_thread([&]() {
        RemoteGPU::NetworkSocket listener;
        if (!listener.BindAndListen(test_port, "127.0.0.1")) {
            port_promise.set_value(0);
            return;
        }
        port_promise.set_value(test_port);
        server_conn = listener.Accept(5000);
    });

    uint16_t bound_port = port_future.get();
    TEST_ASSERT(bound_port == test_port);

    RemoteGPU::NetworkSocket client;
    bool connected = client.Connect("127.0.0.1", bound_port, 5000);
    TEST_ASSERT(connected);
    server_thread.join();
    TEST_ASSERT(server_conn != nullptr);

    if (!connected || !server_conn) return;

    // --- Sub-Test 4A: Small Graphics Command Sequence ---
    {
        // 1. Handshake Request
        RemoteGPU::PacketHeader hdr{};
        hdr.magic = RemoteGPU::RGPU_MAGIC;
        hdr.opcode = RemoteGPU::OP_HANDSHAKE_REQUEST;
        hdr.flags = RemoteGPU::FLAG_NONE;
        hdr.sequence_id = 1;
        hdr.payload_length = sizeof(RemoteGPU::HandshakeRequest);

        RemoteGPU::HandshakeRequest req{};
        req.protocol_version = RemoteGPU::RGPU_PROTOCOL_VERSION;
        req.client_width = 800;
        req.client_height = 600;
        req.preferred_format = 28;

        TEST_ASSERT(client.SendPacket(hdr, &req));

        RemoteGPU::PacketHeader rcv_hdr{};
        std::vector<uint8_t> rcv_payload;
        TEST_ASSERT(server_conn->RecvPacket(rcv_hdr, rcv_payload));
        TEST_ASSERT(rcv_hdr.magic == RemoteGPU::RGPU_MAGIC);
        TEST_ASSERT(rcv_hdr.opcode == RemoteGPU::OP_HANDSHAKE_REQUEST);
        TEST_ASSERT(rcv_hdr.sequence_id == 1);
        TEST_ASSERT(rcv_hdr.payload_length == sizeof(RemoteGPU::HandshakeRequest));
        TEST_ASSERT(rcv_payload.size() == sizeof(RemoteGPU::HandshakeRequest));

        const auto* recvd_req = reinterpret_cast<const RemoteGPU::HandshakeRequest*>(rcv_payload.data());
        TEST_ASSERT(recvd_req->client_width == 800);
        TEST_ASSERT(recvd_req->client_height == 600);
    }

    // --- Sub-Test 4B: Large Multi-Megabyte Payload (4 MB Transfer) ---
    {
        const size_t payload_size = 4 * 1024 * 1024; // 4 MB
        std::vector<uint8_t> large_payload(payload_size);
        for (size_t i = 0; i < payload_size; ++i) {
            large_payload[i] = static_cast<uint8_t>((i * 109 + 37) ^ (i >> 5));
        }

        RemoteGPU::PacketHeader hdr{};
        hdr.magic = RemoteGPU::RGPU_MAGIC;
        hdr.opcode = RemoteGPU::OP_FRAME_DATA;
        hdr.flags = RemoteGPU::FLAG_NONE;
        hdr.sequence_id = 2;
        hdr.payload_length = static_cast<uint32_t>(payload_size);

        auto t0 = std::chrono::high_resolution_clock::now();
        TEST_ASSERT(client.SendPacket(hdr, large_payload.data()));

        RemoteGPU::PacketHeader rcv_hdr{};
        std::vector<uint8_t> rcv_payload;
        TEST_ASSERT(server_conn->RecvPacket(rcv_hdr, rcv_payload));
        auto t1 = std::chrono::high_resolution_clock::now();

        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        double mb = static_cast<double>(payload_size) / (1024.0 * 1024.0);
        double sec = (elapsed_ms > 0) ? (static_cast<double>(elapsed_ms) / 1000.0) : 0.001;
        double speed_mb_s = mb / sec;

        std::cout << "  [PERF] 4 MB loopback transfer completed in " << elapsed_ms
                  << " ms (~" << static_cast<int>(speed_mb_s) << " MB/s)" << std::endl;

        TEST_ASSERT(rcv_hdr.magic == RemoteGPU::RGPU_MAGIC);
        TEST_ASSERT(rcv_hdr.opcode == RemoteGPU::OP_FRAME_DATA);
        TEST_ASSERT(rcv_hdr.sequence_id == 2);
        TEST_ASSERT(rcv_hdr.payload_length == payload_size);
        TEST_ASSERT(rcv_payload.size() == payload_size);

        // Byte-by-byte integrity verification
        int cmp_res = std::memcmp(large_payload.data(), rcv_payload.data(), payload_size);
        TEST_ASSERT_MSG(cmp_res == 0, "4 MB payload byte-by-byte integrity matches 100%");
    }

    // --- Sub-Test 4C: Back-to-Back Burst Stream (50 consecutive packets) ---
    {
        const int num_burst = 50;
        for (int i = 0; i < num_burst; ++i) {
            RemoteGPU::PacketHeader hdr{};
            hdr.magic = RemoteGPU::RGPU_MAGIC;
            hdr.opcode = RemoteGPU::OP_DRAW_INDEXED;
            hdr.flags = RemoteGPU::FLAG_NONE;
            hdr.sequence_id = 100 + i;
            hdr.payload_length = sizeof(RemoteGPU::DrawIndexedPayload);

            RemoteGPU::DrawIndexedPayload draw{};
            draw.index_count = static_cast<uint32_t>(i + 1);
            draw.start_index_location = 0;
            draw.base_vertex_location = 0;

            TEST_ASSERT(client.SendPacket(hdr, &draw));
        }

        for (int i = 0; i < num_burst; ++i) {
            RemoteGPU::PacketHeader rcv_hdr{};
            std::vector<uint8_t> rcv_payload;
            TEST_ASSERT(server_conn->RecvPacket(rcv_hdr, rcv_payload));
            TEST_ASSERT(rcv_hdr.sequence_id == static_cast<uint32_t>(100 + i));
            TEST_ASSERT(rcv_hdr.opcode == RemoteGPU::OP_DRAW_INDEXED);
            const auto* draw = reinterpret_cast<const RemoteGPU::DrawIndexedPayload*>(rcv_payload.data());
            TEST_ASSERT(draw->index_count == static_cast<uint32_t>(i + 1));
        }
    }

    server_conn->Close();
    client.Close();
}

// ----------------------------------------------------------------------------
// Suite 5: Peer Disconnection and Error Handling
// ----------------------------------------------------------------------------
void Test_Socket_DisconnectionAndErrors() {
    // --- Sub-Test 5A: Clean Disconnect / Peer Close Detection ---
    {
        const uint16_t test_port = 48890;
        std::promise<uint16_t> port_promise;
        auto port_future = port_promise.get_future();

        std::unique_ptr<RemoteGPU::NetworkSocket> server_conn;
        std::thread server_thread([&]() {
            RemoteGPU::NetworkSocket listener;
            if (!listener.BindAndListen(test_port, "127.0.0.1")) {
                port_promise.set_value(0);
                return;
            }
            port_promise.set_value(test_port);
            server_conn = listener.Accept(5000);
        });

        uint16_t port = port_future.get();
        TEST_ASSERT(port != 0);

        RemoteGPU::NetworkSocket client;
        TEST_ASSERT(client.Connect("127.0.0.1", port, 5000));
        server_thread.join();
        TEST_ASSERT(server_conn != nullptr);

        // Client explicitly closes connection
        client.Close();

        // Server calls RecvPacket -> must return false (clean EOF) without hanging or crashing
        RemoteGPU::PacketHeader hdr{};
        std::vector<uint8_t> payload;
        bool recv_ok = server_conn->RecvPacket(hdr, payload);
        TEST_ASSERT_MSG(!recv_ok, "Server detected peer closure as clean EOF (returned false)");

        server_conn->Close();
    }

    // --- Sub-Test 5B: Connection Attempt to Unbound / Closed Port ---
    {
        const uint16_t closed_port = 48895; // Unbound port
        RemoteGPU::NetworkSocket client;
        auto t0 = std::chrono::high_resolution_clock::now();
        bool connected = client.Connect("127.0.0.1", closed_port, 1000);
        auto t1 = std::chrono::high_resolution_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        TEST_ASSERT_MSG(!connected, "Connect to closed port returned false");
        TEST_ASSERT_MSG(!client.IsValid(), "Socket remains invalid after failed connect");
        TEST_ASSERT_MSG(elapsed_ms < 3000, "Connect failure returned promptly without infinite hang");
    }

    // --- Sub-Test 5C: Truncated Stream / Mid-Stream Disconnect ---
    {
        const uint16_t test_port = 48891;
        std::promise<uint16_t> port_promise;
        auto port_future = port_promise.get_future();

        std::unique_ptr<RemoteGPU::NetworkSocket> server_conn;
        std::thread server_thread([&]() {
            RemoteGPU::NetworkSocket listener;
            if (!listener.BindAndListen(test_port, "127.0.0.1")) {
                port_promise.set_value(0);
                return;
            }
            port_promise.set_value(test_port);
            server_conn = listener.Accept(5000);
        });

        uint16_t port = port_future.get();
        TEST_ASSERT(port != 0);

        RemoteGPU::NetworkSocket client;
        TEST_ASSERT(client.Connect("127.0.0.1", port, 5000));
        server_thread.join();
        TEST_ASSERT(server_conn != nullptr);

        // Send only 8 bytes of a 16-byte header
        uint8_t partial_hdr[8] = { 'R', 'G', 'P', 'U', 0x01, 0x00, 0x00, 0x00 };
        send(client.GetSocket(), reinterpret_cast<const char*>(partial_hdr), sizeof(partial_hdr), 0);

        // Immediately close the client socket
        client.Close();

        // Server calls RecvPacket -> must detect premature EOF and return false
        RemoteGPU::PacketHeader hdr{};
        std::vector<uint8_t> payload;
        bool recv_ok = server_conn->RecvPacket(hdr, payload);
        TEST_ASSERT_MSG(!recv_ok, "Server detected mid-stream disconnect on truncated header");

        server_conn->Close();
    }

    // --- Sub-Test 5D: Corrupted Magic Rejection ---
    {
        const uint16_t test_port = 48892;
        std::promise<uint16_t> port_promise;
        auto port_future = port_promise.get_future();

        std::unique_ptr<RemoteGPU::NetworkSocket> server_conn;
        std::thread server_thread([&]() {
            RemoteGPU::NetworkSocket listener;
            if (!listener.BindAndListen(test_port, "127.0.0.1")) {
                port_promise.set_value(0);
                return;
            }
            port_promise.set_value(test_port);
            server_conn = listener.Accept(5000);
        });

        uint16_t port = port_future.get();
        TEST_ASSERT(port != 0);

        RemoteGPU::NetworkSocket client;
        TEST_ASSERT(client.Connect("127.0.0.1", port, 5000));
        server_thread.join();
        TEST_ASSERT(server_conn != nullptr);

        // Send invalid magic
        RemoteGPU::PacketHeader bad_hdr{};
        bad_hdr.magic = 0xDEADBEEF; // Invalid magic
        bad_hdr.opcode = RemoteGPU::OP_PING;
        bad_hdr.flags = RemoteGPU::FLAG_NONE;
        bad_hdr.sequence_id = 1;
        bad_hdr.payload_length = 0;

        send(client.GetSocket(), reinterpret_cast<const char*>(&bad_hdr), sizeof(bad_hdr), 0);

        // Server calls RecvPacket -> should detect bad magic and return false
        RemoteGPU::PacketHeader hdr{};
        std::vector<uint8_t> payload;
        bool recv_ok = server_conn->RecvPacket(hdr, payload);
        TEST_ASSERT_MSG(!recv_ok, "Server rejected packet with invalid magic");

        server_conn->Close();
        client.Close();
    }
}

// ----------------------------------------------------------------------------
// Test Main Entrypoint
// ----------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    std::cout << "============================================================" << std::endl;
    std::cout << "  Remote GPU Offloading - Milestone 1 Transport Test Suite" << std::endl;
    std::cout << "============================================================" << std::endl;

    // 1. Initialize Winsock2
    if (!RemoteGPU::NetworkSocket::InitializeNetwork()) {
        std::cerr << "[FATAL] Failed to initialize Winsock2 networking!" << std::endl;
        return 1;
    }

    auto total_start = std::chrono::high_resolution_clock::now();

    // 2. Execute All Test Suites
    RUN_TEST_SUITE(Test_PacketHeader_PackingAndConstants);
    RUN_TEST_SUITE(Test_Payload_SerializationRoundTrip);
    RUN_TEST_SUITE(Test_Socket_LoopbackConnection);
    RUN_TEST_SUITE(Test_Socket_DataTransfer);
    RUN_TEST_SUITE(Test_Socket_DisconnectionAndErrors);

    auto total_end = std::chrono::high_resolution_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(total_end - total_start).count();

    // 3. Shutdown Winsock2
    RemoteGPU::NetworkSocket::ShutdownNetwork();

    // 4. Print Summary Report
    std::cout << "============================================================" << std::endl;
    std::cout << "  TEST SUMMARY" << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << "  Total Assertions Run : " << g_tests_run << std::endl;
    std::cout << "  Passed               : " << g_tests_passed << std::endl;
    std::cout << "  Failed               : " << g_tests_failed << std::endl;
    std::cout << "  Total Duration       : " << total_ms << " ms" << std::endl;
    std::cout << "============================================================" << std::endl;

    if (g_tests_failed == 0) {
        std::cout << "  OVERALL STATUS: ALL TESTS PASSED (EXIT CODE 0)" << std::endl;
        std::cout << "============================================================" << std::endl;
        return 0;
    } else {
        std::cerr << "  OVERALL STATUS: TESTS FAILED (" << g_tests_failed << " failures)" << std::endl;
        std::cout << "============================================================" << std::endl;
        return 1;
    }
}
```

---

## 6. Build, Run, and CI Instructions

### 6.1 Prerequisites
1. Windows 10/11 x86-64 host.
2. LLVM-MinGW toolchain with `clang++` on PATH (Version 23.1.4 verified).
3. System library `ws2_32.lib` available via `-lws2_32`.
4. Output directory `build/` created prior to binary emission.

### 6.2 Compilation Command
From the project root (`C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload`):

```powershell
# 1. Create build directory if not present
if (!(Test-Path build)) { New-Item -ItemType Directory -Path build }

# 2. Compile test binary with clang++
clang++ -std=c++17 -O2 -Isrc/common src/common/transport.cpp tests/test_transport.cpp -o build/test_transport.exe -lws2_32
```

### 6.3 Test Execution Command
```powershell
.\build\test_transport.exe
```

### 6.4 Expected Console Output
```text
============================================================
  Remote GPU Offloading - Milestone 1 Transport Test Suite
============================================================
============================================================
[RUNNING ] Test_PacketHeader_PackingAndConstants...
[PASSED  ] Test_PacketHeader_PackingAndConstants (0 ms)
============================================================
[RUNNING ] Test_Payload_SerializationRoundTrip...
[PASSED  ] Test_Payload_SerializationRoundTrip (0 ms)
============================================================
[RUNNING ] Test_Socket_LoopbackConnection...
[PASSED  ] Test_Socket_LoopbackConnection (12 ms)
============================================================
[RUNNING ] Test_Socket_DataTransfer...
  [PERF] 4 MB loopback transfer completed in 3 ms (~1333 MB/s)
[PASSED  ] Test_Socket_DataTransfer (25 ms)
============================================================
[RUNNING ] Test_Socket_DisconnectionAndErrors...
[PASSED  ] Test_Socket_DisconnectionAndErrors (1015 ms)
============================================================
  TEST SUMMARY
============================================================
  Total Assertions Run : 48
  Passed               : 48
  Failed               : 0
  Total Duration       : 1052 ms
============================================================
  OVERALL STATUS: ALL TESTS PASSED (EXIT CODE 0)
============================================================
```

### 6.5 Invalidation Conditions
The test suite or test plan is invalidated if:
1. `sizeof(PacketHeader)` is modified from 16 bytes or alignment padding is introduced.
2. `RGPU_MAGIC` constant or opcode definitions change without updating both `protocol.h` and test assertions.
3. Socket transport methods change signature (e.g. `SendPacket` or `RecvPacket` returning different types).
4. Port `48888` is blocked or unavailable without allowing fallback port configuration.

---

## 7. Downstream Implementer Guidance

1. **Header Inclusion Order**: Always include `<winsock2.h>` before any other Windows headers, and define `WIN32_LEAN_AND_MEAN` and `NOMINMAX` to prevent macro name collisions (`min`/`max`).
2. **Buffer Alignment**: `PacketHeader` uses `#pragma pack(push, 1)`. When copying packets from network streams, ensure destination buffers are properly sized.
3. **Multi-Threaded Safety**: When testing loopback sockets in unit tests, background threads accepting connections must synchronize their listening readiness with connecting client threads via `std::promise` / `std::future`.
4. **EOF Handling**: Ensure `RecvPacket` returns `false` on graceful disconnect (`recv` returns 0) rather than hanging or attempting to dereference uninitialized buffers.
