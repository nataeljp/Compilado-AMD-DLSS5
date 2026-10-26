// Composicao da resposta da rede: a edicao DESTE quadro (a rede ve o quadro e responde nele mesmo, como a NVIDIA roda o
// DLSS 5). A resposta volta ao quadro pela composicao de referencia da NVIDIA/RenoDX (Shaders.h ComporReferencia); nada e
// trazido de quadros velhos (o modo com atraso, que reprojetava e acumulava a resposta atrasada, foi removido: era o
// fantasma). A estabilidade no tempo e a do historico da propria rede.
#pragma once
#include "Comum.h"

namespace ov {

struct ParamsTemporal
{
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
    // a edicao da resposta: orig/fed = o quadro e o proxy que a rede viu (no tamanho da rede). Grava a edicao no espaco
    // comprimido (Crua) e devolve a em luz linear (o que a composicao soma; RGBA16F, estado SRV)
    Tex* Edicao(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex& orig, Tex& fed, Tex& resp,
                const ParamsTemporal& p);
    bool Valido() const { return valido; }
    Tex& Crua() { return crua; } // a edicao da ultima resposta, no espaco comprimido x / (1 + luma) (diagnostico)

  private:
    Passe passeEdicao, passeLinear;
    Tex crua, linear;
    bool valido = false;
};

} // namespace ov
