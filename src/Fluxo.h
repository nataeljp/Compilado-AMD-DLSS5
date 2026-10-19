// Movimento simulado sem buffer do jogo: fluxo optico entre o quadro atual e o anterior (piramide de luminancia,
// busca em blocos do nivel grosso ao fino, refinamento sub-pixel e mediana vetorial), no espirito do iMMERSE
// Launchpad (MartysMods) -- implementacao propria, nenhum codigo dele. A busca termina em meia resolucao (~0,5 px de
// quantizacao); o refinamento de Lucas-Kanade em resolucao cheia (DIS/KLT) leva o vetor ao sub-pixel. Nos niveis mais
// finos que 1/8, o movimento global (afim robusto, como na estabilizacao de video de celular) entra como candidato:
// desfaz o casamento com o periodo vizinho em textura repetitiva. Medido contra movimento exato: erros grosseiros em
// textura 21% -> 4%, cintilacao da edicao 0,196 -> 0,073 (ideal 0,063).
// Calculado nos dois sentidos: onde o vetor de ida nao volta ao mesmo ponto (oclusao, objeto que entra/sai,
// transparencia, erro de casamento) a confianca cai e o historico daquele pixel e descartado -- e o que evita rastros.
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
    Tex& Movimento() { return mov; }   // RG16F, w x h, pixels (atual -> anterior)
    Tex& Confianca() { return conf; }  // R16F, w x h, 1 = ida e volta batem

    // estatisticas do quadro (ritmo automatico): grava depois do Calcula; le depois que a GPU terminou o quadro
    struct Estat
    {
        float semConfianca = 1.f; // media de (1 - confianca): oclusao e conteudo novo
        float velocidade = 0.f;   // |v| medio em pixels da rede
        float mudancaLuz = 1.f;   // |luma atual - anterior reprojetado| medio (perceptual)
        bool valida = false;
    };
    void Estatisticas(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex& fed, Tex& fedAnt);
    Estat LeEstatisticas();
    ID3D12Resource* BufEstat() { return bufEstat.Get(); } // as somas do quadro na GPU (lidas pelo Temporal no mesmo quadro)
    UINT niveis = 0;

  private:
    static constexpr UINT kMaxNiveis = 6;
    // busca do grosso ao fino de `de` contra `para` (indices da piramide), resultado em `saida` (tamanho da rede)
    void Busca(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, UINT de, UINT para, Tex* campos,
               Tex* tmps, Tex& saida, Tex& modelo);
    Passe pLuma, pReduzLuma, pBusca, pMediana, pAmplia, pConfianca, pEstat, pGradiente, pRefina, pGlobal;
    Tex modeloIda, modeloVolta; // movimento global (afim) de cada sentido, 2x1 RGBA32F
    UINT nivelGlobal = 0;       // nivel em que o modelo global e ajustado (o primeiro com ate 320 px de largura)
    // refinamento de Lucas-Kanade em resolucao cheia: `de` = modelo, `para` = alvo
    void Refina(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, UINT de, UINT para, Tex& bruto,
                Tex& saida);
    ComPtr<ID3D12Resource> bufEstat, bufZero, bufLeitura;
    bool estatGravada = false;
    Tex luma[2][kMaxNiveis];      // [atual/anterior][nivel]; nivel 0 = 1/2 do tamanho da rede
    Tex campoIda[kMaxNiveis], tmpIda[kMaxNiveis];
    Tex campoVolta[kMaxNiveis], tmpVolta[kMaxNiveis];
    Tex rascunho;                 // SRV mudo do nivel mais grosso
    Tex mov, movVolta, conf;
    Tex lumaCheia[2], gradCheia[2]; // luminancia e gradiente na resolucao da rede (refinamento)
    Tex movBruto;                   // vetor de ida da busca (meia resolucao ampliada), antes do refinamento
    UINT atual = 0;
    UINT w = 0, h = 0;
    bool temAnterior = false;
};

} // namespace ov
