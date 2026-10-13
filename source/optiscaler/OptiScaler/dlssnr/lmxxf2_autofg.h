#pragma once
// [lmxxf2] FG automático. Na carga, antes de o OptiScaler fixar o FG da sessão (dllmain, "Initial state of FG"), olha
// que arquivos de FG e de upscaler o PRÓPRIO JOGO trouxe (fora da pasta do OptiScaler) e, se o ini deixou FGInput e
// FGOutput em auto, escolhe o modo que funciona:
//   jogo com DLSS Frame Generation (sl.dlss_g.dll / nvngx_dlssg.dll) -> entrada NvngxFG + o melhor gerador disponível
//                                                                        (Combo = FFX + Enabler, multi-frame até 6x)
//   jogo só com FSR 3 FG ou XeFG                                     -> nada: o FG do jogo passa direto
//   jogo sem FG                                                      -> nada (o FG do OptiScaler fica como modo manual)
// Os valores vão como voláteis (set_volatile_value): o ini continua "auto" e cada sessão decide de novo. O modo de FG
// não troca no meio do jogo (o OptiScaler instala ganchos diferentes por entrada e monta o FG na criação do swapchain).
#include <Config.h>

#include <string>

namespace Lmxxf2AutoFg
{
// Arquivos do jogo achados na varredura (caminho completo; vazio = não tem)
struct Achados
{
    bool varrido = false;
    std::wstring dlssg; // sl.dlss_g.dll ou nvngx_dlssg.dll
    std::wstring dlssd; // nvngx_dlssd.dll ou sl.dlss_d.dll (DLSS Ray Reconstruction)
    std::wstring ffxFg; // amd_fidelityfx_framegeneration_dx12.dll, amd_fidelityfx_dx12.dll (FSR 3.1) ou FSR 3.0
    std::wstring xefg;  // libxess_fg.dll
    std::wstring dlss;  // nvngx_dlss.dll
    std::wstring fsr;   // amd_fidelityfx_upscaler_dx12.dll, amd_fidelityfx_dx12.dll ou ffx_fsr2_api_x64.dll
    std::wstring xess;  // libxess.dll
    bool vulkan = false; // o exe importa vulkan-1.dll e não d3d12.dll (Combo/FFX não rodam em Vulkan)
    unsigned pastas = 0;
    unsigned ms = 0;
};

struct Proposta
{
    FGInput in = FGInput::NoFG;
    FGOutput out = FGOutput::NoFG;
    FGNvngxReplacement subst = FGNvngxReplacement::None;
    std::string motivo;
};

// Varre uma vez (na primeira chamada) e devolve o que achou
const Achados& Detectados();
// O que o automático escolhe com os arquivos achados e os geradores da pasta OptiScaler (não aplica nada)
Proposta Calcula();
// Uma vez no dllmain: aplica a proposta se FGInput e FGOutput estão em auto no ini; também os ajustes de HUD do
// DLSS Enabler ([NvngxFG] DispatchFlags em auto -> o conjunto recomendado)
void Aplica();
// Uma vez no dllmain, depois das regras por jogo (ApplyGameConfigs): com [Spoofing] Dxgi em auto, liga o spoof NVIDIA
// só quando o jogo não traz FSR (o upscaler só entra pelo DLSS); jogo com FSR fica no caminho AMD, sem spoof
void AplicaSpoof();
// Para a UI: o que o spoof automático fez e por quê
std::string Spoof();
// Os ajustes de HUD do DLSS Enabler que o automático usa: Antighosting (GB), Temporal HUD pin, HUD interpolation,
// Ignore UI texture e Pin backbuffer
inline constexpr uint32_t kEnablerHudFlags = 0x5C100000u;
// O automático decidiu o FG desta sessão
bool Aplicado();
// Botão de reset do cartão de FG: FG, HUD do Enabler e spoof voltam ao automático (o que o "Automatico" da UI faz).
// O modo de FG e o spoof valem ao abrir o jogo de novo
void Reaplica();
// "DLSS-FG, FSR 3 FG, DLSS, FSR, XeSS" (o que o jogo trouxe), para a UI
std::string Resumo();
// Os upscalers que o jogo trouxe ("DLSS, FSR, XeSS"), para dizer o que escolher no menu do jogo
std::string Upscalers();
} // namespace Lmxxf2AutoFg
