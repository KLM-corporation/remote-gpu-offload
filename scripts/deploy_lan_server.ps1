# Deploys build\server.exe to the LAN GPU server and restarts its scheduled task, then checks
# that the file now on the server is byte-identical to the local build.
#
# Usage (from any folder):
#   powershell -ExecutionPolicy Bypass -File scripts\deploy_lan_server.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\deploy_lan_server.ps1 -Target user@host
param(
    [string]$Target = 'kylian@192.168.1.17',
    [string]$Key = (Join-Path $env:USERPROFILE '.ssh\id_ed25519'),
    [string]$Task = 'RGPU-LAN-Server',
    [string]$RemoteDir = 'rgpu-lan-test'
)

$build = Join-Path $PSScriptRoot '..\build'
$local = Join-Path $build 'server.exe'
if (-not (Test-Path $local)) { Write-Error "No ${local}, build first (python scripts/run_loopback_test.py --build)"; exit 1 }
$sshArgs = @('-i', $Key, '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8')

Write-Host "[deploy] Stopping $Task on $Target (the previous server.exe is kept as server_prev.exe)"
# taskkill /im matches the exact image name only; the ping is a one-second pause (timeout.exe
# refuses to run without a console) so Windows releases the file before it is overwritten.
ssh @sshArgs $Target "schtasks /end /tn $Task >nul 2>&1 & taskkill /f /im server.exe >nul 2>&1 & ping -n 2 127.0.0.1 >nul & copy /y $RemoteDir\server.exe $RemoteDir\server_prev.exe >nul"
if ($LASTEXITCODE -eq 255) { Write-Error "[deploy] SSH connection to $Target failed"; exit 1 }

Write-Host "[deploy] Copying build\server.exe"
Push-Location $build   # a relative source path keeps scp from reading a drive letter as a host name
try { scp @sshArgs 'server.exe' "${Target}:$RemoteDir/server.exe" } finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { Write-Error "[deploy] Copy failed (is server.exe still running on the server?)"; exit 1 }

Write-Host "[deploy] Starting $Task"
ssh @sshArgs $Target "schtasks /run /tn $Task"
if ($LASTEXITCODE -ne 0) { Write-Error "[deploy] Could not start $Task"; exit 1 }

$expected = (Get-FileHash $local -Algorithm SHA256).Hash.ToLower()
$remote = (ssh @sshArgs $Target "certutil -hashfile $RemoteDir\server.exe SHA256") |
    ForEach-Object { ($_ -replace '\s', '').ToLower() } | Where-Object { $_ -match '^[0-9a-f]{64}$' } | Select-Object -First 1
# Exact image name: other programs on that PC end in "server.exe" too (Plex Media Server.exe, ...).
# No inner quotes in the remote command: PowerShell 5.1 drops them when calling ssh.
$running = (ssh @sshArgs $Target "tasklist /nh /fo csv") | Where-Object { $_ -match '^"server\.exe",' }

if ($remote -ne $expected) { Write-Error "[deploy] The server's file ($remote) differs from build\server.exe ($expected)"; exit 1 }
if (-not $running) { Write-Error "[deploy] server.exe is not running after the restart"; exit 1 }
Write-Host "[deploy] OK: the server runs the new build ($($expected.Substring(0, 12))...)"
