// [lmxxf2] Conteúdo da aba "DLSS5" no menu do OptiScaler: rede lmxxf 2, Appearance filter, iluminação RenoDX e
// desempenho. A janela, as abas, os cartões, a escala, o tema e os atalhos são os do próprio OptiScaler; aqui só o miolo
// de cada cartão (registrados em GetMenuBoxes, aba "DLSS5"). Os valores vão direto para o Config, lido a cada quadro.
#include "dlssnr/amd/AmdBridge.h"
#include "dlssnr/amd/PresentExperimental.h" // AmdFinalImage::Status
#include "dlssnr/menu/NeuralUi.h"
#include "dlssnr/lmxxf2_autofg.h"
#include "dlssnr/lmxxf2_rrjogo.h"
#include "dlssnr/lmxxf/LmxxfTierPolicy.h" // Lmxxf::FreeNetworkSize, a previa do tamanho
#include <algorithm>

namespace Lmxxf2Menu
{
// contexto dos cartões de Desempenho/Qualidade do AMDNR (dlssnr/menu/NeuralPerfQuality.cpp, usados como estão)
inline DlssNr::NeuralUi::Ctx CtxNeural(Config* c)
{
    DlssNr::NeuralUi::Ctx x;
    x.config = c;
    x.menuResScale = ImGui::GetFontSize() / 13.0f;
    x.enabled = c->DlssNrEnabled.value_or_default();
    x.lmxxfMenu = true;
    return x;
}

inline void Dica(const char* help)
{
    if (help && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", help);
}

inline void SliderF(const char* label, CustomOptional<float>& v, float lo, float hi, const char* help = nullptr,
                    const char* fmt = "%.2f")
{
    float x = v.value_or_default();
    if (ImGui::SliderFloat(label, &x, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp))
        v = x;
    Dica(help);
}

inline void Check(const char* label, CustomOptional<bool>& v, const char* help = nullptr)
{
    bool x = v.value_or_default();
    if (ImGui::Checkbox(label, &x))
        v = x;
    Dica(help);
}

inline void Rede(Config* c)
{
    // topo do AMDNR: ligar/desligar (atalho Home), runtime, status ao vivo e avisos (dlssnr/menu/NeuralTop.cpp)
    const auto ctxNr = CtxNeural(c);
    DlssNr::NeuralUi::DrawTop(ctxNr);
    DlssNr::NeuralUi::DrawAttention(ctxNr);
    ImGui::Spacing();

    // [lmxxf2] Tudo o que muda a rede em si fica aqui (antes espalhado por Image look e Qualidade, com cópias):
    // entrada do modelo, força da saída, edição da rede e a exposição que ela recebe.
    ImGui::SeparatorText("Entrada da rede");
    int estilo = c->AmdRuntimeStyle.value_or_default() + 1; // -1 = automático (visual NVIDIA)
    if (ImGui::Combo("Estilo", &estilo, "Automatico (padrao NVIDIA)\0Padrao NVIDIA (0)\0Natural (1)\0Cinematico (2)\0\0"))
        c->AmdRuntimeStyle = estilo - 1;
    Dica("Canal de estilo da primeira camada da rede. Nao e o \"NR style\" do cartao Preset (aquele e um conjunto de valores).");

    SliderF("Tom", c->DlssNrLocalTone, 0.f, 4.f, "Canal de tom da rede (o mais forte; age mais em rostos). 1 = original.");
    SliderF("Estrutura", c->DlssNrLocalStructure, 0.f, 4.f, "Estrutura do cenário (com a máscara ligada) ou da imagem toda (desligada). 1 = original.");
    float pele = c->DlssNrSkinStructure.value_or_default();
    bool peleAuto = pele < 0.f;
    if (ImGui::Checkbox("Pele automática", &peleAuto))
        c->DlssNrSkinStructure = peleAuto ? -1.f : 1.f;
    if (!peleAuto && ImGui::SliderFloat("Estrutura da pele", &pele, 0.f, 4.f, "%.2f"))
        c->DlssNrSkinStructure = pele;
    Check("Máscara de personagem automática", c->DlssNrAutoMask,
          "Mascara de personagem da propria rede: ligada, Estrutura age no cenario e a pele tem a sua; desligada, "
          "Estrutura age na imagem toda.");

    ImGui::SeparatorText("Forca da saida da rede");
    {
        // Forças do runtime (composição Classic; a composição RenoDX do AMDNR saiu, a nossa é a iluminação RenoDRT/ACES)
        SliderF("Forca do detalhe", c->AmdDetailStrength, 0.f, 1.f,
                "Quanto o brilho vai na direcao da resposta da rede: 1 = a rede inteira, 0 = o brilho do proprio jogo.\n"
                "Dentro do runtime (transfer strength), vale do proximo quadro da rede.");
        SliderF("Forca da cor", c->AmdColourStrength, 0.f, 1.f,
                "A cor da propria rede (1) ou o matiz do jogo com o brilho da rede (0). Baixe se a rede mudar demais as "
                "cores.");
    }
    SliderF("Forca do residuo", c->AmdResidualIntensity, 0.f, 2.f,
            "Quanto da mudanca da rede chega na imagem. 1 = como a rede fez; acima de 1 amplifica; 0 = imagem do jogo.");
    SliderF("Limite do residuo", c->AmdResidualLimit, 0.f, 2.f,
            "Teto de quanto um pixel pode mudar, em fracao do brilho dele. Manchas: baixe. 0 = sem limite (com 2 ou 3 "
            "passes ainda limita em 1).");

    ImGui::SeparatorText("Edicao da rede");
    SliderF("Detalhe da edicao", c->AmdLmxxfEditDetail, 0.f, 2.f,
            "Ganho na parte fina da edicao da rede (a edicao menos a media local). 1 = como a rede fez; acima destaca o "
            "re-render fino, abaixo fica so a mudanca de tom.");
    SliderF("Cor da edicao", c->AmdLmxxfEditSaturation, 0.f, 2.f,
            "Cor da edicao contra a mudanca de brilho. 0 = matiz original (corrige cor lavada), 1 = como a rede fez, "
            "2 = dobro.");
    SliderF("Protecao de borda", c->AmdLmxxfEdgeGuard, 0.f, 1.f,
            "Apaga a edicao atravessando bordas de profundidade, para a luz de uma superficie nao vazar como halo no "
            "objeto da frente. 0 = desligado, 1 = nenhuma edicao atravessa.");
    Check("Rejeicao estilo TSR no arrasto", c->AmdLmxxfTsrCarry,
          "A edicao da rede chega um quadro depois e e arrastada pelos vetores de movimento. Ligado: ela so e carregada\n"
          "onde a cor crua do quadro dela bate com a deste, com tolerancia ao ruido aprendida por pixel (como a\n"
          "rejeicao de sombreamento com deteccao de cintilacao do TSR): o que so pisca segura a edicao, o que mudou de\n"
          "verdade (folha mexendo sem vetor, desoclusao) perde a edicao em vez de deixar rastro. E o Edit accumulation\n"
          "do AMDNR tambem sem Model interleave. Com o TSR da rede ligado (cartao Resolucao e TSR da rede) e ele que\n"
          "cuida da edicao; isto vale com ele desligado.");

    if (ImGui::TreeNode("Exposicao que a rede recebe"))
    {
        Check("Auto-exposicao", c->AmdLmxxfAutoExposure,
              "Sem textura de exposicao do jogo, a rede recebe a imagem numa exposicao estimada (brilho estavel); a edicao "
              "e dividida de volta, o tom do jogo fica.");
        ImGui::BeginDisabled(!c->AmdLmxxfAutoExposure.value_or_default());
        Check("Teto de realces da auto-exposicao", c->AmdLmxxfAutoExposureHighlightCap,
              "Evita que poucos pixels muito claros escurecam o quadro todo.");
        ImGui::EndDisabled();
        Check("Protecao de cor nos realces", c->AmdLmxxfHighlightChromaGuard,
              "Realces quase brancos mantem o brilho da rede e a cor do jogo (nao ficam cinza).");
        const int uso = c->AmdUseGameExposure.value_or_default();
        int item = uso == 1 ? 1 : uso == 0 ? 2 : 0;
        if (ImGui::Combo("Exposicao do jogo", &item, "Auto\0Usar\0Ignorar\0\0"))
            c->AmdUseGameExposure = item == 1 ? 1 : item == 2 ? 0 : -1;
        Dica("Se a rede recebe a textura de exposicao do jogo. Auto usa quando o jogo publica uma; Ignorar entrega para a "
             "auto-exposicao.");
        ImGui::TreePop();
    }

    // [lmxxf2] onde a rede entra e como a imagem chega no tamanho dela
    ImGui::SeparatorText("Onde a rede entra");
    int pos = std::clamp(c->Lmxxf2Placement.value_or_default(), 0, 2);
    if (ImGui::Combo("Posicao da rede", &pos,
                     "Antes do upscale (resolucao de render)\0Depois do upscale (resolucao de saida)\0"
                     "Modo captura de video (experimental)\0\0"))
        c->Lmxxf2Placement = pos;
    Dica("Antes: a rede edita a imagem que o upscaler recebe. Depois: a rede edita a saida ja ampliada.\n"
         "Com Ray Reconstruction (FSR-RR) a rede vai sempre para depois: antes do denoiser a cor ainda tem ruido.\n"
         "Modo captura de video: a rede roda no quadro pronto, na hora de mostrar (como sobre um video ou uma captura), sem\n"
         "depender do upscaler do jogo. Serve para jogo sem upscaler ou em que os outros modos falham. Limites: sem\n"
         "vetores de movimento nem profundidade (sem historico, pode tremer/arrastar), o HUD entra na imagem, e com\n"
         "FG cada quadro mostrado passa pela rede. SDR, HDR10 e scRGB. Trocar de/para este modo: reabra o jogo.");
    if (pos == 2)
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", AmdFinalImage::Status().c_str());
        if (DlssNr::AmdBridge::BackendActive())
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.f, 1.f),
                               "A rede ja rodou pelo upscaler nesta sessao: o modo captura de video so assume quando o jogo reabrir.");
        ImGui::PopTextWrapPos();
    }
    {
        const auto st = DlssNr::AmdBridge::Stats();
        if (st.width != 0 && st.height != 0)
            ImGui::TextDisabled("A rede esta recebendo %ux%u", st.width, st.height);
        else
        {
            ImGui::TextDisabled("A rede ainda nao recebeu quadro (tamanho aparece quando ela rodar).");
            // a rede entra no upscaler do OptiScaler: sem upscaler escolhido no jogo ela não roda
            const std::string ups = Lmxxf2AutoFg::Upscalers();
            if (!ups.empty())
                ImGui::TextDisabled("No menu do jogo escolha um upscaler que ele trouxe: %s.", ups.c_str());
        }
    }
    {
        // mesma regra do Evaluate: Ray Reconstruction ativa (o FSR-RR se declara FFX; o diagnóstico dele diz se é RR)
        auto* f = State::Instance().currentFeature;
        FSRDRuntimeSnapshot rrSnap;
        const bool rr = f != nullptr && (f->GetUpscalerType() == Upscaler::DLSSD ||
                                         (f->ReadRayRegenerationDiagnostics(rrSnap) && rrSnap.rayReconstruction));
        const bool depois = rr || pos == 1;
        const bool naFinal = pos == 2;
        const float esc = c->AmdNrScale.value_or_default();
        if (naFinal)
            ImGui::TextDisabled("Cadeia: CAPTURA DO QUADRO (Present) > REDE > tela");
        else
            ImGui::TextDisabled("Cadeia: %s > REDE (resolucao da rede %.0f%%)%s", depois ? "DEPOIS_UPSCALE" : "ANTES_UPSCALE",
                                esc * 100.f, rr && pos == 0 ? "  [RR ativo: depois]" : "");
    }
    ImGui::Spacing();
}

