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
4. **Perfis** (como os presets do ReShade), na seção **JOGO**, logo abaixo da escolha da janela: um arquivo por
   perfil, `perfis\<nome>.json`. Cada jogo tem o perfil dele — o nome do exe, ou o nome dado em **Renomear**
   (`perfis\_jogos.ini`, `exe=nome`) — carregado ao ligar e para onde vão os ajustes ao **Salvar**, ao sair e ao trocar
   de jogo. O seletor **Perfil** traz os ajustes de qualquer perfil (de qualquer jogo) para o jogo ligado; salvar
   continua indo para o perfil deste jogo, sem mexer no arquivo do outro. Jogo sem perfil parte do padrão do programa
   (`config.json`, o destino sem jogo). Apagar manda para a Lixeira. Os perfis do formato anterior
   (`perfis\<exe>\<nome>.json`) são migrados ao abrir; as pastas velhas ficam em `perfis\_antigos`.
5. O programa fica na **bandeja do sistema** (ícones ocultos da barra de tarefas): clique no ícone mostra/esconde o
   painel; botão direito → **Sair** fecha. Fechar o painel (×, Esc) só esconde.

### Memória de vídeo

- **A rede é fixa por sessão.** Tamanho, rede grande ou pequena, passes e rede completa valem do Ctrl+Shift+D até o
  próximo; mudar no painel vale ao religar (Ctrl+Shift+D duas vezes). O driver HIP da AMD nunca devolve os buffers que
  a ponte do runtime importa (`hip_d3d12_bridge.h` do lmxxf, cerca de 44 bytes por pixel da rede), então cada tamanho
  novo de rede prende memória até fechar o overlay; e trocar de rede recarrega os pesos com a GPU parada.
- O painel mostra a VRAM do overlay contra o orçamento que o Windows dá a ele, separada em rede/runtime e texturas do
  overlay, e o `overlay.jsonl` registra a cada 5 s (`"tipo":"memoria"`) o uso, o orçamento, a memória de vídeo na RAM e
  cada tamanho de rede novo.
- Aberto: no Cyberpunk, com a geração de quadros do FSR 4 ligada, a GPU pode travar (`0x887a0006`); o `DLSS5Buffer.log` diz qual
  recurso falhou.
- Acima do orçamento (pico do jogo, jogo pesado) o painel só avisa: o Windows move o excedente para a RAM e a rede
  continua (antes ela era desligada de vez e desligava em todo pico).
- Os pesos da rede já são FP8 (1 byte, como a NVIDIA os entrega). Medido na imagem de referência: INT4 só nos pesos
  (metade da memória dos pesos) cai para 25,9 dB contra a NVIDIA (o nível de "sem rede": cor esverdeada, rosto
  borrado); INT8 tem o mesmo tamanho do FP8; BF16 o dobro.
- Desligar com Ctrl+Shift+D não solta a rede; para devolver toda a VRAM, feche o overlay (bandeja → Sair).
- Mudar o tamanho da rede (`alturaRede` ou a resolução do jogo) recria a sessão dela, em 0,03 s: o driver HIP segura os buffers
  de todo tamanho que já rodou. Devolve só parte: em 720p depois de 2160p o overlay fica com 2,9 GB, contra 1,2 GB sem ter
  passado por 2160p, e não cresce a cada troca (19 trocas medidas). O resto volta ao fechar o overlay.
- Na captura de buffer, a DLL no jogo aloca movimento e profundidade pelo maior tamanho de render que o jogo declarou
  ao FSR: a resolução dinâmica não recria mais as texturas, e as gerações velhas só são soltas depois de a GPU terminar
  de usá-las. Com o overlay fechado ou desligado por 5 s, a DLL fica no jogo, mas a VRAM dela volta para ele; as texturas
  voltam quando o overlay voltar.

