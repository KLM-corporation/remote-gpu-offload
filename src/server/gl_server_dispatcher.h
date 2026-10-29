#pragma once
#ifndef REMOTE_GPU_GL_SERVER_DISPATCHER_H
#define REMOTE_GPU_GL_SERVER_DISPATCHER_H

#include "transport.h"
#include "gl_host_device.h"
#include "gl_protocol.h"
#include "video_codec.h"

#include <cstdint>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <GL/gl.h>

namespace RemoteGPU {

struct GlDispatchApi;

/**
 * @brief Executes one OpenGL client session on a GlHostDevice.
 *
 * Every client-supplied value is validated before it reaches the driver: unknown functions,
 * wrong argument sizes, malformed batches and out-of-range sizes end the session.
 */
class GlServerDispatcher {
public:
    GlServerDispatcher();
    ~GlServerDispatcher();

    GlServerDispatcher(const GlServerDispatcher&) = delete;
    GlServerDispatcher& operator=(const GlServerDispatcher&) = delete;
    GlServerDispatcher(GlServerDispatcher&&) noexcept;
    GlServerDispatcher& operator=(GlServerDispatcher&&) noexcept;

    // `first` is the packet main() already read to pick the API (the handshake request).
    bool ProcessPacketStream(NetworkSocket& socket, const PacketHeader& first,
                             const std::vector<uint8_t>& first_payload);

    uint64_t GetTotalCommandsProcessed() const noexcept { return m_commands; }
    uint64_t GetTotalFramesPresented() const noexcept { return m_frames; }
    uint64_t GetRawFrameBytes() const noexcept { return m_rawBytes; }
    uint64_t GetSentFrameBytes() const noexcept { return m_sentBytes; }
    bool IsFrameCompressionEnabled() const noexcept { return m_compression; }
    bool IsVideoEnabled() const noexcept { return m_video; }
    uint32_t GetVideoBitrateKbps() const noexcept { return m_videoKbps; }

    // Executes a batch of GL commands; false on any malformed or unsupported command.
    bool ExecuteCommands(const uint8_t* data, size_t size);

private:
    bool Dispatch(const PacketHeader& header, const uint8_t* payload, size_t size, NetworkSocket& socket);
    bool HandleHandshake(const PacketHeader& header, const uint8_t* payload, size_t size, NetworkSocket& socket);
    bool HandleCreateContext(const PacketHeader& header, const uint8_t* payload, size_t size, NetworkSocket& socket);
    bool HandleResize(const uint8_t* payload, size_t size);
    bool HandleSwap(const PacketHeader& header, const uint8_t* payload, size_t size, NetworkSocket& socket);
    bool PrepareVideo(uint32_t width, uint32_t height);
    bool EncodeVideo(uint32_t width, uint32_t height);
    bool LoadGlApi();

    GlHostDevice m_device;
    bool     m_handshakeDone = false;
    bool     m_compression = false;
    bool     m_video = false;               // the client decodes FRAME_CODEC_H264
    uint32_t m_videoKbps = 40000;
    uint32_t m_videoFailedWidth = 0;        // size no encoder accepted: lossless frames until it changes
    uint32_t m_videoFailedHeight = 0;
    uint32_t m_videoPendingWidth = 0;       // new size waiting to hold before an encoder is made for it
    uint32_t m_videoPendingHeight = 0;
    uint32_t m_videoPendingFrames = 0;
    std::unique_ptr<Video::H264Encoder> m_encoder;
    uint32_t m_initialWidth = 0;
    uint32_t m_initialHeight = 0;

    std::vector<uint8_t> m_frame;
    std::vector<uint8_t> m_previousFrame;
    std::vector<uint8_t> m_encoded;

    uint64_t m_commands = 0;
    uint64_t m_frames = 0;
    uint64_t m_rawBytes = 0;
    uint64_t m_sentBytes = 0;

    struct Buffer { uint32_t target = 0; GLuint gl_id = 0; uint64_t gpu_size = 0; };
    std::unordered_map<uint32_t, Buffer> m_buffers;
    std::unordered_map<uint32_t, GLuint> m_shaders;
    std::unordered_map<uint32_t, GLuint> m_programs;
    std::unordered_map<uint32_t, GLuint> m_textures;
    std::unordered_map<uint32_t, GLuint> m_framebuffers;
    std::unordered_map<uint32_t, GLuint> m_vaos;
    std::unordered_map<uint32_t, GLuint> m_samplers;
    std::unordered_map<uint32_t, GLuint> m_renderbuffers;
    std::unordered_map<int32_t, int32_t> m_uniformLocs; // client location -> host location
    // Per-stage timings since the last report (see HandleSwap).
    double   m_perfExecuteMs = 0.0;
    double   m_perfReadbackMs = 0.0;
    double   m_perfEncodeMs = 0.0;
    double   m_perfSendMs = 0.0;
    uint64_t m_perfCommandBytes = 0;
    uint64_t m_perfFrameBytes = 0;
    uint32_t m_oversizedWrites = 0;              // writes past a buffer end, logged for the first few
    uint16_t m_lastFunc = 0;                    // function being executed, 0 while reading a command header
    std::unordered_set<uint16_t> m_unknownFuncs; // unknown function ids already reported
    bool m_drawFboDefault = true; // client FBO 0 (host offscreen FBO) bound as draw target
    bool m_readFboDefault = true;
    uint32_t m_boundArrayBuffer = 0;
    std::unordered_map<uint32_t, uint32_t> m_boundBuffers;

    std::unique_ptr<GlDispatchApi> m_gl;
};

} // namespace RemoteGPU

#endif // REMOTE_GPU_GL_SERVER_DISPATCHER_H
