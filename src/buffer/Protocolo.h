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

constexpr uint32_t kVersao = 2; // 2: movimento RG32F acumulado ate a exportacao anterior
constexpr uint32_t kSlotsEntrada = 2;
constexpr uint32_t kSlotsEdicao = 3;

inline void NomeMapa(wchar_t* b, size_t n, uint32_t pid) { swprintf(b, n, L"Local\\DLSS5Ov.Buffer.%u", pid); }
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
//   edicao RGBA16F, corW x corH   (escrita pelo overlay; alfa = log2 da distancia da camera de cada pixel, para o
//                                  jogo so reprojetar a edicao para a mesma superficie)

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
