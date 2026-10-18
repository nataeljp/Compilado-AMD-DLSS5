// Movimento simulado sem buffer do jogo: fluxo optico entre o quadro atual e o anterior (piramide de luminancia,
// busca em blocos do nivel grosso ao fino, refinamento sub-pixel e mediana vetorial), no espirito do iMMERSE
// Launchpad (MartysMods) -- implementacao propria, nenhum codigo dele.
// Convencao de saida (a do DLSS/FSR): vetor = deslocamento em pixels do quadro atual ate onde o ponto estava no
// quadro anterior.
#pragma once
#include "Comum.h"

namespace ov {

class Fluxo
{
  public:
    void Cria(ID3D12Device* d);
    // garante os recursos para o tamanho da rede (w x h)
    void Garante(ID3D12Device* d, UINT w, UINT h);
    // calcula o movimento de `cor` (linear, w x h, estado SRV) contra o quadro anterior guardado;
    // na primeira chamada (ou depois de Reinicia) devolve false: nao ha anterior
    bool Calcula(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, ID3D12Resource* cor,
                 DXGI_FORMAT fmtCor);
    void Reinicia() { temAnterior = false; }
    Tex& Movimento() { return mov; } // RG16F, w x h, pixels
    // erro medio de reconstrucao (diagnostico): nao usado no caminho normal
    UINT niveis = 0;

  private:
    static constexpr UINT kMaxNiveis = 6;
    Passe pLuma, pReduzLuma, pBusca, pMediana, pAmplia;
    Tex luma[2][kMaxNiveis];      // [atual/anterior][nivel]; nivel 0 = 1/2 do tamanho da rede
    Tex nivelCampo[kMaxNiveis];   // vetores de cada nivel (pixels do nivel)
    Tex nivelTmp[kMaxNiveis];     // rascunho da mediana
    Tex rascunho;                 // SRV mudo do nivel mais grosso
    Tex mov;
    UINT atual = 0;
    UINT w = 0, h = 0;
    bool temAnterior = false;
};

} // namespace ov
