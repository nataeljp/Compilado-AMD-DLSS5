#include "Geracao.h"
#include <bit> // ffx_util.h usa std::popcount sem incluir
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

// o swapchain da AMD com externo\ffx_multiquadro.py: o ritmo das apresentacoes (le e zera) e o aviso de destruicao
extern "C" void ffxDlss5RitmoDX12(double* mediaMs, double* desvioMs, double* maxMs, uint32_t* n, double* p99Ms);
extern "C" FfxErrorCode ffxDlss5AoDestruirDX12(FfxSwapchain gameSwapChain, void (*aviso)(void*), void* ctx);
// a composicao de UI padrao do swapchain da AMD (FrameInterpolationSwapchainDX12_UiComposition.h, fora do include)
FfxErrorCode ffxFrameInterpolationUiComposition(const FfxPresentCallbackDescription* params, void* unusedUserCtx);
#pragma comment(lib, "d3dcompiler.lib")

namespace dlss5ov {

struct DECLSPEC_UUID("ADEC44E2-61F0-45C3-AD9F-1B37379284FF") StreamlineRetrieveBaseInterface : IUnknown
{
};

ID3D12CommandList* ListaNativa(ID3D12CommandList* l)
{
    IUnknown* base = nullptr;
    if (!l || FAILED(l->QueryInterface(__uuidof(StreamlineRetrieveBaseInterface), reinterpret_cast<void**>(&base))) || !base)
        return l;
    base->Release(); // a embrulhada segura a nativa
    return reinterpret_cast<ID3D12CommandList*>(base);
}

namespace {
// recursos compartilhados entre o fluxo optico e a interpolacao (os do provedor da AMD, sem o upscaler): o vetor e a
// deteccao de corte de cena do fluxo optico; profundidade e movimento dilatados e a profundidade anterior reconstruida,
// tres jogos (o jogo pode preparar ate 2 quadros a frente do que apresenta)
constexpr int kJogos = 3;
enum
{
    R_FLUXO,
    R_CORTE,
    R_PROF_DIL,
    R_MOV_DIL = R_PROF_DIL + kJogos,
    R_PROF_REC = R_MOV_DIL + kJogos,
    R_N = R_PROF_REC + kJogos
};

D3D12_RESOURCE_STATES D3D(FfxResourceStates s); // abaixo


bool SemLimite() // testes de ritmo: sem o limite pela taxa do monitor nem o limitador
{
    static const bool sem = std::getenv("DLSS5_GERACAO_SEM_LIMITE") != nullptr;
    return sem;
}

// as flags do contexto da interpolacao (SDK 1.1.4) nas da API da geracao (FfxApiCreateContextFramegenerationFlags)
uint32_t FlagsNeural(uint32_t fi)
{
    return ((fi & FFX_FRAMEINTERPOLATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) ? 1u << 1 : 0u) |
           ((fi & FFX_FRAMEINTERPOLATION_ENABLE_JITTER_MOTION_VECTORS) ? 1u << 2 : 0u) |
           ((fi & FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED) ? 1u << 3 : 0u) |
           ((fi & FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INFINITE) ? 1u << 4 : 0u) |
           ((fi & FFX_FRAMEINTERPOLATION_ENABLE_HDR_COLOR_INPUT) ? 1u << 5 : 0u);
}

// o formato de uma copia de recurso do jogo (CopyResource: o mesmo grupo sem tipo); UNKNOWN = nao copiavel aqui
// (profundidade com estencil: dois planos)
DXGI_FORMAT Copiavel(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_UNKNOWN;
    default: return f;
    }
}

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
    std::mutex mx; // Quadro (thread de render do jogo) x o aviso de destruicao (a thread que solta o swapchain)
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

    uint64_t quadro = 0;   // frameID: sobe exatamente 1 por quadro (outro passo reinicia a geracao da AMD)
    // Os quadros preparados (profundidade e movimento dilatados, no despacho do FSR) esperam a apresentacao deles, em
    // ordem: o motor pode preparar o quadro seguinte antes de apresentar este (thread de render a frente), e a AMD so
    // gera no Present -- casar pelo ultimo preparado juntava a imagem de um quadro com o movimento do seguinte (gerado no
    // lugar errado: fantasma e o movimento indo e voltando)
    struct Preparado
    {
        uint64_t id;
        FfxFrameInterpolationPrepareDescription pd;
        ID3D12CommandList* lista;  // a lista do jogo em que o preparo foi gravado
        uint64_t enviado;          // ordem de envio a GPU (0 = ainda nao)
        bool semHud;               // a imagem sem HUD deste quadro esta em semHud[id % kJogos]
        uint32_t passeHud;         // de qual passe do jogo ela veio
        // a neural: as copias da profundidade e do movimento (anelProf/anelMov[id % kJogos]) e a camera do jogo
        bool neural = false;
        bool temCam = false;
        QuadroNeural qn{};
    };
    // a UI do jogo vale de fato: textura ou callback, ou a imagem sem HUD com a copia dela funcionando (usada nas ultimas
    // 30 apresentacoes) -- sem a copia a procura da imagem sem HUD volta, em vez de o HUD ser interpolado com a cena
    bool UiAtiva() const { return uiEmUso && (!uiSoSemHud || chamadas - chamadaUltimaCopia < 30); }
    // escolhe na fila o preparado deste Present (Gera, com filaMx): com os envios, o mais novo ja enviado a GPU; sem, o
    // mais antigo gravado
    size_t EscolhePreparado() const
    {
        size_t k = fila.size();
        if (comEnvio)
        {
            for (size_t i = 0; i < fila.size(); ++i)
                if (fila[i].enviado && (k == fila.size() || fila[i].id > fila[k].id))
                    k = i;
        }
        else if (!fila.empty())
            k = 0;
        return k;
    }
    uint64_t ordemEnvio = 0;
    bool comEnvio = false; // o gancho de ExecuteCommandLists informa os envios
    std::mutex filaMx; // so a fila (nunca segurado em chamada da AMD: o Present dela segura as travas dela e chama Gera)
    // O backend da AMD (fi) e um so para o fluxo optico e a interpolacao, e nao e seguro entre threads: a fila de trabalhos
    // e o anel de descritores dele sao compartilhados. O Prepare roda na thread de render do jogo (Quadro) e o fluxo e a
    // interpolacao no Present (Gera): no TLOU2 sao threads diferentes, e os dois juntos gravavam trabalho de um na lista
    // do outro e sobrescreviam descritores em uso (falha de pagina na GPU: DEVICE_HUNG). Ordem: mx -> fiMx; o Present da
    // AMD (com as travas dela) -> fiMx; nada chama a AMD segurando fiMx
    std::mutex fiMx;
    std::atomic<DWORD> threadPreparo{0};
    bool threadsDitas = false; // o rastro diz uma vez as threads do preparo e da geracao
    // a UI do jogo (UiJogo): o callback dele e chamado pela thread de apresentacao da AMD em cada quadro
    std::mutex uiMx;
    void* uiCb = nullptr;
    void* uiCbCtx = nullptr;
    struct
    {
        uint64_t nosso, jogo;
    } uiIds[8] = {};                   // o frameID do jogo de cada quadro nosso (o callback recebe o do jogo)
    bool uiTexturaNossa = false;       // a textura de UI do jogo esta registrada no nosso swapchain
    std::atomic<bool> uiEmUso{false};
    // a imagem sem HUD do jogo (HUDLessColor, a da ultima configuracao) e a copia dela feita no Present dele, na fila
    // dele (AntesDoPresent)
    ID3D12Resource* uiSemHud = nullptr;
    uint32_t uiSemHudEstado = 0;
    bool uiSoSemHud = false; // a UI do jogo e so a imagem sem HUD: sem a copia, a nossa procura volta a valer
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> filaJogo;
    // por que o Present nao copiou (log de 5 s): chamadas, sem a imagem, fora do tamanho da tela, sem a fila do jogo
    uint64_t antesChamadas = 0, antesSemImagem = 0, antesTamanho = 0, antesSemFila = 0;
    UINT tamW = 0, tamH = 0;
    DXGI_FORMAT tamFmt = DXGI_FORMAT_UNKNOWN;
    uint64_t chamadaUltimaCopia = 0; // a apresentacao (chamadas) da ultima copia usada
    static constexpr int kCopias = 4;
    Microsoft::WRL::ComPtr<ID3D12Resource> copiaSemHud[kCopias];
    D3D12_RESOURCE_STATES copiaEstado[kCopias] = {};
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> copiaAl[kCopias];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> copiaLista;
    Microsoft::WRL::ComPtr<ID3D12Fence> copiaCerca;
    uint64_t copiaValor[kCopias] = {}, copiaN = 0, copiaUltimo = 0;
    std::atomic<int> copiaPronta{-1}; // a copia do Present em curso (Gera, chamada dentro dele, a consome)
    uint64_t copiasFeitas = 0, copiasUsadas = 0;
    static FfxErrorCode UiCallback(const FfxPresentCallbackDescription* d, void* ctx);
    std::atomic<uint64_t> encontros{0}; // vezes em que um esperou o outro em fiMx (sem a trava, os dois juntos no backend)
    std::deque<Preparado> fila;
    uint64_t aFrenteSoma = 0, aFrenteN = 0, aFrenteMax = 0, semPreparo = 0;
    // diagnostico (Rastro): as primeiras apresentacoes com geracao
    std::string rastro;
    uint32_t rastroLinhas = 0;
    uint64_t despachosRastro = 0; // despachos do FSR (Quadro) desde a ultima apresentacao
    std::chrono::steady_clock::time_point rastroAnt{};
    uint64_t gerados = 0;
    uint64_t chamadas = 0; // apresentacoes com geracao (cada uma gera n - 1)
    // a geracao neural (GeracaoNeural.h): a API do loader, os contextos, por quadro preparado as copias da profundidade e
    // do movimento (o jogo os reescreve antes do Present, onde a neural roda) e as cameras do preparo da geracao do jogo
    // ainda sem quadro (com filaMx)
    ApiAmd api;
    GeracaoNeural neural;
Reprojecao repro;
float limiteRepro = -1.f; // pixels de movimento entre os reais em 1080p acima do qual entra a reprojecao; <0 = desligada
                          // (padrao: no TLOU2 nao mudou o giro rapido; DLSS5_GERACAO_LIMITE liga)
    char erroNeural[160] = {};
    Microsoft::WRL::ComPtr<ID3D12Resource> anelProf[kJogos], anelMov[kJogos];
    std::deque<CameraJogo> cameras;
    CameraJogo camAnterior{};
    bool temCamAnterior = false;
    uint64_t quadrosNeural = 0, quadrosAnalitica = 0, ultimoAnalitica = 0;
    // o ultimo quadro real apresentado e as apresentacoes sem gerado por salto (o real anterior nao era o quadro do jogo
    // logo antes deste: o movimento do jogo cobre so 1 quadro e a geracao interpolava por cima de varios -- no TLOU2 a
    // 83 q/s em 4x so 1 de cada ~3 reais chegava a tela, e os gerados saiam com fantasma e o fundo duplicado)
    uint64_t idApresentado = 0, saltos = 0;
    // a luminancia do monitor (nits), para a neural quando o jogo nao passa metadados HDR ao swapchain (a analitica ignora o
    // 0..0 no scRGB; a neural saia 1,5% mais escura no jogo de teste)
    float lumMinTela = 0.f, lumMaxTela = 0.f;
    bool CopiaNeural(ID3D12GraphicsCommandList* c, const QuadroGeracao& q, int jogo, QuadroNeural& qn);
    // a imagem sem HUD de cada quadro preparado (um por jogo de recursos), no formato do backbuffer
    Microsoft::WRL::ComPtr<ID3D12Resource> semHud[kJogos];
    D3D12_RESOURCE_STATES semHudEstado[kJogos] = {};
    ID3D12Resource* semHudApresentado = nullptr; // a da ultima apresentacao (teste: despejo)
    // Conferencia (na apresentacao, antes da AMD), em 1 de cada 16 pixels: quantos da copia sao iguais, bit a bit, ao
    // quadro final, e -- contra a conferencia anterior do mesmo passe -- quantos trocaram entre igual e diferente (a
    // mascara do que difere) e quantos do quadro final mudaram (a cena andou). A certa so difere no HUD, e o HUD fica
    // parado: a mascara nao muda com a cena andando. A de outro passe difere na tela toda (antes de um pos-efeito como
    // grao ou cor) ou nas bordas da cena (antes de uma nitidez), e essas andam com a cena
    static constexpr int kConf = 8; // vagas: o resultado de cada uma e lido quando a marca dela volta da GPU
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rsConf;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> psoConf;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heapConf;              // 2 SRV por vaga
    Microsoft::WRL::ComPtr<ID3D12Resource> contaConf, zeroConf, leConf; // por vaga: iguais, marca, trocas, mudancas, base
    Microsoft::WRL::ComPtr<ID3D12Resource> histConf; // por passe e pixel conferido: difere do final? e o hash do final
    UINT64 histTam = 0;
    D3D12_RESOURCE_STATES contaEstado = D3D12_RESOURCE_STATE_COPY_DEST;
    uint32_t* zeroMapa = nullptr;
    UINT incConf = 0;
    struct Conferencia
    {
        uint32_t marca, passe, total;
    } conf[kConf] = {};
    uint32_t marcaConf = 0;
    bool CriaConferencia();
    void Confere(ID3D12GraphicsCommandList* c, ID3D12Resource* copia, const FfxResource& final_, uint32_t passe);
    void LeConferencias();
    // a escolha do passe: procura (do primeiro ao ultimo, fica o primeiro que confere), em uso, ou nenhum confere
    static constexpr double kConfere = 0.7;    // a certa e igual ao quadro final fora do HUD (que cobre menos de 30%)
    static constexpr double kMovimento = 0.05; // a cena andou: 5% dos pixels do quadro final mudaram
    static constexpr double kInstavel = 0.003; // a mascara andou com a cena: 0,3% da tela trocou
    std::atomic<uint32_t> passeHud{0}, passesHud{1};
    std::atomic<int> faseHud{0}; // 0 procurando, 1 em uso, 2 nenhum confere (espera)
    std::atomic<bool> conferidaHud{false}; // em uso ja conferida com a cena andando
    std::atomic<double> igualHud{0.0}, trocaHud{0.0};
    struct Medida
    {
        uint32_t n, nMov;
        double igual, troca;
    } medida[8] = {};
    uint32_t falhasHud = 0, faltasHud = 0, instaveisHud = 0, pausaHud = 0;
    void Resultado(uint32_t passe, double igual, double troca, bool movimento);
    void Falta();
    void Procura(uint32_t desde = 0);

