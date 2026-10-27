# Geracao multipla (3x/4x) no FidelityFX SDK 1.1.4 (MIT): a AMD gera 1 quadro no meio (t = 0.5 fixo nos shaders e um
# quadro gerado por apresentacao no swapchain). Este patch:
#  - shaders da interpolacao: t = k/N no lugar do 0.5 (constante fInterpolationT, no lugar do _pad1 do cbFI; 0 = 0.5).
#    Os campos de vetores guardam, no ponto do quadro gerado, o vetor ate o quadro atual (negado), mv * (1 - t); ate o
#    anterior e o mesmo vetor x t / (1 - t) (com t = 0.5 os dois sao o mv / 2 da AMD: nada muda no 2x)
#  - ffxFrameInterpolationDispatch: interpolationT e keepPreviousInterpolationSource (varios gerados do mesmo par: so o
#    ultimo guarda o quadro atual como anterior)
#  - swapchain DX12: ate 3 gerados por quadro do jogo (FfxFrameGenerationConfig.numInterpolatedFrames), apresentados em
#    ordem com o intervalo do jogo dividido por N
# Idempotente: o que ja foi aplicado e pulado. Uso: python ffx_multiquadro.py <externo\fidelityfx_sdk\sdk>
from pathlib import Path
import sys

sdk = Path(sys.argv[1])
FI = 'include/FidelityFX/gpu/frameinterpolation/'
SC = 'src/backends/dx12/FrameInterpolationSwapchain/'
mudou = 0


def tr(arq, a, b, n=1, depois=None):
    # depois: o texto de um trecho mais adiante que muda este de novo (ja aplicado tambem)
    global mudou
    p = sdk / arq
    s = p.read_text(encoding='utf-8')
    if b in s or (depois and depois in s):  # ja aplicado
        return
    assert s.count(a) == n, (arq, s.count(a), a[:80])
    p.write_text(s.replace(a, b), encoding='utf-8', newline='')
    mudou += 1


# ---------------- shaders ----------------
tr(FI + 'ffx_frameinterpolation_callbacks_hlsl.h', '''        FfxFloat32      fTanHalfFOV;
        FfxInt32        _pad1;''', '''        FfxFloat32      fTanHalfFOV;
        FfxFloat32      fInterpolationT; // DLSS5 Overlay: era _pad1''')
tr(FI + 'ffx_frameinterpolation_callbacks_hlsl.h', '''    FfxFloat32 TanHalfFoV()
    {
        return fTanHalfFOV;
    }
''', '''    FfxFloat32 TanHalfFoV()
    {
        return fTanHalfFOV;
    }

    // DLSS5 Overlay (geracao multipla): o instante do quadro gerado entre o anterior (0) e o atual (1); 0.5 = 2x
    FfxFloat32 InterpolationT()
    {
        return (fInterpolationT > 0.0f && fInterpolationT < 1.0f) ? fInterpolationT : 0.5f;
    }

    // os campos guardam o vetor do quadro gerado ate o atual (negado); ate o anterior e o mesmo vetor x t / (1 - t)
    FfxFloat32 PreviousVectorScale()
    {
        return InterpolationT() / (1.0f - InterpolationT());
    }
''')
tr(FI + 'ffx_frameinterpolation_game_motion_vector_field.h',
   '    const FfxFloat32x2 fMotionVectorHalf = fGameMotionVector * 0.5f;',
   '    const FfxFloat32x2 fMotionVectorHalf = fGameMotionVector * (1.0f - InterpolationT()); // DLSS5 Overlay: era 0.5')
tr(FI + 'ffx_frameinterpolation_optical_flow_vector_field.h',
   '    FfxFloat32x2 fMotionVectorHalf = fOpticalFlowVector * 0.5f;',
   '    FfxFloat32x2 fMotionVectorHalf = fOpticalFlowVector * (1.0f - InterpolationT()); // DLSS5 Overlay: era 0.5')
tr(FI + 'ffx_frameinterpolation_reconstruct_previous_depth.h',
   '    ReconstructPrevDepth(iPxPos, 1, fDilatedDepth, fMotionVector * 0.5f, RenderSize());',
   '    ReconstructPrevDepth(iPxPos, 1, fDilatedDepth, fMotionVector * (1.0f - InterpolationT()), RenderSize()); // DLSS5 Overlay: era 0.5')
