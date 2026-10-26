#pragma once
#ifndef REMOTE_GPU_SERVER_DISPATCHER_H
#define REMOTE_GPU_SERVER_DISPATCHER_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>

#include "protocol.h"
#include "transport.h"
#include "gpu_device.h"

namespace RemoteGPU {

/**
 * @brief Server Resource Manager tracking the mapping between client-side monotonic
 * resource IDs and host Direct3D 11 COM objects.
 *
 * All primary Direct3D 11 resources (ID3D11Buffer, ID3D11Texture2D, ID3D11ShaderResourceView,
 * ID3D11RenderTargetView, ID3D11DepthStencilView, ID3D11VertexShader, ID3D11PixelShader,
 * ID3D11InputLayout, ID3D11SamplerState) inherit from ID3D11DeviceChild. This enables a unified,
 * type-safe lookup map with query-interface validation.
 */
class ServerResourceManager {
public:
    ServerResourceManager() = default;
    ~ServerResourceManager() { Clear(); }

    // Non-copyable, movable
    ServerResourceManager(const ServerResourceManager&) = delete;
    ServerResourceManager& operator=(const ServerResourceManager&) = delete;
    ServerResourceManager(ServerResourceManager&&) noexcept = default;
    ServerResourceManager& operator=(ServerResourceManager&&) noexcept = default;

    // Direct insertion / registration
    void Register(uint32_t res_id, Microsoft::WRL::ComPtr<ID3D11DeviceChild> resource);

    // Destruction
    bool Destroy(uint32_t res_id);

    // Clear all tracked resources
    void Clear();

    // Query presence
    bool HasResource(uint32_t res_id) const;
    size_t Count() const;

    // Generic typed retrieval via COM QueryInterface (ComPtr::As)
    template <typename T>
    Microsoft::WRL::ComPtr<T> Get(uint32_t res_id) const {
        if (res_id == 0) return nullptr;
        auto it = m_resources.find(res_id);
        if (it == m_resources.end() || !it->second) {
            return nullptr;
        }
        Microsoft::WRL::ComPtr<T> typedObj;
        HRESULT hr = it->second.As(&typedObj);
        if (FAILED(hr)) {
            return nullptr;
        }
        return typedObj;
    }

    // Explicit typed accessors
    Microsoft::WRL::ComPtr<ID3D11Buffer> GetBuffer(uint32_t res_id) const { return Get<ID3D11Buffer>(res_id); }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> GetTexture2D(uint32_t res_id) const { return Get<ID3D11Texture2D>(res_id); }
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> GetSRV(uint32_t res_id) const { return Get<ID3D11ShaderResourceView>(res_id); }
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> GetRTV(uint32_t res_id) const { return Get<ID3D11RenderTargetView>(res_id); }
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> GetDSV(uint32_t res_id) const { return Get<ID3D11DepthStencilView>(res_id); }
    Microsoft::WRL::ComPtr<ID3D11VertexShader> GetVertexShader(uint32_t res_id) const { return Get<ID3D11VertexShader>(res_id); }
    Microsoft::WRL::ComPtr<ID3D11PixelShader> GetPixelShader(uint32_t res_id) const { return Get<ID3D11PixelShader>(res_id); }
    Microsoft::WRL::ComPtr<ID3D11InputLayout> GetInputLayout(uint32_t res_id) const { return Get<ID3D11InputLayout>(res_id); }
    Microsoft::WRL::ComPtr<ID3D11SamplerState> GetSamplerState(uint32_t res_id) const { return Get<ID3D11SamplerState>(res_id); }
    Microsoft::WRL::ComPtr<ID3D11DeviceChild> GetRaw(uint32_t res_id) const;

    const std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11DeviceChild>>& GetAll() const { return m_resources; }

private:
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11DeviceChild>> m_resources;
};

/**
 * @brief Command Dispatcher parsing incoming network command packets and executing
 * corresponding Direct3D 11 API operations against HostGpuDevice.
 */
class ServerDispatcher {
public:
    explicit ServerDispatcher(HostGpuDevice& gpuDevice);
    ~ServerDispatcher();

    // Non-copyable
    ServerDispatcher(const ServerDispatcher&) = delete;
    ServerDispatcher& operator=(const ServerDispatcher&) = delete;

    // Main session packet processing loop over socket
    bool ProcessPacketStream(NetworkSocket& socket);

    // Single command dispatch (callable directly or from packet stream)
    bool DispatchCommand(const PacketHeader& header, const uint8_t* payload, size_t payload_size, NetworkSocket* socket = nullptr);