inline void Filtro(Config* c)
{
    Check("Ligar o filtro", c->AmdLookEnabled);
    int forca = std::clamp(c->AmdLookStrength.value_or_default(), 1, 4) - 1;
    if (ImGui::Combo("Força", &forca, "1x (normal)\0" "2x\0" "3x\0" "4x\0\0"))
        c->AmdLookStrength = forca + 1;
    Dica("Multiplica a mudança que o filtro faz na imagem.");
    SliderF("Mistura", c->AmdLookMix, 0.f, 1.f);
    SliderF("Detalhe de material", c->AmdLookMaterialDetail, 0.f, 2.f);
    SliderF("Definição de forma", c->AmdLookShapeDefinition, 0.f, 2.f);
    SliderF("Luz local", c->AmdLookLocalLighting, 0.f, 2.f);
    SliderF("Microestrutura da pele", c->AmdLookSkinDetail, 0.f, 2.f);
    SliderF("Suavidade do brilho na pele", c->AmdLookSkinSoftness, 0.f, 1.f);
    SliderF("Redução de especular", c->AmdLookSpecularControl, 0.f, 1.f);
    SliderF("Compressão de realces", c->AmdLookHighlightRollOff, 0.f, 1.f);
    SliderF("Separação de cor de material", c->AmdLookColourSeparation, 0.f, 1.f);
    SliderF("Sombras de contato", c->AmdLookShadowDepth, 0.f, 1.f);
    SliderF("Proteção de halo", c->AmdLookAntiHalo, 0.f, 1.f, "Alto = limita a mudança perto das bordas (deixa o filtro mais fraco).");
    SliderF("Proteção de áreas lisas", c->AmdLookFlatAreaProtection, 0.f, 1.f);
    Check("Detectar pele", c->AmdLookDetectSkin);
    if (ImGui::TreeNode("Curva de tom do filtro"))
    {
        SliderF("Força da curva", c->AmdLookTone, 0.f, 1.f);
        SliderF("Exposição (EV)", c->AmdLookExposureEV, -3.f, 3.f);
        SliderF("Contraste", c->AmdLookContrast, .5f, 1.5f);
        SliderF("Saturação", c->AmdLookSaturation, 0.f, 2.f);
        SliderF("Compressão de altas luzes", c->AmdLookHighlightCompression, 0.f, 1.f);
        ImGui::TreePop();
    }
    int ver = int(c->AmdLookInspect.value_or_default());
    if (ImGui::Combo("Ver", &ver, "Imagem final\0Máscara de pele\0Resíduo de material\0Luz local\0\0"))
        c->AmdLookInspect = uint32_t(ver);
}

