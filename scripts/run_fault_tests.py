"""Network fault-tolerance and leak tests for the server + proxied sample app on 127.0.0.1.

Usage: python scripts/run_fault_tests.py [--port 48900] [--only NAME ...]

Scenarios (each must neither crash nor hang either process):
  no_server       client started with nothing listening -> exits with code 1
  server_killed   server killed while the client renders -> client exits with code 3 (Present failed)
  client_killed   client killed while rendering -> server ends that session, stays up,
                  and serves a following normal client
  idle_client     client idles 15 s between frames -> session survives, client exits 0
  session_leak    6 sequential 60-frame sessions on one server -> server private memory stays flat
Binaries must already exist (python scripts/run_loopback_test.py --build builds them).
"""
import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_loopback_test import ROOT, Results, start_server, stop  # noqa: E402

BUILD = ROOT / "build"
SERVER = BUILD / "server.exe"
CLIENT_DIR = BUILD / "client"
SAMPLE = CLIENT_DIR / "sample_cube.exe"
ARTIFACTS = BUILD / "fault_artifacts"
WINDOWS_CRASH_MIN = 0xC0000000  # NTSTATUS error codes (access violation, stack overflow, ...)


def client_cmd(*extra):
    return [str(SAMPLE), "--width", "320", "--height", "240", *extra]


def client_env(port):
    return dict(os.environ, RGPU_SERVER="127.0.0.1", RGPU_PORT=str(port))


def is_crash(code):
    return code is not None and (code & 0xFFFFFFFF) >= WINDOWS_CRASH_MIN


def wait_for(predicate, timeout, interval=0.1):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return True
        time.sleep(interval)
    return predicate()


def log_text(path):
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def private_bytes(pid):
    out = subprocess.run(["powershell", "-NoProfile", "-Command", f"(Get-Process -Id {pid}).PrivateMemorySize64"],
                         capture_output=True, text=True)
    try:
        return int(out.stdout.strip())
    except ValueError:
        return None


def launch_server(port, name, single_session):
    log = ARTIFACTS / f"{name}_server.log"
    proc, listening, pump = start_server(SERVER, port, log, single_session=single_session)
    if not listening.wait(60):
        stop(proc)
        raise RuntimeError("server did not start listening")
    return proc, pump, log


# =============================================================================
# Scenarios
# =============================================================================

