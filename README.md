# DLSS5 Overlay

A rede **DLSS 5 (neural rendering)** rodando em placas AMD RDNA 4 sobre **qualquer jogo**, por captura de vídeo: o
programa captura a janela do jogo, passa a imagem pela rede e mostra o resultado num overlay por cima. Na captura de
vídeo (o padrão) **nada é injetado no jogo** (sem OptiScaler, sem AMDNR, sem DLL no processo do jogo). A captura de
buffer, opcional, coloca um módulo pequeno no jogo aberto para pegar cor sem HUD, profundidade e movimento do upscaler
dele (ver *Modo de Rede*).

## Como usar

O pacote pronto fica na aba **Releases** (zip). Para rodar basta a pasta dele (ou `bin\` depois de compilar):
`DLSS5Overlay.exe`, `runtime\` (runtime HIP v4 do lmxxf + adaptador + pesos), `captura\DLSS5Buffer.dll` (captura de
buffer) e `ml\` (classificador da máscara). `config.json`, `painel.ini` e `overlay.jsonl` (log) são criados ao lado do
exe. O resto do repositório é fonte e ferramentas.
Ao abrir aparece na hora a tela "NR" com a barra de carregamento. A primeira abertura depois de cada atualização compila
os shaders (todos em paralelo) e guarda o resultado em `cache\shaders`; as aberturas seguintes leem do cache.

1. Rode o jogo em **janela** ou **tela cheia sem borda** (tela cheia exclusiva não pode ter overlay por cima).
2. Abra `DLSS5Overlay.exe`. O painel (no estilo do *DLSS 5 Developer Controls* da NVIDIA) fica encostado na direita da
   tela. Deixe o jogo em foco e aperte **Ctrl+Shift+D**, ou escolha a janela em **Jogo** e clique em **Ligar**.
3. Como a Game Bar: overlay e painel só aparecem com o jogo em foco. Alt+Tab, minimizar ou outra janela na frente os
   escondem e pausam a rede; ao voltar ao jogo eles voltam. O overlay acompanha a janela ao mover e redimensionar.
4. **Perfil por jogo**: *Salvar perfil* grava os ajustes em `perfis\<exe>.json`, que carrega sozinho quando o overlay
   liga no mesmo exe. *Redefinir* volta aos padrões; *Salvar como padrão* grava `config.json` (jogos sem perfil). Ao
   sair, os ajustes vão para o perfil ativo (ou para `config.json`).
5. O programa fica na **bandeja do sistema** (ícones ocultos da barra de tarefas): clique no ícone mostra/esconde o
   painel; botão direito → **Sair** fecha. Fechar o painel (×, Esc) só esconde.

### Modo de Rede: captura de vídeo ou de buffer

- **Captura de vídeo** (padrão): lê a janela como o OBS. Funciona em qualquer jogo, inclusive online; a rede vê o HUD
  e o movimento é estimado por fluxo óptico.
- **Captura de buffer**: com o jogo aberto, Ctrl+Shift+D coloca o módulo `captura\DLSS5Buffer.dll` no processo do jogo
  (nada é gravado na pasta dele, nada para instalar). O módulo pega o que o jogo entrega ao upscaler dele (FSR 3.1+):
  a cor **sem HUD**, a **profundidade** e o **movimento exato**; a rede e toda a cadeia continuam no overlay, e a
  edição volta para o próprio jogo, que a soma no quadro seguinte (antes do tonemap e do HUD). Com movimento e
  profundidade de verdade, o acúmulo descarta só onde há oclusão, a grade separa as camadas pela distância e a edição
  só é reprojetada para a mesma superfície. Medido numa sequência com objeto em primeiro plano, contra a captura de
  vídeo: cintilação −45% (0,110 → 0,061), −86% no objeto em movimento, fundo 2,4× mais estável.
  - Requisitos: jogo DirectX 12 com **FSR ligado no menu gráfico**; o painel mostra o estado (conectado, esperando o
    FSR, recusado).
  - **Anti-cheat**: com o anti-cheat ativo (EasyAntiCheat, BattlEye, EA AntiCheat, Vanguard...) a conexão é recusada
    (colocar código no jogo pode dar banimento). Jogo com anti-cheat instalado mas aberto sem ele: conecta, com aviso —
    use só offline.
  - OptiScaler instalado na pasta do jogo assume o upscaler: tire o `dxgi.dll` dele para a captura de buffer.
  - A rede responde 1–2 quadros depois; o jogo soma a edição pelo movimento acumulado de todos os quadros desde o
    quadro que a rede viu (nenhum quadro pula sem o movimento dele ser contado: era o "liga/desliga"), e só para a
    mesma superfície (profundidade). Onde um objeto acabou de sair, a edição vem dos vizinhos do fundo no quadro atual.
    Validado com movimento extremo (câmera até 335 px/quadro, objeto até 317 px/quadro): nenhum quadro sem edição fora
    de corte de cena e nenhuma textura do objeto no fundo descoberto (`ferramentas\fantasma_textura.py`: 0,00; uma
    cópia de 50% do objeto dá 0,98).
  - Painel sobre o jogo: jogos que leem o mouse por entrada bruta (Unreal) travam o cursor do Windows; o módulo pega o
    movimento bruto antes do jogo, manda ao painel e entrega zero ao jogo (a mira não mexe, o jogo não pausa).
  - "Mesma superfície" é decidida pela **distância radial** da câmera (o Z do jogo convertido com o FOV que ele passa ao
    FSR): o Z de um ponto fora do centro muda quando a câmera gira, e num giro rápido ~70% da tela parecia outra
    superfície — a edição vinha dos vizinhos na mesma distância e o letreiro claro era pintado de ciano no carro escuro
    (o verde/ciano e o rastro do Gears). Os vizinhos que preenchem fundo recém-descoberto precisam ter a mesma cor, e o
    conteúdo que entra pela borda num giro fica com o quadro original até a rede responder (antes: faixas esticadas).
    Medido numa sequência com a câmera girando em 3D até ~300 px/quadro sobre um quadro HDR do Gears
    (`gera_sintetico_buffer.py --giro`): pixels com ciano espúrio na imagem final 3,1% → 1,5%, sem faixas nem cópias.
  - Diagnóstico: `captura\DLSS5Buffer.log` registra a cada 5 s os despachos do FSR (por segundo), a resolução de render,
    resets, trocas de saída, jitter, e do lado do overlay quantos quadros foram exportados e receberam edição, com a
    idade média e máxima da edição em quadros.

### Pele e objetos (máscara)

Como o *Developer Masking* + *Show Masks* do painel da NVIDIA. Painel → **PELE E OBJETOS (MÁSCARA)**: *Separar pele e
objetos* liga o classificador, e cada grupo (pele, cabelo, personagem, objetos) recebe **quanto do DLSS 5 entra nele**
(1 = o efeito inteiro da rede, 0 = o quadro original). É o *ControlMask* do binário da NVIDIA: o último passo mistura,
por pixel, o quadro e a resposta da rede (`lerp(original, rede, máscara)`), com a rede rodando uma vez só.

- **A rede já acha a pele sozinha.** O controle de personagem dentro da rede é o *UseAutoMask* da NVIDIA (Avançado →
  Máscara interna da rede), e é por isso que a rede atua mais no personagem que no cenário — a NVIDIA faz igual. Medido
  contra a saída real da NVIDIA (Lara, 1080p): rede achando a pele sozinha **45,8 dB** (45,5 no personagem); dizer à
  rede, pixel a pixel, onde é personagem 44,1 dB (`mascaraExplicita` 1); e ainda tirar dele o canal de cenário 43,2 dB
  (`mascaraExplicita` 2). Por isso a máscara não entra na rede por padrão: ela só decide quanto do efeito fica em cada
  grupo.
- Avançado → **Ajuste por grupo dentro da rede (experimental)**: nosso, não é da NVIDIA. Estrutura, tom e modelo
  (A/B/C) próprios por grupo, nos canais de controle da primeira camada da rede (tom, estrutura, pele, outro, estilo),
  que passam a valer por pixel (runtime v4, `LmxxfNrSetMask`). A rede mistura cada pixel com os vizinhos: numa região
  pequena (a pele) o ajuste rende ~metade do que rende na tela inteira. "Pele realista" = Model C, estrutura 0 e
  tom −0,5 na pele e no cabelo.
- **O que é cada coisa** (classificador em `ml\`, ONNX Runtime + DirectML na mesma placa, numa thread própria — o
  quadro nunca espera): um detector de pessoas (YOLOX-tiny) e a segmentação da cena (LR-ASPP) acham os personagens; no
  recorte de cada um, um modelo de partes (MediaPipe multiclass) separa a silhueta, a pele do rosto e do corpo, o cabelo
  e a roupa/armadura; objetos (veículos, móveis, plantas) vêm da cena. Nunca por cor.
- **Rastreio**: a classificação chega alguns quadros depois e é trazida até o quadro atual pelo movimento acumulado
  (exato na captura de buffer, fluxo óptico no vídeo); entre uma classificação e outra a máscara segue o movimento. A
  borda segue a cor e a **profundidade** do quadro; fundo recém-descoberto volta a ser cenário; o que entra na tela herda
  da mesma superfície (profundidade igual).
- Medido com movimento extremo (câmera até 336 px/quadro, personagem recortado até 317 px/quadro,
  `ferramentas\avalia_mascara.py`): personagem coberto (cobertura 1,00), rastro da máscara 0,04, IoU 0,85; a imagem
  final segue sem fantasma (`fantasma_textura` −0,001, igual à máscara desligada).
- Custo: ~1 ms por quadro na GPU; a classificação (~30 ms) roda numa thread, de 2 a 15 vezes por segundo
  (*Classificações/s*, padrão 6).
- Com todos os grupos em 1 e sem ajuste dentro da rede (ou a máscara desligada) a saída é idêntica byte a byte à de
  antes.

| Atalho | Ação |
|---|---|
| Ctrl+Shift+D | liga/desliga no jogo em primeiro plano |
| Ctrl+Shift+N | modo: rede → original → comparação → só a edição → movimento |
| Ctrl+Shift+S | estilo 0 → 1 → 2 |
| Ctrl+Shift+P | mostra/esconde o painel (sobre o jogo, abre **sem tirar o foco**: o jogo não pausa e o mouse fica no painel até Esc) |
| Ctrl+Shift+G | grava os próximos quadros em bruto (`bin\gravacoes\`), para análise |
| Ctrl+Shift+Q | sai |

Requisitos: placa AMD RDNA 4 (RX 9070 / 9060) e driver AMD com `amdhip64_7.dll` (os drivers atuais trazem), Windows 10
2004 ou mais novo (Windows 11 recomendado: sem a borda amarela da captura).

## Como funciona

1. **Captura** (Windows Graphics Capture) da área cliente da janela; em monitor com HDR, captura em FP16 scRGB.
2. **Entradas simuladas sem buffer do jogo**: a rede recebe a cor; os vetores de movimento saem de um fluxo óptico
   próprio entre os quadros capturados (pirâmide de luminância, busca do grosso ao fino, mediana vetorial), no espírito
   do iMMERSE Launchpad (MartysMods) — implementação própria. Duas técnicas comprovadas em celular dão a precisão que
   segura o acúmulo em movimento: **refinamento de Lucas-Kanade** em resolução cheia (o núcleo do DIS/KLT) e um
   **candidato de movimento global** (afim robusto, como na estabilização de vídeo), que desfaz o casamento errado em
   textura repetitiva. Medido contra movimento exato: cintilação da edição −63% (0,196 → 0,073; ideal 0,063).
3. **Rede DLSS 5** (runtime HIP do lmxxf) no tamanho escolhido (720p a 2160p), numa **fila própria da GPU**: o quadro
   capturado vai para a tela na hora, sem esperar a rede (sem atraso de entrada, no ritmo do jogo); a resposta entra
   quando fica pronta, trazida pelo movimento acumulado desde o quadro que a rede viu.
4. **Composição**: a *edição* da rede (resposta − entrada) é acumulada no tempo num espaço comprimido (x / (1 + luma),
   como o TAA dos jogos, para o realce HDR não virar pontos), reprojetada pelo movimento simulado (menos cintilação) e
   somada ao quadro nativo, que mantém o próprio detalhe. Onde a edição não pode ser trazida (conteúdo animado, oclusão,
   borda nova da tela) ela sai de uma grade bilateral da última resposta (bloco × brilho). Composição de cor
   *Clássica* ou no *estilo RenoDX* (luz e cor controladas separadamente).
5. **Overlay** DirectComposition, transparente a cliques e sem foco: o jogo continua recebendo mouse e teclado.

## Parâmetros (config.json / painel)

Os parâmetros do DLSS 5 seguem os nomes que a NVIDIA expõe no NGX (os mesmos do addon RenoDX DLSS):

| Chave | Faixa | NGX / o que faz |
|---|---|---|
| `modelo` | 0/1/2 | `DLSSNR.Style`: Model A (padrão), B (natural), C (cinemático) — controle de estilo da rede + gradação final da NVIDIA |
| `intensidade` | 0..2 | `DLSSNR.Intensity`: quanto da resposta entra (acima de 1 reforça a luz) |
| `tomLocal` | 0..1 | `DLSSNR.LocalToneStrength`: luz e cor em grande escala; também escala a gradação do modelo |
| `estruturaLocal` | −1..2 | `DLSSNR.LocalStructureStrength` |
| `estruturaPele` | −1..2 | `DLSSNR.SkinStructureStrength` (com a máscara ligada) |
| `mascara` | true/false | `DLSSNR.UseAutoMask`: separa personagem e cena |
| `gradacao` | true/false | aplica a gradação do Model B/C (exposição, contraste, saturação do kernel da NVIDIA) |
| `composicao` | 0/1 | 0 crua (resposta − proxy), 1 referência NVIDIA/RenoDX (razão de luz em dois ramos, piso 1/512, proteção, cor) |
| `corComposicao`, `protecao` | 0..1, 1..8 | mistura de cor e proteção bilateral de luz da composição (padrão 1 e 2,0) |
| `ponte` | −1..2 | quadro → proxy da rede: −1 automática (SDR direta, HDR SoftKnee), 0 direta, 1 SoftKnee, 2 ACES (RRT+ODT, RenoDX) |
| `brancoNits` | 0..400 | branco de papel do HDR (0 = o branco SDR do Windows) |
| `alturaRede` | 720..2160 | altura em que a rede roda (limitada pela janela) |
| `redeCompleta` | true/false | os 71 blocos (camadas) da rede; false pula 42/43/46 (~1 ms) |
| `passes` | 1..3 | quantas vezes a rede inteira roda por quadro (1 = normal) |
| `movimento`, `usaConfianca` | true/false | movimento simulado por fluxo óptico; descartar o acúmulo onde ida e volta não conferem |
| `estabilidade`, `rejeicao` | 1..16, 2..40 | quadros que a edição acumula; quão rápido mudança de cor derruba o acúmulo |
| `ritmo` | 1..4 | 1 = a rede recomeça assim que termina; N = no máximo uma vez a cada N quadros (libera GPU para o jogo) |
| `reusoVit` | true/false | reuso adaptativo do ViT do runtime lmxxf (vale ao reabrir) |
| `forca`, `modo`, `fpsMax`, `ocultaDeGravacoes` | | força da edição final, modo de exibição, teto de FPS, esconder de prints/OBS |
| `mascaraDev`, `mostraMascara`, `taxaMascara` | | pele e objetos: liga, pinta os grupos (Show Masks), classificações por segundo (2..15) |
| `grupoNDlss` | 0..1, N = 1..4 | pele, cabelo, personagem, objetos: quanto do DLSS 5 entra no grupo (o ControlMask da NVIDIA) |
| `grupoNEstrutura`, `grupoNTom`, `grupoNModelo` | N = 1..4 | experimental, dentro da rede: estrutura e tom a mais/menos que o global; modelo −1 = global, 0/1/2 = A/B/C |
| `mascaraExplicita` | 0/1/2 | 0 = a rede acha a pele sozinha (padrão, o da NVIDIA); 1/2 = o personagem marcado por pixel (medido pior) |

## Online e anti-cheat

Na captura de vídeo o programa não injeta código no jogo: ele lê a janela pelo mesmo caminho que o OBS e a Barra de
Jogo usam e desenha numa janela própria. Não há como garantir a política de cada jogo; quem decide é o anti-cheat de cada
um. A captura de buffer coloca código no jogo e por isso é recusada com anti-cheat ativo; use-a só offline.

## Compilar

Precisa: Visual Studio 2022 com C++ (e Windows SDK 10.0.26100), git, Python 3 com numpy (pesos e ferramentas) e,
para gerar o pacote de pesos, o `nvngx_dlssnr.dll` 310.8 da NVIDIA (vem com jogos que trazem o DLSS 5).

```
runtime\lmxxf\compilar.ps1 -Modulos   # lmxxf (commit a9b7a52) + patch -> LmxxfNrRuntime_v4.dll e os 7 modulos HIP da mascara
runtime\adaptador\compilar.ps1        # adaptador LmxxfNrRuntime.dll (API do AMDNR por cima do lmxxf)
build.ps1                             # bin\DLSS5Overlay.exe
build_buffer.ps1                      # bin\captura\DLSS5Buffer.dll e obj\JogoTeste.exe
monta_runtime.ps1                     # bin\runtime a partir de dependencias\ (ou -Pesos, -DllNvidia, -DllPequena)
empacota.ps1 -Versao X                # zip para a aba Releases
```

- `dependencias\` (fora do git, cada um monta a sua): `pesos\DLSS5-AMD` (o pacote de pesos convertidos do
  `nvngx_dlssnr`) e `nvidia\nvngx_dlssnr_310.8.dll` / `nvngx_dlssnr_310.9.16.dll` (as DLLs da NVIDIA, só leitura).
- `monta_runtime.ps1` monta `bin\runtime`: os pesos do pacote DLSS5-AMD (a pasta com `native-game-tiled-assets`), com
  os pesos exatos em FP16 gravados como `.f16` (metade do tamanho, saída idêntica bit a bit), o runtime v4, os pesos do
  histórico extraídos do `nvngx_dlssnr` 310.8 e, com `-DllPequena`, a rede pequena da 310.9.16. Nada é escrito por cima
  de hardlink: os pesos vão por hardlink e o resto é copiado.
- `bin\ml`: `onnxruntime.dll` do pacote NuGet `Microsoft.ML.OnnxRuntime.DirectML` 1.24.4, `DirectML.dll` do
  `Microsoft.AI.DirectML` 1.15.4, `deteccao.onnx` = `yolox_tiny.onnx` do release 0.1.1rc0 do YOLOX, e `cena.onnx` /
  `partes.onnx` por `ferramentas\exporta_classificadores.py` (o `.tflite` é o `selfie_multiclass_256x256` do MediaPipe).

Ferramentas de medida: `ferramentas\compara_ref.py` mede o overlay contra a saída real da NVIDIA. Gravações do jogo
(Ctrl+Shift+G) podem ser reproduzidas com `--reproduz <pasta> [--atraso N]` (N = a resposta da rede entra N quadros
depois, para medir o modo assíncrono); `compara_atraso.py` e `cintilacao.py` medem o custo do atraso e a cintilação.
`gera_sintetico.py` gera uma sequência com movimento exato conhecido e `avalia_sintetico.py` mede erro e cintilação
contra a referência sem ruído. Captura de buffer: `gera_sintetico_buffer.py` (`--pessoa`, `--laco`) e
`obj\JogoTeste.exe` (um jogo D3D12 que chama o FSR 3.1 com a sequência; `--render 0.59` imita o UE5 com escala de
resolução: movimento e profundidade na resolução de render, movimento em uv), com `fantasma_textura.py` e
`avalia_mascara.py`.

### Estrutura

| Pasta | O que tem |
|---|---|
| `src\` | o overlay (captura, fluxo óptico, rede, composição, máscara, painel) |
| `src\buffer\`, `src\jogo\` | protocolo da captura de buffer e o módulo `DLSS5Buffer.dll` (gancho do FSR 3.1 no jogo) |
| `runtime\lmxxf\` | o patch das mudanças no runtime do lmxxf (sobre o commit a9b7a52) e o script que clona e compila |
| `runtime\adaptador\` | o adaptador `LmxxfNrRuntime.dll` |
| `ferramentas\` | pesos (FP16, histórico, rede pequena), medidas contra a NVIDIA, sequências sintéticas, jogo de teste |
| `externo\` | Dear ImGui, Detours, cabeçalhos do FidelityFX, ONNX Runtime e DirectML, conversor de pesos do guentra |
| `licencas\`, `recursos\` | licenças de terceiros; ícone |

## Créditos e licenças

- Este projeto: GPL-3.0 (`LICENSE`). Os componentes de terceiros mantêm as licenças deles (abaixo e em `licencas\`).
- Rede e runtime HIP: **lmxxf** (MIT) — `licencas/LICENSE.lmxxf`. Pesos da rede: NVIDIA (DLSS 5), convertidos do
  `nvngx_dlssnr.dll`.
- Conversor de pesos `externo/guentra/convert_dll.py`: **guentra/dlss5-amd-hip-linux** (MIT) — `licencas/LICENSE.guentra`.
- Interface: **Dear ImGui** (MIT) — `licencas/LICENSE.imgui.txt`. Captura de buffer: **Microsoft Detours** (MIT) e
  cabeçalhos do **FidelityFX SDK** (MIT) — `licencas/LICENSE.detours.txt`, `licencas/LICENSE.fidelityfx.md`.
- Composição de referência e gradação dos Models B/C conforme a análise publicada em
  **zmodelerlover/dlss5-neural-amd** (MIT, `docs/nvidia-parity.md`, `docs/styles-model-abc.md`); ACES de Stephen Hill como
  no **RenoDX** de clshortfuse (MIT). Implementação própria.
- Fluxo óptico inspirado no **iMMERSE Launchpad** (MartysMods): mesma ideia, nenhum código dele.
- Máscara de desenvolvedor (`ml\`): **ONNX Runtime** (MIT) com **DirectML** (Microsoft); modelos **LR-ASPP** do
  torchvision (BSD-3), **MediaPipe** Selfie Multiclass (Apache-2.0) e **YOLOX-tiny** (Apache-2.0) — `licencas/MODELOS_MASCARA.txt`.
  `ferramentas\exporta_classificadores.py` refaz `cena.onnx` e `partes.onnx`; `deteccao.onnx` é o ONNX publicado pelo YOLOX.
