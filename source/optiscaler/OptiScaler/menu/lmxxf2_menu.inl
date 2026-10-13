// [lmxxf2] Conteúdo da aba "DLSS5" no menu do OptiScaler: rede lmxxf 2, Appearance filter, iluminação RenoDX e
// desempenho. A janela, as abas, os cartões, a escala, o tema e os atalhos são os do próprio OptiScaler; aqui só o miolo
// de cada cartão (registrados em GetMenuBoxes, aba "DLSS5"). Os valores vão direto para o Config, lido a cada quadro.
#include "dlssnr/amd/AmdBridge.h"
#include "dlssnr/amd/PresentExperimental.h" // AmdFinalImage::Status
#include "dlssnr/menu/NeuralUi.h"
#include "dlssnr/lmxxf2_autofg.h"
#include "dlssnr/lmxxf2_rrjogo.h"

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
          "do AMDNR tambem sem Model interleave.");

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

// TSR proprio (upscalers/tsr): os ajustes valem na hora; o upscaler e escolhido no seletor do cartao de upscaler
inline void Tsr(Config* c)
{
    auto* f = State::Instance().currentFeature;
    const bool ativo = f != nullptr && f->GetUpscalerType() == Upscaler::TSR;
    ImGui::PushTextWrapPos(0.0f);
    if (ativo)
        ImGui::TextDisabled("Ativo: %ux%u -> %ux%u", f->RenderWidth(), f->RenderHeight(), f->TargetWidth(),
                            f->TargetHeight());
    else
        ImGui::TextDisabled("Escolha \"TSR (lmxxf2)\" na lista de upscalers do cartao acima (troca na hora).");
    ImGui::TextDisabled("Upscaler temporal proprio no estilo do TSR da UE5: paralaxe pela profundidade, recorte de cor "
                        "do historico (corta rastro de folhagem sem vetor), anti-aliasing espacial onde nao ha "
                        "historico. Nitidez: cartao Sharpness (RCAS).");
    ImGui::PopTextWrapPos();
    SliderF("Amostras acumuladas", c->TsrMaxSamples, 2.f, 64.f,
            "Quantos quadros o historico soma no maximo. Menos: responde rapido, menos rastro, mais serrilhado/ruido.\n"
            "Mais: imagem mais estavel e lisa, mais rastro em coisa que se mexe sem vetor.", "%.0f");
    SliderF("Rigidez do recorte", c->TsrClipGamma, 0.25f, 3.f,
            "Largura da caixa de cor que o historico pode ter. Menos: corta mais rastro (folhagem), pode tremer.\n"
            "Mais: mais estavel, deixa mais rastro.");
    SliderF("Sensibilidade a desoclusao", c->TsrParallax, 0.1f, 4.f,
            "Quanto a diferenca de profundidade entre quadros descarta o historico (objeto saindo da frente).");
    SliderF("Anti-aliasing espacial", c->TsrSpatialAA, 0.f, 1.f,
            "Suaviza bordas onde o historico foi descartado (o pixel novo nao tem quadros anteriores).");
    SliderF("Protecao contra cintilacao", c->TsrFlicker, 0.f, 1.f,
            "Rejeicao com deteccao de cintilacao: pixel cuja luz oscila quadro a quadro (brilho especular, ruido, a\n"
            "edicao da rede DLSS5 tremendo) segura o historico em vez de piscar; mudanca de verdade continua cortando.\n"
            "0 = desligada.");
    SliderF("Movimento maximo protegido", c->TsrFlickerMaxVel, 0.5f, 32.f,
            "Acima deste movimento (pixels por quadro) a protecao some: estabilizar algo que anda deixaria rastro.",
            "%.1f");
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
