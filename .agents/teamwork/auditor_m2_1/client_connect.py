import socket
import struct
import time
import sys

RGPU_MAGIC = 0x52475055
RGPU_PROTOCOL_VERSION = 1
OP_HANDSHAKE_REQUEST = 0x0001
OP_HANDSHAKE_RESPONSE = 0x0002
OP_SHUTDOWN = 0x0003

PORT = 49215

print(f"[Client] Connecting to 127.0.0.1:{PORT}...")
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.settimeout(5.0)
sock.connect(("127.0.0.1", PORT))
print("[Client] Connected successfully!")

# 1. Send Handshake
req_payload = struct.pack("<5I", RGPU_PROTOCOL_VERSION, 0, 1920, 1080, 28)
hdr = struct.pack("<IHHI I", RGPU_MAGIC, OP_HANDSHAKE_REQUEST, 0, 1, len(req_payload))
sock.sendall(hdr + req_payload)

resp_hdr_bytes = sock.recv(16)
assert len(resp_hdr_bytes) == 16, f"Expected 16 bytes header, got {len(resp_hdr_bytes)}"
magic, opcode, flags, seq, payload_len = struct.unpack("<IHHI I", resp_hdr_bytes)
print(f"[Client] HandshakeResponse Header: magic={hex(magic)}, opcode={hex(opcode)}, len={payload_len}")

resp_payload = b""
while len(resp_payload) < payload_len:
    chunk = sock.recv(payload_len - len(resp_payload))
    if not chunk:
        break
    resp_payload += chunk

proto_ver, status, feat_lvl, w, h, fmt, vram = struct.unpack("<6I Q", resp_payload[:32])
adapter_desc = resp_payload[32:160].split(b"\x00")[0].decode("utf-8")
print(f"[Client] Server returned: adapter='{adapter_desc}', vram={vram // (1024*1024)} MB, status={status}")

# 2. Send Shutdown
print("[Client] Sending OP_SHUTDOWN...")
shut_payload = struct.pack("<I", 0)
shut_hdr = struct.pack("<IHHI I", RGPU_MAGIC, OP_SHUTDOWN, 0, 2, len(shut_payload))
sock.sendall(shut_hdr + shut_payload)

# Give server moment to process before closing
time.sleep(0.2)
sock.close()
print("[Client] Closed connection. Done.")
