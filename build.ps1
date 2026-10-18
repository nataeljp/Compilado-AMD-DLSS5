# Compila o DLSS5Overlay.exe (MSVC 2022, SDK 10.0.26100 com C++/WinRT). Saida em bin\.
# O Dear ImGui (externo\imgui) e compilado uma vez em obj\imgui; os fontes do projeto, sempre.
$ErrorActionPreference = 'Stop'
$raiz = $PSScriptRoot
$vs = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
New-Item -ItemType Directory -Force "$raiz\bin", "$raiz\obj", "$raiz\obj\imgui" | Out-Null
$flags = '/nologo /std:c++20 /EHsc /O2 /MT /W3 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /permissive- /Zc:__cplusplus /utf-8'
$imgui = 'imgui', 'imgui_draw', 'imgui_tables', 'imgui_widgets', 'imgui_impl_dx12', 'imgui_impl_win32'
$falta = $imgui | Where-Object { -not (Test-Path "$raiz\obj\imgui\$_.obj") }
if ($falta) {
    $f = ($falta | ForEach-Object { "`"$raiz\externo\imgui\$_.cpp`"" }) -join ' '
    cmd /c "`"$vs`" >nul 2>nul & cl $flags /c /Fo`"$raiz\obj\imgui\\`" $f" 2>&1 | Where-Object { $_ -match ' error ' } | Select-Object -First 20
    if ($LASTEXITCODE -ne 0) { "build do imgui falhou"; exit 1 }
}
$fontes = (Get-ChildItem "$raiz\src\*.cpp" | ForEach-Object { '"' + $_.FullName + '"' }) -join ' '
$objsImgui = ($imgui | ForEach-Object { "`"$raiz\obj\imgui\$_.obj`"" }) -join ' '
$libs = 'd3d12.lib d3d11.lib dxgi.lib dcomp.lib d3dcompiler.lib dwmapi.lib windowsapp.lib user32.lib shell32.lib'
$cl = "cl $flags /Fo`"$raiz\obj\\`" /Fe`"$raiz\bin\DLSS5Overlay.exe`" $fontes $objsImgui /link /SUBSYSTEM:WINDOWS $libs"
cmd /c "`"$vs`" >nul 2>nul & $cl" 2>&1 | Tee-Object -FilePath "$raiz\obj\build.log" | Where-Object { $_ -match 'error|warning C4[0-9]{3}' } | Select-Object -First 60
if ($LASTEXITCODE -ne 0) { "build falhou ($LASTEXITCODE)"; exit 1 }
"ok: $raiz\bin\DLSS5Overlay.exe"
