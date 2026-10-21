# Roda o overlay em modo imagem sobre a foto "off" da NVIDIA com um config e compara com a "on".
# -Off/-On: as fotos de comparacao da NVIDIA (ex.: nvidia-dlss-5-zorah-comparison-002-off.jpg / -on.jpg)
# uso: roda_comparacao.ps1 -Nome x -Off foto_off.jpg -On foto_on.jpg -Config '{ "alturaRede": 2160 }' [-Quadros 8] [-Runtime pasta]
param([Parameter(Mandatory)][string]$Nome, [string]$Config = '{}', [int]$Quadros = 8,
      [Parameter(Mandatory)][string]$Off, [Parameter(Mandatory)][string]$On,
      [string]$Runtime = '', [string]$Pula = '')
$ErrorActionPreference = 'Stop'
$raiz = Split-Path $PSScriptRoot
$dir = Join-Path $raiz 'comparacao'
New-Item -ItemType Directory -Force $dir | Out-Null
$cfg = Join-Path $dir "$Nome.config.json"
Set-Content -Path $cfg -Value $Config -Encoding ascii
$saida = Join-Path $dir "$Nome.rgba"
$args_ = "--imagem `"$Off`" --config `"$cfg`" --quadros $Quadros --grava `"$saida`""
if ($Runtime) { $args_ += " --runtime `"$Runtime`"" }
if ($Pula) { $args_ += " --pula $Pula" }
$log = Join-Path $raiz 'bin\overlay.jsonl'
$p = Start-Process (Join-Path $raiz 'bin\DLSS5Overlay.exe') -ArgumentList $args_ -PassThru
if (-not $p.WaitForExit(180000)) { Stop-Process -Id $p.Id -Force; throw "overlay travou ($Nome)" }
if ($p.ExitCode -ne 0) { Get-Content $log | Select-Object -Last 5; throw "overlay saiu com $($p.ExitCode)" }
Get-Content $log | Select-String 'tempo medio|profundidade|recusad'
python (Join-Path $PSScriptRoot 'compara_nvidia.py') $Off $On $saida $Nome --json (Join-Path $dir "$Nome.json")
