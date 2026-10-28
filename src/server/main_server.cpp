// ============================================================================
// Remote GPU Offload - Milestone 2 Standalone Server Entrypoint
// File: src/server/main_server.cpp
// Standard: C++17
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <iomanip>
#include <iostream>
#include <string>
#include <cstdlib>
#include <atomic>

#include "protocol.h"
#include "transport.h"
#include "gpu_device.h"
#include "server_dispatcher.h"
#include "gl_server_dispatcher.h"

using namespace RemoteGPU;

// Global shutdown flag controlled by Windows console control handler
static std::atomic<bool> g_server_running{true};

/**
 * @brief Win32 Console Control Handler for graceful shutdown on Ctrl+C, Ctrl+Break, or Close.
 */
// Windows consoles pause the process on any mouse click (QuickEdit selection) until a key is
// pressed: a stray click froze rendering. Turn that behaviour off and keep stdout line-buffered.
static void DisableConsoleQuickEdit() {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode)) {
        mode &= ~static_cast<DWORD>(ENABLE_QUICK_EDIT_MODE | ENABLE_INSERT_MODE);
        mode |= ENABLE_EXTENDED_FLAGS;
        SetConsoleMode(in, mode);
    }
}

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            std::cout << "\n[Server] Interruption signal received. Initiating graceful shutdown..." << std::endl;
            g_server_running = false;
            return TRUE;
        default:
            return FALSE;
    }
}

/**
 * @brief Print command-line usage information.
 */
