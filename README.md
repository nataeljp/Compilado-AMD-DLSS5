# DLSS5 Overlay

A rede **DLSS 5 (neural rendering)** rodando em placas AMD RDNA 4 sobre **qualquer jogo**, por captura de vídeo: o
programa captura a janela do jogo, passa a imagem pela rede e mostra o resultado num overlay por cima. Na captura de
vídeo (o padrão) **nada é injetado no jogo** (sem OptiScaler, sem AMDNR, sem DLL no processo do jogo). A captura de
buffer, opcional, coloca um módulo pequeno no jogo aberto para pegar cor sem HUD, profundidade e movimento do upscaler
dele (ver *Modo de Rede*).

## Como usar

Para rodar basta a pasta **`bin`**: `DLSS5Overlay.exe` e `runtime\` (runtime HIP v3 do lmxxf + adaptador + pesos);
`config.json`, `painel.ini` e `overlay.jsonl` (log) são criados ao lado do exe. O resto do projeto é fonte e ferramentas.
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

## Online e anti-cheat

Na captura de vídeo o programa não injeta código no jogo: ele lê a janela pelo mesmo caminho que o OBS e a Barra de
Jogo usam e desenha numa janela própria. Não há como garantir a política de cada jogo; quem decide é o anti-cheat de cada
um. A captura de buffer coloca código no jogo e por isso é recusada com anti-cheat ativo; use-a só offline.

## Compilar

`build.ps1` (Visual Studio 2022 Build Tools, Windows SDK 10.0.26100) gera `bin\DLSS5Overlay.exe` com o ícone de
`recursos\` ("NR" em fundo vermelho); `monta_runtime.ps1` monta `bin\runtime` com o runtime v3 (pesos exatos em FP16
gravados como `.f16`: metade do tamanho, saída idêntica bit a bit; pesos do histórico extraídos do `nvngx_dlssnr` 310.8);
`empacota.ps1` gera o zip de distribuição. `ferramentas\compara_ref.py` mede o overlay contra a saída real da NVIDIA.
Gravações do jogo (Ctrl+Shift+G) podem ser reproduzidas com `--reproduz <pasta> [--atraso N]` (N = a resposta da rede
entra N quadros depois, para medir o modo assíncrono); `ferramentas\compara_atraso.py` e `cintilacao.py` medem o custo
do atraso e a cintilação. `ferramentas\gera_sintetico.py` gera uma sequência com movimento exato conhecido e
`avalia_sintetico.py` mede erro e cintilação contra a referência sem ruído (a média das respostas alinhadas).

## Créditos e licenças

- Rede e runtime HIP: **lmxxf** (MIT) — `licencas/LICENSE.lmxxf`. Pesos da rede: NVIDIA (DLSS 5), convertidos do
  `nvngx_dlssnr.dll`.
- Interface: **Dear ImGui** (MIT) — `licencas/LICENSE.imgui.txt`.
- Composição de referência e gradação dos Models B/C conforme a análise publicada em
  **zmodelerlover/dlss5-neural-amd** (MIT, `docs/nvidia-parity.md`, `docs/styles-model-abc.md`); ACES de Stephen Hill como
  no **RenoDX** de clshortfuse (MIT). Implementação própria.
- Fluxo óptico inspirado no **iMMERSE Launchpad** (MartysMods): mesma ideia, nenhum código dele.
