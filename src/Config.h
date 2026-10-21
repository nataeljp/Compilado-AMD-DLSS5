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

// Mascara de desenvolvedor (o "Developer Masking" da NVIDIA): grupos separados do cenario por um classificador, cada um
// com os proprios controles dentro da rede
// Os ajustes de cada grupo sao RELATIVOS ao global: 0 = igual ao global; o global continua mandando no quadro inteiro.
struct GrupoMascara
{
    // quanto do DLSS 5 entra neste grupo (0 = o quadro original, 1 = a rede): o ControlMask da NVIDIA (no binario, o
    // ultimo kernel faz lerp(original, rede, mascara) por pixel, numa passada so)
    float dlss = 1.f;
    // ajustes DENTRO da rede (nosso, nao e da NVIDIA; Avancado): os canais de controle da primeira camada por pixel
    float estrutura = 0.f; // a mais (+) ou a menos (-) que a estrutura global, so neste grupo
    float tom = 0.f;       // a mais ou a menos que o tom global, so neste grupo
    int modelo = -1;       // -1 = o modelo global; 0/1/2 = Model A/B/C so neste grupo
    bool Neutro() const { return estrutura == 0.f && tom == 0.f && modelo < 0; }
};
struct MascaraDev
{
    bool liga = false;     // separa pele, cabelo, personagem e objetos (classificador em bin\ml)
    bool mostra = false;   // pinta os grupos sobre a imagem (o "Show Masks")
    int taxa = 6;          // classificacoes por segundo (entre elas o movimento leva a mascara)
    // onde e personagem: 0 = a rede adivinha (os canais de personagem e de cena valem a tela inteira, o UseAutoMask da
    // NVIDIA); 1 = o classificador diz (canal de personagem so no personagem, de cena so no cenario); 2 = idem, e o canal
    // de cena tambem sai do personagem
    int explicita = 0; // medido contra a NVIDIA (Lara 1080p): 1 = -1,7 dB, 2 = -2,6 dB; a rede ja acha a pele sozinha
    GrupoMascara grupo[4]; // pele, cabelo, personagem (roupa, armadura, acessorios, animais), objetos
    // algum grupo pede controles por pixel dentro da rede (sem isso a rede roda como sempre e so a intensidade muda)
    bool PorPixel() const
    {
        if (explicita > 0)
            return true;
        for (const auto& g : grupo)
            if (!g.Neutro())
                return true;
        return false;
    }
    bool PorIntensidade() const
    {
        for (const auto& g : grupo)
            if (g.dlss != 1.f)
                return true;
        return false;
    }
};

struct Config
{
    ParamsNgx ngx;
    MascaraDev dev;
    ParamsRede rede;        // tamanho/passes/rede completa; os controles saem de `ngx`
    ParamsTemporal temporal;
    int alturaRede = 1080;  // altura em que a rede roda (720..2160); a largura segue a proporcao da janela
    bool redePequena = false; // a rede pequena da 310.9.16 (31 blocos, mais rapida) no lugar da grande (71 blocos)
    float forca = 1.f;      // quanto da edicao final entra no quadro (0..2)
    int ponte = -1;         // ponte do quadro para a rede: -1 auto (SDR direto, HDR SoftKnee), 0 direto, 1 SoftKnee, 2 ACES
    float brancoNits = 0.f; // branco de papel em nits (0 = o branco SDR do Windows)
    float preLuz = 0.f;     // antes da rede: ganho das luzes acima do limiar (ideia do Shadow-Light-Scattering.fx)
    float preSombra = 0.f;  // antes da rede: escurece (>0) ou levanta (<0) as sombras abaixo do limiar
    float preLimiar = 0.5f; // limiar em luma de tela (0..1) a partir do qual as luzes sobem
    float preLimiarSombra = 0.2f; // abaixo dele as sombras descem (preSombra > 0) ou sobem (< 0)
    float preCor = 0.f;     // antes da rede: saturacao (-0,5..1)
    int modo = 0;           // 0 rede, 1 original, 2 comparacao, 3 so a edicao, 4 movimento
    int fonte = 0;          // Modo de Rede: 0 captura de video (WGC), 1 captura de buffer (cor sem HUD, profundidade e
                            // movimento do upscaler do proprio jogo, pela DLSS5Buffer.dll colocada no jogo aberto)
    bool movimento = true;  // movimento simulado por fluxo optico
    bool usaConfianca = true; // descarta o historico onde o fluxo de ida e volta nao bate (oclusao)
    int ritmo = 1;          // a rede roda a cada N quadros (1 = todo quadro; a rede nunca deve sumir em movimento)
    int ritmoMax = 4;       // no automatico: no maximo tantos quadros sem rodar a rede
    int profundidade = 0;   // camadas da rede: 0 = as 71; 1..5 = cada vez menos (blocos residuais pulados); mais rapido
    bool profundidadeAuto = false; // a profundidade segue a velocidade (medido: corta no maximo ~4,7 ms e estraga a imagem)
    int profundidadeMax = 3;      // no automatico, o nivel mais raso permitido
    int temporalRede = 0;   // historico da propria rede (runtime lmxxf DLSS5_TEMPORAL_MODE): 0 desligado, 1 historico, 2 baixa freq., 3 realce
    float realceTemporal = 1.f; // forca do modo 3 (0..2)
    bool reusoVit = true;   // reuso adaptativo do ViT do lmxxf (com perda pequena; vale ao reabrir o programa)
    float divisao = 0.5f;   // posicao da divisao no modo comparacao
    int fpsMax = 60;        // teto de quadros processados por segundo (0 = sem teto)
    bool ocultaDeGravacoes = false; // o overlay fora de prints/OBS

    // controles da rede a partir dos parametros NGX (mascara ligada: pele/cena com a estrutura; desligada: -1)
    ParamsRede Rede() const;
    // a tabela de controles por grupo da mascara de desenvolvedor (o cenario = os controles globais)
    void TabelaMascara(float t[40]) const;
    void Carrega(const std::wstring& arquivo);
    void Salva(const std::wstring& arquivo) const;
};

} // namespace ov
