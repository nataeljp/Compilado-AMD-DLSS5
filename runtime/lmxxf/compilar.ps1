# Runtime da rede (lmxxf, de Kien, MIT) com as mudancas deste projeto: clona o lmxxf no commit base a9b7a52, aplica
# lmxxf_a9b7a52.patch (pulos por quadro, rede pequena, passes sem recriar, mascara por pixel, Streamline) e compila
# LmxxfNrRuntime_v4.dll (no runtime ela vai com o nome lmxxf_upstream.dll; o monta_runtime.ps1 cuida disso).
# -Modulos: compila tambem os 7 modulos HIP com a mascara por pixel (gfx1201). O rtc_compile usa o amd_comgr do driver
# da AMD, sem HIP SDK. Os outros modulos vem prontos no pacote de pesos (native-game-tiled-assets\HIP).
# Precisa: Visual Studio 2022 com C++ e git.
# Uso: powershell -ExecutionPolicy Bypass -File runtime\lmxxf\compilar.ps1 [-Modulos]
param([string]$Fonte = (Join-Path $PSScriptRoot 'fonte'),
      [string]$Repositorio = 'https://github.com/lmxxf/dlss5-on-amd-9070xt-porting.git',
      [switch]$Modulos)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path (Join-Path $Fonte '.git'))) {
    git clone --quiet $Repositorio $Fonte
    git -C $Fonte checkout --quiet a9b7a52
    git -C $Fonte apply --whitespace=nowarn (Join-Path $PSScriptRoot 'lmxxf_a9b7a52.patch')
    if ($LASTEXITCODE -ne 0) { "o patch nao aplicou"; exit 1 }
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$saida = Join-Path $Fonte 'build_msvc'
New-Item -ItemType Directory -Force "$saida\v4" | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'msvc_prelude.h') $saida -Force
Push-Location $Fonte
cmd /c "`"$vcvars`" >nul 2>nul & cl /nologo /std:c++17 /O2 /EHsc /MT /LD /bigobj /D_WIN32_WINNT=0x0A00 /DLMXXF_NR_RUNTIME_EXPORTS /DNOMINMAX `"/FI$saida\msvc_prelude.h`" /I include /I src /I Development\HIP src\LmxxfNrRuntime.cpp /Fobuild_msvc\v4\ /Fe:build_msvc\LmxxfNrRuntime_v4.dll d3d12.lib dxgi.lib d3dcompiler.lib dxguid.lib user32.lib" | Where-Object { $_ -match ' error ' }
Pop-Location
if (-not (Test-Path "$saida\LmxxfNrRuntime_v4.dll")) { "falhou: LmxxfNrRuntime_v4.dll"; exit 1 }
"ok: $saida\LmxxfNrRuntime_v4.dll"
if ($Modulos) {
    $rtc = Join-Path $saida 'rtc_compile.exe'
    if (-not (Test-Path $rtc)) {
        cmd /c "`"$vcvars`" >nul 2>nul & cl /nologo /O2 /EHsc /std:c++17 `"$Fonte\hip\rtc_compile.cpp`" /Fe`"$rtc`" /Fo`"$saida\\`"" | Where-Object { $_ -match ' error ' }
        if (-not (Test-Path $rtc)) { "falhou: rtc_compile.exe"; exit 1 }
    }
    # as opcoes de compilador do pacote oficial para a familia c32-wave1 (post-misched=0 + max-ilp); os outros, sem opcao
    $op = '-mllvm=-enable-post-misched=0 -mllvm=-amdgpu-sched-strategy=max-ilp'
    $mods = Join-Path $saida 'modules_mascara'
    New-Item -ItemType Directory -Force $mods | Out-Null
    $tmp = Join-Path $saida 'modules_mascara_tmp'
    foreach ($m in 'c32-wave1', 'c32-wave1-rtz', 'c32-wave1-fast', 'c32-wave1-fast-norm900', 'prefix_fast',
                   'c32_fused_ffn_attention', 'c32_fused_ffn_attention-packed') {
        $extra = if ($m -like 'c32-wave1*') { $op } else { '' }
        & (Join-Path $Fonte 'hip\build-modules.ps1') -OutputDir "$tmp\$m" -Compiler $rtc -Only $m -Targets gfx1201 -ExtraOpts $extra | Out-Null
        Copy-Item "$tmp\$m\$m.hsaco" $mods -Force
    }
    python -c "import shutil,sys; shutil.rmtree(sys.argv[1])" $tmp
    "ok: 7 modulos em $mods"
}