tr(FI + 'ffx_frameinterpolation_common.h',
   '    vfElement.bPosOutside = !IsUvInside(fUv + vfElement.fMotionVector);',
   '    vfElement.bPosOutside = !IsUvInside(fUv + vfElement.fMotionVector * PreviousVectorScale()); // DLSS5 Overlay', n=2)
tr(FI + 'ffx_frameinterpolation_disocclusion_mask.h',
   '    const FfxFloat32 fDepthClipInterpolatedToPrevious   = 1.0f - ComputeDepthClip(0, fDepthUv + gameMv.fMotionVector, fDilatedDepth);',
   '    const FfxFloat32 fDepthClipInterpolatedToPrevious   = 1.0f - ComputeDepthClip(0, fDepthUv + gameMv.fMotionVector * PreviousVectorScale(), fDilatedDepth); // DLSS5 Overlay')
tr(FI + 'ffx_frameinterpolation_disocclusion_mask.h',
   '    const FfxFloat32x2 fSrcMotionVector   = gameMv.fMotionVector * 2.0f;',
   '    const FfxFloat32x2 fSrcMotionVector   = gameMv.fMotionVector / (1.0f - InterpolationT()); // DLSS5 Overlay: o movimento inteiro (era x 2)')
tr(FI + 'ffx_frameinterpolation.h',
   'SampleTextureBilinear(false, fUvInScreenSpace, +gameMv.fMotionVector * fUvLetterBoxScale, DisplaySize());',
   'SampleTextureBilinear(false, fUvInScreenSpace, +gameMv.fMotionVector * PreviousVectorScale() * fUvLetterBoxScale, DisplaySize()); // DLSS5 Overlay:')
tr(FI + 'ffx_frameinterpolation.h',
   'SampleTextureBilinear(false, fUvInScreenSpace, +ofMv.fMotionVector * fUvLetterBoxScale, DisplaySize());',
   'SampleTextureBilinear(false, fUvInScreenSpace, +ofMv.fMotionVector * PreviousVectorScale() * fUvLetterBoxScale, DisplaySize()); // DLSS5 Overlay')
tr(FI + 'ffx_frameinterpolation.h', '''        FfxFloat32 t = 0.5f;
        t += 0.5f * (1 - (fDisocclusionFactor.x));
        t -= 0.5f * (1 - (fDisocclusionFactor.y));''', '''        // DLSS5 Overlay: t = o instante do gerado (era 0.5); so o atual visivel -> 1, so o anterior -> 0
        const FfxFloat32 fT = InterpolationT();
        FfxFloat32 t = fT;
        t += (1.0f - fT) * (1 - (fDisocclusionFactor.x));
        t -= fT * (1 - (fDisocclusionFactor.y));''')
tr(FI + 'ffx_frameinterpolation.h', '''        FfxFloat32 ofT = 0.5f;

        if (fPrevColorOF.fBilinearWeightSum > 0 && fCurrColorOF.fBilinearWeightSum > 0)
        {
            ofT = 0.5f;''', '''        FfxFloat32 ofT = InterpolationT(); // DLSS5 Overlay: era 0.5

        if (fPrevColorOF.fBilinearWeightSum > 0 && fCurrColorOF.fBilinearWeightSum > 0)
        {
            ofT = InterpolationT();''')

# ---------------- interpolacao (CPU) ----------------
tr('src/components/frameinterpolation/ffx_frameinterpolation_private.h', '''    float    fTanHalfFOV;
    float   _pad1;''', '''    float    fTanHalfFOV;
    float   interpolationT; // DLSS5 Overlay: era _pad1 (0.5 = 2x)''')
