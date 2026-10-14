#pragma once
#include "SysUtils.h"
#include "State.h"

#include <optional>
#include <filesystem>

enum HasDefaultValue
{
    WithDefault,
    NoDefault,
    SoftDefault // Change always gets saved to the config
};

template <class T, HasDefaultValue defaultState = WithDefault> class CustomOptional : public std::optional<T>
{
  private:
    T _defaultValue;
    std::optional<T> _configIni;
    bool _volatile;

  public:
    CustomOptional(T defaultValue)
        requires(defaultState != NoDefault)
        : std::optional<T>(), _defaultValue(std::move(defaultValue)), _configIni(std::nullopt), _volatile(false)
    {
    }

    CustomOptional()
        requires(defaultState == NoDefault)
        : std::optional<T>(), _defaultValue(T {}), _configIni(std::nullopt), _volatile(false)
    {
    }

    // Prevents a change from being saved to ini
    constexpr void set_volatile_value(const T& value)
    {
        if (!_volatile)
        { // make sure the previously set value is saved
            if (this->has_value())
                _configIni = this->value();
            else
                _configIni = std::nullopt;
        }
        _volatile = true;
        std::optional<T>::operator=(value);
    }

    // Use this when first setting a CustomOptional
    constexpr void set_from_config(const std::optional<T>& opt)
    {
        if (!this->has_value())
        {
            _configIni = opt;
            std::optional<T>::operator=(opt);
        }
    }

    constexpr CustomOptional& operator=(const T& value)
    {
        _volatile = false;
        std::optional<T>::operator=(value);
        return *this;
    }

    constexpr CustomOptional& operator=(T&& value)
    {
        _volatile = false;
        std::optional<T>::operator=(std::move(value));
        return *this;
    }

    constexpr CustomOptional& operator=(const std::optional<T>& opt)
    {
        _volatile = false;
        std::optional<T>::operator=(opt);
        return *this;
    }

    constexpr CustomOptional& operator=(std::optional<T>&& opt)
    {
        _volatile = false;
        std::optional<T>::operator=(std::move(opt));
        return *this;
    }

    // Needed for string literals for some reason
    constexpr CustomOptional& operator=(const char* value)
        requires std::same_as<T, std::string>
    {
        _volatile = false;
        std::optional<T>::operator=(T(value));
        return *this;
    }

    constexpr T value_or_default() const&
        requires(defaultState != NoDefault)
    {
        return this->has_value() ? this->value() : _defaultValue;
    }

    constexpr T value_or_default() &&
        requires(defaultState != NoDefault)
    {
        return this->has_value() ? std::move(this->value()) : std::move(_defaultValue);
    }

    // [lmxxf2] do AMDNR: o valor que o ini deve guardar, mesmo quando igual ao padrão
    constexpr std::optional<T> value_for_config_ignore_default()
    {
        if (_volatile)
            return _configIni;
        if (this->has_value())
            return this->value();
        return std::nullopt;
    }

    constexpr std::optional<T> value_for_config()
        requires(defaultState == WithDefault)
    {
        if (_volatile)
        {
            if (_configIni != _defaultValue)
                return _configIni;

            return std::nullopt;
        }

        if (!this->has_value() || *this == _defaultValue)
            return std::nullopt;

        return this->value();
    }

    constexpr std::optional<T> value_for_config()
        requires(defaultState != WithDefault)
    {
        if (_volatile)
            return _configIni;

        if (this->has_value())
            return this->value();

        return std::nullopt;
    }

    constexpr T value_for_config_or(T other)
    {
        auto option = value_for_config();

        if (option.has_value())
            return option.value();
        else
            return other;
    }

    // Like value_for_config(), but keeps an explicit value that happens to equal
    // the class default - "unset" stays a distinct state.
    constexpr std::optional<T> value_for_config_ignore_default()
        requires(defaultState == WithDefault)
    {
        if (_volatile)
            return _configIni;

        if (this->has_value())
            return this->value();

        return std::nullopt;
    }
};

constexpr inline int UnboundKey = -1;
constexpr uint32_t NV_PRESET_LATEST = 0x00FFFFFF;

enum FpsOverlayPos : uint32_t
{
    FpsOverlayPos_TopLeft,
    FpsOverlayPos_TopRight,
    FpsOverlayPos_BottomLeft,
    FpsOverlayPos_BottomRight,
    FpsOverlayPos_COUNT,
};

enum FpsOverlay : uint32_t
{
    FpsOverlay_JustFPS,
    FpsOverlay_Simple,
    FpsOverlay_Detailed,
    FpsOverlay_DetailedGraph,
    FpsOverlay_Full,
    FpsOverlay_FullGraph,
    FpsOverlay_ReflexTimings,
    FpsOverlay_COUNT,
};

// Output scaling downscaler
enum class Scaler : uint32_t
{
    FSR1 = 0,
    Bicubic = 1,
    CatmullRom = 2,
    Lanczos2 = 3,
    Lanczos3 = 4,
    Kaiser2 = 5,
    Kaiser3 = 6,
    Magic = 7,
    Count
};

enum class ForceReflex : uint32_t
{
    InGame,
    ForceDisable,
    ForceEnable,
    Count
};

enum class LFXMode : uint32_t
{
    Conservative,
    Aggressive,
    ReflexIDs,
    Count
};

enum class LowLatencyInput : uint32_t
{
    None,
    Auto,
    AntiLag2,
    Reflex,
    XeLL,
    UeLowLatency,
    OptiScaler, // OptiScaler's own Streamline (DLSSG output) when the game has no low latency of its own
    _
};

enum class LowLatencyMode : uint32_t
{
    None,
    Auto,
    LatencyFlex,
    AntiLag2,
    XeLL,
    AntiLagVk,
    Reflex,
    FpsLimiter,
    _
};

enum class ReprojectionFill : uint32_t
{
    StrechEdge,
    Dithering,
    Noise,
    Debug,
};

template <> struct EnumConfig<ReprojectionFill>
{
    static constexpr auto default_value = ReprojectionFill::Dithering;

    static constexpr std::pair<ReprojectionFill, std::string_view> mapping[] = {
        { ReprojectionFill::StrechEdge, "strech" },
        { ReprojectionFill::Dithering, "dithering" },
        { ReprojectionFill::Noise, "noise" },
        { ReprojectionFill::Debug, "debug" }
    };
};

class Config
{
  public:
    Config();

    // Init flags
    CustomOptional<bool, NoDefault> DepthInverted;
    CustomOptional<bool, NoDefault> AutoExposure;
    CustomOptional<bool, NoDefault> HDR;
    CustomOptional<bool, NoDefault> JitterCancellation;
    CustomOptional<bool, NoDefault> DisplayResolution;
    CustomOptional<bool, NoDefault> DisableReactiveMask;
    CustomOptional<float> DlssReactiveMaskBias { 0.45f };

    // Logging
    CustomOptional<bool> LogToFile { false };
    CustomOptional<bool> LogToConsole { false };
    CustomOptional<bool> LogToDebug { false };
    CustomOptional<bool> LogToNGX { false };
    CustomOptional<bool> OpenConsole { false };
    CustomOptional<bool> DebugWait { false }; // not in ini
    CustomOptional<int> LogLevel { 0 };
    CustomOptional<std::wstring> LogFileName { L"OptiScaler.log" };
    CustomOptional<bool> LogSingleFile { true };
    CustomOptional<bool> LogAsync { false };
    CustomOptional<int> LogAsyncThreads { 4 };

    // XeSS
    CustomOptional<bool> BuildPipelines { true };
    CustomOptional<int32_t> NetworkModel { 0 };
    CustomOptional<bool> CreateHeaps { true };

    // DLSS
    CustomOptional<bool> DLSSEnabled { true };
    CustomOptional<bool> RenderPresetOverride { false };
    CustomOptional<uint32_t> RenderPresetForAll { 0 };
    CustomOptional<uint32_t> RenderPresetDLAA { 0 };
    CustomOptional<uint32_t> RenderPresetUltraQuality { 0 };
    CustomOptional<uint32_t> RenderPresetQuality { 0 };
    CustomOptional<uint32_t> RenderPresetBalanced { 0 };
    CustomOptional<uint32_t> RenderPresetPerformance { 0 };
    CustomOptional<uint32_t> RenderPresetUltraPerformance { 0 };

