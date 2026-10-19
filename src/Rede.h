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

// A rede roda numa fila D3D12 so dela (a da sessao do runtime): a ponte do runtime faz a fila da sessao esperar o HIP
// terminar (hip_d3d12_bridge.h, "D3D output wait"); na fila do overlay isso prendia cada quadro atras da rede inteira
// (atraso de entrada e quadros fora de sincronia). Agora a fila do overlay nunca espera a rede: o quadro vai para a
// tela na hora e a resposta entra quando fica pronta.
class Rede
{
  public:
    // pasta: onde estao LmxxfNrRuntime.dll, lmxxf_upstream.dll e native-game-tiled-assets
    // pequena: a rede pequena da 310.9.16 (runtime\pequena + os assets da grande, montados por hardlink na 1a vez)
    bool Inicia(Gpu& g, const std::wstring& pasta, bool pequena = false);
    void Encerra();
    bool Pronta() const { return ctx != nullptr && !falhou; }

    // grava na lista do overlay a copia do quadro que a rede vai ver (o overlay sobrescreve o dele no quadro seguinte).
    // So com a rede livre (sem trabalho em voo).
    void CopiaEntrada(ID3D12GraphicsCommandList* c, Tex& fed);
    // manda a rede na fila dela depois que a fila do overlay chegar em `valorOverlay` (a copia da entrada e quem leu a
    // resposta anterior ja passaram): entrada -> HIP -> saida copiada para Resposta()
    bool Envia(ID3D12Fence* cercaOverlay, UINT64 valorOverlay, const ParamsRede& p);
    bool EmVoo() const { return emVoo; }
    bool Chegou() const { return emVoo && cerca->GetCompletedValue() >= valorJob; } // sem esperar
    void EsperaChegar();
    // depois que chegou: libera o trabalho no runtime (a resposta ja e uma copia nossa)
    void Aposenta();
    // espera e descarta o trabalho em voo (troca de tamanho, encerramento)
    void Descarta();
    Tex& Entrada() { return entrada; }   // o quadro que a rede viu (estado COMMON fora das listas)
    Tex& Resposta() { return resposta; } // a ultima resposta (estado COMMON fora das listas)

    // profundidade dinamica (runtime v3): blocos residuais pulados a partir do proximo quadro; false = recusado/ausente
    bool DefineProfundidade(const std::vector<uint32_t>& blocos);
    bool TemProfundidade() const { return setSkip != nullptr; }
    std::string Status();
    std::string UltimoErro();

  private:
    bool Erro(int32_t s, const char* onde);
    bool Prepara(ID3D12GraphicsCommandList* c, const ParamsRede& p);
    ID3D12Device* dev = nullptr;
    ComPtr<ID3D12CommandQueue> fila;
    ComPtr<ID3D12Fence> cerca;
    HANDLE evento = nullptr;
    UINT64 valor = 0, valorJob = 0;
    ComPtr<ID3D12CommandAllocator> al[2];
    ComPtr<ID3D12GraphicsCommandList> cl[2];
    Tex entrada, resposta;
    HMODULE mod = nullptr;
    int32_t (*setSkip)(void*, const uint32_t*, uint32_t) = nullptr;
    LmxxfNrApi api{};
    void* ctx = nullptr;
    LmxxfNrJob job{};
    bool emVoo = false;
    bool falhou = false;
    UINT64 quadro = 0;
    std::wstring assets;
};

} // namespace ov
