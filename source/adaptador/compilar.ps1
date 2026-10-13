# Adaptador LmxxfNrRuntime.dll: expõe a API de runtime que o código do AMDNR espera (LmxxfNrApi_amdnr.h) por cima do
# runtime upstream do lmxxf (lmxxf_upstream.dll). Precisa: Visual Studio 2022 com C++.
# Uso: powershell -ExecutionPolicy Bypass -File compilar.ps1
$ErrorActionPreference = 'Stop'
$aqui = Split-Path -Parent $MyInvocation.MyCommand.Path
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
Push-Location $aqui
cmd /c "`"$vcvars`" >nul 2>nul & rc /nologo /fo adaptador.res adaptador.rc & cl /nologo /std:c++17 /O2 /EHsc /MT /LD /DNOMINMAX adaptador.cpp adaptador.res /Fe:LmxxfNrRuntime.dll user32.lib"
Pop-Location
if (Test-Path "$aqui\LmxxfNrRuntime.dll") { "ok: $aqui\LmxxfNrRuntime.dll" } else { exit 1 }
