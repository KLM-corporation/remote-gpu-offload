#pragma once
#ifndef REMOTE_GPU_PROTOCOL_H
#define REMOTE_GPU_PROTOCOL_H

#include <cstdint>

namespace RemoteGPU {

constexpr uint32_t RGPU_MAGIC = 0x52475055; // 'RGPU'
constexpr uint32_t RGPU_PROTOCOL_VERSION = 1;
constexpr uint16_t RGPU_DEFAULT_PORT = 48888;

#pragma pack(push, 1)

struct PacketHeader {
    uint32_t magic;          // RGPU_MAGIC
    uint16_t opcode;         // RgpuOpcode
    uint16_t flags;          // RgpuFlags
    uint32_t sequence_id;    // Monotonic packet sequence number
    uint32_t payload_length; // Size of following payload in bytes
};

#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 16, "PacketHeader must be exactly 16 bytes");

enum RgpuFlags : uint16_t {
    FLAG_NONE              = 0x0000,
    FLAG_SYNC              = 0x0001,
    FLAG_RESPONSE_REQUIRED = 0x0002,
    FLAG_COMPRESSED        = 0x0004,
    FLAG_ONEWAY            = 0x0008,
    FLAG_END_OF_FRAME      = 0x0010,
    FLAG_HEARTBEAT         = 0x0020
};

} // namespace RemoteGPU

#endif // REMOTE_GPU_PROTOCOL_H