    // DLSSD
    CustomOptional<bool> DLSSDRenderPresetOverride { false };
    CustomOptional<uint32_t> DLSSDRenderPresetForAll { 0 };
    CustomOptional<uint32_t> DLSSDRenderPresetDLAA { 0 };
    CustomOptional<uint32_t> DLSSDRenderPresetUltraQuality { 0 };
    CustomOptional<uint32_t> DLSSDRenderPresetQuality { 0 };
    CustomOptional<uint32_t> DLSSDRenderPresetBalanced { 0 };
    CustomOptional<uint32_t> DLSSDRenderPresetPerformance { 0 };
    CustomOptional<uint32_t> DLSSDRenderPresetUltraPerformance { 0 };

    // Nukems
    CustomOptional<bool> NvngxFGMakeDepthCopy { false };

    // Libraries
    CustomOptional<std::wstring, NoDefault> MainDllPath;
    CustomOptional<std::wstring, NoDefault> FfxDx12Path;
    CustomOptional<std::wstring, NoDefault> FfxDx12SRPath;
    CustomOptional<std::wstring, NoDefault> FfxDx12FGPath;
    CustomOptional<std::wstring, NoDefault> FfxDx12RRPath;
    CustomOptional<std::wstring, NoDefault> FfxDx12RCPath;
    CustomOptional<std::wstring, NoDefault> FfxVkPath;
    CustomOptional<std::wstring, NoDefault> XeSSLibrary;
    CustomOptional<std::wstring, NoDefault> XeFGLibrary;
    CustomOptional<std::wstring, NoDefault> XeLLLibrary;
    CustomOptional<std::wstring, NoDefault> XeSSDx11Library;
    CustomOptional<std::wstring, NoDefault> NvngxPath;
    CustomOptional<std::wstring, NoDefault> NVNGX_DLSS_Library;
    CustomOptional<std::wstring, NoDefault> DLSSFeaturePath;
    CustomOptional<std::wstring, NoDefault> NvapiDllPath;

    // Sharpness
    CustomOptional<SharpenShader> SharpnessShader { SharpenShader::RCAS };
    CustomOptional<bool> OverrideSharpness { false };
    CustomOptional<float> Sharpness { 0.4f };

    // RCAS
    CustomOptional<bool> RcasEnabled { false };
    CustomOptional<bool> ContrastEnabled { false };
    CustomOptional<float> Contrast { -0.3f };

    // DA Sharpening
    CustomOptional<float, NoDefault> DADepthScale;
    CustomOptional<float, NoDefault> DADepthBias;
    CustomOptional<bool, NoDefault> DAClampOutput;
    CustomOptional<bool> UseDepthAwareSharpen { false };
    CustomOptional<bool> UseDASDepthAwareSharpen { false };
    CustomOptional<bool> DADepthIsLinear { false };

    // MAS
    CustomOptional<bool> MotionSharpnessEnabled { false };
    CustomOptional<bool> MotionSharpnessDebug { false };
    CustomOptional<float> MotionSharpness { 0.2f };
    CustomOptional<float> MotionThreshold { 0.0f };
    CustomOptional<float> MotionScaleLimit { 10.0f };

    // Magnifier
    CustomOptional<bool> MagnifierEnabled { false };
    CustomOptional<float> MagnifierSize { 15.f }; // % of screen Height
    CustomOptional<int> MagnifierZoomFactor { 4 };
    CustomOptional<float> MagnifierBorderSize { 0.3f };   // % of screen Height
    CustomOptional<float> MagnifierCursorOffsetX { 0.f }; // Pixels
    CustomOptional<float> MagnifierCursorOffsetY { 0.f }; // Pixels
    CustomOptional<float, NoDefault> MagnifierStaticPosX; // % of screen Width, static pos enabled if both are defined
    CustomOptional<float, NoDefault> MagnifierStaticPosY; // % of screen Height

    // Menu
    CustomOptional<float, NoDefault> MenuScale;
    CustomOptional<float, NoDefault> MenuHeight; // Unscaled, no value means default height
    CustomOptional<float, NoDefault> MenuWidth;  // [lmxxf2] Unscaled, no value means default width (680)
    CustomOptional<bool> OverlayMenu { true };
    CustomOptional<int> ShortcutKey { VK_INSERT };
    CustomOptional<bool> ExtendedLimits { false };
    CustomOptional<bool> ShowFps { false };
    /// 0 Top Left, 1 Top Right, 2 Bottom Left, 3 Bottom Right
    CustomOptional<FpsOverlayPos> FpsOverlayPosition { FpsOverlayPos_TopLeft };
    /// 0 Only FPS, 1 +Avg FPS & Upscaler info 2 +Frame Time,
    /// 3 +Upscaler Time, 4 +Frame Time Graph, 5 +Upscaler Time Graph
    /// 6 +Reflex timings
    CustomOptional<FpsOverlay> FpsOverlayType { FpsOverlay_JustFPS };
    CustomOptional<int> FpsShortcutKey { VK_PRIOR };
    CustomOptional<int> FpsCycleShortcutKey { VK_NEXT };
    CustomOptional<bool> FpsOverlayHorizontal { false };
    CustomOptional<float> FpsOverlayAlpha { 0.4f };
    CustomOptional<float, NoDefault> FpsScale; // No value means same as MenuScale
    CustomOptional<bool> UseHQFont { true };
    CustomOptional<bool> DisableSplash { false };
    CustomOptional<float> FontSize { 14.0f };
    CustomOptional<std::wstring, NoDefault> TTFFontPath;
    CustomOptional<int> FGShortcutKey { VK_END };
    CustomOptional<bool> LightTheme { false };
    CustomOptional<bool> OverlaysUseTheme { false };
    CustomOptional<float> MenuAccentColorR { 0.00f };
    CustomOptional<float> MenuAccentColorG { 0.40f };
    CustomOptional<float> MenuAccentColorB { 0.77f };
    CustomOptional<float> MenuBGColorR { 0.0f };
    CustomOptional<float> MenuBGColorG { 0.0f };
    CustomOptional<float> MenuBGColorB { 0.0f };
    CustomOptional<float> MenuBGColorA { 0.93f };
    CustomOptional<bool> MenuBlur { true };
    CustomOptional<float> MenuBlurStrength { 1.0f };
    CustomOptional<bool> CustomTabEnabled { true };
    CustomOptional<std::string> CustomTabCards { "upscaler,sharpness,fg_selection,fg_fsrfg,fg_xefg,fg_dlssg,fg_"
                                                 "reprojection,fg_optifg,fg_nvngx,fg_fsrfg_inputs,fg_sl_"
                                                 "inputs,framerate" }; // Comma separated ids of the boxes

    // Hooks
    CustomOptional<bool> HookOriginalNvngxOnly { false };
    CustomOptional<bool> EarlyHooking { false };
    CustomOptional<bool> UseNtdllHooks { true };

    // Upscale Ratio Override
    CustomOptional<bool> UpscaleRatioOverrideEnabled { false };
    CustomOptional<float> UpscaleRatioOverrideValue { 1.3f };

    // DRS
    CustomOptional<bool> DrsMinOverrideEnabled { false };
    CustomOptional<bool> DrsMaxOverrideEnabled { false };

    // Quality Overrides
    CustomOptional<bool> QualityRatioOverrideEnabled { false };
    CustomOptional<float> QualityRatio_DLAA { 1.0f };
    CustomOptional<float> QualityRatio_UltraQuality { 1.3f };
    CustomOptional<float> QualityRatio_Quality { 1.5f };
    CustomOptional<float> QualityRatio_Balanced { 1.7f };
    CustomOptional<float> QualityRatio_Performance { 2.0f };
    CustomOptional<float> QualityRatio_UltraPerformance { 3.0f };

    // ProcessFilter
    CustomOptional<std::wstring, NoDefault> TargetProcess;
    CustomOptional<std::wstring> ProcessExclusionList = {
        L"crashpad_handler.exe|crashreport.exe|crashreporter.exe|crs-handler.exe|crs-uploader.exe|crs-video.exe|"
        L"unitycrashhandler64.exe|idtechlauncher.exe|cefviewwing.exe|ace-setup64.exe|ace-service64.exe|"
        L"qtwebengineprocess.exe|platformprocess.exe|bugsplathd64.exe|bssndrpt64.exe|pspcsdkappmgr.exe|pspcsdkcore.exe|"
        L"pspcsdkstttts.exe|pspcsdktelemetry.exe|pspcsdkui.exe|pspcsdkupdatechecker.exe|pspcsdkvoicechat.exe|"
        L"pspcsdkwebview.exe|windhawk.exe|vscodium.exe|crash_reporter.exe|steamerrorreporter64.exe|crashreportclient."
        L"exe|edcefcrashpadprocess.exe|edcefrenderprocess.exe"
    };

