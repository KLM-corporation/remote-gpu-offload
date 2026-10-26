#include "draft_transport.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

using namespace RemoteGPU;

int main() {
    std::cout << "[Test] Initializing network...\n";
    assert(NetworkSocket::InitializeNetwork());

    // Test 1: Bind and Listen on an ephemeral port (port 0)
    std::cout << "[Test 1] Testing BindAndListen on ephemeral port...\n";
    NetworkSocket server_sock;
    assert(server_sock.BindAndListen(0, "127.0.0.1"));
    uint16_t port = server_sock.GetLocalPort();
    std::cout << "  Server listening on 127.0.0.1:" << port << "\n";
    assert(port > 0);

    // Test 2: Client Connect & Server Accept in threads
    std::cout << "[Test 2] Testing Connect and Accept...\n";
    std::unique_ptr<NetworkSocket> accepted_sock;
    NetworkSocket client_sock;

    std::thread server_thread([&]() {
        accepted_sock = server_sock.Accept(3000);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(client_sock.Connect("127.0.0.1", port, 3000));

    server_thread.join();
    assert(accepted_sock != nullptr);
    assert(accepted_sock->IsValid());
    std::cout << "  Connected! Client peer: " << client_sock.GetPeerAddress() << ":" << client_sock.GetPeerPort() << "\n";

    // Test 3: SendPacket & RecvPacket (small payload)
    std::cout << "[Test 3] Testing SendPacket / RecvPacket with small payload...\n";
    PacketHeader hdr{};
    hdr.magic = RGPU_MAGIC;
    hdr.opcode = 0x0001;
    hdr.flags = FLAG_NONE;
    hdr.sequence_id = 42;
    std::string test_msg = "Hello Remote GPU Offload Protocol!";
    hdr.payload_length = static_cast<uint32_t>(test_msg.size());

    assert(client_sock.SendPacket(hdr, test_msg.data()));

    PacketHeader recv_hdr{};
    std::vector<uint8_t> recv_payload;
    assert(accepted_sock->RecvPacket(recv_hdr, recv_payload));

    assert(recv_hdr.magic == RGPU_MAGIC);
    assert(recv_hdr.opcode == 0x0001);
    assert(recv_hdr.sequence_id == 42);
    assert(recv_hdr.payload_length == test_msg.size());
    std::string received_msg(recv_payload.begin(), recv_payload.end());
    assert(received_msg == test_msg);
    std::cout << "  Received packet verified: " << received_msg << "\n";

    // Test 4: SendPacket & RecvPacket with 1 MB payload (multi-megabyte buffer)
    std::cout << "[Test 4] Testing 1 MB payload transfer...\n";
    std::vector<uint8_t> large_data(1024 * 1024);
    for (size_t i = 0; i < large_data.size(); ++i) {
        large_data[i] = static_cast<uint8_t>(i % 256);
    }
    PacketHeader large_hdr{};
    large_hdr.magic = RGPU_MAGIC;
    large_hdr.opcode = 0x0010;
    large_hdr.sequence_id = 43;
    large_hdr.payload_length = static_cast<uint32_t>(large_data.size());

    assert(client_sock.SendPacket(large_hdr, large_data.data()));

    PacketHeader large_recv_hdr{};
    std::vector<uint8_t> large_recv_payload;
    assert(accepted_sock->RecvPacket(large_recv_hdr, large_recv_payload));

    assert(large_recv_hdr.payload_length == large_data.size());
    assert(large_recv_payload == large_data);
    std::cout << "  1 MB transfer verified with bitwise equality!\n";

    // Test 5: Client disconnect detection on server
    std::cout << "[Test 5] Testing graceful disconnect detection...\n";
    client_sock.Close();
    // Next recv on accepted_sock should return false (EOF)
    PacketHeader eof_hdr{};
    std::vector<uint8_t> eof_payload;
    assert(!accepted_sock->RecvPacket(eof_hdr, eof_payload));
    std::cout << "  Server cleanly detected client disconnect!\n";

    // Test 6: Connect to non-existent port (failure without hang)
    std::cout << "[Test 6] Testing connection failure without hang...\n";
    NetworkSocket bad_sock;
    // Port 1 is typically closed
    bool conn_failed = !bad_sock.Connect("127.0.0.1", 1, 500);
    assert(conn_failed);
    std::cout << "  Connection to closed port failed cleanly within timeout!\n";

    accepted_sock->Close();
    server_sock.Close();
    NetworkSocket::ShutdownNetwork();
    std::cout << "[All Tests Passed Successfully!]\n";
    return 0;
}