tr('include/FidelityFX/host/ffx_frameinterpolation.h', '''    FfxResource                         distortionField;                    ///< A resource containing distortion offset data used when distortion post effects are enabled.
} FfxFrameInterpolationDispatchDescription;''', '''    FfxResource                         distortionField;                    ///< A resource containing distortion offset data used when distortion post effects are enabled.

    // DLSS5 Overlay (geracao multipla, 3x/4x): varios quadros gerados entre o mesmo par de quadros do jogo
    float                               interpolationT;                     ///< instante do gerado entre o quadro anterior (0) e o atual (1); 0 = 0.5 (2x)
    bool                                keepPreviousInterpolationSource;    ///< nao guarda o atual como anterior: ainda ha gerados deste par
} FfxFrameInterpolationDispatchDescription;''')
tr('src/components/frameinterpolation/ffx_frameinterpolation.cpp', '''    contextPrivate->constants.fTanHalfFOV               = tanf(cameraAngleHorizontal * 0.5f);
''', '''    contextPrivate->constants.fTanHalfFOV               = tanf(cameraAngleHorizontal * 0.5f);
    // DLSS5 Overlay: o instante deste gerado (0.5 = 2x)
    contextPrivate->constants.interpolationT = (params->interpolationT > 0.f && params->interpolationT < 1.f) ? params->interpolationT : 0.5f;
''')
tr('src/components/frameinterpolation/ffx_frameinterpolation.cpp', '''        // store current buffer
        {''', '''        // store current buffer (DLSS5 Overlay: so no ultimo gerado do par)
        if (!params->keepPreviousInterpolationSource)
        {''')

# ---------------- swapchain ----------------
tr('include/FidelityFX/host/ffx_interface.h', '''    bool                            drawDebugPacingLines;            ///< Sets the state of pacing debug lines. Set to true to display debug lines
} FfxFrameGenerationConfig;''', '''    bool                            drawDebugPacingLines;            ///< Sets the state of pacing debug lines. Set to true to display debug lines
    uint32_t                        numInterpolatedFrames;           ///< DLSS5 Overlay: quadros gerados por quadro do jogo, 1 (2x) a 3 (4x); 0 = 1
} FfxFrameGenerationConfig;''')
tr(SC + 'FrameInterpolationSwapchainDX12.h', '''#define FFX_FRAME_INTERPOLATION_SWAP_CHAIN_MAX_BUFFER_COUNT 6
''', '''#define FFX_FRAME_INTERPOLATION_SWAP_CHAIN_MAX_BUFFER_COUNT 6
#define FFX_FRAME_INTERPOLATION_SWAP_CHAIN_MAX_INTERPOLATED  3 // DLSS5 Overlay: ate 4x
''')
tr(SC + 'FrameInterpolationSwapchainDX12.h', '''        Interpolated_1,
        Real,''', '''        Interpolated_1,
        Interpolated_2, // DLSS5 Overlay: 3x
        Interpolated_3, // DLSS5 Overlay: 4x
        Real,''')
tr(SC + 'FrameInterpolationSwapchainDX12.h',
   '    ReplacementResource interpolationOutputs[2]                                                      = {};',
   '    ReplacementResource interpolationOutputs[2 * FFX_FRAME_INTERPOLATION_SWAP_CHAIN_MAX_INTERPOLATED] = {}; // DLSS5 Overlay: 2 jogos de ate 3')
tr(SC + 'FrameInterpolationSwapchainDX12.h', '''    bool                drawDebugPacingLines            = false;
''', '''    bool                drawDebugPacingLines            = false;
    UINT                numInterpolatedFrames           = 1; // DLSS5 Overlay: gerados por quadro do jogo (1 = 2x)
''')
tr(SC + 'FrameInterpolationSwapchainDX12.h', '''    void dispatchInterpolationCommands(FfxResource* pInterpolatedFrame, FfxResource* pRealFrame);''',
   '''    void dispatchInterpolationCommands(FfxResource* pInterpolatedFrames, FfxResource* pRealFrame); // DLSS5 Overlay: ate 3 gerados
    bool verifyInterpolationOutputs();''')

