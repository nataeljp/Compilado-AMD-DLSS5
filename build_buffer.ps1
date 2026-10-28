# Captura de buffer: compila o jogo de teste (ferramentas\jogo_teste) e a DLSS5Buffer.dll (o modulo que o overlay coloca
# no jogo aberto). Saida: bin\captura\DLSS5Buffer.dll e obj\JogoTeste.exe (ou -Saida <pasta>).
param([string]$Saida = '', [string]$JogoTeste = '')
$ErrorActionPreference = 'Stop'
$raiz = $PSScriptRoot
if (-not $Saida) { $Saida = "$raiz\bin\captura" }
if (-not $JogoTeste) { $JogoTeste = "$raiz\obj\JogoTeste.exe" }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = Join-Path (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) 'VC\Auxiliary\Build\vcvars64.bat'
New-Item -ItemType Directory -Force $Saida, "$raiz\obj\buffer" | Out-Null
$flags = '/nologo /std:c++20 /EHsc /O2 /MT /W3 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /permissive- /Zc:__cplusplus /utf-8'
$cli = "`"$raiz\src\buffer\ClienteBuffer.cpp`" `"$raiz\src\buffer\Geracao.cpp`" `"$raiz\src\buffer\GeracaoNeural.cpp`""
# geracao de quadros: as bibliotecas do FidelityFX SDK 1.1.4 compiladas do codigo fonte (externo\compila_ffx.ps1)
$ffx = "$raiz\externo\fidelityfx_sdk\sdk"
$ffxInc = "/I`"$ffx\include`""
$ffxLibs = "`"$ffx\bin\ffx_sdk\ffx_backend_dx12_x64.lib`" `"$ffx\bin\ffx_sdk\ffx_frameinterpolation_x64.lib`" `"$ffx\bin\ffx_sdk\ffx_opticalflow_x64.lib`""
$cl = "cl $flags $ffxInc /Fo`"$raiz\obj\buffer\\`" /Fe`"$JogoTeste`" `"$raiz\ferramentas\jogo_teste\JogoTeste.cpp`" $cli /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup d3d12.lib dxgi.lib user32.lib $ffxLibs"
cmd /c "`"$vs`" >nul 2>nul & $cl" 2>&1 | Where-Object { $_ -match 'error|warning C4[0-9]{3}' } | Select-Object -First 30
if ($LASTEXITCODE -ne 0) { "jogo de teste falhou ($LASTEXITCODE)"; exit 1 }
"ok: $JogoTeste"
$modulo = Get-ChildItem "$raiz\src\jogo\*.cpp" -ErrorAction SilentlyContinue
if ($modulo) {
    $fontes = ($modulo | ForEach-Object { '"' + $_.FullName + '"' }) -join ' '
    $inc = "/I`"$raiz\externo\ffx`" /I`"$raiz\externo\detours`" $ffxInc"
    # as funcoes da dxgi.dll do Windows (a DLL instalada na pasta do jogo como dxgi.dll) e as exportacoes
    $asm = "ml64 /nologo /c /Fo`"$raiz\obj\buffer\ProxyDxgi.obj`" `"$raiz\src\jogo\ProxyDxgi.asm`""
    $cl = "$asm && cl $flags /LD $inc /Fo`"$raiz\obj\buffer\\`" /Fe`"$Saida\DLSS5Buffer.dll`" $fontes $cli `"$raiz\obj\buffer\ProxyDxgi.obj`" /link /MAP:`"$Saida\DLSS5Buffer.map`" /DEF:`"$raiz\src\jogo\DLSS5Buffer.def`" /DELAYLOAD:dxgi.dll delayimp.lib d3d12.lib dxgi.lib user32.lib `"$raiz\externo\detours\detours.lib`" $ffxLibs"
    cmd /c "`"$vs`" >nul 2>nul & $cl" 2>&1 | Where-Object { $_ -match 'error|warning C4[0-9]{3}|LNK' } | Select-Object -First 30
    if ($LASTEXITCODE -ne 0) { "DLSS5Buffer.dll falhou ($LASTEXITCODE)"; exit 1 }
    "ok: $Saida\DLSS5Buffer.dll"
}
