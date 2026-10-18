// config.json ao lado do exe: objeto plano de numeros/booleanos (lido na inicializacao e regravado ao sair)
#pragma once
#include "Rede.h"
#include "Temporal.h"
#include <map>

namespace ov {

// Os parametros do DLSS 5 como a NVIDIA os expoe (NGX: DLSSNR.*); viram os controles da rede e a composicao
struct ParamsNgx
{
    int modelo = 0;             // DLSSNR.Style: 0 Model A (padrao), 1 Model B (natural), 2 Model C (cinematico)
    float intensidade = 1.f;    // DLSSNR.Intensity: quanto da resposta entra (0..2; acima de 1 reforca a luz)
    float tomLocal = 1.f;       // DLSSNR.LocalToneStrength (0..1): luz e cor em grande escala; escala a gradacao
    float estruturaLocal = 1.f; // DLSSNR.LocalStructureStrength
    float estruturaPele = 1.f;  // DLSSNR.SkinStructureStrength
    bool mascara = true;        // DLSSNR.UseAutoMask: separa personagem e cena (sem ela, pele/cena = -1)
    bool gradacao = true;       // a gradacao final do Model B/C (exposicao, contraste, saturacao)
};

struct Config
{
    ParamsNgx ngx;
    ParamsRede rede;        // tamanho/passes/rede completa; os controles saem de `ngx`
    ParamsTemporal temporal;
    int alturaRede = 1080;  // altura em que a rede roda (720..2160); a largura segue a proporcao da janela
    float forca = 1.f;      // quanto da edicao final entra no quadro (0..2)
    int ponte = -1;         // ponte do quadro para a rede: -1 auto (SDR direto, HDR SoftKnee), 0 direto, 1 SoftKnee, 2 ACES
    float brancoNits = 0.f; // branco de papel em nits (0 = o branco SDR do Windows)
    int modo = 0;           // 0 rede, 1 original, 2 comparacao, 3 so a edicao, 4 movimento
    bool movimento = true;  // movimento simulado por fluxo optico
    bool usaConfianca = true; // descarta o historico onde o fluxo de ida e volta nao bate (oclusao)
    int ritmo = 0;          // 0 automatico (pelo movimento); N: a rede roda a cada N quadros; nos outros a edicao segue o movimento
    int ritmoMax = 4;       // no automatico: no maximo tantos quadros sem rodar a rede
    bool reusoVit = true;   // reuso adaptativo do ViT do lmxxf (com perda pequena; vale ao reabrir o programa)
    float divisao = 0.5f;   // posicao da divisao no modo comparacao
    int fpsMax = 60;        // teto de quadros processados por segundo (0 = sem teto)
    bool ocultaDeGravacoes = false; // o overlay fora de prints/OBS

    // controles da rede a partir dos parametros NGX (mascara ligada: pele/cena com a estrutura; desligada: -1)
    ParamsRede Rede() const;
    void Carrega(const std::wstring& arquivo);
    void Salva(const std::wstring& arquivo) const;
};

} // namespace ov