c = SC + 'FrameInterpolationSwapchainDX12.cpp'
M = 'FFX_FRAME_INTERPOLATION_SWAP_CHAIN_MAX_INTERPOLATED'
# pacing: o intervalo do jogo dividido pelos N quadros apresentados (era / 2)
tr(c, '''                    const int64_t conservativeAvg   = int64_t(frameTime.getAverage() * 0.5 - frameTime.getVariance() * presenter->varianceFactor);
                    const int64_t deltaToUse        = conservativeAvg > qpcSafetyMargin ? (conservativeAvg - qpcSafetyMargin) : 0;
                    entry.frames[PacingData::FrameType::Interpolated_1].presentQpcDelta = deltaToUse;
                    entry.frames[PacingData::FrameType::Real].presentQpcDelta           = deltaToUse;''', '''                    // DLSS5 Overlay: os gerados e o real dividem o intervalo do jogo (era * 0.5, 2x)
                    uint32_t numFrames = 1;
                    for (uint32_t k = 0; k < ''' + M + '''; k++)
                        numFrames += entry.frames[PacingData::FrameType::Interpolated_1 + k].doPresent ? 1 : 0;
                    const int64_t conservativeAvg   = int64_t(frameTime.getAverage() / numFrames - frameTime.getVariance() * presenter->varianceFactor);
                    const int64_t deltaToUse        = conservativeAvg > qpcSafetyMargin ? (conservativeAvg - qpcSafetyMargin) : 0;
                    for (uint32_t k = 0; k < PacingData::FrameType::Count; k++)
                        entry.frames[k].presentQpcDelta = deltaToUse;''')
# saidas: so as dos gerados em uso
tr(c, '''                if (interpolationOutputs[i].resource == nullptr)
                {
                    if (FAILED(device->CreateCommittedResource(''', '''                if (interpolationOutputs[i].resource == nullptr && (i % ''' + M + ''') < numInterpolatedFrames) // DLSS5 Overlay
                {
                    if (FAILED(device->CreateCommittedResource(''')
tr(c, '''FfxResource FrameInterpolationSwapChainDX12::interpolationOutput(int index)
{
    index = interpolationBufferIndex;
''', '''// DLSS5 Overlay: as saidas que faltam quando o numero de gerados sobe (criadas so para os gerados em uso)
bool FrameInterpolationSwapChainDX12::verifyInterpolationOutputs()
{
    ID3D12Resource* base = replacementSwapBuffers[0].resource;
    if (base == nullptr)
        return false;

    bool ok = true;
    for (size_t i = 0; i < _countof(interpolationOutputs); i++)
    {
        if (interpolationOutputs[i].resource != nullptr || (i % ''' + M + ''') >= numInterpolatedFrames)
            continue;

        ID3D12Device* device = nullptr;
        if (FAILED(base->GetDevice(IID_PPV_ARGS(&device))))
            return false;

        auto bufferDesc = base->GetDesc();
        bufferDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_CLEAR_VALUE clearValue{ bufferDesc.Format, 0.f, 0.f, 0.f, 1.f };

        D3D12_HEAP_PROPERTIES heapProperties{};
        D3D12_HEAP_FLAGS      heapFlags;
        base->GetHeapProperties(&heapProperties, &heapFlags);
        heapFlags &= ~D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
        heapFlags &= ~D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES;
        heapFlags &= ~D3D12_HEAP_FLAG_DENY_BUFFERS;
        heapFlags &= ~D3D12_HEAP_FLAG_ALLOW_DISPLAY;

        if (FAILED(device->CreateCommittedResource(
                &heapProperties, heapFlags, &bufferDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &clearValue, IID_PPV_ARGS(&interpolationOutputs[i].resource))))
        {
            ok = false;
        }
        else
        {
            totalUsageInBytes += GetResourceGpuMemorySize(interpolationOutputs[i].resource);
            interpolationOutputs[i].resource->SetName(L"AMD FSR Interpolation Output");
        }

        SafeRelease(device);
    }

    return ok;
}

FfxResource FrameInterpolationSwapChainDX12::interpolationOutput(int index)
{
    index = interpolationBufferIndex * ''' + M + ''' + index; // DLSS5 Overlay: o gerado `index` do jogo de saidas atual
''')
tr(c, '''        currentFrameID          = config->frameID;
        presentInterpolatedOnly = config->onlyPresentInterpolated;''', '''        currentFrameID          = config->frameID;
        presentInterpolatedOnly = config->onlyPresentInterpolated;
        // DLSS5 Overlay: gerados por quadro do jogo (1 = 2x .. 3 = 4x), muda sem refazer nada
        numInterpolatedFrames   = FFX_MINIMUM(FFX_MAXIMUM(config->numInterpolatedFrames, 1u), (uint32_t)''' + M + ''');''')
