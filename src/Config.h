// config.json ao lado do exe: objeto plano de numeros/booleanos (lido na inicializacao e regravado ao sair)
#pragma once
#include "Rede.h"
#include "Temporal.h"
#include <cmath>
#include <map>

namespace ov {

// Os parametros do DLSS 5 como a NVIDIA os expoe (NGX: DLSSNR.*); viram os controles da rede e a composicao
struct ParamsNgx
{
    int modelo = 0;             // DLSSNR.Style: 0 Model A (padrao), 1 Model B (natural), 2 Model C (cinematico)
    float intensidade = 1.f;    // DLSSNR.Intensity: quanto da resposta entra (0..4; acima de 1 reforca a luz)
    float tomLocal = 1.f;       // DLSSNR.LocalToneStrength (0..1): luz e cor em grande escala; escala a gradacao
    float estruturaLocal = 1.f; // DLSSNR.LocalStructureStrength
    float estruturaPele = 1.f;  // DLSSNR.SkinStructureStrength
    bool mascara = true;        // DLSSNR.UseAutoMask: separa personagem e cena (sem ela, pele/cena = -1)
    bool gradacao = true;       // a gradacao final do Model B/C (exposicao, contraste, saturacao)
};

// cor por matiz depois da rede (seis eixos): -1..1, 0 = sem mudanca
struct CorMatiz
{
    float sat[6] = {}; // vermelho, amarelo, verde, ciano, azul, magenta: -1 = cinza, +1 = o dobro de saturacao
    float luz[6] = {}; // brilho de cada faixa: -1 = apaga, +1 = o dobro (pesa pelo croma: cinza e branco nao mudam)
    bool Ativa() const
    {
        for (int i = 0; i < 6; ++i)
            if (sat[i] != 0.f || luz[i] != 0.f)
                return true;
        return false;
    }
};

struct Config
{
    ParamsNgx ngx;
    ParamsRede rede;        // tamanho/passes/rede completa; os controles saem de `ngx`
    ParamsTemporal temporal;
    int alturaRede = 1080;  // altura em que a rede roda (720..2160); a largura segue a proporcao da janela
    bool redePequena = false; // a rede pequena da 310.9.16 (31 blocos, mais rapida) no lugar da grande (71 blocos)
    float forca = 1.f;      // quanto da edicao final entra no quadro (0..4)
    float nitidez = 0.f;    // nitidez CAS depois da composicao, so no lado da rede (0 = desligada, 1 = o maximo do CAS)
    int ponte = -1;         // ponte do quadro para a rede: -1 auto (SDR direto, HDR SoftKnee), 0 direto, 1 SoftKnee, 2 ACES,
                            // 3 curva do jogo, 4 luz de cena direta (so o ombro do codificador da NVIDIA)
    float brancoNits = 0.f; // branco de papel em nits (0 = o branco SDR do Windows)
    float preLuz = 0.f;     // antes da rede: ganho das luzes acima do limiar (ideia do Shadow-Light-Scattering.fx)
    float preSombra = 0.f;  // antes da rede: escurece (>0) ou levanta (<0) as sombras abaixo do limiar
    float preLimiar = 0.5f; // limiar em luma de tela (0..1) a partir do qual as luzes sobem
    float preLimiarSombra = 0.2f; // abaixo dele as sombras descem (preSombra > 0) ou sobem (< 0)
    float preCor = 0.f;     // antes da rede: saturacao (-1..2)
    CorMatiz matiz;         // depois da rede: saturacao e brilho por matiz
    int modo = 0;           // 0 rede, 1 original, 2 comparacao, 3 so a edicao, 4 movimento
    int fonte = 0;          // Modo de Rede: 0 captura de video (WGC), 1 captura de buffer (cor sem HUD, profundidade e
                            // movimento do upscaler do proprio jogo, pela DLSS5Buffer.dll colocada no jogo aberto)
    // quadros gerados (Ritmo 2..4, como o AMD Fluid Motion Frames): 0 repetido (o quadro processado se repete; sem atraso
    // extra), 1 mesclado (mistura do processado anterior com o novo: 50/50, 40/60, 30/70; o novo vem depois deles, N
    // quadros depois de feito)
    int gerado = 0;
    // geracao de quadros da AMD na captura de buffer (o jogo abre pelo overlay: o swapchain e trocado na criacao):
    // 0 desligada, 2 = 2x (um quadro gerado por quadro do jogo)
    int geracao = 0;
    bool movimento = true;  // movimento simulado por fluxo optico (as guias do historico da propria rede)
    int ritmo = 1;          // a rede edita 1 de cada N quadros; os outros sao gerados dos processados (1 = todo quadro)
    int profundidade = 0;   // blocos da rede grande: 0 = 71, 1 = 68, 2 = 63, 3 = 55, 4 = 47 (residuais pulados); mais rapido
    bool profundidadeAuto = false; // a profundidade segue a velocidade (medido: corta no maximo ~4,7 ms e estraga a imagem)
    int profundidadeMax = 3;      // no automatico, o nivel mais raso permitido
    int temporalRede = 1;   // historico da propria rede (runtime lmxxf DLSS5_TEMPORAL_MODE): 0 desligado, 1 historico (com as
                            // guias de movimento e profundidade do overlay), 2 baixa freq., 3 realce; vale ao reabrir
    float realceTemporal = 1.f; // forca do modo 3 (0..2)
    float forcaHistorico = 1.f; // modo 1: escala do peso que a cabeca de historico da rede decide (0..1; 1 = o da rede)
    float recorteHistorico = 0.f; // modo 1: recorte do historico pela vizinhanca da saida crua, desvios (0 = sem recorte;
                                  // sem a resposta atrasada ele so piorava: cintilacao 0,237 -> 0,256, rastro e fantasma
                                  // tambem, medido no jogo de teste)
    // A decodificacao da NVIDIA (ColorStrength / TransferStrength do runtime) fica em 1: repetia o Tom e a Forca da edicao
    bool reusoVit = true;   // reuso adaptativo do ViT do lmxxf (com perda pequena; vale ao reabrir o programa)
    float divisao = 0.5f;   // posicao da divisao no modo comparacao
    int fpsMax = 60;        // teto de quadros processados por segundo (0 = sem teto)
    bool ocultaDeGravacoes = false; // o overlay fora de prints/OBS

    // controles da rede a partir dos parametros NGX, nas faixas da NVIDIA
    ParamsRede Rede() const;
    void Carrega(const std::wstring& arquivo);
    void Salva(const std::wstring& arquivo) const;
    std::string Texto() const; // o JSON que Salva grava (comparar dois Config: os mesmos ajustes = o mesmo texto)
};

} // namespace ov
