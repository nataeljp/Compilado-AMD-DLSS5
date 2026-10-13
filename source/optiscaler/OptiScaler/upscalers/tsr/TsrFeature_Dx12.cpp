#include <pch.h>
#include <Config.h>
#include "TsrFeature_Dx12.h"
#include "TsrShaders.h"

#include <dlssnr/amd/SystemCompiler.h>

#include <algorithm>
#include <cstring>

namespace
{
// constantes de raiz: a mesma ordem do cbuffer C de TsrShaders.h (tudo 4 bytes, sem preenchimento)
struct Constantes
{
    uint32_t renderW, renderH;
    uint32_t saidaW, saidaH;
    float jitterX, jitterY;
    float mvEscalaX, mvEscalaY;
    uint32_t mvW, mvH;
    float exposicao;
    uint32_t flags;
    float maxAmostras;
    float gammaRecorte;
    float paralaxe;
    float antiAlias;
    float jitterAntX, jitterAntY;
    float mvParaRenderX, mvParaRenderY;
    float cintilacao;
    float cintilacaoVelMax;
    float razaoQuadro; // tempo do quadro / 16,67 ms
};
static_assert(sizeof(Constantes) == 23 * 4);

DXGI_FORMAT FormatoSrv(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    default:
        return f;
    }
}

// UAV da saída do jogo: formatos _SRGB não aceitam escrita tipada; grava pela vista UNORM e o shader codifica
DXGI_FORMAT FormatoUav(DXGI_FORMAT f, bool& srgb)
{
    srgb = false;
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        srgb = true;
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        srgb = true;
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return FormatoSrv(f);
    }
}

template <class T> T* Recurso(NVSDK_NGX_Parameter* p, const char* nome)
{
    T* r = nullptr;
    if (p->Get(nome, &r) != NVSDK_NGX_Result_Success)
        p->Get(nome, (void**) &r);
    return r;
}

bool UnrealOuForcado()
{
    const auto& st = State::Instance();
    return st.NVNGX_Engine == NVSDK_NGX_ENGINE_TYPE_UNREAL || st.gameEngine == GameEngineType::Unreal ||
           (st.gameQuirks & GameQuirk::ForceUnrealEngine);
}
} // namespace

TsrFeatureDx12::TsrFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature_Dx12(InHandleId, InParameters), IFeature(InHandleId, InParameters)
{
    _initParameters = SetInitParameters(InParameters);
    _moduleLoaded = true;
}

TsrFeatureDx12::~TsrFeatureDx12() = default;

bool TsrFeatureDx12::CriaPipelines()
{
    // raiz: tabela (t0..t7, u0..u3) + 20 constantes de 32 bits em b0 + amostradores fixos s0 linear / s1 ponto
    D3D12_DESCRIPTOR_RANGE faixas[2] {};
    faixas[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSrv, 0, 0, 0 };
    faixas[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kUav, 0, 0, kSrv };
    D3D12_ROOT_PARAMETER params[2] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = { 2, faixas };
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = { 0, 0, sizeof(Constantes) / 4 };
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC amostras[2] {};
    for (UINT i = 0; i < 2; i++)
    {
        amostras[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        amostras[i].AddressU = amostras[i].AddressV = amostras[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        amostras[i].MaxLOD = D3D12_FLOAT32_MAX;
        amostras[i].ShaderRegister = i;
        amostras[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC rd {};
    rd.NumParameters = 2;
    rd.pParameters = params;
    rd.NumStaticSamplers = 2;
    rd.pStaticSamplers = amostras;
    ComPtr<ID3DBlob> blob, erro;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &erro)) ||
        FAILED(Device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&_raiz))))
    {
        LOG_ERROR("[TSR] raiz: {}", erro ? (const char*) erro->GetBufferPointer() : "falhou");
        return false;
    }

    struct Pass
    {
        const char* entrada;
        const char* define;
        ComPtr<ID3D12PipelineState>* pso;
    } passes[] = { { "CSParalaxe", "TSR_PARALAXE", &_psoParalaxe },
                   { "CSAntiAlias", "TSR_ANTIALIAS", &_psoAntiAlias },
                   { "CSHistorico", "TSR_HISTORICO", &_psoHistorico } };
    for (const auto& p : passes)
    {
        const D3D_SHADER_MACRO defs[] = { { p.define, "1" }, { nullptr, nullptr } };
        ComPtr<ID3DBlob> codigo, erros;
        const HRESULT hr = DlssNr::SysCompiler::Compile(kTsrShader, std::strlen(kTsrShader), "TSR (lmxxf2)", defs,
                                                        nullptr, p.entrada, "cs_5_0",
                                                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &codigo, &erros);
        if (FAILED(hr) || !codigo)
        {
            LOG_ERROR("[TSR] shader {}: {}", p.entrada, erros ? (const char*) erros->GetBufferPointer() : "falhou");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = _raiz.Get();
        pd.CS = { codigo->GetBufferPointer(), codigo->GetBufferSize() };
        if (FAILED(Device->CreateComputePipelineState(&pd, IID_PPV_ARGS(p.pso->GetAddressOf()))))
        {
            LOG_ERROR("[TSR] pipeline {} falhou", p.entrada);
            return false;
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kAneis * kPasses * kBloco;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(Device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_heap))))
    {
        LOG_ERROR("[TSR] heap de descritores falhou");
        return false;
    }
    _heap->SetName(L"TSR lmxxf2");
    _incremento = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

