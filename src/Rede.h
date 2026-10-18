// Host da rede DLSS 5: LmxxfNrRuntime.dll (lmxxf, MIT; adaptador AMDNR/lmxxf2) pela API C publica.
#pragma once
#include "Comum.h"
#include "ApiRede.h"

namespace ov {

// Os parametros do DLSS 5 que valem a pena: os controles do primeiro bloco (o que o NGX expoe como estilo e as
// mascaras), as forcas da decodificacao (a "Intensity"), passes, rede completa e a suavizacao temporal da saida.
struct ParamsRede
{
    float estilo = 1.f;     // canal de estilo (0 = padrao da NVIDIA, 1 = Stellar Blade, ate 4)
    float tom = 1.f;        // controles: -1..4 (1 = como foi treinado)
    float estrutura = 1.f;
    float pele = 1.f;
    float outro = 1.f;
    float detalhe = 1.f;    // forca de detalhe da decodificacao, 0..1
    float cor = 1.f;        // forca de cor da decodificacao, 0..1
    int passes = 1;         // 1..3 (cada passe roda a rede inteira de novo)
    bool redeCompleta = true; // os 71 blocos (sem pular 42/43/46)
    float suaviza = 0.f;    // suavizacao temporal da saida (precisa de movimento), 0..0,8
};

class Rede
{
  public:
    // pasta: onde estao LmxxfNrRuntime.dll, lmxxf_upstream.dll e native-game-tiled-assets
    bool Inicia(Gpu& g, const std::wstring& pasta);
    void Encerra();
    bool Pronta() const { return ctx != nullptr && !falhou; }

    // grava as entradas na lista `c` (o chamador fecha e executa); `cor` em estado `estadoCor`
    // `movimento` opcional (RG float, deslocamento em pixels ate a posicao no quadro anterior)
    bool Prepara(ID3D12GraphicsCommandList* c, ID3D12Resource* cor, D3D12_RESOURCE_STATES estadoCor, UINT w, UINT h,
                 ID3D12Resource* movimento, D3D12_RESOURCE_STATES estadoMov, bool zeraHistorico, const ParamsRede& p);
    // enfileira a rede (HIP) na fila, depois da lista de entrada executada
    bool Enfileira(ID3D12CommandQueue* fila);
    // grava a saida na lista `c`; devolve a resposta (estado NON_PIXEL_SHADER_RESOURCE) ou nullptr
    ID3D12Resource* Saida(ID3D12GraphicsCommandList* c);
    // depois que a GPU terminou a lista da saida
    void Aposenta();

    std::string Status();
    std::string UltimoErro();

  private:
    bool Erro(int32_t s, const char* onde);
    HMODULE mod = nullptr;
    LmxxfNrApi api{};
    void* ctx = nullptr;
    LmxxfNrJob job{};
    bool emVoo = false;
    bool falhou = false;
    UINT64 quadro = 0;
    std::wstring assets;
};

} // namespace ov
