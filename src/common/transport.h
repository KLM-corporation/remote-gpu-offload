#pragma once
#ifndef REMOTE_GPU_TRANSPORT_H
#define REMOTE_GPU_TRANSPORT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
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
    SOCKET GetSocket() const noexcept { return GetRawSocket(); }
    SOCKET Release() noexcept;

    bool SetTimeouts(int send_timeout_ms, int recv_timeout_ms);
    bool EnableKeepAlive(unsigned long idle_ms, unsigned long interval_ms);
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