inline void Luz(Config* c)
{
    Check("Ligar a iluminação", c->Lmxxf2LightEnabled);
    int tm = std::clamp(c->Lmxxf2LightToneMapper.value_or_default(), 0, 2);
    if (ImGui::Combo("Tone mapper", &tm, "Nenhum (só graduação)\0RenoDRT\0ACES\0\0"))
        c->Lmxxf2LightToneMapper = tm;
    SliderF("Exposição", c->Lmxxf2LightExposure, 0.f, 4.f);
    SliderF("Realces", c->Lmxxf2LightHighlights, 0.f, 2.f);
    SliderF("Sombras", c->Lmxxf2LightShadows, 0.f, 2.f);
    SliderF("Contraste", c->Lmxxf2LightContrast, 0.f, 2.f);
    SliderF("Saturação", c->Lmxxf2LightSaturation, 0.f, 2.f);
    SliderF("Mistura", c->Lmxxf2LightMix, 0.f, 1.f);
    if (tm != 0 && ImGui::TreeNode("RenoDRT / ACES"))
    {
        SliderF("Dechroma", c->Lmxxf2LightDechroma, 0.f, 1.f);
        SliderF("Flare", c->Lmxxf2LightFlare, 0.f, 1.f);
        SliderF("Blowout", c->Lmxxf2LightBlowout, 0.f, 1.f);
        SliderF("Correção de matiz", c->Lmxxf2LightHueStrength, 0.f, 1.f);
        SliderF("Brilho de pico (nits)", c->Lmxxf2LightPeakNits, 80.f, 10000.f, nullptr, "%.0f");
        SliderF("Brilho do jogo (nits)", c->Lmxxf2LightGameNits, 80.f, 1000.f, nullptr, "%.0f");
        SliderF("Cinza médio (nits)", c->Lmxxf2LightMidGrayNits, 1.f, 100.f, nullptr, "%.1f");
        SliderF("Correção de gamma", c->Lmxxf2LightGamma, 0.f, 2.f);
        ImGui::TreePop();
    }
}

