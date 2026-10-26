"""M5 final gate: builds everything and runs the complete validation suite.

Usage: python scripts/run_final_gate.py [--port 48920] [--skip-build]

Stages (all must pass for exit code 0):
  build            server, proxy d3d11.dll, sample app and test executables
  test_frame_codec frame compression codec unit tests
  test_transport   transport unit tests
  test_server      headless GPU / dispatcher unit tests
  test_server_fuzz dispatcher fuzzing
  client_proxy     proxy DLL against a live server (tests/test_client_proxy.cpp)
  dxgi_proxy       dxgi.dll factory/adapter path, ResizeBuffers (tests/test_dxgi_proxy.cpp)
  render_states    blend / depth-stencil / rasterizer / scissor forwarding, checked on pixels
  textures         mip chains, texture arrays, BC formats, array views, mip UpdateSubresource
  pipeline_features instancing, MRT, depth-only binding, VS resources, buffer SRVs
  compute          compute shaders, UAVs (texture + structured buffer), UAV clears
  copies           CopyResource, CopySubresourceRegion (texture, mip, buffer), pixel shader UAVs
  shader_stages    geometry shader + tessellation (hull/domain) with their CBs, SRVs, samplers
  real_app_compat  Microsoft D3D11 tutorials through the proxy (skipped if not fetched)
  loopback_e2e     sample app through the proxy for 60 frames + pixel verification
  fault_tolerance  disconnects, idle session and multi-session leak check
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_fault_tests  # noqa: E402
import run_compat_tests  # noqa: E402
import run_loopback_test  # noqa: E402
from run_loopback_test import ROOT, start_server, stop  # noqa: E402

BUILD = ROOT / "build"
GATE = BUILD / "gate"

SERVER_SRC = ["src/common/transport.cpp", "src/common/frame_codec.cpp", "src/server/gpu_device.cpp", "src/server/server_dispatcher.cpp"]
SERVER_LIBS = ["-ld3d11", "-ldxgi", "-lws2_32", "-ld3dcompiler"]
TEST_BUILDS = {
    "test_frame_codec": ["-Isrc/common", "src/common/frame_codec.cpp", "tests/test_frame_codec.cpp"],
    "test_transport": ["-Isrc/common", "src/common/transport.cpp", "tests/test_transport.cpp", "-lws2_32"],
    "test_server": ["-Isrc/common", "-Isrc/server", *SERVER_SRC, "tests/test_server.cpp", *SERVER_LIBS],
    "test_server_fuzz": ["-Isrc/common", "-Isrc/server", *SERVER_SRC, "tests/test_server_fuzz.cpp", *SERVER_LIBS],
    "test_client_proxy": ["tests/test_client_proxy.cpp", "-ld3dcompiler", "-luser32", "-lgdi32", "-ldxguid"],
    "test_dxgi_proxy": ["tests/test_dxgi_proxy.cpp", "-luser32", "-ldxguid", "-luuid"],
    "test_render_states": ["tests/test_render_states.cpp", "-ld3dcompiler", "-luser32", "-ldxguid"],
    "test_textures": ["tests/test_textures.cpp", "-ld3dcompiler", "-luser32", "-ldxguid"],
    "test_pipeline_features": ["tests/test_pipeline_features.cpp", "-ld3dcompiler", "-luser32", "-ldxguid"],
    "test_compute": ["tests/test_compute.cpp", "-ld3dcompiler", "-luser32", "-ldxguid"],
    "test_copies": ["tests/test_copies.cpp", "-ld3dcompiler", "-luser32", "-ldxguid"],
    "test_shader_stages": ["tests/test_shader_stages.cpp", "-ld3dcompiler", "-luser32", "-ldxguid"],
}


def run_exe(name, args=(), timeout=600, env=None):
    log = GATE / f"{name}.log"
    t0 = time.monotonic()
    try:
        p = subprocess.run([str(GATE / f"{name}.exe"), *args], cwd=ROOT, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=timeout, env=env)
        log.write_text(p.stdout + p.stderr, encoding="utf-8")
        return p.returncode == 0, f"exit {p.returncode} in {time.monotonic() - t0:.1f} s"
    except subprocess.TimeoutExpired:
        return False, f"timed out after {timeout} s"


def stage_build():
    if not run_loopback_test.build(BUILD):
        return False, "product build failed"
    compiler = shutil.which("clang++")
    for name, args in TEST_BUILDS.items():
        cmd = [compiler, "-std=c++17", "-O2", *args, "-o", str(GATE / f"{name}.exe")]
        if subprocess.run(cmd, cwd=ROOT, capture_output=True).returncode != 0:
            return False, f"{name} failed to compile"
    return True, "server, proxy DLL, sample and 12 test executables"


def stage_against_server(test, args, port, single_session=True):
    """Runs a client-side test executable against a dedicated server."""
    server, listening, pump = start_server(BUILD / "server.exe", port, GATE / f"{test}_server.log",
                                           single_session=single_session)
    try:
        if not listening.wait(60):
            return False, "server did not start"
        env = dict(os.environ, RGPU_PORT=str(port))
        return run_exe(test, args, timeout=120, env=env)
    finally:
        stop(server)
        pump.join(5)


def stage_compat(port):
    if not run_compat_tests.sources_present():
        return True, "SKIPPED: tutorial sources absent (python scripts/run_compat_tests.py --fetch)"
    return run_compat_tests.main(["--port", str(port)]) == 0, "see build/compat/results"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=48920)
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args(argv)
    GATE.mkdir(parents=True, exist_ok=True)

    stages = []
    if not args.skip_build:
        stages.append(("build", stage_build))
    stages += [
        ("test_frame_codec", lambda: run_exe("test_frame_codec")),
        ("test_transport", lambda: run_exe("test_transport")),
        ("test_server", lambda: run_exe("test_server")),
        ("test_server_fuzz", lambda: run_exe("test_server_fuzz")),
        ("client_proxy", lambda: stage_against_server(
            "test_client_proxy", [str(BUILD / "client" / "d3d11.dll"), "60"], args.port)),
        # The factory probes the server once before the device connects: two sessions.
        ("dxgi_proxy", lambda: stage_against_server("test_dxgi_proxy", [str(BUILD / "client")], args.port + 2,
                                                    single_session=False)),
        # One session per scenario.
        ("render_states", lambda: stage_against_server("test_render_states", [str(BUILD / "client" / "d3d11.dll")],
                                                       args.port + 4, single_session=False)),
        ("textures", lambda: stage_against_server("test_textures", [str(BUILD / "client" / "d3d11.dll")], args.port + 5)),
        ("pipeline_features", lambda: stage_against_server("test_pipeline_features",
                                                           [str(BUILD / "client" / "d3d11.dll")], args.port + 6)),
        ("compute", lambda: stage_against_server("test_compute", [str(BUILD / "client" / "d3d11.dll")], args.port + 7)),
        ("copies", lambda: stage_against_server("test_copies", [str(BUILD / "client" / "d3d11.dll")], args.port + 8)),
        ("shader_stages", lambda: stage_against_server("test_shader_stages", [str(BUILD / "client" / "d3d11.dll")], args.port + 9)),
        ("real_app_compat", lambda: stage_compat(args.port + 3)),
        ("loopback_e2e", lambda: (run_loopback_test.main(["--port", str(args.port + 1), "--frames", "60"]) == 0,
                                  "see build/loopback_artifacts")),
        ("fault_tolerance", lambda: (run_fault_tests.main(["--port", str(args.port + 10)]) == 0,
                                     "see build/fault_artifacts")),
    ]

    results = []
    for name, stage in stages:
        print(f"\n######## {name} ########", flush=True)
        try:
            ok, detail = stage()
        except Exception as exc:
            ok, detail = False, repr(exc)
        results.append((name, ok, detail))
        print(f"######## {name}: {'PASS' if ok else 'FAIL'} ({detail})", flush=True)
        if name == "build" and not ok:
            break

    print("\n" + "=" * 60)
    print(" M5 FINAL GATE")
    print("=" * 60)
    for name, ok, detail in results:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name:<17} {detail}")
    passed = len(results) == len(stages) and all(ok for _, ok, _ in results)
    print("=" * 60)
    print(f" GATE: {'PASSED' if passed else 'FAILED'}  (logs: {GATE})")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