    // Hotfixes
    CustomOptional<bool> CheckForUpdate { true };
    CustomOptional<bool, SoftDefault> DisableOverlays { false };

    CustomOptional<bool> SimulateWaitableObject { false };

    CustomOptional<float, NoDefault> MipmapBiasOverride; // disabled by default
    CustomOptional<bool> MipmapBiasFixedOverride { false };
    CustomOptional<bool> MipmapBiasScaleOverride { false };
    CustomOptional<bool> MipmapBiasOverrideAll { false };

    CustomOptional<int, NoDefault> AnisotropyOverride; // disabled by default
    CustomOptional<bool> OverrideShaderSampler { true };
    CustomOptional<bool> AnisotropyModifyComp { true };
    CustomOptional<bool> AnisotropyModifyMinMax { true };
    CustomOptional<bool> AnisotropySkipPointFilter { true };

    CustomOptional<int, NoDefault> RoundInternalResolution; // disabled by default

    CustomOptional<int, NoDefault> SkipFirstFrames; // disabled by default
    CustomOptional<bool> RestoreComputeSignature { false };
    CustomOptional<bool> RestoreGraphicSignature { false };
    CustomOptional<bool> ExtendedStateRestore { false };

    CustomOptional<bool> UsePrecompiledShaders { true };

    CustomOptional<bool> UseGenericAppIdWithDlss { false };
    CustomOptional<bool> PreferDedicatedGpu { true };
    CustomOptional<bool> PreferFirstDedicatedGpu { false };

    CustomOptional<int32_t, NoDefault> ColorResourceBarrier;    // disabled by default
    CustomOptional<int32_t, NoDefault> MVResourceBarrier;       // disabled by default
    CustomOptional<int32_t, NoDefault> DepthResourceBarrier;    // disabled by default
    CustomOptional<int32_t, NoDefault> ExposureResourceBarrier; // disabled by default
    CustomOptional<int32_t, NoDefault> MaskResourceBarrier;     // disabled by default
    CustomOptional<int32_t, NoDefault> OutputResourceBarrier;   // disabled by default

    CustomOptional<bool> CreateD3D12DeviceForLuma { false };

    // Upscalers
    CustomOptional<Upscaler, SoftDefault> Dx11Upscaler { Upscaler::FSR22 };
    CustomOptional<Upscaler, SoftDefault> Dx12Upscaler { Upscaler::XeSS };
    CustomOptional<Upscaler, SoftDefault> VulkanUpscaler { Upscaler::FSR22 };

    // Output Scaling
    CustomOptional<bool> OutputScalingEnabled { false };
    CustomOptional<float> OutputScalingMultiplier { 1.5f };
    CustomOptional<Scaler> OutputScalingDownscaler { Scaler::FSR1 };

    // FSR
    CustomOptional<bool> FsrDebugView { false };
    CustomOptional<bool> Fsr4EnableDebugView { false };
    CustomOptional<int> FfxUpscalerIndex { 0 };
    CustomOptional<int> FfxFGIndex { 0 };
    CustomOptional<bool> FsrUseMaskForTransparency { true };
    CustomOptional<bool> FsrNonLinearColorSpace { false };
    CustomOptional<bool> FsrNonLinearSRGB { false };
    CustomOptional<bool> FsrNonLinearPQ { false };
    CustomOptional<bool> FsrAgilitySDKUpgrade { false };

    // These default values will be overwritten at upscaler init time with optimized values
    CustomOptional<float> FsrVelocity { 1.0f };
    CustomOptional<float> FsrReactiveScale { 1.0f };
    CustomOptional<float> FsrShadingScale { 1.0f };
    CustomOptional<float> FsrAccAddPerFrame { 0.333f };
    CustomOptional<float> FsrMinDisOccAcc { -0.333f };

    // FSR-RR
    // Auto: game-native for SR, NVIDIA RR on capable NVIDIA GPUs, otherwise validated FFX + FSR-RR.
    CustomOptional<bool, NoDefault> FfxDenoiserEnabled;
    CustomOptional<int> FfxDenoiserProfile { 1 }; // 0 Fast, 1 Balanced, 2 Quality; -1 custom
    CustomOptional<int> FfxDenoiserIndex { 0 };
    CustomOptional<uint64_t> FfxDenoiserDebugMode { 0 };
    // -1: overview, 0..FFX_API_DENOISER_DEBUG_VIEW_MAX_VIEWPORTS-1: fullscreen viewport
    CustomOptional<int> FfxDenoiserDebugViewport { -1 };
    // Enables AMD's internal RR debug descriptors. Requires context recreation.
    CustomOptional<bool> FfxDenoiserInternalDebugViews { false };
    // Per-lobe RR path, FSRDSignals::Route: 0 Auto, 1 Direct, 2 Indirect, 3 Split. The RR
    // signal layout is derived from these, the albedo fix and the title's guides.
    CustomOptional<int> FfxDenoiserDiffuseRoute { 0 };
    CustomOptional<int> FfxDenoiserSpecularRoute { 0 };
    // Primary view depth stands in for a missing or invalid hit distance.
    CustomOptional<bool> FfxDenoiserEstimateHitDistances { false };
    // Single-signal denoising: dispatch only the enabled signals. A title whose raw
    // signal content only makes sense for one of the two can denoise the surviving
    // signal alone; the disabled signal's output stays zero and composition falls
    // back to the floor/raw-correlation path for it. Requires context recreation.
    CustomOptional<bool> FfxDenoiserDenoiseDiffuse { true };
    CustomOptional<bool> FfxDenoiserDenoiseSpecular { true };
    CustomOptional<bool> FfxDenoiserGpuTimings{true};
    // Uses only semantic Streamline AO noisy/denoised tags. Resource-inspector
    // candidates are deliberately never promoted to signal inputs.
    CustomOptional<bool> FfxDenoiserTaggedAmbientOcclusion { false };
    // Disabled preserves the existing contract that RR input normals are world-space.
    CustomOptional<bool> FfxDenoiserNormalsInViewSpace { false };
    // Prefer the title's own linearised view depth when it publishes one, instead of
    // deriving it from the title's depth buffer and the projection. Off by default: the
    // derived path is the one every title so far has been validated against.
    CustomOptional<bool> FfxDenoiserUseTitleLinearDepth { false };
    // Responsivity values on the unstable side of this threshold route the specular
    // radiance through the spatial path as well. Zero disables the test.
    CustomOptional<float> FfxDenoiserResponsivityThreshold { 0.0f };
    // Selects which side of that threshold counts as unstable, since the polarity of the
    // title's mask is a title property.
    CustomOptional<bool> FfxDenoiserResponsivityInvert { false };

    // Routes pixels flagged by the DLSS bias-current-color mask (particles, alpha layers,
    // animated and video textures) around the denoiser via the floor and skip signal.
    // 0 restores the behaviour where the mask was bound but unused.
    CustomOptional<float> FfxDenoiserBiasMaskStrength { 1.0f };

    // Spatial Floor and independently selected, master-scaled recovery paths.
    CustomOptional<float> FfxDenoiserSpecularAlbedoDemodulation{1.0f};
    CustomOptional<float> FfxDenoiserDiffuseAlbedoModulation{1.0f};
    // Experimental local color/albedo fit; zero preserves the existing signal split.
    CustomOptional<float> FfxDenoiserAdditiveLightSplit{0.0f};
    // Albedo bleed fix (INI AlbedoBleedFix): denoises unmodulated copies of the specular and
    // diffuse shares as RR Direct Specular / Indirect Diffuse and blends toward them where the
    // title's albedo shows a surface the light does not (water over a sea floor). Independent
    // of the profile; needs a specular hit distance and runs only at 1/1 modulation.
    CustomOptional<bool> FfxDenoiserUnsupportedAlbedoRecovery{false};
    CustomOptional<bool> FfxDenoiserFloorFlatRecovery{true};
    CustomOptional<bool> FfxDenoiserFloorSpecularRecovery{true};
    CustomOptional<bool> FfxDenoiserFloorDiffuseRecovery{false};
    CustomOptional<int> FfxDenoiserFloorFlatNoiseMethod{0};
    CustomOptional<int> FfxDenoiserFloorSpecularNoiseMethod{1};
    CustomOptional<int> FfxDenoiserFloorDiffuseNoiseMethod{1};
    CustomOptional<float> FfxDenoiserFloorLumaRecovery{1.0f};
    CustomOptional<float> FfxDenoiserFloorChromaRecovery{1.0f};

