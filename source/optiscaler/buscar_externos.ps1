# Baixa as dependências externas do OptiScaler (os submódulos do upstream) nos commits exatos usados neste compilado.
# Uso: powershell -ExecutionPolicy Bypass -File buscar_externos.ps1
$ErrorActionPreference = 'Stop'
$aqui = Split-Path -Parent $MyInvocation.MyCommand.Path
$externos = @(
    @{ pasta = 'external/FidelityFX-SDK';    url = 'https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK';     commit = 'c6efa6bf7f2027b3ec94f28578bb5965eabb9e55' },
    @{ pasta = 'external/FidelityFX-SDK-v2'; url = 'https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git'; commit = '60f4ea81909200d8542eca14dccb2628b763a9a3' },
    @{ pasta = 'external/magic_enum';        url = 'https://github.com/Neargye/magic_enum';                          commit = 'a733a2ea665ca5d72b7270f0334bf2e7b82bd0cc' },
    @{ pasta = 'external/nvapi';             url = 'https://github.com/NVIDIA/nvapi.git';                            commit = '9b181ea572f680327fe01a14a0f1f41c78034104' },
    @{ pasta = 'external/simpleini';         url = 'https://github.com/brofield/simpleini';                          commit = '6048871ea9ee0ec24be5bd099d161a10567d7dc2' },
    @{ pasta = 'external/spdlog';            url = 'https://github.com/gabime/spdlog.git';                           commit = 'faa0a7a9c5a3550ed5461fab7d8e31c37fd1a2ef' },
    @{ pasta = 'external/unordered_dense';   url = 'https://github.com/martinus/unordered_dense.git';                commit = '73f3cbb237e84d483afafc743f1f14ec53e12314' },
    @{ pasta = 'external/vulkan';            url = 'https://github.com/KhronosGroup/Vulkan-Headers.git';             commit = 'd64e9e156ac818c19b722ca142230b68e3daafe3' },
    @{ pasta = 'external/xess';              url = 'https://github.com/intel/xess.git';                              commit = '8fe81bdbbaf00b3c1b733fd0d830c333dc84e6f0' }
)
foreach ($e in $externos) {
    $destino = Join-Path $aqui $e.pasta
    if (Test-Path (Join-Path $destino '.git')) { "ja existe: $($e.pasta)"; continue }
    if ((Test-Path $destino) -and (Get-ChildItem $destino -Force | Measure-Object).Count -gt 0) {
        "pasta ocupada sem .git, pulando: $($e.pasta)"; continue
    }
    "baixando $($e.pasta) @ $($e.commit.Substring(0, 7))"
    git clone --quiet --filter=blob:none --no-checkout $e.url $destino
    git -C $destino checkout --quiet $e.commit
    git -C $destino submodule update --init --recursive --quiet
}
'pronto'
