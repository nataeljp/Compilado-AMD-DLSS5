# Compila o TesteFgMl.exe (MSVC 2022): a geracao de quadros da AMD pela API do FidelityFX, fora de jogo.
# uso: compila.ps1 [-Saida pasta]  (padrao: obj\TesteFgMl.exe)
param([string]$Saida = '')
$ErrorActionPreference = 'Stop'
$raiz = Split-Path (Split-Path $PSScriptRoot)
if (-not $Saida) { $Saida = "$raiz\obj" }
New-Item -ItemType Directory -Force $Saida, "$raiz\obj\teste_fg_ml" | Out-Null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = Join-Path (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) 'VC\Auxiliary\Build\vcvars64.bat'
$inc = "/I`"$raiz\externo\fidelityfx_sdk\ffx-api\include`""
$cl = "cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /utf-8 $inc /Fo`"$raiz\obj\teste_fg_ml\\`" /Fe`"$Saida\TesteFgMl.exe`" `"$PSScriptRoot\TesteFgMl.cpp`""
cmd /c "`"$vs`" >nul 2>nul & $cl" 2>&1 | Where-Object { $_ -match 'error|warning C4[0-9]{3}' } | Select-Object -First 30
if ($LASTEXITCODE -ne 0) { "TesteFgMl falhou ($LASTEXITCODE)"; exit 1 }
"ok: $Saida\TesteFgMl.exe"
