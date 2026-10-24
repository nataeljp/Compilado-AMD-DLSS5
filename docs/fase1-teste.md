# Fase 1: compilar e testar

Branch `projecao-fase1`. Muda só o overlay (`DLSS5Overlay.exe`) e o módulo do jogo (`DLSS5Buffer.dll`); o runtime da
rede (`runtime\lmxxf`, `runtime\adaptador`) e os pesos ficam como estão.

| Commit | O que muda |
|---|---|
| remove a máscara por classificador | Sai a separação de pele e objetos por YOLOX-tiny, LR-ASPP e MediaPipe (`bin\ml`). Fica a seção **PELE (NVIDIA)**: a rede acha a pele sozinha (*UseAutoMask*) e o ajuste fino é a *Estrutura da pele* (*SkinStructureStrength*). |
| rede fixa por sessão e vigia de VRAM | Tamanho, rede grande ou pequena, passes e rede completa valem ao religar. VRAM do overlay no painel e no log; acima do orçamento por 2 s a rede é desligada. |
| teto de fps com timer de alta resolução | O `fpsMax` deixa de usar `sleep_for`, que acordava até ~15 ms depois do pedido. |
| sem recriar texturas com resolução dinâmica | A DLL do jogo aloca pelo tamanho máximo de render e solta gerações velhas só depois de a GPU terminar. |

O código foi checado num ambiente Linux com MinGW (sintaxe do C++) e com `glslangValidator` (os shaders HLSL), mas
**não foi compilado com o MSVC nem rodado**. O build abaixo é o primeiro de verdade.

## 1. Guardar o exe atual (para comparar depois)

No PowerShell, na pasta do repositório:

```powershell
Copy-Item bin\DLSS5Overlay.exe bin\DLSS5Overlay_antes.exe
```

## 2. Pegar o branch

```powershell
git fetch origin projecao-fase1
git checkout projecao-fase1
```

## 3. Compilar

