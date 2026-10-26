# Compila as bibliotecas do FidelityFX SDK 1.1.4 que a geracao de quadros usa (src\buffer\Geracao.cpp): o backend DX12,
# Frame Interpolation e Optical Flow, estaticas e com o runtime C estatico (/MT, como a DLSS5Buffer.dll e o jogo de
# teste). O SDK fica fora do git: o zip oficial da AMD (FidelityFX-SDK-v1.1.4.zip, github.com/GPUOpen-LibrariesAndSDKs/
# FidelityFX-SDK) extraido em externo\fidelityfx_sdk. Os shaders sao compilados pelo proprio SDK (FidelityFX_SC.exe).
# Saida: externo\fidelityfx_sdk\sdk\bin\ffx_sdk\*.lib (o build_buffer.ps1 liga dali).
$ErrorActionPreference = 'Stop'
$sdk = "$PSScriptRoot\fidelityfx_sdk\sdk"
if (-not (Test-Path "$sdk\CMakeLists.txt")) { "FidelityFX SDK 1.1.4 nao encontrado em $sdk"; exit 1 }
# geracao multipla (3x/4x): t = k/N nos shaders da interpolacao e ate 3 gerados por quadro no swapchain (idempotente)
python -I "$PSScriptRoot\ffx_multiquadro.py" $sdk
if ($LASTEXITCODE -ne 0) { "patch do SDK falhou"; exit 1 }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake -S $sdk -B "$sdk\build" -G 'Visual Studio 17 2022' -A x64 -DFFX_API_BACKEND=DX12_X64 -DFFX_FSR3=ON `
    -DFFX_AUTO_COMPILE_SHADERS=1 -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded | Select-Object -Last 2
& $cmake --build "$sdk\build" --config Release --target ffx_backend_dx12_x64 ffx_frameinterpolation_x64 ffx_opticalflow_x64 `
    --parallel 4 | Select-String 'error|-> '
