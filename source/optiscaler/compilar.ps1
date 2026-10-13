# Compila o OptiScaler deste compilado (sai OptiScaler.dll, que vai para o jogo como dxgi.dll).
# Precisa: Visual Studio 2022 (ou Build Tools) com "Desenvolvimento para desktop com C++" e o SDK do Windows 10/11,
# e as dependências de buscar_externos.ps1.
# Uso: powershell -ExecutionPolicy Bypass -File compilar.ps1
$ErrorActionPreference = 'Stop'
$aqui = Split-Path -Parent $MyInvocation.MyCommand.Path
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio com C++ nao encontrado' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$log = Join-Path $aqui 'build.log'
cmd /c "`"$vcvars`" >nul 2>nul & msbuild `"$aqui\OptiScaler\OptiScaler.vcxproj`" /p:Configuration=Release /p:Platform=x64 /p:SolutionDir=`"$aqui\\`" /m /v:quiet /nologo /fl /flp:logfile=`"$log`";verbosity=minimal"
$erros = Select-String $log -Pattern ' error '
"erros: $($erros.Count)"
$erros | Select-Object -First 40 | ForEach-Object { $_.Line }
$dll = Join-Path $aqui 'x64\Release\a\OptiScaler.dll'
if ($erros.Count -eq 0 -and (Test-Path $dll)) { "ok: $dll" } else { exit 1 }