// [lmxxf2] GI: o RTGI original do neural-amd-opti (Caliel666, GPL-3.0), com todos os ajustes ([AmdRtgi]; AmdPreSr::RtgiNative
// roda junto da rede no lmxxf). Um GI só: o AMDNR Screen GI saiu do menu e do Evaluate a pedido do dono.
inline void Gi(Config* c)
{
    // shaders do RTGI (neural-amd-opti, GPL-3.0; o pacote traz): <pasta do dxgi.dll>\experimental_lighting
    static bool shaders = false;
    static ULONGLONG visto = 0;
    if (visto == 0 || GetTickCount64() - visto > 2000)
    {
        visto = GetTickCount64();
        std::error_code ec;
        const auto d = Util::DllPath().parent_path() / L"experimental_lighting";
        shaders = std::filesystem::exists(d / L"GatherCS.cso", ec) && std::filesystem::exists(d / L"ResolveCS.cso", ec);
    }

    bool lig = c->AmdRtgiEnabled.value_or_default();
    if (ImGui::Checkbox("GI de tela (RTGI)", &lig))
    {
        c->AmdRtgiEnabled = lig;
    }
    Dica("Luz rebatida e oclusao ambiente tracadas em espaco de tela a partir da profundidade do jogo.\n"
         "RTGI original do neural-amd-opti (Caliel666, GPL-3.0, github.com/Caliel666/neural-amd-opti); roda junto da\n"
         "rede DLSS5.");
    if (!shaders)
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.f, 1.f),
                           "Faltam experimental_lighting\\GatherCS.cso e ResolveCS.cso ao lado do dxgi.dll\n"
                           "(o pacote traz a pasta; copie-a junto com o dxgi.dll).");
    if (lig && !c->DlssNrEnabled.value_or_default())
        ImGui::TextDisabled("O RTGI roda junto da rede: ligue a rede DLSS5.");

    if (lig)
    {
        ImGui::Indent();
        int amostras = (static_cast<int>(std::min(c->AmdRtgiQuality.value_or_default(), 15u)) + 1) * 8;
        if (ImGui::SliderInt("Amostras por pixel", &amostras, 8, 128, "%d", ImGuiSliderFlags_AlwaysClamp))
            c->AmdRtgiQuality = static_cast<uint32_t>(std::clamp((amostras + 4) / 8, 1, 16) - 1);
        Dica("Amostras de luz por pixel (de 8 em 8). Mais = mais limpo e mais caro; o custo cresce junto.\n"
             "Os niveis antigos: Very low 8, Low 16, Medium 24, High 32, Ultra 40.");
        int den = static_cast<int>(std::min(c->AmdRtgiDenoiser.value_or_default(), 2u));
        if (ImGui::Combo("Denoiser", &den, "Baixo\0Medio\0Alto\0\0"))
            c->AmdRtgiDenoiser = static_cast<uint32_t>(den);
        Dica("Quanto a luz tracada e filtrada.");
        SliderF("Mistura", c->AmdRtgiMix, 0.f, 1.f, "Quanto do efeito chega na imagem; 0 = nada.");
        SliderF("Luz rebatida", c->AmdRtgiLighting, 0.f, 10.f, "Brilho da luz rebatida.");
        SliderF("Oclusao ambiente", c->AmdRtgiOcclusion, 0.f, 10.f, "Escurecimento em cantos e frestas.");
        SliderF("Nivel ambiente", c->AmdRtgiAmbient, .25f, 1.f, "Nivel da luz ambiente; 1 = padrao.");
        SliderF("Sombra de contato", c->AmdRtgiContact, 0.f, 2.f, "Sombra extra onde objetos se encostam; 0 = desligada.");
        SliderF("Saturacao da luz rebatida", c->AmdRtgiSaturation, 0.f, 2.f, "Forca da cor da luz rebatida; 1 = como tracada.");
        SliderF("Raio de busca", c->AmdRtgiRadius, .25f, 3.f, "Ate onde cada pixel procura luz.");
        SliderF("Espessura dos objetos", c->AmdRtgiThickness, 0.f, 1.f,
                "Quanto as superficies sao consideradas grossas atras do que a profundidade mostra.");
        SliderF("Suavidade", c->AmdRtgiSmoothness, 0.f, 1.f, "Quanto o resultado e suavizado.");
        SliderF("Faixa de fade", c->AmdRtgiFade, .001f, 1.f, "Quao suave o efeito some na distancia.", "%.3f");

        // câmera: FOV e distância do jogo (FSR / Streamline / DLSS-FG) enquanto em auto; senão os valores à mão
        bool camAuto = !c->AmdRtgiFov.value_for_config().has_value() && !c->AmdRtgiFarPlane.value_for_config().has_value();
        if (ImGui::Checkbox("FOV e distancia do jogo (automatico)", &camAuto))
        {
            if (camAuto)
            {
                c->AmdRtgiFov.reset();
                c->AmdRtgiFarPlane.reset();
            }
            else
            {
                c->AmdRtgiFov = c->AmdRtgiFov.value_or_default();
                c->AmdRtgiFarPlane = c->AmdRtgiFarPlane.value_or_default();
            }
        }
        Dica("Ligado: o FOV vertical e o far plane vem da camera que o jogo passa ao upscaler (FSR, Streamline ou\n"
             "DLSS-FG). Se o jogo nao passa, fica o padrao 60 / 600: desligue e ajuste a mao.");
        ImGui::BeginDisabled(camAuto);
        SliderF("FOV da camera", c->AmdRtgiFov, 20.f, 140.f, "FOV vertical do jogo, em graus.", "%.0f");
        SliderF("Distancia (far plane)", c->AmdRtgiFarPlane, 10.f, 10000.f, "Far plane do jogo.", "%.0f");
        ImGui::EndDisabled();

        int ver = static_cast<int>(std::min(c->AmdRtgiInspect.value_or_default(), 1u));
        if (ImGui::Combo("Ver##rtgi", &ver, "Imagem final\0So a luz do GI\0\0"))
            c->AmdRtgiInspect = static_cast<uint32_t>(ver);
        if (ImGui::Button("Restaurar padrao do GI"))
        {
            c->AmdRtgiQuality.reset();
            c->AmdRtgiDenoiser.reset();
            c->AmdRtgiInspect.reset();
            c->AmdRtgiContact.reset();
            c->AmdRtgiSaturation.reset();
            c->AmdRtgiRadius.reset();
            c->AmdRtgiMix.reset();
            c->AmdRtgiLighting.reset();
            c->AmdRtgiOcclusion.reset();
            c->AmdRtgiAmbient.reset();
            c->AmdRtgiThickness.reset();
            c->AmdRtgiSmoothness.reset();
            c->AmdRtgiFade.reset();
            c->AmdRtgiFov.reset();
            c->AmdRtgiFarPlane.reset();
        }
        ImGui::Unindent();
    }
}

