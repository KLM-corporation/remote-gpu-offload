@echo off
set "GAMEDIR=C:\Program Files (x86)\Steam\steamapps\common\Poppy Playtime\WindowsNoEditor\Poppy_Playtime\Binaries\Win64"
del /F /Q "%GAMEDIR%\d3d11.dll" 2>nul
del /F /Q "%GAMEDIR%\dxgi.dll" 2>nul
echo Proxy DLLs removed. Poppy Playtime restored to normal.