    CustomOptional<bool> FfxDenoiserFloorEnabled { true };
    // Reduced Floor pass policy is the Balanced default; Quality uses five passes.
    CustomOptional<bool> FfxDenoiserFloorFastMode { true };
    CustomOptional<float> FfxDenoiserFloorRecovery { 1.0f };
    CustomOptional<float> FfxDenoiserFloorHandoverCorrelationMix { 1.0f };
    CustomOptional<float> FfxDenoiserFloorHandoverAnchorClamp { 4.0f };

    // Overrides the DLSS.Use.HW.Depth interpretation. Unset follows NGX, which
    // defaults to linear when the title publishes nothing - and reading a hardware
    // depth buffer as linear collapses the whole scene to sub-unit distances.
    CustomOptional<bool, NoDefault> FfxDenoiserHardwareDepth;

    // Pushes AMD's own queried baseline for the six tunable RR keys instead of the
    // values below. A/B reference only - the fork's defaults remain the shipping
    // configuration, and the sliders keep their values while this is enabled.
    CustomOptional<bool> FfxDenoiserUseAmdDefaults { false };

    CustomOptional<float> FfxDenoiserDisocThreshold { 0.1f };
    CustomOptional<float> FfxDenoiserCrossBlNormStr { 0.5f };
    CustomOptional<float> FfxDenoiserStabilityBias { 0.5f };
    CustomOptional<float> FfxDenoiserMaxRadiance { 4e4f };
    CustomOptional<float> FfxDenoiserRadianceClip { 40.0f };
    CustomOptional<float> FfxDenoiserGaussKernRelax { 0.5f };
    CustomOptional<float> FfxDenoiserDebugDepthMax { 1024.0f };

    // Records the probe readbacks: seven render targets per input probe interval plus two per
    // denoiser output probe, and the log lines that report them. Off by default - the numbers
    // are for diagnosis, and an always-on probe wrote a gigabyte of log in a session.
    CustomOptional<bool> FfxDenoiserDiagnostics { false };

    CustomOptional<bool> FfxDenoiserDiffuseHitDistance { true };
    // Only the demodulation divisor is floored; unrepresentable energy is routed once.
    CustomOptional<float> FfxDenoiserDemodDivisorFloor { 8e-3f };

    // FSR4
    CustomOptional<FSR4Support> Fsr4ForceModel { FSR4Support::None };
    CustomOptional<uint32_t, NoDefault> Fsr4Preset;
    CustomOptional<bool> Fsr4EnableWatermark { false };
    CustomOptional<bool> Fsr4DoNotLoadAmdxc64 { false };

    // FSR Common
    CustomOptional<float> FsrVerticalFov { 60.0f };
    CustomOptional<float> FsrHorizontalFov { 0.0f }; // off by default
    CustomOptional<float> FsrCameraNear { 0.1f };
    CustomOptional<float> FsrCameraFar { 100000.0f };
    CustomOptional<bool> FsrUseFsrInputValues { true };

    // dx11wdx12
    CustomOptional<bool> Dx11DelayedInit { false };
    CustomOptional<bool> DontUseNTShared { true };

    // vulkanwdx12
    CustomOptional<bool> VulkanUseCopyForInputs { false };
    CustomOptional<bool> VulkanUseCopyForOutput { false };

    // NVAPI Override
    CustomOptional<bool> DisableFlipMetering { false };
    CustomOptional<bool> DisableOTA { false };
    CustomOptional<bool> ImASillyGooseThatIsAboutToMisuseReflex { false };

    // Spoofing
    CustomOptional<bool, SoftDefault> DxgiSpoofing { true };
    CustomOptional<bool> DxgiFactoryWrapping { false };
    CustomOptional<bool> StreamlineSpoofing { true };
    CustomOptional<std::string, NoDefault> DxgiBlacklist; // disabled by default
    CustomOptional<int, NoDefault> DxgiVRAM;              // disabled by default
    CustomOptional<bool> VulkanSpoofing { false };
    CustomOptional<bool> VulkanExtensionSpoofing { false };
    CustomOptional<int, NoDefault> VulkanVRAM; // disabled by default
    CustomOptional<bool> SpoofHAGS { false };
    CustomOptional<bool> SpoofFeatureLevel { false };
    CustomOptional<uint32_t> SpoofedVendorId { VendorId::Nvidia };
    CustomOptional<uint32_t> SpoofedDeviceId { 0x2B85 };
    CustomOptional<uint32_t, NoDefault> TargetVendorId;
    CustomOptional<uint32_t, NoDefault> TargetDeviceId;
    CustomOptional<std::wstring> SpoofedGPUName { L"NVIDIA GeForce RTX 5090" };
    CustomOptional<bool> UESpoofIntelAtomics64 { false };
    CustomOptional<bool> SpoofRegistry { false };
    CustomOptional<bool> SpoofUser32 { false };
    CustomOptional<std::wstring> SpoofedDriver { L"32.0.15.9155" };

    // Plugins
    CustomOptional<std::wstring, NoDefault> PluginPath;
    CustomOptional<bool> LoadSpecialK { false };
    CustomOptional<bool> LoadReShade { false };
    CustomOptional<bool> LoadCustomAmdxc64OnRdna2 { false };
    CustomOptional<bool> LoadAsiPlugins { false };
    CustomOptional<int> LateAsiPluginsDelay { 30 };

    // Frame Generation
    CustomOptional<FGInput> FGInput { FGInput::NoFG };
    CustomOptional<FGOutput> FGOutput { FGOutput::NoFG };
    CustomOptional<FGNvngxReplacement> FGNvngxReplacement { FGNvngxReplacement::Nukems };
    CustomOptional<bool> FGDrawUIOverFG { false };
    CustomOptional<bool> FGUIPremultipliedAlpha { true };
    CustomOptional<bool> FGDisableHudless { false };
    CustomOptional<bool> FGDisableUI { false };
    CustomOptional<bool> FGSkipReset { false };
    CustomOptional<int> FGAllowedFrameAhead { 1 };
    CustomOptional<bool> FGDepthValidNow { false };
    CustomOptional<bool> FGVelocityValidNow { false };
    CustomOptional<bool> FGHudlessValidNow { false };
    CustomOptional<bool> FGOnlyAcceptFirstHudless { false };
    CustomOptional<bool> FGPreserveSwapChain { true };
    CustomOptional<bool> FGSkipResizeBuffers { false };
    CustomOptional<bool> FGModifyBufferState { false };
    CustomOptional<bool> FGModifySCIndex { false };
    CustomOptional<float> FGHudCutoff { 0.0f };
    CustomOptional<FrameTimeSource> FTInput { FrameTimeSource::Input };
    CustomOptional<bool> FGReprojectionEnabled { false }; // WIP Not saved or loaded

    // OptiFG
    // Unset (auto) follows the game's DLSSG state when FG Input is DLSSG, use IsFGEnabled() to read
    CustomOptional<bool, SoftDefault> FGEnabled { false };
    CustomOptional<bool> FGUseMutexForSwapchain { true };
    CustomOptional<bool> FGMakeMVCopy { true };
    CustomOptional<bool> FGMakeDepthCopy { true };
    CustomOptional<bool> FGResourceFlip { false };
    CustomOptional<bool> FGResourceFlipOffset { false };
    CustomOptional<bool> FGAlwaysCaptureFSRFGSwapchain { false };

    CustomOptional<int, NoDefault> FGRectLeft;
    CustomOptional<int, NoDefault> FGRectTop;
    CustomOptional<int, NoDefault> FGRectWidth;
    CustomOptional<int, NoDefault> FGRectHeight;