### Modo de Rede: captura de vídeo, de buffer ou da API

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
  - **Rede lenta (4K)**: acima de 95 ms por quadro (2160 com 2 passes: 119 ms) o jogo não espera a rede: roda solto, e a edição
    chega atrasada e reprojetada (medido no TLOU2: 16 quadros de idade, rastro). "Prender o jogo à rede lenta" faz ele esperar
    (prazo de 3x a média medida, de 100 a 900 ms): sem rastro, mas cada quadro leva o tempo da rede, com input lag. Abaixo de
    95 ms o jogo espera sempre a edição do quadro anterior. Fidelidade em tela 4K contra a saída da NVIDIA (1 passe; tempo de
    quadro no modo imagem): rede 1080 33,4 dB (28 ms), 1440 37,0 (47), 1620 38,0 (58), 1800 39,6 (68), 2160 45,85 (83).
  - **Dar GPU à rede**: `Limite de FPS do jogo` (captura de buffer e da API; DX11, DX12 e Vulkan) é um teto no present do jogo: o
    tempo de GPU que ele deixa de usar fica para a rede, sem atraso a mais na entrada. Medido com o jogo de teste em 4K, sem teto:
    rede 1440 com 1 passe a 10,4 fps; com teto 30, 17,4. No buffer com teto 30 e 1 passe: rede 2160 a 10,1 fps, 1440 a 18,0, 1080 a
    25,2 (sem teto, 2160 não chegou a 5 fps). A rede 2160 sozinha não passa de 10–16 fps com 1 passe: 20 fps só em tamanho menor.
    Prioridade de GPU não adianta nesta placa: `D3DKMTSetProcessSchedulingPriorityClass` (alta e tempo real, aceitas) e
    `hipStreamCreateWithPriority` deram os mesmos 10,3 fps de rede com a GPU cheia.
  - **Jogo no limite da VRAM**: acima do orçamento do processo, criar recurso falha (doc. do D3D12, *Residency*), e um jogo
    pesado usa tudo o que o orçamento deixa. As texturas da captura saem sem a residência na criação e com `MakeResident`,
    que passa acima do orçamento (o Windows tira do jogo o que ele usa menos); o log e o painel dizem quantas e, se ainda
    falhar, o código do erro e a VRAM do momento, e o módulo tenta de novo a cada 2 s. Em buffer, o que só a captura de
    vídeo usa sai da VRAM (swapchain e fluxo óptico).
  - **Jogo já aberto** é o caso normal, e a leitura dos buffers é igual: o módulo engancha a DLL do FSR que o jogo já
    carregou e lê cor, movimento e profundidade a cada despacho. Só as flags de criação do contexto do FSR ele não viu; as
    que importam saem dos próprios recursos: movimento na resolução de tela pelo tamanho das texturas, HDR pelo formato da
    cor, Z invertido medido no overlay, plano distante infinito pela câmera. O log diz "deduzidas".
  - **Módulo de versão anterior no jogo** (o jogo aberto antes de atualizar o overlay): o módulo não sai de um jogo aberto
    e os protocolos não conversam; o overlay avisa para fechar e abrir o jogo. O que versões antigas deixaram na pasta do
    jogo (`version.dll`, `dxgi.dll` com a nossa marca, `.antiga.*`, `.alvo`) vai para a Lixeira ao ligar; DLL de outro
    programa fica.
  - **Diagnóstico no log** (`DLSS5Buffer.log` ao lado da DLL): a cada 5 s o maior intervalo entre quadros e os engasgos
    (> 2x a média), o ritmo das apresentações e quantos quadros o jogo grava à frente; o **vigia** grava onde cada thread
    está parada se os quadros param por 4 s ou a janela trava (módulo + deslocamento; os da DLL saem pelo
    `DLSS5Buffer.map` gerado na compilação); acessos inválidos (até 5) com a pilha.
  - **Dispositivo removido**: o TLOU2 para (de propósito: escreve o código em 0x0) com `DXGI_ERROR_INVALID_CALL` quando
    `GetDeviceRemovedReason` diz que o dispositivo caiu por uma chamada inválida. O módulo confere o dispositivo depois de
    cada `ExecuteCommandLists`: o envio que o derrubou vai para o log (fila, listas — se é a do FSR, com os passes do
    módulo — e quem enviou); as listas que o runtime recusa ao fechar também. O DRED da Microsoft (o último comando de
    cada lista e a falha de página da GPU) só vale para dispositivo criado depois de o módulo entrar; com o jogo já aberto,
    o log traz o resto sem ele.
    Reproduzido no jogo de teste `--invalida Q` (envia uma lista sem fechar): o mesmo `0x887a0001`, e o log aponta a
    lista e quem a enviou.