tr(c, '''void FrameInterpolationSwapChainDX12::dispatchInterpolationCommands(FfxResource* pInterpolatedFrame, FfxResource* pRealFrame)
{
    FFX_ASSERT(pInterpolatedFrame);''', '''void FrameInterpolationSwapChainDX12::dispatchInterpolationCommands(FfxResource* pInterpolatedFrames, FfxResource* pRealFrame)
{
    FFX_ASSERT(pInterpolatedFrames);''')
tr(c, '''    presentInfo.interpolationQueue->Wait(presentInfo.compositionFenceGPU, interpolationOutputs[interpolationBufferIndex].availabilityFenceValue);''',
   '''    presentInfo.interpolationQueue->Wait(presentInfo.compositionFenceGPU, interpolationOutputs[interpolationBufferIndex * ''' + M + '''].availabilityFenceValue);''')
tr(c, '''        *pInterpolatedFrame = interpolationOutput();
        frameInterpolationResetCondition = false;
    }''', '''        pInterpolatedFrames[0] = interpolationOutput();
        frameInterpolationResetCondition = false;
    }''')
tr(c, '''        FfxFrameGenerationDispatchDescription desc{};
        desc.commandList = dx12CommandList;
        desc.outputs[0] = interpolationOutput();
        desc.presentColor = backbuffer;
        desc.reset = frameInterpolationResetCondition;
        desc.numInterpolatedFrames = 1;''', '''        // DLSS5 Overlay: ate 3 gerados, cada um na sua saida
        const UINT numFrames = verifyInterpolationOutputs() ? numInterpolatedFrames : 1;

        FfxFrameGenerationDispatchDescription desc{};
        desc.commandList = dx12CommandList;
        for (UINT k = 0; k < numFrames; k++)
            desc.outputs[k] = interpolationOutput(k);
        desc.presentColor = backbuffer;
        desc.reset = frameInterpolationResetCondition;
        desc.numInterpolatedFrames = numFrames;''')
tr(c, '''        if (desc.numInterpolatedFrames > 0)
        {
            frameInterpolationResetCondition = false;
            *pInterpolatedFrame = interpolationOutput();
        }''', '''        if (desc.numInterpolatedFrames > 0)
        {
            frameInterpolationResetCondition = false;
            for (UINT k = 0; k < desc.numInterpolatedFrames; k++)
                pInterpolatedFrames[k] = interpolationOutput(k);
        }''', depois='gerou && desc.numInterpolatedFrames')
tr(c, '''    FfxResource interpolatedFrame{}, realFrame{};
    dispatchInterpolationCommands(&interpolatedFrame, &realFrame);''', '''    FfxResource interpolatedFrames[''' + M + ''']{}, realFrame{};
    dispatchInterpolationCommands(interpolatedFrames, &realFrame);''')
tr(c, '''    // interpolated
    PacingData::FrameInfo& fiInterpolated = entry.frames[PacingData::FrameType::Interpolated_1];
    if (interpolatedFrame.resource != nullptr)
    {
        fiInterpolated.doPresent                        = true;
        fiInterpolated.resource                         = interpolatedFrame;
        fiInterpolated.interpolationCompletedFenceValue = interpolationFenceValue;
        fiInterpolated.presentIndex                     = ++framesSentForPresentation;
    }''', '''    // interpolated (DLSS5 Overlay: os gerados em ordem, antes do real)
    UINT numInterpolatedPresents = 0;
    for (UINT k = 0; k < ''' + M + '''; k++)
    {
        PacingData::FrameInfo& fiInterpolated = entry.frames[PacingData::FrameType::Interpolated_1 + k];
        if (interpolatedFrames[k].resource != nullptr)
        {
            fiInterpolated.doPresent                        = true;
            fiInterpolated.resource                         = interpolatedFrames[k];
            fiInterpolated.interpolationCompletedFenceValue = interpolationFenceValue;
            fiInterpolated.presentIndex                     = ++framesSentForPresentation;
            numInterpolatedPresents++;
        }
    }''')
tr(c, '''    interpolationOutputs[interpolationBufferIndex].availabilityFenceValue = entry.numFramesSentForPresentationBase + fiInterpolated.doPresent;''',
   '''    interpolationOutputs[interpolationBufferIndex * ''' + M + '''].availabilityFenceValue = entry.numFramesSentForPresentationBase + numInterpolatedPresents;''')
