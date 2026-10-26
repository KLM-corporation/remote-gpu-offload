import socket
import struct
import subprocess
import time
import sys

RGPU_MAGIC = 0x52475055
RGPU_PROTOCOL_VERSION = 1
OP_HANDSHAKE_REQUEST = 0x0001
OP_HANDSHAKE_RESPONSE = 0x0002
OP_SHUTDOWN = 0x0003

PORT = 49210

print(f"[Auditor Test] Launching server_auditor.exe on port {PORT} with --single-session...")
server_proc = subprocess.Popen(
    ["build/server_auditor.exe", "--port", str(PORT), "--single-session"],
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True
)

try:
    time.sleep(0.5)
    print(f"[Auditor Test] Connecting to 127.0.0.1:{PORT}...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5.0)
    sock.connect(("127.0.0.1", PORT))
    print("[Auditor Test] Connected! Constructing OP_HANDSHAKE_REQUEST packet...")

    # HandshakeRequestPayload:
    # uint32_t protocol_version;
    # uint32_t client_flags;
    # uint32_t client_width;
    # uint32_t client_height;
    # uint32_t preferred_format;
    req_payload = struct.pack("<5I", RGPU_PROTOCOL_VERSION, 0, 1920, 1080, 28)

    # PacketHeader:
    # uint32_t magic;
    # uint16_t opcode;
    # uint16_t flags;
    # uint32_t sequence_id;
    # uint32_t payload_length;
    hdr = struct.pack("<IHHI I", RGPU_MAGIC, OP_HANDSHAKE_REQUEST, 0, 1, len(req_payload))
    sock.sendall(hdr + req_payload)

    # Recv response header (16 bytes)
    resp_hdr_bytes = sock.recv(16)
    assert len(resp_hdr_bytes) == 16, f"Expected 16 bytes header, got {len(resp_hdr_bytes)}"
    magic, opcode, flags, seq, payload_len = struct.unpack("<IHHI I", resp_hdr_bytes)
    assert magic == RGPU_MAGIC, f"Invalid magic: {hex(magic)}"
    assert opcode == OP_HANDSHAKE_RESPONSE, f"Invalid opcode: {hex(opcode)}"
    print(f"[Auditor Test] Received HandshakeResponse header: seq={seq}, payload_len={payload_len}")

    # Recv response payload
    # HandshakeResponsePayload:
    # uint32_t protocol_version;
    # uint32_t status_code;
    # uint32_t feature_level;
    # uint32_t frame_width;
    # uint32_t frame_height;
    # uint32_t frame_format;
    # uint64_t dedicated_vram;
    # char adapter_desc[128];
    resp_payload = b""
    while len(resp_payload) < payload_len:
        chunk = sock.recv(payload_len - len(resp_payload))
        if not chunk:
            break
        resp_payload += chunk

    assert len(resp_payload) == payload_len, f"Incomplete payload: {len(resp_payload)} / {payload_len}"
    proto_ver, status, feat_lvl, w, h, fmt, vram = struct.unpack("<6I Q", resp_payload[:32])
    adapter_desc_bytes = resp_payload[32:160]
    adapter_desc = adapter_desc_bytes.split(b"\x00")[0].decode("utf-8")

    print(f"[Auditor Test] Handshake Verification Results:")
    print(f"  - Protocol Version : {proto_ver}")
    print(f"  - Status Code      : {status}")
    print(f"  - Feature Level    : {hex(feat_lvl)}")
    print(f"  - Offscreen Targets: {w}x{h} (Format {fmt})")
    print(f"  - Dedicated VRAM   : {vram // (1024*1024)} MB")
    print(f"  - Adapter Desc     : {adapter_desc}")

    assert proto_ver == RGPU_PROTOCOL_VERSION
    assert status == 0
    assert w == 1920
    assert h == 1080
    assert "Intel" in adapter_desc or "GeForce" in adapter_desc or "Radeon" in adapter_desc or len(adapter_desc) > 0

    # Send OP_SHUTDOWN
    print("[Auditor Test] Sending OP_SHUTDOWN to terminate session...")
    shut_payload = struct.pack("<I", 0) # exit_code = 0
    shut_hdr = struct.pack("<IHHI I", RGPU_MAGIC, OP_SHUTDOWN, 0, 2, len(shut_payload))
    sock.sendall(shut_hdr + shut_payload)
    sock.close()

    stdout, _ = server_proc.communicate(timeout=5)
    print("[Auditor Test] Server stdout:")
    print(stdout)
    assert server_proc.returncode == 0, f"Server exit code: {server_proc.returncode}"
    print("[Auditor Test] STANDALONE SERVER VERIFICATION SUCCESSFUL (EXIT CODE 0)!")

except Exception as e:
    print(f"[Auditor Test] ERROR: {e}")
    server_proc.kill()
    sys.exit(1)
