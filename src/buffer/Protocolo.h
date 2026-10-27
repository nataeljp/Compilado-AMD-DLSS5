// Captura de buffer: o protocolo entre o modulo dentro do jogo (ClienteBuffer, dentro da DLSS5Buffer.dll) e o overlay.
//
// O jogo exporta, depois do upscale, a cor (sem HUD, linear), os vetores de movimento e a profundidade para texturas
// D3D12 compartilhadas; o overlay roda a rede e toda a cadeia (temporal, grade, composicao) e devolve uma EDICAO
// (C(final) - C(cor), C(x) = x / (1 + luma)) que o jogo soma ao quadro seguinte, reprojetada pelos vetores dele.
//
// Sem espera de GPU entre processos: cada lado so usa um slot depois de ver no CPU (GetCompletedValue) que a cerca do
// outro lado ja passou. Se o overlay fecha ou trava, o jogo simplesmente deixa de aplicar; se o jogo fecha, o overlay
// volta a esperar.
//  - entrada (2 slots): o jogo grava o slot N % 2 so se o overlay ja copiou o que estava nele (consumido); o overlay
//    so le o slot depois de cercaJogo >= N e marca consumido depois que a copia dele terminou.
//  - edicao (3 slots, rodizio): o jogo le a edicao publicada mais nova cuja escrita ja terminou (cercaOverlay) e anota
//    lido[k] = N. O overlay so escreve o slot k depois que a escrita anterior (k - 1) terminou e cercaJogo >= lido[k]:
//    o jogo ja ve k - 1 pronta (ou k - 2), nunca k, e todo quadro que leu k ja saiu da GPU.
//  - sem atraso (espera > 0, pedido pelo overlay): o unico ponto em que o jogo espera. No quadro seguinte a uma
//    exportacao o jogo espera no CPU (com prazo) a edicao dela; o overlay roda a rede no proprio quadro e dispara o
//    evento NomeEventoPublica a cada publicacao. A edicao chega 1 quadro depois, nao 4-6 (como a NVIDIA, que roda a
//    rede dentro do quadro); custa FPS.
//
// Tudo e criado pelo JOGO (ele sabe tamanhos e formatos): a memoria compartilhada "Local\DLSS5Ov.Buffer.<pid>", as
// cercas e as texturas, por nome (NomeRecurso). Quando muda tamanho ou formato, o jogo recria tudo com outra geracao.
#pragma once
#include <cmath>
#include <cstdint>
#include <cwchar>

