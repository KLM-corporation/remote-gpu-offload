#pragma once
#ifndef REMOTE_GPU_GL_SERVER_DISPATCHER_H
#define REMOTE_GPU_GL_SERVER_DISPATCHER_H

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <GL/gl.h>

#include "gl_host_device.h"
#include "gl_protocol.h"
#include "transport.h"

namespace RemoteGPU {

/**
 * @brief Executes one OpenGL client session on a GlHostDevice.
 *
 * Every client-supplied value is validated before it reaches the driver: unknown functions,
 * wrong argument sizes, malformed batches and out-of-range sizes end the session.
 */
class GlServerDispatcher {
public:
    GlServerDispatcher() = default;

    // `first` is the packet main() already read to pick the API (the handshake request).
    bool ProcessPacketStream(NetworkSocket& socket, const PacketHeader& first,
                             const std::vector<uint8_t>& first_payload);

    uint64_t GetTotalCommandsProcessed() const noexcept { return m_commands; }
    uint64_t GetTotalFramesPresented() const noexcept { return m_frames; }
    uint64_t GetRawFrameBytes() const noexcept { return m_rawBytes; }
    uint64_t GetSentFrameBytes() const noexcept { return m_sentBytes; }
    bool IsFrameCompressionEnabled() const noexcept { return m_compression; }

    // Executes a batch of GL commands; false on any malformed or unsupported command.
    bool ExecuteCommands(const uint8_t* data, size_t size);

private:
    bool Dispatch(const PacketHeader& header, const uint8_t* payload, size_t size, NetworkSocket& socket);
    bool HandleHandshake(const PacketHeader& header, const uint8_t* payload, size_t size, NetworkSocket& socket);
    bool HandleCreateContext(const PacketHeader& header, const uint8_t* payload, size_t size, NetworkSocket& socket);
    bool HandleResize(const uint8_t* payload, size_t size);
    bool HandleSwap(const PacketHeader& header, const uint8_t* payload, size_t size, NetworkSocket& socket);

    GlHostDevice m_device;
    bool     m_handshakeDone = false;
    bool     m_compression = false;
    uint32_t m_initialWidth = 0;
    uint32_t m_initialHeight = 0;

    std::vector<uint8_t> m_frame;
    std::vector<uint8_t> m_previousFrame;
    std::vector<uint8_t> m_encoded;

    uint64_t m_commands = 0;
    uint64_t m_frames = 0;
    uint64_t m_rawBytes = 0;
    uint64_t m_sentBytes = 0;

    struct Buffer { uint32_t target = 0; GLuint gl_id = 0; std::vector<uint8_t> data; };
    std::unordered_map<uint32_t, Buffer> m_buffers;
    std::unordered_map<uint32_t, GLuint> m_shaders;
    std::unordered_map<uint32_t, GLuint> m_programs;
    uint32_t m_boundArrayBuffer = 0;
};

} // namespace RemoteGPU

#endif // REMOTE_GPU_GL_SERVER_DISPATCHER_H
