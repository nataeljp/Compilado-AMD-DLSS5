# Monta bin\runtime a partir de uma pasta DLSS5-AMD (LmxxfNrRuntime.dll, lmxxf_upstream.dll, native-game-tiled-assets).
# Usa hardlinks quando a origem esta no mesmo volume (nao duplica os pesos), copia senao.
param([string]$Origem = 'D:\AI_Claude\Compilado-AMD-DLSS5\inteiro\DLSS5-AMD')
$ErrorActionPreference = 'Stop'
$dst = Join-Path $PSScriptRoot 'bin\runtime'
$mesmoVolume = (Split-Path -Qualifier $Origem) -eq (Split-Path -Qualifier $dst)
$itens = @('LmxxfNrRuntime.dll', 'lmxxf_upstream.dll') | ForEach-Object { Get-Item (Join-Path $Origem $_) }
$itens += Get-ChildItem (Join-Path $Origem 'native-game-tiled-assets') -Recurse -File
$n = 0
foreach ($f in $itens) {
    $rel = $f.FullName.Substring($Origem.Length).TrimStart('\')
    $alvo = Join-Path $dst $rel
    New-Item -ItemType Directory -Force (Split-Path $alvo) | Out-Null
    if (Test-Path $alvo) { continue }
    if ($mesmoVolume) { New-Item -ItemType HardLink -Path $alvo -Target $f.FullName | Out-Null }
    else { Copy-Item $f.FullName $alvo }
    $n++
}
"runtime: $n arquivos novos em $dst"
