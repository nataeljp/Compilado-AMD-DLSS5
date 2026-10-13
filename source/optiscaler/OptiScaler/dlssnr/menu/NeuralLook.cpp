// Copyright (c) 2026 3zwr1 (AMDNR)
// SPDX-License-Identifier: GPL-3.0-or-later
// moved from dlssnr/DlssNr_Menu.cpp
#include "pch.h"
#include "NeuralUi.h"
#include "RrSectionState.h" // (0.3.4.2, P2) which status the Ray Regeneration section shows

#include <dlssnr/amd/AmdBridge.h>
#include <dlssnr/lmxxf/LmxxfBackend.h> // Lmxxf::ControlsRefused
#include <hooks/Streamline_Hooks.h>    // StreamlineHooks::rrHardwareDecision (the card gate's own word)
#include <shaders/fsrd_preprocess/FSRDRuntimeStatus.h>
#include <Config.h>
#include <State.h>

#include <imgui/imgui.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

// The Neural tab's Image look and Ray Regeneration sections (plan section 4.D and 4.E, items T7/T8, W2-M3a), drawn
// as the owner-approved mock (0.3.4 menu rework; the owner-approved menu mock and
// APPROVED_MOCK.png):
//   Image look: Colour composition, Detail strength, Colour strength (Classic; RenoDX has its own rows), then the
//     folded Model strength, Exposure and highlights and Appearance filter trees ("off" / "on" after the last label;
//     "default" / "custom" after the first two, owner decision 5).
//   Ray Regeneration (0.3.4.2, P2: always drawn, its controls disabled while RR is not denoising): the status line,
//     which says which of the four states it is in (RrSectionState.h), Path-traced profile (its
//     two rows only while it is ticked), Disocclusion threshold, Bias mask strength, Skin smoothing with Strength /
//     Radius / Skin classifier as its kids, then the folded "More Ray Regeneration options" (AMD's default tuning, the
//     other five temporal values and the Skin mask tuning tree): the one row beyond the mock, so every [FSR-RR] key
//     keeps a row.
// Rows the mock does not show in its default view sit in those trees; the W3-D rows (the danielblnc runtime knobs, the
// native character mask, Game exposure, the merged Highlight colour guard) are the last rows of their tree. Rows a
// runtime cannot use are greyed or hidden through RuntimeCaps (NeuralUi Gate/Hidden). The RR debug view is in
// NeuralTools.cpp.
namespace DlssNr::NeuralUi
{
namespace
{
using RuntimeCaps::Cap;
using RuntimeCaps::Support;

// (0.3.4 W3-D) The danielblnc runtime knobs ([DlssNr] AmdRuntimeStyle / AmdToneCurve / AmdToneLift /
// AmdUseGameExposure) reach only the builds whose AmdLayout maps them (AmdPreSr::KnobsUnmapped; the public 0.3.3 and
// 0.4.0 runtimes among them): with any other build the keys do nothing (the backend logs it once per knob). -1 = no danielblnc
// runtime identified yet (nothing greyed), 0 = the installed build does not map them, 1 = mapped.
// (0.3.4, danielblnc support) The bridge answers from the identified layout (the loaded pass DLL, else pass 1 identified once),
// not by name: a build whose row is named by its digest prefix shows its file's own version in RuntimeName, which
// matches no row name. An unknown build reads 0, as before.
int DanielKnobsMapped()
{
    return DlssNr::AmdBridge::DanielKnobsMapped();
}

// The tag and the help line of a knob row the installed danielblnc build cannot take (R2: the tag stays short; no
// version names in either).
constexpr const char* kKnobsOffTag = "not in this danielblnc build";
constexpr const char* kKnobsOffNote = "Not available with the installed danielblnc build: the key does nothing there.";

// (0.3.4, HX request 4) An LmxxfNrRuntime.dll older than the native character mask controls refused them
// (Lmxxf::ControlsRefused, a latch for the process): the tag of the Full network pattern, and the full sentence in the
// hover of the three rows it concerns.
constexpr const char* kControlsRefusedTag = "refused: runtime DLL too old";
constexpr const char* kControlsRefusedNote =
    "This LmxxfNrRuntime.dll predates the native character mask: AutoMask, Structure intensity and Character structure"
    " do not act on lmxxf with it. Use the LmxxfNrRuntime.dll shipped with this OptiScaler build.";

// neuralSlider's staging (NeuralUi.h) with a number format and a clamp: the handle tracks live, the value is handed
// back once, on release or when Ctrl+Click typing ends. R9: the IsItem* reads follow the slider directly. Returns true
// on that commit. Used by Black lift: a knob change restarts the model's history, so a drag commits once.
bool StagedSlider(const char* label, float* value, float lo, float hi, const char* format)
{
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID id = ImGui::GetID(label);
    const ImGuiID activeId = id ^ 0x6e72534cu;
    float shown = storage->GetBool(activeId, false) ? storage->GetFloat(id) : *value;
    FillSlider(label, &shown, lo, hi, format, ImGuiSliderFlags_AlwaysClamp);
    const bool active = ImGui::IsItemActive();
    const bool commit = ImGui::IsItemDeactivatedAfterEdit();
    storage->SetFloat(id, shown);
    storage->SetBool(activeId, active);
    if (commit)
        *value = std::clamp(shown, lo, hi);
    return commit;
}

// (0.3.4, owner decision 5) The fold rows' dim tag: "custom" when a row drawn inside the tree for the active runtime
// (greyed rows included, hidden ones not) holds a value other than its Config.h default, else "default". "Differs"
// is value_for_config(): the value Save Settings would write (none of these keys is ever set volatile). Read only.
template <typename T>
bool IsCustom(CustomOptional<T>& key)
{
    return key.value_for_config().has_value();
}

void FoldStateTag(bool custom) { DimTag(custom ? "custom" : "default"); }

// (0.3.4.2, P2) One row of a disabled section drawn dim but still clickable. ImGui's disabled flag blocks a fold
// row's click as well, and a player who came to the Ray Regeneration section while RR is not denoising must still be
// able to open the fold and read what is in it - that is the whole point of keeping the section on screen. Construct
// it inside the section's BeginDisabled scope and around the fold row only (the scope must be the innermost one
// there): it closes the scope, keeps the greyed look through the style alpha, and opens the scope again.
class FoldRowClickable
{
  public:
    explicit FoldRowClickable(bool disabled) : disabled_(disabled)
    {
        ImGui::EndDisabled();
        if (disabled_)
        {
            const ImGuiStyle& style = ImGui::GetStyle();
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, style.Alpha * style.DisabledAlpha);
        }
    }
    ~FoldRowClickable()
    {
        if (disabled_)
            ImGui::PopStyleVar();
        ImGui::BeginDisabled(disabled_);
    }
    FoldRowClickable(const FoldRowClickable&) = delete;
    FoldRowClickable& operator=(const FoldRowClickable&) = delete;

