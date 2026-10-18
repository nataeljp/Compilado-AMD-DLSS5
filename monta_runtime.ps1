# Monta bin\runtime a partir de uma pasta DLSS5-AMD (LmxxfNrRuntime.dll, lmxxf_upstream.dll, native-game-tiled-assets).
# Pesos exatos em FP16 vao como .f16 (metade do tamanho, saida identica bit a bit); o resto por hardlink/copia.
param([string]$Origem = 'D:\AI_Claude\Compilado-AMD-DLSS5\inteiro\DLSS5-AMD')
$ErrorActionPreference = 'Stop'
$dst = Join-Path $PSScriptRoot 'bin\runtime'
if (Test-Path $dst) { python -c "import shutil,sys; shutil.rmtree(sys.argv[1])" $dst }
python (Join-Path $PSScriptRoot 'ferramentas\pesos_f16.py') $Origem $dst
