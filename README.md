# DLSS5 Overlay

A rede **DLSS 5 (neural rendering)** rodando em placas AMD RDNA 4 sobre **qualquer jogo**, por captura de vídeo: o
programa captura a janela do jogo, passa a imagem pela rede e mostra o resultado num overlay por cima. **Nada é
injetado no jogo** (sem OptiScaler, sem AMDNR, sem DLL no processo do jogo).

## Como usar

1. Rode o jogo em **janela** ou **tela cheia sem borda** (tela cheia exclusiva não pode ter overlay por cima).
2. Abra `DLSS5Overlay.exe`. No painel, escolha a janela do jogo e clique em **Ligar**, ou deixe o jogo em foco e aperte
   **Ctrl+Alt+D**.
3. Ajuste ao vivo no painel; **Salvar configuração** grava `config.json`.

| Atalho | Ação |
|---|---|
| Ctrl+Alt+D | liga/desliga no jogo em primeiro plano |
| Ctrl+Alt+N | modo: rede → original → comparação → só a edição → movimento |
| Ctrl+Alt+S | estilo 0 → 1 → 2 |
| Ctrl+Alt+Q | sai |

Requisitos: placa AMD RDNA 4 (RX 9070 / 9060) e driver AMD com `amdhip64_7.dll` (os drivers atuais trazem), Windows 10
2004 ou mais novo (Windows 11 recomendado: sem a borda amarela da captura).

## Como funciona

1. **Captura** (Windows Graphics Capture) da área cliente da janela; em monitor com HDR, captura em FP16 scRGB.
2. **Entradas simuladas sem buffer do jogo**: a rede recebe a cor; os vetores de movimento saem de um fluxo óptico
   próprio entre os quadros capturados (pirâmide de luminância, busca do grosso ao fino, sub-pixel, mediana vetorial),
   no espírito do iMMERSE Launchpad (MartysMods) — implementação própria.
3. **Rede DLSS 5** (runtime HIP do lmxxf) no tamanho escolhido (720p a 2160p).
4. **Composição**: a *edição* da rede (resposta − entrada) é acumulada no tempo, reprojetada pelo movimento simulado
   (menos cintilação), e somada ao quadro nativo, que mantém o próprio detalhe. Composição de cor *Clássica* ou no
   *estilo RenoDX* (luz e cor controladas separadamente).
5. **Overlay** DirectComposition, transparente a cliques e sem foco: o jogo continua recebendo mouse e teclado.

## Parâmetros (config.json / painel)

| Chave | Faixa | O que faz |
|---|---|---|
| `alturaRede` | 720..2160 | altura em que a rede roda (limitada pela janela) |
| `redeCompleta` | true/false | os 71 blocos (false pula 42/43/46: ~1 ms mais rápido) |
| `passes` | 1..3 | cada passe roda a rede de novo sobre a própria resposta |
| `estilo` | 0..4 | 0 = padrão da NVIDIA, 1 = Stellar Blade, 2 = mais forte |
| `tom`, `estrutura`, `pele`, `outro` | −1..4 | controles do primeiro bloco da rede (1 = como treinada) |
| `detalhe`, `cor` | 0..1 | forças da decodificação do runtime (composição clássica) |
| `composicao` | 0/1 | 0 clássica, 1 estilo RenoDX |
| `rdxDetalhe`, `rdxCor`, `rdxGuarda` | 0..2, 0..2, 1..16 | RenoDX: quanto da luz da rede, quanto da cor, limite da mudança de luz |
| `forca` | 0..2 | quanto da edição da rede entra no quadro |
| `movimento` | true/false | movimento simulado por fluxo óptico |
| `estabilidade` | 1..16 | quadros que a edição acumula |
| `rejeicao` | 2..40 | quão rápido uma mudança de cor derruba o acúmulo |
| `ritmo` | 1..4 | a rede roda a cada N quadros; nos outros a edição segue o movimento |
| `fpsMax` | 0..240 | teto de quadros processados (0 = sem teto) |
| `ocultaDeGravacoes` | true/false | esconde o overlay de prints/OBS |

O tom 0 com estilo 2 é o ajuste que mais se aproximou das imagens de referência da NVIDIA nas comparações do projeto.

## Online e anti-cheat

O programa não injeta código no jogo: ele lê a janela pelo mesmo caminho que o OBS e a Barra de Jogo usam e desenha
numa janela própria. Não há como garantir a política de cada jogo; quem decide é o anti-cheat de cada um.

## Compilar

`build.ps1` (Visual Studio 2022 Build Tools, Windows SDK 10.0.26100) gera `bin\DLSS5Overlay.exe`;
`monta_runtime.ps1` monta `bin\runtime` (pesos exatos em FP16 gravados como `.f16`: metade do tamanho, saída idêntica
bit a bit); `empacota.ps1` gera o zip de distribuição.

## Créditos e licenças

- Rede e runtime HIP: **lmxxf** (MIT) — `licencas/LICENSE.lmxxf`. Pesos da rede: NVIDIA (DLSS 5), convertidos do
  `nvngx_dlssnr.dll`.
- Interface: **Dear ImGui** (MIT) — `licencas/LICENSE.imgui.txt`.
- Composição de cor inspirada no **RenoDX** de clshortfuse (MIT): UpgradeToneMap e correção de matiz (implementação
  própria). OkLab: constantes de Björn Ottosson.
- Fluxo óptico inspirado no **iMMERSE Launchpad** (MartysMods): mesma ideia, nenhum código dele.
