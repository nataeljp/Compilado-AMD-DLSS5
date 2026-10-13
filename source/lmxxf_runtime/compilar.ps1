# Runtime da rede (lmxxf, de Kien, MIT) com as mudanças deste compilado: baixa o lmxxf no commit base, aplica
# lmxxf_a9b7a52.patch e compila LmxxfNrRuntime_v2.dll (no pacote ela vai com o nome lmxxf_upstream.dll).
# Precisa: Visual Studio 2022 com C++ e git. Os kernels HIP (.hsaco) já vão prontos no pacote (inteiro\DLSS5-AMD\
# native-game-tiled-assets\HIP); para refazê-los veja o README do lmxxf (hip\rtc_compile.cpp, HIP SDK da AMD).
# Uso: powershell -ExecutionPolicy Bypass -File compilar.ps1
$ErrorActionPreference = 'Stop'
$aqui = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $aqui 'lmxxf'
if (-not (Test-Path (Join-Path $src '.git'))) {
    git clone --quiet https://github.com/lmxxf/dlss5-on-amd-9070xt-porting.git $src
    git -C $src checkout --quiet a9b7a52
    git -C $src apply --whitespace=nowarn (Join-Path $aqui 'lmxxf_a9b7a52.patch')
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$saida = Join-Path $src 'build_msvc'
New-Item -ItemType Directory -Force "$saida\v2" | Out-Null
Copy-Item (Join-Path $aqui 'msvc_prelude.h') $saida -Force
Push-Location $src
cmd /c "`"$vcvars`" >nul 2>nul & cl /nologo /std:c++17 /O2 /EHsc /MT /LD /bigobj /D_WIN32_WINNT=0x0A00 /DLMXXF_NR_RUNTIME_EXPORTS /DNOMINMAX `"/FI$saida\msvc_prelude.h`" /I include /I src /I Development\HIP src\LmxxfNrRuntime.cpp /Fobuild_msvc\v2\ /Fe:build_msvc\LmxxfNrRuntime_v2.dll d3d12.lib dxgi.lib d3dcompiler.lib dxguid.lib user32.lib"
Pop-Location
if (Test-Path "$saida\LmxxfNrRuntime_v2.dll") { "ok: $saida\LmxxfNrRuntime_v2.dll" } else { exit 1 }
