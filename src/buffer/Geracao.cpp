#include "Geracao.h"
#include <bit> // ffx_util.h usa std::popcount sem incluir
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace dlss5ov {

namespace {
// recursos compartilhados entre o fluxo optico e a interpolacao (os do provedor da AMD, sem o upscaler): o vetor e a
// deteccao de corte de cena do fluxo optico; profundidade e movimento dilatados e a profundidade anterior reconstruida,
// dois jogos (um por quadro em voo)
enum
{
    R_FLUXO,
    R_CORTE,
    R_PROF_DIL,
    R_MOV_DIL = R_PROF_DIL + 2,
    R_PROF_REC = R_MOV_DIL + 2,
    R_N = R_PROF_REC + 2
};

FfxResourceStates Estado(D3D12_RESOURCE_STATES s)
{
    if (s & D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        return FFX_RESOURCE_STATE_UNORDERED_ACCESS;
    if (s & D3D12_RESOURCE_STATE_RENDER_TARGET)
        return FFX_RESOURCE_STATE_RENDER_TARGET;
    if (s & (D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ))
        return FFX_RESOURCE_STATE_DEPTH_ATTACHEMENT;
    if (s == D3D12_RESOURCE_STATE_COPY_DEST)
        return FFX_RESOURCE_STATE_COPY_DEST;
    if (s == D3D12_RESOURCE_STATE_COPY_SOURCE)
        return FFX_RESOURCE_STATE_COPY_SRC;
    const bool px = (s & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0,
               cp = (s & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0;
    if (px && cp)
        return FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    if (px)
        return FFX_RESOURCE_STATE_PIXEL_READ;
    if (cp)
        return FFX_RESOURCE_STATE_COMPUTE_READ;
    if (s == D3D12_RESOURCE_STATE_GENERIC_READ)
        return FFX_RESOURCE_STATE_GENERIC_READ;
    return FFX_RESOURCE_STATE_COMMON;
}
} // namespace

struct Geracao::Impl
{
    ID3D12Device* dev = nullptr;
    IDXGISwapChain4* sc = nullptr;
    FfxSwapchain ffxSc = nullptr;
    char erro[256] = {};

    // contexto (criado em segundo plano: a compilacao dos shaders da AMD leva 10-25 s e congelaria o jogo no primeiro
    // quadro; enquanto isso o swapchain apresenta so os quadros do jogo). Refeito quando a tela, o render maximo ou as
    // flags mudam.
    std::thread criador;
    std::atomic<int> estado{0}; // 0 nada, 1 criando, 2 pronto, 3 falhou
    double msCompilacao = 0;
    bool contexto = false;
    UINT telaW = 0, telaH = 0, maxRW = 0, maxRH = 0;
    DXGI_FORMAT fmtTela = DXGI_FORMAT_UNKNOWN;
    uint32_t flagsFi = 0;
    FfxInterface shared{}, fi{};
    void* rascunhoShared = nullptr;
    void* rascunhoFi = nullptr;
    FfxUInt32 idShared = 0;
    FfxOpticalflowContext of{};
    FfxFrameInterpolationContext fic{};
    FfxResourceInternal rec[R_N] = {};
    bool criado[R_N] = {};

    uint32_t vez = 0;      // qual jogo de recursos dilatados e o deste quadro
    uint64_t quadro = 0;   // frameID: sobe exatamente 1 por quadro (outro passo reinicia a geracao da AMD)
    FfxFrameInterpolationPrepareDescription prep[2] = {};
    uint64_t gerados = 0;

    void Erro(const char* f, int codigo)
    {
        snprintf(erro, sizeof erro, "%s (0x%x)", f, codigo);
    }
    FfxResource Rec(int i) { return shared.fpGetResource(&shared, rec[i]); }
    void Destroi();
    bool Cria();
    // cria (ou refaz) o contexto em segundo plano para esta tela, render maximo e flags
    void CriaAoFundo(UINT w, UINT h, DXGI_FORMAT f, UINT mrw, UINT mrh, uint32_t flags)
    {
        if (criador.joinable())
            criador.join();
        telaW = w, telaH = h, fmtTela = f, maxRW = mrw, maxRH = mrh, flagsFi = flags;
        estado = 1;
        criador = std::thread([this] {
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = Cria();
            msCompilacao = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            estado = ok ? 2 : 3;
        });
    }
    static FfxErrorCode Gera(const FfxFrameGenerationDispatchDescription* d, void* ctx);

    // teste: um quadro gerado e o real ao lado (DLSS5_GERACAO_DUMP)
    void Despejo(const FfxFrameGenerationDispatchDescription* d);
    Microsoft::WRL::ComPtr<ID3D12Resource> despejoLeitura[2];
    uint64_t despejoQuadro = 0;
    UINT despejoW = 0, despejoH = 0, despejoPitch = 0;
    DXGI_FORMAT despejoFmt = DXGI_FORMAT_UNKNOWN;
};

void Geracao::Impl::Destroi()
{
    if (!contexto)
        return;
    if (ffxSc)
        ffxWaitForPresents(ffxSc); // nada da AMD em voo usando os recursos
    for (int i = 0; i < R_N; ++i)
        if (criado[i])
            shared.fpDestroyResource(&shared, rec[i], idShared), criado[i] = false;
    ffxFrameInterpolationContextDestroy(&fic);
    ffxOpticalflowContextDestroy(&of);
    shared.fpDestroyBackendContext(&shared, idShared);
    free(rascunhoShared);
    free(rascunhoFi);
    rascunhoShared = rascunhoFi = nullptr;
    for (auto& r : rec)
        r = {};
    contexto = false;
}

bool Geracao::Impl::Cria()
{
    Destroi();
    // como o provedor da AMD: a interface compartilhada para 1 contexto, a da interpolacao para 2 (fluxo + interpolacao)
    const size_t t1 = ffxGetScratchMemorySizeDX12(1), t2 = ffxGetScratchMemorySizeDX12(2);
    rascunhoShared = calloc(t1, 1);
    rascunhoFi = calloc(t2, 1);
    FfxErrorCode e = ffxGetInterfaceDX12(&shared, ffxGetDeviceDX12(dev), rascunhoShared, t1, 1);
    if (e == FFX_OK)
        e = ffxGetInterfaceDX12(&fi, ffxGetDeviceDX12(dev), rascunhoFi, t2, 2);
    if (e != FFX_OK)
        return Erro("ffxGetInterfaceDX12", e), false;
    if ((e = shared.fpCreateBackendContext(&shared, FFX_EFFECT_SHAREDRESOURCES, nullptr, &idShared)) != FFX_OK)
        return Erro("contexto compartilhado", e), false;
    FfxOpticalflowContextDescription od{};
    od.backendInterface = fi;
    od.resolution = {telaW, telaH};
    if ((e = ffxOpticalflowContextCreate(&of, &od)) != FFX_OK)
        return Erro("ffxOpticalflowContextCreate", e), false;
    FfxFrameInterpolationContextDescription fd{};
    fd.backendInterface = fi;
    fd.flags = flagsFi;
    fd.maxRenderSize = {maxRW, maxRH};
    fd.displaySize = {telaW, telaH};
    fd.backBufferFormat = ffxGetSurfaceFormatDX12(fmtTela);
    fd.previousInterpolationSourceFormat = fd.backBufferFormat;
    if ((e = ffxFrameInterpolationContextCreate(&fic, &fd)) != FFX_OK)
        return Erro("ffxFrameInterpolationContextCreate", e), false;
    contexto = true; // a partir daqui Destroi solta o que foi criado
    FfxOpticalflowSharedResourceDescriptions ofr{};
    ffxOpticalflowGetSharedResourceDescriptions(&of, &ofr);
    if ((e = shared.fpCreateResource(&shared, &ofr.opticalFlowVector, idShared, &rec[R_FLUXO])) != FFX_OK)
        return Erro("recursos do fluxo optico", e), Destroi(), false;
    criado[R_FLUXO] = true;
    if ((e = shared.fpCreateResource(&shared, &ofr.opticalFlowSCD, idShared, &rec[R_CORTE])) != FFX_OK)
        return Erro("recursos do fluxo optico", e), Destroi(), false;
    criado[R_CORTE] = true;
    FfxFrameInterpolationSharedResourceDescriptions fr{};
    ffxFrameInterpolationGetSharedResourceDescriptions(&fic, &fr);
    for (int i = 0; i < 2; ++i)
    {
        wchar_t nome[3][96];
        FfxCreateResourceDescription d[3] = {fr.dilatedDepth, fr.dilatedMotionVectors, fr.reconstructedPrevNearestDepth};
        const int alvo[3] = {R_PROF_DIL + i, R_MOV_DIL + i, R_PROF_REC + i};
        for (int k = 0; k < 3; ++k)
        {
            swprintf(nome[k], 96, L"%s%d", d[k].name ? d[k].name : L"geracao", i);
            d[k].name = nome[k];
            if ((e = shared.fpCreateResource(&shared, &d[k], idShared, &rec[alvo[k]])) != FFX_OK)
                return Erro("recursos da interpolacao", e), Destroi(), false;
            criado[alvo[k]] = true;
        }
    }
    return true;
}

// chamada pelo swapchain da AMD na apresentacao (lista e quadro dele): fluxo optico do backbuffer e interpolacao, com
// a profundidade e o movimento preparados no quadro (como Dispatch do provedor da AMD)
FfxErrorCode Geracao::Impl::Gera(const FfxFrameGenerationDispatchDescription* d, void* ctx)
{
    Impl* p = static_cast<Impl*>(ctx);
    const FfxFrameInterpolationPrepareDescription& pd = p->prep[d->frameID & 1];
    FfxOpticalflowDispatchDescription o{};
    o.commandList = d->commandList;
    o.color = d->presentColor;
    o.reset = d->reset;
    o.backbufferTransferFunction = d->backBufferTransferFunction;
    o.minMaxLuminance = {d->minMaxLuminance[0], d->minMaxLuminance[1]};
    o.opticalFlowVector = p->Rec(R_FLUXO);
    o.opticalFlowSCD = p->Rec(R_CORTE);
    FfxErrorCode e = ffxOpticalflowContextDispatch(&p->of, &o);

    FfxFrameInterpolationDispatchDescription f{};
    f.commandList = d->commandList;
    f.displaySize = {d->presentColor.description.width, d->presentColor.description.height};
    f.currentBackBuffer = d->presentColor;
    f.reset = d->reset;
    f.renderSize = pd.renderSize;
    f.output = d->outputs[0];
    f.opticalFlowVector = p->Rec(R_FLUXO);
    f.opticalFlowSceneChangeDetection = p->Rec(R_CORTE);
    f.opticalFlowBlockSize = 8;
    f.opticalFlowScale = {1.f / f.displaySize.width, 1.f / f.displaySize.height};
    f.frameTimeDelta = pd.frameTimeDelta;
    f.cameraNear = pd.cameraNear;
    f.cameraFar = pd.cameraFar;
    f.viewSpaceToMetersFactor = pd.viewSpaceToMetersFactor;
    f.cameraFovAngleVertical = pd.cameraFovAngleVertical;
    f.dilatedDepth = p->Rec(R_PROF_DIL + p->vez);
    f.dilatedMotionVectors = p->Rec(R_MOV_DIL + p->vez);
    f.reconstructedPrevDepth = p->Rec(R_PROF_REC + p->vez);
    if (d->interpolationRect.width && d->interpolationRect.height)
        f.interpolationRect = d->interpolationRect;
    else
        f.interpolationRect = {0, 0, int32_t(f.displaySize.width), int32_t(f.displaySize.height)};
    f.backBufferTransferFunction = d->backBufferTransferFunction;
    f.minMaxLuminance[0] = d->minMaxLuminance[0];
    f.minMaxLuminance[1] = d->minMaxLuminance[1];
    f.frameID = d->frameID;
    e |= ffxFrameInterpolationDispatch(&p->fic, &f);
    ++p->gerados;
    p->Despejo(d);
    return e;
}

namespace {
D3D12_RESOURCE_STATES D3D(FfxResourceStates s)
{
    switch (s)
    {
    case FFX_RESOURCE_STATE_UNORDERED_ACCESS: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case FFX_RESOURCE_STATE_COMPUTE_READ: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case FFX_RESOURCE_STATE_PIXEL_READ: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ:
        return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case FFX_RESOURCE_STATE_COPY_SRC: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case FFX_RESOURCE_STATE_COPY_DEST: return D3D12_RESOURCE_STATE_COPY_DEST;
    case FFX_RESOURCE_STATE_RENDER_TARGET: return D3D12_RESOURCE_STATE_RENDER_TARGET;
    default: return D3D12_RESOURCE_STATE_COMMON; // PRESENT = COMMON no D3D12
    }
}
} // namespace

// teste (DLSS5_GERACAO_DUMP=<pasta>): no 60o quadro gerado copia ele e o backbuffer real deste quadro para leitura e
// grava os dois (RGBA no formato do backbuffer) uns quadros depois, quando a GPU ja terminou
void Geracao::Impl::Despejo(const FfxFrameGenerationDispatchDescription* d)
{
    static const char* pasta = std::getenv("DLSS5_GERACAO_DUMP");
    if (!pasta)
        return;
    if (despejoLeitura[0] && gerados == despejoQuadro + 8)
    {
        const char* nomes[2] = {"gerado", "real"};
        for (int k = 0; k < 2; ++k)
        {
            void* m = nullptr;
            if (FAILED(despejoLeitura[k]->Map(0, nullptr, &m)))
                continue;
            char arq[512];
            snprintf(arq, sizeof arq, "%s\\%s_%ux%u_pitch%u_fmt%d.raw", pasta, nomes[k], despejoW, despejoH, despejoPitch,
                     int(despejoFmt));
            if (FILE* f = fopen(arq, "wb"))
            {
                fwrite(m, 1, size_t(despejoPitch) * despejoH, f);
                fclose(f);
            }
            despejoLeitura[k]->Unmap(0, nullptr);
        }
        despejoLeitura[0] = despejoLeitura[1] = nullptr;
    }
    if (gerados != 60 || despejoQuadro)
        return;
    despejoQuadro = gerados;
    auto* c = reinterpret_cast<ID3D12GraphicsCommandList*>(d->commandList);
    const FfxResource* fontes[2] = {&d->outputs[0], &d->presentColor};
    for (int k = 0; k < 2; ++k)
    {
        auto* r = reinterpret_cast<ID3D12Resource*>(fontes[k]->resource);
        const D3D12_RESOURCE_DESC rd = r->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&despejoLeitura[k]))))
            return;
        const D3D12_RESOURCE_STATES antes = D3D(fontes[k]->state);
        D3D12_RESOURCE_BARRIER b{};
        b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, antes, D3D12_RESOURCE_STATE_COPY_SOURCE};
        if (antes != D3D12_RESOURCE_STATE_COPY_SOURCE)
            c->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{despejoLeitura[k].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src{r, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        c->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        if (antes != D3D12_RESOURCE_STATE_COPY_SOURCE)
            c->ResourceBarrier(1, &b);
        despejoW = UINT(rd.Width);
        despejoH = rd.Height;
        despejoPitch = fp.Footprint.RowPitch;
        despejoFmt = rd.Format;
    }
}

Geracao::Geracao() : p(new Impl) {}

Geracao::~Geracao()
{
    if (p->criador.joinable())
        p->criador.join();
    p->Destroi();
    delete p;
}



HRESULT Geracao::CriaSwapchain(IDXGIFactory* fabrica, ID3D12CommandQueue* fila, HWND janela, const DXGI_SWAP_CHAIN_DESC1* d,
                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* tela, IDXGISwapChain1** saida)
{
    FfxSwapchain s = nullptr;
    const FfxErrorCode e = ffxCreateFrameinterpolationSwapchainForHwndDX12(janela, d, tela, fila, fabrica, s);
    if (e != FFX_OK || !s)
        return p->Erro("ffxCreateFrameinterpolationSwapchainForHwndDX12", e), E_FAIL;
    p->ffxSc = s;
    p->sc = ffxGetDX12SwapchainPtr(s); // a referencia da criacao fica com o modulo
    fila->GetDevice(IID_PPV_ARGS(&p->dev));
    return p->sc->QueryInterface(IID_PPV_ARGS(saida));
}

HRESULT Geracao::CriaSwapchain(IDXGIFactory* fabrica, ID3D12CommandQueue* fila, DXGI_SWAP_CHAIN_DESC* d,
                               IDXGISwapChain** saida)
{
    FfxSwapchain s = nullptr;
    const FfxErrorCode e = ffxCreateFrameinterpolationSwapchainDX12(d, fila, fabrica, s);
    if (e != FFX_OK || !s)
        return p->Erro("ffxCreateFrameinterpolationSwapchainDX12", e), E_FAIL;
    p->ffxSc = s;
    p->sc = ffxGetDX12SwapchainPtr(s);
    fila->GetDevice(IID_PPV_ARGS(&p->dev));
    return p->sc->QueryInterface(IID_PPV_ARGS(saida));
}

bool Geracao::TemSwapchain() const
{
    return p->sc != nullptr;
}

bool Geracao::Quadro(ID3D12GraphicsCommandList* c, const QuadroGeracao& q, bool ligada)
{
    if (!p->sc || !c || !q.prof || !q.mov || !q.renderW || !q.renderH)
        return false;
    DXGI_SWAP_CHAIN_DESC1 sd{};
    p->sc->GetDesc1(&sd);
    const uint32_t flags = (q.movNaTela ? FFX_FRAMEINTERPOLATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS : 0) |
                           (q.movComJitter ? FFX_FRAMEINTERPOLATION_ENABLE_JITTER_MOTION_VECTORS : 0) |
                           (q.invertida ? FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED : 0) |
                           (q.infinita ? FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INFINITE : 0) |
                           (q.hdr || sd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? FFX_FRAMEINTERPOLATION_ENABLE_HDR_COLOR_INPUT : 0);
    const UINT mrw = std::max(q.maxRenderW ? q.maxRenderW : q.renderW, sd.Width),
               mrh = std::max(q.maxRenderH ? q.maxRenderH : q.renderH, sd.Height);
    if (p->estado == 1)
        return false; // compilando em segundo plano: o swapchain apresenta so os quadros do jogo
    if (p->estado != 2 || sd.Width != p->telaW || sd.Height != p->telaH || sd.Format != p->fmtTela ||
        flags != p->flagsFi || mrw > p->maxRW || mrh > p->maxRH)
    {
        if (p->estado == 3 && sd.Width == p->telaW && sd.Height == p->telaH && flags == p->flagsFi)
            return false; // ja falhou com isto (UltimoErro diz por que): nao tenta em todo quadro
        p->CriaAoFundo(sd.Width, sd.Height, sd.Format, mrw, mrh, flags);
        p->quadro = 0;
        return false;
    }
    // profundidade e movimento deste quadro, dilatados e com a profundidade anterior reconstruida (Prepare da AMD)
    p->vez ^= 1;
    ++p->quadro;
    FfxFrameInterpolationPrepareDescription& pd = p->prep[p->quadro & 1];
    pd = {};
    pd.commandList = ffxGetCommandListDX12(c);
    pd.renderSize = {q.renderW, q.renderH};
    pd.jitterOffset = {q.jitterX, q.jitterY};
    pd.motionVectorScale = {q.movEscX, q.movEscY};
    pd.frameTimeDelta = q.msQuadro;
    pd.cameraNear = q.perto;
    pd.cameraFar = q.longe;
    pd.viewSpaceToMetersFactor = 1.f;
    pd.cameraFovAngleVertical = q.fovY > 0.01f ? q.fovY : 1.0472f;
    pd.depth = ffxGetResourceDX12(q.prof, ffxGetResourceDescriptionDX12(q.prof), L"profundidade do jogo", Estado(q.profEstado));
    pd.motionVectors = ffxGetResourceDX12(q.mov, ffxGetResourceDescriptionDX12(q.mov), L"movimento do jogo", Estado(q.movEstado));
    pd.frameID = p->quadro;
    pd.dilatedDepth = p->Rec(R_PROF_DIL + p->vez);
    pd.dilatedMotionVectors = p->Rec(R_MOV_DIL + p->vez);
    pd.reconstructedPrevDepth = p->Rec(R_PROF_REC + p->vez);
    FfxErrorCode e = ffxFrameInterpolationPrepare(&p->fic, &pd);
    if (e != FFX_OK)
        return p->Erro("ffxFrameInterpolationPrepare", e), false;
    // o swapchain da AMD chama Gera na apresentacao deste quadro
    FfxFrameGenerationConfig cfg{};
    cfg.swapChain = p->ffxSc;
    cfg.frameGenerationCallback = &Impl::Gera;
    cfg.frameGenerationCallbackContext = p;
    cfg.frameGenerationEnabled = ligada;
    cfg.allowAsyncWorkloads = false;
    cfg.onlyPresentInterpolated = false;
    cfg.interpolationRect = {0, 0, int32_t(p->telaW), int32_t(p->telaH)};
    cfg.frameID = p->quadro;
    if ((e = p->shared.fpSwapChainConfigureFrameGeneration(&cfg)) != FFX_OK)
        return p->Erro("configuracao do swapchain", e), false;
    return true;
}

const char* Geracao::UltimoErro() const
{
    return p->erro;
}

uint64_t Geracao::Gerados() const
{
    return p->gerados;
}

int Geracao::EstadoContexto() const
{
    return p->estado;
}

double Geracao::MsCompilacao() const
{
    return p->msCompilacao;
}

} // namespace dlss5ov