static void PrintUsage(const char* exe_name) {
    std::cout << "Remote GPU Offload Server (Direct3D 11 Execution Daemon)\n\n"
              << "Usage: " << exe_name << " [options]\n\n"
              << "Options:\n"
              << "  --port <port>        TCP port to listen on (default: " << RGPU_DEFAULT_PORT << ")\n"
              << "  --adapter <index>    DXGI adapter index to bind (-1 for auto-select dedicated GPU, default: -1)\n"
              << "  --bind <ip>          Bind IP address (default: 0.0.0.0)\n"
              << "  --single-session     Exit cleanly after a single client session completes\n"
              << "  -h, --help           Show this help message and exit\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    uint16_t port = RGPU_DEFAULT_PORT;
    int adapter_index = -1;
    std::string bind_ip = "0.0.0.0";
    bool single_session = false;

    // 1. Parse Command-Line Arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port") {
            if (i + 1 < argc) {
                int p = std::atoi(argv[++i]);
                if (p > 0 && p <= 65535) {
                    port = static_cast<uint16_t>(p);
                } else {
                    std::cerr << "[Server Error] Invalid port number: " << argv[i] << " (must be 1-65535)" << std::endl;
                    return 1;
                }
            } else {
                std::cerr << "[Server Error] Missing value for --port argument" << std::endl;
                return 1;
            }
        } else if (arg == "--adapter") {
            if (i + 1 < argc) {
                adapter_index = std::atoi(argv[++i]);
            } else {
                std::cerr << "[Server Error] Missing value for --adapter argument" << std::endl;
                return 1;
            }
        } else if (arg == "--bind") {
            if (i + 1 < argc) {
                bind_ip = argv[++i];
            } else {
                std::cerr << "[Server Error] Missing value for --bind argument" << std::endl;
                return 1;
            }
        } else if (arg == "--single-session") {
            single_session = true;
        } else if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "[Server Error] Unrecognized argument: " << arg << std::endl;
            PrintUsage(argv[0]);
            return 1;
        }
    }

    std::cout << "============================================================" << std::endl;
    std::cout << "  Remote GPU Offloading Execution Server Daemon (Milestone 2)" << std::endl;
    std::cout << "============================================================" << std::endl;

    // 2. Register Console Signal Handler
    DisableConsoleQuickEdit();
    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
        std::cerr << "[Server] Warning: Failed to set console control handler." << std::endl;
    }

    // 3. Initialize Winsock2
    if (!NetworkSocket::InitializeNetwork()) {
        std::cerr << "[Server] FATAL: Winsock2 initialization failed!" << std::endl;
        return 1;
    }

    // GPU initialization is deferred until the first non-OpenGL session. OpenGL uses its own
    // WGL host device and must remain usable on machines without a D3D11 adapter.
    HostGpuDevice gpu;

    // Bind and listen before initializing an API-specific device so the daemon can accept either
    // protocol without paying for (or requiring) the other graphics stack.
    NetworkSocket listen_sock;
    if (!listen_sock.BindAndListen(port, bind_ip.c_str())) {
        std::cerr << "[Server] FATAL: Failed to bind and listen on " << bind_ip << ":" << port << std::endl;
        gpu.Reset();
        NetworkSocket::ShutdownNetwork();
        return 1;
    }

    std::cout << "[Server] Listening for client connections on " << bind_ip << ":" << port << "..." << std::endl;
    std::cout << "[Server] Ready to receive offloading workloads. Press Ctrl+C to terminate." << std::endl;

    uint32_t session_counter = 0;

    // 6. Main Connection Accept Loop
    while (g_server_running) {
        // Accept with 500ms timeout to periodically evaluate g_server_running
        auto client_sock = listen_sock.Accept(500);
        if (!client_sock) {
            continue; // Timeout expired, re-check shutdown condition
        }

        session_counter++;
        std::cout << "\n[Server] Client connection #" << session_counter << " accepted from "
                  << client_sock->GetPeerAddress() << ":" << client_sock->GetPeerPort() << std::endl;

        // A client may legitimately stay idle (minimised window, level loading), so the session
        // must not end on a receive timeout. TCP keepalive detects peers that silently vanished.
        client_sock->SetTimeouts(10000, 0);
        if (!client_sock->EnableKeepAlive(5000, 1000)) {
            std::cerr << "[Server] Warning: TCP keepalive could not be enabled for this session." << std::endl;
        }

        // The first packet is the handshake; its flags say which graphics API the client speaks.
        PacketHeader first = {};
        std::vector<uint8_t> first_payload;
        if (!client_sock->RecvPacket(first, first_payload)) {
            std::cout << "[Server] Client left before sending a handshake." << std::endl;
            client_sock->Close();
            if (single_session) break;
            continue;
        }
        const bool is_opengl = first.opcode == OP_HANDSHAKE_REQUEST &&
                               first_payload.size() >= sizeof(HandshakeRequestPayload) &&
                               (reinterpret_cast<const HandshakeRequestPayload*>(first_payload.data())->client_flags &
                                CLIENT_FLAG_API_OPENGL) != 0;

        auto report_frames = [](uint64_t raw_bytes, uint64_t sent_bytes, bool compression) {
            if (raw_bytes == 0) return;
            const double raw_mb = raw_bytes / (1024.0 * 1024.0);
            const double sent_mb = sent_bytes / (1024.0 * 1024.0);
            std::cout << "[Server] Frame stream: " << std::fixed << std::setprecision(2) << raw_mb << " MB rendered, "
                      << sent_mb << " MB sent (" << std::setprecision(1) << (100.0 * sent_mb / raw_mb) << "%, compression "
                      << (compression ? "on" : "off") << ")" << std::defaultfloat << std::endl;
        };

        if (is_opengl) {
            GlServerDispatcher gl_dispatcher;
            std::cout << "[Server] Session #" << session_counter << " started (OpenGL). Processing command stream..." << std::endl;
            const bool session_ok = gl_dispatcher.ProcessPacketStream(*client_sock, first, first_payload);
            std::cout << "[Server] Session #" << session_counter << " finished (Status: "
                      << (session_ok ? "Normal" : "Disconnected/Aborted") << ")." << std::endl;
            std::cout << "[Server] Total commands executed: " << gl_dispatcher.GetTotalCommandsProcessed()
                      << ", Total frames presented: " << gl_dispatcher.GetTotalFramesPresented() << std::endl;
            report_frames(gl_dispatcher.GetRawFrameBytes(), gl_dispatcher.GetSentFrameBytes(),
                          gl_dispatcher.IsFrameCompressionEnabled());
            client_sock->Close();
        } else {
            // Initialize the D3D device only when a D3D client actually connects.
            if (!gpu.GetDevice()) {
                std::cout << "[Server] Enumerating DXGI adapters and initializing dedicated GPU..." << std::endl;
                if (!gpu.InitializeDedicatedGpu(adapter_index)) {
                    std::cerr << "[Server] Failed to initialize host GPU device; rejecting D3D session." << std::endl;
                    client_sock->Close();
                    if (single_session) break;
                    continue;
                }
                std::cout << "[Server] Active GPU Adapter : " << gpu.GetAdapterDescription() << std::endl;
                std::cout << "[Server] Vendor ID / Device : 0x" << std::hex << gpu.GetVendorId()
                          << " / 0x" << gpu.GetDeviceId() << std::dec << std::endl;
                std::cout << "[Server] Dedicated VRAM     : " << (gpu.GetDedicatedVram() / (1024 * 1024)) << " MB" << std::endl;
                std::cout << "[Server] Shared System RAM  : " << (gpu.GetSharedRam() / (1024 * 1024)) << " MB" << std::endl;
                std::cout << "[Server] Direct3D 11 Level  : 0x" << std::hex << gpu.GetFeatureLevel() << std::dec << std::endl;
            }

            ServerDispatcher dispatcher(gpu);

            std::cout << "[Server] Session #" << session_counter << " started. Processing command stream..." << std::endl;
            bool session_ok = dispatcher.ProcessPacketStream(*client_sock, &first, &first_payload);

            std::cout << "[Server] Session #" << session_counter << " finished (Status: "
                      << (session_ok ? "Normal" : "Disconnected/Aborted") << ")." << std::endl;
            std::cout << "[Server] Total commands executed: " << dispatcher.GetTotalCommandsProcessed()
                      << ", Total frames presented: " << dispatcher.GetTotalFramesPresented() << std::endl;
            report_frames(dispatcher.GetRawFrameBytes(), dispatcher.GetSentFrameBytes(),
                          dispatcher.IsFrameCompressionEnabled());

            // Clean disconnect handling: Close client socket & reset session state
            client_sock->Close();
            dispatcher.ResetSession();
        }

        std::cout << "[Server] Session resources cleanly released. Ready for next connection." << std::endl;

        if (single_session) {
            std::cout << "[Server] --single-session specified: terminating server loop." << std::endl;
            break;
        }
    }

    // 7. Clean Shutdown & Resource Deallocation
    std::cout << "\n[Server] Shutting down listener socket..." << std::endl;
    listen_sock.Close();

    std::cout << "[Server] Releasing Host GPU resources..." << std::endl;
    gpu.Reset();

    std::cout << "[Server] Shutting down Winsock2 subsystem..." << std::endl;
    NetworkSocket::ShutdownNetwork();

    std::cout << "[Server] Execution server shutdown complete. Exit code 0." << std::endl;
    return 0;
}
