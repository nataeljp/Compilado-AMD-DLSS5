# Compila o DLSS5Overlay.exe (MSVC 2022, SDK 10.0.26100 com C++/WinRT). Saida em bin\.
$ErrorActionPreference = 'Stop'
$raiz = $PSScriptRoot
$vs = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
New-Item -ItemType Directory -Force "$raiz\bin", "$raiz\obj" | Out-Null
$fontes = (Get-ChildItem "$raiz\src\*.cpp" | ForEach-Object { '"' + $_.FullName + '"' }) -join ' '
$libs = 'd3d12.lib d3d11.lib dxgi.lib dcomp.lib d3dcompiler.lib dwmapi.lib windowsapp.lib user32.lib shell32.lib'
$cl = "cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /permissive- /Zc:__cplusplus " +
      "/Fo`"$raiz\obj\\`" /Fe`"$raiz\bin\DLSS5Overlay.exe`" $fontes /link /SUBSYSTEM:WINDOWS $libs"
cmd /c "`"$vs`" >nul 2>nul & $cl" 2>&1 | Tee-Object -FilePath "$raiz\obj\build.log" | Where-Object { $_ -match 'error|warning C4[0-9]{3}' } | Select-Object -First 60
if ($LASTEXITCODE -ne 0) { "build falhou ($LASTEXITCODE)"; exit 1 }
"ok: $raiz\bin\DLSS5Overlay.exe"
