#pragma once
// [lmxxf2] Ray Regeneration do JOGO: o denoiser AMD FFX 1.2 que o próprio jogo cria (Crimson Desert e outros).
// O OptiScaler engolia os configures e as consultas desse contexto (devolvia OK sem repassar ao provedor): o denoiser
// rodava com os valores de fábrica e o jogo recebia lixo ao pedir os padrões. Agora tudo é repassado, os valores que o
// jogo pede são guardados e a UI troca cada um ([RayRegenJogo]); chave sem valor = o que o jogo pediu.
#include <ffx_api.h>
#include <Config.h>

#include <string>

namespace Lmxxf2RrJogo
{
inline constexpr int kChaves = 6; // FfxApiConfigureDenoiserKey 1..6 (a 7, faixa de profundidade do debug, fica de fora)

// configure do jogo num contexto de denoiser/radiance: guarda o valor pedido, troca pelo da UI quando ajustado, repassa
ffxReturnCode_t Configure(ffxContext* context, ffxConfigureDescHeader* desc);
// o jogo criou um contexto que não é de upscale: guarda sinais, flags e tamanho do denoiser
void Criado(const ffxContext* context, const ffxCreateContextDescHeader* desc, ffxReturnCode_t resultado);
// antes de cada dispatch do denoiser do jogo: lê os padrões AMD uma vez, aplica o que a UI mudou e o reset pedido
void AntesDoDispatch(ffxContext* context, ffxDispatchDescHeader* desc);
// UI: o próximo dispatch do denoiser recomeça o histórico temporal (o "Reset" do sample da AMD)
void PedeReset();
// o jogo destruiu o contexto
void Destruido(ffxContext context);

struct Estado
{
    int contextos = 0;
    uint64_t configures = 0;
    uint64_t dispatches = 0;
    bool jogoDefiniu[kChaves] {};
    float jogo[kChaves] {};
    bool padraoOk[kChaves] {};
    float padrao[kChaves] {};
    bool aplicadoOk[kChaves] {};
    float aplicado[kChaves] {};
    std::string erro;
    // criação do contexto pelo jogo
    bool criado = false;
    uint32_t sinais = 0, sinaisXadrez = 0, flagsCriacao = 0, versao = 0, maxW = 0, maxH = 0;
    // último dispatch
    uint32_t renderW = 0, renderH = 0, flagsDispatch = 0;
    float dispatchesPorSeg = 0.f;
    uint64_t resets = 0;
};
Estado Le();

struct Info
{
    const char* nome;
    const char* dica;
    float min;
    float max;
    const char* formato;
    bool logaritmico;
};
const Info& Chave(int i);
CustomOptional<float, NoDefault>& Valor(int i);
// tudo volta ao que o jogo pediu
void Restaura();
} // namespace Lmxxf2RrJogo
