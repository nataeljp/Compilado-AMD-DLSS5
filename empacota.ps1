# Gera o zip de distribuicao (para a aba Releases): DLSS5Overlay.exe, runtime (pesos FP16), config padrao, licencas, README.
param([string]$Versao = '0.2')
$ErrorActionPreference = 'Stop'
$raiz = $PSScriptRoot
$saida = Join-Path (Split-Path $raiz) 'DLSS5-Overlay-release'
$pasta = Join-Path $saida "DLSS5-Overlay-v$Versao"
if (Test-Path $pasta) { python -c "import shutil,sys; shutil.rmtree(sys.argv[1])" $pasta }
New-Item -ItemType Directory -Force $pasta | Out-Null
Copy-Item "$raiz\bin\DLSS5Overlay.exe" $pasta
# captura de buffer: o modulo que o overlay coloca no jogo aberto
New-Item -ItemType Directory -Force "$pasta\captura" | Out-Null
Copy-Item "$raiz\bin\captura\DLSS5Buffer.dll" "$pasta\captura"
Copy-Item "$raiz\README.md" $pasta
Copy-Item "$raiz\licencas" "$pasta\licencas" -Recurse
# runtime sem logs nem caches (as variantes e a pasta montada da rede pequena sao refeitas pelo programa)
robocopy "$raiz\bin\runtime" "$pasta\runtime" /E /XF *.log /XD nr_variantes native-game-tiled-assets-pequena logs shader-cache /NFL /NDL /NJH /NJS /NP | Out-Null
# robocopy: 0-7 = sucesso (1 = copiou arquivos), 8+ = falha
if ($LASTEXITCODE -ge 8) { "robocopy falhou ($LASTEXITCODE)"; exit 1 }
$global:LASTEXITCODE = 0
# configuracao padrao (a do usuario nao vai no pacote)
Set-Content -Path "$pasta\config.json" -Encoding ascii -Value @'
{
  "modelo": 0,
  "intensidade": 1,
  "tomLocal": 1,
  "estruturaLocal": 1,
  "estruturaPele": 1,
  "mascara": true,
  "gradacao": true,
  "composicao": 1,
  "corComposicao": 1,
  "protecao": 2,
  "passes": 1,
  "redeCompleta": true,
  "alturaRede": 1080,
  "forca": 1,
  "ponte": -1,
  "brancoNits": 0,
  "modo": 0,
  "movimento": true,
  "usaConfianca": true,
  "ritmo": 1,
  "ritmoMax": 4,
  "reusoVit": true,
  "estabilidade": 8,
  "rejeicao": 12,
  "divisao": 0.5,
  "fpsMax": 60,
  "ocultaDeGravacoes": false
}
'@
$zip = Join-Path $saida "DLSS5-Overlay-v$Versao.zip"
if (Test-Path $zip) { python -c "import os,sys; os.remove(sys.argv[1])" $zip }
Compress-Archive -Path $pasta -DestinationPath $zip -CompressionLevel Optimal
# a pasta montada era so para compactar: fica o zip (a copia descompactada duplicava 660 MB)
python -c "import shutil,sys; shutil.rmtree(sys.argv[1])" $pasta
$h = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"{0}: {1:N1} MB, sha256 {2}" -f $zip, ((Get-Item $zip).Length / 1MB), $h
