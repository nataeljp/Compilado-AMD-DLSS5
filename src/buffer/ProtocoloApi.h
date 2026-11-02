// Captura da API: o back buffer lido dentro do jogo, no Present, que e o ponto comum do D3D11 e do D3D12 (a DXGI). Nao depende de
// upscaler nem de motor. Como a captura de video, o quadro vem pronto (com HUD), sem profundidade e sem movimento do jogo, e o
// resultado sai na janela do overlay. Memoria compartilhada propria: nada aqui toca no protocolo da captura de buffer.
// Tudo e criado pelo JOGO, por nome. O modulo so engancha o Present depois que o overlay pedir (batimento).
#pragma once
#include <cstdint>
#include <cwchar>

namespace dlss5ov {

constexpr uint32_t kVersaoApi = 2;
constexpr uint32_t kSlotsApi = 2;
constexpr uint32_t kApiVulkan = 1; // CabApi::api: 11 e 12 sao os DirectX
// a camada Vulkan so aceita carregar com este evento aberto: o overlay o segura enquanto a opcao esta ligada
constexpr const wchar_t* kEventoCamada = L"Local\\DLSS5Ov.Camada";

inline void NomeMapaApi(wchar_t* b, size_t n, uint32_t pid) { swprintf(b, n, L"Local\\DLSS5Ov.Api.%u", pid); }
// qual: L"cor", L"cerca"
inline void NomeRecursoApi(wchar_t* b, size_t n, uint32_t pid, uint32_t geracao, const wchar_t* qual, uint32_t slot)
{
    swprintf(b, n, L"DLSS5OvApi.%u.%u.%ls.%u", pid, geracao, qual, slot);
}

struct CabApi
{
    uint32_t versao;
    uint32_t tamanho;                    // sizeof(CabApi)
    volatile uint64_t batimentoOverlay;  // GetTickCount64 do overlay; parado ha mais de 1 s = nao copia, 5 s = solta a VRAM
    volatile uint64_t batimentoJogo;     // GetTickCount64 do jogo a cada Present
    volatile uint64_t janela;            // overlay -> jogo: a janela alvo (o swapchain dela tem prioridade)
    volatile uint64_t janelaSwap;        // jogo -> overlay: a janela do swapchain copiado
    volatile uint32_t geracao;           // as texturas e a cerca desta geracao; 0 = ainda sem
    uint32_t w, h, fmt;                  // o back buffer (DXGI_FORMAT, sem _SRGB)
    uint32_t api;                        // 11, 12 ou kApiVulkan
    volatile uint32_t espacoCor;         // DXGI_COLOR_SPACE_TYPE + 1 do SetColorSpace1 (ou do swapchain Vulkan); 0 = o jogo nao disse
    volatile uint64_t quadro[kSlotsApi]; // N: a GPU do jogo terminou de escrever o slot quando a cerca >= N
    volatile uint64_t consumido;         // ultimo quadro que o overlay ja copiou (o slot pode ser reescrito)
    char erro[160];                      // por que nao copia ("" = sem erro)
    volatile uint64_t batimentoLimite;   // GetTickCount64 do overlay enquanto pede o teto de FPS (LimiteFps.h), em qualquer captura com o modulo
    volatile uint32_t limiteFps;         // overlay -> jogo: teto de quadros por segundo; 0 = sem teto
    uint32_t pad;
};

} // namespace dlss5ov