  private:
    bool disabled_;
};

// One appearance filter slider (0.3.3.2: a local lambda of the sub-tab): a live shader constant, 2 decimals, one line
// of help on the label's hover. A named function, so menu_rows sees each row (a forwarder of FillSlider).
template <typename Option>
void LookSlider(const char* label, Option& option, float lo, float hi, const char* help, const char* ini)
{
    float value = option.value_or_default();
    if (FillSlider(label, &value, lo, hi, "%.2f"))
        option = value;
    HelpForLastItem(help, {}, ini);
}
} // namespace

// Plan E / T8: Ray Regeneration, its own section after Image look (the dispatcher draws it there). It does not
// depend on the NR runtime: both hosts run after RR (writeBack). The RR debug view is a Diagnostics row
// (NeuralTools.cpp), drawn in this section only while no runtime is installed (DrawRrDebugView); the "FSR Ray
// Regeneration:" / "RR" label prefixes are gone, the [FSR-RR] key names live in the tooltips' ini footers. The RR
// sliders clamp typed values (Ctrl+Click) to their range: the feature hands the temporal values to FFX unchecked.
// (0.3.4 menu rework) The mock's order: Path-traced profile, Disocclusion threshold, Bias mask strength, Skin smoothing
// and its kids; the rest in "More Ray Regeneration options" (MOCK-SPEC O3).
// [lmxxf2] A seção Ray Regeneration do AMDNR não entra: o FSR Ray Regeneration deste host é o do fork
// burak113/OptiScaler (upscalers/fsr31/FSRDFeature_Dx12), com o menu dele (MenuCommon::RenderDenoiserSettings).
void DrawRayRegeneration(const Ctx&) {}

