# Monta bin\runtime: pesos e assets de uma pasta DLSS5-AMD (native-game-tiled-assets), os DLLs do runtime v3 (lmxxf com
# profundidade dinamica e passes sem recriar a rede + adaptador v3) e os pesos de historico extraidos do nvngx_dlssnr 310.8.
# Pesos exatos em FP16 vao como .f16 (metade do tamanho, saida identica bit a bit); o resto por hardlink/copia.
param([string]$Origem = 'D:\AI_Claude\Compilado-AMD-DLSS5\inteiro\DLSS5-AMD',
      [string]$Upstream = 'D:\AI_Claude\dlssnr_rebuild\lmxxf\build_msvc\LmxxfNrRuntime_v3.dll',
      [string]$Adaptador = 'D:\AI_Claude\dlssnr_rebuild\tools\lmxxf_port\adaptador\v3\LmxxfNrRuntime.dll',
      [string]$DllNvidia = 'D:\GaMeS\Cyberpunk 2077\bin\x64\nvngx_dlssnr.dll')
$ErrorActionPreference = 'Stop'
$dst = Join-Path $PSScriptRoot 'bin\runtime'
if (Test-Path $dst) { python -c "import shutil,sys; shutil.rmtree(sys.argv[1])" $dst }
python (Join-Path $PSScriptRoot 'ferramentas\pesos_f16.py') $Origem $dst
Copy-Item $Upstream (Join-Path $dst 'lmxxf_upstream.dll') -Force
Copy-Item $Adaptador (Join-Path $dst 'LmxxfNrRuntime.dll') -Force
python (Join-Path $PSScriptRoot 'ferramentas\extrai_historico.py') $DllNvidia (Join-Path $dst 'native-game-tiled-assets')
