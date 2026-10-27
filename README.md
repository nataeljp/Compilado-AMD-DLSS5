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
- Acima do orçamento (pico do jogo, jogo pesado) o painel só avisa: o Windows move o excedente para a RAM e a rede
  continua (antes ela era desligada de vez e desligava em todo pico).
- Os pesos da rede já são FP8 (1 byte, como a NVIDIA os entrega). Medido na imagem de referência: INT4 só nos pesos
  (metade da memória dos pesos) cai para 25,9 dB contra a NVIDIA (o nível de "sem rede": cor esverdeada, rosto
  borrado); INT8 tem o mesmo tamanho do FP8; BF16 o dobro.
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
  - **Geração de quadros (AMD)**: a geração do FidelityFX SDK 1.1.4 (Frame Interpolation + Optical Flow + o swapchain
    de geração da AMD, compilados do código fonte, MIT), ligada como o provedor de referência da AMD: quadros novos de
    verdade entre os do jogo, com o movimento, a profundidade e a câmera que o jogo manda ao FSR. A interpolação parte do
    backbuffer final, então os quadros gerados já levam a edição da rede. A DLL tem de entrar antes do DirectX: ao
    ligar a geração com o jogo conectado, o overlay copia a `DLSS5Buffer.dll` para a pasta do executável do jogo como
    `version.dll` — ou `dxgi.dll` —, uma DLL que o executável importa (lida da tabela de importação dele), então o Windows
    a carrega antes do DirectX em toda abertura; as funções da DLL do Windows de mesmo nome seguem para ela por
    `src\jogo\ProxyDxgi.asm`. Como `version.dll` ela engancha `CreateDXGIFactory*` na dxgi do Windows (já carregada, ou
    pelo aviso de carga do carregador). O overlay pede para reabrir o jogo **uma vez**; daí em diante o jogo já abre com
    ela (pela Steam ou como for), o swapchain dele vira o da AMD na criação, o overlay conecta sozinho (modo buffer) e a
    geração liga, desliga e muda só pelo painel. A DLL de outro programa (Cyber Engine Tweaks usa `version.dll`; ReShade
    e OptiScaler, `dxgi.dll`) nunca é substituída; "Remover do jogo" manda a nossa para a Lixeira; overlay atualizado
    atualiza a cópia do jogo ao conectar (e uma instalação antiga com o outro nome vai para a Lixeira). Os shaders da AMD e os do módulo compilam em segundo plano (no
    primeiro quadro do FSR a compilação parava o jogo por 14–25 s). Medido no jogo de teste com a rede ligada: 27
    quadros do jogo + 27 gerados por segundo, cada gerado exatamente entre os dois reais (24,6 dB para cada lado).
    Desligue a geração do próprio jogo e o AFMF do driver.
  - **3x e 4x** (a AMD só faz 2x): `externo\ffx_multiquadro.py` (aplicado pelo `externo\compila_ffx.ps1`, idempotente)
    troca nos shaders da interpolação o meio fixo (t = 0,5) pelo instante de cada gerado (t = k/N, na constante que era
    `_pad1`; os vetores até o quadro atual valem mv·(1−t) e até o anterior mv·t), a interpolação roda uma vez por gerado
    (só a última guarda o quadro atual como anterior) e o swapchain apresenta até 3 gerados por quadro do jogo, em ordem,
    com o intervalo do jogo dividido por N. Com t = 0,5 as contas são as da AMD: o 2x não muda. O módulo escreve no log o
    ritmo real das apresentações (intervalo médio, desvio, maior e o **1% low**: o intervalo que só 1% das apresentações
    passa, por um histograma de 0,1 ms no presenter da AMD) e o painel mostra, com a geração ligada, os quadros do jogo,
    os da tela (reais + gerados) e o 1% low dos últimos 5 s. Medido no sample da AMD a 3x: 116 q/s na tela, 1% low ~100.
  - **Preparo e geração em threads diferentes**: o preparo da AMD roda na thread de render (despacho do FSR) e o fluxo
    óptico + a interpolação no Present — no TLOU2, outra thread. Os dois usam o mesmo backend da AMD (fila de trabalhos e
    recursos dinâmicos do contexto da interpolação, que não são seguros entre threads) e agora passam um de cada vez por
    uma trava; com o contexto sendo refeito (tela ou render máximo novos) a geração do Present não roda, e antes de
    destruir o contexto a geração é desligada no swapchain (a configuração espera o Present em curso). O log conta as
    vezes em que um esperou o outro ("preparo e geração se encontraram N vezes").
  - **Cada Present com o próprio quadro**: o preparo (profundidade e movimento dilatados) é gravado no despacho do FSR e a
    AMD gera no Present. Um motor com a thread de render à frente grava o quadro seguinte antes do Present do atual — casar
    pelo último preparado juntava a imagem de um quadro com o movimento do seguinte (gerado no lugar errado: fantasma e o
    movimento indo e voltando). Os preparos ficam numa fila (3 jogos de recursos) e cada Present usa o mais novo já
    **enviado** à GPU (gancho de `ExecuteCommandLists` desde o primeiro quadro). Medido no jogo de teste `--adiantado`
    (grava à frente, envia em ordem): a DLL nova gera o mesmo quadro que o motor normal; a antiga, outro. O log diz
    quantos quadros o jogo grava à frente. Com o **NVIDIA Streamline** no jogo (`sl.interposer.dll`, como no TLOU2) as
    listas de comandos são embrulhadas: o jogo entrega a nativa ao FSR (como a NVIDIA recomenda) e envia a embrulhada,
    e as duas nunca casavam (no TLOU2: todas as apresentações sem o preparo do próprio quadro). As duas pontas viram a
    nativa (`QueryInterface` com o `StreamlineRetrieveBaseInterface` do Streamline). As 120 primeiras apresentações com
    geração vão para o log, uma a uma: os despachos do FSR desde a anterior, a fila e qual quadro cada uma usou.
  - **HUD parado nos gerados**: interpolado junto com a cena, o HUD andava com ela. A AMD resolve com a imagem sem HUD
    (`currentBackBuffer_HUDLess`: onde o quadro final difere dela é HUD, e o gerado leva o HUD do quadro final, parado;
    o fluxo óptico também parte dela, como no FSR 3). O jogo não a entrega: o módulo a acha pelo método do HUDfix do
    OptiScaler (`src\jogo\SemHud.cpp`) — segue os descritores das texturas do tamanho e do formato da tela, da criação
    (`Create*View`, `CopyDescriptors*`) ao uso (alvo de desenho + desenho; tabela de compute, pelas faixas de UAV da root
    signature, + despacho), numera os passes do jogo depois do upscaler e copia o escolhido logo depois dele. A escolha é
    conferida na GPU a cada apresentação, contra o quadro final, em 1 de cada 16 pixels: a certa é igual a ele bit a bit
    fora do HUD (≥ 70% da tela, mas não 100% — essa já tem HUD) e, com a cena andando, a máscara do que difere fica
    parada; a de antes de um pós-efeito difere na tela toda (grão, cor) ou nas bordas, que andam com a cena (nitidez).
    Fica o primeiro passe que confere; só a conferida vai para a AMD. Medido (o HUD dos gerados contra o do quadro
    real): sample da AMD a 4K, SDR e HDR10 em tela cheia, passe 0 de 3, 100% igual (sem a imagem: 64%, até 204/255 de
    diferença); jogo de teste `--hud` (tonemap → HUD; com `--pos grao` ou `--pos nitidez` entre eles): o passe certo nos
    três casos, 100% igual, instantes 0,333/0,669, nenhuma mensagem da camada de validação do D3D12. `DLSS5_SEM_HUD=0`
    desliga (teste A/B).
  - **Limite pela taxa do monitor**: a tela nunca recebe mais quadros por segundo que a taxa do monitor (acima dela cada
    atualização mostra pedaços de vários quadros). Desligada, a geração liga quando o jogo entrega até 75% da taxa, com
    o menor N que enche a tela; ligada, o N só sobe (o jogo caindo) e nunca cai sozinho — o limitador abaixo e o do
    próprio jogo distorcem a medida, e o N ia e voltava. Com N gerados o jogo é limitado a 97% da taxa ÷ N (relógio de
    alta resolução + espera ativa, antes da trava). Os gerados ignoram o vsync do jogo: com ele cada um esperava um
    vblank do compositor a 60 Hz e o jogo caía pela metade. A taxa é a física do sinal (`QueryDisplayConfig` com `QDC_VIRTUAL_REFRESH_RATE_AWARE`): com a taxa dinâmica do
    Windows 11 a área de trabalho fica "virtual" a 60 Hz e a tela segue a 120 — `EnumDisplaySettings` só via os 60 e a
    geração saía desligada num monitor de 120 Hz com FreeSync. Lida pelo vigia, fora da thread de render.
  - **Só no executável do jogo**: na pasta do jogo a DLL é carregada por todo programa dali (o relatório de erros
    `crs-*.exe` do TLOU2, lançadores). O módulo só age nos executáveis listados em `DLSS5Overlay.alvo` (escrito pelo
    overlay na instalação); nos outros ela só repassa a DLL do Windows, sem ganchos nem log.
  - **Swapchain só do jogo**: o módulo não segura o swapchain da AMD; quando o jogo o solta (modo de tela, HDR, saída) a
    AMD avisa antes de destruir (`ffxDlss5AoDestruirDX12`) e o contexto sai junto — preso, ele ocupava a janela (a
    criação do novo falhava) e na saída as threads da AMD morriam com ele vivo.
  - **Saída do processo**: carregada da pasta do jogo, a DLL entra antes do d3d12 e sai depois do driver. Os globais com
    objetos do D3D12 (o cliente do overlay, os shaders compilados) nunca são destruídos — o destrutor deles chamava o
    driver que já tinha saído e o `amdxc64.dll` abortava o processo (`0xc0000409`) a cada fechamento do jogo.
  - **Diagnóstico no log** (`DLSS5Buffer.log` ao lado da DLL): a cada 5 s o maior intervalo entre quadros e os engasgos
    (> 2x a média), o ritmo das apresentações e quantos quadros o jogo grava à frente; o **vigia** grava onde cada thread
    está parada se os quadros param por 4 s ou a janela trava (módulo + deslocamento; os da DLL saem pelo
    `DLSS5Buffer.map` gerado na compilação); acessos inválidos (até 5) com a pilha.
  - **Dispositivo removido**: o TLOU2 para (de propósito: escreve o código em 0x0) com `DXGI_ERROR_INVALID_CALL` quando
    `GetDeviceRemovedReason` diz que o dispositivo caiu por uma chamada inválida. O módulo liga o DRED da Microsoft
    antes de o jogo criar o dispositivo (o último comando de cada lista e a falha de página da GPU) e confere o
    dispositivo depois de cada `ExecuteCommandLists`: o envio que o derrubou vai para o log (fila, listas — se é a do
    FSR, com os passes do módulo — e quem enviou), com o DRED; as listas que o runtime recusa ao fechar também.
    Reproduzido no jogo de teste `--invalida Q` (envia uma lista sem fechar): o mesmo `0x887a0001`, e o log aponta a
    lista e quem a enviou.
  - **HUD e legendas que o jogo entrega ao FSR 3 dele**: um jogo com geração FSR 3 manda a UI à parte — numa textura
    que o swapchain compõe, num callback que a desenha em cada quadro, ou como a imagem sem HUD. O swapchain de geração
    do jogo fica por fora do nosso e compunha essa UI antes de passar o quadro: ela chegava na imagem e era interpolada
    com a cena (no TLOU2, HUD e legendas tremendo nos gerados; a nossa procura da imagem sem HUD não acha passe nenhum
    nele). Com a nossa gerando, o módulo tira a UI do swapchain do jogo e a entrega ao nosso, que a compõe parada em cada
    quadro, real e gerado, como a geração do jogo faria — automático, o log diz qual dos três o jogo usa. Medido no
    sample da AMD com a geração dele ligada (o mesmo arranjo do TLOU2), câmera girando, a procura desligada: o módulo
    antigo deixava 42% dos pixels da UI diferentes nos gerados; agora 0,1% (callback), 0,1% (textura), 0,0% (imagem sem
    HUD); em 4x 0,4–0,9% (só a UI semitransparente, onde a cena aparece por trás). Sem UI à parte vale a procura.
    A imagem sem HUD (o modo do TLOU2) é copiada para uma textura nossa no Present do swapchain de geração do jogo, na
    fila dele — depois do quadro inteiro e antes do seguinte; lida depois, na fila da interpolação, ela já podia ser a
    do quadro seguinte. É a da última configuração, como o swapchain da AMD faz no Present. Sem a cópia, a procura da
    imagem sem HUD volta a valer (antes o HUD ficava interpolado com a cena: legenda borrada e ícone da arma dobrado
    nos gerados da gravação do TLOU2); o log diz a cada 5 s quantas foram copiadas e por que não.
  - **O pedido do painel fica**: o último pedido de geração do painel vale até ele mandar outro. Antes, com o overlay
    parado (gravação segurando o laço dele, rede recriada ao mudar um ajuste, desligado e religado) a geração caía para
    a do lançamento por 1–2 s, a do jogo voltava e o HUD e o ritmo pulavam. Medido: overlay desligado 8 s no meio do
    sample, a geração seguiu em 3x.
  - **Gravação (Ctrl+Shift+G) com a geração**: o módulo grava junto, na pasta da gravação, um quadro gerado — os
    gerados, o real, a imagem sem HUD e os quadros como foram para a tela (`tela_*`, com a UI composta).
  - **N que desce por prova**: preso num N alto (no TLOU2 a rede a 4K deixou o jogo a 13 q/s, o N foi a 4 e o
    limitador o segurou em 29 q/s depois de ele poder fazer ~50 — mais atraso de entrada à toa), um N a menos é tentado
    quando o trabalho do jogo diz que ele enche a tela com 5% de folga; em 3 s a tela cheia confirma, senão volta e a
    próxima prova espera o dobro (10 s a 160 s). Medido no jogo de teste `--fps 20 --fps-depois 300 50`: 4x → 3x (jogo
    de 20 a 39 q/s); a prova de 2x não encheu e voltou em 1 s.
  - **A geração FSR 3 do próprio jogo**: com a do jogo ligada e o painel pedindo 2x–4x, a dele é desligada e a nossa
    liga — só quando a nossa pode gerar (o swapchain da AMD é o nosso: a DLL entrou antes do DirectX; e o contexto está
    pronto). Injetado com o jogo já aberto a nossa não gera, e desligar a do jogo o deixava sem nenhuma; agora a do jogo
    segue (medido: sample da AMD com a geração dele, injeção tardia, 75 s ligada). O pedido do painel só muda a decisão
    depois de 1 s igual (no TLOU2 alternava 0x/4x várias vezes por segundo com o overlay religando).
  - O jogo espera a edição do quadro anterior (1 quadro, como a NVIDIA roda o DLSS 5: a rede dentro do tempo do quadro)
    e a soma pelo movimento acumulado desde o quadro que a rede viu (nenhum quadro pula sem o movimento dele ser
    contado: era o "liga/desliga"), e só para a
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
  - No jogo, a DLL aplica a edição do quadro anterior com a mesma oclusão reconstruída (cada ponto deste quadro marca
    a distância dele onde estava no quadro da edição); antes ela só comparava a distância gravada na edição e deixava
    passar a edição do personagem no chão perto dele.
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
| `modelo` | 0/1/2 | `DLSSNR.Style`: Model A (padrão), B (natural), C (cinemático) — controle de estilo da rede + gradação final da NVIDIA |
| `intensidade` | 0..2 | `DLSSNR.Intensity`: quanto da resposta entra (acima de 1 reforça a luz; na composição de referência) |
| `tomLocal` | 0..1 | `DLSSNR.LocalToneStrength`: luz e cor em grande escala; também escala a gradação do modelo |
| `estruturaLocal` | −1..2 | `DLSSNR.LocalStructureStrength` |
| `estruturaPele` | −1..2 | `DLSSNR.SkinStructureStrength` (com a pele separada pela rede) |
| `mascara` | true/false | `DLSSNR.UseAutoMask`: a rede separa a pele do resto da cena |
| `gradacao` | true/false | aplica a gradação do Model B/C (exposição, contraste, saturação do kernel da NVIDIA) |
| `composicao` | 0/1 | 0 crua (resposta − proxy), 1 referência NVIDIA/RenoDX (razão de luz em dois ramos, piso 1/512, proteção, cor) |
| `corComposicao`, `protecao` | 0..1, 1..16 | mistura de cor e proteção bilateral de luz da composição (padrão 1 e 16 = desligada) |
| `matizVermelhoSat` … `matizMagentaSat`, `matizVermelhoLuz` … `matizMagentaLuz` | −1..1 | cor por matiz depois da rede, em seis faixas (Vermelho, Amarelo, Verde, Ciano, Azul, Magenta): saturação (−1 = cinza, +1 = o dobro) e brilho (−1 = apaga, +1 = o dobro; pesa pela saturação do pixel, cinza e branco não mudam). Faixas vizinhas se misturam em triângulo. Padrão 0 = desligado |
| `ponte` | −1..4 | quadro → proxy da rede: −1 automática (SDR direta, HDR SoftKnee), 0 direta, 1 SoftKnee, 2 ACES (RRT+ODT, RenoDX), 3 curva do jogo, 4 luz de cena direta (só na captura de buffer: só o ombro do codificador da NVIDIA comprime os realces; na captura de vídeo vira a automática) |
| `brancoNits` | 0..400 | branco de papel do HDR (0 = o branco SDR do Windows) |
| `alturaRede` | 720..2160 | altura em que a rede roda (limitada pela janela); vale ao religar |
| `redeCompleta` | true/false | os 71 blocos (camadas) da rede; false pula 42/43/46 (~1 ms); vale ao religar |
| `passes` | 1..3 | quantas vezes a rede inteira roda por quadro (1 = normal); vale ao religar |
| `profundidade` | 0..4 | blocos da rede grande: 0 = 71, 1 = 68 (sem 42/43/46), 2 = 63 (sem 40–47), 3 = 55 (e sem 23–30), 4 = 47 (e sem o ViT 31–38; só com `reusoVit` desligado). Só blocos residuais saem; muda na hora; só no `config.json`. Não se aplica à rede pequena, que é outro modelo |
| `movimento` | true/false | movimento simulado por fluxo óptico (as guias do histórico da própria rede na captura de vídeo) |
| `geracaoQuadros` | 0/2/3/4 | captura de buffer: geração de quadros da AMD (0 desligada, 2 = 2x .. 4 = 4x); a DLL vai para a pasta do jogo (version.dll ou dxgi.dll) |
| `ritmo`, `quadroGerado` | 1..4, 0/1 | só na captura de vídeo: **quadros gerados** — a rede edita 1 de cada N quadros e os N−1 seguintes são gerados dos processados (nunca um quadro novo com edição velha): 0 repetido (na captura de buffer o processado aparece 1 quadro depois), 1 mesclado (os N quadros de cada ciclo misturam só o processado atual e o próximo, peso do próximo em passos iguais: 2x 50/50, 3x 25/50/75%, 4x 20/40/60/80%; sempre 1 quadro de atraso) |
| `reusoVit` | true/false | reuso adaptativo do ViT do runtime lmxxf (vale ao reabrir) |
| `temporalRede` | 0..3 | histórico da própria rede (`DLSS5_TEMPORAL_MODE` do runtime; vale ao reabrir e desliga o `reusoVit`; os passes 2–3 valem com ele — a rede nasce com 1 e o adaptador aplica os passes pela chamada direta `LmxxfNrSetPasses`, e só o último passe recebe o histórico). 1 = a entrada de histórico da rede recebe a resposta anterior trazida pelo movimento (captura de buffer: movimento e profundidade do jogo; captura de vídeo: fluxo óptico) e a cabeça de histórico do bloco 70 decide a mistura; precisa do runtime deste branch (`LmxxfNrSetHistoryGuides`) e do `post70-history-head.f16` que o `monta_runtime.ps1` extrai. 2 e 3 = suavização de baixa frequência / realce na saída do runtime |
| `forcaHistoricoRede` | 0..2 | com `temporalRede` 1: escala do peso que a própria rede dá à resposta anterior (força × 0,74 × a decisão da cabeça de histórico, teto 0,95); 1 = o da rede; padrão 2 = a decisão dela dobrada. Medido nos quadros do TLOU2 com a câmera parada: a saída da rede oscila 9% menos (a edição, 5%), com a régua de movimento igual. É o máximo da acumulação da NVIDIA ali: sem as guardas de cor/profundidade do runtime ganha só mais 1% e começa a deixar rastro; o resto da cintilação a cabeça de histórico não segura |
| `recorteHistoricoRede` | 0..4 | com `temporalRede` 1: a resposta anterior só entra dentro de média ± este número de desvios da saída crua deste quadro no 3×3 (YCoCg), o recorte por variância do TAA, feito no runtime antes da mistura (`LmxxfNrSetHistoryClip`). Sem ele, o que a rede pinta em volta de um objeto em movimento ficava no fundo que passa atrás dele e andava com esse fundo, uma cópia por quadro da rede (o rastro em degraus). 0 = sem recorte; padrão 1 |
| `forca` | 0..2 | força da edição final (1 = a da rede) |
| `nitidezCas` | 0..1 | nitidez depois da composição pelo algoritmo do CAS (AMD FidelityFX, MIT), só no lado da rede; em HDR em c / (1 + c) por canal. Na captura de buffer vai junto com a edição que volta ao jogo. 0 = desligada (padrão), 1 = o máximo do CAS |
| `modo`, `fpsMax`, `ocultaDeGravacoes` | | modo de exibição (0 rede, 1 original, 2 comparação, 3 só a edição, 4 movimento, 5 saída crua da última resposta), teto de FPS, esconder de prints/OBS |

No painel os ajustes finos têm três casas decimais: Shift + arrastar anda 1/10 do normal, e a roda do mouse sobre o
deslizador soma 0,01 (0,001 com Shift). Clique no número para digitar o valor (Enter aplica, Esc cancela; funciona com
o jogo em foco). A seta no fim de cada ajuste (ou duplo clique no trilho) volta ao padrão.
As faixas de Tom, Estrutura e Intensidade são as da NVIDIA: medido na rede, fora delas ela inverte (estrutura acima de
2 e tom acima de 1 clareiam a imagem em vez de reforçar o efeito).

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
externo\compila_ffx.ps1               # bibliotecas do FidelityFX SDK 1.1.4 (geracao de quadros; o SDK extraido em externo\fidelityfx_sdk)
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
