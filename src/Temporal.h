// Composicao da resposta da rede e estabilidade temporal da edicao (no host). A resposta volta ao quadro pela composicao
// de referencia da NVIDIA/RenoDX (Shaders.h ComporReferencia); a edicao (composto - original) e acumulada no tempo,
// reprojetada pelo movimento simulado, com rejeicao fotometrica e por confianca do fluxo e limite pela vizinhanca contra
// fantasma. Nos quadros em que a resposta nao chega a ultima edicao segue o movimento; a que chega atrasada vem pelo
// deslocamento acumulado desde o quadro que a rede viu. O acumulo e no espaco comprimido x / (1 + luma) (como o TAA dos
// jogos: o realce HDR nao vira pontos coloridos quando a edicao anda meio pixel), e onde a edicao nao pode ser trazida
// (conteudo animado, oclusao, borda nova) ela sai de uma grade bilateral da ultima resposta (bloco x brilho, HDRnet).
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
    void Reinicia() { valido = gradeValida = false; }
    // A rede responde com atraso (fila propria): a resposta e do quadro t0 que ela viu, nao do atual.
    // Edicao: quando a resposta chega, a edicao dela no espaco de t0 (origT0/fedT0: o quadro e o proxy de t0).
    void Edicao(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex& origT0, Tex& fedT0, Tex& resp,
                const ParamsTemporal& p);
    // Acumula: um quadro a mais no deslocamento ate t0 (zera = o anterior era o proprio t0). Devolve o deslocamento.
    Tex* Acumula(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex* mov, Tex* confianca, bool zera);
    // Grade: na chegada (depois de Edicao), a grade bilateral da edicao de t0; `deslocamento` = o ate t0 (nullptr = t0 e
    // este quadro). AcumulaGrade: um quadro a mais no deslocamento ate o t0 da grade (todo quadro, antes de Roda).
    void Grade(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex& origT0, Tex* deslocamento);
    Tex* AcumulaGrade(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex* mov, Tex* confianca);
    // grava o passe. chegou: a edicao de Edicao() entra neste quadro, trazida de t0 pelo `deslocamento` (nullptr = t0 e
    // este quadro); senao so reprojeta. mov == nullptr: sem movimento. confianca: do fluxo (1 = confiavel), opcional.
    // Devolve a edicao estabilizada (RGBA16F: rgb = edicao, a = quadros acumulados), estado SRV
    Tex* Roda(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex& orig, Tex& fed, Tex& fedAnt,
              bool chegou,
              Tex* fedT0, Tex* deslocamento, Tex* mov, Tex* confianca, const ParamsTemporal& p);
    bool Valido() const { return valido; }
    Tex& Crua() { return crua; } // a edicao da ultima resposta, no espaco de t0 (diagnostico)

  private:
    Tex* AcumulaEm(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex (&par)[2], UINT& qual, Tex* mov,
                   Tex* confianca, bool zera);
    Passe passe, passeEdicao, passeAcumula, passeGrade;
    Tex grade, gradeCor, deslG[2];
    Tex* deslGAgora = nullptr;
    UINT gw = 0, gh = 0, deslGAtual = 0;
    bool gradeValida = false, zeraG = false;
    Tex hist[2];
    Tex desl[2];
    Tex crua, linear;
    UINT atual = 0, deslAtual = 0;
    bool valido = false;
};

} // namespace ov