    // a tela nunca recebe mais quadros por segundo que a taxa do monitor: o N em uso cai ate caber e so sobe com folga
    // (trocar entre 1 e 2 liga e desliga o swapchain da AMD: nada de ficar trocando)
    std::chrono::steady_clock::time_point tAnt{}, trocaDesde{};
    // prova de um N a menos (NEmUso): de qual N, desde quando, e quando pode a proxima (a espera dobra a cada falha)
    uint32_t provaDe = 0;
    int provaEspera = 10;
    std::chrono::steady_clock::time_point provaDesde{}, provaProxima{};
    double msJogo = 0, msAtual = 0;
    std::atomic<uint32_t> trocasN{0};
    std::atomic<uint32_t> hz{0}, hzMax{0}; // DefineTaxa (outra thread)
    uint32_t nEmUso = 0;
    // o limitador: com N gerados o jogo nao passa de 97% da taxa / N (dentro da faixa do FreeSync); a espera dele sai da
    // medida do ritmo natural do jogo
    double msSono = 0;
    HANDLE relogio = nullptr;
    void Limita();
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
    static void AoDestruir(void* ctx);
    void Adota(FfxSwapchain s, ID3D12CommandQueue* fila);

    // teste: um quadro gerado e o real ao lado (DLSS5_GERACAO_DUMP)
    void Despejo(const FfxFrameGenerationDispatchDescription* d, uint64_t id);
    bool CopiaParaLeitura(ID3D12GraphicsCommandList* c, const FfxResource& fonte, int slot);
    // os gerados (ate 3), o real, o real anterior e a imagem sem HUD do real; e, como foram para a tela (com a UI
    // composta pelo swapchain, UiCallback), os gerados e o real do mesmo quadro
    Microsoft::WRL::ComPtr<ID3D12Resource> despejoLeitura[10];
    uint64_t despejoQuadro = 0;
    std::atomic<uint64_t> despejoId{0}; // o quadro (frameID nosso) despejado
    int despejoTelaK = 0;               // gerados dele ja copiados na composicao
    // para onde e em qual apresentacao (chamadas): DLSS5_GERACAO_DUMP(_QUADRO) no lancamento ou pedido em DespejaEm
    std::mutex despejoMx;
    std::string despejoPedido, despejoPasta;
    uint64_t despejoAlvo = 0;
    std::atomic<bool> despejoAtivo{false};
    UINT despejoW = 0, despejoH = 0, despejoPitch = 0;
    DXGI_FORMAT despejoFmt = DXGI_FORMAT_UNKNOWN;
};

void Geracao::Impl::Destroi()
{
    if (!contexto)
        return;
    if (ffxSc)
    {
        // a geracao desligada antes: a configuracao espera o Present em curso (Gera gravado, a lista ainda nao enviada) e
        // a AMD para de chamar Gera; so entao a espera das apresentacoes cobre tudo o que usa estes recursos
        FfxFrameGenerationConfig cfg{};
        cfg.swapChain = ffxSc;
        cfg.frameGenerationEnabled = false;
        cfg.numInterpolatedFrames = 1;
        shared.fpSwapChainConfigureFrameGeneration(&cfg);
        ffxWaitForPresents(ffxSc); // nada da AMD em voo usando os recursos
    }
    std::lock_guard<std::mutex> lf(fiMx);
    neural.Destroi();
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
    for (auto& r : semHud) // tela ou formato novos (as apresentacoes em voo ja terminaram: Destroi)
        r = nullptr;
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
    for (int i = 0; i < kJogos; ++i)
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
    if (!CriaConferencia()) // sem ela nenhuma imagem sem HUD vai para a AMD (nao conferida, poderia ser a errada)
        faseHud = 2;
    // a luminancia do monitor da janela (aqui, fora da thread de render: consultar o monitor disputa trava com o DirectX)
    if (sc)
    {
        Microsoft::WRL::ComPtr<IDXGIOutput> saida;
        Microsoft::WRL::ComPtr<IDXGIOutput6> saida6;
        DXGI_OUTPUT_DESC1 od{};
        if (SUCCEEDED(sc->GetContainingOutput(&saida)) && SUCCEEDED(saida.As(&saida6)) && SUCCEEDED(saida6->GetDesc1(&od)))
            lumMinTela = od.MinLuminance, lumMaxTela = od.MaxLuminance;
    }
    // a neural no provedor 4.x do loader do jogo (sem ele, ou fora do RDNA 4, so a analitica)
    static const bool soAnalitica = std::getenv("DLSS5_GERACAO_ANALITICA") != nullptr;
    erroNeural[0] = 0;
    if (soAnalitica)
        snprintf(erroNeural, sizeof erroNeural, "desligada (DLSS5_GERACAO_ANALITICA)");
    else if (!neural.Cria(api, dev, telaW, telaH, maxRW, maxRH, fmtTela, FlagsNeural(flagsFi)))
        snprintf(erroNeural, sizeof erroNeural, "%s", neural.Erro());
    // a reprojecao acima do limite de movimento (DLSS5_GERACAO_LIMITE = pixels em 1080p; 0 = so ela; negativo desliga)
    if (const char* l = std::getenv("DLSS5_GERACAO_LIMITE"))
        limiteRepro = float(atof(l));
    if (limiteRepro >= 0.f && !repro.Pronta())
        repro.Cria(dev);
    return true;
}

