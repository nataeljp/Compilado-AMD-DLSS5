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
- **Resolução livre da rede, de 720p a 4K**: o runtime lmxxf roda o tamanho exato pedido
  (`DLSS5_NETWORK_FREE_RES`), sem os três degraus 720/900/1080 nem o teto de 1920x1080.
  - O tamanho é **fixo**, escolhido numa lista (igual ao quadro, 720p, 900p, 1080p, 1440p, 2160p) com só as
    resoluções que cabem no display, na proporção do quadro.
  - Ele nunca muda durante o jogo, porque cada troca recria a rede e trava um instante.
  - Pelas medições do lmxxf, 1440p custa cerca de 1,7x e 4K cerca de 3,6x a rede em 1080p.
- **TSR da rede DLSS5** (aba DLSS5, cartão "Resolução e TSR da rede"): o acúmulo temporal do TSR da UE5, escrito do zero
  a partir das etapas que a Epic descreve publicamente (nenhum código da Unreal), aplicado na **edição da rede**, não no
  quadro do jogo. O upscaler do jogo (FSR4 etc.) continua o mesmo e o quadro dele fica intacto.
  - A edição é medida na resolução da rede, contra o que a rede recebeu. Antes, a resposta esticada substituía o quadro
    4K, que era o borrado.
  - A entrada da rede recebe um jitter subpixel diferente a cada alimentação (Halton 2,3). A edição de cada resposta é
    acumulada na resolução de saída e converge para ela com o tempo, que é o "feed" do TSR: vazão = pixels da rede ×
    respostas/s.
  - Cada quadro tem:
    - histórico reprojetado (Catmull-Rom);
    - heurística de paralaxe pela profundidade;
    - rejeição de sombreamento cor crua contra cor crua, com tolerância ao ruído aprendida por pixel;
    - detecção de cintilação que conta em tempo, não em quadros, e por isso funciona igual com FreeSync/VRR.

    Onde o histórico é rejeitado, a edição some em vez de deixar rastro.
  - O TSR leva a edição da rede até o quadro: amplia se a rede é menor, reduz se é maior.
  - **Ajuste automático medido**: a cada quadro a GPU mede o atraso do histórico contra a resposta nova da rede
    (rastro e borrado), a rejeição, a cintilação e o movimento.
    - O histórico conta em tempo (amostras = tempo × respostas/s × cobertura), então FPS variando (FreeSync, VSync)
      não muda o rastro.
    - O controle ajusta sozinho o tempo de histórico, o recorte, a desoclusão e a proteção contra cintilação.
    - Uma preferência vai de nitidez e menos rastro até estabilidade e anti-aliasing.
  - **Ritmo da rede**: todo quadro, **alvo de vazão** (padrão 124,4 MP/s, a referência da Epic) ou **alvo de FPS**.
    Com o tamanho fixo, os alvos escolhem em quantos quadros a rede roda; o TSR acumula no tempo, então nada é
    recriado.
  - **Ressurreição de histórico**: grava dois quadros persistentes a cada 31 quadros. Onde o histórico é descartado,
    a edição volta se a vizinhança da cor crua bate. O TSR da Epic reprojeta esses quadros pelas matrizes da câmera,
    que o jogo não entrega ao upscaler; aqui a comparação é no mesmo lugar da tela, então ajuda com a câmera parada.
  - O histórico próprio da rede recebe a troca de jitter junto com os vetores.
- **Rejeição estilo TSR no arrasto da edição** (com o TSR da rede desligado): a edição só é carregada onde a cor crua
  do quadro dela bate com a deste. É o "Edit accumulation" do AMDNR, também sem Model interleave.
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

1. Baixe o `.zip` em **Releases** (ou use a pasta `inteiro/`) e copie **todo o conteúdo** para a pasta do executável
   do jogo (a pasta onde está o `.exe` que roda o jogo; em Unreal é `...\Binaries\Win64`). São 4 itens:
   ```
   dxgi.dll          o OptiScaler deste compilado
   OptiScaler.ini    as configurações
   OptiScaler\       FidelityFX, XeSS, Streamline, plugins (OptiPatcher) e licenças
   DLSS5-AMD\        a rede: runtime (LmxxfNrRuntime.dll + lmxxf_upstream.dll), pesos e kernels
                     (native-game-tiled-assets\) e os shaders da iluminação (experimental_lighting\)
   ```
   Uma instalação antiga, com essas DLLs, os `plugins` e o `experimental_lighting` soltos na pasta do jogo, também
   continua funcionando.
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
5. **TSR da rede** (`source/optiscaler/OptiScaler/dlssnr/lmxxf/EditTsr.h`): os dois passes (integra a resposta,
   reprojeta e compõe). As ligações ficam em `LmxxfBackend.cpp`: a edição na resolução da rede (`ResidualShader`, modo
   2), o jitter da alimentação (`BlitShader`) e a troca de jitter no histórico da rede (`MotionChainShader`). O tamanho
   livre está em `lmxxf/LmxxfTierPolicy.h` (`FreeNetworkSize`), e a flag do runtime é definida em `dllmain.cpp`.
6. **Menu** (`source/optiscaler/OptiScaler/menu/lmxxf2_menu.inl` e `menu_common.cpp`).

## Créditos

Cada um pela sua parte (licenças completas em `inteiro/OptiScaler/Licenses`):

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
(`inteiro/OptiScaler/Licenses/AMDNR_NOTICE.txt`). Componentes de terceiros seguem as próprias licenças, na mesma pasta.
