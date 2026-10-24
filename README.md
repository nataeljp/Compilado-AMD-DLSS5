# DLSS5 Overlay

A rede **DLSS 5 (neural rendering)** rodando em placas AMD RDNA 4 sobre **qualquer jogo**, por captura de vídeo: o
programa captura a janela do jogo, passa a imagem pela rede e mostra o resultado num overlay por cima. Na captura de
vídeo (o padrão) **nada é injetado no jogo** (sem OptiScaler, sem AMDNR, sem DLL no processo do jogo). A captura de
buffer, opcional, coloca um módulo pequeno no jogo aberto para pegar cor sem HUD, profundidade e movimento do upscaler
dele (ver *Modo de Rede*).

## Como usar

O pacote pronto fica na aba **Releases** (zip). Para rodar basta a pasta dele (ou `bin\` depois de compilar):
`DLSS5Overlay.exe`, `runtime\` (runtime HIP v4 do lmxxf + adaptador + pesos) e `captura\DLSS5Buffer.dll` (captura
de buffer). `config.json`, `painel.ini` e `overlay.jsonl` (log) são criados ao lado do
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

### Memória de vídeo

- **A rede é fixa por sessão.** Tamanho, rede grande ou pequena, passes e rede completa valem do Ctrl+Shift+D até o
  próximo; mudar no painel vale ao religar (Ctrl+Shift+D duas vezes). O driver HIP da AMD nunca devolve os buffers que
  a ponte do runtime importa (`hip_d3d12_bridge.h` do lmxxf, cerca de 44 bytes por pixel da rede), então cada tamanho
  novo de rede prende memória até fechar o overlay; e trocar de rede recarrega os pesos com a GPU parada.
- O painel mostra a VRAM do overlay contra o orçamento que o Windows dá a ele, e o `overlay.jsonl` registra a cada 5 s
  (`"tipo":"memoria"`) o uso, o orçamento, a memória de vídeo na RAM e cada tamanho de rede novo.
- Se a VRAM do overlay passar do orçamento por 2 s, a rede é desligada de vez (uma travada só, em vez de o Windows
  mover a rede para a RAM e o fps cair sem voltar) e o painel avisa. Religar recria a rede.
- Desligar com Ctrl+Shift+D não solta a rede; para devolver toda a VRAM, feche o overlay (bandeja → Sair).
- Na captura de buffer, a DLL no jogo aloca movimento e profundidade pelo maior tamanho de render que o jogo declarou
  ao FSR: a resolução dinâmica não recria mais as texturas, e as gerações velhas só são soltas depois de a GPU terminar
  de usá-las.

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
  - Oclusão como no FSR (profundidade reconstruída): cada ponto deste quadro marca, no lugar onde estava no quadro
    anterior, a distância dele; o histórico de um pixel só cai se outro ponto **deste quadro**, mais perto da câmera,
    estava lá (fundo que acabou de aparecer). Comparar com a distância do quadro anterior derrubava o personagem
    inteiro: com a câmera andando em volta dele (2–3 m) a distância muda ~10% por quadro, o histórico saía e a edição
    dele virava estimativa atrasada (o rastro em personagem e objetos). Contra o quadro anterior (e no jogo, com a
    edição de 1–3 quadros atrás) só conta o que o movimento da câmera não explica. Medido com a câmera girando e
    andando (`--giro --anda`): confiança no objeto perto 0,46 → 0,997; a faixa recém-descoberta segue 100% detectada.
  - No jogo, a DLL aplica a edição de 1–3 quadros atrás com a mesma oclusão reconstruída (cada ponto deste quadro marca
    a distância dele onde estava no quadro da edição); antes ela só comparava a distância gravada na edição e deixava
    passar a edição do personagem no chão perto dele.
  - A média temporal da edição encurta com o movimento do pixel (metade a 8 px/quadro: `mediaMovimento`, que
    `DLSS5_ESTAB_PX` sobrepõe): a edição da rede depende do ângulo de vista e da animação, e 8 quadros de média em
    movimento eram a edição atrasada sobre a cena; parado, a média longa segura a cintilação.
  - Medido na gravação do Gears reproduzida fora do jogo (`--reproduz` de uma gravação do modo buffer usa o movimento e a
    distância gravados: o modo buffer inteiro, determinístico) contra a edição ideal (a resposta da rede reprojetada):
    resíduo no overlay 0,041 → 0,018 (0,040 → 0,027 perto de bordas de profundidade); na etapa do jogo, simulada com
    os dados gravados, a edição que chegava de algo que estava na frente 0,070 → 0.
  - Diagnóstico: `captura\DLSS5Buffer.log` registra a cada 5 s os despachos do FSR (por segundo), a resolução de render,
    resets, trocas de saída, jitter, e do lado do overlay quantos quadros foram exportados e receberam edição, com a
    idade média e máxima da edição em quadros.

### Pele (controles da NVIDIA)

A separação de pele, cabelo, personagem e objetos por classificador (YOLOX-tiny, LR-ASPP e MediaPipe) saiu. Ficam os
controles que a NVIDIA expõe, que a própria rede aplica. Painel → **PELE (NVIDIA)**:

- **Pele separada pela rede** é o *UseAutoMask* da NVIDIA: a rede acha a pele sozinha e separa do resto da cena. Medido
  contra a saída real da NVIDIA (Lara, 1080p), a rede achando a pele sozinha deu **45,8 dB**; dizer a ela, pixel a
  pixel, onde era personagem deu 44,1 dB e 43,2 dB.
- **Estrutura da pele** é o *SkinStructureStrength*: a estrutura só na pele que a rede achou. O resto da cena segue a
  *Estrutura local* (*LocalStructureStrength*).
- O *ControlMask* da NVIDIA (o *Developer Masking*) é uma máscara que o jogo entrega; jogos com FSR não entregam nada
  parecido, e ele não é imitado aqui.
- O runtime v4 continua com os controles por pixel (`LmxxfNrSetMask`), sem uso pelo overlay; desligados, a saída é a
  mesma.

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
| `intensidade` | 0..4 | `DLSSNR.Intensity`: quanto da resposta entra (acima de 1 reforça a luz) |
| `tomLocal` | 0..2 | `DLSSNR.LocalToneStrength`: luz e cor em grande escala; também escala a gradação do modelo (acima de 1 extrapola o que a NVIDIA expõe) |
| `estruturaLocal` | −2..4 | `DLSSNR.LocalStructureStrength` |
| `estruturaPele` | −2..4 | `DLSSNR.SkinStructureStrength` (com a pele separada pela rede) |
| `mascara` | true/false | `DLSSNR.UseAutoMask`: a rede separa a pele do resto da cena |
| `gradacao` | true/false | aplica a gradação do Model B/C (exposição, contraste, saturação do kernel da NVIDIA) |
| `composicao` | 0/1 | 0 crua (resposta − proxy), 1 referência NVIDIA/RenoDX (razão de luz em dois ramos, piso 1/512, proteção, cor) |
| `corComposicao`, `protecao` | 0..2, 1..16 | mistura de cor e proteção bilateral de luz da composição (padrão 1 e 16 = desligada) |
| `matizVermelhoSat` … `matizMagentaSat`, `matizVermelhoLuz` … `matizMagentaLuz` | −1..1 | cor por matiz depois da rede, em seis faixas (Vermelho, Amarelo, Verde, Ciano, Azul, Magenta): saturação (−1 = cinza, +1 = o dobro) e brilho (−1 = apaga, +1 = o dobro; pesa pela saturação do pixel, cinza e branco não mudam). Faixas vizinhas se misturam em triângulo. Padrão 0 = desligado |
| `ponte` | −1..4 | quadro → proxy da rede: −1 automática (SDR direta, HDR SoftKnee), 0 direta, 1 SoftKnee, 2 ACES (RRT+ODT, RenoDX), 3 curva do jogo, 4 luz de cena direta (captura de buffer: só o ombro do codificador da NVIDIA comprime os realces) |
| `brancoNits` | 0..400 | branco de papel do HDR (0 = o branco SDR do Windows) |
| `alturaRede` | 720..2160 | altura em que a rede roda (limitada pela janela); vale ao religar |
| `redeCompleta` | true/false | os 71 blocos (camadas) da rede; false pula 42/43/46 (~1 ms); vale ao religar |
| `passes` | 1..3 | quantas vezes a rede inteira roda por quadro (1 = normal); vale ao religar |
| `profundidade` | 0..4 | blocos da rede grande: 0 = 71, 1 = 68 (sem 42/43/46), 2 = 63 (sem 40–47), 3 = 55 (e sem 23–30), 4 = 47 (e sem o ViT 31–38; só com `reusoVit` desligado). Só blocos residuais saem; muda na hora. Não se aplica à rede pequena, que é outro modelo |
| `movimento`, `usaConfianca` | true/false | movimento simulado por fluxo óptico; descartar o acúmulo onde ida e volta não conferem |
| `estabilidade`, `rejeicao` | 1..16, 2..40 | quadros que a edição acumula; quão rápido mudança de cor derruba o acúmulo |
| `mediaMovimento` | 0..32 px | captura de buffer: a média cai pela metade a cada tantos px/quadro de movimento (padrão 8; 0 = fixa) |
| `ritmo` | 1..4 | 1 = a rede recomeça assim que termina; N = no máximo uma vez a cada N quadros (libera GPU para o jogo) |
| `reusoVit` | true/false | reuso adaptativo do ViT do runtime lmxxf (vale ao reabrir) |
| `forca` | 0..4 | força da edição final (1 = a da rede) |
| `modo`, `fpsMax`, `ocultaDeGravacoes` | | modo de exibição, teto de FPS, esconder de prints/OBS |

No painel os ajustes finos têm três casas decimais: Shift + arrastar anda 1/10 do normal, e a roda do mouse sobre o
deslizador soma 0,01 (0,001 com Shift). Duplo clique volta ao padrão.

## Online e anti-cheat

Na captura de vídeo o programa não injeta código no jogo: ele lê a janela pelo mesmo caminho que o OBS e a Barra de
Jogo usam e desenha numa janela própria. Não há como garantir a política de cada jogo; quem decide é o anti-cheat de cada
um. A captura de buffer coloca código no jogo e por isso é recusada com anti-cheat ativo; use-a só offline.

## Compilar

Precisa: Visual Studio 2022 com C++ (e Windows SDK 10.0.26100), git, Python 3 com numpy (pesos e ferramentas) e,
para gerar o pacote de pesos, o `nvngx_dlssnr.dll` 310.8 da NVIDIA (vem com jogos que trazem o DLSS 5).

```
runtime\lmxxf\compilar.ps1 -Modulos   # lmxxf (commit a9b7a52) + patch -> LmxxfNrRuntime_v4.dll e os 7 modulos HIP de entrada por pixel
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
Ferramentas de medida: `ferramentas\compara_ref.py` mede o overlay contra a saída real da NVIDIA. Gravações do jogo
(Ctrl+Shift+G) podem ser reproduzidas com `--reproduz <pasta> [--atraso N]` (N = a resposta da rede entra N quadros
depois, para medir o modo assíncrono); `compara_atraso.py` e `cintilacao.py` medem o custo do atraso e a cintilação.
`gera_sintetico.py` gera uma sequência com movimento exato conhecido e `avalia_sintetico.py` mede erro e cintilação
contra a referência sem ruído. Captura de buffer: `gera_sintetico_buffer.py` (`--pessoa`, `--laco`) e
`obj\JogoTeste.exe` (um jogo D3D12 que chama o FSR 3.1 com a sequência; `--render 0.59` imita o UE5 com escala de
resolução: movimento e profundidade na resolução de render, movimento em uv), com `fantasma_textura.py`.

### Estrutura

| Pasta | O que tem |
|---|---|
| `src\` | o overlay (captura, fluxo óptico, rede, composição, painel) |
| `src\buffer\`, `src\jogo\` | protocolo da captura de buffer e o módulo `DLSS5Buffer.dll` (gancho do FSR 3.1 no jogo) |
| `runtime\lmxxf\` | o patch das mudanças no runtime do lmxxf (sobre o commit a9b7a52) e o script que clona e compila |
| `runtime\adaptador\` | o adaptador `LmxxfNrRuntime.dll` |
| `ferramentas\` | pesos (FP16, histórico, rede pequena), medidas contra a NVIDIA, sequências sintéticas, jogo de teste |
| `externo\` | Dear ImGui, Detours, cabeçalhos do FidelityFX, conversor de pesos do guentra |
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