// a profundidade e o movimento deste quadro copiados para o anel da neural (na lista do jogo, logo depois do despacho
// do FSR, quando ainda sao os deste quadro), e os parametros dele
bool Geracao::Impl::CopiaNeural(ID3D12GraphicsCommandList* c, const QuadroGeracao& q, int jogo, QuadroNeural& qn)
{
    auto garante = [&](Microsoft::WRL::ComPtr<ID3D12Resource>& r, ID3D12Resource* fonte) -> bool {
        const D3D12_RESOURCE_DESC f = fonte->GetDesc();
        const DXGI_FORMAT fmt = Copiavel(f.Format);
        if (fmt == DXGI_FORMAT_UNKNOWN || f.SampleDesc.Count != 1)
            return false;
        if (r)
        {
            const D3D12_RESOURCE_DESC d = r->GetDesc();
            if (d.Width == f.Width && d.Height == f.Height && d.Format == fmt && d.MipLevels == f.MipLevels)
                return true;
        }
        D3D12_RESOURCE_DESC d = f;
        d.Format = fmt;
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        d.Alignment = 0;
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        r = nullptr;
        return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                      IID_PPV_ARGS(&r)));
    };
    if (!garante(anelProf[jogo], q.prof) || !garante(anelMov[jogo], q.mov))
        return false;
    auto copia = [&](ID3D12Resource* de, D3D12_RESOURCE_STATES estado, ID3D12Resource* para) {
        D3D12_RESOURCE_BARRIER b[2] = {};
        b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[0].Transition = {de, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, estado, D3D12_RESOURCE_STATE_COPY_SOURCE};
        b[1].Transition = {para, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_COPY_DEST};
        const bool transiciona = estado != D3D12_RESOURCE_STATE_COPY_SOURCE;
        c->ResourceBarrier(transiciona ? 2 : 1, transiciona ? b : b + 1);
        c->CopyResource(para, de);
        std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
        std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
        c->ResourceBarrier(transiciona ? 2 : 1, transiciona ? b : b + 1);
    };
    copia(q.prof, q.profEstado, anelProf[jogo].Get());
    copia(q.mov, q.movEstado, anelMov[jogo].Get());
    qn.prof = anelProf[jogo].Get();
    qn.mov = anelMov[jogo].Get();
    qn.renderW = q.renderW, qn.renderH = q.renderH;
    qn.jitterX = q.jitterX, qn.jitterY = q.jitterY, qn.movEscX = q.movEscX, qn.movEscY = q.movEscY;
    qn.msQuadro = q.msQuadro;
    qn.perto = q.perto > 0.f ? q.perto : 0.1f;
    qn.longe = q.longe > qn.perto ? q.longe : 1e5f;
    qn.fovY = q.fovY > 0.01f ? q.fovY : 1.0472f;
    return true;
}

// a conferencia da imagem sem HUD: um compute que, em 1 de cada 16 pixels, conta os iguais (rgb, bit a bit no formato
// da tela) entre a copia e o quadro final e, contra o historico do passe (a conferencia anterior dele), os que trocaram
// entre igual e diferente e os do quadro final que mudaram
bool Geracao::Impl::CriaConferencia()
{
    // o historico: 8 passes x os pixels conferidos desta tela (zerado na criacao = sem historico)
    const UINT64 hist = 8ull * ((telaW + 2) / 4) * ((telaH + 2) / 4) * 4;
    if (psoConf && histTam >= hist)
        return true;
    auto buffer = [&](D3D12_HEAP_TYPE tipo, UINT64 tam, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES est,
                      Microsoft::WRL::ComPtr<ID3D12Resource>& r) {
        D3D12_HEAP_PROPERTIES hp{tipo};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = tam;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = fl;
        r = nullptr;
        return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, est, nullptr, IID_PPV_ARGS(&r)));
    };
    if (histTam < hist)
    {
        histTam = 0;
        if (!buffer(D3D12_HEAP_TYPE_DEFAULT, hist, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, histConf))
            return psoConf = nullptr, false;
        histConf->SetName(L"geracao: historico da conferencia da imagem sem HUD");
        histTam = hist;
    }
    if (psoConf)
        return true;
    static const char kHlsl[] = R"(
Texture2D<float4> copia : register(t0);
Texture2D<float4> final_ : register(t1);
RWByteAddressBuffer conta : register(u0); // por vaga (32 bytes): iguais, marca, trocas, mudancas, com historico
RWByteAddressBuffer hist : register(u1);  // por passe e pixel conferido: bit 31 tem historico, bits 1-30 hash do final, bit 0 difere
cbuffer k : register(b0) { uint w, h, vaga, passe, aw, ah, pad0, pad1; };
groupshared uint gIg, gTroca, gMuda, gBase;
[numthreads(8, 8, 1)]
void CSConfere(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    if (gi == 0)
    {
        gIg = 0;
        gTroca = 0;
        gMuda = 0;
        gBase = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    const uint2 p = id.xy * 4 + 1;
    if (id.x < aw && id.y < ah && p.x < w && p.y < h)
    {
        const float3 b = final_[p].rgb;
        const uint dif = any(copia[p].rgb != b) ? 1u : 0u;
        const uint3 u = asuint(b);
        const uint hh = (u.x * 73856093u) ^ (u.y * 19349663u) ^ (u.z * 83492791u);
        const uint novo = ((hh | 0x80000000u) & ~1u) | dif;
        const uint end = ((passe * ah + id.y) * aw + id.x) * 4;
        const uint velho = hist.Load(end);
        hist.Store(end, novo);
        if (!dif)
            InterlockedAdd(gIg, 1);
        if (velho & 0x80000000u)
        {
            InterlockedAdd(gBase, 1);
            if ((velho & 1u) != dif)
                InterlockedAdd(gTroca, 1);
            if ((velho >> 1) != (novo >> 1))
                InterlockedAdd(gMuda, 1);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0)
    {
        const uint b0 = vaga * 32;
        if (gIg)
            conta.InterlockedAdd(b0, gIg);
        if (gTroca)
            conta.InterlockedAdd(b0 + 8, gTroca);
        if (gMuda)
            conta.InterlockedAdd(b0 + 12, gMuda);
        if (gBase)
            conta.InterlockedAdd(b0 + 16, gBase);
    }
}
)";
    Microsoft::WRL::ComPtr<ID3DBlob> cs, err, ser;
    if (FAILED(D3DCompile(kHlsl, sizeof kHlsl - 1, "dlss5ov_confere", nullptr, nullptr, "CSConfere", "cs_5_1",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err)))
        return false;
    D3D12_DESCRIPTOR_RANGE faixa{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
    D3D12_ROOT_PARAMETER par[4]{};
    par[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    par[0].Constants.Num32BitValues = 8;
    par[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    par[1].DescriptorTable = {1, &faixa};
    par[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    par[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    par[3].Descriptor.ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC rd{4, par, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &ser, &err)) ||
        FAILED(dev->CreateRootSignature(0, ser->GetBufferPointer(), ser->GetBufferSize(), IID_PPV_ARGS(&rsConf))))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rsConf.Get();
    pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kConf * 2,
                                  D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&psoConf))) ||
        FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heapConf))))
        return psoConf = nullptr, false;
    incConf = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    void* m = nullptr;
    if (!buffer(D3D12_HEAP_TYPE_DEFAULT, kConf * 32, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_DEST, contaConf) ||
        !buffer(D3D12_HEAP_TYPE_UPLOAD, kConf * 32, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, zeroConf) ||
        !buffer(D3D12_HEAP_TYPE_READBACK, kConf * 32, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, leConf) ||
        FAILED(zeroConf->Map(0, nullptr, &m)))
        return psoConf = nullptr, false;
    zeroMapa = static_cast<uint32_t*>(m); // mapeado para sempre (upload)
    contaEstado = D3D12_RESOURCE_STATE_COPY_DEST;
    return true;
}

namespace {
DXGI_FORMAT Tipado(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

D3D12_RESOURCE_BARRIER Transicao(ID3D12Resource* r, D3D12_RESOURCE_STATES de, D3D12_RESOURCE_STATES para)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, de, para};
    return b;
}
} // namespace