// Ray Regeneration do jogo (denoiser AMD FFX 1.2 que o próprio jogo cria): os seis ajustes do denoiser por cima do que
// o jogo pede, aplicados antes do próximo dispatch dele (dlssnr/lmxxf2_rrjogo.cpp)
inline void RrJogo(Config* c)
{
    const auto e = Lmxxf2RrJogo::Le();
    if (e.contextos == 0 && e.configures == 0 && e.dispatches == 0)
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("O jogo ainda nao usou o Ray Regeneration da AMD nesta sessao. Nos jogos que trazem "
                            "(ex.: Crimson Desert), ligue FSR Ray Regeneration no menu do jogo.");
        ImGui::PopTextWrapPos();
    }
    else
    {
        // dispatches/s: zero com o jogo rodando = o jogo parou de usar o denoiser (opção desligada, cena sem RT)
        ImGui::TextDisabled("Ativo: %d contexto(s), %.0f dispatches/s, %ux%u, %llu ajustes vindos do jogo", e.contextos,
                            e.dispatchesPorSeg, e.renderW, e.renderH, (unsigned long long) e.configures);
        if (e.criado)
        {
            static const char* const kSinais[] = { "oclusao ambiente", "difusa direta", "especular direta",
                                                   "luz dominante", "difusa indireta", "especular indireta",
                                                   "oclusao especular" };
            std::string sinais;
            for (int b = 0; b < 7; b++)
                if (e.sinais & (1u << b))
                    sinais += (sinais.empty() ? "" : ", ") + std::string(kSinais[b]) +
                              ((e.sinaisXadrez & (1u << b)) ? " (xadrez)" : "");
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("O jogo denoisa: %s | max %ux%u%s", sinais.empty() ? "-" : sinais.c_str(), e.maxW,
                                e.maxH, (e.flagsCriacao & 1u) ? " | debug ligado" : "");
            ImGui::PopTextWrapPos();
        }
    }
    if (!e.erro.empty())
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.f, 1.f), "%s", e.erro.c_str());

    bool aj = c->RrJogoAjustar.value_or_default();
    if (ImGui::Checkbox("Ajustar o Ray Regeneration do jogo", &aj))
        c->RrJogoAjustar = aj;
    Dica("Desligado: o denoiser roda com o que o jogo pede. Ligado: cada ajuste que voce mexer substitui o do jogo;\n"
         "os marcados (jogo) seguem o jogo. Vale na hora. E o denoiser do proprio jogo, nao o FSR-RR do OptiScaler\n"
         "(esse fica no cartao do upscaler e substitui o DLSS Ray Reconstruction).");

    // padrão do provedor AMD 1.2 (medido no Crimson Desert), se ele não respondeu à consulta
    static constexpr float kAmd[Lmxxf2RrJogo::kChaves] = { 1.0f, 1.0f, 65504.0f, 50.0f, 0.0f, 0.01f };
    for (int i = 0; i < Lmxxf2RrJogo::kChaves; i++)
    {
        const auto& k = Lmxxf2RrJogo::Chave(i);
        auto& v = Lmxxf2RrJogo::Valor(i);
        const float base = e.jogoDefiniu[i] ? e.jogo[i] : e.padraoOk[i] ? e.padrao[i] : kAmd[i];
        float x = v.has_value() ? v.value() : base;
        ImGui::PushID(i);
        ImGuiSliderFlags fl = ImGuiSliderFlags_AlwaysClamp;
        if (k.logaritmico)
            fl |= ImGuiSliderFlags_Logarithmic;
        if (ImGui::SliderFloat(k.nome, &x, k.min, k.max, k.formato, fl))
        {
            v = x;
            c->RrJogoAjustar = true; // mexer liga o ajuste
        }
        Dica(k.dica);
        ImGui::SameLine();
        if (v.has_value())
        {
            if (ImGui::SmallButton("jogo"))
                v.reset();
            Dica("Volta ao valor que o jogo pede");
        }
        else
        {
            ImGui::TextDisabled("(jogo)");
        }
        ImGui::PopID();
    }

    std::string pediu, padrao;
    for (int i = 0; i < Lmxxf2RrJogo::kChaves; i++)
    {
        pediu += (i ? " / " : "") + (e.jogoDefiniu[i] ? std::format("{:g}", e.jogo[i]) : std::string("-"));
        padrao += (i ? " / " : "") + (e.padraoOk[i] ? std::format("{:g}", e.padrao[i]) : std::string("-"));
    }
    ImGui::TextDisabled("Jogo pediu: %s", pediu.c_str());
    ImGui::TextDisabled("Padrao AMD: %s", padrao.c_str());

    if (ImGui::Button("Recomecar historico##rrjogo"))
        Lmxxf2RrJogo::PedeReset();
    Dica("O \"Reset\" do sample da AMD: o proximo quadro do denoiser comeca sem o acumulado dos anteriores.\n"
         "Use depois de mexer muito nos ajustes, ou se ficar rastro/mancha parada na imagem.");
    if (e.resets > 0)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(%llu)", (unsigned long long) e.resets);
    }
}

