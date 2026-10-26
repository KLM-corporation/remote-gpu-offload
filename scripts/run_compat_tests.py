"""Real-application compatibility tests: Microsoft's Direct3D 11 tutorials through the proxy.

Usage: python scripts/run_compat_tests.py [--fetch] [--port 48940] [--seconds 4] [--only 07 ...]

The tutorials (github.com/walbourn/directx-sdk-samples, MIT licence) are third-party programs that
were not written for RemoteGPU, so they exercise the paths real applications take: D3D11CreateDevice
then IDXGIDevice -> GetAdapter -> GetParent(IDXGIFactory2) -> CreateSwapChainForHwnd, shaders
compiled from .fxh files, constant buffers updated with UpdateSubresource, a DDS texture, etc.

--fetch downloads only their Direct3D11Tutorials folder (about 1.5 MB, sparse git clone) into
build/third_party. Each tutorial is built with clang++, staged next to the proxy d3d11.dll and
dxgi.dll, run against a local server for a few seconds, closed with WM_CLOSE, and its last
presented frame is checked with tests/verify_pixels.py. PNG previews go to build/compat/results.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_loopback_test import ROOT, Results, start_server, stop  # noqa: E402

sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tests" / "compat"))
import verify_pixels  # noqa: E402
from run_app import run_app  # noqa: E402

BUILD = ROOT / "build"
REPO = BUILD / "third_party" / "directx-sdk-samples"
TUTORIALS_DIR = REPO / "Direct3D11Tutorials"
OUT = BUILD / "compat"
REPO_URL = "https://github.com/walbourn/directx-sdk-samples.git"

# Tutorial -> (data files to stage, minimum colour clusters in the frame)
TUTORIALS = {
    "02": (["Tutorial02.fxh"], 1),                  # flat yellow triangle
    "04": (["Tutorial04.fxh"], 2),                  # vertex-coloured cube
    "05": (["Tutorial05.fxh"], 2),                  # two animated cubes
    "06": (["Tutorial06.fxh"], 2),                  # lit cube + light markers
    # Textured cube tinted by one clock-driven colour: at dark moments the whole texture falls in
    # a single colour cluster (seen on an RTX 4070 run), so only contrast and coverage are required.
    "07": (["Tutorial07.fxh", "seafloor.dds"], 1),
}


def sources_present():
    return all((TUTORIALS_DIR / f"Tutorial{n}" / f"Tutorial{n}.cpp").exists() for n in TUTORIALS)


def fetch():
    if sources_present():
        return True
    REPO.parent.mkdir(parents=True, exist_ok=True)
    steps = [
        ["git", "clone", "--depth", "1", "--filter=blob:none", "--sparse", REPO_URL, str(REPO)],
        ["git", "-C", str(REPO), "sparse-checkout", "set", "Direct3D11Tutorials"],
    ]
    return all(subprocess.run(cmd).returncode == 0 for cmd in steps) and sources_present()


def build_tutorial(n, stage):
    src = TUTORIALS_DIR / f"Tutorial{n}"
    sources = [str(src / f"Tutorial{n}.cpp")]
    if (src / "DDSTextureLoader.cpp").exists():
        sources.append(str(src / "DDSTextureLoader.cpp"))
    cmd = [shutil.which("clang++"), "-std=c++17", "-O2", "-static", "-municode", "-DUNICODE", "-D_UNICODE",
           "-Wno-nonportable-include-path", f"-I{ROOT / 'tests' / 'compat' / 'shim'}", f"-I{src}", *sources,
           "-o", str(stage / f"Tutorial{n}.exe"), "-mwindows", "-ld3d11", "-ld3dcompiler", "-ldxgi", "-luuid", "-ldxguid"]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stderr[-2000:])
    return result.returncode == 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fetch", action="store_true", help="download the tutorial sources if missing")
    parser.add_argument("--port", type=int, default=48940)
    parser.add_argument("--seconds", type=float, default=4.0, help="how long each tutorial runs")
    parser.add_argument("--only", nargs="+", choices=list(TUTORIALS))
    args = parser.parse_args(argv)

    if args.fetch and not fetch():
        print("[compat] could not download the tutorial sources")
        return 1
    if not sources_present():
        print(f"[compat] tutorial sources missing in {TUTORIALS_DIR} (run with --fetch)")
        return 1
    proxy = [BUILD / "client" / "d3d11.dll", BUILD / "client" / "dxgi.dll"]
    if not all(p.exists() for p in proxy) or not (BUILD / "server.exe").exists():
        print("[compat] missing binaries (run: python scripts/run_loopback_test.py --build)")
        return 1

    results_dir = OUT / "results"
    results_dir.mkdir(parents=True, exist_ok=True)
    r = Results()
    server_log = results_dir / "server.log"
    server, listening, pump = start_server(BUILD / "server.exe", args.port, server_log, single_session=False)
    try:
        if not r.check(listening.wait(60), "server listening"):
            return 1
        for n, (data_files, min_clusters) in TUTORIALS.items():
            if args.only and n not in args.only:
                continue
            print(f"[compat] Tutorial{n}")
            stage = OUT / f"Tutorial{n}"
            stage.mkdir(parents=True, exist_ok=True)
            if not r.check(build_tutorial(n, stage), f"Tutorial{n} builds"):
                continue
            for f in data_files:
                shutil.copy2(TUTORIALS_DIR / f"Tutorial{n}" / f, stage)
            for p in proxy:
                shutil.copy2(p, stage)

            capture = results_dir / f"Tutorial{n}.bmp"
            if capture.exists():
                capture.unlink()
            env = dict(os.environ, RGPU_SERVER="127.0.0.1", RGPU_PORT=str(args.port), RGPU_CAPTURE_FILE=str(capture))
            code, out = run_app(stage / f"Tutorial{n}.exe", stage, args.seconds, env)
            (results_dir / f"Tutorial{n}.log").write_text(out, encoding="utf-8")
            r.check(code == 0, f"Tutorial{n} runs through the proxy and exits cleanly", f"exit {code}")
            r.check("Connected. Remote adapter" in out, f"Tutorial{n} rendered on the remote server")
            if not r.check(capture.exists(), f"Tutorial{n} presented frames"):
                continue
            width, height, pixels = verify_pixels.read_bmp(str(capture))
            m = verify_pixels.analyse(width, height, pixels)
            verify_pixels.write_png(str(results_dir / f"Tutorial{n}.png"), width, height, pixels)
            checks = m["checks"]
            ok = (checks["mean_luminance"] and checks["luminance_stddev"] and checks["non_clear_ratio"]
                  and m["color_clusters"] >= min_clusters)
            r.check(ok, f"Tutorial{n} frame content",
                    f"{width}x{height}, luma {m['mean_luminance']}, non-clear {m['non_clear_ratio'] * 100:.1f}%, "
                    f"clusters {m['color_clusters']} (need {min_clusters})")
    finally:
        stop(server)
        pump.join(5)

    print("=" * 60)
    print(f" COMPAT TESTS: {'PASSED' if r.passed else 'FAILED'}  (frames: {results_dir})")
    print("=" * 60)
    return 0 if r.passed else 1


if __name__ == "__main__":
    sys.exit(main())