    // OptiFG - Hudfix
    CustomOptional<bool> FGDisableHUDFix { false };
    CustomOptional<bool> FGHUDFix { false };
    CustomOptional<int> FGHUDLimit { 1 };
    CustomOptional<bool> FGHUDFixExtended { false };
    CustomOptional<bool> FGImmediateCapture { false };
    CustomOptional<bool> FGHudfixPersistentBindings { true };
    CustomOptional<bool> FGDontUseSwapchainBuffers { false };
    CustomOptional<bool> FGRelaxedResolutionCheck { false };
    CustomOptional<bool> FGHudfixDisableRTV { false };
    CustomOptional<bool> FGHudfixDisableSRV { false };
    CustomOptional<bool> FGHudfixDisableUAV { false };
    CustomOptional<bool> FGHudfixDisableOM { false };
    CustomOptional<bool> FGHudfixDisableDispatch { false };
    CustomOptional<bool> FGHudfixDisableDI { false };
    CustomOptional<bool> FGHudfixDisableDII { false };
    CustomOptional<bool> FGHudfixDisableSCR { true };
    CustomOptional<bool> FGHudfixDisableSGR { true };

    // OptiFG - Resource Tracking
    CustomOptional<bool> FGAlwaysTrackHeaps { false };
    CustomOptional<bool> FGResourceBlocking { false };
    CustomOptional<bool> FGUseShards { false };

    // OptiFG - DLSS-D Depth scale
    CustomOptional<bool> FGEnableDepthScale { false };
    CustomOptional<float> FGDepthScaleMax { 10000.0f };

    // FSR-FG
    CustomOptional<bool> FGDebugView { false };
    CustomOptional<bool> FGDebugResetLines { false };
    CustomOptional<bool> FGDebugTearLines { false };
    CustomOptional<bool> FGDebugPacingLines { false };
    CustomOptional<bool> FGAsync { false };
    CustomOptional<bool> FGFramePacingTuning { true };
    CustomOptional<float> FGFPTSafetyMarginInMs { 0.01f };
    CustomOptional<float> FGFPTVarianceFactor { 0.3f };
    CustomOptional<bool> FGFPTAllowHybridSpin { false };
    CustomOptional<int> FGFPTHybridSpinTime { 2 };
    CustomOptional<bool> FGFPTAllowWaitForSingleObjectOnFence { false };

    CustomOptional<bool> FSRFGSkipConfigForHudless { false };
    CustomOptional<bool> FSRFGSkipDispatchForHudless { false };
    CustomOptional<bool> FSRFGEnableWatermark { false };

    // XeFG
    CustomOptional<bool> FGXeFGIgnoreInitChecks { false };
    CustomOptional<int> FGXeFGInterpolationCount { 1 };
    CustomOptional<int, NoDefault> FGXeFGOverrideInterpolationCount; // Game's own XeFG (XeFG input), 0 = off
    CustomOptional<bool> FGXeFGUIComposition { false };
    CustomOptional<bool> FGXeFGDepthInverted { true };
    CustomOptional<bool> FGXeFGJitteredMV { false };
    CustomOptional<bool> FGXeFGHighResMV { false };
    CustomOptional<bool> FGXeFGDebugView { false };
    CustomOptional<bool> FGXeFGForceBorderless { false };

    // DLSSG
    CustomOptional<int> FGDLSSGInterpolationCount { 1 }; // For Opti's own SL instance
    CustomOptional<bool> FGDLSSGUseGamesReflexMarkers { true };
    CustomOptional<int, NoDefault>
        FGDLSSGOverrideInterpolationCount; // For overriding game's value sent to SL, could be Nvngx FG, could be noFG
                                           // but someone just uses real DLSSG
    CustomOptional<bool> FGDLSSGOverrideForceDMFG { false };   // Overrides game's DLSSG mode to Dynamic
    CustomOptional<bool> FGDLSSGForceDMFG { false };           // Overrides Opti's DLSSG mode to Dynamic
    CustomOptional<float> FGDLSSGFramerateTargetDMFG { 0.0f }; // 0.0 means auto-detects the display refresh rate

    // Reprojection
    CustomOptional<ReprojectionFill> ReprojectionFillMode { ReprojectionFill::Dithering };
    CustomOptional<float> ReprojectionDepthCutoff { 0.1f };
    CustomOptional<uint32_t> ReprojectionCutoffExpand { 1 };
    CustomOptional<bool> ReprojectionLateLatch { true };

    // As per
    // https://github.com/artur-graniszewski/dlss-enabler-main/blob/a92464d468eb0d91ae17befa66c6bf6229f20b9f/Utils/DlssgProxy.cpp#L1033
    CustomOptional<uint32_t> NvngxFGDispatchFlags { 0x10000000 }; // IGNORE_UI_TEXTURE
    CustomOptional<bool> NvngxFGShowDebug { false };
    CustomOptional<bool> NvngxFGDisableHudless { false };

    // fakenvapi
    CustomOptional<bool> UseFakenvapi { true };
    CustomOptional<bool> ForceXeLL { false };
    CustomOptional<bool> FN_ForceLatencyFlex { false };
    CustomOptional<LFXMode> FN_LatencyFlexMode { LFXMode::Conservative };
    CustomOptional<ForceReflex> FN_ForceReflex { ForceReflex::InGame };
    CustomOptional<LowLatencyInput> LowLatencyInput { LowLatencyInput::Auto };
    CustomOptional<LowLatencyMode> LowLatencyOutput { LowLatencyMode::Auto };