// grava a conferencia da copia deste quadro na lista da apresentacao (antes da AMD, que liga o heap e a raiz dela)
void Geracao::Impl::Confere(ID3D12GraphicsCommandList* c, ID3D12Resource* copia, const FfxResource& final_, uint32_t passe)
{
    if (!psoConf || !c || !copia || !final_.resource)
        return;
    auto* fin = reinterpret_cast<ID3D12Resource*>(final_.resource);
    const D3D12_RESOURCE_DESC fd = fin->GetDesc();
    const uint32_t w = uint32_t(fd.Width), h = fd.Height, aw = (w + 2) / 4, ah = (h + 2) / 4;
    if (passe >= 8 || 8ull * aw * ah * 4 > histTam)
        return;
    const uint32_t v = marcaConf % kConf, marca = ++marcaConf;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = Tipado(fmtTela);
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE hc = heapConf->GetCPUDescriptorHandleForHeapStart();
    hc.ptr += SIZE_T(v) * 2 * incConf;
    dev->CreateShaderResourceView(copia, &sv, hc);
    hc.ptr += incConf;
    dev->CreateShaderResourceView(fin, &sv, hc);
    for (int i = 0; i < 8; ++i)
        zeroMapa[v * 8 + i] = i == 1 ? marca : 0;
    conf[v] = {marca, passe, aw * ah};
    // a copia ja esta em leitura de compute (SemHud); o quadro final, no estado que a AMD deu
    const D3D12_RESOURCE_STATES finEstado = D3D(final_.state), leitura = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const bool transFin = !(finEstado & leitura);
    D3D12_RESOURCE_BARRIER b[2];
    UINT nb = 0;
    if (contaEstado != D3D12_RESOURCE_STATE_COPY_DEST)
        b[nb++] = Transicao(contaConf.Get(), contaEstado, D3D12_RESOURCE_STATE_COPY_DEST);
    if (transFin)
        b[nb++] = Transicao(fin, finEstado, leitura);
    if (nb)
        c->ResourceBarrier(nb, b);
    c->CopyBufferRegion(contaConf.Get(), UINT64(v) * 32, zeroConf.Get(), UINT64(v) * 32, 32);
    b[0] = Transicao(contaConf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    c->ResourceBarrier(1, b);
    ID3D12DescriptorHeap* heaps[] = {heapConf.Get()};
    c->SetDescriptorHeaps(1, heaps);
    c->SetComputeRootSignature(rsConf.Get());
    c->SetPipelineState(psoConf.Get());
    const uint32_t k[8] = {w, h, v, passe, aw, ah, 0, 0};
    c->SetComputeRoot32BitConstants(0, 8, k, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE hg = heapConf->GetGPUDescriptorHandleForHeapStart();
    hg.ptr += UINT64(v) * 2 * incConf;
    c->SetComputeRootDescriptorTable(1, hg);
    c->SetComputeRootUnorderedAccessView(2, contaConf->GetGPUVirtualAddress());
    c->SetComputeRootUnorderedAccessView(3, histConf->GetGPUVirtualAddress());
    c->Dispatch((aw + 7) / 8, (ah + 7) / 8, 1);
    nb = 0;
    b[nb++] = Transicao(contaConf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (transFin)
        b[nb++] = Transicao(fin, leitura, finEstado);
    c->ResourceBarrier(nb, b);
    c->CopyBufferRegion(leConf.Get(), UINT64(v) * 32, contaConf.Get(), UINT64(v) * 32, 32);
    contaEstado = D3D12_RESOURCE_STATE_COPY_SOURCE;
}

// os resultados que ja voltaram da GPU (a marca da vaga no buffer de leitura e a desta conferencia)
void Geracao::Impl::LeConferencias()
{
    if (!leConf)
        return;
    void* m = nullptr;
    const D3D12_RANGE tudo{0, kConf * 32};
    if (FAILED(leConf->Map(0, &tudo, &m)))
        return;
    const uint32_t* x = static_cast<const uint32_t*>(m);
    Conferencia prontas[kConf];
    uint32_t valores[kConf][8], n = 0;
    for (int v = 0; v < kConf; ++v)
        if (conf[v].marca && x[v * 8 + 1] == conf[v].marca)
        {
            prontas[n] = conf[v];
            std::copy(x + v * 8, x + v * 8 + 8, valores[n++]);
            conf[v].marca = 0;
        }
    const D3D12_RANGE nada{0, 0};
    leConf->Unmap(0, &nada);
    for (uint32_t i = 0; i < n; ++i)
    {
        const uint32_t* r = valores[i]; // iguais, marca, trocas, mudancas, com historico
        const double total = prontas[i].total ? double(prontas[i].total) : 1.0, base = r[4] ? double(r[4]) : 1.0;
        Resultado(prontas[i].passe, double(r[0]) / total, double(r[2]) / base, r[4] && double(r[3]) / base >= kMovimento);
    }
}

// procura do passe `desde` em diante (sem passes depois dele: nenhum confere, espera)
void Geracao::Impl::Procura(uint32_t desde)
{
    faseHud = 0;
    conferidaHud = false;
    falhasHud = faltasHud = instaveisHud = 0;
    for (auto& m : medida)
        m = {};
    if (desde >= std::min<uint32_t>(passesHud, 8))
    {
        desde = 0;
        faseHud = 2;
        pausaHud = 600;
    }
    passeHud = desde;
}

// uma conferencia (igual: fracao da tela igual ao quadro final; troca: da mascara do que difere, contra a anterior do
// mesmo passe; movimento: a cena andou desde ela). Procurando, do primeiro passe ao ultimo, fica o PRIMEIRO que:
//   - e igual ao quadro final em 70% da tela ou mais, mas nao em tudo (antes dos pos-efeitos de cor/grao difere na tela
//     toda; o proprio quadro final, 100% igual, ja tem o HUD; num menu grande nenhum confere e a procura segue);
//   - com a cena andando, a mascara do que difere fica parada (o HUD) -- a de antes de uma nitidez anda com as bordas.
//   Sem a cena andar em 40 conferencias, vale so a igualdade, e a mascara e conferida em uso quando a cena andar.
// Em uso: um menu nao derruba a escolha (ela continua certa e o menu fica parado); a mascara andando com a cena 8 vezes
// seguidas (passe de antes de um pos-efeito, aceito sem a cena andar) ou 600 seguidas quase sem nada igual (o jogo mudou
// os passes) = procura de novo, do primeiro (um efeito de HUD animado passageiro nao empurra a escolha para um passe
// que ja tem HUD). Nenhum confere: espera 600 apresentacoes e procura de novo
void Geracao::Impl::Resultado(uint32_t passe, double igual, double troca, bool movimento)
{
    if (passe != passeHud || passe >= 8)
        return; // de uma escolha anterior
    igualHud = igual;
    if (movimento)
        trocaHud = troca;
    if (faseHud == 1)
    {
        faltasHud = 0;
        falhasHud = igual >= 0.05 ? 0 : falhasHud + 1;
        if (movimento)
        {
            instaveisHud = troca > kInstavel ? instaveisHud + 1 : 0;
            if (!instaveisHud)
                conferidaHud = true;
        }
        if (instaveisHud >= 8 || falhasHud >= 600)
            Procura();
        return;
    }
    if (faseHud != 0)
        return;
    faltasHud = 0;
    Medida& m = medida[passe];
    ++m.n;
    m.igual += igual;
    if (movimento)
        ++m.nMov, m.troca += troca;
    const double media = m.igual / m.n;
    bool aceita = false, rejeita = false;
    if (m.n >= 3 && (media < kConfere || media >= 0.9999))
        rejeita = true;
    else if (m.nMov >= 3)
        (m.troca / m.nMov <= kInstavel ? aceita : rejeita) = true;
    else if (m.n >= 40)
        aceita = true;
    if (aceita)
    {
        const bool conferida = m.nMov >= 3;
        Procura(passe);
        faseHud = 1;
        conferidaHud = conferida;
    }
    else if (rejeita)
        Procura(passe + 1);
}

// a apresentacao veio sem copia do passe escolhido (o quadro teve menos passes): procurando, 6 seguidas = o passe nao
// existe (conta como nao conferido); em uso, 240 seguidas = o passe sumiu (o jogo mudou os passes)
void Geracao::Impl::Falta()
{
    if (faseHud == 1)
    {
        if (++faltasHud >= 240)
            Procura();
        return;
    }
    if (faseHud != 0 || ++faltasHud < 6)
        return;
    faltasHud = 0;
    for (int i = 0; i < 3; ++i)
        Resultado(passeHud, 0.0, 0.0, false);
}

// chamada pelo swapchain da AMD na apresentacao (lista e quadro dele): fluxo optico do backbuffer e interpolacao, com
// a profundidade e o movimento preparados no quadro (como Dispatch do provedor da AMD). Com n - 1 gerados, a interpolacao
// roda uma vez para cada, em t = k/n, e so a ultima guarda o quadro atual como o anterior do proximo par
FfxErrorCode Geracao::Impl::Gera(const FfxFrameGenerationDispatchDescription* d, void* ctx)
{
    Impl* p = static_cast<Impl*>(ctx);
    // o quadro deste backbuffer: o preparado mais antigo ainda nao apresentado (a apresentacao segue a ordem dos quadros)
    Preparado pr{};
    {
        std::lock_guard<std::mutex> lk(p->filaMx);
        // com os envios: o preparo mais novo ja enviado a GPU (a imagem deste Present saiu das listas enviadas antes
        // dele); os mais velhos que ele saem (quadros que nunca foram apresentados). Sem: o mais antigo gravado
        const size_t k = p->EscolhePreparado();
        if (p->rastroLinhas < 120)
        {
            const auto agora = std::chrono::steady_clock::now();
            char linha[320];
            int n = snprintf(linha, sizeof linha, "apresentacao %u (+%.1f ms, %llu despachos do FSR desde a anterior): fila",
                             p->rastroLinhas, p->rastroLinhas ? std::chrono::duration<double, std::milli>(agora - p->rastroAnt).count() : 0.0,
                             (unsigned long long)p->despachosRastro);
            for (const auto& f : p->fila)
                n += snprintf(linha + n, sizeof linha - n, " %llu:%llu", (unsigned long long)f.id, (unsigned long long)f.enviado);
            snprintf(linha + n, sizeof linha - n, " -> %s%llu\n", k == p->fila.size() ? "nenhum (so o real) " : "quadro ",
                     k == p->fila.size() ? 0ull : (unsigned long long)p->fila[k].id);
            p->rastro += linha;
            ++p->rastroLinhas;
            p->rastroAnt = agora;
            p->despachosRastro = 0;
        }
        if (k == p->fila.size())
        {
            ++p->semPreparo;
            return FFX_ERROR_INVALID_ARGUMENT; // sem o movimento deste quadro: a apresentacao vai sem gerado
        }
        pr = p->fila[k];
        uint64_t maisNovo = pr.id;
        for (auto it = p->fila.begin(); it != p->fila.end();)
        {
            maisNovo = std::max(maisNovo, it->id);
            it = it->id <= pr.id ? p->fila.erase(it) : it + 1;
        }
        const uint64_t aFrente = maisNovo - pr.id; // quadros que o jogo ja gravou a frente do apresentado
        static const char* depura = std::getenv("DLSS5_GERACAO_FILA"); // teste: as decisoes da fila num arquivo
        if (depura && p->chamadas < 40)
            if (FILE* f = fopen(depura, "a"))
            {
                fprintf(f, "present %llu: escolhido %llu (envio %llu), mais novo %llu, configurado %llu, envio %d\n",
                        (unsigned long long)p->chamadas, (unsigned long long)pr.id, (unsigned long long)pr.enviado,
                        (unsigned long long)maisNovo, (unsigned long long)d->frameID, int(p->comEnvio));
                fclose(f);
            }
        p->aFrenteSoma += aFrente;
        ++p->aFrenteN;
        p->aFrenteMax = std::max(p->aFrenteMax, aFrente);
        // entre reais que nao sao seguidos no jogo, sem gerado (so o real): nunca interpolar por cima de quadros pulados
        const bool salto = p->idApresentado && pr.id != p->idApresentado + 1 && !d->reset;
        p->idApresentado = pr.id;
        if (salto)
        {
            ++p->saltos;
            return FFX_ERROR_INVALID_ARGUMENT;
        }
    }
    // o backend da AMD e o do preparo: um de cada vez (fiMx). Com o contexto sendo refeito (estado 1) nao ha o que gerar
    std::unique_lock<std::mutex> lf(p->fiMx, std::try_to_lock);
    if (!lf.owns_lock())
        ++p->encontros, lf.lock();
    if (p->estado != 2 || !p->contexto)
        return FFX_ERROR_INVALID_ARGUMENT;
    if (!p->threadsDitas && p->rastroLinhas < 120)
    {
        p->threadsDitas = true;
        char linha[96];
        snprintf(linha, sizeof linha, "threads: preparo %lu, geracao %lu\n", (unsigned long)p->threadPreparo.load(),
                 (unsigned long)GetCurrentThreadId());
        std::lock_guard<std::mutex> lk(p->filaMx); // o rastro e lido (Rastro) com ela
        p->rastro += linha;
    }
    const FfxFrameInterpolationPrepareDescription& pd = pr.pd;
    // a copia sem HUD deste quadro e conferida contra o quadro final; so a do passe conferido vai para a AMD: o fluxo
    // optico e a interpolacao partem dela e o HUD vem do quadro final, parado (como o FSR 3 da AMD com HUDLessColor,
    // ffx_fsr3.cpp)
    p->LeConferencias();
    if (p->faseHud == 2 && p->pausaHud && --p->pausaHud == 0)
        p->Procura();
    ID3D12Resource* semHud = nullptr;
    // a do proprio jogo (HUDLessColor), copiada no Present dele (AntesDoPresent): a deste quadro
    const int copia = p->copiaPronta.exchange(-1);
    if (p->uiEmUso && copia >= 0)
    {
        semHud = p->copiaSemHud[copia].Get();
        ++p->copiasUsadas;
        p->chamadaUltimaCopia = p->chamadas;
    }
    else if (pr.semHud)
    {
        ID3D12Resource* copia = p->semHud[pr.id % kJogos].Get();
        p->Confere(reinterpret_cast<ID3D12GraphicsCommandList*>(d->commandList), copia, d->presentColor, pr.passeHud);
        if (p->faseHud == 1 && pr.passeHud == p->passeHud)
            semHud = copia;
    }
    else if (!p->UiAtiva()) // com a UI do jogo (textura ou callback) o quadro ja chega sem ela: nada a procurar
        p->Falta();
    p->semHudApresentado = semHud ? semHud : pr.semHud ? p->semHud[pr.id % kJogos].Get() : nullptr;
    // a neural (2x: o meio; 4x: o intercalado de dois contextos) com a camera deste quadro. 3x: o meio uma vez,
    // apresentado duas (real > meio > meio > real), sem quadro de outro tempo
    const uint32_t m = std::clamp<uint32_t>(d->numInterpolatedFrames, 1, 3);
    const uint32_t mg = m == 2 ? 1u : m; // os gerados de fato
    auto repete = [&] {
        if (m != 2)
            return;
        auto* c = reinterpret_cast<ID3D12GraphicsCommandList*>(d->commandList);
        auto* o0 = reinterpret_cast<ID3D12Resource*>(d->outputs[0].resource);
        auto* o1 = reinterpret_cast<ID3D12Resource*>(d->outputs[1].resource);
        D3D12_RESOURCE_BARRIER b[2] = {};
        b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[0].Transition = {o0, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_COPY_SOURCE};
        b[1].Transition = {o1, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_COPY_DEST};
        c->ResourceBarrier(2, b);
        c->CopyResource(o1, o0);
        for (auto& x : b)
            std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
        c->ResourceBarrier(2, b);
    };
    // a reprojecao nos gerados acima do limite de movimento (escala com a altura da tela)
    auto borra = [&] {
        if (!pr.neural || !p->repro.Pronta() || p->limiteRepro < 0.f)
            return;
        ID3D12Resource* saidas[3] = {};
        for (uint32_t k = 0; k < mg; ++k)
            saidas[k] = reinterpret_cast<ID3D12Resource*>(d->outputs[k].resource);
        p->repro.Gera(reinterpret_cast<ID3D12GraphicsCommandList*>(d->commandList), pr.id, pr.qn,
                      (p->flagsFi & FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED) != 0,
                      reinterpret_cast<ID3D12Resource*>(d->presentColor.resource), semHud, saidas, mg,
                      p->limiteRepro * float(d->presentColor.description.height) / 1080.f, d->reset);
    };
    // sem a camera do jogo (a geracao FSR dele desligada no menu: sem o preparo), a neural vai sem ela (pior no giro, mas
    // gera; so a Redstone, sem a analitica)
    if (pr.neural && p->neural.Pronta())
    {
        ID3D12Resource* saidas[3] = {};
        for (uint32_t k = 0; k < mg; ++k)
            saidas[k] = reinterpret_cast<ID3D12Resource*>(d->outputs[k].resource);
        if (p->neural.Gera(reinterpret_cast<ID3D12GraphicsCommandList*>(d->commandList), pr.id, pr.qn,
                           p->temCamAnterior ? p->camAnterior : pr.qn.cam,
                           reinterpret_cast<ID3D12Resource*>(d->presentColor.resource), uint32_t(d->presentColor.state),
                           semHud, uint32_t(FFX_RESOURCE_STATE_COMPUTE_READ), saidas,
                           uint32_t(d->outputs[0].state), mg, d->reset, uint32_t(d->backBufferTransferFunction),
                           d->minMaxLuminance[1] > 0.f ? d->minMaxLuminance[0] : p->lumMinTela,
                           d->minMaxLuminance[1] > 0.f ? d->minMaxLuminance[1] : (p->lumMaxTela > 0.f ? p->lumMaxTela : 1000.f)))
        {
            p->camAnterior = pr.qn.cam, p->temCamAnterior = pr.temCam;
            borra();
            repete();
            p->gerados += m;
            ++p->chamadas;
            ++p->quadrosNeural;
            p->Despejo(d, pr.id);
            return FFX_OK;
        }
        snprintf(p->erroNeural, sizeof p->erroNeural, "%s", p->neural.Erro());
    }
    p->temCamAnterior = pr.temCam;
    if (pr.temCam)
        p->camAnterior = pr.qn.cam;
    // so a neural (FSR 4 Redstone): sem ela (sem a camera do jogo, sem o provedor 4.x ou se falhar) vai so o real -- a
    // analitica do FSR 3.1 saiu (blocos 8x8 no cabelo e no movimento rapido)
    ++p->quadrosAnalitica;
    return FFX_ERROR_INVALID_ARGUMENT;
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

void Geracao::Impl::Despejo(const FfxFrameGenerationDispatchDescription* d, uint64_t id)
{
    {
        std::lock_guard<std::mutex> lk(despejoMx);
        if (!despejoPedido.empty()) // a gravacao do overlay (DespejaEm): um quadro daqui a 30 apresentacoes
        {
            despejoPasta = std::move(despejoPedido);
            despejoPedido.clear();
            despejoAlvo = chamadas + 30;
            despejoQuadro = 0;
            despejoId = 0;
            for (auto& r : despejoLeitura)
                r = nullptr;
            despejoAtivo = true;
        }
    }
    if (!despejoAtivo)
        return;
    const char* pasta = despejoPasta.c_str();
    auto* c = reinterpret_cast<ID3D12GraphicsCommandList*>(d->commandList);
    if (despejoQuadro && chamadas == despejoQuadro + 8)
    {
        const char* nomes[10] = {"gerado1", "gerado2", "gerado3", "real", "anterior", "semhud",
                                 "tela_gerado1", "tela_gerado2", "tela_gerado3", "tela_real"};
        for (int k = 0; k < 10; ++k)
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
        despejoAtivo = false; // um quadro por pedido
        return;
    }
    const uint64_t alvo = despejoAlvo;
    if (chamadas == alvo - 1)
        CopiaParaLeitura(c, d->presentColor, 4);
    if (chamadas != alvo || despejoQuadro)
        return;
    despejoQuadro = chamadas;
    despejoTelaK = 0;
    despejoId = id;
    const uint32_t m = std::clamp<uint32_t>(d->numInterpolatedFrames, 1, 3);
    for (uint32_t k = 0; k < m; ++k)
        if (!CopiaParaLeitura(c, d->outputs[k], int(k)))
            return;
    CopiaParaLeitura(c, d->presentColor, 3);
    if (semHudApresentado)
        CopiaParaLeitura(c, ffxGetResourceDX12(semHudApresentado, ffxGetResourceDescriptionDX12(semHudApresentado),
                                               L"imagem sem HUD", FFX_RESOURCE_STATE_COMPUTE_READ), 5);
}

Geracao::Geracao() : p(new Impl)
{
    if (const char* e = std::getenv("DLSS5_GERACAO_DUMP"))
    {
        const char* q = std::getenv("DLSS5_GERACAO_DUMP_QUADRO");
        p->despejoPasta = e;
        p->despejoAlvo = std::max<uint64_t>(2, q ? uint64_t(_atoi64(q)) : 60);
        p->despejoAtivo = true;
    }
}

void Geracao::DespejaEm(const char* pasta)
{
    std::lock_guard<std::mutex> lk(p->despejoMx);
    p->despejoPedido = pasta ? pasta : "";
}

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
    const HRESULT h = ffxGetDX12SwapchainPtr(s)->QueryInterface(IID_PPV_ARGS(saida));
    p->Adota(s, fila);
    return h;
}

HRESULT Geracao::CriaSwapchain(IDXGIFactory* fabrica, ID3D12CommandQueue* fila, DXGI_SWAP_CHAIN_DESC* d,
                               IDXGISwapChain** saida)
{
    FfxSwapchain s = nullptr;
    const FfxErrorCode e = ffxCreateFrameinterpolationSwapchainDX12(d, fila, fabrica, s);
    if (e != FFX_OK || !s)
        return p->Erro("ffxCreateFrameinterpolationSwapchainDX12", e), E_FAIL;
    // como o da janela: o swapchain e so do jogo, com o aviso de destruicao (preso aqui, a troca de modo de tela falhava e
    // a saida caia no driver)
    const HRESULT h = ffxGetDX12SwapchainPtr(s)->QueryInterface(IID_PPV_ARGS(saida));
    p->Adota(s, fila);
    return h;
}

bool Geracao::TemSwapchain() const
{
    return p->sc != nullptr;
}

// o swapchain criado (com a referencia da criacao) passa ao jogo: o modulo guarda so o ponteiro e e avisado antes da
// destruicao
void Geracao::Impl::Adota(FfxSwapchain s, ID3D12CommandQueue* fila)
{
    std::lock_guard<std::mutex> lk(mx);
    ffxSc = s;
    sc = ffxGetDX12SwapchainPtr(s);
    if (!dev)
        fila->GetDevice(IID_PPV_ARGS(&dev));
    ffxDlss5AoDestruirDX12(s, &Impl::AoDestruir, this);
    sc->Release(); // a da criacao: o jogo ja tem a dele (QueryInterface)
}

// a AMD vai destruir o swapchain (o jogo soltou a ultima referencia; as apresentacoes em voo ja terminaram): o contexto
// sai antes, e o proximo swapchain do jogo tambem vira o da AMD
void Geracao::Impl::AoDestruir(void* ctx)
{
    Impl* p = static_cast<Impl*>(ctx);
    if (p->criador.joinable())
        p->criador.join();
    std::lock_guard<std::mutex> lk(p->mx);
    p->ffxSc = nullptr; // Destroi nao espera de novo (nem toca no swapchain que esta saindo)
    p->sc = nullptr;
    {
        std::lock_guard<std::mutex> lf(p->filaMx);
        p->fila.clear();
        p->rastroLinhas = 0; // o proximo swapchain tem o rastro dele
    }
    p->Destroi();
    p->estado = 0;
    p->quadro = 0;
    p->nEmUso = 0;
    p->provaDe = 0;
}

// com a geracao ligada o jogo nao passa de 97% da taxa / N -- acima disso a tela recebe mais do que mostra, e com o ritmo
// do jogo oscilando em volta do encaixe (o sample da AMD entre 60 e 65 q/s a 120 Hz) a geracao ligava e desligava: engasgo
void Geracao::Impl::Limita()
{
    msSono = 0;
    const uint32_t taxa = hz;
    if (!taxa || nEmUso < 2 || SemLimite() || !tAnt.time_since_epoch().count())
        return;
    const double alvo = 1000.0 * nEmUso / (taxa * 0.97);
    const auto inicio = std::chrono::steady_clock::now();
    const double falta = alvo - std::chrono::duration<double, std::milli>(inicio - tAnt).count();
    if (falta <= 0.05 || falta > 100.0)
        return;
    if (!relogio)
        relogio = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (relogio && falta > 1.0) // o grosso dormindo (relogio de alta resolucao), o resto girando
    {
        LARGE_INTEGER t;
        t.QuadPart = -LONGLONG((falta - 0.6) * 10000.0); // relativo, em 100 ns
        if (SetWaitableTimer(relogio, &t, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(relogio, 200);
    }
    while (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tAnt).count() < alvo)
        YieldProcessor();
    msSono = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - inicio).count();
}

// o ritmo do jogo entre as chamadas de Quadro: o entregue (msAtual) e o de trabalho, sem a espera do limitador (msJogo)
void Geracao::Impl::Mede()
{
    const auto agora = std::chrono::steady_clock::now();
    if (tAnt.time_since_epoch().count())
    {
        const double ms = std::chrono::duration<double, std::milli>(agora - tAnt).count();
        if (ms > 0.5 && ms < 250.0)
        {
            msAtual = msAtual > 0 ? msAtual * 0.9 + ms * 0.1 : ms;
            msJogo = msJogo > 0 ? msJogo * 0.9 + (ms - msSono) * 0.1 : ms - msSono;
        }
    }
    tAnt = agora;
}

void Geracao::DefineTaxa(uint32_t hz, uint32_t hzMax)
{
    p->hz = hz;
    p->hzMax = std::max(hz, hzMax);
}

uint32_t Geracao::Impl::NEmUso(uint32_t pedido)
{
    if (pedido < 2)
    {
        nEmUso = 1;
        trocaDesde = {};
        provaDe = 0;
        return 1;
    }
    if (pedido < provaDe) // o pedido mudou no painel durante uma prova: vale o pedido
        provaDe = 0;
    const uint32_t hzAgora = hz;
    if (SemLimite() || !hzAgora)
        return nEmUso = pedido;
    if (msJogo <= 0)
        return 1; // o primeiro quadro: sem gerados ate saber o ritmo do jogo
    const double taxa = hzAgora;
    // o menor N que enche a tela (mais quadros de verdade: menos atraso e menos artefato; o limitador segura o jogo em
    // 97% da taxa / N); sem nenhum que encha, o pedido
    auto menorN = [&](double fps) {
        for (uint32_t k = 2; k <= pedido; ++k)
            if (fps * k >= taxa * 0.97)
                return k;
        return pedido;
    };
    uint32_t novo;
    if (nEmUso < 2)
        // desligada: liga com o jogo entregando ate 3/4 da taxa (acima disso trocaria quadros de verdade por gerados
        // quase sem ganho)
        novo = 1000.0 / msAtual <= taxa * 0.75 ? menorN(1000.0 / msAtual) : 1;
    else
    {
        // ligada: so ajusta o N pelo ritmo de trabalho (sem a espera do limitador) e nunca desliga sozinha -- o limitador
        // do proprio jogo (o sample da AMD trava em 60) confunde essa medida e a geracao ficava ligando e desligando. E
        // so troca com folga clara: sobe se a tela ficar 10% abaixo da taxa, desce se um a menos ainda passar 5% dela
        // (sem isso, com o jogo a 48 q/s o N ia e voltava entre 2 e 3 a cada 2 s -- cada troca muda o ritmo da tela)
        // So sobe: segurado pelo limitador o jogo parece mais rapido do que e (a GPU folga e o tempo de CPU cai -- em 3x
        // ele media 75 q/s de trabalho e em 2x so fazia 43), entao descer sozinho voltava a subir a cada poucos segundos.
        // Desce quando o pedido muda no painel (ou a geracao desliga e liga) -- e por prova: preso num N alto (o jogo lento
        // por um tempo: no TLOU2 a rede a 4K o deixou a 13 q/s, o N foi a 4 e o limitador o segurou em 29 q/s depois de
        // ele poder fazer ~50; mais atraso de entrada sem precisar), um a menos e tentado quando o trabalho diz que ele
        // enche a tela com 5% de folga. Depois de 3 s a tela cheia confirma; se nao encher, volta e a proxima prova espera
        // o dobro (10 s .. 160 s): a medida enganosa do limitador custa so umas provas espacadas, nao o vai e volta
        const double fps = 1000.0 / msJogo;
        const auto agora = std::chrono::steady_clock::now();
        novo = nEmUso;
        // o jogo entrega (com a espera do limitador) mais reais do que cabem na tela com este N: o limitador nao o segurou
        // (TLOU2: 83 q/s em 4x a 120 Hz) e a tela pulava reais. Desce ja, ate 2
        const double entregue = 1000.0 / msAtual;
        if (nEmUso > 2 && entregue * nEmUso > taxa * 1.15)
        {
            uint32_t k = nEmUso;
            while (k > 2 && entregue * k > taxa * 1.15)
                --k;
            ++trocasN;
            provaDe = 0, trocaDesde = {};
            return nEmUso = k;
        }
        if (provaDe)
        {
            const double entregue = 1000.0 / msAtual; // com a espera do limitador: o que a tela recebe de fato
            const auto dur = agora - provaDesde;
            if (dur >= std::chrono::seconds(1) && entregue * nEmUso < taxa * 0.85) // nao enche: volta e espera mais
            {
                provaEspera = std::min(provaEspera * 2, 160);
                provaProxima = agora + std::chrono::seconds(provaEspera);
                trocaDesde = {};
                ++trocasN;
                nEmUso = provaDe, provaDe = 0;
                return nEmUso;
            }
            if (dur >= std::chrono::seconds(3)) // encheu: fica, e pode tentar mais um abaixo depois
                provaDe = 0, provaEspera = 10, provaProxima = agora + std::chrono::seconds(10);
            return nEmUso;
        }
        if (fps * nEmUso < taxa * 0.90)
            novo = std::max(nEmUso, menorN(fps));
        else if (nEmUso > 2 && fps * (nEmUso - 1) >= taxa * 1.05 && agora >= provaProxima)
        {
            ++trocasN;
            provaDe = nEmUso, provaDesde = agora, trocaDesde = {};
            return --nEmUso;
        }
        novo = std::min(novo, pedido);
    }
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
    else if (agora - trocaDesde >= std::chrono::milliseconds(novo < nEmUso ? (nEmUso > 2 && novo >= 2 ? 3000 : 500) : 1500))
    {
        ++trocasN; // diagnostico: o log de 5 s mostra quantas vezes o N mudou
        nEmUso = novo;
        trocaDesde = {};
    }
    return nEmUso;
}

bool Geracao::Quadro(ID3D12GraphicsCommandList* c, const QuadroGeracao& q, uint32_t n)
{
    p->Limita(); // antes da trava: a espera nao segura o aviso de destruicao
    std::lock_guard<std::mutex> lk(p->mx);
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
        std::lock_guard<std::mutex> lk(p->filaMx);
        p->fila.clear();
        return false;
    }
    // profundidade e movimento deste quadro, dilatados e com a profundidade anterior reconstruida (Prepare da AMD), num
    // dos tres jogos de recursos; entra na fila e espera a apresentacao dele
    ++p->quadro;
    const int jogo = int(p->quadro % kJogos);
    FfxFrameInterpolationPrepareDescription pd{};
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
    pd.dilatedDepth = p->Rec(R_PROF_DIL + jogo);
    pd.dilatedMotionVectors = p->Rec(R_MOV_DIL + jogo);
    pd.reconstructedPrevDepth = p->Rec(R_PROF_REC + jogo);
    FfxErrorCode e;
    {
        std::unique_lock<std::mutex> lf(p->fiMx, std::try_to_lock);
        if (!lf.owns_lock())
            ++p->encontros, lf.lock();
        p->threadPreparo = GetCurrentThreadId();
        e = ffxFrameInterpolationPrepare(&p->fic, &pd);
    }
    if (e != FFX_OK)
        return p->Erro("ffxFrameInterpolationPrepare", e), false;
    Impl::Preparado novo{p->quadro, pd, ListaNativa(c), 0, false, 0};
    if (p->neural.Pronta() || p->repro.Pronta()) // a neural e a reprojecao usam o movimento copiado
        novo.neural = p->CopiaNeural(c, q, jogo, novo.qn);
    {
        std::lock_guard<std::mutex> lk(p->filaMx);
        ++p->despachosRastro;
        // a camera do preparo da geracao do jogo deste quadro, se ja veio (o jogo pode prepara-la antes do upscaler)
        if (novo.neural && !p->cameras.empty())
            novo.qn.cam = p->cameras.front(), novo.temCam = true, p->cameras.pop_front();
        p->fila.push_back(novo);
        while (p->fila.size() > size_t(kJogos)) // mais preparados que apresentacoes (o jogo pulou um Present): o velho sai
            p->fila.pop_front();
    }
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
    {
        // a UI do jogo desenhada pelo callback dele em cada quadro (real e gerado); sem ele, a composicao da AMD (a
        // textura de UI do jogo, se registrada no nosso, ou so a copia)
        std::lock_guard<std::mutex> lu(p->uiMx);
        if (p->uiCb || p->despejoAtivo) // despejando, sempre: o despejo copia o quadro composto
            cfg.presentCallback = &Impl::UiCallback, cfg.presentCallbackContext = p;
    }
    if ((e = p->shared.fpSwapChainConfigureFrameGeneration(&cfg)) != FFX_OK)
        return p->Erro("configuracao do swapchain", e), false;
    return true;
}

// a UI do jogo vem para o nosso swapchain (ou volta para o dele): chamada pelo modulo a cada configuracao da geracao do
// jogo, na thread de render, depois do despacho do FSR deste quadro (o preparado mais novo da fila e o deste quadro)
void Geracao::UiJogo(const UiDoJogo& u, bool nossa)
{
    std::lock_guard<std::mutex> lk(p->mx);
    if (!p->ffxSc)
        return;
    p->uiEmUso = nossa && u.Tem();
    {
        std::lock_guard<std::mutex> lu(p->uiMx);
        p->uiCb = nossa ? u.callback : nullptr;
        p->uiCbCtx = nossa ? u.callbackCtx : nullptr;
        p->uiIds[p->quadro % 8] = {p->quadro, u.frameId};
    }
    // a textura: registrada no nosso a cada quadro (a copia interna dela, a cada Present, solta o registro), com a copia
    // ligada (os gerados deste quadro sao compostos depois de o jogo comecar a UI do proximo); fora, tirada uma vez
    if (nossa && u.textura)
    {
        const FfxResource r = ffxGetResourceDX12(u.textura, ffxGetResourceDescriptionDX12(u.textura), L"UI do jogo",
                                                 FfxResourceStates(u.texturaEstado));
        ffxRegisterFrameinterpolationUiResourceDX12(p->ffxSc, r,
                                                    u.texturaFlags | FFX_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING);
        p->uiTexturaNossa = true;
    }
    else if (p->uiTexturaNossa)
    {
        ffxRegisterFrameinterpolationUiResourceDX12(p->ffxSc, FfxResource{}, 0);
        p->uiTexturaNossa = false;
    }
    // a imagem sem HUD: a da ultima configuracao, copiada no Present (AntesDoPresent) -- como o swapchain da AMD, que no
    // Present usa a configuracao mais nova (o jogo configura o quadro antes do Present dele e o seguinte depois). Casar
    // pelo frameID do preparo do jogo falhou no TLOU2: nenhuma copia (o preparo dele nao vem na lista do FSR)
    p->uiSemHud = nossa ? u.semHud : nullptr;
    p->uiSemHudEstado = u.semHudEstado;
    p->uiSoSemHud = u.semHud && !u.textura && !u.callback;
}

void Geracao::CopiasSemHud(uint64_t& feitas, uint64_t& usadas) const
{
    feitas = p->copiasFeitas, usadas = p->copiasUsadas;
}

void Geracao::FilaDoJogo(ID3D12CommandQueue* q)
{
    std::lock_guard<std::mutex> lk(p->mx);
    p->filaJogo = q;
}

// No Present do swapchain de geracao do jogo (gancho do modulo), antes dele: a imagem sem HUD do quadro copiada para uma
// das nossas, na fila do jogo -- depois de tudo o que o jogo enviou para este quadro e antes do seguinte. A interpolacao
// (na fila dela, depois) le a copia: a mesma imagem que a geracao do jogo leria no Present
void Geracao::AntesDoPresent()
{
    std::lock_guard<std::mutex> lk(p->mx);
    p->copiaPronta = -1;
    if (!p->uiEmUso || !p->dev || p->estado != 2)
        return;
    ++p->antesChamadas;
    ID3D12Resource* fonte = p->uiSemHud;
    const uint32_t estadoFonte = p->uiSemHudEstado;
    if (!fonte)
    {
        ++p->antesSemImagem;
        return;
    }
    if (!p->filaJogo)
    {
        ++p->antesSemFila;
        return;
    }
    const D3D12_RESOURCE_DESC fd = fonte->GetDesc();
    if (fd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || fd.Width != p->telaW || fd.Height != p->telaH ||
        fd.SampleDesc.Count != 1)
    {
        ++p->antesTamanho; // a interpolacao quer a imagem no tamanho da tela
        p->tamW = UINT(fd.Width), p->tamH = fd.Height, p->tamFmt = fd.Format;
        return;
    }
    if (!p->copiaCerca)
    {
        if (FAILED(p->dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p->copiaCerca))))
            return;
        for (auto& a : p->copiaAl)
            if (FAILED(p->dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))))
                return;
        if (FAILED(p->dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p->copiaAl[0].Get(), nullptr,
                                             IID_PPV_ARGS(&p->copiaLista))))
            return;
        p->copiaLista->Close();
    }
    const int k = int(p->copiaN++ % Impl::kCopias);
    if (p->copiaCerca->GetCompletedValue() < p->copiaValor[k]) // a copia de 4 Presents atras (e a leitura dela) acabou
        p->copiaCerca->SetEventOnCompletion(p->copiaValor[k], nullptr);
    auto& alvo = p->copiaSemHud[k];
    if (alvo)
    {
        const D3D12_RESOURCE_DESC ad = alvo->GetDesc();
        if (ad.Width != fd.Width || ad.Height != fd.Height || ad.Format != Tipado(fd.Format))
            alvo = nullptr;
    }
    if (!alvo)
    {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = fd.Width;
        d.Height = fd.Height;
        d.DepthOrArraySize = d.MipLevels = 1;
        d.Format = Tipado(fd.Format);
        d.SampleDesc.Count = 1;
        if (FAILED(p->dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&alvo))))
            return;
        alvo->SetName(L"geracao: imagem sem HUD do jogo (copia do Present)");
        p->copiaEstado[k] = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    if (FAILED(p->copiaAl[k]->Reset()) || FAILED(p->copiaLista->Reset(p->copiaAl[k].Get(), nullptr)))
        return;
    auto* c = p->copiaLista.Get();
    const D3D12_RESOURCE_STATES estadoJogo = D3D(FfxResourceStates(estadoFonte));
    auto transicao = [](ID3D12Resource* r, D3D12_RESOURCE_STATES de, D3D12_RESOURCE_STATES para) {
        D3D12_RESOURCE_BARRIER b{};
        b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, de, para};
        return b;
    };
    D3D12_RESOURCE_BARRIER b[2];
    UINT nb = 0;
    if (estadoJogo != D3D12_RESOURCE_STATE_COPY_SOURCE)
        b[nb++] = transicao(fonte, estadoJogo, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (p->copiaEstado[k] != D3D12_RESOURCE_STATE_COPY_DEST)
        b[nb++] = transicao(alvo.Get(), p->copiaEstado[k], D3D12_RESOURCE_STATE_COPY_DEST);
    if (nb)
        c->ResourceBarrier(nb, b);
    c->CopyResource(alvo.Get(), fonte);
    nb = 0;
    if (estadoJogo != D3D12_RESOURCE_STATE_COPY_SOURCE)
        b[nb++] = transicao(fonte, D3D12_RESOURCE_STATE_COPY_SOURCE, estadoJogo);
    b[nb++] = transicao(alvo.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    c->ResourceBarrier(nb, b);
    p->copiaEstado[k] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (FAILED(c->Close()))
        return;
    ID3D12CommandList* l[] = {c};
    p->filaJogo->ExecuteCommandLists(1, l);
    p->filaJogo->Signal(p->copiaCerca.Get(), ++p->copiaUltimo);
    p->copiaValor[k] = p->copiaUltimo;
    p->copiaPronta = k;
    ++p->copiasFeitas;
}

bool Geracao::UiDoJogoEmUso() const
{
    return p->UiAtiva();
}

// o log de 5 s: a imagem sem HUD do jogo copiada (e por que nao) e usada desde o ultimo relato; false sem a UI do jogo
bool Geracao::RelatoUi(char* b, size_t n)
{
    std::lock_guard<std::mutex> lk(p->mx);
    if (!p->uiEmUso)
        return false;
    char tam[64] = "";
    if (p->antesTamanho)
        snprintf(tam, sizeof tam, " (%ux%u fmt %d, tela %ux%u)", p->tamW, p->tamH, int(p->tamFmt), p->telaW, p->telaH);
    static uint64_t feitas0 = 0, usadas0 = 0;
    snprintf(b, n,
             "UI do jogo em uso%s; imagem sem HUD copiada no Present em %llu de %llu (sem imagem %llu, outro tamanho %llu%s, "
             "sem a fila do jogo %llu), usada em %llu",
             p->UiAtiva() ? "" : " mas SEM a copia (a procura da imagem sem HUD volta)",
             (unsigned long long)(p->copiasFeitas - feitas0), (unsigned long long)p->antesChamadas,
             (unsigned long long)p->antesSemImagem, (unsigned long long)p->antesTamanho, tam,
             (unsigned long long)p->antesSemFila, (unsigned long long)(p->copiasUsadas - usadas0));
    feitas0 = p->copiasFeitas, usadas0 = p->copiasUsadas;
    p->antesChamadas = p->antesSemImagem = p->antesTamanho = p->antesSemFila = 0;
    return true;
}

// o callback de UI do jogo (FfxApiPresentCallbackFunc, ffx_api/ffx_framegeneration.h) chamado pelo nosso swapchain, na
// thread de apresentacao da AMD, em cada quadro: a descricao do SDK vira a da API (os mesmos campos e enums, como no
// provedor da AMD, ffx_provider_framegeneration.cpp) e o frameID volta a ser o do jogo
namespace {
struct ApiRecurso
{
    void* resource;
    uint32_t type, format, width, height, depth, mipCount, flags, usage;
    uint32_t state;
};
struct ApiPresenta // ffxCallbackDescFrameGenerationPresent
{
    uint64_t type;
    void* pNext;
    void* device;
    void* commandList;
    ApiRecurso currentBackBuffer, currentUI, outputSwapChainBuffer;
    bool isGeneratedFrame;
    uint64_t frameID;
};
ApiRecurso Api(const FfxResource& r)
{
    return {r.resource, uint32_t(r.description.type), uint32_t(r.description.format), r.description.width,
            r.description.height, r.description.depth, r.description.mipCount, uint32_t(r.description.flags),
            uint32_t(r.description.usage), uint32_t(r.state)};
}
} // namespace

FfxErrorCode Geracao::Impl::UiCallback(const FfxPresentCallbackDescription* d, void* ctx)
{
    Impl* p = static_cast<Impl*>(ctx);
    void* cb;
    void* cbCtx;
    uint64_t idJogo = d->frameID;
    {
        std::lock_guard<std::mutex> lu(p->uiMx);
        cb = p->uiCb, cbCtx = p->uiCbCtx;
        if (p->uiIds[d->frameID % 8].nosso == d->frameID)
            idJogo = p->uiIds[d->frameID % 8].jogo;
    }
    FfxErrorCode e;
    if (!cb) // sem o do jogo (ou ele deixou de mandar): a composicao da AMD (a textura de UI registrada, ou so a copia)
        e = ffxFrameInterpolationUiComposition(d, nullptr);
    else
    {
        ApiPresenta a{};
        a.type = 0x00020005u; // FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT
        a.device = d->device;
        a.commandList = d->commandList;
        a.currentBackBuffer = Api(d->currentBackBuffer);
        a.currentUI = Api(d->currentUI);
        a.outputSwapChainBuffer = Api(d->outputSwapChainBuffer);
        a.isGeneratedFrame = d->isInterpolatedFrame;
        a.frameID = idJogo;
        using Fn = uint32_t (*)(ApiPresenta*, void*);
        e = reinterpret_cast<Fn>(cb)(&a, cbCtx) == 0 ? FFX_OK : FFX_ERROR_BACKEND_API_ERROR;
    }
    // teste (DLSS5_GERACAO_DUMP): o quadro despejado como foi para a tela, gerados em ordem e o real, com a UI
    if (p->despejoAtivo && p->despejoId && d->frameID == p->despejoId)
    {
        auto* c = reinterpret_cast<ID3D12GraphicsCommandList*>(d->commandList);
        if (!d->isInterpolatedFrame)
            p->CopiaParaLeitura(c, d->outputSwapChainBuffer, 9);
        else if (p->despejoTelaK < 3)
            p->CopiaParaLeitura(c, d->outputSwapChainBuffer, 6 + p->despejoTelaK++);
    }
    return e;
}

const char* Geracao::UltimoErro() const
{
    return p->erro;
}

uint64_t Geracao::Gerados() const
{
    return p->gerados;
}

uint64_t Geracao::Encontros() const
{
    return p->encontros;
}

int Geracao::EstadoContexto() const
{
    return p->estado;
}

double Geracao::MsCompilacao() const
{
    return p->msCompilacao;
}

void Geracao::Submetido(ID3D12CommandList* const* l, UINT n)
{
    std::lock_guard<std::mutex> lk(p->filaMx);
    p->comEnvio = true;
    for (UINT j = 0; j < n; ++j)
    {
        ID3D12CommandList* nativa = ListaNativa(l[j]);
        for (auto& f : p->fila)
            if (f.lista == nativa && !f.enviado)
            {
                f.enviado = ++p->ordemEnvio;
                break;
            }
    }
}

bool Geracao::SemHud(ID3D12GraphicsCommandList* c, ID3D12Resource* fonte, D3D12_RESOURCE_STATES estado, uint32_t passe)
{
    std::lock_guard<std::mutex> lk(p->mx);
    if (passe < 8 && passe + 1 > p->passesHud)
        p->passesHud = passe + 1; // quantos passes o jogo tem (a procura corre todos)
    if (!p->sc || p->estado != 2 || !p->quadro || !c || !fonte || p->faseHud == 2 || passe != p->passeHud)
        return false;
    std::lock_guard<std::mutex> lf(p->fiMx); // semHud e o estado deles tambem sao lidos por Gera (Confere)
    const D3D12_RESOURCE_DESC fd = fonte->GetDesc();
    if (fd.Width != p->telaW || fd.Height != p->telaH)
        return false;
    const int jogo = int(p->quadro % kJogos);
    auto& alvo = p->semHud[jogo];
    if (!alvo)
    {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = p->telaW;
        d.Height = p->telaH;
        d.DepthOrArraySize = d.MipLevels = 1;
        d.Format = p->fmtTela; // a mesma familia da fonte (SemHud.cpp): copia direta
        d.SampleDesc.Count = 1;
        if (FAILED(p->dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&alvo))))
            return false;
        alvo->SetName(L"geracao: imagem sem HUD");
        p->semHudEstado[jogo] = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    auto transicao = [](ID3D12Resource* r, D3D12_RESOURCE_STATES de, D3D12_RESOURCE_STATES para) {
        D3D12_RESOURCE_BARRIER b{};
        b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, de, para};
        return b;
    };
    D3D12_RESOURCE_BARRIER b[2];
    UINT nb = 0;
    if (estado != D3D12_RESOURCE_STATE_COPY_SOURCE)
        b[nb++] = transicao(fonte, estado, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (p->semHudEstado[jogo] != D3D12_RESOURCE_STATE_COPY_DEST)
        b[nb++] = transicao(alvo.Get(), p->semHudEstado[jogo], D3D12_RESOURCE_STATE_COPY_DEST);
    if (nb)
        c->ResourceBarrier(nb, b);
    D3D12_TEXTURE_COPY_LOCATION para{alvo.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION de{fonte, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    c->CopyTextureRegion(&para, 0, 0, 0, &de, nullptr);
    nb = 0;
    if (estado != D3D12_RESOURCE_STATE_COPY_SOURCE)
        b[nb++] = transicao(fonte, D3D12_RESOURCE_STATE_COPY_SOURCE, estado);
    b[nb++] = transicao(alvo.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    c->ResourceBarrier(nb, b);
    p->semHudEstado[jogo] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    std::lock_guard<std::mutex> lq(p->filaMx);
    for (auto& f : p->fila)
        if (f.id == p->quadro)
            f.semHud = true, f.passeHud = passe;
    return true;
}

void Geracao::SemHudEstado(uint32_t& passe, uint32_t& passes, double& igual, double& troca, int& fase) const
{
    passe = p->passeHud;
    passes = p->passesHud;
    igual = p->igualHud.load();
    troca = p->trocaHud.load();
    fase = p->faseHud == 1 && !p->conferidaHud ? 3 : int(p->faseHud);
}

uint32_t Geracao::Fila(double& mediaAFrente, uint64_t& maxAFrente, uint64_t& semPreparo) const
{
    std::lock_guard<std::mutex> lk(p->filaMx);
    const uint32_t n = uint32_t(p->aFrenteN);
    mediaAFrente = p->aFrenteN ? double(p->aFrenteSoma) / double(p->aFrenteN) : 0.0;
    maxAFrente = p->aFrenteMax;
    semPreparo = p->semPreparo;
    p->aFrenteSoma = p->aFrenteN = p->aFrenteMax = p->semPreparo = 0;
    return n;
}

std::string Geracao::Rastro()
{
    std::lock_guard<std::mutex> lk(p->filaMx);
    std::string r;
    r.swap(p->rastro);
    return r;
}

uint32_t Geracao::TrocasN() const
{
    return p->trocasN.exchange(0);
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

void Geracao::ApiDaAmd(const ApiAmd& api)
{
    std::lock_guard<std::mutex> lk(p->mx);
    p->api = api;
}

void Geracao::CameraDoJogo(const CameraJogo& cam)
{
    std::lock_guard<std::mutex> lk(p->filaMx);
    // o quadro mais antigo da fila ainda sem camera (o jogo a prepara depois do upscaler); sem ele, espera o proximo
    for (auto& f : p->fila)
        if (f.neural && !f.temCam)
        {
            f.qn.cam = cam, f.temCam = true;
            return;
        }
    p->cameras.push_back(cam);
    while (p->cameras.size() > size_t(kJogos))
        p->cameras.pop_front();
}

std::string Geracao::RelatoNeural()
{
    char b[320];
    if (p->neural.Pronta())
        snprintf(b, sizeof b, "geracao neural da AMD %s: %llu quadros reais por ela, %llu pela analitica%s%s",
                 p->neural.Provedor(), (unsigned long long)p->quadrosNeural, (unsigned long long)p->quadrosAnalitica,
                 p->erroNeural[0] ? " | ultimo erro: " : "", p->erroNeural);
    else
        snprintf(b, sizeof b, "geracao neural da AMD: nao (%s); %llu quadros reais pela analitica",
                 p->erroNeural[0] ? p->erroNeural : "contexto ainda nao criado", (unsigned long long)p->quadrosAnalitica);
    p->quadrosNeural = p->quadrosAnalitica = 0;
    return b;
}

double Geracao::FpsJogo() const
{
    return p->msJogo > 0 ? 1000.0 / p->msJogo : 0.0;
}

uint32_t Geracao::Ritmo(double& mediaMs, double& desvioMs, double& maxMs, double& p99Ms) const
{
    uint32_t n = 0;
    ffxDlss5RitmoDX12(&mediaMs, &desvioMs, &maxMs, &n, &p99Ms);
    return n;
}

} // namespace dlss5ov
