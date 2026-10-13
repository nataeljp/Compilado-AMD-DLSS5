# Compila as três DLLs deste compilado e troca as do pacote pronto (..\inteiro). O resto do pacote (FidelityFX, XeSS,
# Streamline, pesos, kernels HIP, RTGI, OptiPatcher) já vem pronto em inteiro\.
# Uso: powershell -ExecutionPolicy Bypass -File atualiza_inteiro.ps1
$ErrorActionPreference = 'Stop'
$aqui = Split-Path -Parent $MyInvocation.MyCommand.Path
$inteiro = Join-Path (Split-Path -Parent $aqui) 'inteiro'

& (Join-Path $aqui 'optiscaler\buscar_externos.ps1')
& (Join-Path $aqui 'optiscaler\compilar.ps1')
& (Join-Path $aqui 'lmxxf_runtime\compilar.ps1')
& (Join-Path $aqui 'adaptador\compilar.ps1')

Copy-Item (Join-Path $aqui 'optiscaler\x64\Release\a\OptiScaler.dll') (Join-Path $inteiro 'dxgi.dll') -Force
Copy-Item (Join-Path $aqui 'lmxxf_runtime\lmxxf\build_msvc\LmxxfNrRuntime_v2.dll') (Join-Path $inteiro 'lmxxf_upstream.dll') -Force
Copy-Item (Join-Path $aqui 'adaptador\LmxxfNrRuntime.dll') (Join-Path $inteiro 'LmxxfNrRuntime.dll') -Force
"inteiro atualizado: $inteiro"
