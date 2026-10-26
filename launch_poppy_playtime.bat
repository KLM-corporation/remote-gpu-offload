@echo off
setlocal

set "GAMEDIR=C:\Program Files (x86)\Steam\steamapps\common\Poppy Playtime\WindowsNoEditor\Poppy_Playtime\Binaries\Win64"
set "PROXYDIR=C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\build\client"

echo ============================================================
echo   Launching Poppy Playtime with Remote RTX 4070 Offload
echo ============================================================

echo [1/3] Copying d3d11.dll and dxgi.dll to Poppy Playtime...
copy /Y "%PROXYDIR%\d3d11.dll" "%GAMEDIR%\d3d11.dll"
copy /Y "%PROXYDIR%\dxgi.dll" "%GAMEDIR%\dxgi.dll"

echo [2/3] Configuring Remote GPU environment variables...
set "RGPU_SERVER=192.168.1.17"
set "RGPU_PORT=48888"
set "RGPU_FRAME_LATENCY=2"
set "RGPU_COMPRESSION=1"

echo [3/3] Starting Poppy_Playtime-Win64-Shipping.exe (-dx11 -windowed)...
cd /D "%GAMEDIR%"
start "" "Poppy_Playtime-Win64-Shipping.exe" -dx11 -windowed -ResX=1280 -ResY=720

echo ============================================================
echo   Game started! Watch the console on your RTX 4070 PC!
echo ============================================================
