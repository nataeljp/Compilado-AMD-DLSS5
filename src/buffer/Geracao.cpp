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

// o swapchain da AMD com externo\ffx_multiquadro.py: o ritmo das apresentacoes (le e zera)
extern "C" void ffxDlss5RitmoDX12(double* mediaMs, double* desvioMs, double* maxMs, uint32_t* n);

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
    uint64_t chamadas = 0; // apresentacoes com geracao (cada uma gera n - 1)

    // a tela nunca recebe mais quadros por segundo que a taxa do monitor: o N em uso cai ate caber e so sobe com folga
    // (trocar entre 1 e 2 liga e desliga o swapchain da AMD: nada de ficar trocando)
    std::chrono::steady_clock::time_point tAnt{}, trocaDesde{};
    double msJogo = 0;
    uint32_t hz = 0, hzMax = 0, nEmUso = 0;
    ULONGLONG hzQuando = 0;
    std::wstring hzMonitor;
    DWORD hzW = 0, hzH = 0;
    void Mede();
    uint32_t NEmUso(uint32_t pedido);

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
    bool CopiaParaLeitura(ID3D12GraphicsCommandList* c, const FfxResource& fonte, int slot);
    Microsoft::WRL::ComPtr<ID3D12Resource> despejoLeitura[5]; // os gerados (ate 3), o real e o real anterior
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
// a profundidade e o movimento preparados no quadro (como Dispatch do provedor da AMD). Com n - 1 gerados, a interpolacao
// roda uma vez para cada, em t = k/n, e so a ultima guarda o quadro atual como o anterior do proximo par
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
    const uint32_t m = std::clamp<uint32_t>(d->numInterpolatedFrames, 1, 3);
    for (uint32_t k = 0; k < m; ++k)
    {
        f.output = d->outputs[k];
        f.interpolationT = float(k + 1) / float(m + 1);
        f.keepPreviousInterpolationSource = k + 1 < m;
        e |= ffxFrameInterpolationDispatch(&p->fic, &f);
        ++p->gerados;
    }
    ++p->chamadas;
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

// teste (DLSS5_GERACAO_DUMP=<pasta>): na 59a apresentacao com geracao copia o backbuffer real (o anterior); na 60a, os
// gerados (gerado1..3, em t = 1/n...) e o real deste quadro; grava tudo (RGBA no formato do backbuffer) uns quadros
// depois, quando a GPU ja terminou
bool Geracao::Impl::CopiaParaLeitura(ID3D12GraphicsCommandList* c, const FfxResource& fonte, int slot)
{
    auto* r = reinterpret_cast<ID3D12Resource*>(fonte.resource);
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
                                            IID_PPV_ARGS(&despejoLeitura[slot]))))
        return false;
    const D3D12_RESOURCE_STATES antes = D3D(fonte.state);
    D3D12_RESOURCE_BARRIER b{};
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, antes, D3D12_RESOURCE_STATE_COPY_SOURCE};
    if (antes != D3D12_RESOURCE_STATE_COPY_SOURCE)
        c->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{despejoLeitura[slot].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
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
    return true;
}