- **Captura da API**: a mesma DLL no jogo, mas a cópia sai da apresentação do quadro (`IDXGISwapChain::Present`, o ponto
  comum do DirectX 11 e 12), com ou sem upscaler e em qualquer motor. A cópia é do back buffer: traz o HUD, não traz
  profundidade nem movimento (o movimento vem do fluxo óptico) e o resultado sai na janela do overlay, como na captura de
  vídeo. Ganha da captura de vídeo porque recebe o quadro assim que o jogo o termina, no formato dele.
  - Lê sRGB em 8 e 10 bits, HDR10 (PQ/BT.2020) e scRGB em ponto flutuante. Em 10 bits, o HDR10 é reconhecido pelo
    `SetColorSpace1` do jogo; se ele chamou antes do gancho, vale o estado do monitor.
  - O back buffer pode ter outro tamanho que a janela: o compositor estica o quadro.
  - No D3D12 a cópia vai na fila que desenhou o quadro, achada dentro do swapchain (o `GetDevice` recusa); sem achar, a
    última fila DIRECT que enviou listas na thread da apresentação.
  - Sem overlay por 1 s o módulo para de copiar; por 5 s, solta as texturas.
  - **Anti-cheat**: como na captura de buffer, a conexão é recusada.
  - **Vulkan**: marque "Vulkan" no painel e abra o jogo depois (o dispositivo não aceita as extensões de compartilhamento
    depois de criado). O overlay registra a camada `captura\DLSS5Camada.dll` em
    `HKCU\SOFTWARE\Khronos\Vulkan\ImplicitLayers` só enquanto está aberto, e tira o registro ao fechar. Nada vai para a
    pasta do jogo. Enquanto registrada, a DLL carrega nos programas Vulkan que abrirem nesse tempo: com anti-cheat ativo
    ela recusa a negociação com o carregador, e nos outros programas só copia quadros se o overlay ligar neles. Jogo
    rodando como administrador não enxerga a camada (o carregador ignora o `HKCU`).

### Formato da cor e da edição

Em Avançado → Composição, junto de Linear / Logarítmica / Mista, escolha entre qualidade e VRAM. Entre os formatos que aceitam escrita
em shader em todo hardware D3D12 ([tabela da Microsoft](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/hardware-support-for-direct3d-12-1-formats)):

| Opção | Para | Bits | O que muda |
|---|---|---|---|
| `R16G16B16A16_FLOAT` | HDR e SDR | 64 | o padrão: a maior qualidade |
| `R11G11B10_FLOAT + R8_UNORM` | HDR e SDR | 40 | cor sem alfa (a folga da ponte 3 vai no `R8`); edição em `R10G10B10A2_UNORM + R8_UNORM` |
| `R10G10B10A2_UNORM + R8_UNORM` | SDR | 40 | cor em 10 bits lineares; a mesma edição |
| `R16G16B16A16_UNORM` | SDR | 64 | cor sem float, edição como no padrão |
| `R8G8B8A8_UNORM` | SDR | 32 | cor em sRGB, igual à captura de 8 bits; edição em `R8G8B8A8_SNORM` |

