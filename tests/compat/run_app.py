"""Runs a windowed D3D11 application for a few seconds, then closes its window(s) with WM_CLOSE.

Usage (module): run_app(exe, cwd, seconds, env) -> (exit_code or None if it had to be killed, output)
The Direct3D 11 tutorials render until their window is closed, so the harness closes it.
"""
import ctypes
import ctypes.wintypes as wt
import subprocess
import time

user32 = ctypes.windll.user32
WM_CLOSE = 0x0010
EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def windows_of(pid):
    found = []

    def callback(hwnd, _):
        owner = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(EnumWindowsProc(callback), 0)
    return found


def run_app(exe, cwd, seconds, env=None, close_timeout=20):
    proc = subprocess.Popen([str(exe)], cwd=str(cwd), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace")
    end = time.monotonic() + seconds
    while time.monotonic() < end and proc.poll() is None:
        time.sleep(0.1)
    if proc.poll() is None:
        for hwnd in windows_of(proc.pid):
            user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        try:
            proc.wait(close_timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            out = proc.communicate()[0]
            return None, out
    return proc.returncode, proc.communicate()[0]
