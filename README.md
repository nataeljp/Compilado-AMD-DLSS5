# Compilado AMD DLSS5

Rede neural do **DLSS 5 (Neural Rendering)** rodando em placas **AMD Radeon** (testado na RX 9070, RDNA 4), dentro
de um fork atual do **OptiScaler**, junto com o que a comunidade já tinha feito em volta dela: Ray Regeneration,
geração de quadros multi-frame, iluminação, GI e uma série de correções para os jogos funcionarem sozinhos.

É um **compilado**: quase tudo aqui é trabalho de outras pessoas, juntado, portado para o OptiScaler mais novo e
corrigido. Os créditos de cada parte estão no fim deste arquivo e no menu (Help / Status → Créditos). O objetivo de
publicar é que mais gente consiga estudar, montar o próprio e fazer melhor.

> Não é afiliado à NVIDIA, à AMD nem à Intel. O desenho da rede DLSS 5 e os pesos pertencem à NVIDIA.

## O que tem

- **Rede DLSS5 (lmxxf 2) na AMD**, por HIP, com três posições:
  - antes do upscale (resolução de render, com vetores de movimento e profundidade do jogo);
  - depois do upscale (na saída ampliada; automático quando há Ray Reconstruction);
  - **modo captura de vídeo**: a rede roda no quadro pronto, na hora do Present, como sobre um vídeo. Não depende do
    upscaler do jogo. Aceita SDR, HDR10 (PQ) e scRGB.
- **Controles da rede**: estilo, tom, estrutura, pele, máscara de personagem, força do detalhe/cor/resíduo, edição,
  exposição; presets e estilos salvos, desempenho e qualidade do AMDNR; filtro de aparência; iluminação
  **RenoDRT / ACES**; **RTGI** (GI de tela).
- **Rejeição estilo TSR no arrasto da edição da rede**: a edição do lmxxf chega um quadro depois e é levada pelos
  vetores de movimento; agora ela só é carregada onde a cor crua do quadro dela bate com a deste, com tolerância ao
  ruído aprendida por pixel (o "Edit accumulation" do AMDNR, também sem Model interleave). O que só pisca segura a
  edição; folhagem que anda sem vetor perde a edição em vez de deixar rastro.
- **TSR próprio** (`TSR (lmxxf2)` na lista de upscalers, `Dx12Upscaler=tsr` no ini): upscaler temporal escrito do zero
  seguindo as etapas que a Epic descreve publicamente para o TSR da UE5 (nenhum código da Unreal):
  - heurística de paralaxe (desoclusão pela profundidade reprojetada);
  - anti-aliasing espacial onde não há histórico;
  - histórico com recorte por variância em YCoCg;
  - **rejeição de sombreamento com detecção de cintilação**, que conta em tempo e não em quadros (como o ajuste ao
    frame rate do TSR), então se comporta igual com FreeSync/VRR.

  Os ajustes ficam no cartão "TSR (lmxxf2)" da aba Upscaling: amostras, rigidez do recorte, desoclusão,
  anti-aliasing, proteção contra cintilação e movimento máximo protegido.
- **Ray Regeneration**:
  - **FSR-RR** (denoiser AMD FFX 1.2 + FSR no lugar do DLSS Ray Reconstruction);
  - o **Ray Regeneration do próprio jogo** (ex.: Crimson Desert) agora recebe os ajustes que o jogo manda (o
    OptiScaler original devolvia "ok" sem repassar) e pode ser ajustado no menu: os 6 parâmetros do denoiser nas
    faixas do sample da AMD, sinais que o jogo usa, dispatches/s e "Recomeçar histórico".
- **Frame Generation**:
  - **multi-frame até 6x sem spoof** pelo XeFG destravado (entrada FSR 3 FG do jogo → saída XeFG);
  - jogo com DLSS Frame Generation → FFX + DLSS Enabler (Combo), até 6x;
  - escolha **automática** pelo que o jogo traz, ajustes de HUD do Enabler automáticos.
- **Spoof NVIDIA automático**: só liga quando o jogo não tem FSR (o upscaler só entra pelo DLSS).
- **Modo seguro que aprende com queda**: se o jogo cai nos primeiros minutos, na próxima vez desliga o recurso
  suspeito (a rede, se a queda veio logo depois de ela ligar; senão spoof → FG automático → rede).