def scenario_no_server(r, port):
    t0 = time.monotonic()
    try:
        c = subprocess.run(client_cmd("--frames", "10"), cwd=CLIENT_DIR, env=client_env(port),
                           capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        r.check(False, "client gives up when no server listens", "hung for > 30 s")
        return
    r.check(c.returncode == 1, "client exits with code 1 when no server listens",
            f"code {c.returncode} after {time.monotonic() - t0:.1f} s")


def scenario_server_killed(r, port):
    server, pump, log = launch_server(port, "server_killed", single_session=True)
    client = subprocess.Popen(client_cmd("--frames", "0"), cwd=CLIENT_DIR, env=client_env(port),
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        started = wait_for(lambda: "Session #1 started" in log_text(log), 20)
        r.check(started, "session established before the fault")
        time.sleep(2.0)
        r.check(client.poll() is None, "client still rendering before the fault")
        server.kill()
        t0 = time.monotonic()
        try:
            out, _ = client.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            r.check(False, "client exits after the server dies", "hung for > 30 s")
            return
        (ARTIFACTS / "server_killed_client.log").write_text(out, encoding="utf-8")
        r.check(not is_crash(client.returncode), "client does not crash when the server dies",
                f"code 0x{client.returncode & 0xFFFFFFFF:08X}")
        r.check(client.returncode == 3, "client reports Present failure (exit code 3)",
                f"code {client.returncode} after {time.monotonic() - t0:.1f} s")
    finally:
        stop(client)
        stop(server)
        pump.join(5)


def scenario_client_killed(r, port):
    server, pump, log = launch_server(port, "client_killed", single_session=False)
    try:
        client = subprocess.Popen(client_cmd("--frames", "0"), cwd=CLIENT_DIR, env=client_env(port),
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        r.check(wait_for(lambda: "Session #1 started" in log_text(log), 20), "session established before the fault")
        time.sleep(2.0)
        client.kill()
        client.wait(10)
        ended = wait_for(lambda: "Session #1 finished" in log_text(log), 30)
        r.check(ended, "server detects the dropped client and ends the session")
        r.check("Session #1 finished (Status: Disconnected/Aborted)" in log_text(log),
                "server reports the session as aborted, not normal")
        r.check(server.poll() is None, "server keeps running after the client dies",
                "" if server.poll() is None else f"exited with code {server.returncode}")
        if server.poll() is not None:
            return
        c = subprocess.run(client_cmd("--frames", "30"), cwd=CLIENT_DIR, env=client_env(port),
                           capture_output=True, text=True, timeout=60)
        r.check(c.returncode == 0, "next client renders normally on the same server", f"code {c.returncode}")
        r.check(wait_for(lambda: "Session #2 finished (Status: Normal)" in log_text(log), 15),
                "second session ends normally")
    finally:
        stop(server)
        pump.join(5)


def scenario_idle_client(r, port):
    server, pump, log = launch_server(port, "idle_client", single_session=True)
    try:
        try:
            c = subprocess.run(client_cmd("--frames", "30", "--stall-ms", "15000"), cwd=CLIENT_DIR,
                               env=client_env(port), capture_output=True, text=True, timeout=90)
        except subprocess.TimeoutExpired:
            r.check(False, "idle client finishes", "hung for > 90 s")
            return
        (ARTIFACTS / "idle_client_client.log").write_text(c.stdout + c.stderr, encoding="utf-8")
        r.check(c.returncode == 0, "session survives a 15 s idle period", f"client code {c.returncode}")
        server.wait(15)
        r.check("Status: Normal" in log_text(log), "server reports a normal session end")
    finally:
        stop(server)
        pump.join(5)


def scenario_session_leak(r, port, sessions=6, frames=60, max_growth_mb=8.0):
    server, pump, log = launch_server(port, "session_leak", single_session=False)
    try:
        samples = []
        for i in range(sessions):
            c = subprocess.run(client_cmd("--frames", str(frames)), cwd=CLIENT_DIR, env=client_env(port),
                               capture_output=True, text=True, timeout=60)
            if c.returncode != 0:
                r.check(False, f"session {i + 1} renders", f"client code {c.returncode}")
                return
            wait_for(lambda n=i + 1: f"Session #{n} finished" in log_text(log), 15)
            samples.append(private_bytes(server.pid))
        r.check(all(s is not None for s in samples), f"{sessions} sequential sessions completed")
        if all(s is not None for s in samples):
            # Session 1 includes one-time driver/shader-cache warm-up, so compare against session 2.
            growth = (samples[-1] - samples[1]) / (1024 * 1024)
            trend = ", ".join(f"{s / (1024 * 1024):.1f}" for s in samples)
            r.check(growth <= max_growth_mb, f"server private memory flat across sessions (<= {max_growth_mb} MB)",
                    f"growth {growth:+.2f} MB; per-session MB: {trend}")
        frames_logged = [int(x) for x in re.findall(r"Total frames presented: (\d+)", log_text(log))]
        r.check(frames_logged == [frames] * sessions, "every session rendered all its frames", str(frames_logged))
    finally:
        stop(server)
        pump.join(5)


SCENARIOS = {
    "no_server": scenario_no_server,
    "server_killed": scenario_server_killed,
    "client_killed": scenario_client_killed,
    "idle_client": scenario_idle_client,
    "session_leak": scenario_session_leak,
}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=48900, help="first port; each scenario uses its own")
    parser.add_argument("--only", nargs="+", choices=list(SCENARIOS), help="run only these scenarios")
    args = parser.parse_args(argv)

    for path in (SERVER, CLIENT_DIR / "d3d11.dll", SAMPLE):
        if not path.exists():
            print(f"[faults] missing {path} (run: python scripts/run_loopback_test.py --build)")
            return 1
    ARTIFACTS.mkdir(parents=True, exist_ok=True)

    r = Results()
    for offset, (name, scenario) in enumerate(SCENARIOS.items()):
        if args.only and name not in args.only:
            continue
        print(f"[faults] scenario: {name}")
        try:
            scenario(r, args.port + offset)
        except Exception as exc:  # a broken scenario is a failure, not a crash of the runner
            r.check(False, f"{name} ran without harness error", repr(exc))

    print("=" * 60)
    print(f" FAULT TESTS: {'PASSED' if r.passed else 'FAILED'}  (artifacts: {ARTIFACTS})")
    print("=" * 60)
    return 0 if r.passed else 1


if __name__ == "__main__":
    sys.exit(main())