    // Inputs
    CustomOptional<bool> EnableDlssInputs { true };
    CustomOptional<bool> EnableXeSSInputs { true };
    CustomOptional<bool> UseFsr2Inputs { true };
    CustomOptional<bool> UseFsr2Dx11Inputs { false };
    CustomOptional<bool> UseFsr2VulkanInputs { false };
    CustomOptional<bool> Fsr2Pattern { false };
    CustomOptional<bool> UseFsr3Inputs { true };
    CustomOptional<bool> Fsr3Pattern { false };
    // ---- [lmxxf2] chaves do núcleo neural do AMDNR (GPL-3.0, 3zwr1) ----
    CustomOptional<int> KeepPreviousLogs { 3 };
    CustomOptional<bool> CrashHandler { false };
    CustomOptional<bool> CrashDump { false };
    CustomOptional<bool> DlssNrEnabled { false };
    CustomOptional<bool> DlssNrRunBeforeSr { false };
    CustomOptional<bool> DlssNrApplyAfterRR { false };
    CustomOptional<unsigned int> DlssNrRRPasses { 1 };
    CustomOptional<float> DlssNrRRWorkingScale { 0.5f };
    CustomOptional<int> DlssNrToggleKey { VK_HOME }; // toggles Neural Rendering in-game, both runtimes; Backspace in the binder unbinds
    CustomOptional<int> DlssNrCaptureKey { UnboundKey };
    CustomOptional<uint32_t> DlssNrPreset { 0 };
    CustomOptional<float> DlssNrIntensity { 1.0f };
    CustomOptional<uint32_t> DlssNrStyle { 0 };
    CustomOptional<uint32_t, NoDefault> DlssNrPass2Preset;
    CustomOptional<uint32_t, NoDefault> DlssNrPass2Style;
    CustomOptional<uint32_t, NoDefault> DlssNrPass3Preset;
    CustomOptional<uint32_t, NoDefault> DlssNrPass3Style;
    CustomOptional<float> DlssNrLocalStructure { 1.0f };
    CustomOptional<float> DlssNrLocalTone { 1.0f };
    CustomOptional<bool> AmdNeuralLighting { true };
    CustomOptional<float> AmdNeuralLightingStrength { 1.0f };
    CustomOptional<int> AmdEncoding { 0 };
    CustomOptional<int> AmdHighlightProxy { 0 };
    CustomOptional<float> AmdHighlightProxyKnee { 1.0f };
    CustomOptional<float> AmdHighlightProxyRange { 1.0f };
    CustomOptional<int> AmdSlots { 1 };
    CustomOptional<float> AmdNrScale { 1 };
    CustomOptional<int> AmdNrSizeStep { 64 };
    CustomOptional<float> AmdTemporalStability { 0.6f };
    CustomOptional<int> AmdStabilityMode { 0 };
    CustomOptional<float> AmdStabilityThreshold { 0.04f };
    CustomOptional<float> AmdStabilityStaticRelax { 0.0f }; // O-4: 0.5 after the in-game A/B
    CustomOptional<bool> AmdStabilityStaticDebug { false };
    CustomOptional<float> AmdDetailStrength { 1.0f };
    CustomOptional<float> AmdColourStrength { 1.0f };
    CustomOptional<uint32_t> AmdComposition { 0 };
    CustomOptional<float> AmdComposeDetail { 1.0f };
    CustomOptional<float> AmdComposeColour { 1.0f };
    CustomOptional<float> AmdSharpness { 0.0f };
    CustomOptional<bool> AmdDynamicRes { false };
    CustomOptional<int> AmdDynamicTargetFps { 60 };
    CustomOptional<float> AmdInterleave { 0.f };
    CustomOptional<int> AmdInterleaveFill { 0 };
    CustomOptional<bool> AmdInterleaveSharp { true };
    CustomOptional<int> AmdInterleaveDebug { 0 };
    CustomOptional<int> AmdInterleavePreset { 10 };
    CustomOptional<float> AmdInterleavePacing { -1.f };
    CustomOptional<float> AmdInterleaveGhostBound { 0.6f };
    CustomOptional<bool> AmdInterleaveFreshHistory { false };
    CustomOptional<bool> AmdInterleaveModelHistory { false };
    CustomOptional<bool> AmdLmxxfHistory { true };
    // [lmxxf2] rejeição estilo TSR no arrasto da edição (Edit accumulation também sem Model interleave)
    CustomOptional<bool> AmdLmxxfTsrCarry { true };
    // [lmxxf2] resolucao livre da rede: o runtime roda o tamanho exato pedido (DLSS5_NETWORK_FREE_RES=1), sem os degraus
    // 720/900/1080, ate a superficie 4K dele. O runtime le a flag uma vez por processo: vale ao reabrir o jogo.
    CustomOptional<bool> AmdLmxxfFreeRes { true };
    // [lmxxf2] Screen Percentage da rede, como o do TSR: a rede roda em (quadro x SP) exato, 25..100 (% de cada eixo do
    // quadro em que a edicao e composta; com "Depois do upscale", a saida). 0 = automatica (NR resolution % do cartao
    // Desempenho, teto 1920x1080 como antes). So com a resolucao livre.
    CustomOptional<float> AmdLmxxfScreenPct { 0.0f };
    CustomOptional<bool> LmxxfFullNetwork { false };
    CustomOptional<float> AmdLmxxfEditDetail { 1.0f };
    CustomOptional<float> AmdLmxxfEditSaturation { 1.0f };
    CustomOptional<bool> AmdLmxxfAutoExposure { true };
    CustomOptional<bool> AmdLmxxfHighlightChromaGuard { true };
    CustomOptional<bool> AmdLmxxfAutoExposureHighlightCap { true };
    CustomOptional<float> AmdLmxxfEdgeGuard { 0.5f };
    CustomOptional<float> AmdLmxxfOutputSmooth { 0.6f };
    CustomOptional<bool> AmdInterleaveAdaptive { false };
    CustomOptional<int> AmdInterleaveAdaptiveSensitivity { 1 };
    CustomOptional<int> AmdJitterSign { 1 };
    CustomOptional<bool> AmdNetworkOutput { false };
    CustomOptional<float> AmdResidualIntensity { 1.0f };
    CustomOptional<float> AmdResidualLimit { 0.25f };
    CustomOptional<float> AmdResidualFade { 0.0f };
    CustomOptional<bool> AmdEditShaper { false };
    CustomOptional<int> AmdEditShaperLimit { 0 };
    CustomOptional<int> AmdEditShaperScope { 0 };
    CustomOptional<bool> AmdEditShaperCarryCap { false };
    CustomOptional<bool> AmdDanielHighlightGuard { false };
    CustomOptional<int> AmdRuntimeStyle { -1 };
    CustomOptional<int> AmdToneCurve { -1 };
    CustomOptional<float> AmdToneLift { -1.f };
    CustomOptional<int> AmdUseGameExposure { -1 };
    CustomOptional<bool> AmdDanielFastMode { false };
    CustomOptional<bool> AmdLmxxfTierSnap { false };
    CustomOptional<int> AmdLmxxfTierCap { 0 };
    CustomOptional<bool> AmdLmxxfFastMode { false };
    CustomOptional<bool> AmdResidualTemporal { false };
    CustomOptional<bool> AmdGraphicsWaitExperimental { true };
    CustomOptional<bool> AmdEveryFrame { true };
    CustomOptional<bool> AmdFinalImage { false };
    CustomOptional<int> AmdSpinDraw { 0 };
    CustomOptional<bool> AmdVkLateCopyWait { false };
    CustomOptional<bool> AmdNrColourGuard { true };
    CustomOptional<bool> AmdSkipUnwrittenFrames { true };
    CustomOptional<bool> AmdNeuralListRecovery { true };
    CustomOptional<bool> AmdDeviceGate { true };
    CustomOptional<bool> AmdOneStreamPerFrame { true };
    CustomOptional<bool> AmdLateSubmitGrace { true };
    CustomOptional<bool> AmdStreamlineDeviceFix { true };
    CustomOptional<bool> AmdRtgiEnabled { false };
    CustomOptional<uint32_t> AmdRtgiQuality { 2 };
    CustomOptional<uint32_t> AmdRtgiDenoiser { 1 };
    CustomOptional<uint32_t> AmdRtgiInspect { 0 };
    CustomOptional<float> AmdRtgiContact { 0 };
    CustomOptional<float> AmdRtgiSaturation { 1 };
    CustomOptional<float> AmdRtgiRadius { 1 };
    CustomOptional<float> AmdRtgiMix { 1 };
    CustomOptional<float> AmdRtgiLighting { 5 };
    CustomOptional<float> AmdRtgiOcclusion { 1 };
    CustomOptional<float> AmdRtgiAmbient { 1 };
    CustomOptional<float> AmdRtgiThickness { .1f };
    CustomOptional<float> AmdRtgiSmoothness { .5f };
    CustomOptional<float> AmdRtgiFade { .3f };
    CustomOptional<float> AmdRtgiFov { 60 };
    CustomOptional<float> AmdRtgiFarPlane { 600 };
    // [lmxxf2] Ray Regeneration do jogo (denoiser FFX 1.2 que o jogo cria): sem valor = o que o jogo pediu
    // [lmxxf2] TSR da rede (dlssnr/lmxxf/EditTsr.h): acumulo temporal da EDICAO da rede; o quadro do jogo fica intacto
    CustomOptional<bool> TsrRede { true };         // liga o TSR na edicao da rede
    CustomOptional<bool> TsrJitter { true };       // jitter subpixel na entrada da rede (a edicao converge para a saida)
    CustomOptional<float> TsrMaxSamples { 16.0f }; // amostras acumuladas no maximo: menos = responde rapido, mais = estavel
    CustomOptional<float> TsrClipGamma { 1.0f };   // largura do recorte do historico pela resposta nova: menos = menos atraso
    CustomOptional<float> TsrParallax { 1.0f };    // sensibilidade a desoclusao pela profundidade
    CustomOptional<float> TsrFlicker { 0.75f };    // rejeicao com deteccao de cintilacao (0 = desligada)
    CustomOptional<float> TsrFlickerMaxVel { 4.0f }; // px do quadro: movimento acima disso nao e protegido
    CustomOptional<bool> RrJogoAjustar { false };
    CustomOptional<float, NoDefault> RrJogoCrossBlNormStr;
    CustomOptional<float, NoDefault> RrJogoStabilityBias;
    CustomOptional<float, NoDefault> RrJogoMaxRadiance;
    CustomOptional<float, NoDefault> RrJogoRadianceClip;
    CustomOptional<float, NoDefault> RrJogoGaussKernRelax;
    CustomOptional<float, NoDefault> RrJogoDisocThreshold;
    CustomOptional<bool> AmdGiEnabled { false };
    CustomOptional<int> AmdGiQuality { 2 };          // 0 Low, 1 Medium, 2 High, 3 Ultra, 4 Auto; unset = 0 on APUs
    CustomOptional<float> AmdGiIntensity { 1.0f };   // Bounce light 0..3
    CustomOptional<float> AmdGiOcclusion { 1.0f };   // Ambient occlusion 0..2
    CustomOptional<float> AmdGiRadius { 1.0f };      // 0.25..4, multiplier on the tier's screen radius
    CustomOptional<float> AmdGiThickness { 0.0f };   // 0 = auto, 0.01..1 (fraction of view depth)
    CustomOptional<float> AmdGiSaturation { 1.0f };  // Bounce colour 0..2
    CustomOptional<float> AmdGiSky { 0.0f };         // Sky light 0..2
    CustomOptional<float> AmdGiFeedback { 0.5f };    // Multi-bounce 0..0.8
    CustomOptional<int> AmdGiPlacement { 0 };        // 0 before NR, 1 after NR
    CustomOptional<float> AmdGiFov { 0.0f };         // 0 = auto (camera chain), else 20..140 degrees
    CustomOptional<int> AmdGiFovAxis { 0 };          // 0 vertical, 1 horizontal
    CustomOptional<float> AmdGiNearFade { -1.0f };   // -1 auto, 0 off, > 0 value
    CustomOptional<float> AmdGiDistanceFade { -1.0f };
    CustomOptional<int> AmdGiEncoding { -1 };        // -1 auto, 0 linear, 1 sRGB, 2 gamma 2.2
    CustomOptional<int> AmdGiDebugView { 0 };        // 0..10, not saved
    CustomOptional<int> AmdGiDepthConvention { -1 }; // hidden: -1 auto, 0 standard, 1 reversed, 2 linear
    CustomOptional<float> AmdGiTraceCap { 0.0f };    // hidden: trace-grid Mpx, 0 = tier
    CustomOptional<int> AmdGiAlbedoMode { 0 };       // hidden: 0..2
    CustomOptional<float> AmdGiAoLitProtect { 1.0f }; // hidden: 0..1
    CustomOptional<int> AmdGiTranslucency { 0 };     // hidden: 0 off, 1 heuristic
    CustomOptional<bool> AmdLookEnabled { false };
    CustomOptional<uint32_t> AmdLookAppearance { 2 };
    CustomOptional<float> AmdLookMix { 1.0f };
    CustomOptional<float> AmdLookMaterialDetail { 1.15f };
    CustomOptional<float> AmdLookShapeDefinition { 1.20f };
    CustomOptional<float> AmdLookLocalLighting { 1.15f };
    CustomOptional<float> AmdLookSkinDetail { 1.10f };
    CustomOptional<float> AmdLookSkinSoftness { 0.486f };
    CustomOptional<bool> AmdLookDetectSkin { true };
    CustomOptional<float> AmdLookSpecularControl { 0.58f };
    CustomOptional<float> AmdLookHighlightRollOff { 0.9f };
    CustomOptional<float> AmdLookColourSeparation { 0.0f };
    CustomOptional<float> AmdLookShadowDepth { 0.2f };
    CustomOptional<float> AmdLookAntiHalo { 0.901f };
    CustomOptional<float> AmdLookFlatAreaProtection { 0.0f };
    CustomOptional<uint32_t> AmdLookInspect { 0 };
    CustomOptional<int> AmdLookStrength { 1 }; // [lmxxf2] força do filtro: 1x..4x
    CustomOptional<int> Lmxxf2Placement { 0 }; // [lmxxf2] 0 = rede antes do upscale, 1 = depois (na saída)
    CustomOptional<bool> Lmxxf2ModoSeguro { true }; // [lmxxf2] aprende com queda no carregamento (dlssnr/lmxxf2_seguro.h)
    CustomOptional<bool> Lmxxf2MenuShowAll { false }; // [lmxxf2] mostra as abas ocultas do OptiScaler
    // [lmxxf2-luz] iluminação RenoDX ([Lighting])
    CustomOptional<bool> Lmxxf2LightEnabled { false };
    CustomOptional<int> Lmxxf2LightToneMapper { 0 };
    CustomOptional<float> Lmxxf2LightExposure { 1.0f };
    CustomOptional<float> Lmxxf2LightHighlights { 1.0f };
    CustomOptional<float> Lmxxf2LightShadows { 1.0f };
    CustomOptional<float> Lmxxf2LightContrast { 1.0f };
    CustomOptional<float> Lmxxf2LightSaturation { 1.0f };
    CustomOptional<float> Lmxxf2LightDechroma { 0.5f };
    CustomOptional<float> Lmxxf2LightFlare { 0.0f };
    CustomOptional<float> Lmxxf2LightBlowout { 0.0f };
    CustomOptional<float> Lmxxf2LightHueStrength { 1.0f };
    CustomOptional<float> Lmxxf2LightPeakNits { 1000.0f };
    CustomOptional<float> Lmxxf2LightGameNits { 203.0f };
    CustomOptional<float> Lmxxf2LightMidGrayNits { 18.0f };
    CustomOptional<float> Lmxxf2LightGamma { 0.0f };
    CustomOptional<float> Lmxxf2LightMix { 1.0f };
    CustomOptional<float> AmdLookTone { 0.0f };
    CustomOptional<float> AmdLookExposureEV { 1.0f };
    CustomOptional<float> AmdLookContrast { 1.0f };
    CustomOptional<float> AmdLookSaturation { 1.0f };
    CustomOptional<float> AmdLookHighlightCompression { 0.0f };
    CustomOptional<float> DlssNrSkinStructure { -1.0f };
    CustomOptional<bool> DlssNrAutoMask { true };
    CustomOptional<bool> DlssNrSkinProtection { false };
    CustomOptional<bool> DlssNrSkinToneEnabled { true };
    CustomOptional<float> DlssNrSkinDetail { 1.0f };
    CustomOptional<float> DlssNrSkinColour { 1.0f };
    CustomOptional<float> DlssNrEnvironmentDetail { 1.0f };
    CustomOptional<float> DlssNrEnvironmentColour { 1.0f };
    CustomOptional<bool> DlssNrShowSkinMask { false };
    CustomOptional<float, NoDefault> DlssNrPass2Intensity;
    CustomOptional<float, NoDefault> DlssNrPass2LocalStructure;
    CustomOptional<float, NoDefault> DlssNrPass2LocalTone;
    CustomOptional<float, NoDefault> DlssNrPass2SkinStructure;
    CustomOptional<bool, NoDefault> DlssNrPass2AutoMask;
    CustomOptional<float, NoDefault> DlssNrPass3Intensity;
    CustomOptional<float, NoDefault> DlssNrPass3LocalStructure;
    CustomOptional<float, NoDefault> DlssNrPass3LocalTone;
    CustomOptional<float, NoDefault> DlssNrPass3SkinStructure;
    CustomOptional<bool, NoDefault> DlssNrPass3AutoMask;
    CustomOptional<float> DlssNrTransferStrength { 1.0f };
    CustomOptional<float> DlssNrColourStrength { 1.0f };
    CustomOptional<uint32_t> DlssNrReversibleMode { 0 };
    CustomOptional<bool> DlssNrApplyModel { true };
    CustomOptional<bool> DlssNrHoldFrame { false };
    CustomOptional<float> DlssNrMaxRatio { 2.0f };
    CustomOptional<uint32_t> DlssNrTransfer { 1 };
    CustomOptional<bool> DlssNrWhitePointFromExposure { true };
    CustomOptional<bool> DlssNrProbeD3D11 { false };
    CustomOptional<uint32_t> DlssNrDebugView { 0 };
    CustomOptional<uint32_t> DlssNrCompare { 0 };
    CustomOptional<float> DlssNrCompareSplit { 0.5f };
    CustomOptional<float> DlssNrCompareZoom { 1.0f };
    CustomOptional<bool> DlssNrCompareSwap { false };
    CustomOptional<bool> DlssNrCompareTags { false };
    CustomOptional<float> DlssNrTagScale { 1.5f };
    CustomOptional<float> DlssNrWorkingScale { 1.0f };
    CustomOptional<Scaler> DlssNrScalingDownscaler { Scaler::Lanczos3 };
    CustomOptional<bool> DlssNrProxyProbe { false };
    CustomOptional<bool> DlssNrUseProxy { false };
    CustomOptional<bool> DlssNrScanExposure { false };
    CustomOptional<uint32_t> DlssNrWhitePointSource { 1 };
    CustomOptional<bool> DlssNrScanMeter { false };
    CustomOptional<float> DlssNrScanAnchorValue { 0.0f };       // legacy single anchor, migrated then unused
    CustomOptional<float> DlssNrScanAnchorWhitePoint { 0.0f };  // legacy single anchor, migrated then unused
    CustomOptional<std::string> DlssNrScanAnchors { std::string() };
    CustomOptional<std::string> DlssNrBackend { std::string() };
    CustomOptional<std::string> AmdStyleSlot1 { std::string() };
    CustomOptional<std::string> AmdStyleSlot2 { std::string() };
    CustomOptional<std::string> AmdStyleSlot3 { std::string() };
    CustomOptional<bool> DlssNrScanInverted { false };
    CustomOptional<float> DlssNrWhitePointTrim { 1.0f };
    CustomOptional<float> DlssNrScanTrim { 1.0f };
    CustomOptional<uint32_t> DlssNrPasses { 1 };
    CustomOptional<bool> DlssNrAutoCapture { false };
    CustomOptional<float> DlssNrWhitePointScale { 1.0f };
    CustomOptional<float> RrDefaultSharpness { 0.25f };
    CustomOptional<bool> SkipOwnVulkanObjects { true };
    CustomOptional<bool> DiagNoInputHooks { false };
    CustomOptional<std::string> DiagInputHooksSkip { std::string() };
    CustomOptional<bool> DiagNoAmdxc64Hooks { false };
    CustomOptional<bool> DiagNoStreamlineHooks { false };
    CustomOptional<bool> DiagNoAntiLag2 { false };
    CustomOptional<bool> DiagNoTrustHooks { false };
    CustomOptional<bool> DiagNoFfxProxies { false };
    CustomOptional<bool> DiagNoXessProxies { false };
    CustomOptional<bool> DiagNoSwapchainWrap { false };
    CustomOptional<bool> DiagNoOverlay { false };
    CustomOptional<bool> DiagNoD3D12Hooks { false };
    CustomOptional<bool> DiagNoAmdGpuProbe { false };
    CustomOptional<int> MenuToggleDebounceMs { 400 };
    CustomOptional<bool> MenuLowLevelHookPassThrough { true };
    CustomOptional<bool> WarnMissingREFramework { true };
    CustomOptional<int> FfxDenoiserDiffuseSignalType { 0 };  // 0: Direct, 1: Indirect
    CustomOptional<int> FfxDenoiserSpecularSignalType { 1 }; // 0: Direct, 1: Indirect
    CustomOptional<float> FfxDenoiserFloorDetailBoost { 0.0f };
    CustomOptional<float> FfxDenoiserFloorNormalSharpness { 16.0f };
    CustomOptional<float> FfxDenoiserFloorAlbedoGuide { 1.0f };
    CustomOptional<float> FfxDenoiserFloorLumSymmetry { 1.0f };
    CustomOptional<float> FfxDenoiserFloorGrazingSharpness { 0.0f };
    CustomOptional<float> FfxDenoiserFloorEnvelopeBias { 0.0f };
    CustomOptional<float> FfxDenoiserFloorSoftMin { 0.0f };
    CustomOptional<bool> FfxDenoiserAllowPreRdna4 { false };
    CustomOptional<bool> FfxDenoiserPathTracedProfile { false };
    CustomOptional<float> FfxDenoiserProfileTextureRoute { 1.0f };
    CustomOptional<bool> FfxDenoiserPathTracedTemporal { true };
    CustomOptional<bool> FfxDenoiserAlbedoFp16 { false };
    CustomOptional<bool> FfxDenoiserNgxDirectSLConstants { false };
    CustomOptional<bool> FfxDenoiserSkinSmoothing { false };
    CustomOptional<float> FfxDenoiserSkinSmoothingStrength { 1.0f };
    CustomOptional<int> FfxDenoiserSkinSmoothingRadius { 6 };
    CustomOptional<bool> FfxDenoiserSkinSmoothingShowMask { false };
    CustomOptional<float> FfxDenoiserSkinSmoothingGuideLow { 0.004f };
    CustomOptional<float> FfxDenoiserSkinSmoothingGuideHigh { 0.04f };
    CustomOptional<int> FfxDenoiserSkinSmoothingClassifier { 1 };
    CustomOptional<float> FfxDenoiserSkinSmoothingMovedLow { 0.05f };
    CustomOptional<float> FfxDenoiserSkinSmoothingMovedHigh { 0.12f };
    CustomOptional<bool> FfxDenoiserSkinSmoothingShowCues { false };
    CustomOptional<float> FfxDenoiserCorrelationBias { 1.0f };
    CustomOptional<float> FfxDenoiserFloorIsolation { 1.0f };
    CustomOptional<float> FfxDenoiserRoughnessFloor { 0.1f };
    CustomOptional<int> FfxDenoiserFloorHandover { 1 };
    CustomOptional<float> FfxDenoiserFloorHandoverStrength { 1.0f };
    CustomOptional<float> FfxDenoiserFloorRawBlend { 1.0f };
    CustomOptional<float> FfxDenoiserFloorClampSmoothing { 0.0f };
    CustomOptional<float> FfxDenoiserFloorStructureGate { 1.0f };
    CustomOptional<float> FfxDenoiserFloorHandoverDetail { 1.0f };
    CustomOptional<bool> ExternalFrameGeneration { false };
    CustomOptional<bool> FGAllowXeFGWithNativeXeFG { false };
    CustomOptional<bool> FGXeFGAllowDilatedMV { false };
    // [lmxxf2] teto dos patches do XeFG/XeLL (XeFGUnlock.h / XeLLUnLock.h); o que vale e MaxInterpolatedFrames
    static constexpr int32_t XeFGMaxInterpolations = 7;
    CustomOptional<bool> FGXeFGUnlockEnabled { true };
    CustomOptional<bool> FGXeFGUnlockUnverified { false };
    CustomOptional<int> FGXeFGMaxInterpolatedFrames { 5 };
    CustomOptional<bool> FGXeFGExtraPacing { true };
    CustomOptional<bool> FGXeFGPaceOnSwapchain { false };
    // ---- fim [lmxxf2] ----
    CustomOptional<bool> UseFfxInputs { true };
    CustomOptional<bool> EnableHotSwapping { false };
    CustomOptional<bool> EnableFsr2Inputs { true };
    CustomOptional<bool> EnableFsr3Inputs { true };
    CustomOptional<bool> EnableFfxInputs { true };