tr(c, '''    interpolationBufferIndex                                                   = presentCount % _countof(interpolationOutputs);''',
   '''    interpolationBufferIndex                                                   = presentCount % 2; // DLSS5 Overlay: 2 jogos de saidas''')
# ritmo real das apresentacoes (intervalo entre os Present do presenter): diagnostico lido pelo modulo
tr(c, '''DWORD WINAPI presenterThread(LPVOID param)
{''', '''// DLSS5 Overlay: o ritmo das apresentacoes (ms entre os Present do presenter), lido e zerado por ffxDlss5RitmoDX12
#include <cmath>
static SRWLOCK  g_dlss5RitmoTrava = SRWLOCK_INIT;
static double   g_dlss5RitmoSoma = 0.0, g_dlss5RitmoSoma2 = 0.0, g_dlss5RitmoMax = 0.0;
static uint32_t g_dlss5RitmoN = 0;

static void dlss5RitmoAcumula(double ms)
{
    AcquireSRWLockExclusive(&g_dlss5RitmoTrava);
    g_dlss5RitmoSoma += ms;
    g_dlss5RitmoSoma2 += ms * ms;
    g_dlss5RitmoMax = FFX_MAXIMUM(g_dlss5RitmoMax, ms);
    g_dlss5RitmoN++;
    ReleaseSRWLockExclusive(&g_dlss5RitmoTrava);
}

extern "C" void ffxDlss5RitmoDX12(double* mediaMs, double* desvioMs, double* maxMs, uint32_t* n)
{
    AcquireSRWLockExclusive(&g_dlss5RitmoTrava);
    const double media = g_dlss5RitmoN ? g_dlss5RitmoSoma / g_dlss5RitmoN : 0.0;
    *mediaMs  = media;
    *desvioMs = g_dlss5RitmoN ? std::sqrt(FFX_MAXIMUM(0.0, g_dlss5RitmoSoma2 / g_dlss5RitmoN - media * media)) : 0.0;
    *maxMs    = g_dlss5RitmoMax;
    *n        = g_dlss5RitmoN;
    g_dlss5RitmoSoma = g_dlss5RitmoSoma2 = g_dlss5RitmoMax = 0.0;
    g_dlss5RitmoN = 0;
    ReleaseSRWLockExclusive(&g_dlss5RitmoTrava);
}

DWORD WINAPI presenterThread(LPVOID param)
{''')
tr(c, '''                            int64_t currentPresentQPC;
                            QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&currentPresentQPC));
                            presenter->previousPresentQpc = currentPresentQPC;''', '''                            int64_t currentPresentQPC;
                            QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&currentPresentQPC));
                            if (presenter->previousPresentQpc) // DLSS5 Overlay: ritmo
                                dlss5RitmoAcumula(double(currentPresentQPC - presenter->previousPresentQpc) * 1000.0 / double(qpcFrequency));
                            presenter->previousPresentQpc = currentPresentQPC;''')