// [lmxxf2] Resolucao da rede (livre: o tamanho exato de 720p a 4K, sem os degraus 720/900/1080) e TSR da rede
// (dlssnr/lmxxf/EditTsr.h: acumulo temporal da EDICAO da rede; o upscaler do jogo e o quadro dele nao mudam)
inline bool FreeResVivo()
{
    char v[4] {};
    return GetEnvironmentVariableA("DLSS5_NETWORK_FREE_RES", v, sizeof v) == 1 && v[0] == '1';
}

inline void ResolucaoTsr(Config* c)
{
    const AmdPreSr::Stats st = DlssNr::AmdBridge::Stats();
    const bool livre = FreeResVivo();
    ImGui::PushTextWrapPos(0.0f);

    ImGui::SeparatorText("Resolucao da rede");
    Check("Resolucao livre (exata, ate 4K)", c->AmdLmxxfFreeRes,
          "Ligado: a rede roda exatamente o tamanho pedido, de 320 px ate a superficie 4K do runtime (sem degraus).\n"
          "Desligado: os 3 degraus antigos (720 / 900 / 1080) e o teto de 1920x1080.\n"
          "O runtime le isto uma vez por processo: vale ao reabrir o jogo.");
    if (c->AmdLmxxfFreeRes.value_or_default() != livre)
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.f, 1.f), "Reabra o jogo para valer (este processo esta com %s).",
                           livre ? "a resolucao livre" : "os degraus");

    // Screen Percentage da rede: pela vazao alvo do TSR, ou o NR resolution (o mesmo valor do cartao Desempenho), exato
    Check("Screen Percentage pela vazao do TSR", c->TsrFeedAuto,
          "Como o \"feed\" do TSR da UE5: vazao = pixels da rede x respostas por segundo. Ligado, o Screen Percentage e\n"
          "o que da a vazao alvo com o FPS medido, entre o minimo e o maximo abaixo (revisto a cada 3 s; cada troca\n"
          "recria a rede, um soluco). FPS baixo pede rede maior, que custa mais: o maximo segura.\n"
          "Desligado: o Screen Percentage e o NR resolution, exato.");
    if (c->TsrFeedAuto.value_or_default())
    {
        SliderF("Vazao alvo (MP/s)", c->TsrFeedMps, 30.f, 400.f,
                "A referencia da Epic e 124,4 MP/s: 1080p a 60 Hz, ou 4K a 50% a 60 Hz. Mais: a edicao da rede converge\n"
                "mais rapido para a resolucao do quadro, a rede fica maior.", "%.1f");
        SliderF("Screen Percentage minimo", c->TsrFeedSpMin, 25.f, 150.f, "% de cada eixo do quadro.", "%.0f%%");
        SliderF("Screen Percentage maximo", c->TsrFeedSpMax, 25.f, 150.f,
                "% de cada eixo do quadro. Acima de 100%% a rede recebe uma copia ampliada (custa, nao ganha detalhe).",
                "%.0f%%");
    }
    else
    {
        static float edita = 100.f;
        static bool editando = false;
        if (!editando)
            edita = c->AmdNrScale.value_or_default() * 100.f;
        ImGui::SliderFloat("Screen Percentage da rede", &edita, 25.f, 150.f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
        editando = ImGui::IsItemActive();
        // uma recriacao da rede ao soltar, nao uma por movimento do mouse; passos de 5% como o NR resolution
        if (ImGui::IsItemDeactivatedAfterEdit())
            c->AmdNrScale = std::round(edita / 5.f) * 5.f / 100.f;
        Dica("O mesmo valor do NR resolution (cartao Desempenho da rede), agora exato: cada eixo do quadro x SP. Na saida\n"
             "4K: 50% = 1920x1080, 67% = 2560x1440, 100% = 3840x2160. Com \"Antes do upscale\" o quadro e o de render.");
    }
    if (!livre)
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.f, 1.f),
                           "Sem a resolucao livre neste processo: degraus 720/900/1080 e teto 1080p (NR resolution como antes).");
    if (st.frameW > 0 && st.frameH > 0)
    {
        const double frac = double(st.width) * st.height / (double(st.frameW) * st.frameH);
        ImGui::TextDisabled("Rede agora: %ux%u = Screen Percentage %.0f%% do quadro %ux%u%s", st.width, st.height,
                            100.0 * std::sqrt(frac), st.frameW, st.frameH, livre ? " (exato)" : " (degraus)");
        if (livre)
        {
            unsigned pw = 0, ph = 0;
            Lmxxf::FreeProcessing(st.width, st.height, pw, ph);
            ImGui::TextDisabled("Processamento %ux%u, ~%.1fx o custo da rede em 1080p; vazao %.1f MP/s a %.1f respostas/s%s",
                                pw, ph, Lmxxf::FreeCostVs1080(st.width, st.height), double(st.width) * st.height * st.answersPerSec / 1e6,
                                st.answersPerSec, st.feedAuto ? " (alvo acima)" : "");
        }
    }

    ImGui::SeparatorText("TSR da rede");
    Check("TSR na edicao da rede", c->TsrRede,
          "O acumulo temporal do TSR (UE5) aplicado na EDICAO da rede DLSS5, nao no quadro do jogo: o upscaler (FSR4)\n"
          "continua o mesmo e o quadro dele fica intacto. A edicao e medida na resolucao da rede contra o que ela recebeu\n"
          "(antes a resposta esticada substituia o quadro: era o borrado) e acumulada na resolucao do quadro, com\n"
          "paralaxe pela profundidade, cor crua contra cor crua (folha sem vetor perde a edicao em vez de deixar rastro)\n"
          "e deteccao de cintilacao. Desligado: o arrasto do AMDNR (Edit accumulation).");
    Check("Jitter na entrada da rede", c->TsrJitter,
          "Rede menor que o quadro: cada alimentacao amostra o quadro deslocada de uma fracao de pixel (Halton 2,3), entao\n"
          "cada resposta cobre outras posicoes e a edicao converge para a resolucao do quadro com o tempo (o \"feed\" do\n"
          "TSR). O historico proprio da rede recebe a troca de jitter junto com os vetores. Desligue para comparar.");
    Check("Ressurreicao de historico", c->TsrResurrection,
          "O \"Resurrection\" do TSR: dois quadros persistentes (a cada 31 quadros) guardam a edicao e a cor crua; onde o\n"
          "historico e descartado, a edicao volta do quadro persistente se a vizinhanca da cor crua bate (objeto que passou\n"
          "na frente e saiu). O TSR da Epic reprojeta esses quadros pelas matrizes da camera, que o jogo nao entrega ao\n"
          "upscaler: aqui vale no mesmo lugar da tela, entao ajuda com a camera parada; andando, nao bate e nada volta.");
    if (st.tsrActive && st.frameW > 0 && st.width > 0)
    {
        const double frac = double(st.width) * st.height / (double(st.frameW) * st.frameH);
        const double mps = double(st.width) * st.height * st.answersPerSec / 1e6;
        const double ms1 = frac * st.answersPerSec > 0.0 ? 1000.0 / (frac * st.answersPerSec) : 0.0;
        const float amostras = c->TsrMaxSamples.value_or_default();
        ImGui::TextDisabled("Ativo: rede %ux%u -> quadro %ux%u, %s", st.width, st.height, st.frameW, st.frameH,
                            st.tsrJitter ? "com jitter" : "sem jitter (rede do tamanho do quadro ou jitter desligado)");
        ImGui::TextDisabled("Vazao: %.1f MP/s (%.1f respostas/s) - 1 amostra por pixel a cada %.0f ms, %.0f amostras em %.2f s",
                            mps, st.answersPerSec, ms1, amostras, ms1 * amostras / 1000.0);
    }
    else
        ImGui::TextDisabled("Inativo (desligado, modo sem vetores de movimento ou rede parada).");
    SliderF("Amostras no historico", c->TsrMaxSamples, 2.f, 32.f,
            "Quantas amostras (em pixels de saida) o historico da edicao soma no maximo. Menos: responde rapido a mudanca\n"
            "da rede. Mais: edicao mais estavel e mais fina. A conversao em tempo esta na linha Vazao acima.", "%.0f");
    SliderF("Rigidez do recorte", c->TsrClipGamma, 0.25f, 3.f,
            "Quanto o historico da edicao pode se afastar do que a resposta nova diz em volta do pixel. Menos: menos atraso\n"
            "quando a rede muda de ideia, pode tremer. Mais: mais estavel.");
    SliderF("Sensibilidade a desoclusao", c->TsrParallax, 0.f, 4.f,
            "Quanto a diferenca de profundidade entre quadros descarta a edicao (objeto saindo da frente). 0 = desligada.");
    SliderF("Protecao contra cintilacao", c->TsrFlicker, 0.f, 1.f,
            "Pixel cuja luz oscila quadro a quadro (brilho especular, ruido, a resposta da rede tremendo) segura a edicao\n"
            "em vez de piscar; mudanca de verdade continua cortando. Conta em tempo (60 fps de referencia): igual com\n"
            "FreeSync / VRR. 0 = desligada.");
    SliderF("Movimento maximo protegido", c->TsrFlickerMaxVel, 0.5f, 32.f,
            "Acima deste movimento (pixels do quadro por quadro) a protecao contra cintilacao some: segurar o que anda\n"
            "deixaria rastro.", "%.1f");
    ImGui::PopTextWrapPos();
}

