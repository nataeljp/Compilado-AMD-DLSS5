# Monta bin\runtime: os pesos e assets de um pacote DLSS5-AMD (a pasta com native-game-tiled-assets), o runtime v4 deste
# repositorio (runtime\lmxxf: lmxxf + patch, e runtime\adaptador) e os pesos de historico extraidos do nvngx_dlssnr 310.8.
# Pesos exatos em FP16 vao como .f16 (metade do tamanho, saida identica bit a bit); os outros .f32 por hardlink.
# Runtime v4: LmxxfNrSetMask no runtime e no adaptador, os 7 kernels de entrada da rede com os controles por pixel e o
# codificador que passa o alfa. O overlay nao usa mais os controles por pixel (a mascara por classificador saiu); os
# modulos continuam porque o runtime v4 foi feito com eles, e com os controles por pixel desligados a saida e a mesma.
# Antes: runtime\lmxxf\compilar.ps1 -Modulos e runtime\adaptador\compilar.ps1.
# Uso: monta_runtime.ps1 [-Pesos <pasta DLSS5-AMD>] [-DllNvidia <nvngx_dlssnr.dll 310.8>] [-DllPequena <nvngx_dlssnr.dll 310.9.16>]
#   sem parametros usa dependencias\ (fora do git: pesos\DLSS5-AMD, nvidia\nvngx_dlssnr_310.8.dll e _310.9.16.dll)
param([string]$Pesos = (Join-Path $PSScriptRoot 'dependencias\pesos\DLSS5-AMD'),
      [string]$DllNvidia = (Join-Path $PSScriptRoot 'dependencias\nvidia\nvngx_dlssnr_310.8.dll'),
      [string]$DllPequena = (Join-Path $PSScriptRoot 'dependencias\nvidia\nvngx_dlssnr_310.9.16.dll'),
      [string]$Lmxxf = (Join-Path $PSScriptRoot 'runtime\lmxxf\fonte'),
      [string]$Adaptador = (Join-Path $PSScriptRoot 'runtime\adaptador\LmxxfNrRuntime.dll'))
$ErrorActionPreference = 'Stop'
foreach ($f in $Pesos, $DllNvidia, $Adaptador, (Join-Path $Lmxxf 'build_msvc\LmxxfNrRuntime_v4.dll')) {
    if (-not (Test-Path $f)) { "falta: $f"; exit 1 }
}
$dst = Join-Path $PSScriptRoot 'bin\runtime'
$assets = Join-Path $dst 'native-game-tiled-assets'
# troca um arquivo sem escrever por cima dele: se o destino for hardlink de outro pacote, so o vinculo sai
function Coloca([string]$de, [string]$para) {
    if (Test-Path $para) { [IO.File]::Delete($para) }
    Copy-Item $de $para
}
if (Test-Path $dst) { python -c "import shutil,sys; shutil.rmtree(sys.argv[1])" $dst }
python (Join-Path $PSScriptRoot 'ferramentas\pesos_f16.py') $Pesos $dst
Coloca (Join-Path $Lmxxf 'build_msvc\LmxxfNrRuntime_v4.dll') (Join-Path $dst 'lmxxf_upstream.dll')
Coloca $Adaptador (Join-Path $dst 'LmxxfNrRuntime.dll')
# os 7 modulos com a entrada por pixel (gfx1201, COMGR) e o codificador que passa o alfa (runtime v4)
Get-ChildItem (Join-Path $Lmxxf 'build_msvc\modules_mascara') -Filter *.hsaco | ForEach-Object { Coloca $_.FullName (Join-Path $assets "HIP\$($_.Name)") }
Coloca (Join-Path $Lmxxf 'shaders\native_codec_encode.hlsl') (Join-Path $assets 'native_codec_encode.hlsl')
python (Join-Path $PSScriptRoot 'ferramentas\extrai_historico.py') $DllNvidia $assets
# rede pequena da 310.9.16 (opcao 'Rede' no painel): runtime\pequena = so os pesos dela + network-layout.txt
if ($DllPequena -and (Test-Path $DllPequena)) {
    python (Join-Path $PSScriptRoot 'ferramentas\monta_pacote_pequena.py') (Join-Path $env:TEMP 'dlss5_rede_pequena') $dst $DllPequena $DllNvidia
} else { "sem -DllPequena: a rede pequena fica de fora (escolher 'pequena' no painel usa a grande)" }
