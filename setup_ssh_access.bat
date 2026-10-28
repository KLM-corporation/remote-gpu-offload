@echo off
setlocal EnableExtensions

set "TARGET=192.168.1.17"
set "SSH_USER=kylian"
set "KEY=%USERPROFILE%\.ssh\id_ed25519"
set "PUBKEY=%KEY%.pub"

where ssh-keygen >nul 2>&1
if errorlevel 1 (
    echo ERROR: ssh-keygen is not available in PATH.
    exit /b 1
)

if not exist "%PUBKEY%" (
    echo Generating an Ed25519 SSH key...
    if not exist "%USERPROFILE%\.ssh" mkdir "%USERPROFILE%\.ssh"
    ssh-keygen -t ed25519 -N "" -f "%KEY%"
    if errorlevel 1 (
        echo ERROR: key generation failed.
        exit /b 1
    )
) else (
    echo Existing public key found: "%PUBKEY%"
)

echo.
echo Enter the Windows credentials that already have WinRM access to %TARGET%.
echo The password is used only by PowerShell and is not saved.
echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$target='%TARGET%'; $user='%SSH_USER%'; $pub=(Get-Content -Raw -LiteralPath ($env:USERPROFILE+'\.ssh\id_ed25519.pub')).Trim(); $cred=Get-Credential -UserName $user -Message ('Credentials for WinRM on '+$target); Invoke-Command -ComputerName $target -Credential $cred -ScriptBlock { param($key,$account); $local=Get-LocalUser -Name $account -ErrorAction Stop; $profile=$local | Select-Object -ExpandProperty SID | ForEach-Object { (Get-CimInstance Win32_UserProfile | Where-Object SID -eq $_.Value).LocalPath }; if (-not $profile) { $profile='C:\Users\'+$account }; $isAdmin=([Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator); if ($isAdmin) { $auth='C:\ProgramData\ssh\administrators_authorized_keys'; $dir='C:\ProgramData\ssh'; New-Item -ItemType Directory -Force -Path $dir | Out-Null; Set-Content -LiteralPath $auth -Value $key -Encoding ascii; icacls $auth /inheritance:r /grant:r '*S-1-5-32-544:F' '*S-1-5-18:F' | Out-Null } else { $dir=Join-Path $profile '.ssh'; $auth=Join-Path $dir 'authorized_keys'; New-Item -ItemType Directory -Force -Path $dir | Out-Null; Set-Content -LiteralPath $auth -Value $key -Encoding ascii; icacls $dir /inheritance:r /grant:r ($account+':(OI)(CI)F') | Out-Null; icacls $auth /inheritance:r /grant:r ($account+':F') | Out-Null }; Restart-Service sshd; Write-Host ('Installed key at '+$auth); Get-Service sshd | Format-Table Status,Name } -ArgumentList $pub,$user"
if errorlevel 1 (
    echo.
    echo ERROR: remote key installation failed.
    exit /b 1
)

echo.
echo Testing SSH key authentication...
ssh -o BatchMode=yes -o IdentitiesOnly=yes -o IdentityFile="%KEY%" -o ConnectTimeout=8 "%SSH_USER%@%TARGET%" "hostname"
if errorlevel 1 (
    echo.
    echo SSH authentication test failed. Run: ssh -vvv -o IdentitiesOnly=yes -i "%KEY%" %SSH_USER%@%TARGET% hostname
    exit /b 1
)

echo.
echo SSH access is ready for %SSH_USER%@%TARGET%.
endlocal