bool TsrFeatureDx12::InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (IsInited())
        return true;
    if (Device == nullptr)
    {
        LOG_ERROR("[TSR] sem device");
        return false;
    }

    // saída: a do jogo, ou maior com Output Scaling (o OptiScaler reduz depois), como nos outros upscalers
    auto& cfg = *Config::Instance();
    if (cfg.OutputScalingEnabled.value_or_default() && (LowResMV() || RenderWidth() == DisplayWidth()))
    {
        const float mult = std::clamp(cfg.OutputScalingMultiplier.value_or_default(), 0.5f, 3.0f);
        _targetWidth = static_cast<unsigned int>(DisplayWidth() * mult);
        _targetHeight = static_cast<unsigned int>(DisplayHeight() * mult);
    }
    else
    {
        _targetWidth = DisplayWidth();
        _targetHeight = DisplayHeight();
    }

    ScopedSkipHeapCapture skipHeapCapture {};
    if (!CriaPipelines())
        return false;

    LOG_INFO("[TSR] pronto: render {}x{} -> saida {}x{}", RenderWidth(), RenderHeight(), TargetWidth(), TargetHeight());
    SetInit(true);
    return true;
}

TsrFeatureDx12::ComPtr<ID3D12Resource> TsrFeatureDx12::CriaTextura(UINT w, UINT h, DXGI_FORMAT formato,
                                                                   const wchar_t* nome)
{
    D3D12_HEAP_PROPERTIES hp {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = formato;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    if (FAILED(Device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                               nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    r->SetName(nome);
    return r;
}

bool TsrFeatureDx12::GaranteTexturas(UINT renderMaxW, UINT renderMaxH, UINT saidaW, UINT saidaH)
{
    if (_hist[0].res && _histW == saidaW && _histH == saidaH && _renW >= renderMaxW && _renH >= renderMaxH)
        return true;

    ScopedSkipHeapCapture skipHeapCapture {};
    _histW = saidaW;
    _histH = saidaH;
    _renW = std::max(_renW, renderMaxW);
    _renH = std::max(_renH, renderMaxH);
    for (int i = 0; i < 2; i++)
    {
        _hist[i] = { CriaTextura(saidaW, saidaH, DXGI_FORMAT_R16G16B16A16_FLOAT, L"TSR historico") };
        _meta[i] = { CriaTextura(saidaW, saidaH, DXGI_FORMAT_R16G16B16A16_FLOAT, L"TSR cintilacao") };
        _lin[i] = { CriaTextura(_renW, _renH, DXGI_FORMAT_R32_FLOAT, L"TSR profundidade linear") };
    }
    _vel = { CriaTextura(_renW, _renH, DXGI_FORMAT_R16G16B16A16_FLOAT, L"TSR movimento") };
    _aa = { CriaTextura(_renW, _renH, DXGI_FORMAT_R16G16B16A16_FLOAT, L"TSR anti-aliasing") };
    _primeiro = true;
    const bool ok = _hist[0].res && _hist[1].res && _meta[0].res && _meta[1].res && _lin[0].res && _lin[1].res &&
                    _vel.res && _aa.res;
    if (!ok)
        LOG_ERROR("[TSR] texturas {}x{} / {}x{} falharam", saidaW, saidaH, _renW, _renH);
    return ok;
}

void TsrFeatureDx12::Transiciona(ID3D12GraphicsCommandList* cmd, Textura& t, D3D12_RESOURCE_STATES novo)
{
    if (t.estado == novo)
        return;
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { t.res.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, t.estado, novo };
    cmd->ResourceBarrier(1, &b);
    t.estado = novo;
}

D3D12_CPU_DESCRIPTOR_HANDLE TsrFeatureDx12::Cpu(UINT bloco, UINT slot) const
{
    auto h = _heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(bloco * kBloco + slot) * _incremento;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE TsrFeatureDx12::Gpu(UINT bloco) const
{
    auto h = _heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(bloco * kBloco) * _incremento;
    return h;
}

void TsrFeatureDx12::Srv(UINT bloco, UINT slot, ID3D12Resource* r, DXGI_FORMAT forcado)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d {};
    d.Format = forcado != DXGI_FORMAT_UNKNOWN ? forcado : FormatoSrv(r->GetDesc().Format);
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    Device->CreateShaderResourceView(r, &d, Cpu(bloco, slot));
}

void TsrFeatureDx12::Uav(UINT bloco, UINT slot, ID3D12Resource* r, DXGI_FORMAT forcado)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC d {};
    d.Format = forcado != DXGI_FORMAT_UNKNOWN ? forcado : FormatoSrv(r->GetDesc().Format);
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    Device->CreateUnorderedAccessView(r, nullptr, &d, Cpu(bloco, kSrv + slot));
}

void TsrFeatureDx12::Nulos(UINT bloco)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC s {};
    s.Format = DXGI_FORMAT_R32_FLOAT;
    s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    s.Texture2D.MipLevels = 1;
    for (UINT i = 0; i < kSrv; i++)
        Device->CreateShaderResourceView(nullptr, &s, Cpu(bloco, i));
    D3D12_UNORDERED_ACCESS_VIEW_DESC u {};
    u.Format = DXGI_FORMAT_R32_FLOAT;
    u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    for (UINT i = 0; i < kUav; i++)
        Device->CreateUnorderedAccessView(nullptr, nullptr, &u, Cpu(bloco, kSrv + i));
}

bool TsrFeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    auto& cfg = *Config::Instance();

    // ---- entradas do jogo (mesmas barreiras configuráveis dos outros upscalers)
    ID3D12Resource* cor = Recurso<ID3D12Resource>(InParameters, NVSDK_NGX_Parameter_Color);
    ID3D12Resource* mv = Recurso<ID3D12Resource>(InParameters, NVSDK_NGX_Parameter_MotionVectors);
    ID3D12Resource* prof = Recurso<ID3D12Resource>(InParameters, NVSDK_NGX_Parameter_Depth);
    ID3D12Resource* saida = Recurso<ID3D12Resource>(InParameters, NVSDK_NGX_Parameter_Output);
    ID3D12Resource* expo = AutoExposure() ? nullptr : Recurso<ID3D12Resource>(InParameters, NVSDK_NGX_Parameter_ExposureTexture);
    if (!cor || !mv || !prof || !saida)
    {
        LOG_ERROR("[TSR] falta entrada: cor {} mv {} profundidade {} saida {}", (void*) cor, (void*) mv, (void*) prof,
                  (void*) saida);
        return false;
    }
    _hasColor = _hasMV = _hasDepth = _hasOutput = true;
    _hasExposure = expo != nullptr;

    // (AMDNR C3-A) a cor da rede DLSS5 chega em NON_PIXEL_SHADER_RESOURCE e sai nele: sem barreira de cor do jogo
    const bool corDaRede = IsNrColour(InParameters, cor);
    if (!cfg.ColorResourceBarrier.has_value() && UnrealOuForcado())
        cfg.ColorResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!cfg.MVResourceBarrier.has_value() && UnrealOuForcado())
        cfg.MVResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const auto npsr = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (!corDaRede && cfg.ColorResourceBarrier.has_value())
        ResourceBarrier(InCommandList, cor, (D3D12_RESOURCE_STATES) cfg.ColorResourceBarrier.value(), npsr);
    if (cfg.MVResourceBarrier.has_value())
        ResourceBarrier(InCommandList, mv, (D3D12_RESOURCE_STATES) cfg.MVResourceBarrier.value(), npsr);
    if (cfg.DepthResourceBarrier.has_value())
        ResourceBarrier(InCommandList, prof, (D3D12_RESOURCE_STATES) cfg.DepthResourceBarrier.value(), npsr);
    if (expo && cfg.ExposureResourceBarrier.has_value())
        ResourceBarrier(InCommandList, expo, (D3D12_RESOURCE_STATES) cfg.ExposureResourceBarrier.value(), npsr);
    const auto estadoSaida = TitleOutputState(saida);
    if (estadoSaida)
        ResourceBarrier(InCommandList, saida, *estadoSaida, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // ---- tamanhos, jitter, vetores
    unsigned int rw = 0, rh = 0;
    GetRenderResolution(InParameters, &rw, &rh);
    const auto sd = saida->GetDesc();
    const UINT ow = static_cast<UINT>(std::min<UINT64>(sd.Width, TargetWidth() ? TargetWidth() : sd.Width));
    const UINT oh = std::min<UINT>(sd.Height, TargetHeight() ? TargetHeight() : sd.Height);
    if (rw == 0 || rh == 0 || ow == 0 || oh == 0)
        return false;
    if (!GaranteTexturas(std::max(rw, RenderWidth()), std::max(rh, RenderHeight()), ow, oh))
        return false;

    float jx = 0.f, jy = 0.f, msx = 1.f, msy = 1.f;
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jx);
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jy);
    if (InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &msx) != NVSDK_NGX_Result_Success ||
        InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &msy) != NVSDK_NGX_Result_Success)
        msx = msy = 1.f;
    unsigned int resetNgx = 0;
    InParameters->Get(NVSDK_NGX_Parameter_Reset, &resetNgx);
    const bool mudouRender = rw != _ultimoRenderW || rh != _ultimoRenderH;
    const bool reset = resetNgx != 0 || _primeiro || mudouRender;
    _ultimoRenderW = rw;
    _ultimoRenderH = rh;

    const UINT mvW = LowResMV() ? rw : ow;
    const UINT mvH = LowResMV() ? rh : oh;
    bool srgb = false;
    const DXGI_FORMAT formatoSaida = FormatoUav(sd.Format, srgb);

    Constantes k {};
    k.renderW = rw;
    k.renderH = rh;
    k.saidaW = ow;
    k.saidaH = oh;
    k.jitterX = std::isfinite(jx) ? jx : 0.f;
    k.jitterY = std::isfinite(jy) ? jy : 0.f;
    k.mvEscalaX = msx;
    k.mvEscalaY = msy;
    k.mvW = mvW;
    k.mvH = mvH;
    k.exposicao = 1.f;
    k.flags = (DepthInverted() ? 1u : 0u) | (reset ? 2u : 0u) | (LowResMV() ? 4u : 0u) | (expo ? 8u : 0u) |
              (srgb ? 16u : 0u);
    k.maxAmostras = std::clamp(cfg.TsrMaxSamples.value_or_default(), 2.f, 64.f);
    k.gammaRecorte = std::clamp(cfg.TsrClipGamma.value_or_default(), 0.25f, 3.f);
    k.paralaxe = std::clamp(cfg.TsrParallax.value_or_default(), 0.1f, 4.f);
    k.antiAlias = std::clamp(cfg.TsrSpatialAA.value_or_default(), 0.f, 1.f);
    k.jitterAntX = _jitterAnterior[0];
    k.jitterAntY = _jitterAnterior[1];
    k.mvParaRenderX = static_cast<float>(rw) / static_cast<float>(mvW);
    k.mvParaRenderY = static_cast<float>(rh) / static_cast<float>(mvH);
    k.cintilacao = std::clamp(cfg.TsrFlicker.value_or_default(), 0.f, 1.f);
    k.cintilacaoVelMax = std::clamp(cfg.TsrFlickerMaxVel.value_or_default(), 0.5f, 32.f);
    // tempo deste quadro real (Evaluate roda uma vez por quadro renderizado; com FG os gerados nao contam)
    {
        LARGE_INTEGER agora {}, freq {};
        QueryPerformanceCounter(&agora);
        QueryPerformanceFrequency(&freq);
        double ms = _ultimoQpc != 0 ? double(agora.QuadPart - _ultimoQpc) * 1000.0 / double(freq.QuadPart) : 16.667;
        _ultimoQpc = agora.QuadPart;
        float dtJogo = 0.f;
        if (InParameters->Get(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, &dtJogo) == NVSDK_NGX_Result_Success &&
            std::isfinite(dtJogo) && dtJogo >= 1.f && dtJogo <= 250.f)
            ms = dtJogo;
        k.razaoQuadro = static_cast<float>(std::clamp(ms / 16.667, 0.1, 6.0));
    }

    const UINT anterior = _atual ^ 1u;
    const UINT base = _anel * kPasses;
    _anel = (_anel + 1) % kAneis;

    {
        ScopedSkipHeapCapture skipHeapCapture {};
        // passe 1: paralaxe
        Nulos(base + 0);
        Srv(base + 0, 0, prof);
        Srv(base + 0, 1, mv);
        Srv(base + 0, 2, _lin[anterior].res.Get());
        Uav(base + 0, 0, _vel.res.Get());
        Uav(base + 0, 1, _lin[_atual].res.Get());
        // passe 2: anti-aliasing espacial
        Nulos(base + 1);
        Srv(base + 1, 0, cor);
        if (expo)
            Srv(base + 1, 4, expo);
        Uav(base + 1, 0, _aa.res.Get());
        // passe 3: histórico
        Nulos(base + 2);
        Srv(base + 2, 0, cor);
        Srv(base + 2, 1, _aa.res.Get());
        Srv(base + 2, 2, _vel.res.Get());
        Srv(base + 2, 3, _hist[anterior].res.Get());
        if (expo)
            Srv(base + 2, 4, expo);
        Srv(base + 2, 5, _meta[anterior].res.Get());
        Uav(base + 2, 0, _hist[_atual].res.Get());
        Uav(base + 2, 1, saida, formatoSaida);
        Uav(base + 2, 2, _meta[_atual].res.Get());
    }

    ID3D12DescriptorHeap* heaps[] = { _heap.Get() };
    InCommandList->SetDescriptorHeaps(1, heaps);
    InCommandList->SetComputeRootSignature(_raiz.Get());
    InCommandList->SetComputeRoot32BitConstants(1, sizeof(Constantes) / 4, &k, 0);

    Transiciona(InCommandList, _lin[anterior], npsr);
    Transiciona(InCommandList, _lin[_atual], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transiciona(InCommandList, _vel, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transiciona(InCommandList, _aa, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    InCommandList->SetPipelineState(_psoParalaxe.Get());
    InCommandList->SetComputeRootDescriptorTable(0, Gpu(base + 0));
    InCommandList->Dispatch((rw + 7) / 8, (rh + 7) / 8, 1);

    InCommandList->SetPipelineState(_psoAntiAlias.Get());
    InCommandList->SetComputeRootDescriptorTable(0, Gpu(base + 1));
    InCommandList->Dispatch((rw + 7) / 8, (rh + 7) / 8, 1);

    Transiciona(InCommandList, _vel, npsr);
    Transiciona(InCommandList, _aa, npsr);
    Transiciona(InCommandList, _hist[anterior], npsr);
    Transiciona(InCommandList, _hist[_atual], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transiciona(InCommandList, _meta[anterior], npsr);
    Transiciona(InCommandList, _meta[_atual], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    InCommandList->SetPipelineState(_psoHistorico.Get());
    InCommandList->SetComputeRootDescriptorTable(0, Gpu(base + 2));
    InCommandList->Dispatch((ow + 7) / 8, (oh + 7) / 8, 1);

    // a saída é lida a seguir pelo resto do OptiScaler / jogo
    D3D12_RESOURCE_BARRIER uavB {};
    uavB.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavB.UAV.pResource = saida;
    InCommandList->ResourceBarrier(1, &uavB);

    // ---- devolve os estados do jogo
    if (!corDaRede && cfg.ColorResourceBarrier.has_value())
        ResourceBarrier(InCommandList, cor, npsr, (D3D12_RESOURCE_STATES) cfg.ColorResourceBarrier.value());
    if (cfg.MVResourceBarrier.has_value())
        ResourceBarrier(InCommandList, mv, npsr, (D3D12_RESOURCE_STATES) cfg.MVResourceBarrier.value());
    if (cfg.DepthResourceBarrier.has_value())
        ResourceBarrier(InCommandList, prof, npsr, (D3D12_RESOURCE_STATES) cfg.DepthResourceBarrier.value());
    if (expo && cfg.ExposureResourceBarrier.has_value())
        ResourceBarrier(InCommandList, expo, npsr, (D3D12_RESOURCE_STATES) cfg.ExposureResourceBarrier.value());
    if (estadoSaida)
        ResourceBarrier(InCommandList, saida, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, *estadoSaida);

    _jitterAnterior[0] = k.jitterX;
    _jitterAnterior[1] = k.jitterY;
    _atual = anterior;
    _primeiro = false;
    _frameCount++;
    return true;
}
