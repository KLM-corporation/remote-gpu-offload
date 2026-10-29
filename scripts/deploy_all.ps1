# Deploys a fresh build for the Minecraft LAN setup in one go: the proxy opengl32.dll into the
# Minecraft folders of this PC, then build\server.exe onto the GPU server (deploy_lan_server.ps1).
#
# Usage: powershell -ExecutionPolicy Bypass -File scripts\deploy_all.ps1
param(
    [string[]]$ClientDirs = @(
        "$env:USERPROFILE\curseforge\minecraft\Instances\test",
        "$env:USERPROFILE\curseforge\minecraft\Install\java\java-runtime-epsilon\bin",
        "$env:USERPROFILE\curseforge\minecraft\Install\java\java-runtime-gamma\bin",
        "$env:USERPROFILE\curseforge\minecraft\Install\java\java-runtime-delta\bin",
        "$env:USERPROFILE\curseforge\minecraft\Install\java\Jre_21\bin"
    )
)

$dll = Join-Path $PSScriptRoot '..\build\client\opengl32.dll'
if (-not (Test-Path $dll)) { Write-Error "No build\client\opengl32.dll, build first (python scripts/run_loopback_test.py --build)"; exit 1 }
if (Get-Process javaw -ErrorAction SilentlyContinue) {
    Write-Error "[deploy] Minecraft (javaw.exe) is running and holds opengl32.dll: close it, then run this again"
    exit 1
}

$hash = (Get-FileHash $dll).Hash
foreach ($dir in $ClientDirs) {
    if (-not (Test-Path $dir)) { Write-Host "[deploy] Skipping missing folder $dir"; continue }
    $target = Join-Path $dir 'opengl32.dll'
    Copy-Item $dll $target -Force
    if ((Get-FileHash $target).Hash -ne $hash) { Write-Error "[deploy] Copy to $dir does not match the build"; exit 1 }
    Write-Host "[deploy] opengl32.dll -> $dir"
}

& (Join-Path $PSScriptRoot 'deploy_lan_server.ps1')
exit $LASTEXITCODE
