# Gera o zip de distribuicao (para a aba Releases): DLSS5Overlay.exe, runtime (pesos FP16), config padrao, licencas, README.
param([string]$Versao = '0.1')
$ErrorActionPreference = 'Stop'
$raiz = $PSScriptRoot
$saida = Join-Path (Split-Path $raiz) 'DLSS5-Overlay-release'
$pasta = Join-Path $saida "DLSS5-Overlay-v$Versao"
if (Test-Path $pasta) { python -c "import shutil,sys; shutil.rmtree(sys.argv[1])" $pasta }
New-Item -ItemType Directory -Force $pasta | Out-Null
Copy-Item "$raiz\bin\DLSS5Overlay.exe" $pasta
Copy-Item "$raiz\README.md" $pasta
Copy-Item "$raiz\licencas" "$pasta\licencas" -Recurse
# runtime sem logs
robocopy "$raiz\bin\runtime" "$pasta\runtime" /E /XF *.log /NFL /NDL /NJH /NJS /NP | Out-Null
# configuracao padrao (a do usuario nao vai no pacote)
Set-Content -Path "$pasta\config.json" -Encoding ascii -Value @'
{
  "estilo": 1,
  "tom": 1,
  "estrutura": 1,
  "pele": 1,
  "outro": 1,
  "detalhe": 1,
  "cor": 1,
  "passes": 1,
  "redeCompleta": true,
  "alturaRede": 1080,
  "forca": 1,
  "modo": 0,
  "divisao": 0.5,
  "fpsMax": 60,
  "movimento": true,
  "ritmo": 1,
  "ocultaDeGravacoes": false,
  "estabilidade": 8,
  "rejeicao": 12,
  "composicao": 0,
  "rdxDetalhe": 1,
  "rdxCor": 1,
  "rdxGuarda": 4
}
'@
$zip = Join-Path $saida "DLSS5-Overlay-v$Versao.zip"
if (Test-Path $zip) { python -c "import os,sys; os.remove(sys.argv[1])" $zip }
Compress-Archive -Path $pasta -DestinationPath $zip -CompressionLevel Optimal
$h = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"{0}: {1:N1} MB, sha256 {2}" -f $zip, ((Get-Item $zip).Length / 1MB), $h
