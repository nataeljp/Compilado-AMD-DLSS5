// Estabilidade temporal da edicao da rede (no host): o runtime atual nao usa historico, entao a edicao
// (resposta - alimentado) e acumulada aqui, reprojetada pelo movimento simulado (fluxo optico), com rejeicao
// fotometrica e limite pela vizinhanca contra fantasma. Tambem cobre os quadros em que a rede nao roda (ritmo):
// a ultima edicao segue o movimento.
#pragma once
#include "Comum.h"

namespace ov {

struct ParamsTemporal
{
    int maxQuadros = 8;       // quantos quadros a edicao acumula no maximo (1 = sem acumulo)
    float rejeicao = 12.f;    // quao rapido uma divergencia de cor entre os quadros derruba o historico
    float folga = 0.02f;      // folga do limite pela vizinhanca da edicao atual
};

class Temporal
{
  public:
    void Cria(ID3D12Device* d);
    void Garante(ID3D12Device* d, UINT w, UINT h);
    void Reinicia() { valido = false; }
    // grava o passe; resp == nullptr: quadro sem rede (so reprojeta). mov == nullptr: sem movimento (zera historico)
    // devolve a edicao estabilizada (RGBA16F: rgb = edicao, a = quadros acumulados), estado SRV
    Tex* Roda(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex& fed, Tex& fedAnt,
              ID3D12Resource* resp, DXGI_FORMAT fmtResp, Tex* mov, const ParamsTemporal& p);
    bool Valido() const { return valido; }

  private:
    Passe passe;
    Tex hist[2];
    UINT atual = 0;
    bool valido = false;
};

} // namespace ov