Formato SDR em quadro com realce (HDR ou captura de buffer): a cor fica em `R16G16B16A16_FLOAT`, porque cortaria acima de 1; o painel
mostra o que está em uso. A edição tem sinal: nos formatos de 10 e 8 bits ela é comprimida com o zero exato, e pixel sem edição fica
intacto. Trocar refaz as texturas: a GPU para, as velhas saem e só então as novas entram, nos dois processos. A entrada da rede
(`orig`, `fed`) e a saída seguem em float16.

### Denoise (AMD FSR Ray Regeneration)

O denoise é o **AMD FSR Ray Regeneration 1.2.1** (FidelityFX SDK 2.4.0, MIT), o denoiser de machine learning da AMD. Exige RX 9000 ou mais nova, DirectX 12 e Windows 11. Roda no overlay, no tamanho da rede, e a rede não muda. Ele recebe o que o contrato da AMD pede (`ffx_denoiser.h` e o sample `denoiserrendermodule.cpp`), só com o que existe: a imagem editada em luz linear como a luz a filtrar e albedo 1, porque o jogo não entrega albedo e não há o que separar; material 0 (o manual manda começar assim); profundidade, perto, longe e FOV do jogo na captura de buffer, e a projeção do despacho é a mesma câmera que deu essa profundidade; o céu e o que não foi desenhado ficam no plano de longe e passam sem filtro pelos limites de profundidade do SDK, como no sample. O painel expõe a força e os parâmetros do SDK (normais, estabilidade, radiância máxima, recorte, gaussiana, desoclusão). Desoclusão é o limiar de profundidade que recomeça o histórico, não oclusão ambiente. O que o jogo não entrega não é inventado: rugosidade fica fixa, câmera identidade (o movimento dela está nos vetores), normais calculadas da profundidade. A profundidade vem do jogo: na captura de buffer a original, com o FOV e o movimento do jogo; na da API (DX11 e DX12) pelo buffer de profundidade dele, sempre o do mesmo quadro da cor, com escala relativa e fluxo óptico no movimento; em Vulkan e na captura de vídeo não há, e ela fica fixa. Buffer em formato que não dá para copiar (D24S8, ou D32S8 no DX11) fica sem profundidade, e o estado no painel diz o formato. Sem albedo do jogo, o filtro trata a textura como parte da luz: não sabe o que é tapete ou vidro. A visão de depuração é a da AMD e mostra o que o denoiser recebeu. Ele foi treinado para ruído de ray tracing, não para o grão da rede: o resultado depende do ajuste no jogo.

### Pele (controles da NVIDIA)

A separação de pele, cabelo, personagem e objetos por classificador (YOLOX-tiny, LR-ASPP e MediaPipe) saiu. Ficam os
controles que a NVIDIA expõe, que a própria rede aplica. Painel → **PELE (NVIDIA)**:

- **Pele separada pela rede** (*UseAutoMask*) liga o canal de pele da rede com a **Estrutura da pele**
  (*SkinStructureStrength*); desligada, o canal fica no padrão da NVIDIA e o resto da cena não muda (antes os canais de
  pele e de cena iam a −1 e o efeito da rede caía pela metade na imagem inteira).
- Medido canal por canal na imagem de referência: nenhum canal da rede separa a pele sozinho (cada um muda pele e
  cena quase igual) e o de pele tem efeito pequeno. A NVIDIA separa a pele com um modelo de máscara próprio, que não
  existe neste runtime.
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
3. **Rede DLSS 5** (runtime HIP do lmxxf) no tamanho escolhido (720p a 2160p), como a NVIDIA roda o DLSS 5: a rede vê
   o quadro e responde nele mesmo, e o quadro vai para a tela depois dela (custa FPS, como na NVIDIA). O modo que
   mostrava o quadro na hora e trazia a resposta atrasada pelo movimento foi removido: era o fantasma.
   O que a rede vê (as 16 entradas da primeira camada, `hip/prefix_fast.hip` do lmxxf): 3 canais de ruído, a cor (RGB),
   um segundo RGB que é a entrada de histórico (sem o histórico ligado ela recebe a cor de novo), 5 controles da NVIDIA,
   1 constante e 1 zerada. Não há entrada de luz, profundidade ou normal: na captura de buffer, o movimento e a
   profundidade do jogo chegam à rede só pelo histórico (`temporalRede`).
