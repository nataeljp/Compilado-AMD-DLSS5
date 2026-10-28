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
    // como a edicao chega ao quadro (Shaders.h CodificaLuz): 0 linear (a diferenca somada), 1 logaritmica (log2 da
    // razao, multiplicada; piso 1/512), 2 mista (clarear somado, escurecer pela razao). Iguais na mesma resolucao com
    // forca 1; diferem quando a edicao e ampliada (rede menor que a tela), com outra forca e quando o jogo a leva ao
    // quadro seguinte
    int luz = 2;
};

// a luz da edicao na tela (a ampliacao da rede ao tamanho da tela): o pixel da tela e a mesma superficie do pixel da
// rede, e a razao conserva a textura dele -- a mista usa a razao aqui e so leva a luz somada ao quadro seguinte do jogo
// (Zorah, rede 1080 numa tela 4K contra a rede 2160: 32,8 dB log, 31,5 linear, 30,2 mista)
inline UINT LuzNaTela(int luz) { return luz == 0 ? 0u : 1u; }

class Temporal
{
  public:
    void Cria(ID3D12Device* d);
    void Garante(ID3D12Device* d, UINT w, UINT h);
    void Reinicia() { valido = false; }
    // a edicao da resposta: orig/fed = o quadro e o proxy que a rede viu (no tamanho da rede). Devolve a em luz linear
    // (o que a composicao soma; RGBA16F, estado SRV); comCrua: grava tambem a no espaco comprimido (Crua)
    Tex* Edicao(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex& orig, Tex& fed, Tex& resp,
                const ParamsTemporal& p, bool comCrua);
    bool Valido() const { return valido; }
    // a edicao da ultima resposta pedida com comCrua, no espaco comprimido x / (1 + luma) (diagnostico). GaranteCrua
    // antes de usa-la (a exibicao "saida crua", a gravacao, o dump); SoltaCrua com a GPU sem trabalho pendente
    Tex& Crua() { return crua; }
    void GaranteCrua(ID3D12Device* d);
    void SoltaCrua() { crua.Solta(); }

  private:
    Passe passeEdicao;
    Tex crua, linear;
    bool valido = false;
};

} // namespace ov