- **Correção para Unreal Engine**: a cor que a rede entrega ao upscaler não recebe mais a barreira de "render
  target" do Unreal (fazia o Gears of War: E-Day cair com `E_INVALIDARG` ao ligar a rede).
- **Menu**: botão **R** no canto de cada cartão (volta o cartão ao padrão), largura e tamanho de fonte ajustáveis,
  aba Log, créditos.

## Estrutura

```
inteiro/   o pacote pronto para copiar na pasta do jogo (o mesmo usado nos testes)
source/    tudo o que é preciso para compilar e para refazer o seu
  optiscaler/        fork do OptiScaler com o AMDNR e as mudanças deste compilado (ramo lmxxf2)
  lmxxf_runtime/     patch sobre o runtime do lmxxf (commit a9b7a52) + script de build
  adaptador/         LmxxfNrRuntime.dll: a API de runtime do AMDNR por cima do runtime do lmxxf
  ferramentas/
    optiscaler_fork/       geradores de tabela usados pelo menu (reset por cartão, teste ao vivo)
    pesos/                 como os pesos foram gerados a partir da nvngx_dlssnr.dll
    port_amdnr_upstream/   scripts de uso único que portaram o AMDNR para o OptiScaler atual (histórico)
  atualiza_inteiro.ps1     compila as 3 DLLs e troca as do inteiro/
```

## Instalar

1. Copie **todo o conteúdo de `inteiro/`** para a pasta do executável do jogo (a pasta onde está o `.exe` que roda o
   jogo; em Unreal é `...\Binaries\Win64`).
2. Abra o jogo, escolha **FSR** (ou o upscaler que o jogo tiver) no menu do jogo.
3. **Insert** abre o menu. A aba **DLSS5** tem a rede; a aba **Frame Gen** tem o FG automático, o spoof e o modo
   seguro; **Upscaling** tem o Ray Regeneration do jogo.

Requisitos: Windows 10/11, placa AMD RDNA 4 (RX 9070 / 9070 XT; RDNA 3 com o runtime do AMDNR), driver Adrenalin
recente com HIP (`amdhip64_7.dll`). Placas NVIDIA usam o caminho NVIDIA do OptiScaler.

## Compilar

Precisa: Visual Studio 2022 (ou Build Tools) com C++ e SDK do Windows, git.

```powershell
cd source
powershell -ExecutionPolicy Bypass -File atualiza_inteiro.ps1
```

Isso baixa as dependências do OptiScaler nos commits exatos (`optiscaler/buscar_externos.ps1`), compila o
OptiScaler (`dxgi.dll`), o runtime do lmxxf com o patch (`lmxxf_upstream.dll`) e o adaptador
(`LmxxfNrRuntime.dll`), e troca os três em `inteiro/`. O resto do pacote (FidelityFX, XeSS, Streamline, pesos,
kernels HIP, RTGI, OptiPatcher) vem pronto.

## Como montar o seu próprio DLSS5 na AMD

O caminho que este compilado seguiu, peça por peça:

1. **Pesos**: saem da `nvngx_dlssnr.dll` da NVIDIA pelo conversor MIT do guentra
   (`linux/dlssnr/convert_dll.py` em [guentra/dlss5-amd-hip-linux](https://github.com/guentra/dlss5-amd-hip-linux)),
   modo `amd-consumer-derived`. `source/ferramentas/pesos/converte_guentra.py` roda o conversor e compara com uma
   pasta existente; `gera_assets.py` é a versão própria anterior (com as lacunas anotadas no código). O resultado são
   os 184 `.f32` de `inteiro/DLSS5-AMD/native-game-tiled-assets`.
2. **Rede na GPU AMD**: o runtime HIP do **lmxxf**
   ([lmxxf/dlss5-on-amd-9070xt-porting](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting)), com o patch de
   `source/lmxxf_runtime` (otimizações bit-exatas e ajustes da ponte D3D12↔HIP). Kernels `.hsaco` prontos em
   `inteiro/.../HIP`; para refazer, `hip/rtc_compile.cpp` do lmxxf e o HIP SDK da AMD.
3. **Ponte para o jogo**: o código do **AMDNR** (`source/optiscaler/OptiScaler/dlssnr/`): `amd/AmdBridge.cpp` pega a
   cor, os vetores e a profundidade que o jogo entrega ao upscaler (`upscalers/IFeature_Dx12.cpp`, `Evaluate`), roda
   a rede e devolve a cor editada ao upscaler; `lmxxf/LmxxfBackend.cpp` é o lado do runtime lmxxf; o modo captura de
   vídeo está em `amd/PresentExperimental.h`.
4. **Adaptador** (`source/adaptador`): traduz a API de runtime que o AMDNR espera para a do lmxxf.
5. **TSR próprio** (`source/optiscaler/OptiScaler/upscalers/tsr/`): `TsrShaders.h` tem os três passes (paralaxe,
   anti-aliasing, histórico) e `TsrFeature_Dx12.cpp` liga ao OptiScaler como mais um upscaler.
6. **Menu** (`source/optiscaler/OptiScaler/menu/lmxxf2_menu.inl` e `menu_common.cpp`).

## Créditos

Cada um pela sua parte (licenças completas em `inteiro/Licenses`):

| Quem | O quê | Licença |
|---|---|---|
| **AMDNR by 3zwr1** — [github.com/3zwr1/AMD-NR---OptiScaler](https://github.com/3zwr1/AMD-NR---OptiScaler) | núcleo neural do AMD NR dentro do OptiScaler: ponte AMD, integração do runtime lmxxf, passes temporais, interleave, controles e menu da rede, auto-exposição, formato do runtime, kernels c32w | GPL-3.0 + termos adicionais (seções 3 e 4 do `AMDNR_NOTICE.txt`) |
| **OptiScaler** e contribuidores — [github.com/optiscaler/OptiScaler](https://github.com/optiscaler/OptiScaler) | a base: upscalers, frame generation, ganchos, menu | GPL-3.0 |
| **lmxxf (Kien)** — [github.com/lmxxf/dlss5-on-amd-9070xt-porting](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) | porte da rede DLSS 5 para AMD: kernels HIP e runtime | MIT |
| **TheAutomatic**, **Dagherbou**, **wilsjo2**, **Matheus** | linhagem DLSS-NR / pre-SR de onde o AMDNR partiu; partes do runtime lmxxf | GPL-3.0 / MIT |
| **guentra** — [github.com/guentra/dlss5-amd-hip-linux](https://github.com/guentra/dlss5-amd-hip-linux) | conversor dos pesos da DLL | MIT |
| **Zach Hembree (DarkHelmet)** e **burak113** — [github.com/burak113/OptiScaler](https://github.com/burak113/OptiScaler) | FSR Ray Regeneration para o OptiScaler | GPL-3.0 |
| **Caliel666** — [github.com/Caliel666/neural-amd-opti](https://github.com/Caliel666/neural-amd-opti) | RTGI original; porte do desbloqueio de multi-frame e pacing do XeFG | GPL-3.0 |
| **Coldwood1026** — [github.com/Coldwood1026/OptiScalerDp4aUnlock](https://github.com/Coldwood1026/OptiScalerDp4aUnlock) | XeFGUnlock / teto do libxell | GPL-3.0 |
| **RenoDX (clshortfuse)** — [github.com/clshortfuse/renodx](https://github.com/clshortfuse/renodx) | matemática RenoDRT / ACES da iluminação | MIT |
| **Nukem9** — [github.com/Nukem9/dlssg-to-fsr3](https://github.com/Nukem9/dlssg-to-fsr3) | dlssg-to-fsr3 | GPL-3.0 |
| **DLSS Enabler (Artur Graniszewski)** — [github.com/artur-graniszewski/DLSS-Enabler](https://github.com/artur-graniszewski/DLSS-Enabler) | `dlss-enabler-headless.dll` (gerador multi-frame pelo nvngx) | termos do autor |
| **OptiPatcher** — [github.com/optiscaler/OptiPatcher](https://github.com/optiscaler/OptiPatcher) | plugin `.asi` que libera DLSS sem spoof | MIT |
| **AMD** (FidelityFX), **Intel** (XeSS / XeFG / XeLL), **NVIDIA** (Streamline, cabeçalhos NGX), **Microsoft** (DirectX) | SDKs e binários redistribuíveis | licenças próprias |
| **NVIDIA** | o desenho da rede DLSS 5 e os pesos | — |

Este é um trabalho derivado e **modificado** do OptiScaler e do AMDNR; as linhas de copyright dos arquivos de cada
autor foram mantidas.

## Licença

O código do fork é **GPL-3.0** (`LICENSE`), com os termos adicionais do AMDNR para o material dele
(`inteiro/Licenses/AMDNR_NOTICE.txt`). Componentes de terceiros seguem as próprias licenças, na mesma pasta.
