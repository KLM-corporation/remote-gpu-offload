"""Automated loopback end-to-end test: GPU server + proxied sample app on 127.0.0.1.

Usage: python scripts/run_loopback_test.py [--port 48888] [--frames 60] [--build] [--timeout 120]

Steps:
  1. (optional --build) compile server.exe, build/client/d3d11.dll and build/client/sample_cube.exe
  2. start server.exe --single-session and wait until it listens
  3. run sample_cube.exe from build/client (so it loads the proxy d3d11.dll) for N frames,
     with RGPU_CAPTURE_FILE set so the proxy saves the last presented frame
  4. wait for the server to finish the session and exit
  5. check exit codes, the server frame count, and the captured frame with tests/verify_pixels.py
Exit code 0 only when every check passes.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests"))
import verify_pixels  # noqa: E402

BUILD_COMMANDS = [
    ["-std=c++17", "-O2", "-Isrc/common", "-Isrc/server",
     "src/common/transport.cpp", "src/common/frame_codec.cpp", "src/server/gpu_device.cpp", "src/server/server_dispatcher.cpp",
     "src/server/main_server.cpp", "-o", "{build}/server.exe",
     "-ld3d11", "-ldxgi", "-lws2_32", "-ld3dcompiler"],
    ["-shared", "-static", "-std=c++17", "-O2", "-Isrc/common", "-Isrc/client",
     "src/common/transport.cpp", "src/common/frame_codec.cpp", "src/client/presentation.cpp", "src/client/proxy_device.cpp",
     "src/client/proxy_context.cpp", "src/client/proxy_swapchain.cpp", "src/client/proxy_dxgi.cpp",
     "src/client/d3d11_wrapper.cpp",
     "src/client/d3d11.def", "-o", "{build}/client/d3d11.dll",
     "-lws2_32", "-lgdi32", "-luuid", "-ldxguid"],
    ["-shared", "-static", "-std=c++17", "-O2", "src/client/dxgi_wrapper.cpp", "src/client/dxgi.def",
     "-o", "{build}/client/dxgi.dll"],
    ["-std=c++17", "-O2", "-static", "-Isrc/sample_app", "src/sample_app/main_sample.cpp",
     "-o", "{build}/client/sample_cube.exe", "-ld3d11", "-ld3dcompiler", "-luser32", "-ldxguid"],
]


class Results:
    def __init__(self):
        self.items = []

    def check(self, ok, label, detail=""):
        self.items.append(ok)
        print(f"  [{'PASS' if ok else 'FAIL'}] {label}{(' - ' + detail) if detail else ''}")
        return ok

    @property
    def passed(self):
        return bool(self.items) and all(self.items)


def build(build_dir):
    compiler = shutil.which("clang++")
    if not compiler:
        print("[runner] clang++ not found on PATH")
        return False
    (build_dir / "client").mkdir(parents=True, exist_ok=True)
    for args in BUILD_COMMANDS:
        cmd = [compiler] + [a.format(build=build_dir.as_posix()) for a in args]
        print(f"[runner] building {Path(cmd[cmd.index('-o') + 1]).name}")
        if subprocess.run(cmd, cwd=ROOT).returncode != 0:
            return False
    return True


def start_server(server_exe, port, log_path, single_session=True):
    log = open(log_path, "w", encoding="utf-8", errors="replace")
    cmd = [str(server_exe), "--port", str(port), "--bind", "127.0.0.1"]
    if single_session:
        cmd.append("--single-session")
    proc = subprocess.Popen(cmd,
                            cwd=server_exe.parent, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace")
    listening = threading.Event()

    def pump():
        for line in proc.stdout:
            log.write(line)
            log.flush()
            if "Listening" in line:
                listening.set()
        log.close()

    thread = threading.Thread(target=pump, daemon=True)
    thread.start()
    return proc, listening, thread


def stop(proc):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=48888)
    parser.add_argument("--frames", type=int, default=60)
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=480)
    parser.add_argument("--timeout", type=float, default=120.0, help="seconds allowed for the client run")
    parser.add_argument("--build", action="store_true", help="compile the server, proxy DLL and sample first")
    parser.add_argument("--build-dir", default=str(ROOT / "build"))
    parser.add_argument("--artifacts", default=None, help="output folder (default: <build-dir>/loopback_artifacts)")
    args = parser.parse_args(argv)

    build_dir = Path(args.build_dir).resolve()
    artifacts = Path(args.artifacts).resolve() if args.artifacts else build_dir / "loopback_artifacts"
    artifacts.mkdir(parents=True, exist_ok=True)
    server_exe = build_dir / "server.exe"
    client_dir = build_dir / "client"
    proxy_dll = client_dir / "d3d11.dll"
    sample_exe = client_dir / "sample_cube.exe"
    capture = artifacts / "last_frame.bmp"
    server_log = artifacts / "server.log"
    client_log = artifacts / "client.log"

    print("=" * 60)
    print(f" RemoteGPU loopback test: 127.0.0.1:{args.port}, {args.frames} frames")
    print("=" * 60)
    if args.build and not build(build_dir):
        print("[runner] FAILED: build error")
        return 1

    r = Results()
    print("[runner] preflight")
    for path in (server_exe, proxy_dll, sample_exe):
        if not r.check(path.exists(), f"{path.relative_to(build_dir)} exists"):
            print("[runner] FAILED: missing binaries (run with --build)")
            return 1
    r.check(not (server_exe.parent / "d3d11.dll").exists(), "no proxy d3d11.dll next to server.exe",
            "the server would load the proxy instead of the real driver")
    if capture.exists():
        capture.unlink()

    print("[runner] starting server")
    server, listening, pump = start_server(server_exe, args.port, server_log)
    try:
        if not r.check(listening.wait(60) and server.poll() is None, "server listening"):
            return 1

        print("[runner] running proxied sample client")
        env = dict(os.environ, RGPU_SERVER="127.0.0.1", RGPU_PORT=str(args.port), RGPU_CAPTURE_FILE=str(capture))
        t0 = time.monotonic()
        try:
            client = subprocess.run([str(sample_exe), "--frames", str(args.frames), "--width", str(args.width),
                                     "--height", str(args.height)], cwd=client_dir, env=env,
                                    capture_output=True, text=True, timeout=args.timeout)
        except subprocess.TimeoutExpired:
            r.check(False, "client finished in time", f"> {args.timeout:.0f} s")
            return 1
        elapsed = time.monotonic() - t0
        client_log.write_text(client.stdout + client.stderr, encoding="utf-8")
        print("    " + "\n    ".join((client.stdout + client.stderr).strip().splitlines()))
        r.check(client.returncode == 0, "client exit code 0", f"got {client.returncode}")
        m = re.search(r"Presented (\d+) frames .*\(([\d.]+) FPS\)", client.stdout)
        r.check(bool(m) and int(m.group(1)) >= args.frames, f"client presented >= {args.frames} frames",
                f"{m.group(1)} frames at {m.group(2)} FPS" if m else "no frame report")
        r.check("Remote adapter" in client.stderr, "client rendered through the remote proxy")

        try:
            server.wait(15)
        except subprocess.TimeoutExpired:
            pass
        r.check(server.poll() is not None and server.returncode == 0, "server exited cleanly (code 0)",
                f"code {server.poll()}")
    finally:
        stop(server)
        pump.join(5)

    log = server_log.read_text(encoding="utf-8", errors="replace")
    adapter = re.search(r"Active GPU Adapter\s*:\s*(.+)", log)
    if adapter:
        print(f"[runner] server GPU: {adapter.group(1).strip()}")
    frames = re.search(r"Total frames presented: (\d+)", log)
    r.check(bool(frames) and int(frames.group(1)) >= args.frames, "server rendered all frames",
            frames.group(0) if frames else "no frame count in server log")
    r.check("Status: Normal" in log, "session ended normally on the server")

    print("[runner] verifying captured frame")
    r.check(capture.exists(), "frame captured", str(capture))
    if capture.exists():
        rc = verify_pixels.main([str(capture), "--png", str(artifacts / "last_frame.png"),
                                 "--json", str(artifacts / "pixel_metrics.json")])
        r.check(rc == 0, "pixel verification")

    print("=" * 60)
    print(f" RESULT: {'PASSED' if r.passed else 'FAILED'}  (artifacts: {artifacts})")
    print("=" * 60)
    return 0 if r.passed else 1


if __name__ == "__main__":
    sys.exit(main())