    // Session control & lifecycle
    void ResetSession();
    bool IsHandshakeCompleted() const { return m_handshakeCompleted; }
    uint64_t GetTotalCommandsProcessed() const { return m_totalCommandsProcessed; }
    uint64_t GetTotalFramesPresented() const { return m_totalFramesPresented; }
    uint64_t GetLastPresentedFrame() const { return m_lastPresentedFrame; }
    bool IsFrameCompressionEnabled() const { return m_frameCompression; }
    uint64_t GetRawFrameBytes() const { return m_rawFrameBytes; }    // pixel bytes rendered
    uint64_t GetSentFrameBytes() const { return m_sentFrameBytes; }  // pixel bytes put on the wire

    ServerResourceManager& GetResourceManager() { return m_resources; }
    const ServerResourceManager& GetResourceManager() const { return m_resources; }

    // Direct readback buffer accessor for testing
    // (with frame compression the last frame is kept as the delta reference)
    const std::vector<uint8_t>& GetLastReadbackBuffer() const {
        return m_frameCompression ? m_previousFrame : m_frameReadbackBuffer;
    }

private:
    // Individual opcode handlers
    bool HandleHandshakeRequest(const PacketHeader& header, const uint8_t* payload, size_t size, NetworkSocket* socket);
    bool HandleShutdown(const PacketHeader& header, const uint8_t* payload, size_t size);

    // Resource creation handlers
    bool HandleCreateBuffer(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCreateTexture2D(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCreateTexture3D(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCreateTexture1D(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCreateSrv(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCreateRtv(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCreateDsv(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCreateVertexShader(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCreatePixelShader(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCreateInputLayout(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCreateSampler(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleDestroyResource(const PacketHeader& header, const uint8_t* payload, size_t size);

    // Pipeline state handlers
    bool HandleIASetInputLayout(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleIASetVertexBuffer(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleIASetIndexBuffer(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleIASetTopology(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleVSSetShader(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleVSSetCB(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandlePSSetShader(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandlePSSetCB(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandlePSSetSrv(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandlePSSetSampler(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleRSSetViewport(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleOMSetRenderTargets(const PacketHeader& header, const uint8_t* payload, size_t size);

    // Updates & draw call handlers
    bool HandleUpdateSubresource(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleWriteBufferDiscard(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleClearRtv(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleClearDsv(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleDraw(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleDrawIndexed(const PacketHeader& header, const uint8_t* payload, size_t size);

    // Instancing, multiple render targets, vertex shader resources
    bool HandleDrawInstanced(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleDrawIndexedInstanced(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleOMSetRenderTargetsMrt(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleVSSetSrv(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleVSSetSampler(const PacketHeader& header, const uint8_t* payload, size_t size);

    // Compute shaders & unordered access views
    bool HandleCreateComputeShader(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCreateUav(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCSSetShader(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCSSetSrv(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCSSetUav(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCSSetCB(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCSSetSampler(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleDispatch(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleClearUav(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCopyResource(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCopySubresourceRegion(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleOMSetRtvsAndUavs(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCreateStageShader(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleStageSetShader(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleStageBind(const PacketHeader& header, const uint8_t* payload, size_t size);

    // Fixed-function state handlers
    bool HandleCreateBlendState(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCreateDepthStencilState(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleCreateRasterizerState(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleOMSetBlendState(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleOMSetDepthStencilState(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleRSSetState(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandleRSSetScissor(const PacketHeader& header, const uint8_t* payload, size_t size);

    // Presentation handlers
    bool HandleResizeBuffers(const PacketHeader& header, const uint8_t* payload, size_t size);
    bool HandlePresent(const PacketHeader& header, const uint8_t* payload, size_t size, NetworkSocket* socket);

private:
    HostGpuDevice& m_gpuDevice;
    ServerResourceManager m_resources;

    bool m_handshakeCompleted = false;
    uint32_t m_clientProtocolVersion = 0;
    uint32_t m_frameWidth = 0;
    uint32_t m_frameHeight = 0;
    uint64_t m_lastPresentedFrame = 0;

    std::vector<uint8_t> m_frameReadbackBuffer;

    // Frame compression (negotiated through CLIENT_FLAG_FRAME_COMPRESSION)
    bool m_frameCompression = false;
    std::vector<uint8_t> m_previousFrame;  // last frame the client holds (delta reference)
    std::vector<uint8_t> m_encodedFrame;
    uint64_t m_rawFrameBytes = 0;
    uint64_t m_sentFrameBytes = 0;

    uint64_t m_totalCommandsProcessed = 0;
    uint64_t m_totalFramesPresented = 0;
};

} // namespace RemoteGPU

#endif // REMOTE_GPU_SERVER_DISPATCHER_H
