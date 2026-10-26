# Uninstalls proxy DLLs from Poppy Playtime
$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Poppy Playtime\WindowsNoEditor\Poppy_Playtime\Binaries\Win64"

Remove-Item "$GameDir\d3d11.dll" -ErrorAction SilentlyContinue -Force
Remove-Item "$GameDir\dxgi.dll" -ErrorAction SilentlyContinue -Force

Write-Host "Proxy DLLs removed from Poppy Playtime. Game restored to vanilla." -ForegroundColor Green
