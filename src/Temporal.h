// Composicao da resposta da rede e estabilidade temporal da edicao (no host). A resposta volta ao quadro pela composicao
// de referencia da NVIDIA/RenoDX (Shaders.h ComporReferencia); a edicao (composto - original) e acumulada no tempo,
// reprojetada pelo movimento simulado, com rejeicao fotometrica e por confianca do fluxo e limite pela vizinhanca contra
// fantasma. Nos quadros em que a rede nao roda (ritmo) a ultima edicao segue o movimento.
#pragma once
#include "Comum.h"

namespace ov {

struct ParamsTemporal
{
    int maxQuadros = 8;       // quantos quadros a edicao acumula no maximo (1 = sem acumulo)
    float rejeicao = 12.f;    // quao rapido uma divergencia de cor entre os quadros derruba o historico
    float folga = 0.02f;      // folga do limite pela vizinhanca da edicao atual
    int composicao = 1;       // 0 crua (resposta - proxy), 1 referencia NVIDIA/RenoDX
    float intensidade = 1.f;  // DLSSNR.Intensity (vem dos parametros NGX)
    float cor = 1.f;          // mistura de cor da composicao (0 = cores do quadro na luz da rede, 1 = cores da rede)
    float guarda = 16.f;      // protecao bilateral: a luz muda no maximo esse fator (16 = desligada; medido contra a
                              // saida real da NVIDIA, 2,0 custava 1,5-2 dB)
    float exposicao = 1.f;    // ganho do quadro antes da rede (ponto de operacao); desfeito na composicao
};

class Temporal
{
  public:
    void Cria(ID3D12Device* d);
    void Garante(ID3D12Device* d, UINT w, UINT h);
    void Reinicia() { valido = false; }
    // grava o passe. orig: quadro no tamanho da rede (linear); fed: o proxy que a rede viu; resp == nullptr: quadro sem
    // rede (so reprojeta). mov == nullptr: sem movimento (zera historico). confianca: do fluxo (1 = confiavel), opcional.
    // Devolve a edicao estabilizada (RGBA16F: rgb = edicao, a = quadros acumulados), estado SRV
    Tex* Roda(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex& orig, Tex& fed, Tex& fedAnt,
              ID3D12Resource* resp, DXGI_FORMAT fmtResp, Tex* mov, Tex* confianca, const ParamsTemporal& p);
    bool Valido() const { return valido; }
    Tex& Crua() { return crua; } // a edicao deste quadro antes do acumulo (diagnostico)

  private:
    Passe passe;
    Tex hist[2];
    Tex crua;
    UINT atual = 0;
    bool valido = false;
};

} // namespace ov