namespace dlss5ov {

// tan(fovY / 2) para converter o Z da camera em distancia radial (overlay e jogo fazem a mesma conta); 0 = FOV
// desconhecido ou absurdo (os shaders comparam o Z, como antes)
inline float TanMeioFov(float fovY) { return fovY > 0.01f && fovY < 3.1f ? std::tan(0.5f * fovY) : 0.f; }

constexpr uint32_t kVersao = 9; // 2: movimento RG32F acumulado ate a exportacao anterior; 3: espera (sem atraso);
                                // 4: quadros repetidos; 5: modo dos quadros gerados (repetido / mesclado);
                                // 6: a luz de cada edicao (linear / logaritmica); 7: geracao de quadros da AMD;
                                // 8: a geracao em uso, a taxa do monitor e o ritmo do jogo; 9: quadros na tela e 1% low
constexpr uint32_t kSlotsEntrada = 2;
constexpr uint32_t kSlotsEdicao = 3;

inline void NomeMapa(wchar_t* b, size_t n, uint32_t pid) { swprintf(b, n, L"Local\\DLSS5Ov.Buffer.%u", pid); }
// evento (auto-reset, criado pelo jogo): o overlay dispara depois de cada Publica
inline void NomeEventoPublica(wchar_t* b, size_t n, uint32_t pid) { swprintf(b, n, L"Local\\DLSS5Ov.Publica.%u", pid); }

// Jogo aberto pelo overlay ja com o modulo (geracao de quadros: o swapchain do jogo e trocado pelo da AMD na criacao).
// O overlay cria o lancamento antes de carregar a DLL; o modulo le no carregamento, engancha a criacao do swapchain e
// dispara o evento pronto -- so entao o overlay solta o jogo (que nasceu suspenso).
struct Lancamento
{
    uint32_t versao;  // kVersao
    uint32_t geracao; // quadros apresentados por quadro do jogo pedidos no lancamento (0/1 = sem geracao, 2..4)
};
inline void NomeLancamento(wchar_t* b, size_t n, uint32_t pid) { swprintf(b, n, L"Local\\DLSS5Ov.Lancamento.%u", pid); }
inline void NomeEventoPronto(wchar_t* b, size_t n, uint32_t pid) { swprintf(b, n, L"Local\\DLSS5Ov.Pronto.%u", pid); }
// o modulo esta neste processo (instalado na pasta do jogo como dxgi.dll, ou injetado): nao colocar outro
inline void NomeModulo(wchar_t* b, size_t n, uint32_t pid) { swprintf(b, n, L"Local\\DLSS5Ov.Modulo.%u", pid); }
// qual: L"cor", L"mov", L"prof", L"exp", L"edicao", L"cercaJogo", L"cercaOverlay"
inline void NomeRecurso(wchar_t* b, size_t n, uint32_t pid, uint32_t geracao, const wchar_t* qual, uint32_t slot)
{
    swprintf(b, n, L"DLSS5Ov.%u.%u.%ls.%u", pid, geracao, qual, slot);
}

// Formatos das texturas compartilhadas (o jogo converte para eles):
//   cor    RGBA16F, corW x corH  (linear; x exposicao do jogo quando ele a entrega)
//   mov    RG32F,   movW x movH  (pixels da COR, acumulado do quadro atual ate a exportacao anterior: p_ant = p + mov)
//   prof   R32F,    profW x profH (como o jogo entrega; profInvertida diz se perto = 1)
//   exp    R32F,    1 x 1         (a exposicao aplicada na cor; 1 sem exposicao do jogo)
//   edicao RGBA16F, corW x corH   (escrita pelo overlay; rgb = C(saida) - C(cor), C(x) = x / (1 + luma), ou na luz
//                                  logaritmica log2(saida / cor) com piso 1/512; alfa = log2 da distancia da camera de
//                                  cada pixel, para o jogo so reprojetar a edicao para a mesma superficie)

struct SlotEntrada // escrito pelo jogo
{
    volatile uint64_t quadro; // N: a GPU do jogo terminou de escrever o slot quando cercaJogo >= N
    float jitterX, jitterY;   // jitter do quadro (pixels de render), so informativo depois do upscale
    float perto, longe, fovY; // camera (o FSR entrega; 0 = desconhecida): profundidade linear e normais
    uint32_t reset;           // o jogo pediu para recomecar o historico (corte de cena)
};

struct SlotEdicao // escrito pelo overlay
{
    volatile uint64_t quadro; // a edicao foi feita na cor do quadro N do jogo (0 = vazio)
    volatile uint64_t cerca;  // a escrita terminou quando cercaOverlay >= cerca
    volatile uint32_t luz;    // 0 = luz linear (diferenca no espaco comprimido), 1 = logaritmica (log2 da razao)
    uint32_t pad;
};

struct Cabecalho
{
    uint32_t versao;
    uint32_t tamanho; // sizeof(Cabecalho)
    uint32_t pidJogo;
    volatile uint32_t geracao; // texturas e cercas desta geracao; 0 = ainda sem
    uint32_t corW, corH, movW, movH, profW, profH;
    uint32_t profInvertida; // perto = 1 (Z invertido, o padrao do UE5); 2 = o jogo nao disse (o overlay mede)
    uint32_t profInfinita;  // plano distante no infinito
    uint32_t hdr;           // NGX IsHDR (cor linear com realce acima de 1)
    char exe[64];           // nome do executavel do jogo (perfil)
    volatile uint64_t batimentoJogo;    // GetTickCount64 do jogo a cada quadro
    volatile uint64_t batimentoOverlay; // GetTickCount64 do overlay; parado ha mais de 1 s = sem overlay
    volatile uint32_t ativo;            // o overlay quer os quadros (modo buffer ligado nele)
    volatile uint32_t aplica;           // o jogo soma a edicao (0: so exporta; ex. modo "original" no painel)
    volatile uint32_t espera;           // sem atraso: 1 = o quadro seguinte a cada exportacao espera a edicao dela (e
                                        // nao exporta: a rede roda a cada 2 quadros); 2 = todo quadro espera e exporta
    volatile uint32_t repete;           // sem atraso (espera > 0): quadros repetidos N = 2..4. O jogo exporta 1 de cada N
                                        // quadros, guarda a cor dele e mostra esse quadro com a edicao exata dele (sem
                                        // movimento) nos N quadros seguintes, no lugar do quadro novo sem rede (1 = todo
                                        // quadro processado, sem repeticao)
    volatile uint32_t gerado;           // com repete >= 2: 0 = quadro repetido; 1 = mesclado: entre o processado anterior
                                        // e o novo, os N-1 quadros gerados misturam os dois (50/50, 40/60, 30/70) e o novo
                                        // vem depois deles (o novo aparece N quadros depois, nao 1)
    volatile uint32_t geracaoQuadros;   // overlay -> jogo: geracao de quadros da AMD (0/1 = desligada, 2..4 quadros
                                        // apresentados por quadro do jogo)
    volatile uint32_t geracaoPronta;    // jogo -> overlay: o swapchain da AMD esta em uso (a DLL entrou antes do DirectX)
    volatile uint32_t geracaoEmUso;     // jogo -> overlay: quadros apresentados por quadro do jogo de fato (pode ser menos
                                        // que o pedido: a tela nunca recebe mais quadros por segundo que a taxa do monitor)
    volatile uint32_t hzMonitor;        // jogo -> overlay: a taxa atual do monitor do jogo (0 = desconhecida)
    volatile uint32_t hzMaximo;         // jogo -> overlay: a maior taxa que o monitor aceita nesta resolucao
    volatile uint32_t fpsJogo10;        // jogo -> overlay: quadros do jogo por segundo x 10
    volatile uint32_t apresentados10;   // jogo -> overlay: quadros na tela por segundo (reais + gerados) x 10, a cada 5 s
    volatile uint32_t baixo1pct10;      // jogo -> overlay: "1% low" dos quadros na tela x 10 (1000 / o intervalo que so
                                        // 1% das apresentacoes passa), a cada 5 s; 0 = sem apresentacoes da AMD
    volatile uint64_t consumido;        // ultimo quadro de entrada que o overlay ja copiou (o slot pode ser reescrito)
    volatile uint64_t lido[kSlotsEdicao]; // ultimo quadro do jogo que gravou leitura do slot de edicao k
    volatile uint64_t quadroJogo;         // ultimo quadro gravado pelo jogo
    volatile uint64_t quadroAplicado;     // ultimo quadro em que o jogo somou uma edicao (diagnostico)
    volatile uint32_t profInvertidaUsada; // a convencao que o overlay usou na distancia (alfa) da edicao
    // Mouse do painel aberto sobre o jogo: o jogo le o mouse por entrada bruta (o cursor do sistema nem anda), entao o
    // modulo pega o movimento e os cliques antes dele, zera o que o jogo recebe e manda para o overlay (como o UUU/IGCS).
    volatile uint32_t painelAberto;          // overlay -> jogo
    volatile int32_t mouseDx, mouseDy;       // jogo -> overlay: movimento bruto acumulado (o overlay troca por 0)
    volatile int32_t mouseRoda;              // entalhes x 120
    volatile uint32_t botaoDesce[3], botaoSobe[3]; // contadores (esquerdo, direito, meio)
    volatile uint32_t mouseEventos;          // eventos de mouse que o jogo leu (diagnostico)
    SlotEntrada entrada[kSlotsEntrada];
    SlotEdicao edicao[kSlotsEdicao];
};

} // namespace dlss5ov
