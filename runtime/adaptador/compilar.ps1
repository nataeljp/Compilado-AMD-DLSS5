# Adaptador LmxxfNrRuntime.dll: expoe a API de runtime do AMDNR (LmxxfNrApi_amdnr.h) por cima do runtime do lmxxf
# (lmxxf_upstream.dll) e repassa os exports extras (pulos por quadro, passes, mascara por pixel). Precisa: Visual Studio
# 2022 com C++. Saida: runtime\adaptador\LmxxfNrRuntime.dll (o monta_runtime.ps1 a coloca em bin\runtime).
# Uso: powershell -ExecutionPolicy Bypass -File runtime\adaptador\compilar.ps1
$ErrorActionPreference = 'Stop'
$aqui = $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
Push-Location $aqui
cmd /c "`"$vcvars`" >nul 2>nul & rc /nologo /fo adaptador.res adaptador.rc & cl /nologo /std:c++17 /O2 /EHsc /MT /LD /DNOMINMAX adaptador.cpp adaptador.res /Fe:LmxxfNrRuntime.dll /link user32.lib" | Where-Object { $_ -match ' error ' }
Pop-Location
if (Test-Path "$aqui\LmxxfNrRuntime.dll") { "ok: $aqui\LmxxfNrRuntime.dll" } else { exit 1 }
