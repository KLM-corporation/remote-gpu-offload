# Launcher script for Poppy Playtime Chapter 1 via Remote GPU Offload
$ErrorActionPreference = "Stop"

$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Poppy Playtime\WindowsNoEditor\Poppy_Playtime\Binaries\Win64"
$GameExe = "$GameDir\Poppy_Playtime-Win64-Shipping.exe"
$ProxySrc = "C:\Users\LHRIC\.gemini\antigravity\scratch\remote_gpu_offload\build\client"

if (-not (Test-Path $GameExe)) {
    Write-Host "[Error] Poppy Playtime executable not found at: $GameExe" -ForegroundColor Red
    exit 1
}

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  Launching Poppy Playtime with Remote RTX 4070 Offload" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

# 1. Deploy proxy DLLs next to the game executable
Write-Host "[1/3] Deploying d3d11.dll and dxgi.dll to game directory..." -ForegroundColor Yellow
Copy-Item "$ProxySrc\d3d11.dll" -Destination "$GameDir\d3d11.dll" -Force
Copy-Item "$ProxySrc\dxgi.dll" -Destination "$GameDir\dxgi.dll" -Force
Write-Host "      Done!" -ForegroundColor Green

# 2. Configure RemoteGPU environment variables
Write-Host "[2/3] Configuring RemoteGPU connection to RTX 4070..." -ForegroundColor Yellow
$env:RGPU_SERVER = "192.168.1.17"
$env:RGPU_PORT = "48888"
$env:RGPU_FRAME_LATENCY = "2"
$env:RGPU_COMPRESSION = "1"
Write-Host "      RGPU_SERVER        = $env:RGPU_SERVER" -ForegroundColor White
Write-Host "      RGPU_PORT          = $env:RGPU_PORT" -ForegroundColor White
Write-Host "      RGPU_FRAME_LATENCY = $env:RGPU_FRAME_LATENCY" -ForegroundColor White

# 3. Launch the game in windowed DirectX 11 mode
Write-Host "[3/3] Starting Poppy Playtime (-dx11 -windowed)..." -ForegroundColor Yellow
Start-Process -FilePath $GameExe -ArgumentList "-dx11", "-windowed", "-ResX=1280", "-ResY=720"

Write-Host "============================================================" -ForegroundColor Green
Write-Host "  Game launched! Look at the server console on your RTX 4070!" -ForegroundColor Green
Write-Host "============================================================" -ForegroundColor Green