void Geracao::Impl::Despejo(const FfxFrameGenerationDispatchDescription* d)
{
    static const char* pasta = std::getenv("DLSS5_GERACAO_DUMP");
    if (!pasta)
        return;
    auto* c = reinterpret_cast<ID3D12GraphicsCommandList*>(d->commandList);
    if (despejoQuadro && chamadas == despejoQuadro + 8)
    {
        const char* nomes[5] = {"gerado1", "gerado2", "gerado3", "real", "anterior"};
        for (int k = 0; k < 5; ++k)
        {
            void* m = nullptr;
            if (!despejoLeitura[k] || FAILED(despejoLeitura[k]->Map(0, nullptr, &m)))
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
        for (auto& r : despejoLeitura)
            r = nullptr;
    }
    if (chamadas == 59)
        CopiaParaLeitura(c, d->presentColor, 4);
    if (chamadas != 60 || despejoQuadro)
        return;
    despejoQuadro = chamadas;
    const uint32_t m = std::clamp<uint32_t>(d->numInterpolatedFrames, 1, 3);
    for (uint32_t k = 0; k < m; ++k)
        if (!CopiaParaLeitura(c, d->outputs[k], int(k)))
            return;
    CopiaParaLeitura(c, d->presentColor, 3);
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

bool Geracao::SwapchainSolto() const
{
    if (!p->sc)
        return false;
    const ULONG r = p->sc->AddRef(); // a do modulo + as do jogo + esta
    p->sc->Release();
    return r <= 2;
}

void Geracao::SoltaSwapchain()
{
    if (p->criador.joinable())
        p->criador.join();
    p->Destroi(); // espera as apresentacoes da AMD
    if (p->sc)
        p->sc->Release();
    if (p->dev)
        p->dev->Release();
    p->sc = nullptr;
    p->ffxSc = nullptr;
    p->dev = nullptr;
    p->estado = 0;
    p->quadro = 0;
    p->nEmUso = 0;
}

// o ritmo do jogo (entre as chamadas de Quadro) e a taxa do monitor da janela (relida a cada 2 s; a maior que ele aceita,
// so quando o monitor ou a resolucao mudam)
void Geracao::Impl::Mede()
{
    const auto agora = std::chrono::steady_clock::now();
    if (tAnt.time_since_epoch().count())
    {
        const double ms = std::chrono::duration<double, std::milli>(agora - tAnt).count();
        if (ms > 0.5 && ms < 250.0)
            msJogo = msJogo > 0 ? msJogo * 0.9 + ms * 0.1 : ms;
    }
    tAnt = agora;
    const ULONGLONG t = GetTickCount64();
    if (hzQuando && t - hzQuando < 2000)
        return;
    hzQuando = t;
    HWND h = nullptr;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (!sc || FAILED(sc->GetHwnd(&h)) || !h || !GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi) ||
        !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        return;
    hz = dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 0;
    if (hzMonitor != mi.szDevice || hzW != dm.dmPelsWidth || hzH != dm.dmPelsHeight)
    {
        hzMonitor = mi.szDevice;
        hzW = dm.dmPelsWidth;
        hzH = dm.dmPelsHeight;
        hzMax = hz;
        DEVMODEW d{};
        d.dmSize = sizeof d;
        for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &d); ++i)
            if (d.dmPelsWidth == hzW && d.dmPelsHeight == hzH)
                hzMax = std::max<uint32_t>(hzMax, d.dmDisplayFrequency);
    }
}

uint32_t Geracao::Impl::NEmUso(uint32_t pedido)
{
    static const bool semLimite = std::getenv("DLSS5_GERACAO_SEM_LIMITE") != nullptr; // testes de ritmo
    if (pedido < 2)
    {
        nEmUso = 1;
        trocaDesde = {};
        return 1;
    }
    if (semLimite || !hz)
        return nEmUso = pedido;
    if (msJogo <= 0)
        return 1; // o primeiro quadro: sem gerados ate saber o ritmo do jogo
    const double fps = 1000.0 / msJogo;
    uint32_t novo = nEmUso ? std::min(nEmUso, pedido) : pedido;
    while (novo > 1 && fps * novo > hz * 1.05) // nao cabe: cai
        --novo;
    while (novo < pedido && fps * (novo + 1) <= hz * 0.97) // cabe com folga: sobe
        ++novo;
    if (!nEmUso || nEmUso > pedido) // o primeiro quadro, ou o pedido baixou no painel: ja
    {
        trocaDesde = {};
        return nEmUso = novo;
    }
    const auto agora = std::chrono::steady_clock::now();
    if (novo == nEmUso)
        trocaDesde = {};
    else if (!trocaDesde.time_since_epoch().count())
        trocaDesde = agora;
    else if (agora - trocaDesde >= std::chrono::milliseconds(novo < nEmUso ? 500 : 1500))
    {
        nEmUso = novo;
        trocaDesde = {};
    }
    return nEmUso;
}

bool Geracao::Quadro(ID3D12GraphicsCommandList* c, const QuadroGeracao& q, uint32_t n)
{
    if (!p->sc || !c || !q.prof || !q.mov || !q.renderW || !q.renderH)
        return false;
    p->Mede();
    n = p->NEmUso(std::min<uint32_t>(n, 4));
    const bool ligada = n >= 2;
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
    cfg.numInterpolatedFrames = ligada ? std::min<uint32_t>(n, 4) - 1 : 1;
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

uint32_t Geracao::EmUso() const
{
    return p->nEmUso ? p->nEmUso : 1;
}

uint32_t Geracao::Hz() const
{
    return p->hz;
}

uint32_t Geracao::HzMaximo() const
{
    return p->hzMax;
}

double Geracao::FpsJogo() const
{
    return p->msJogo > 0 ? 1000.0 / p->msJogo : 0.0;
}

uint32_t Geracao::Ritmo(double& mediaMs, double& desvioMs, double& maxMs) const
{
    uint32_t n = 0;
    ffxDlss5RitmoDX12(&mediaMs, &desvioMs, &maxMs, &n);
    return n;
}

} // namespace dlss5ov