4. **Composição**: a *edição* da rede deste quadro (composição de referência NVIDIA/RenoDX − original) é somada ao
   quadro nativo, que mantém o próprio detalhe, com teto de brilho (4× o original + 4: a rede reilumina, não cria sol).
   A estabilidade no tempo é a do histórico da própria rede (`temporalRede`), como na NVIDIA.
5. **Overlay** DirectComposition, transparente a cliques e sem foco: o jogo continua recebendo mouse e teclado.

## Parâmetros (config.json / painel)

Os parâmetros do DLSS 5 seguem os nomes que a NVIDIA expõe no NGX (os mesmos do addon RenoDX DLSS):

| Chave | Faixa | NGX / o que faz |
|---|---|---|
| `modelo` | 0/1/2 | `DLSSNR.Style`: Model A (padrão), B (natural), C (cinemático) — controle de estilo da rede + gradação final da NVIDIA. Contra a foto Zorah da NVIDIA (DLSS 5 ligado) o mais próximo é o C: realces do rosto (p90 de L\*) 0,0 com a rede 2160 e +0,7 com a 1080, contra −5,1 do A (o A escurece a pele e mantém o grão dela); erro regional 8,7 C, 11,7 A |
| `intensidade` | 0..2 | `DLSSNR.Intensity`: quanto da resposta entra (acima de 1 reforça a luz; na composição de referência) |
| `tomLocal` | 0..1 | `DLSSNR.LocalToneStrength`: luz e cor em grande escala; também escala a gradação do modelo |
| `estruturaLocal` | −1..2 | `DLSSNR.LocalStructureStrength` |
| `estruturaPele` | −1..2 | `DLSSNR.SkinStructureStrength` (com a pele separada pela rede) |
| `mascara` | true/false | `DLSSNR.UseAutoMask`: a rede separa a pele do resto da cena |
| `gradacao` | true/false | aplica a gradação do Model B/C (exposição, contraste, saturação do kernel da NVIDIA) |
| `composicao` | 0/1 | 0 crua (resposta − proxy), 1 referência NVIDIA/RenoDX (razão de luz em dois ramos, piso 1/512, proteção, cor) |
| `corComposicao`, `protecao` | 0..1, 1..16 | mistura de cor e proteção bilateral de luz da composição (padrão 1 e 16 = desligada) |
| `luz` | 0/1/2 | como a edição chega à tela e ao jogo: 0 linear (diferença somada), 1 logarítmica (razão multiplicada), 2 mista (padrão). No mesmo pixel as três dão o mesmo resultado. Na ampliação da rede para a tela a mista é a logarítmica (o pixel da tela é a mesma superfície do pixel da rede; Zorah, rede 1080 numa tela 4K contra a rede 2160: 32,8 dB log, 31,5 linear, 30,2 somando). Para o quadro seguinte do jogo (captura de buffer), clarear soma a luz e escurecer multiplica: levada pelo movimento, a razão de um fio de cabelo escuro que a rede clareou 10× multiplicava o céu claro ao lado (o grão branco na borda do cabelo); somada, a mesma luz vira o halo dela. Medido levando a edição ao quadro seguinte em 4 reproduções 2160p do TLOU2: erro 2,9–3,6× menor que a logarítmica, pintas claras na borda do cabelo de 0,09–2,34% para 0–0,25%; na cena 3D do jogo de teste a luz média fica igual (0,590 log, 0,587 mista) |
| `matizVermelhoSat` … `matizMagentaSat`, `matizVermelhoLuz` … `matizMagentaLuz` | −1..1 | cor por matiz depois da rede, em seis faixas (Vermelho, Amarelo, Verde, Ciano, Azul, Magenta): saturação (−1 = cinza, +1 = o dobro) e brilho (−1 = apaga, +1 = o dobro; pesa pela saturação do pixel, cinza e branco não mudam). Faixas vizinhas se misturam em triângulo. Padrão 0 = desligado |
| `ponte` | −1..4 | quadro → proxy da rede: −1 automática (SDR direta, HDR SoftKnee), 0 direta, 1 SoftKnee, 2 ACES (RRT+ODT, RenoDX), 3 curva do jogo, 4 luz de cena direta (só na captura de buffer: só o ombro do codificador da NVIDIA comprime os realces; na captura de vídeo vira a automática) |
| `brancoNits` | 0..400 | branco de papel do HDR (0 = o branco SDR do Windows) |
| `alturaRede` | 720..2160 | altura em que a rede roda (limitada pela janela); vale ao religar |
| `redeCompleta` | true/false | os 71 blocos (camadas) da rede; false pula 42/43/46 (~1 ms); vale ao religar |
| `passes` | 1..3 | quantas vezes a rede inteira roda por quadro (1 = normal); vale ao religar |
| `profundidade` | 0..4 | blocos da rede grande: 0 = 71, 1 = 68 (sem 42/43/46), 2 = 63 (sem 40–47), 3 = 55 (e sem 23–30), 4 = 47 (e sem o ViT 31–38; só com `reusoVit` desligado). Só blocos residuais saem; muda na hora; só no `config.json`. Não se aplica à rede pequena, que é outro modelo |
| `movimento` | true/false | movimento simulado por fluxo óptico (as guias do histórico da própria rede na captura de vídeo) |
| `reusoVit` | true/false | reuso adaptativo do ViT do runtime lmxxf (vale ao reabrir) |
| `temporalRede` | 0..3 | histórico da própria rede (`DLSS5_TEMPORAL_MODE` do runtime; vale ao reabrir e desliga o `reusoVit`; os passes 2–3 valem com ele — a rede nasce com 1 e o adaptador aplica os passes pela chamada direta `LmxxfNrSetPasses`, e só o último passe recebe o histórico). 1 = a entrada de histórico da rede recebe a resposta anterior trazida pelo movimento (captura de buffer: movimento e profundidade do jogo; captura de vídeo: fluxo óptico) e a cabeça de histórico do bloco 70 decide a mistura; precisa do runtime deste branch (`LmxxfNrSetHistoryGuides`) e do `post70-history-head.f16` que o `monta_runtime.ps1` extrai. 2 e 3 = suavização de baixa frequência / realce na saída do runtime |
| `forcaHistoricoRede` | 0..2 | com `temporalRede` 1: escala do peso que a própria rede dá à resposta anterior (força × 0,74 × a decisão da cabeça de histórico, teto 0,95); 1 = o da rede; padrão 2 = a decisão dela dobrada. Medido nos quadros do TLOU2 com a câmera parada: a saída da rede oscila 9% menos (a edição, 5%), com a régua de movimento igual. É o máximo da acumulação da NVIDIA ali: sem as guardas de cor/profundidade do runtime ganha só mais 1% e começa a deixar rastro; o resto da cintilação a cabeça de histórico não segura |
| `recorteHistoricoRede` | 0..4 | com `temporalRede` 1: a resposta anterior só entra dentro de média ± este número de desvios da saída crua deste quadro no 3×3 (YCoCg), o recorte por variância do TAA, feito no runtime antes da mistura (`LmxxfNrSetHistoryClip`). Sem ele, o que a rede pinta em volta de um objeto em movimento ficava no fundo que passa atrás dele e andava com esse fundo, uma cópia por quadro da rede (o rastro em degraus). 0 = sem recorte; padrão 1 |
| `forca` | 0..2 | força da edição final (1 = a da rede) |
| `limiteFpsJogo` | 0..240 | teto de FPS do jogo, no present (captura de buffer e da API): o que ele deixa de usar de GPU fica para a rede. 0 = sem teto |
| `dnRegenForca` | 0..1 | força do Ray Regeneration; os parâmetros do SDK ficam em `dnRegenNormal`, `dnRegenEstabilidade`, `dnRegenRadiancia`, `dnRegenRecorte`, `dnRegenGaussiana`, `dnRegenOclusao` (este é o limiar de desoclusão; o nome da chave fica para não perder o valor salvo) |
| `dnCas` | 0..1 | CAS da AMD (MIT) na imagem final, depois do denoise (0 = desligado) |
| `dnRecorte` | 0,5..3 | quanto o histórico pode se afastar do quadro atual, em desvios da vizinhança |
| `dnBordasPasses`, `dnBordasSigma` | 1..3, 0,5..4 | alcance (2, 4, 8 px) e tolerância de luminância do filtro de bordas |
| `modo`, `fpsMax`, `ocultaDeGravacoes` | | modo de exibição (0 rede, 1 original, 2 comparação, 3 só a edição, 4 movimento, 5 saída crua da última resposta), teto de FPS, esconder de prints/OBS |