    // Framerate
    CustomOptional<float> FramerateLimit { 0.0f };

    // HDR
    CustomOptional<bool> ForceHDR { false };
    CustomOptional<bool> UseHDR10 { false };
    CustomOptional<bool> SkipColorSpace { false };

    // V-Sync
    CustomOptional<bool> OverrideVsync { false };
    CustomOptional<bool, NoDefault> ForceVsync;
    CustomOptional<UINT> VsyncInterval { 0 };

    // Old configs for compat reasons
    CustomOptional<bool, NoDefault> _DONTUSE_Fsr4ForceEnableInt8;

    bool LoadFromPath(const wchar_t* InPath);
    bool SaveIni();
    bool SaveXeFG();

    void CheckUpscalerFiles();

    // FG on/off and interpolation count, following the game's DLSSG state when FGEnabled is unset
    static bool FGCanFollowGame();
    bool FGFollowsGame() const;
    bool IsFGEnabled() const;
    int FGInterpolationCount(int outputCount) const;
    // Applies the FSR-RR denoiser's default profile: every FfxDenoiser* setting
    // goes back to the default declared on its member. The single place that
    // knows the complete setting set and which keys take effect at context
    // creation - the menu's Reset button goes through this instead of keeping
    // its own assignment list. Returns true when a context-creation setting
    // changed and the caller must rebuild the RR context.
    bool ResetFfxDenoiserSettings();
    void ApplyFfxDenoiserProfile(int profile);
    int GetFfxDenoiserProfile() const;

    std::vector<std::string> GetConfigLog();

    static Config* Instance();

  private:
    inline static Config* _config;
    inline static std::vector<std::string> _log;

    std::filesystem::path absoluteFileName;
    std::wstring fileName = L"OptiScaler.ini";

    bool Reload(std::filesystem::path iniPath);

    std::optional<std::string> readString(std::string section, std::string key, bool lowercase = false);
    std::optional<std::wstring> readWString(std::string section, std::string key, bool lowercase = false);
    std::optional<float> readFloat(std::string section, std::string key);
    std::optional<int> readInt(std::string section, std::string key);
    std::optional<uint32_t> readUInt(std::string section, std::string key);
    std::optional<bool> readBool(std::string section, std::string key);

    template <typename Enum> std::optional<Enum> readEnum(std::string section, std::string key);
};