void DrawImageLook(const Ctx& ctx)
{
    Config* config = ctx.config;
    const bool lmxxfMenu = ctx.lmxxfMenu;

    SectionHeader("Image look");
    // "Network output" lives under Diagnostics. It was removed from here once as a
    // no-op - at the time the colour was replaced outright, so there was nothing
    // after the model to bypass. The residual composition (strength, limit, fade),
    // the temporal pass, the appearance filter, the proxy and the sharpener all
    // exist now, and the toggle shows the model's answer with every one of them off.
    // (0.3.4, plan D) Order: Colour composition and its mode's rows, then the folded Model strength, Exposure and
    // highlights and Appearance filter trees. Tone / Structure / Character structure moved into Model strength.
    // COLOUR COMPOSITION ([DlssNr] AmdComposition), outside every lmxxf-only block on
    // purpose: both runtimes get the same control with the same meaning, and refuse it the
    // same way (parity rule). Classic (0) is the picture this build has always made - the
    // Detail / Colour strength sliders below it. RenoDX (1, experimental) is the tail of
    // dlssnr.hlsl run on the host after the model (dlssnr/amd/NrCompose.h), with its own
    // Detail / Colour keys (AmdComposeDetail 0..2, AmdComposeColour 0..4 - a 2 set here never
    // lands in Classic's 0..1 range) and the Highlight guard and skin keys shared with the
    // NVIDIA path. The Classic sliders do nothing in RenoDX mode (danielblnc skips its mix,
    // lmxxf's runtime is sent 1/1), so each mode shows only its own. NR styles and the
    // Quality / Balanced / Performance preset leave every composition key alone. A refusal
    // (display-referred frame, Network output, an lmxxf override) comes back from the
    // backend as AmdBridge::CompositionNote(); it is drawn orange.
    int composition = config->AmdComposition.value_or_default() == 1 ? 1 : 0;
    if (Combo("Colour composition", &composition, "Classic\0RenoDX (experimental)\0"))
        config->AmdComposition = static_cast<uint32_t>(composition);
    // (0.3.4, plan D1/R4) One tooltip for both runtimes; the runtime's own line comes from RuntimeCaps.
    Help("Classic: the picture as before. RenoDX (experimental): RenoDX's colour composition after the model, for"
         " HDR games; an SDR frame falls back to Classic, with a note below. NR style and Preset leave it alone.",
         { RuntimeCaps::Menu().noteComposition,
           "Credits: RenoDX composition by clshortfuse (MIT), codec reconstruction by lmxxf (MIT); shader lineage"
           " Dagherbou, wilsjo2." },
         "[DlssNr] AmdComposition");
    const bool renodx = composition == 1;
    if (renodx)
    {
        // The backend's word on this mode: why it is refused ("RenoDX refused: ..."), or what
        // it is doing, which includes where the white point comes from (title exposure,
        // estimate, lmxxf's fed units - the same thing Stats::compositionWhiteSource says,
        // so it is not repeated here). Backends clear it in Classic, but one that has stopped
        // recording cannot, so it is only read while RenoDX is selected - a stale line never
        // sits under Classic.
        const std::string note = DlssNr::AmdBridge::CompositionNote();
        if (!note.empty())
        {
            std::string lower = note;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            const bool refused = lower.find("refus") != std::string::npos ||
                                 lower.find("falls back") != std::string::npos ||
                                 lower.find("fall back") != std::string::npos;
            ImGui::PushStyleColor(ImGuiCol_Text, refused ? ImVec4(1.f, 0.55f, 0.2f, 1.f)
                                                         : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", note.c_str());
            ImGui::PopStyleColor();
            // (0.3.4, plan D1a/R3) Fix buttons: a refusal caused by a setting of ours offers one click that puts
            // that setting back to its default; the note clears on the next recorded frame. Both runtimes refuse
            // Network output first, then Encoding sRGB / Gamma 2.2, so the button follows the same order.
            if (refused)
            {
                const int encoding = config->AmdEncoding.value_or_default();
                if (config->AmdNetworkOutput.value_or_default())
                {
                    if (Button("Turn off Network output"))
                        config->AmdNetworkOutput = false;
                    HelpForLastItem("Unticks Diagnostics > Network output, which shows the model's raw answer.", {},
                                    "[DlssNr] AmdNetworkOutput");
                }
                else if (encoding == 2 || encoding == 3)
                {
                    if (Button("Encoding: Auto"))
                        config->AmdEncoding = 0;
                    HelpForLastItem("Sets Runtime options > Encoding back to Auto (the game's own colour space).", {},
                                    "[DlssNr] AmdEncoding");
                }
            }
        }
    }
    if (!renodx && lmxxfMenu)
    {
        // [lmxxf2] Força do detalhe / da cor ficam no cartão "Rede DLSS5", junto com o resto do que é da própria rede
        ImGui::TextDisabled("Forca do detalhe e da cor: cartao Rede DLSS5 (Forca da saida da rede).");
    }
    else if (!renodx)
    {
        // Detail/Colour strength apply live (post pass); at 1.0 they are inert. The range stays 0..1 (the keys'
        // clamp), so 1.00 fills the bar.
        float detail = config->AmdDetailStrength.value_or_default();
        if (FillSlider("Detail strength", &detail, 0.f, 1.f, "%.2f"))
            config->AmdDetailStrength = std::clamp(detail, 0.f, 1.f);
        Help("How far the brightness moves toward the model's answer: 1 = the full model, 0 = the frame's own"
             " brightness (the model's colour still comes through unless Colour strength is 0 too). Classic only.",
             { lmxxfMenu ? "lmxxf: the runtime's own transfer strength, from the next model frame."
                         : "danielblnc: after the appearance filter; 1 and 1 skip the mix." },
             "[DlssNr] AmdDetailStrength");
        float colourStr = config->AmdColourStrength.value_or_default();
        if (FillSlider("Colour strength", &colourStr, 0.f, 1.f, "%.2f"))
            config->AmdColourStrength = std::clamp(colourStr, 0.f, 1.f);
        Help("The model's own colour (1) or the frame's hue at the model's brightness (0). Lower it if the model shifts"
             " colours too much. Classic only.",
             { lmxxfMenu ? "lmxxf: inside the runtime's codec, from the next model frame." : nullptr },
             "[DlssNr] AmdColourStrength");
    }
    else
    {
        // RenoDX mode. The staged slider (DeferredSlider): the handle tracks live, the key is
        // written once on release, and nothing is drawn between a slider and its IsItem*
        // calls. Its Reset puts back the shipped default. Own ID scope, so these labels can
        // never collide with the NVIDIA section's identically named controls.
        ImGui::PushID("AmdCompose");
        DeferredSlider("Composition detail", &config->AmdComposeDetail, 0.0f, 2.0f, 1.0f);
        Help("How far the picture moves from the original toward the model's answer: 0 = the original, 1 = the"
             " model's answer, above 1 more of the model's change in brightness, still held by the Highlight guard."
             " Classic's Detail strength keeps its own value.",
             {}, "[DlssNr] AmdComposeDetail");
        DeferredSlider("Composition colour", &config->AmdComposeColour, 0.0f, 4.0f, 1.0f);
        Help("Whose colour arrives with the light: 0 = the original's hue, 1 = the model's colour, above 1 more"
             " vivid, eased toward neutral at the gamut edge. Near-grey scenes such as fog need 3-4 to show.",
             {}, "[DlssNr] AmdComposeColour");
        DeferredSlider("Highlight guard", &config->DlssNrMaxRatio, 1.0f, 8.0f, 2.0f, "%.1fx");
        Help("How far the model's answer may brighten or darken a pixel against the original, as a multiple (2x by"
             " default). Lower it if highlights bloom or crush; raise it only if bright areas look clipped. 8 is 8x,"
             " not off.",
             { lmxxfMenu ? "lmxxf: binds the answer before Residual strength / limit, the edit shaper and the carry."
                         : "danielblnc: binds the runtime's answer; the Look, stability and sharpening act after it." },
             "[DlssNr] MaxRatio");
        // The skin / environment final edit of dlssnr.hlsl, on the NVIDIA path's keys. No mask
        // preview on AMD (ShowSkinMask stays NVIDIA-only in this change).
        const bool skinOpen = TreeNode("Skin and environment");
        HelpForLastItem("A colour-based selection on the original, the composition's last step: skin and everything"
                        " else can each keep less of the model's light and colour. Warm scenery can be selected and"
                        " coloured light can hide skin; there is no mask preview.");
        if (skinOpen)
        {
            bool filter = config->DlssNrSkinProtection.value_or_default();
            if (Checkbox("Separate skin and environment", &filter))
                config->DlssNrSkinProtection = filter;
            // (0.3.4, HX request 5) lmxxf's native character mask acts inside the model since the AUTOMASK host step.
            Help("Splits the composition's result into skin and everything else by colour; each keeps less of the"
                 " model's light (detail) and colour. 1 / 1 = the composition as it is, 0 / 0 = the original.",
                 { lmxxfMenu ? "lmxxf: not the appearance filter's Automatic skin mask; the model's native character"
                               " mask ([DlssNr] AutoMask, on by default) and Character structure act inside the model."
                             : "danielblnc: one of four skin controls, none feeding another: this mask, the appearance"
                               " filter's Automatic skin mask, Character structure and Native character mask (both in"
                               " Model strength; [DlssNr] AutoMask, on by default)." },
                 "[DlssNr] SkinProtection");
            ImGui::BeginDisabled(!filter);
            bool tone = config->DlssNrSkinToneEnabled.value_or_default();
            if (Checkbox("Allow skin tone changes", &tone))
                config->DlssNrSkinToneEnabled = tone;
            Help("Off keeps the colour of skin pixels; their light and detail can still change. Set Skin detail to 0"
                 " as well to keep skin as it was.",
                 {}, "[DlssNr] SkinToneEnabled");
            DeferredSlider("Skin detail", &config->DlssNrSkinDetail, 0.0f, 1.0f, 1.0f);
            Help("How much of the model's light and detail lands on skin; 0 = the original.", {},
                 "[DlssNr] SkinDetail");
            {
                Gate gate(filter && !tone, "needs Allow skin tone changes");
                DeferredSlider("Skin colour", &config->DlssNrSkinColour, 0.0f, 1.0f, 1.0f);
                Help("How much of the model's colour lands on skin.", {}, "[DlssNr] SkinColour");
            }
            DeferredSlider("Environment detail", &config->DlssNrEnvironmentDetail, 0.0f, 1.0f, 1.0f);
            Help("How much of the model's light and detail lands on everything that is not skin.", {},
                 "[DlssNr] EnvironmentDetail");
            DeferredSlider("Environment colour", &config->DlssNrEnvironmentColour, 0.0f, 1.0f, 1.0f);
            Help("How much of the model's colour lands on everything that is not skin.", {},
                 "[DlssNr] EnvironmentColour");
            ImGui::EndDisabled();
            ImGui::TreePop();
        }
        ImGui::PopID();
    } // renodx

    // [lmxxf2] No menu do lmxxf este cartão é só a composição da saída. Model strength (Estrutura, Pele, Máscara e o
    // Tone intensity que o lmxxf não usa) repetia o cartão Rede DLSS5, que agora tem também a edição da rede e a
    // exposição que ela recebe; Appearance filter repetia o cartão Appearance filter.
    if (lmxxfMenu)
        return;

    // Plan D3: Model strength, folded. The same rows in the same place on every runtime (R10); a row the runtime
    // cannot use yet is greyed with RuntimeCaps' tag. Tone / Structure / Character structure are the model's own
    // strengths (staged: the model is rebuilt on release; lmxxf takes Structure and Character structure since the
    // AUTOMASK host step); Edit detail / Edit colour / Edge guard are lmxxf's edit shaper (LmxxfBackend.cpp's residual
    // shader), live. Output smoothing moved to Quality > More quality options (plan C5b, NeuralPerfQuality.cpp).
    // (0.3.4 W3-D) Last in the tree: danielblnc's runtime knobs (Network style, Tone curve, Black lift) and the native
    // character mask ([DlssNr] AutoMask).
    // (0.3.4, owner decision 5) The tree's tag: "custom" when a row it draws for this runtime is off its default.
    const bool hideKnobs = Hidden(Cap::RuntimeKnobs);
    const bool hideMask = Hidden(Cap::NativeCharacterMask);
    const bool strengthCustom =
        IsCustom(config->AmdNeuralLightingStrength) || IsCustom(config->DlssNrLocalStructure) ||
        IsCustom(config->DlssNrSkinStructure) || IsCustom(config->AmdLmxxfEditDetail) ||
        IsCustom(config->AmdLmxxfEditSaturation) || IsCustom(config->AmdLmxxfEdgeGuard) ||
        (!hideKnobs &&
         (IsCustom(config->AmdRuntimeStyle) || IsCustom(config->AmdToneCurve) || IsCustom(config->AmdToneLift))) ||
        (!hideMask && IsCustom(config->DlssNrAutoMask));
    const bool strengthOpen = TreeNode("Model strength");
    HelpForLastItem("The model's own strengths (Tone, Structure, Character structure), the edit shaper (Edit detail,"
                    " Edit colour, Edge guard), danielblnc's runtime knobs (Network style, Tone curve, Black lift) and"
                    " the native character mask. Each runtime uses the rows it supports.");
    FoldStateTag(strengthCustom);
    if (strengthOpen)
    {
        // (0.3.4, HX request 4) lmxxf only: an older LmxxfNrRuntime.dll refused the character mask controls.
        const bool ctlRefused = lmxxfMenu && Lmxxf::ControlsRefused();
        {
            Gate gate(Cap::Tone);
            neuralSlider("Tone intensity", config->AmdNeuralLightingStrength, 0, 2);
            Help("How strongly the model relights the scene; 1 = the model's own. Applies on release (the model is"
                 " rebuilt).",
                 {}, "[DlssNr] AmdNeuralLightingStrength");
        }
        {
            // Range opened from 2 to 4. Two was reported as still too subtle to see without toggling the effect on
            // and off; if the model clamps internally, the slider simply stops doing anything past that point.
            Gate gate(Cap::Structure);
            neuralSlider("Structure intensity", config->DlssNrLocalStructure, 0, 4);
            Help("How much surface structure the model adds; 1 = the model's own. Applies on release (the model is"
                 " rebuilt).",
                 { ctlRefused ? kControlsRefusedNote : nullptr }, "[DlssNr] LocalStructure");
        }
        {
            // (0.3.4, plan D3) Follow Structure: SkinStructure -1 (the Config.h default) follows Structure
            // intensity. 0.3.3.2 drew the slider on the raw value, so -1 showed as the slider's floor and could
            // never be set back. Ticked, the slider shows Structure's value greyed (a local stand-in: one call
            // site, nothing written); unticking writes Structure's current value as the start of its own.
            Gate gate(Cap::CharacterStructure);
            const bool follow = config->DlssNrSkinStructure.value_or_default() < 0.f;
            CustomOptional<float> followShown { config->DlssNrLocalStructure.value_or_default() };
            ImGui::BeginDisabled(follow);
            neuralSlider("Character structure", follow ? followShown : config->DlssNrSkinStructure, 0, 4);
            ImGui::EndDisabled();
            Help("Structure on characters (faces, skin). Follow Structure (the default) uses Structure intensity;"
                 " untick it to give characters their own strength. Applies on release (the model is rebuilt).",
                 { ctlRefused ? kControlsRefusedNote : nullptr }, "[DlssNr] SkinStructure (-1 = follow)");
            ImGui::SameLine();
            bool followBox = follow;
            if (Checkbox("Follow Structure", &followBox))
                config->DlssNrSkinStructure = followBox ? -1.0f : config->DlssNrLocalStructure.value_or_default();
            HelpForLastItem("Ticked (the default): Character structure follows Structure intensity. Untick it to give"
                            " characters their own strength, starting at Structure's value.",
                            {}, "[DlssNr] SkinStructure (-1 = follow)");
        }
        {
            Gate gate(Cap::EditShaper);
            float ed = config->AmdLmxxfEditDetail.value_or_default();
            if (FillSlider("Edit detail", &ed, 0.f, 2.f, "%.2f"))
                config->AmdLmxxfEditDetail = std::clamp(ed, 0.f, 2.f);
            Help("Gain on the fine part of the model's edit (the edit minus its local average). 1 = as the model made"
                 " it; above 1 the fine re-rendering stands out more, below it only the broad tonal change is kept.",
                 {}, "[DlssNr] AmdLmxxfEditDetail");
        }
        {
            Gate gate(Cap::EditShaper);
            float es = config->AmdLmxxfEditSaturation.value_or_default();
            if (FillSlider("Edit colour", &es, 0.f, 2.f, "%.2f"))
                config->AmdLmxxfEditSaturation = std::clamp(es, 0.f, 2.f);
            Help("The edit's colour against its brightness change. 0 keeps the original hue (fixes washed-out"
                 " colour), 1 = as the model made it, 2 = doubled.",
                 {}, "[DlssNr] AmdLmxxfEditSaturation");
        }
        {
            Gate gate(Cap::EditShaper);
            float eg = config->AmdLmxxfEdgeGuard.value_or_default();
            if (FillSlider("Edge guard", &eg, 0.f, 1.f, "%.2f"))
                config->AmdLmxxfEdgeGuard = std::clamp(eg, 0.f, 1.f);
            Help("Fades the edit across depth edges, so one surface's relighting cannot bleed onto the object in"
                 " front as a halo. 0 = off, 1 = no edit across an edge.",
                 {}, "[DlssNr] AmdLmxxfEdgeGuard");
        }

        // (0.3.4 W3-D, DANIEL-033-SET) danielblnc's runtime knobs, -1 = auto (the host writes nothing and the runtime
        // keeps its own value, 0.3.3.2). Each row writes only its own key; a change restarts the model's history.
        // Greyed "not in this danielblnc build" when the installed build does not map them (DanielKnobsMapped).
        if (!hideKnobs)
        {
            const bool knobsOff = !lmxxfMenu && DanielKnobsMapped() == 0;
            {
                Gate gate(Cap::RuntimeKnobs, knobsOff, kKnobsOffTag);
                const int style = config->AmdRuntimeStyle.value_or_default();
                int item = style >= 0 && style <= 2 ? style + 1 : 0;
                if (Combo("Network style", &item, "Auto\0Style 0\0Style 1\0Style 2\0"))
                    config->AmdRuntimeStyle = item - 1;
                Help("danielblnc's own network style, in its own names 0 Default, 1 Natural, 2 Cinematic (not the NR"
                     " style above). Auto keeps the runtime's own value. A change restarts the model's history.",
                     { knobsOff ? kKnobsOffNote : nullptr }, "[DlssNr] AmdRuntimeStyle (-1 = auto)");
            }
            {
                Gate gate(Cap::RuntimeKnobs, knobsOff, kKnobsOffTag);
                const int curve = config->AmdToneCurve.value_or_default();
                int item = curve == 0 || curve == 1 ? curve + 1 : 0;
                if (Combo("Tone curve", &item, "Auto\0Reinhard (soft)\0ACES (filmic)\0"))
                    config->AmdToneCurve = item - 1;
                Help("The tone curve danielblnc's runtime encodes the frame with for the model: Reinhard (soft) or"
                     " ACES (filmic). Auto keeps the runtime's own choice. A change restarts the model's history.",
                     { knobsOff ? kKnobsOffNote : nullptr }, "[DlssNr] AmdToneCurve (-1 = auto)");
            }
            {
                // The Follow Structure pattern: Auto ticked = -1, the slider greyed and never written; unticking
                // writes 0 as the start of an own value. The slider is staged (a drag commits once, on release).
                Gate gate(Cap::RuntimeKnobs, knobsOff, kKnobsOffTag);
                const float stored = config->AmdToneLift.value_or_default();
                const bool autoLift = stored < 0.f;
                float lift = autoLift ? 0.f : std::clamp(stored, 0.f, 0.25f);
                ImGui::BeginDisabled(autoLift);
                if (StagedSlider("Black lift", &lift, 0.f, 0.25f, "%.3f"))
                    config->AmdToneLift = lift;
                ImGui::EndDisabled();
                Help("Lifts the blacks in danielblnc's runtime codec. It acts on the first pass only. Auto keeps the"
                     " runtime's own value. Applies on release; a change restarts the model's history.",
                     { knobsOff ? kKnobsOffNote : nullptr }, "[DlssNr] AmdToneLift (-1 = auto)");
                ImGui::SameLine();
                bool autoBox = autoLift;
                if (Checkbox("Auto##blackLift", &autoBox))
                    config->AmdToneLift = autoBox ? -1.0f : 0.0f;
                HelpForLastItem("Ticked (the default): the runtime keeps its own black lift. Untick it to set one"
                                " here, starting at 0.",
                                {}, "[DlssNr] AmdToneLift (-1 = auto)");
            }
        }

        // (0.3.4, MN3 / W3-D) The model's native character mask ([DlssNr] AutoMask, default on): danielblnc's charMask
        // input (AM-DAN) and lmxxf's character mask controls (the AUTOMASK host step). Its help promises no visible
        // change (AutoMask plan P7).
        if (!hideMask)
        {
            Gate gate(Cap::NativeCharacterMask, ctlRefused, kControlsRefusedTag, /*keepClickable*/ ctlRefused);
            bool mask = config->DlssNrAutoMask.value_or_default();
            if (Checkbox("Native character mask", &mask))
                config->DlssNrAutoMask = mask;
            Help("The model's own mask for characters (faces, skin). On by default; off treats every pixel alike. Not"
                 " the appearance filter's Automatic skin mask.",
                 { lmxxfMenu ? "lmxxf: a change rebuilds the network (about a second)."
                             : "danielblnc: a change restarts the model's history.",
                   ctlRefused ? kControlsRefusedNote : nullptr },
                 "[DlssNr] AutoMask");
        }
        HiddenCount({ hideKnobs ? "Network style" : nullptr, hideKnobs ? "Tone curve" : nullptr,
                      hideKnobs ? "Black lift" : nullptr, hideMask ? "Native character mask" : nullptr },
                    "They belong to the other runtime only.");
        ImGui::TreePop();
    }

    // Plan D4: Exposure and highlights, folded. danielblnc's auto-exposure lives inside its closed runtime: the
    // switch is drawn ticked and locked there, and the highlight cap (which only shapes our own estimate) is hidden
    // and counted. (0.3.4 menu rework) The cap is not indented (the mock); W3-D adds Game exposure as the last row and
    // makes Highlight colour guard one row for both runtimes.
    // (0.3.4, owner decision 5) The tree's tag, over the rows it draws for this runtime: Auto-exposure only where it is
    // a switch (danielblnc draws it ticked and locked), the highlight cap unless hidden, the guard row's active key.
    const bool aeAlways = RuntimeCaps::Get(Cap::AutoExposureSwitch) == Support::Always;
    const bool hideCap = Hidden(Cap::AutoExposureCap);
    CustomOptional<bool>& guardKey = lmxxfMenu ? config->AmdLmxxfHighlightChromaGuard : config->AmdDanielHighlightGuard;
    const bool exposureCustom = (!aeAlways && IsCustom(config->AmdLmxxfAutoExposure)) ||
                                (!hideCap && IsCustom(config->AmdLmxxfAutoExposureHighlightCap)) ||
                                IsCustom(guardKey) || IsCustom(config->AmdUseGameExposure);
    const bool exposureOpen = TreeNode("Exposure and highlights");
    HelpForLastItem("How the model is fed exposure when the game publishes none, how bright highlights keep their"
                    " colour, and whether the game's own exposure is used.");
    FoldStateTag(exposureCustom);
    if (exposureOpen)
    {
        const bool ae = config->AmdLmxxfAutoExposure.value_or_default();
        {
            Gate gate(Cap::AutoExposureSwitch);
            bool shown = aeAlways || ae;
            if (Checkbox("Auto-exposure", &shown) && !aeAlways)
                config->AmdLmxxfAutoExposure = shown;
            Help("When the game publishes no exposure texture, the model is fed at an estimated exposure, so it sees a"
                 " steady brightness; the edit is divided back, so the game's own tone stays. Off feeds the colour as"
                 " it is. Next model frame.",
                 {}, "[DlssNr] AmdLmxxfAutoExposure");
        }
        if (!hideCap)
        {
            Gate gate(Cap::AutoExposureCap, !ae, "needs Auto-exposure");
            bool hlc = config->AmdLmxxfAutoExposureHighlightCap.value_or_default();
            if (Checkbox("Auto-exposure highlight cap", &hlc))
                config->AmdLmxxfAutoExposureHighlightCap = hlc;
            Help("Stops a few very bright pixels from darkening the whole frame: past 8% of the picture near"
                 " white it stops raising the exposure, past 25% it may lower it faster. Next model frame.",
                 {}, "[DlssNr] AmdLmxxfAutoExposureHighlightCap");
        }
        {
            // (0.3.4 W3-D, 7.1) One row, the active runtime's key (the Network history pattern): lmxxf
            // AmdLmxxfHighlightChromaGuard (default on), danielblnc AmdDanielHighlightGuard (DANIEL-GUARD, default
            // off). The danielblnc cell of Cap::HighlightColourGuard is Yes since G3 (RuntimeCaps.h), so the row is live
            // there; were it Planned again, the row would grey but a ticked danielblnc key would stay clickable, so a
            // value set in the ini could be undone (R3). guardKey: the active runtime's key (above the tree).
            bool hcg = guardKey.value_or_default();
            Gate gate(Cap::HighlightColourGuard, false, nullptr, /*keepClickable*/ !lmxxfMenu && hcg);
            if (Checkbox("Highlight colour guard", &hcg))
                guardKey = hcg;
            Help("Keeps bright highlights from turning grey: where the fed pixel is near white, the answer keeps its"
                 " brightness and takes the game's colour; darker pixels are untouched. Next model answer.",
                 { lmxxfMenu ? nullptr
                             : "danielblnc: off by default; the first tick compiles a shader (a short hitch)." },
                 lmxxfMenu ? "[DlssNr] AmdLmxxfHighlightChromaGuard" : "[DlssNr] AmdDanielHighlightGuard");
        }
        {
            // (0.3.4 W3-D, 7.2, EXPO-PARITY) [DlssNr] AmdUseGameExposure, both runtimes: -1 auto, 1 use, 0 ignore.
            // On danielblnc it is one of the runtime knobs, so a build that does not map them greys it.
            const bool knobsOff = !lmxxfMenu && DanielKnobsMapped() == 0;
            Gate gate(Cap::UseGameExposure, knobsOff, kKnobsOffTag);
            const int use = config->AmdUseGameExposure.value_or_default();
            int item = use == 1 ? 1 : use == 0 ? 2 : 0;
            if (Combo("Game exposure", &item, "Auto\0Use\0Ignore\0"))
                config->AmdUseGameExposure = item == 1 ? 1 : item == 2 ? 0 : -1;
            Help("Whether the model is fed the game's exposure texture. Auto uses it when the game publishes one."
                 " Ignore: the runtime's own auto-exposure takes over. Next model frame.",
                 { lmxxfMenu ? nullptr : "danielblnc: Auto keeps the runtime's own setting.",
                   knobsOff ? kKnobsOffNote : nullptr },
                 "[DlssNr] AmdUseGameExposure (-1 = auto)");
        }
        HiddenCount({ hideCap ? "Auto-exposure highlight cap" : nullptr },
                    "It shapes AMDNR's own exposure estimate; danielblnc's auto-exposure runs inside its closed"
                    " runtime.");
        ImGui::TreePop();
    }

    // Plan D5: the appearance filter, folded, with its state after the header's label (the mock's dim tag).
    const bool lookOpen = TreeNode("Appearance filter");
    HelpForLastItem("A look filter after the neural pass: material, skin and lighting emphasis, a tone curve, and"
                    " a reset that touches only the filter.");
    DimTag(config->AmdLookEnabled.value_or_default() ? "on" : "off");
    if (lookOpen)
    {
        DrawAppearanceFilter(ctx);
        ImGui::TreePop();
    }
}

// Plan D5: the rows of the appearance filter tree at the end of Image look (DrawImageLook draws the tree header
// with its on/off word). Until 0.3.3.2 this was the tools row's fourth sub-tab. Every slider is a shader constant
// and applies live, as before; one line of help each, on the slider's label hover (19 rows).
void DrawAppearanceFilter(const Ctx& ctx)
{
    Config* config = ctx.config;

    bool lookEnabled = config->AmdLookEnabled.value_or_default();
    if (Checkbox("Enable appearance filter", &lookEnabled))
        config->AmdLookEnabled = lookEnabled;
    Help("A look filter after the neural pass (not a neural model): material, skin and lighting emphasis, and a"
         " tone curve. Off by default.",
         {}, "[AmdLook] Enabled");
    ImGui::BeginDisabled(!lookEnabled);
    LookSlider("Effect mix", config->AmdLookMix, 0, 1,
               "How much of the filtered picture replaces the frame; 0 = no detail change (the tone curve still acts).",
               "[AmdLook] Mix");
    LookSlider("Material detail", config->AmdLookMaterialDetail, 0, 2,
               "Gain on fine texture detail: a pixel against its close neighbours.", "[AmdLook] MaterialDetail");
    LookSlider("Shape definition", config->AmdLookShapeDefinition, 0, 2,
               "Gain on the broader shapes around a pixel.", "[AmdLook] ShapeDefinition");
    LookSlider("Local lighting", config->AmdLookLocalLighting, 0, 2,
               "Local light and shadow: a pixel against its blurred surroundings, eased in highlights.",
               "[AmdLook] LocalLighting");
    LookSlider("Skin microstructure", config->AmdLookSkinDetail, 0, 2,
               "Fine detail on the pixels the Automatic skin mask marks as skin.", "[AmdLook] SkinDetail");
    LookSlider("Skin highlight softness", config->AmdLookSkinSoftness, 0, 1,
               "How much Specular reduction also acts on skin; 1 = the same as elsewhere.", "[AmdLook] SkinSoftness");
    LookSlider("Specular reduction", config->AmdLookSpecularControl, 0, 1,
               "Dims small bright highlights that look like plastic.", "[AmdLook] SpecularControl");
    LookSlider("Highlight roll-off", config->AmdLookHighlightRollOff, 0, 1,
               "Softens the brightest tones and eases the detail gain in them.", "[AmdLook] HighlightRollOff");
    LookSlider("Material colour separation", config->AmdLookColourSeparation, 0, 1,
               "Lifts the colour of materials a little (less on skin).", "[AmdLook] ColourSeparation");
    LookSlider("Contact shadows", config->AmdLookShadowDepth, 0, 1,
               "Darkens pixels that are darker than their surroundings, like a contact shadow.",
               "[AmdLook] ShadowDepth");
    LookSlider("Halo protection", config->AmdLookAntiHalo, 0, 1,
               "Keeps each change inside its neighbours' range, so edges get no halos.", "[AmdLook] AntiHalo");
    LookSlider("Flat-area protection", config->AmdLookFlatAreaProtection, 0, 1,
               "No detail gain on flat areas, so their noise is not raised.", "[AmdLook] FlatAreaProtection");
    bool detectSkin = config->AmdLookDetectSkin.value_or_default();
    if (Checkbox("Automatic skin mask", &detectSkin))
        config->AmdLookDetectSkin = detectSkin;
    HelpForLastItem("Finds skin by colour for the two skin rows; off treats every pixel alike. Not the neural"
                    " model's own character mask.",
                    {}, "[AmdLook] DetectSkin");
    ImGui::Spacing();
    ImGui::TextDisabled("%s", "Tone curve");
    LookSlider("Tonemap strength", config->AmdLookTone, 0, 1,
               "How much of the tone curve below is applied; 0 = off.", "[AmdLook] Tone");
    LookSlider("Exposure (EV)", config->AmdLookExposureEV, -3, 3, "Exposure before the curve, in stops.",
               "[AmdLook] ExposureEV");
    LookSlider("Contrast", config->AmdLookContrast, 0.5, 1.5, "Contrast around mid-grey; 1 = unchanged.",
               "[AmdLook] Contrast");
    LookSlider("Saturation", config->AmdLookSaturation, 0, 2, "Colour saturation; 1 = unchanged.",
               "[AmdLook] Saturation");
    LookSlider("Highlight compression", config->AmdLookHighlightCompression, 0, 1,
               "Compresses bright areas before the curve.", "[AmdLook] HighlightCompression");
    int inspect = static_cast<int>(config->AmdLookInspect.value_or_default());
    if (Combo("Inspect", &inspect, "Final image\0Skin mask\0Material residual\0Local lighting\0"))
        config->AmdLookInspect = static_cast<uint32_t>(inspect);
    HelpForLastItem("Shows one of the filter's inputs instead of the picture. Final image to play.", {},
                    "[AmdLook] Inspect");
    ImGui::EndDisabled();
    // (0.3.4, plan D5/T7) Resets the [AmdLook] keys only, to their Config.h defaults (the values 0.3.3.2 wrote,
    // key for key). 0.3.3.2's button also wrote Passes, NR resolution, Encoding, RunBeforeSR, LocalStructure,
    // SkinStructure (1, default -1), AmdNeuralLighting and Tone intensity (AmdNeuralLightingStrength 0.5, default
    // 1.0); those lines are gone. The history reset stays, as before.
    if (Button("Reset appearance filter"))
    {
        config->AmdLookEnabled = std::optional<bool> {};
        config->AmdLookAppearance = std::optional<uint32_t> {};
        config->AmdLookMix = std::optional<float> {};
        config->AmdLookMaterialDetail = std::optional<float> {};
        config->AmdLookShapeDefinition = std::optional<float> {};
        config->AmdLookLocalLighting = std::optional<float> {};
        config->AmdLookSkinDetail = std::optional<float> {};
        config->AmdLookSkinSoftness = std::optional<float> {};
        config->AmdLookDetectSkin = std::optional<bool> {};
        config->AmdLookSpecularControl = std::optional<float> {};
        config->AmdLookHighlightRollOff = std::optional<float> {};
        config->AmdLookColourSeparation = std::optional<float> {};
        config->AmdLookShadowDepth = std::optional<float> {};
        config->AmdLookAntiHalo = std::optional<float> {};
        config->AmdLookFlatAreaProtection = std::optional<float> {};
        config->AmdLookInspect = std::optional<uint32_t> {};
        config->AmdLookTone = std::optional<float> {};
        config->AmdLookExposureEV = std::optional<float> {};
        config->AmdLookContrast = std::optional<float> {};
        config->AmdLookSaturation = std::optional<float> {};
        config->AmdLookHighlightCompression = std::optional<float> {};
        DlssNr::AmdBridge::InvalidateHistory();
    }
    HelpForLastItem("Every appearance filter setting back to its default, and the filter off. Nothing outside the"
                    " filter changes.",
                    {}, "[AmdLook] *");
}
} // namespace DlssNr::NeuralUi
