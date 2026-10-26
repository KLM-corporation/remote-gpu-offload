# Milestone 1: Winsock2 Socket Transport Implementation Specification

**Document**: `transport_impl_spec.md`  
**Author**: M1 Transport Impl Explorer (`explorer_m1_2`)  
**Target Files**: `src/common/transport.h` and `src/common/transport.cpp`  
**Milestone**: M1 (Network Protocol & Transport Layer)  
**Status**: Complete Implementation Specification & Verified Reference Code  

---

## 1. Executive Summary & Architectural Overview

The Remote GPU Offloading system operates by intercepting Direct3D 11 graphics API calls on a client PC, packaging the command stream into binary packets, transmitting them over a network socket to a remote GPU server, and receiving back uncompressed or compressed framebuffers for presentation.

The transport layer is the core communication backbone of this architecture. It must satisfy three strict requirements:
1. **Low Latency**: Command submission from the client rendering loop must not incur artificial buffering delays (Nagle's algorithm must be disabled via `TCP_NODELAY`).
2. **High Throughput**: Framebuffers transferred at 60 FPS (e.g., 1280x720 RGBA8 uncompressed = ~3.51 MB/frame = ~210 MB/s) must not suffer TCP window exhaustion or bufferbloat; socket send/receive buffers must be sized to 16 MB (`SO_SNDBUF`, `SO_RCVBUF`).
3. **Robust Framing & Non-Crashing Resilience**: TCP is an unstructured byte stream. Commands and frames must be framed with exact-byte guarantees (`SendExact`, `RecvExact`, `SendPacket`, `RecvPacket`). Socket drops, server shutdowns, or broken connections must be handled gracefully without process crashes, uncaught exceptions, or infinite hangs.

This document provides the complete, production-grade technical design and reference implementation of `NetworkSocket` for `src/common/transport.h` and `src/common/transport.cpp`.

---

## 2. Core Architectural Design: `NetworkSocket`

### 2.1 RAII & Resource Management Semantics
- **Raw Socket Handle**: Wraps an underlying Windows `SOCKET` handle, initialized to `INVALID_SOCKET`.
- **Non-Copyable**: Sockets represent unique kernel communication endpoints; copy construction and copy assignment are explicitly deleted (`= delete`).
- **Movable**: Sockets support move construction and move assignment (`noexcept`), transferring ownership of the `SOCKET` handle and leaving the source object in a safe `INVALID_SOCKET` state.
- **Destructor Safety**: The destructor automatically invokes `Close()`. If the socket is connected, `Close()` executes `shutdown(m_socket, SD_BOTH)` to cleanly signal the remote peer before calling `closesocket(m_socket)`.

### 2.2 Global Winsock Lifecycle Management
- Windows requires `WSAStartup()` and `WSACleanup()`.
- To allow safe invocation from multiple modules (e.g. server daemon, client proxy DLL `DllMain`, automated test harnesses) without redundant initializations or premature cleanups, `InitializeNetwork()` and `ShutdownNetwork()` utilize a thread-safe `std::atomic<int> s_wsaRefCount`.
- `InitializeNetwork()` requests Winsock version 2.2 (`MAKEWORD(2, 2)`). Only the first caller invokes `WSAStartup`; subsequent callers increment the reference count.
- `ShutdownNetwork()` decrements the count; only when the count reaches zero is `WSACleanup` invoked.

### 2.3 Class Interface Specification (`transport.h`)

```cpp
#pragma once
#ifndef REMOTE_GPU_TRANSPORT_H
#define REMOTE_GPU_TRANSPORT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "protocol.h"

// Fallback definition for SIO_LOOPBACK_FAST_PATH if not defined by SDK headers
#ifndef SIO_LOOPBACK_FAST_PATH
#define SIO_LOOPBACK_FAST_PATH _WSAIOW(IOC_VENDOR, 16)
#endif

namespace RemoteGPU {

class NetworkSocket {
public:
    // --- Global Winsock Lifecycle ---
    static bool InitializeNetwork();
    static void ShutdownNetwork();

    // --- Constructors & Destructor ---
    NetworkSocket();
    explicit NetworkSocket(SOCKET sock) noexcept;
    ~NetworkSocket();

    // Non-copyable
    NetworkSocket(const NetworkSocket&) = delete;
    NetworkSocket& operator=(const NetworkSocket&) = delete;

    // Movable
    NetworkSocket(NetworkSocket&& other) noexcept;
    NetworkSocket& operator=(NetworkSocket&& other) noexcept;

    // --- Connection & Listening ---
    bool Connect(const char* host, uint16_t port, int timeout_ms = 5000);
    bool BindAndListen(uint16_t port, const char* bind_ip = "0.0.0.0", int backlog = 5);
    std::unique_ptr<NetworkSocket> Accept(int timeout_ms = -1);

    // --- Data Transmission ---
    bool SendExact(const void* data, size_t length);
    bool RecvExact(void* data, size_t length);

    // --- High-Level Packet Framing ---
    bool SendPacket(const PacketHeader& header, const void* payload = nullptr);
    bool RecvPacket(PacketHeader& out_header, std::vector<uint8_t>& out_payload);
    bool RecvPacket(PacketHeader& out_header, void* out_payload, size_t max_payload_len);

    // --- State & Utility ---
    void Close();
    bool IsValid() const noexcept;
    SOCKET GetRawSocket() const noexcept;
    SOCKET Release() noexcept;

    bool SetTimeouts(int send_timeout_ms, int recv_timeout_ms);
    bool SetNonBlocking(bool non_blocking);

    std::string GetPeerAddress() const;
    uint16_t GetPeerPort() const;
    std::string GetLocalAddress() const;
    uint16_t GetLocalPort() const;

    // --- Diagnostics & Error Reporting ---
    static std::string GetLastErrorMessage(int err = -1);
    static void LogError(const char* context, int err = -1);

private:
    static bool ConfigureConnectedSocket(SOCKET sock, int timeout_ms = 10000);

    SOCKET m_socket;
};

} // namespace RemoteGPU

#endif // REMOTE_GPU_TRANSPORT_H
```

---

## 3. Connection Management & Socket Configuration

### 3.1 Non-Blocking Connect with Timeout (`Connect`)
Standard Winsock `connect()` is blocking and can freeze the calling thread for 20–30 seconds if the destination endpoint is offline or drops TCP SYN packets.

The `Connect(const char* host, uint16_t port, int timeout_ms)` method implements non-blocking connect with millisecond timeout:
1. **Name Resolution**: Resolves target host using `getaddrinfo()` with `AF_INET` (IPv4) and `SOCK_STREAM` (TCP). This supports numerical IPs (`"127.0.0.1"`, `"192.168.1.50"`) and hostnames (`"localhost"`, `"desktop-pc"`).
2. **Socket Creation & Fast Loopback**: Creates `SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)`. Invokes `WSAIoctl(sock, SIO_LOOPBACK_FAST_PATH, ...)` before calling `connect()` as required by the Windows kernel.
3. **Non-Blocking Mode Switch**: Enables non-blocking mode via `ioctlsocket(sock, FIONBIO, &mode)` (`mode = 1`).
4. **Connection Initiate**: Calls `connect()`.
   - If `connect()` returns `0`, the socket connected immediately (common on loopback).
   - If `connect()` returns `SOCKET_ERROR` with `WSAGetLastError() == WSAEWOULDBLOCK`:
     - Monitors the socket with `select()` on both `write_set` (for successful completion) and `error_set` (for connection refusal or failure).
     - Passes `struct timeval` configured from `timeout_ms`.
     - Upon `select()` indicating writability, checks socket error state via `getsockopt(sock, SOL_SOCKET, SO_ERROR, ...)` to ensure `so_error == 0`.
5. **Restoration & Tuning**: Restores blocking mode (`mode = 0`) and applies `ConfigureConnectedSocket()`.

### 3.2 Listener Socket Configuration (`BindAndListen`)
1. Creates socket handle.
2. Applies `SO_REUSEADDR` (`setsockopt(SOL_SOCKET, SO_REUSEADDR)`): Prevents `WSAEADDRINUSE` conflicts when restarting the server rapidly during development and automated tests.
3. Applies `SIO_LOOPBACK_FAST_PATH`: Must be invoked on the listening socket **before** `listen()` so accepted sockets inherit loopback fast-path routing.
4. Binds to `bind_ip` (`"0.0.0.0"` for all LAN interfaces, `"127.0.0.1"` for loopback-only).
5. Calls `listen(sock, backlog)` and stores the socket.

### 3.3 Connection Acceptance (`Accept`)
1. **Optional Timeout**: If `timeout_ms >= 0`, `select()` checks `read_set` on `m_socket`. If no incoming connection arrives within `timeout_ms`, returns `nullptr` cleanly.
2. **Accept**: Calls `accept()`. If `INVALID_SOCKET` is returned, logs the error and returns `nullptr`.
3. **Socket Tuning**: Immediately configures the accepted socket via `ConfigureConnectedSocket()`.
4. **Ownership Transfer**: Returns `std::make_unique<NetworkSocket>(client_sock)`.

### 3.4 Socket Tuning Pipeline (`ConfigureConnectedSocket`)
Every connected socket (both client-side and server-side) passes through an identical optimization pipeline:
1. **`TCP_NODELAY = 1`**: Disables Nagle's algorithm (`setsockopt(IPPROTO_TCP, TCP_NODELAY)`). Direct3D draw packets are sent immediately without waiting for ACK coalescing, reducing round-trip latency from ~40ms to <0.1ms.
2. **16 MB Buffers (`SO_SNDBUF`, `SO_RCVBUF`)**: Direct3D frame readbacks for 1080p RGBA8 represent ~8.3 MB per frame. The default Windows socket buffer (64 KB) would stall transfers due to TCP window exhaustion. Expanding buffers to `16 * 1024 * 1024` bytes (16 MB) allows uncompressed multi-megabyte frame streaming at full network line rate.
3. **Timeouts (`SO_SNDTIMEO`, `SO_RCVTIMEO`)**: Sockets are configured with a 10-second default I/O timeout. If a peer abruptly freezes or crashes, `recv()` and `send()` abort with `WSAETIMEDOUT` rather than hanging the thread indefinitely.
4. **`SIO_LOOPBACK_FAST_PATH`**: Applied to accepted sockets for loopback IPC acceleration.

---

## 4. Exact-Byte Framing & Stream Processing

TCP is a stream-oriented protocol without message boundaries. A single `send()` can be received across multiple `recv()` calls, or multiple `send()` calls can be merged into a single `recv()`.

### 4.1 Exact Send Loop (`SendExact`)
```cpp
bool NetworkSocket::SendExact(const void* data, size_t length) {
    if (!IsValid()) return false;
    if (length == 0) return true;
    if (!data) return false;

    const char* ptr = static_cast<const char*>(data);
    size_t total_sent = 0;

    while (total_sent < length) {
        size_t remaining = length - total_sent;
        int chunk_size = static_cast<int>(std::min<size_t>(remaining, 64 * 1024 * 1024));
        int sent = send(m_socket, ptr + total_sent, chunk_size, 0);

        if (sent == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEINTR) continue;
            if (err == WSAEWOULDBLOCK) {
                fd_set write_set;
                FD_ZERO(&write_set);
                FD_SET(m_socket, &write_set);
                struct timeval tv{ 5, 0 };
                int sel = select(0, nullptr, &write_set, nullptr, &tv);
                if (sel > 0 && FD_ISSET(m_socket, &write_set)) continue;
            }
            return false; // Disconnect or socket error
        }

        if (sent == 0) return false; // Peer closed connection
        total_sent += static_cast<size_t>(sent);
    }
    return true;
}
```

### 4.2 Exact Receive Loop (`RecvExact`)
```cpp
bool NetworkSocket::RecvExact(void* data, size_t length) {
    if (!IsValid()) return false;
    if (length == 0) return true;
    if (!data) return false;

    char* ptr = static_cast<char*>(data);
    size_t total_recv = 0;

    while (total_recv < length) {
        size_t remaining = length - total_recv;
        int chunk_size = static_cast<int>(std::min<size_t>(remaining, 64 * 1024 * 1024));
        int received = recv(m_socket, ptr + total_recv, chunk_size, 0);

        if (received == 0) {
            // Graceful EOF / connection closed cleanly by peer
            return false;
        }

        if (received == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEINTR) continue;
            if (err == WSAEWOULDBLOCK) {
                fd_set read_set;
                FD_ZERO(&read_set);
                FD_SET(m_socket, &read_set);
                struct timeval tv{ 5, 0 };
                int sel = select(0, &read_set, nullptr, nullptr, &tv);
                if (sel > 0 && FD_ISSET(m_socket, &read_set)) continue;
            }
            return false; // Socket error or connection dropped
        }

        total_recv += static_cast<size_t>(received);
    }
    return true;
}
```

### 4.3 High-Level Packet Framing (`SendPacket`)
`SendPacket` transmits a `PacketHeader` (16 bytes) followed by its optional variable-length payload.
To eliminate user-space memory copies and ensure atomic transmission:
1. Uses Winsock `WSASend` with a 2-element `WSABUF` scatter-gather array:
   - `bufs[0].buf = &header; bufs[0].len = 16;`
   - `bufs[1].buf = payload; bufs[1].len = header.payload_length;`
2. In the vast majority of calls, `WSASend` transmits the entire header and payload in a **single atomic system call**.
3. If a partial send occurs (e.g. socket buffer congestion during a 30 MB transfer):
   - If `bytes_sent < 16`, sends remaining header bytes followed by full payload via `SendExact`.
   - If `bytes_sent >= 16`, sends the remaining `payload_length - (bytes_sent - 16)` bytes via `SendExact`.
4. If `WSASend` returns `WSAEWOULDBLOCK`, falls back to sequential `SendExact`.

### 4.4 Packet Reception & Validation (`RecvPacket`)
1. Reads the exact 16-byte `PacketHeader` via `RecvExact`.
2. Validates `magic == RGPU_MAGIC` (`0x52475055`). If mismatched, detects stream desynchronization and returns `false`.
3. Validates `payload_length <= 256 * 1024 * 1024` (256 MB safety ceiling) to prevent corrupted headers from causing process aborts via `std::bad_alloc`.
4. Resizes the `std::vector<uint8_t>& out_payload` to `payload_length`.
5. Reads the exact payload bytes into `out_payload.data()` via `RecvExact`.
6. Provides a secondary overload `RecvPacket(PacketHeader& hdr, void* out_buf, size_t max_len)` for zero-allocation reception directly into caller-managed memory buffers.

---

## 5. Windows Fast Loopback Optimization (`SIO_LOOPBACK_FAST_PATH`)

Windows 8 and Windows Server 2012 introduced `SIO_LOOPBACK_FAST_PATH` (`_WSAIOW(IOC_VENDOR, 16)`).
- **Mechanism**: Standard TCP loopback routes packets through the complete TCP/IP network driver stack (AFD -> TCPIP.sys -> NDIS loopback driver). When `SIO_LOOPBACK_FAST_PATH` is enabled on both communicating endpoints, Winsock short-circuits internal buffer queues directly within the AFD (Ancillary Function Driver) subsystem, bypassing IP fragmentation, checksum computation, and NDIS driver round-trips.
- **Rules of Application**:
  - Must be applied to the client socket **before calling `connect()`**.
  - Must be applied to the server listening socket **before calling `listen()`**.
  - Must be applied to accepted sockets returned by `accept()`.
- **Transparent Fallback**: If a socket configured with `SIO_LOOPBACK_FAST_PATH` connects to an external LAN IP address instead of `127.0.0.1`, the Windows TCP stack transparently falls back to normal network routing without error.

---

## 6. Complete Source Code Specification

### 6.1 `src/common/transport.h`

```cpp
#pragma once
#ifndef REMOTE_GPU_TRANSPORT_H
#define REMOTE_GPU_TRANSPORT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "protocol.h"

// Fallback definition for SIO_LOOPBACK_FAST_PATH if not defined by SDK headers
#ifndef SIO_LOOPBACK_FAST_PATH
#define SIO_LOOPBACK_FAST_PATH _WSAIOW(IOC_VENDOR, 16)
#endif

namespace RemoteGPU {

/**
 * @brief High-performance Winsock2 TCP socket wrapper with RAII lifecycle,
 * non-blocking timeout support, 16 MB buffer tuning, loopback fast-path,
 * and reliable exact-byte framing.
 */
class NetworkSocket {
public:
    // --- Global Winsock Lifecycle ---
    static bool InitializeNetwork();
    static void ShutdownNetwork();

    // --- Constructors & Destructor ---
    NetworkSocket();
    explicit NetworkSocket(SOCKET sock) noexcept;
    ~NetworkSocket();

    // Non-copyable
    NetworkSocket(const NetworkSocket&) = delete;
    NetworkSocket& operator=(const NetworkSocket&) = delete;

    // Movable
    NetworkSocket(NetworkSocket&& other) noexcept;
    NetworkSocket& operator=(NetworkSocket&& other) noexcept;

    // --- Connection & Listening ---
    bool Connect(const char* host, uint16_t port, int timeout_ms = 5000);
    bool BindAndListen(uint16_t port, const char* bind_ip = "0.0.0.0", int backlog = 5);
    std::unique_ptr<NetworkSocket> Accept(int timeout_ms = -1);

    // --- Data Transmission ---
    bool SendExact(const void* data, size_t length);
    bool RecvExact(void* data, size_t length);

    // --- High-Level Packet Framing ---
    bool SendPacket(const PacketHeader& header, const void* payload = nullptr);
    bool RecvPacket(PacketHeader& out_header, std::vector<uint8_t>& out_payload);
    bool RecvPacket(PacketHeader& out_header, void* out_payload, size_t max_payload_len);

    // --- State & Utility ---
    void Close();
    bool IsValid() const noexcept;
    SOCKET GetRawSocket() const noexcept;
    SOCKET Release() noexcept;

    bool SetTimeouts(int send_timeout_ms, int recv_timeout_ms);
    bool SetNonBlocking(bool non_blocking);

    std::string GetPeerAddress() const;
    uint16_t GetPeerPort() const;
    std::string GetLocalAddress() const;
    uint16_t GetLocalPort() const;

    // --- Diagnostics & Error Reporting ---
    static std::string GetLastErrorMessage(int err = -1);
    static void LogError(const char* context, int err = -1);

private:
    static bool ConfigureConnectedSocket(SOCKET sock, int timeout_ms = 10000);

    SOCKET m_socket;
};

} // namespace RemoteGPU

#endif // REMOTE_GPU_TRANSPORT_H
```

### 6.2 `src/common/transport.cpp`

```cpp
#include "transport.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <iostream>

namespace RemoteGPU {

static std::atomic<int> s_wsaRefCount{0};

bool NetworkSocket::InitializeNetwork() {
    if (s_wsaRefCount.fetch_add(1) == 0) {
        WSADATA wsa_data{};
        int res = WSAStartup(MAKEWORD(2, 2), &wsa_data);
        if (res != 0) {
            s_wsaRefCount.fetch_sub(1);
            LogError("WSAStartup failed", res);
            return false;
        }
    }
    return true;
}

void NetworkSocket::ShutdownNetwork() {
    int count = s_wsaRefCount.load();
    while (count > 0) {
        if (s_wsaRefCount.compare_exchange_weak(count, count - 1)) {
            if (count == 1) {
                WSACleanup();
            }
            break;
        }
    }
}

NetworkSocket::NetworkSocket()
    : m_socket(INVALID_SOCKET) {
}

NetworkSocket::NetworkSocket(SOCKET sock) noexcept
    : m_socket(sock) {
}

NetworkSocket::~NetworkSocket() {
    Close();
}

NetworkSocket::NetworkSocket(NetworkSocket&& other) noexcept
    : m_socket(other.m_socket) {
    other.m_socket = INVALID_SOCKET;
}

NetworkSocket& NetworkSocket::operator=(NetworkSocket&& other) noexcept {
    if (this != &other) {
        Close();
        m_socket = other.m_socket;
        other.m_socket = INVALID_SOCKET;
    }
    return *this;
}

bool NetworkSocket::ConfigureConnectedSocket(SOCKET sock, int timeout_ms) {
    if (sock == INVALID_SOCKET) {
        return false;
    }

    // 1. Disable Nagle's algorithm (TCP_NODELAY) for minimal command latency
    int nodelay = 1;
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay)) != 0) {
        LogError("setsockopt(TCP_NODELAY) failed");
    }

    // 2. Expand socket send and receive buffers to 16 MB for high-throughput framebuffer transfers
    int buf_size = 16 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buf_size), sizeof(buf_size));
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf_size), sizeof(buf_size));

    // 3. Set send and receive timeouts to prevent indefinite hangs
    if (timeout_ms > 0) {
        DWORD timeout = static_cast<DWORD>(timeout_ms);
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    }

    // 4. Windows Loopback Fast Path
    DWORD fast_path = 1;
    DWORD bytes_returned = 0;
    WSAIoctl(sock, SIO_LOOPBACK_FAST_PATH, &fast_path, sizeof(fast_path), nullptr, 0, &bytes_returned, nullptr, nullptr);

    return true;
}

bool NetworkSocket::Connect(const char* host, uint16_t port, int timeout_ms) {
    Close();

    if (!host) {
        LogError("Connect: null host provided");
        return false;
    }

    struct addrinfo hints{};
    hints.ai_family = AF_INET; // IPv4
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    std::string port_str = std::to_string(port);
    struct addrinfo* addr_list = nullptr;
    int status = getaddrinfo(host, port_str.c_str(), &hints, &addr_list);
    if (status != 0) {
        LogError("Connect: getaddrinfo failed", status);
        return false;
    }

    SOCKET connected_sock = INVALID_SOCKET;
    int last_connect_error = 0;

    for (struct addrinfo* ptr = addr_list; ptr != nullptr; ptr = ptr->ai_next) {
        SOCKET sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (sock == INVALID_SOCKET) {
            last_connect_error = WSAGetLastError();
            continue;
        }

        // Apply SIO_LOOPBACK_FAST_PATH before connect()
        DWORD fast_path = 1;
        DWORD bytes_returned = 0;
        WSAIoctl(sock, SIO_LOOPBACK_FAST_PATH, &fast_path, sizeof(fast_path), nullptr, 0, &bytes_returned, nullptr, nullptr);

        // Put socket in non-blocking mode to implement timeout
        u_long non_blocking = 1;
        ioctlsocket(sock, FIONBIO, &non_blocking);

        int conn_res = connect(sock, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen));
        bool success = false;

        if (conn_res == 0) {
            // Immediate connection (common on loopback)
            success = true;
        } else {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
                fd_set write_set, error_set;
                FD_ZERO(&write_set);
                FD_ZERO(&error_set);
                FD_SET(sock, &write_set);
                FD_SET(sock, &error_set);

                struct timeval tv{};
                struct timeval* tv_ptr = nullptr;
                if (timeout_ms >= 0) {
                    tv.tv_sec = timeout_ms / 1000;
                    tv.tv_usec = (timeout_ms % 1000) * 1000;
                    tv_ptr = &tv;
                }

                int sel_res = select(0, nullptr, &write_set, &error_set, tv_ptr);
                if (sel_res > 0) {
                    if (FD_ISSET(sock, &write_set) && !FD_ISSET(sock, &error_set)) {
                        int so_error = 0;
                        int opt_len = sizeof(so_error);
                        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &opt_len) == 0 && so_error == 0) {
                            success = true;
                        } else {
                            last_connect_error = so_error != 0 ? so_error : WSAGetLastError();
                        }
                    } else {
                        int so_error = 0;
                        int opt_len = sizeof(so_error);
                        getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &opt_len);
                        last_connect_error = so_error != 0 ? so_error : WSAECONNREFUSED;
                    }
                } else if (sel_res == 0) {
                    last_connect_error = WSAETIMEDOUT;
                } else {
                    last_connect_error = WSAGetLastError();
                }
            } else {
                last_connect_error = err;
            }
        }

        // Restore blocking mode
        u_long blocking = 0;
        ioctlsocket(sock, FIONBIO, &blocking);

        if (success) {
            ConfigureConnectedSocket(sock, 10000);
            connected_sock = sock;
            break;
        } else {
            closesocket(sock);
        }
    }

    freeaddrinfo(addr_list);

    if (connected_sock == INVALID_SOCKET) {
        LogError("Connect: failed to connect to host", last_connect_error);
        return false;
    }

    m_socket = connected_sock;
    return true;
}

bool NetworkSocket::BindAndListen(uint16_t port, const char* bind_ip, int backlog) {
    Close();

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        LogError("BindAndListen: socket() creation failed");
        return false;
    }

    // Configure SO_REUSEADDR for rapid port reuse during testing and restarts
    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse)) != 0) {
        LogError("BindAndListen: setsockopt(SO_REUSEADDR) failed");
    }

    // Apply SIO_LOOPBACK_FAST_PATH before listen()
    DWORD fast_path = 1;
    DWORD bytes_returned = 0;
    WSAIoctl(sock, SIO_LOOPBACK_FAST_PATH, &fast_path, sizeof(fast_path), nullptr, 0, &bytes_returned, nullptr, nullptr);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (!bind_ip || strcmp(bind_ip, "0.0.0.0") == 0 || strlen(bind_ip) == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (strcmp(bind_ip, "127.0.0.1") == 0 || strcmp(bind_ip, "localhost") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else {
        if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) <= 0) {
            LogError("BindAndListen: invalid bind IP address");
            closesocket(sock);
            return false;
        }
    }

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        LogError("BindAndListen: bind() failed");
        closesocket(sock);
        return false;
    }

    if (listen(sock, backlog) == SOCKET_ERROR) {
        LogError("BindAndListen: listen() failed");
        closesocket(sock);
        return false;
    }

    m_socket = sock;
    return true;
}

std::unique_ptr<NetworkSocket> NetworkSocket::Accept(int timeout_ms) {
    if (!IsValid()) {
        LogError("Accept: listener socket is invalid");
        return nullptr;
    }

    if (timeout_ms >= 0) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(m_socket, &read_set);

        struct timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int sel = select(0, &read_set, nullptr, nullptr, &tv);
        if (sel == 0) {
            // Timeout, no pending connection
            return nullptr;
        } else if (sel < 0) {
            LogError("Accept: select() failed");
            return nullptr;
        }
    }

    sockaddr_in client_addr{};
    int addr_len = sizeof(client_addr);
    SOCKET client_sock = accept(m_socket, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
    if (client_sock == INVALID_SOCKET) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINTR) {
            LogError("Accept: accept() failed");
        }
        return nullptr;
    }

    ConfigureConnectedSocket(client_sock, 10000);
    return std::make_unique<NetworkSocket>(client_sock);
}

bool NetworkSocket::SendExact(const void* data, size_t length) {
    if (!IsValid()) {
        return false;
    }
    if (length == 0) {
        return true;
    }
    if (!data) {
        LogError("SendExact: null data with non-zero length");
        return false;
    }

    const char* ptr = static_cast<const char*>(data);
    size_t total_sent = 0;

    while (total_sent < length) {
        size_t remaining = length - total_sent;
        int chunk_size = static_cast<int>(std::min<size_t>(remaining, 64 * 1024 * 1024));
        int sent = send(m_socket, ptr + total_sent, chunk_size, 0);

        if (sent == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEINTR) {
                continue;
            }
            if (err == WSAEWOULDBLOCK) {
                fd_set write_set;
                FD_ZERO(&write_set);
                FD_SET(m_socket, &write_set);
                struct timeval tv{ 5, 0 };
                int sel = select(0, nullptr, &write_set, nullptr, &tv);
                if (sel > 0 && FD_ISSET(m_socket, &write_set)) {
                    continue;
                }
            }
            // Disconnect or fatal error
            return false;
        }

        if (sent == 0) {
            // Connection closed by peer
            return false;
        }

        total_sent += static_cast<size_t>(sent);
    }

    return true;
}

bool NetworkSocket::RecvExact(void* data, size_t length) {
    if (!IsValid()) {
        return false;
    }
    if (length == 0) {
        return true;
    }
    if (!data) {
        LogError("RecvExact: null data with non-zero length");
        return false;
    }

    char* ptr = static_cast<char*>(data);
    size_t total_recv = 0;

    while (total_recv < length) {
        size_t remaining = length - total_recv;
        int chunk_size = static_cast<int>(std::min<size_t>(remaining, 64 * 1024 * 1024));
        int received = recv(m_socket, ptr + total_recv, chunk_size, 0);

        if (received == 0) {
            // Graceful EOF / connection closed by remote peer
            return false;
        }

        if (received == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEINTR) {
                continue;
            }
            if (err == WSAEWOULDBLOCK) {
                fd_set read_set;
                FD_ZERO(&read_set);
                FD_SET(m_socket, &read_set);
                struct timeval tv{ 5, 0 };
                int sel = select(0, &read_set, nullptr, nullptr, &tv);
                if (sel > 0 && FD_ISSET(m_socket, &read_set)) {
                    continue;
                }
            }
            // Socket error or connection dropped
            return false;
        }

        total_recv += static_cast<size_t>(received);
    }

    return true;
}

bool NetworkSocket::SendPacket(const PacketHeader& header, const void* payload) {
    if (!IsValid()) {
        return false;
    }
    if (header.magic != RGPU_MAGIC) {
        LogError("SendPacket: invalid header magic");
        return false;
    }
    if (header.payload_length > 0 && !payload) {
        LogError("SendPacket: header specifies non-zero payload length but payload pointer is null");
        return false;
    }

    // Zero-copy scatter-gather transmission via WSASend
    WSABUF bufs[2];
    bufs[0].buf = reinterpret_cast<CHAR*>(const_cast<PacketHeader*>(&header));
    bufs[0].len = static_cast<ULONG>(sizeof(PacketHeader));
    DWORD buf_count = 1;
    size_t total_bytes = sizeof(PacketHeader);

    if (payload && header.payload_length > 0) {
        bufs[1].buf = reinterpret_cast<CHAR*>(const_cast<void*>(payload));
        bufs[1].len = static_cast<ULONG>(header.payload_length);
        buf_count = 2;
        total_bytes += header.payload_length;
    }

    DWORD bytes_sent = 0;
    int ret = WSASend(m_socket, bufs, buf_count, &bytes_sent, 0, nullptr, nullptr);

    if (ret == 0) {
        if (bytes_sent == total_bytes) {
            return true;
        }
        // Partial send occurred: finish transmitting remaining bytes via SendExact
        if (bytes_sent < sizeof(PacketHeader)) {
            const char* hdr_ptr = reinterpret_cast<const char*>(&header) + bytes_sent;
            if (!SendExact(hdr_ptr, sizeof(PacketHeader) - bytes_sent)) {
                return false;
            }
            if (payload && header.payload_length > 0) {
                if (!SendExact(payload, header.payload_length)) {
                    return false;
                }
            }
        } else {
            size_t payload_sent = bytes_sent - sizeof(PacketHeader);
            size_t payload_remaining = header.payload_length - payload_sent;
            const char* payload_ptr = static_cast<const char*>(payload) + payload_sent;
            if (!SendExact(payload_ptr, payload_remaining)) {
                return false;
            }
        }
        return true;
    }

    // If WSASend blocked or failed, fall back to SendExact
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK) {
        if (!SendExact(&header, sizeof(PacketHeader))) {
            return false;
        }
        if (payload && header.payload_length > 0) {
            if (!SendExact(payload, header.payload_length)) {
                return false;
            }
        }
        return true;
    }

    // Fatal socket error
    return false;
}

bool NetworkSocket::RecvPacket(PacketHeader& out_header, std::vector<uint8_t>& out_payload) {
    if (!RecvExact(&out_header, sizeof(PacketHeader))) {
        return false;
    }

    if (out_header.magic != RGPU_MAGIC) {
        LogError("RecvPacket: protocol magic mismatch (stream desynchronized)");
        return false;
    }

    constexpr uint32_t MAX_ALLOWED_PAYLOAD = 256 * 1024 * 1024; // 256 MB safety limit
    if (out_header.payload_length > MAX_ALLOWED_PAYLOAD) {
        LogError("RecvPacket: payload length exceeds 256 MB safety limit");
        return false;
    }

    if (out_header.payload_length > 0) {
        try {
            out_payload.resize(out_header.payload_length);
        } catch (const std::bad_alloc&) {
            LogError("RecvPacket: out of memory allocating payload buffer");
            return false;
        }

        if (!RecvExact(out_payload.data(), out_header.payload_length)) {
            return false;
        }
    } else {
        out_payload.clear();
    }

    return true;
}

bool NetworkSocket::RecvPacket(PacketHeader& out_header, void* out_payload, size_t max_payload_len) {
    if (!RecvExact(&out_header, sizeof(PacketHeader))) {
        return false;
    }

    if (out_header.magic != RGPU_MAGIC) {
        LogError("RecvPacket: protocol magic mismatch (stream desynchronized)");
        return false;
    }

    if (out_header.payload_length > max_payload_len) {
        LogError("RecvPacket: incoming payload length exceeds provided buffer capacity");
        return false;
    }

    if (out_header.payload_length > 0) {
        if (!out_payload) {
            LogError("RecvPacket: null output payload buffer for non-zero payload length");
            return false;
        }

        if (!RecvExact(out_payload, out_header.payload_length)) {
            return false;
        }
    }

    return true;
}

void NetworkSocket::Close() {
    if (m_socket != INVALID_SOCKET) {
        shutdown(m_socket, SD_BOTH);
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
}

bool NetworkSocket::IsValid() const noexcept {
    return m_socket != INVALID_SOCKET;
}

SOCKET NetworkSocket::GetRawSocket() const noexcept {
    return m_socket;
}

SOCKET NetworkSocket::Release() noexcept {
    SOCKET sock = m_socket;
    m_socket = INVALID_SOCKET;
    return sock;
}

bool NetworkSocket::SetTimeouts(int send_timeout_ms, int recv_timeout_ms) {
    if (!IsValid()) {
        return false;
    }
    bool ok = true;
    if (send_timeout_ms >= 0) {
        DWORD timeout = static_cast<DWORD>(send_timeout_ms);
        if (setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0) {
            ok = false;
        }
    }
    if (recv_timeout_ms >= 0) {
        DWORD timeout = static_cast<DWORD>(recv_timeout_ms);
        if (setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0) {
            ok = false;
        }
    }
    return ok;
}

bool NetworkSocket::SetNonBlocking(bool non_blocking) {
    if (!IsValid()) {
        return false;
    }
    u_long mode = non_blocking ? 1 : 0;
    return ioctlsocket(m_socket, FIONBIO, &mode) == 0;
}

std::string NetworkSocket::GetPeerAddress() const {
    if (!IsValid()) return {};
    sockaddr_in addr{};
    int len = sizeof(addr);
    if (getpeername(m_socket, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        return std::string(ip);
    }
    return {};
}

uint16_t NetworkSocket::GetPeerPort() const {
    if (!IsValid()) return 0;
    sockaddr_in addr{};
    int len = sizeof(addr);
    if (getpeername(m_socket, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        return ntohs(addr.sin_port);
    }
    return 0;
}

std::string NetworkSocket::GetLocalAddress() const {
    if (!IsValid()) return {};
    sockaddr_in addr{};
    int len = sizeof(addr);
    if (getsockname(m_socket, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        return std::string(ip);
    }
    return {};
}

uint16_t NetworkSocket::GetLocalPort() const {
    if (!IsValid()) return 0;
    sockaddr_in addr{};
    int len = sizeof(addr);
    if (getsockname(m_socket, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        return ntohs(addr.sin_port);
    }
    return 0;
}

std::string NetworkSocket::GetLastErrorMessage(int err) {
    if (err < 0) {
        err = WSAGetLastError();
    }
    switch (err) {
        case 0: return "No error";
        case WSAEINTR: return "Interrupted function call (WSAEINTR)";
        case WSAEBADF: return "Bad file number (WSAEBADF)";
        case WSAEACCES: return "Permission denied (WSAEACCES)";
        case WSAEFAULT: return "Bad address pointer (WSAEFAULT)";
        case WSAEINVAL: return "Invalid argument (WSAEINVAL)";
        case WSAEMFILE: return "Too many open sockets (WSAEMFILE)";
        case WSAEWOULDBLOCK: return "Resource temporarily unavailable (WSAEWOULDBLOCK)";
        case WSAEINPROGRESS: return "Operation now in progress (WSAEINPROGRESS)";
        case WSAEALREADY: return "Operation already in progress (WSAEALREADY)";
        case WSAENOTSOCK: return "Socket operation on nonsocket (WSAENOTSOCK)";
        case WSAEDESTADDRREQ: return "Destination address required (WSAEDESTADDRREQ)";
        case WSAEMSGSIZE: return "Message too long (WSAEMSGSIZE)";
        case WSAEPROTOTYPE: return "Protocol wrong type for socket (WSAEPROTOTYPE)";
        case WSAENOPROTOOPT: return "Bad protocol option (WSAENOPROTOOPT)";
        case WSAEPROTONOSUPPORT: return "Protocol not supported (WSAEPROTONOSUPPORT)";
        case WSAESOCKTNOSUPPORT: return "Socket type not supported (WSAESOCKTNOSUPPORT)";
        case WSAEOPNOTSUPP: return "Operation not supported (WSAEOPNOTSUPP)";
        case WSAEPFNOSUPPORT: return "Protocol family not supported (WSAEPFNOSUPPORT)";
        case WSAEAFNOSUPPORT: return "Address family not supported by protocol family (WSAEAFNOSUPPORT)";
        case WSAEADDRINUSE: return "Address already in use (WSAEADDRINUSE)";
        case WSAEADDRNOTAVAIL: return "Cannot assign requested address (WSAEADDRNOTAVAIL)";
        case WSAENETDOWN: return "Network is down (WSAENETDOWN)";
        case WSAENETUNREACH: return "Network is unreachable (WSAENETUNREACH)";
        case WSAENETRESET: return "Network dropped connection on reset (WSAENETRESET)";
        case WSAECONNABORTED: return "Software caused connection abort (WSAECONNABORTED)";
        case WSAECONNRESET: return "Connection reset by peer (WSAECONNRESET)";
        case WSAENOBUFS: return "No buffer space available (WSAENOBUFS)";
        case WSAEISCONN: return "Socket is already connected (WSAEISCONN)";
        case WSAENOTCONN: return "Socket is not connected (WSAENOTCONN)";
        case WSAESHUTDOWN: return "Cannot send after socket shutdown (WSAESHUTDOWN)";
        case WSAETOOMANYREFS: return "Too many references (WSAETOOMANYREFS)";
        case WSAETIMEDOUT: return "Connection timed out (WSAETIMEDOUT)";
        case WSAECONNREFUSED: return "Connection refused (WSAECONNREFUSED)";
        case WSAELOOP: return "Cannot translate name (WSAELOOP)";
        case WSAENAMETOOLONG: return "Name too long (WSAENAMETOOLONG)";
        case WSAEHOSTDOWN: return "Host is down (WSAEHOSTDOWN)";
        case WSAEHOSTUNREACH: return "No route to host (WSAEHOSTUNREACH)";
        case WSAENOTEMPTY: return "Directory not empty (WSAENOTEMPTY)";
        case WSAEPROCLIM: return "Too many processes (WSAEPROCLIM)";
        case WSAEUSERS: return "User quota exceeded (WSAEUSERS)";
        case WSAEDQUOT: return "Disk quota exceeded (WSAEDQUOT)";
        case WSAESTALE: return "Stale file handle (WSAESTALE)";
        case WSAEREMOTE: return "Item is remote (WSAEREMOTE)";
        case WSASYSNOTREADY: return "Network subsystem is unavailable (WSASYSNOTREADY)";
        case WSAVERNOTSUPPORTED: return "Winsock.dll version out of range (WSAVERNOTSUPPORTED)";
        case WSANOTINITIALISED: return "Successful WSAStartup not yet performed (WSANOTINITIALISED)";
        case WSAEDISCON: return "Graceful shutdown in progress (WSAEDISCON)";
        default: {
            char buf[64];
            snprintf(buf, sizeof(buf), "Winsock error code %d", err);
            return std::string(buf);
        }
    }
}

void NetworkSocket::LogError(const char* context, int err) {
    if (err < 0) {
        err = WSAGetLastError();
    }
    std::string err_str = GetLastErrorMessage(err);
    std::cerr << "[RemoteGPU Transport Error] " << context << ": " << err_str << " (code " << err << ")\n";
}

} // namespace RemoteGPU
```

---

## 7. Verification Evidence & Test Execution Results

The implementation was compiled with `clang++ -std=c++17 -Wall -Wextra` and linked against `-lws2_32`. All six test cases executed and passed with return code 0:

| # | Test Scenario | Verified Behavior | Status |
|---|---------------|-------------------|:------:|
| 1 | Ephemeral Port Bind & Listen | Server binds to `127.0.0.1:0`; OS allocates dynamic port; `GetLocalPort()` returns non-zero port | PASS |
| 2 | Connect & Accept | Client connects via loopback; server accepts connection; socket properties verified | PASS |
| 3 | Small Packet Framing | Transmitted 35-byte text payload; `PacketHeader` (16B) + payload parsed; magic, opcode, sequence verified | PASS |
| 4 | Large 1 MB Binary Transfer | Transmitted 1,048,576 bytes; `WSASend` / `SendExact` and `RecvExact` verified bitwise equality | PASS |
| 5 | Peer Disconnection Detection | Client closed connection; server `RecvPacket` detected clean EOF (return false) without crashing | PASS |
| 6 | Closed Port Connection Failure | Client connected to closed port; aborted within 500ms timeout with `WSAETIMEDOUT` without hanging | PASS |

```
Execution Output:
[Test] Initializing network...
[Test 1] Testing BindAndListen on ephemeral port...
  Server listening on 127.0.0.1:58862
[Test 2] Testing Connect and Accept...
  Connected! Client peer: 127.0.0.1:58862
[Test 3] Testing SendPacket / RecvPacket with small payload...
  Received packet verified: Hello Remote GPU Offload Protocol!
[Test 4] Testing 1 MB payload transfer...
  1 MB transfer verified with bitwise equality!
[Test 5] Testing graceful disconnect detection...
  Server cleanly detected client disconnect!
[Test 6] Testing connection failure without hang...
[RemoteGPU Transport Error] Connect: failed to connect to host: Connection timed out (WSAETIMEDOUT) (code 10060)
  Connection to closed port failed cleanly within timeout!
[All Tests Passed Successfully!]
```

---

## 8. Recommendations for Milestone 1 Implementer

1. **Header Inclusion Order**: When implementing in `src/common/transport.h`, ensure `#include <winsock2.h>` appears before any Windows header, preceded by `#define WIN32_LEAN_AND_MEAN`.
2. **Build Configuration**: Link with `-lws2_32` on MinGW/Clang, or `ws2_32.lib` on MSVC.
3. **No Dynamic Allocations in Frame Receive**: When streaming framebuffers in M2/M3, use the zero-allocation overload `RecvPacket(out_header, buffer_ptr, capacity)` to receive pixel bytes directly into staging textures or DIB buffers without heap reallocations.
