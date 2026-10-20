# Captura de buffer: compila o jogo de teste (ferramentas\jogo_teste) e a DLSS5Buffer.dll (o modulo que o overlay coloca
# no jogo aberto). Saida: bin\captura\DLSS5Buffer.dll e obj\JogoTeste.exe (ou -Saida <pasta>).
param([string]$Saida = '')
$ErrorActionPreference = 'Stop'
$raiz = $PSScriptRoot
if (-not $Saida) { $Saida = "$raiz\bin\captura" }
$vs = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
New-Item -ItemType Directory -Force $Saida, "$raiz\obj\buffer" | Out-Null
$flags = '/nologo /std:c++20 /EHsc /O2 /MT /W3 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /permissive- /Zc:__cplusplus /utf-8'
$cli = "`"$raiz\src\buffer\ClienteBuffer.cpp`""
$cl = "cl $flags /Fo`"$raiz\obj\buffer\\`" /Fe`"$raiz\obj\JogoTeste.exe`" `"$raiz\ferramentas\jogo_teste\JogoTeste.cpp`" $cli /link /SUBSYSTEM:CONSOLE d3d12.lib dxgi.lib user32.lib"
cmd /c "`"$vs`" >nul 2>nul & $cl" 2>&1 | Where-Object { $_ -match 'error|warning C4[0-9]{3}' } | Select-Object -First 30
if ($LASTEXITCODE -ne 0) { "jogo de teste falhou ($LASTEXITCODE)"; exit 1 }
"ok: $raiz\obj\JogoTeste.exe"
$modulo = Get-ChildItem "$raiz\src\jogo\*.cpp" -ErrorAction SilentlyContinue
if ($modulo) {
    $fontes = ($modulo | ForEach-Object { '"' + $_.FullName + '"' }) -join ' '
    $inc = "/I`"$raiz\externo\ffx`" /I`"$raiz\externo\detours`""
    $cl = "cl $flags /LD $inc /Fo`"$raiz\obj\buffer\\`" /Fe`"$Saida\DLSS5Buffer.dll`" $fontes $cli /link d3d12.lib dxgi.lib user32.lib `"$raiz\externo\detours\detours.lib`""
    cmd /c "`"$vs`" >nul 2>nul & $cl" 2>&1 | Where-Object { $_ -match 'error|warning C4[0-9]{3}|LNK' } | Select-Object -First 30
    if ($LASTEXITCODE -ne 0) { "DLSS5Buffer.dll falhou ($LASTEXITCODE)"; exit 1 }
    "ok: $Saida\DLSS5Buffer.dll"
}