# o dono do contexto (o modulo) e avisado na destruicao: o swapchain e so do jogo (nada fica preso a ele -- preso, a janela
# continuava ocupada quando o jogo recriava o swapchain, e na saida do processo as threads da AMD morriam com ele vivo: o
# driver caia em amdxc64.dll). O aviso vem depois das apresentacoes em voo e antes de soltar o resto
tr(SC + 'FrameInterpolationSwapchainDX12.h', '''    UINT                numInterpolatedFrames           = 1; // DLSS5 Overlay: gerados por quadro do jogo (1 = 2x)
''', '''    UINT                numInterpolatedFrames           = 1; // DLSS5 Overlay: gerados por quadro do jogo (1 = 2x)
public:
    void              (*dlss5AoDestruir)(void*)         = nullptr; // DLSS5 Overlay: aviso de destruicao ao dono
    void*               dlss5AoDestruirCtx              = nullptr;
protected:
''')
tr(c, '''FrameInterpolationSwapChainDX12::~FrameInterpolationSwapChainDX12()
{
    shutdown();
}''', '''FrameInterpolationSwapChainDX12::~FrameInterpolationSwapChainDX12()
{
    // DLSS5 Overlay: o dono solta o que usa (a interpolacao dele roda nas apresentacoes: ja terminaram)
    if (dlss5AoDestruir && presentInfo.device)
    {
        waitForPresents();
        dlss5AoDestruir(dlss5AoDestruirCtx);
    }
    shutdown();
}

// DLSS5 Overlay: aviso de destruicao ao dono (chamado uma vez, na thread que solta a ultima referencia)
extern "C" FfxErrorCode ffxDlss5AoDestruirDX12(FfxSwapchain gameSwapChain, void (*aviso)(void*), void* ctx)
{
    IDXGISwapChain4* swapChain = ffxGetDX12SwapchainPtr(gameSwapChain);
    FrameInterpolationSwapChainDX12* fi = nullptr;
    if (swapChain && SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&fi))))
    {
        fi->dlss5AoDestruir    = aviso;
        fi->dlss5AoDestruirCtx = ctx;
        SafeRelease(fi);
        return FFX_OK;
    }
    return FFX_ERROR_INVALID_ARGUMENT;
}''')
# o callback falhou (sem o preparo do quadro): a apresentacao vai so com o real (antes a AMD apresentava as saidas da
# interpolacao assim mesmo, com o conteudo de 2 quadros atras: um gerado velho na tela)
tr(c, '''        if (frameGenerationCallback(&desc, frameGenerationCallbackContext) == FFX_OK)
        {
            interpolationCommandList->execute(true);''', '''        const bool gerou = frameGenerationCallback(&desc, frameGenerationCallbackContext) == FFX_OK; // DLSS5 Overlay
        if (gerou)
        {
            interpolationCommandList->execute(true);''')
tr(c, '''        if (desc.numInterpolatedFrames > 0)
        {
            frameInterpolationResetCondition = false;
            for (UINT k = 0; k < desc.numInterpolatedFrames; k++)''', '''        if (gerou && desc.numInterpolatedFrames > 0) // DLSS5 Overlay: falhou -> so o real
        {
            frameInterpolationResetCondition = false;
            for (UINT k = 0; k < desc.numInterpolatedFrames; k++)''')
# com a geracao, as apresentacoes saem sem vsync (com ALLOW_TEARING quando o sistema deixa), seja qual for o do jogo: com
# vsync cada quadro gerado esperava uma atualizacao da tela -- num compositor a 60 Hz (a taxa dinamica do Windows 11 deixa
# a area de trabalho a 60 com o sinal a 120) o jogo caia de 60 para 30 q/s e a tela seguia em 60 (medido na Sponza 4K do
# sample da AMD). O ritmo da AMD ja espaca os quadros e o FreeSync sincroniza a tela; sem geracao vale o vsync do jogo
tr(c, '''    entry.vsync                             = bVsync;''',
   '''    entry.vsync                             = false; // DLSS5 Overlay: era bVsync (o do jogo)''')
tr(c, '''    entry.vsync                             = false; // DLSS5 Overlay: era bVsync (o do jogo)
''', '''    entry.vsync                             = false; // DLSS5 Overlay: era bVsync (o do jogo)
    (void)bVsync;
''')
# a fila do jogo fica segura enquanto o swapchain existe: dentro do swapchain de geracao do proprio jogo (o FSR 3 dele,
# ligado no menu do TLOU2), a "fila do jogo" que o nosso recebe e a fila de apresentacao interna do de fora, que ele solta
# antes de soltar o nosso -- o shutdown do nosso sinalizava nela ja morta (acesso invalido no fechamento, medido no sample
# da AMD com a geracao dele ligada)
tr(c, '''        presentInfo.gameQueue       = queue;
''', '''        presentInfo.gameQueue       = queue;
        queue->AddRef(); // DLSS5 Overlay: solta no fim do shutdown
''')
tr(c, '''        DeleteCriticalSection(&presentInfo.criticalSectionScheduledFrame);

        std::ignore = SafeRelease(presentInfo.device);''', '''        DeleteCriticalSection(&presentInfo.criticalSectionScheduledFrame);

        SafeRelease(presentInfo.gameQueue); // DLSS5 Overlay: segura desde a criacao
        std::ignore = SafeRelease(presentInfo.device);''')
print(f'ffx_multiquadro: {mudou} trechos aplicados')