No painel os ajustes finos têm três casas decimais: Shift + arrastar anda 1/10 do normal, e a roda do mouse sobre o
deslizador soma 0,01 (0,001 com Shift). Clique no número para digitar o valor (Enter aplica, Esc cancela; funciona com
o jogo em foco). A seta no fim de cada ajuste (ou duplo clique no trilho) volta ao padrão.
As faixas de Tom, Estrutura e Intensidade são as da NVIDIA: medido na rede, fora delas ela inverte (estrutura acima de
2 e tom acima de 1 clareiam a imagem em vez de reforçar o efeito).

## Online e anti-cheat

Na captura de vídeo o programa não injeta código no jogo: ele lê a janela pelo mesmo caminho que o OBS e a Barra de
Jogo usam e desenha numa janela própria. Não há como garantir a política de cada jogo; quem decide é o anti-cheat de cada
um. As capturas de buffer e da API colocam código no jogo e por isso são recusadas com anti-cheat ativo; use-as só offline.

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
(Ctrl+Shift+G) podem ser reproduzidas com `--reproduz <pasta>`; `cintilacao.py` mede a cintilação.
`gera_sintetico.py` gera uma sequência com movimento exato conhecido e `avalia_sintetico.py` mede erro e cintilação
contra a referência sem ruído. Captura de buffer: `gera_sintetico_buffer.py` (`--pessoa`, `--laco`) e
`obj\JogoTeste.exe` (um jogo D3D12 que chama o FSR 3.1 com a sequência; `--render 0.59` imita o UE5 com escala de
resolução: movimento e profundidade na resolução de render, movimento em uv), com `fantasma_textura.py`. Mais perto de um
jogo: `--como-jogo --cubo --fsr4 --res 7680 4320 --render 0.5` (cena 3D, FSR 4 em Desempenho com jitter, 4K → 8K; o
carregador e o upscaler da AMD ao lado do exe), `--grao`, `--tempo` e `--recorte` (grava o meio da saída para
`avalia_denoise.py`). Com `--fps` baixo ele mede o tempo de GPU por quadro: o limite dos testes pesados.

### Estrutura

| Pasta | O que tem |
|---|---|
| `src\` | o overlay (captura, fluxo óptico, rede, composição, painel) |
| `src\buffer\`, `src\jogo\` | protocolos das capturas de buffer e da API e o módulo `DLSS5Buffer.dll` (ganchos do FSR 3.1+ e do `Present` no jogo) |
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