Feche os jogos em que a captura de buffer já colocou a `DLSS5Buffer.dll`: com ela carregada, o Windows não deixa
sobrescrever o arquivo, e o jogo continuaria com a versão velha.

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1          # bin\DLSS5Overlay.exe
powershell -ExecutionPolicy Bypass -File build_buffer.ps1   # bin\captura\DLSS5Buffer.dll e obj\JogoTeste.exe
```

- Não precisa refazer o runtime (`runtime\lmxxf\compilar.ps1`, `runtime\adaptador\compilar.ps1`, `monta_runtime.ps1`)
  se o `bin\runtime` já funciona.
- A pasta `bin\ml` não é mais usada e pode ser apagada.
- Os shaders mudados são recompilados sozinhos na primeira abertura (o `cache\shaders` usa o hash do código).
- O `config.json` antigo continua servindo: as chaves da máscara são ignoradas e somem no próximo salvamento.
- Se o build falhar, o erro completo fica em `obj\build.log`. Mande as linhas com `error`.

## 4. Testes

### 4.1 Saída igual à de antes (máscara desligada)

Com a máscara desligada, a remoção não deve mudar nenhum byte. Use uma imagem qualquer do jogo e um config fixo
(`teste.json`, cópia do seu `config.json` com `"mascaraDev": false`):

```powershell
bin\DLSS5Overlay_antes.exe --imagem foto.png --quadros 8 --config teste.json --grava antes.raw --sem-painel
bin\DLSS5Overlay.exe       --imagem foto.png --quadros 8 --config teste.json --grava depois.raw --sem-painel
fc.exe /b antes.raw depois.raw
```

`fc` deve dizer que não há diferenças. Se houver, rode o `_antes` duas vezes e compare as duas saídas dele: se elas
também diferirem, a rede não é determinística nessa configuração e o teste não serve.

### 4.2 Painel

- A seção **PELE (NVIDIA)** aparece; **PELE E OBJETOS (MÁSCARA)** e **Ajuste por grupo dentro da rede** sumiram.
- Abaixo da linha da rede aparece `VRAM do overlay X de Y GB (orçamento) · na RAM Z GB`.
- Com o jogo ligado, troque o tamanho da rede: aparece o aviso amarelo "Em uso: ...". Ctrl+Shift+D duas vezes aplica.

### 4.3 VRAM em 30 minutos

1. Gerenciador de Tarefas → Detalhes → botão direito no cabeçalho → Selecionar colunas → "Memória de GPU dedicada" e
   "Memória de GPU compartilhada".
2. Ligue no jogo e anote as duas colunas do `DLSS5Overlay.exe` e do jogo.
3. Jogue 30 minutos sem mexer no painel e anote de novo.

A memória dedicada do overlay deve ficar estável. No `overlay.jsonl`, as linhas `"tipo":"memoria"` mostram a cada 5 s o
uso, o orçamento e quantos tamanhos de rede foram usados; deve haver um tamanho só.

### 4.4 Captura de buffer com resolução dinâmica

Num jogo com resolução dinâmica ligada:

- No `captura\DLSS5Buffer.log`, a linha a cada 5 s mostra `render AxB..CxD`; tamanhos diferentes = resolução dinâmica
  ativa.
- No `overlay.jsonl`, as linhas `"tipo":"buffer"` com `geracao N` devem aparecer uma vez (ou poucas, quando o máximo
  cresce), e não a cada mudança de resolução como antes.
- As travadas momentâneas ao mudar a resolução devem sumir.

### 4.5 Teto de fps

Compare o mesmo trecho do jogo com `"fpsMax": 60` e `"fpsMax": 0`. Com o jogo acima de 60 fps, o 60 limita o overlay
a 60 quadros por segundo de propósito; o atraso extra do `sleep_for` deve ter sumido.

## 5. O que mandar de volta

- Se compilou, ou as linhas de erro do `obj\build.log`.
- O resultado do `fc` do teste 4.1.
- As linhas `"tipo":"memoria"` do `overlay.jsonl` no começo e no fim dos 30 minutos.
- Se usar a captura de buffer: a linha `render` do `DLSS5Buffer.log` e quantas linhas `geracao` apareceram.

## 6. Rodada seguinte: banding no HDR, cor por matiz e histórico da rede

| Mudança | O que muda |
|---|---|
| sem o seletor de blocos | O painel volta a ter só Grande/Pequena. O "Padrão NVIDIA" volta a `profundidade` para 0 (71 blocos). |
| banding no HDR automático | Na ponte automática em HDR (SoftKnee), o quadro comprimido ficava guardado em FP16, e a volta ao HDR ampliava o degrau do FP16: entre 2× e 2,5× o branco sobravam 4 níveis, com degraus de ~17% (o banding em volta do sol). Agora o quadro guarda a luz linear e o joelho é feito na leitura, em float32: os mesmos 2× a 2,5× ficam com 257 níveis e degraus de ~0,09%. |
| cor por matiz | No HDR automático o ajuste roda na luz HDR (antes rodava na tela comprimida e pesava mais nos realces). Cada deslizador tem a cor da faixa dele. |
| histórico da própria rede | Avançado → "Histórico da própria rede (ao reabrir)". A rede tem uma entrada de histórico que até aqui recebia o quadro atual de novo; ligada, ela recebe a resposta anterior trazida pelo movimento (captura de buffer: movimento e profundidade do jogo; captura de vídeo: fluxo óptico). Precisa do runtime recompilado. |

Como na fase 1: checado com MinGW e glslang, **não compilado com o MSVC nem rodado**.

### 6.1 Compilar

Feche o overlay antes (ele prende as DLLs de `bin\runtime`).

```powershell
cd D:\AI_Claude\DLSS5-Overlay
git pull
powershell -ExecutionPolicy Bypass -File build.ps1
```

Só para o histórico da rede, o runtime e o adaptador:

```powershell
powershell -ExecutionPolicy Bypass -File runtime\lmxxf\compilar.ps1
powershell -ExecutionPolicy Bypass -File runtime\adaptador\compilar.ps1
Remove-Item bin\runtime\lmxxf_upstream.dll, bin\runtime\LmxxfNrRuntime.dll
Copy-Item runtime\lmxxf\fonte\build_msvc\LmxxfNrRuntime_v4.dll bin\runtime\lmxxf_upstream.dll
Copy-Item runtime\adaptador\LmxxfNrRuntime.dll bin\runtime\LmxxfNrRuntime.dll
Test-Path bin\runtime\native-game-tiled-assets\post70-history-head.f16
```

- O `compilar.ps1` do lmxxf agora guarda o hash do patch em `runtime\lmxxf\fonte\.patch_aplicado`. Com o patch novo,
  ele volta a fonte ao commit base e aplica o atual sozinho; a pasta `build_msvc` fica.
- O `Test-Path` deve dar `True`: a cabeça de histórico da rede vem da DLL 310.8 da NVIDIA pelo `monta_runtime.ps1`.
  Se der `False`, rode o `monta_runtime.ps1` de novo (ele refaz `bin\runtime` inteiro).

### 6.2 Testes

1. **Banding**: HDR ligado, ponte Automática, a cena do sol. O degradê em volta do sol deve ficar liso, também com
   Exibição → Original.
2. **Cor por matiz**: em HDR, Ciano: saturação −0,5 deve tirar o ciano do branco sem mudar a pele.
3. **Histórico da rede**: marque a opção e reabra o programa. Abaixo dela o painel diz "Em uso: com o movimento ...";
   se disser que o runtime não tem `LmxxfNrSetHistoryGuides`, o runtime novo não foi copiado. Compare parado e andando,
   com a opção ligada e desligada (cintilação e rastro).
   - No `overlay.jsonl`, a linha `historico da rede: guia(s) ... entregue(s) ao adaptador` diz que o overlay mandou as
     guias.
   - No `bin\runtime\lmxxf_adaptador.log`, uma linha `ERRO SetHistoryGuides` ou `ERRO PrepareFrame` diz que o runtime
     recusou; mande a linha. Sem o `post70-history-head.f16`, o erro do PrepareFrame cita esse arquivo.

## 7. Rodada 3: abas, perfis, opções da rede e o fantasma em movimento

| Mudança | O que muda |
|---|---|
| abas | O título vira duas abas: **DLSS 5** (os controles) e **PERFIS**. O aviso "jogo em foco · Esc devolve o mouse" ganhou linha própria (antes cobria o título). |
| rede numa seção só | Grande/Pequena, tamanho (540p a 2160p), passes, rede completa, ritmo e o aviso "Em uso" ficam só em Avançado → **Execução da rede**. |
| perfis | Vários por jogo, com nome: salvar como novo, salvar no em uso, carregar (duplo clique também), renomear, apagar, "usar ao ligar", e um nome para o jogo. O `perfis\<exe>.json` antigo vira "Principal". |
| luz direta | A ponte 4 só vale na captura de buffer; na de vídeo vira a automática. |
| opções da rede | **Força do histórico** (com o histórico ligado) e, em Composição, **Cor da rede (NVIDIA)** e **Força da decodificação (NVIDIA)**, os dois controles da decodificação da própria NVIDIA. |
| fantasma em movimento | Captura de vídeo: onde a edição trazida pelo movimento não confere, ela vem dos vizinhos de cor parecida, não só da grade grossa da rede (Avançado → Movimento → **Borda em movimento pelos vizinhos**). E a Exibição **Saída crua da rede** mostra a resposta sem acúmulo, movimento nem grade. |

Como antes: checado com MinGW e glslang, **não compilado com o MSVC nem rodado**.

### 7.1 Compilar

O runtime e o adaptador mudaram de novo (força do histórico). Feche o overlay antes.

```powershell
cd D:\AI_Claude\DLSS5-Overlay
git pull
powershell -ExecutionPolicy Bypass -File build.ps1
powershell -ExecutionPolicy Bypass -File runtime\lmxxf\compilar.ps1
powershell -ExecutionPolicy Bypass -File runtime\adaptador\compilar.ps1
Remove-Item bin\runtime\lmxxf_upstream.dll, bin\runtime\LmxxfNrRuntime.dll
Copy-Item runtime\lmxxf\fonte\build_msvc\LmxxfNrRuntime_v4.dll bin\runtime\lmxxf_upstream.dll
Copy-Item runtime\adaptador\LmxxfNrRuntime.dll bin\runtime\LmxxfNrRuntime.dll
```

### 7.2 Testes

1. **Fantasma**: captura de vídeo, a cena do cavalo correndo. Compare **Borda em movimento pelos vizinhos** ligada e
   desligada (muda na hora). Depois Exibição → **Saída crua da rede**: se a máscara aparece nela, é da rede; se só
   aparece em "Rede", é do movimento.
2. **Ciano**: Composição → **Cor da rede (NVIDIA)** em 0,5 e em 0 com a intensidade alta.
3. **Histórico**: **Força do histórico** em 1, 0,5 e 0 andando (rastro contra cintilação).
4. **Perfis**: salve dois perfis, troque entre eles, renomeie, marque um "ao ligar", feche e abra o programa e confira
   que ele carregou sozinho.

## 8. Rodada 4: grade afim contra a máscara em movimento (captura de buffer)

A máscara semitransparente que anda com o objeto vinha de a edição ser uma diferença por pixel feita no quadro que a
rede viu e levada até o quadro atual (atraso da rede + acúmulo de vários quadros). Agora, a cada resposta, o overlay
ajusta a transformação de cor que a rede fez (y = A·x + b, por célula de 16 px da rede e faixa de brilho) e a aplica,
na resolução cheia, à cor do quadro **atual**. A edição principal é recalculada do conteúdo de agora; só o detalhe que
a grade não explica (o resíduo) segue o caminho antigo, na força **Detalhe por pixel**.

`ferramentas\simula_grade_afim.py` reproduz as contas numa cena sintética (um objeto andando 14 px). A edição foi
construída nela, não é a da rede. Erro contra a edição certa do quadro atual, em volta do objeto / na área descoberta:

| | vetor exato | vetor com 1,5 px de erro |
|---|---|---|
| transporte por pixel (antes) | 0,0197 / 0,0372 | 0,0560 / 0,0607 |
| grade afim | 0,0139 / 0,0153 | 0,0139 / 0,0153 |
| grade afim + 0,25 do resíduo | 0,0119 / 0,0139 | 0,0138 / 0,0153 |

Ali a grade explicou 74% da edição; quanto ela explica da rede de verdade se mede no jogo.

Só o overlay mudou (não precisa recompilar runtime nem DLL):

```powershell
git pull
powershell -ExecutionPolicy Bypass -File build.ps1
```

Testes (captura de buffer, a cena em que a máscara aparecia):

1. Avançado → Movimento: **Grade afim (edição sem fantasma)** ligada e desligada (muda na próxima resposta).
2. **Detalhe por pixel** em 0, 0,25 e 1: em 0 nada é transportado; se a máscara sumir em 0 e voltar em 1, o resto é
   do resíduo.
3. **Média em movimento** de volta a 8 (ou menos): em 32 o acúmulo quase não encurta em movimento.
4. Rede em 540p ou 720p: com a grade afim aplicada na resolução cheia o resultado deve ficar menos borrado.
5. Se aparecer uma pulsação a cada resposta da rede (a grade troca de uma vez), me diga: dá para misturar a grade
   anterior com a nova em alguns quadros.
