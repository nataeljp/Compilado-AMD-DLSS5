// Captura da API, lado do overlay (buffer/ProtocoloApi.h): abre a copia do back buffer que o modulo faz no Present do jogo.
#pragma once
#include "Comum.h"
#include "buffer/ProtocoloApi.h"

namespace ov {

class FonteApi
{
  public:
    ~FonteApi() { Fecha(); }
    bool Abre(Gpu& g, DWORD pid, HWND janela);
    void Fecha();
    bool Aberta() const { return cab != nullptr; }
    void Batimento();
    bool JogoVivo() const { return cab && GetTickCount64() - cab->batimentoJogo < 2000; }
    // disparado quando a GPU do jogo termina o proximo quadro
    HANDLE Evento() const { return pronto; }
    const char* Erro() const { return cab ? cab->erro : ""; }
    UINT Api() const { return cab ? cab->api : 0; }
    // DXGI_COLOR_SPACE_TYPE + 1; 0 = o jogo nao disse
    UINT EspacoCor() const { return cab ? cab->espacoCor : 0; }
    // a janela do swapchain que o modulo copia, e a que ele deve preferir
    HWND JanelaSwap() const { return cab ? reinterpret_cast<HWND>(uintptr_t(cab->janelaSwap)) : nullptr; }
    void DefineAlvo(HWND janela);
    // o quadro mais novo que a GPU do jogo ja terminou e o overlay ainda nao pegou
    bool Pega(Tex*& t, uint64_t& n);
    // a GPU do overlay terminou de ler o quadro n: o jogo pode reescrever o slot
    void Consumido(uint64_t n);

  private:
    bool AbreGeracao();
    void SoltaTexturas();
    Gpu* gpu = nullptr;
    DWORD pid = 0;
    HANDLE mapa = nullptr, pronto = nullptr;
    dlss5ov::CabApi* cab = nullptr;
    uint32_t geracao = 0, geracaoPega = 0;
    ComPtr<ID3D12Fence> cerca;
    Tex cor[dlss5ov::kSlotsApi];
    uint64_t ultimoPego = 0;
};

// O teto de FPS do jogo (buffer/LimiteFps.h): esta memoria existe em todo jogo com o modulo ou a camada dentro, tambem na captura de buffer
class LimiteJogo
{
  public:
    ~LimiteJogo() { Fecha(); }
    // a cada volta do laco; pid 0 (sem o modulo no jogo) ou fps 0 = sem teto
    void Define(DWORD pid, UINT fps);

  private:
    void Fecha();
    DWORD aberto = 0;
    HANDLE mapa = nullptr;
    dlss5ov::CabApi* cab = nullptr;
    ULONGLONG tentaEm = 0;
};

} // namespace ov
