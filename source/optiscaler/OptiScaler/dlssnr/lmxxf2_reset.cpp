// [lmxxf2] GERADO por tools/optiscaler_fork/gera_reset_cartoes.py - não editar à mão.
// Botão "R" de cada cartão do menu: volta ao padrão os membros do Config que o cartão mexe.
#include "pch.h"
#include "Config.h"
#include "dlssnr/lmxxf2_autofg.h"
#include "dlssnr/lmxxf2_reset.h"

#include <cstring>

namespace Lmxxf2Reset
{
namespace
{
// operator= com optional vazio: sai do modo volátil e fica sem valor (o padrão / auto), como o menu faz
template <class T, HasDefaultValue D> void Zera(CustomOptional<T, D>& o) { o = std::optional<T> {}; }

struct Cartao
{
    const char* id;
    int chaves;
    void (*zera)(Config*);
};

const Cartao kCartoes[] = {
        { "lmxxf2_rede", 20, [](Config* c) {
            Zera(c->AmdColourStrength);
            Zera(c->AmdDetailStrength);
            Zera(c->AmdFinalImage);
            Zera(c->AmdLmxxfAutoExposure);
            Zera(c->AmdLmxxfAutoExposureHighlightCap);
            Zera(c->AmdLmxxfEdgeGuard);
            Zera(c->AmdLmxxfEditDetail);
            Zera(c->AmdLmxxfEditSaturation);
            Zera(c->AmdLmxxfHighlightChromaGuard);
            Zera(c->AmdLmxxfTsrCarry);
            Zera(c->AmdResidualIntensity);
            Zera(c->AmdResidualLimit);
            Zera(c->AmdRuntimeStyle);
            Zera(c->AmdUseGameExposure);
            Zera(c->AmdVkLateCopyWait);
            Zera(c->DlssNrAutoMask);
            Zera(c->DlssNrLocalStructure);
            Zera(c->DlssNrLocalTone);
            Zera(c->DlssNrSkinStructure);
            Zera(c->Lmxxf2Placement);
        } },
        { "lmxxf2_tsr", 14, [](Config* c) {
            Zera(c->AmdLmxxfFreeRes);
            Zera(c->AmdLmxxfNetHeight);
            Zera(c->TsrClipGamma);
            Zera(c->TsrFeedMps);
            Zera(c->TsrFlicker);
            Zera(c->TsrFlickerMaxVel);
            Zera(c->TsrFpsAlvo);
            Zera(c->TsrJitter);
            Zera(c->TsrMaxSamples);
            Zera(c->TsrModo);
            Zera(c->TsrParallax);
            Zera(c->TsrRede);
            Zera(c->TsrResurrection);
            Zera(c->TsrRitmoMin);
        } },
        { "lmxxf2_desempenho_amdnr", 11, [](Config* c) {
            Zera(c->AmdDanielFastMode);
            Zera(c->AmdDynamicRes);
            Zera(c->AmdDynamicTargetFps);
            Zera(c->AmdInterleave);
            Zera(c->AmdInterleavePacing);
            Zera(c->AmdInterleavePreset);
            Zera(c->AmdLmxxfFastMode);
            Zera(c->AmdLmxxfTierCap);
            Zera(c->AmdNrScale);
            Zera(c->DlssNrPasses);
            Zera(c->LmxxfFullNetwork);
        } },
        { "lmxxf2_qualidade_amdnr", 9, [](Config* c) {
            Zera(c->AmdLmxxfHistory);
            Zera(c->AmdLmxxfOutputSmooth);
            Zera(c->AmdResidualFade);
            Zera(c->AmdResidualTemporal);
            Zera(c->AmdSharpness);
            Zera(c->AmdStabilityMode);
            Zera(c->AmdStabilityStaticRelax);
            Zera(c->AmdStabilityThreshold);
            Zera(c->AmdTemporalStability);
        } },
        { "lmxxf2_filtro", 20, [](Config* c) {
            Zera(c->AmdLookAntiHalo);
            Zera(c->AmdLookColourSeparation);
            Zera(c->AmdLookContrast);
            Zera(c->AmdLookDetectSkin);
            Zera(c->AmdLookExposureEV);
            Zera(c->AmdLookFlatAreaProtection);
            Zera(c->AmdLookHighlightCompression);
            Zera(c->AmdLookHighlightRollOff);
            Zera(c->AmdLookInspect);
            Zera(c->AmdLookLocalLighting);
            Zera(c->AmdLookMaterialDetail);
            Zera(c->AmdLookMix);
            Zera(c->AmdLookSaturation);
            Zera(c->AmdLookShadowDepth);
            Zera(c->AmdLookShapeDefinition);
            Zera(c->AmdLookSkinDetail);
            Zera(c->AmdLookSkinSoftness);
            Zera(c->AmdLookSpecularControl);
            Zera(c->AmdLookStrength);
            Zera(c->AmdLookTone);
        } },
        { "lmxxf2_luz", 15, [](Config* c) {
            Zera(c->Lmxxf2LightBlowout);
            Zera(c->Lmxxf2LightContrast);
            Zera(c->Lmxxf2LightDechroma);
            Zera(c->Lmxxf2LightExposure);
            Zera(c->Lmxxf2LightFlare);
            Zera(c->Lmxxf2LightGameNits);
            Zera(c->Lmxxf2LightGamma);
            Zera(c->Lmxxf2LightHighlights);
            Zera(c->Lmxxf2LightHueStrength);
            Zera(c->Lmxxf2LightMidGrayNits);
            Zera(c->Lmxxf2LightMix);
            Zera(c->Lmxxf2LightPeakNits);
            Zera(c->Lmxxf2LightSaturation);
            Zera(c->Lmxxf2LightShadows);
            Zera(c->Lmxxf2LightToneMapper);
        } },
        { "lmxxf2_gi", 15, [](Config* c) {
            Zera(c->AmdRtgiAmbient);
            Zera(c->AmdRtgiContact);
            Zera(c->AmdRtgiDenoiser);
            Zera(c->AmdRtgiFade);
            Zera(c->AmdRtgiFarPlane);
            Zera(c->AmdRtgiFov);
            Zera(c->AmdRtgiInspect);
            Zera(c->AmdRtgiLighting);
            Zera(c->AmdRtgiMix);
            Zera(c->AmdRtgiOcclusion);
            Zera(c->AmdRtgiQuality);
            Zera(c->AmdRtgiRadius);
            Zera(c->AmdRtgiSaturation);
            Zera(c->AmdRtgiSmoothness);
            Zera(c->AmdRtgiThickness);
        } },
        { "lmxxf2_ferramentas_amdnr", 26, [](Config* c) {
            Zera(c->AmdEditShaper);
            Zera(c->AmdEditShaperCarryCap);
            Zera(c->AmdEditShaperLimit);
            Zera(c->AmdEditShaperScope);
            Zera(c->AmdEncoding);
            Zera(c->AmdEveryFrame);
            Zera(c->AmdGiDebugView);
            Zera(c->AmdGiEnabled);
            Zera(c->AmdGiEncoding);
            Zera(c->AmdGiFeedback);
            Zera(c->AmdGiFov);
            Zera(c->AmdGiFovAxis);
            Zera(c->AmdGiIntensity);
            Zera(c->AmdGiOcclusion);
            Zera(c->AmdGiQuality);
            Zera(c->AmdGiRadius);
            Zera(c->AmdGiSaturation);
            Zera(c->AmdGiSky);
            Zera(c->AmdGiThickness);
            Zera(c->AmdHighlightProxy);
            Zera(c->AmdHighlightProxyKnee);
            Zera(c->AmdInterleave);
            Zera(c->AmdInterleaveDebug);
            Zera(c->AmdInterleavePreset);
            Zera(c->AmdNetworkOutput);
            Zera(c->AmdSlots);
        } },
        { "upscaler", 71, [](Config* c) {
            Zera(c->DLSSDRenderPresetBalanced);
            Zera(c->DLSSDRenderPresetDLAA);
            Zera(c->DLSSDRenderPresetForAll);
            Zera(c->DLSSDRenderPresetOverride);
            Zera(c->DLSSDRenderPresetPerformance);
            Zera(c->DLSSDRenderPresetQuality);
            Zera(c->DLSSDRenderPresetUltraPerformance);
            Zera(c->DLSSDRenderPresetUltraQuality);
            Zera(c->DisableReactiveMask);
            Zera(c->DlssReactiveMaskBias);
            Zera(c->DontUseNTShared);
            Zera(c->FfxDenoiserAdditiveLightSplit);
            Zera(c->FfxDenoiserBiasMaskStrength);
            Zera(c->FfxDenoiserCrossBlNormStr);
            Zera(c->FfxDenoiserDebugDepthMax);
            Zera(c->FfxDenoiserDebugMode);
            Zera(c->FfxDenoiserDebugViewport);
            Zera(c->FfxDenoiserDemodDivisorFloor);
            Zera(c->FfxDenoiserDiagnostics);
            Zera(c->FfxDenoiserDiffuseAlbedoModulation);
            Zera(c->FfxDenoiserDisocThreshold);
            Zera(c->FfxDenoiserFloorChromaRecovery);
            Zera(c->FfxDenoiserFloorDiffuseNoiseMethod);
            Zera(c->FfxDenoiserFloorDiffuseRecovery);
            Zera(c->FfxDenoiserFloorEnabled);
            Zera(c->FfxDenoiserFloorFastMode);
            Zera(c->FfxDenoiserFloorFlatNoiseMethod);
            Zera(c->FfxDenoiserFloorFlatRecovery);
            Zera(c->FfxDenoiserFloorHandoverAnchorClamp);
            Zera(c->FfxDenoiserFloorHandoverCorrelationMix);
            Zera(c->FfxDenoiserFloorLumaRecovery);
            Zera(c->FfxDenoiserFloorRecovery);
            Zera(c->FfxDenoiserFloorSpecularNoiseMethod);
            Zera(c->FfxDenoiserFloorSpecularRecovery);
            Zera(c->FfxDenoiserGaussKernRelax);
            Zera(c->FfxDenoiserHardwareDepth);
            Zera(c->FfxDenoiserInternalDebugViews);
            Zera(c->FfxDenoiserMaxRadiance);
            Zera(c->FfxDenoiserNormalsInViewSpace);
            Zera(c->FfxDenoiserRadianceClip);
            Zera(c->FfxDenoiserResponsivityInvert);
            Zera(c->FfxDenoiserResponsivityThreshold);
            Zera(c->FfxDenoiserSpecularAlbedoDemodulation);
            Zera(c->FfxDenoiserStabilityBias);
            Zera(c->FfxDenoiserUnsupportedAlbedoRecovery);
            Zera(c->FfxDenoiserUseAmdDefaults);
            Zera(c->FfxDenoiserUseTitleLinearDepth);
            Zera(c->FfxUpscalerIndex);
            Zera(c->Fsr4EnableWatermark);
            Zera(c->Fsr4Preset);
            Zera(c->FsrAccAddPerFrame);
            Zera(c->FsrDebugView);
            Zera(c->FsrMinDisOccAcc);
            Zera(c->FsrNonLinearColorSpace);
            Zera(c->FsrNonLinearPQ);
            Zera(c->FsrNonLinearSRGB);
            Zera(c->FsrReactiveScale);
            Zera(c->FsrShadingScale);
            Zera(c->FsrVelocity);
            Zera(c->NetworkModel);
            Zera(c->RenderPresetBalanced);
            Zera(c->RenderPresetDLAA);
            Zera(c->RenderPresetForAll);
            Zera(c->RenderPresetOverride);
            Zera(c->RenderPresetPerformance);
            Zera(c->RenderPresetQuality);
            Zera(c->RenderPresetUltraPerformance);
            Zera(c->RenderPresetUltraQuality);
            Zera(c->UseGenericAppIdWithDlss);
            Zera(c->VulkanUseCopyForInputs);
            Zera(c->VulkanUseCopyForOutput);
        } },
        { "lmxxf2_rrjogo", 7, [](Config* c) {
            Zera(c->RrJogoAjustar);
            Zera(c->RrJogoCrossBlNormStr);
            Zera(c->RrJogoDisocThreshold);
            Zera(c->RrJogoGaussKernRelax);
            Zera(c->RrJogoMaxRadiance);
            Zera(c->RrJogoRadianceClip);
            Zera(c->RrJogoStabilityBias);
        } },
        { "sharpness", 14, [](Config* c) {
            Zera(c->Contrast);
            Zera(c->ContrastEnabled);
            Zera(c->DAClampOutput);
            Zera(c->DADepthBias);
            Zera(c->DADepthScale);
            Zera(c->MotionScaleLimit);
            Zera(c->MotionSharpness);
            Zera(c->MotionSharpnessDebug);
            Zera(c->MotionSharpnessEnabled);
            Zera(c->MotionThreshold);
            Zera(c->OverrideSharpness);
            Zera(c->RcasEnabled);
            Zera(c->Sharpness);
            Zera(c->SharpnessShader);
        } },
        { "upscale_ratio", 9, [](Config* c) {
            Zera(c->QualityRatioOverrideEnabled);
            Zera(c->QualityRatio_Balanced);
            Zera(c->QualityRatio_DLAA);
            Zera(c->QualityRatio_Performance);
            Zera(c->QualityRatio_Quality);
            Zera(c->QualityRatio_UltraPerformance);
            Zera(c->QualityRatio_UltraQuality);
            Zera(c->UpscaleRatioOverrideEnabled);
            Zera(c->UpscaleRatioOverrideValue);
        } },
        { "output_scaling", 3, [](Config* c) {
            Zera(c->OutputScalingDownscaler);
            Zera(c->OutputScalingEnabled);
            Zera(c->OutputScalingMultiplier);
        } },
        { "magnifier", 7, [](Config* c) {
            Zera(c->MagnifierBorderSize);
            Zera(c->MagnifierCursorOffsetX);
            Zera(c->MagnifierCursorOffsetY);
            Zera(c->MagnifierSize);
            Zera(c->MagnifierStaticPosX);
            Zera(c->MagnifierStaticPosY);
            Zera(c->MagnifierZoomFactor);
        } },
        { "lmxxf2_fg_auto", 5, [](Config* c) {
            Zera(c->DxgiSpoofing);
            Zera(c->FGInput);
            Zera(c->FGNvngxReplacement);
            Zera(c->FGOutput);
            Zera(c->NvngxFGDispatchFlags);
            Lmxxf2AutoFg::Reaplica();
        } },
        { "fg_selection", 19, [](Config* c) {
            Zera(c->FGAllowedFrameAhead);
            Zera(c->FGDLSSGFramerateTargetDMFG);
            Zera(c->FGDLSSGOverrideForceDMFG);
            Zera(c->FGDLSSGOverrideInterpolationCount);
            Zera(c->FGDepthValidNow);
            Zera(c->FGDisableHudless);
            Zera(c->FGDisableUI);
            Zera(c->FGDrawUIOverFG);
            Zera(c->FGHudCutoff);
            Zera(c->FGHudlessValidNow);
            Zera(c->FGInput);
            Zera(c->FGNvngxReplacement);
            Zera(c->FGOnlyAcceptFirstHudless);
            Zera(c->FGOutput);
            Zera(c->FGSkipReset);
            Zera(c->FGUIPremultipliedAlpha);
            Zera(c->FGVelocityValidNow);
            Zera(c->FGXeFGOverrideInterpolationCount);
            Zera(c->FTInput);
            Lmxxf2AutoFg::Reaplica();
        } },
        { "fg_fsrfg", 17, [](Config* c) {
            Zera(c->FGAsync);
            Zera(c->FGDebugPacingLines);
            Zera(c->FGDebugResetLines);
            Zera(c->FGDebugTearLines);
            Zera(c->FGDebugView);
            Zera(c->FGFPTAllowHybridSpin);
            Zera(c->FGFPTAllowWaitForSingleObjectOnFence);
            Zera(c->FGFPTHybridSpinTime);
            Zera(c->FGFPTSafetyMarginInMs);
            Zera(c->FGFPTVarianceFactor);
            Zera(c->FGFramePacingTuning);
            Zera(c->FGRectHeight);
            Zera(c->FGRectLeft);
            Zera(c->FGRectTop);
            Zera(c->FGRectWidth);
            Zera(c->FSRFGEnableWatermark);
            Zera(c->FfxFGIndex);
        } },
        { "fg_xefg", 13, [](Config* c) {
            Zera(c->FGRectHeight);
            Zera(c->FGRectLeft);
            Zera(c->FGRectTop);
            Zera(c->FGRectWidth);
            Zera(c->FGXeFGDebugView);
            Zera(c->FGXeFGDepthInverted);
            Zera(c->FGXeFGForceBorderless);
            Zera(c->FGXeFGHighResMV);
            Zera(c->FGXeFGIgnoreInitChecks);
            Zera(c->FGXeFGInterpolationCount);
            Zera(c->FGXeFGJitteredMV);
            Zera(c->FGXeFGUIComposition);
            Zera(c->FontSize);
        } },
        { "fg_dlssg", 4, [](Config* c) {
            Zera(c->FGDLSSGForceDMFG);
            Zera(c->FGDLSSGFramerateTargetDMFG);
            Zera(c->FGDLSSGInterpolationCount);
            Zera(c->FGDLSSGUseGamesReflexMarkers);
        } },
        { "fg_reprojection", 4, [](Config* c) {
            Zera(c->ReprojectionCutoffExpand);
            Zera(c->ReprojectionDepthCutoff);
            Zera(c->ReprojectionFillMode);
            Zera(c->ReprojectionLateLatch);
        } },
        { "fg_optifg", 24, [](Config* c) {
            Zera(c->FGAlwaysTrackHeaps);
            Zera(c->FGDepthScaleMax);
            Zera(c->FGDisableHUDFix);
            Zera(c->FGEnableDepthScale);
            Zera(c->FGHUDFix);
            Zera(c->FGHUDFixExtended);
            Zera(c->FGHUDLimit);
            Zera(c->FGHudfixDisableDI);
            Zera(c->FGHudfixDisableDII);
            Zera(c->FGHudfixDisableDispatch);
            Zera(c->FGHudfixDisableOM);
            Zera(c->FGHudfixDisableRTV);
            Zera(c->FGHudfixDisableSCR);
            Zera(c->FGHudfixDisableSGR);
            Zera(c->FGHudfixDisableSRV);
            Zera(c->FGHudfixDisableUAV);
            Zera(c->FGImmediateCapture);
            Zera(c->FGMakeDepthCopy);
            Zera(c->FGMakeMVCopy);
            Zera(c->FGRelaxedResolutionCheck);
            Zera(c->FGResourceBlocking);
            Zera(c->FGResourceFlip);
            Zera(c->FGResourceFlipOffset);
            Zera(c->FGUseMutexForSwapchain);
        } },
        { "fg_nvngx", 9, [](Config* c) {
            Zera(c->FGAsync);
            Zera(c->FGDLSSGOverrideForceDMFG);
            Zera(c->FGDebugView);
            Zera(c->FSRFGEnableWatermark);
            Zera(c->FfxFGIndex);
            Zera(c->NvngxFGDisableHudless);
            Zera(c->NvngxFGDispatchFlags);
            Zera(c->NvngxFGMakeDepthCopy);
            Zera(c->NvngxFGShowDebug);
        } },
        { "fg_fsrfg_inputs", 2, [](Config* c) {
            Zera(c->FSRFGSkipConfigForHudless);
            Zera(c->FSRFGSkipDispatchForHudless);
        } },
        { "framerate", 1, [](Config* c) {
            Zera(c->FramerateLimit);
        } },
        { "low_latency", 4, [](Config* c) {
            Zera(c->FN_ForceLatencyFlex);
            Zera(c->FN_ForceReflex);
            Zera(c->FN_LatencyFlexMode);
            Zera(c->ForceXeLL);
        } },
        { "vsync", 3, [](Config* c) {
            Zera(c->ForceVsync);
            Zera(c->OverrideVsync);
            Zera(c->VsyncInterval);
        } },
        { "mipmap", 4, [](Config* c) {
            Zera(c->MipmapBiasFixedOverride);
            Zera(c->MipmapBiasOverride);
            Zera(c->MipmapBiasOverrideAll);
            Zera(c->MipmapBiasScaleOverride);
        } },
        { "anisotropy", 4, [](Config* c) {
            Zera(c->AnisotropyModifyComp);
            Zera(c->AnisotropyModifyMinMax);
            Zera(c->AnisotropyOverride);
            Zera(c->AnisotropySkipPointFilter);
        } },
        { "init_flags", 8, [](Config* c) {
            Zera(c->AutoExposure);
            Zera(c->DepthInverted);
            Zera(c->DisableReactiveMask);
            Zera(c->DisplayResolution);
            Zera(c->DlssReactiveMaskBias);
            Zera(c->FsrUseMaskForTransparency);
            Zera(c->HDR);
            Zera(c->JitterCancellation);
        } },
        { "fsr_common", 5, [](Config* c) {
            Zera(c->FsrCameraFar);
            Zera(c->FsrCameraNear);
            Zera(c->FsrHorizontalFov);
            Zera(c->FsrUseFsrInputValues);
            Zera(c->FsrVerticalFov);
        } },
        { "upscaler_inputs", 8, [](Config* c) {
            Zera(c->EnableFfxInputs);
            Zera(c->EnableFsr2Inputs);
            Zera(c->EnableFsr3Inputs);
            Zera(c->Fsr2Pattern);
            Zera(c->Fsr3Pattern);
            Zera(c->UseFfxInputs);
            Zera(c->UseFsr2Inputs);
            Zera(c->UseFsr3Inputs);
        } },
        { "spoofing", 5, [](Config* c) {
            Zera(c->DisableOverlays);
            Zera(c->DxgiSpoofing);
            Zera(c->UseNtdllHooks);
            Zera(c->VulkanExtensionSpoofing);
            Zera(c->VulkanSpoofing);
            Lmxxf2AutoFg::AplicaSpoof();
        } },
        { "plugins", 3, [](Config* c) {
            Zera(c->CheckForUpdate);
            Zera(c->DisableSplash);
            Zera(c->LoadAsiPlugins);
        } },
        { "shaders_limits", 4, [](Config* c) {
            Zera(c->DrsMaxOverrideEnabled);
            Zera(c->DrsMinOverrideEnabled);
            Zera(c->ExtendedLimits);
            Zera(c->UsePrecompiledShaders);
        } },
        { "barriers", 6, [](Config* c) {
            Zera(c->ColorResourceBarrier);
            Zera(c->DepthResourceBarrier);
            Zera(c->ExposureResourceBarrier);
            Zera(c->MVResourceBarrier);
            Zera(c->MaskResourceBarrier);
            Zera(c->OutputResourceBarrier);
        } },
        { "root_signatures", 2, [](Config* c) {
            Zera(c->RestoreComputeSignature);
            Zera(c->RestoreGraphicSignature);
        } },
        { "menu_scale", 4, [](Config* c) {
            Zera(c->FontSize);
            Zera(c->MenuHeight);
            Zera(c->MenuScale);
            Zera(c->MenuWidth);
        } },
        { "theme", 10, [](Config* c) {
            Zera(c->LightTheme);
            Zera(c->MenuAccentColorB);
            Zera(c->MenuAccentColorG);
            Zera(c->MenuAccentColorR);
            Zera(c->MenuBGColorA);
            Zera(c->MenuBGColorB);
            Zera(c->MenuBGColorG);
            Zera(c->MenuBGColorR);
            Zera(c->MenuBlur);
            Zera(c->MenuBlurStrength);
        } },
        { "fps_overlay", 7, [](Config* c) {
            Zera(c->FpsOverlayAlpha);
            Zera(c->FpsOverlayHorizontal);
            Zera(c->FpsOverlayPosition);
            Zera(c->FpsOverlayType);
            Zera(c->FpsScale);
            Zera(c->OverlaysUseTheme);
            Zera(c->ShowFps);
        } },
        { "logging", 5, [](Config* c) {
            Zera(c->LogLevel);
            Zera(c->LogSingleFile);
            Zera(c->LogToConsole);
            Zera(c->LogToFile);
            Zera(c->LogToNGX);
        } },
};

const Cartao* Acha(const char* id)
{
    if (id == nullptr)
        return nullptr;
    for (const auto& c : kCartoes)
        if (std::strcmp(c.id, id) == 0)
            return &c;
    return nullptr;
}
} // namespace

int Chaves(const char* cartao)
{
    const auto* c = Acha(cartao);
    return c == nullptr ? -1 : c->chaves;
}

bool Restaura(const char* cartao)
{
    const auto* c = Acha(cartao);
    if (c == nullptr)
        return false;
    c->zera(Config::Instance());
    LOG_INFO("[lmxxf2] reset do cartao {} ({} chaves)", cartao, c->chaves);
    return true;
}
} // namespace Lmxxf2Reset