// Créditos do compilado (Help / Status). O aviso do AMDNR pede o crédito visível no menu e a versão marcada como
// modificada; os outros autores, cada um pela sua parte
inline void Creditos()
{
    struct Linha
    {
        const char* quem;
        const char* oque;
        const char* onde;
    };
    static const Linha kLinhas[] = {
        { "AMDNR by 3zwr1", "nucleo neural, ponte AMD, menu da rede, passes temporais (GPL-3.0 + termos 7b/7c/7e)",
          "github.com/3zwr1/AMD-NR---OptiScaler" },
        { "OptiScaler", "base do projeto: upscalers, FG, ganchos, menu (GPL-3.0)", "github.com/optiscaler/OptiScaler" },
        { "lmxxf (Kien)", "porte da rede DLSS5 para AMD, kernels HIP e runtime (MIT)",
          "github.com/lmxxf/dlss5-on-amd-9070xt-porting" },
        { "TheAutomatic, Dagherbou, wilsjo2, Matheus", "linhagem DLSS-NR pre-SR que o AMDNR herdou (GPL-3.0)", "" },
        { "guentra", "conversor dos pesos da DLL (MIT)", "github.com/guentra/dlss5-amd-hip-linux" },
        { "Zach Hembree (DarkHelmet) e burak113", "FSR Ray Regeneration para o OptiScaler (GPL-3.0)",
          "github.com/burak113/OptiScaler" },
        { "Caliel666", "RTGI original e porte do desbloqueio de multi-frame do XeFG (GPL-3.0)",
          "github.com/Caliel666/neural-amd-opti" },
        { "Coldwood1026", "XeFGUnlock / pacing do XeFG (GPL-3.0)", "github.com/Coldwood1026/OptiScalerDp4aUnlock" },
        { "RenoDX (clshortfuse)", "matematica RenoDRT / ACES da iluminacao (MIT)", "github.com/clshortfuse/renodx" },
        { "Nukem9", "dlssg-to-fsr3 (GPL-3.0)", "github.com/Nukem9/dlssg-to-fsr3" },
        { "DLSS Enabler (Artur Graniszewski)", "gerador multi-frame pelo nvngx (binario, termos proprios)",
          "github.com/artur-graniszewski/DLSS-Enabler" },
        { "OptiPatcher", "plugin .asi que libera DLSS sem spoof (MIT)", "github.com/optiscaler/OptiPatcher" },
        { "AMD, Intel, NVIDIA, Microsoft", "FidelityFX, XeSS, Streamline, DirectX (licencas proprias)", "" },
        { "NVIDIA", "o desenho da rede DLSS 5 e os pesos pertencem a NVIDIA", "" },
    };
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted("Compilado AMD DLSS5 - versao MODIFICADA do OptiScaler e do AMDNR.");
    ImGui::TextDisabled("Sem afiliacao com NVIDIA, AMD ou Intel.");
    for (const auto& l : kLinhas)
    {
        ImGui::Bullet();
        ImGui::TextUnformatted(l.quem);
        ImGui::SameLine();
        ImGui::TextDisabled("- %s%s%s", l.oque, l.onde[0] ? " | " : "", l.onde);
    }
    ImGui::PopTextWrapPos();
}

inline void Desempenho()
{
    const float fps = ImGui::GetIO().Framerate;
    ImGui::Text("FPS %.1f  |  quadro %.2f ms", fps, fps > 0 ? 1000.f / fps : 0.f);
    const bool ligada = Config::Instance()->DlssNrEnabled.value_or_default();
    ImGui::Text("Rede: %s", !ligada ? "desligada" : DlssNr::AmdBridge::BackendActive() ? "rodando" : "iniciando");
}
} // namespace Lmxxf2Menu
