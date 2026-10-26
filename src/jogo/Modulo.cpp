// DLSS5Buffer.dll: o modulo que o DLSS5 Overlay coloca no jogo aberto (Modo de Rede = captura de buffer).
// Nao roda rede nenhuma: so pega, depois do upscale, o que o jogo entrega ao FSR 3.1+ (cor sem HUD, profundidade,
// movimento, exposicao, camera) e conversa com o overlay pelo ClienteBuffer (buffer/Protocolo.h). Ganchos:
//   - ffxCreateContext / ffxDispatch da DLL do upscaler (amd_fidelityfx_upscaler_dx12.dll; FSR 3.1 antigo:
//     amd_fidelityfx_dx12.dll): depois do despacho de upscale, grava os passes do cliente na mesma lista;
//   - ID3D12CommandQueue::ExecuteCommandLists: quando essa lista e submetida, sinaliza a cerca do jogo;
//   - entrada (com o painel do overlay aberto sobre o jogo): GetRawInputData/GetRawInputBuffer e as mensagens de mouse
//     da janela do jogo -- o movimento e os cliques vao para o painel e o jogo recebe zero (a mira nao mexe).
//   - geracao de quadros: o modulo tem de estar no jogo antes do DirectX. Instalado na pasta do jogo como dxgi.dll (o
//     jogo carrega a DLL sozinho em toda abertura; as funcoes da dxgi do Windows vao para ela por ProxyDxgi.asm) ou
//     aberto pelo overlay (Protocolo.h Lancamento): nas fabricas DXGI, CreateSwapChainForHwnd/CreateSwapChain -- o jogo
//     recebe o swapchain da AMD (buffer/Geracao.h), e a cada despacho do FSR a profundidade, o movimento e a camera vao
//     para a geracao.
// Registro em DLSS5Buffer.log ao lado da DLL.
#include "../buffer/ClienteBuffer.h"
#include "../buffer/Geracao.h"
#include "ffx_api.h"
#include "ffx_upscale.h"
#include <detours.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <cwctype>
#include <string>
#include <unordered_map>

namespace {

std::mutex g_mx;
dlss5ov::ClienteBuffer g_cli;
bool g_clienteOk = false, g_clienteFalhou = false;
ID3D12Device* g_dev = nullptr;
ID3D12GraphicsCommandList* g_lista = nullptr; // a lista em que o cliente gravou (sinaliza quando for submetida)
std::unordered_map<ffxContext, uint32_t> g_flags; // flags de criacao de cada contexto de upscale
// o maior tamanho de render declarado na criacao de cada contexto (as texturas do cliente cabem nele)
std::unordered_map<ffxContext, std::pair<uint32_t, uint32_t>> g_maxRender;
FILE* g_log = nullptr;
HMODULE g_eu = nullptr;

void Log(const char* f, ...)
{
    if (!g_log)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, f);
    vfprintf(g_log, f, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

PfnFfxCreateContext oCriaContexto = nullptr;
PfnFfxDispatch oDespacha = nullptr;
using PfnExecuta = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
PfnExecuta oExecuta = nullptr;
using PfnRawData = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using PfnRawBuffer = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
PfnRawData oRawData = nullptr;
PfnRawBuffer oRawBuffer = nullptr;
HHOOK g_ganchoMsg = nullptr;
volatile LONG g_rawVistos = 0;

// ---- geracao de quadros da AMD (jogo aberto pelo overlay) ----
// nunca destruida: na saida do processo o Windows ja matou a thread de apresentacao da AMD, e esperar por ela (o que o
// destrutor faz) deixava o jogo pendurado ao fechar
dlss5ov::Geracao& g_ger = *new dlss5ov::Geracao;
bool g_lancado = false;                // o modulo entrou antes do DirectX (instalado como dxgi.dll ou aberto pelo
                                       // overlay): o swapchain vira o da AMD; a geracao liga e desliga so pelo painel
bool g_proxy = false;                  // carregado como dxgi.dll da pasta do jogo
uint32_t g_geracaoLancamento = 0;      // o pedido do lancamento (vale ate o overlay conectar)
thread_local bool t_criandoSc = false; // o swapchain da AMD cria o de verdade pela mesma fabrica: esse passa direto
using PfnCriaFab = HRESULT(WINAPI*)(REFIID, void**);
using PfnCriaFab2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
PfnCriaFab oCriaFab = nullptr, oCriaFab1 = nullptr;
PfnCriaFab2 oCriaFab2 = nullptr;
using PfnCriaSc = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using PfnCriaScHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                  const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
PfnCriaSc oCriaSc = nullptr;
PfnCriaScHwnd oCriaScHwnd = nullptr;
std::once_flag g_ganchoFabrica;

// o cabecalho deste processo: o do cliente, ou (antes do primeiro FSR, ou num processo de teste que tem o proprio
// cliente) o mapeamento pelo nome
dlss5ov::Cabecalho* g_cabNome = nullptr;
dlss5ov::Cabecalho* CabSeAberto()
{
    dlss5ov::Cabecalho* c = g_clienteOk ? g_cli.Cab() : nullptr;
    if (!c)
    {
        if (!g_cabNome)
        {
            static ULONGLONG tentou = 0;
            if (GetTickCount64() - tentou > 1000)
            {
                tentou = GetTickCount64();
                wchar_t nome[96];
                dlss5ov::NomeMapa(nome, 96, GetCurrentProcessId());
                if (HANDLE m = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, nome))
                    g_cabNome = static_cast<dlss5ov::Cabecalho*>(
                        MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(dlss5ov::Cabecalho)));
            }
        }
        c = g_cabNome;
    }
    return c && c->painelAberto && c->ativo && GetTickCount64() - c->batimentoOverlay < 1000 ? c : nullptr;
}

// o movimento e os cliques vao para o painel; o jogo recebe um evento vazio
void Filtra(RAWMOUSE& m)
{
    InterlockedIncrement(&g_rawVistos);
    dlss5ov::Cabecalho* c = CabSeAberto();
    {
        // diagnostico (1 por segundo): por que o painel nao recebe
        static ULONGLONG ultimo = 0;
        const ULONGLONG agora = GetTickCount64();
        if (agora - ultimo > 1000)
        {
            ultimo = agora;
            dlss5ov::Cabecalho* b = g_clienteOk ? g_cli.Cab() : g_cabNome;
            Log("mouse bruto: cabecalho %s, painel %u, ativo %u, batimento ha %llu ms -> %s", b ? "ok" : "nenhum",
                b ? b->painelAberto : 0u, b ? b->ativo : 0u, b ? (unsigned long long)(agora - b->batimentoOverlay) : 0ull,
                c ? "para o painel" : "para o jogo");
        }
    }
    if (!c)
        return;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&c->mouseEventos));
    if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE))
    {
        InterlockedExchangeAdd(reinterpret_cast<volatile LONG*>(&c->mouseDx), m.lLastX);
        InterlockedExchangeAdd(reinterpret_cast<volatile LONG*>(&c->mouseDy), m.lLastY);
    }
    const USHORT b = m.usButtonFlags;
    const USHORT desce[3] = {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_DOWN};
    const USHORT sobe[3] = {RI_MOUSE_LEFT_BUTTON_UP, RI_MOUSE_RIGHT_BUTTON_UP, RI_MOUSE_MIDDLE_BUTTON_UP};
    for (int i = 0; i < 3; ++i)
    {
        if (b & desce[i])
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&c->botaoDesce[i]));
        if (b & sobe[i])
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&c->botaoSobe[i]));
    }
    if (b & RI_MOUSE_WHEEL)
        InterlockedExchangeAdd(reinterpret_cast<volatile LONG*>(&c->mouseRoda), short(m.usButtonData));
    m.lLastX = m.lLastY = 0;
    m.usButtonFlags = 0;
    m.usButtonData = 0;
}

UINT WINAPI RawData(HRAWINPUT h, UINT cmd, LPVOID dados, PUINT tam, UINT cab)
{
    const UINT r = oRawData(h, cmd, dados, tam, cab);
    if (cmd == RID_INPUT && dados && r != UINT(-1) && r >= sizeof(RAWINPUTHEADER))
    {
        auto* ri = static_cast<RAWINPUT*>(dados);
        if (ri->header.dwType == RIM_TYPEMOUSE)
            Filtra(ri->data.mouse);
    }
    return r;
}

UINT WINAPI RawBuffer(PRAWINPUT dados, PUINT tam, UINT cab)
{
    const UINT r = oRawBuffer(dados, tam, cab);
    if (dados && r != UINT(-1))
    {
        PRAWINPUT ri = dados;
        for (UINT i = 0; i < r; ++i)
        {
            if (ri->header.dwType == RIM_TYPEMOUSE)
                Filtra(ri->data.mouse);
            // o proximo bloco (NEXTRAWINPUTBLOCK: alinhado em 8 bytes)
            ri = reinterpret_cast<PRAWINPUT>((reinterpret_cast<ULONG_PTR>(ri) + ri->header.dwSize + 7) & ~ULONG_PTR(7));
        }
    }
    return r;
}

// mensagens de mouse da janela do jogo (jogos que usam o cursor do Windows): viram WM_NULL com o painel aberto
LRESULT CALLBACK GanchoMensagem(int code, WPARAM w, LPARAM l)
{
    if (code == HC_ACTION && w == PM_REMOVE)
    {
        auto* m = reinterpret_cast<MSG*>(l);
        if (m->message >= WM_MOUSEFIRST && m->message <= WM_MOUSELAST && CabSeAberto())
            m->message = WM_NULL;
    }
    return CallNextHookEx(nullptr, code, w, l);
}

BOOL CALLBACK JanelaDoJogo(HWND h, LPARAM l)
{
    DWORD pid = 0;
    const DWORD t = GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !GetWindow(h, GW_OWNER))
    {
        *reinterpret_cast<DWORD*>(l) = t;
        return FALSE;
    }
    return TRUE;
}

void GanchoMensagens()
{
    if (g_ganchoMsg)
        return;
    DWORD t = 0;
    EnumWindows(JanelaDoJogo, reinterpret_cast<LPARAM>(&t));
    if (t)
    {
        g_ganchoMsg = SetWindowsHookExW(WH_GETMESSAGE, GanchoMensagem, g_eu, t);
        Log("gancho nas mensagens de mouse da janela do jogo (thread %lu): %s", t, g_ganchoMsg ? "ok" : "falhou");
    }
}

void GanchosEntrada()
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    oRawData = reinterpret_cast<PfnRawData>(GetProcAddress(u, "GetRawInputData"));
    oRawBuffer = reinterpret_cast<PfnRawBuffer>(GetProcAddress(u, "GetRawInputBuffer"));
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&oRawData), reinterpret_cast<PVOID>(&RawData));
    DetourAttach(reinterpret_cast<PVOID*>(&oRawBuffer), reinterpret_cast<PVOID>(&RawBuffer));
    const LONG r = DetourTransactionCommit();
    GanchoMensagens();
    // que entrada o jogo usa (o mouse por GameInput nao passa pela entrada bruta)
    RAWINPUTDEVICE d[16];
    UINT n = 16;
    std::string reg;
    const UINT k = GetRegisteredRawInputDevices(d, &n, sizeof(RAWINPUTDEVICE));
    for (UINT i = 0; i < k && k != UINT(-1); ++i)
        reg += " " + std::to_string(d[i].usUsagePage) + "/" + std::to_string(d[i].usUsage) + " flags 0x" +
               [](DWORD f) { char b[16]; sprintf_s(b, "%lx", f); return std::string(b); }(d[i].dwFlags);
    Log("ganchos de entrada: bruta %s; entrada bruta registrada pelo jogo:%s; GameInput %s",
        r == NO_ERROR ? "ok" : "falhou", reg.empty() ? " (nenhuma ainda)" : reg.c_str(),
        GetModuleHandleW(L"GameInput.dll") || GetModuleHandleW(L"GameInputRedist.dll") ? "carregado" : "ausente");
}

D3D12_RESOURCE_STATES Estado(uint32_t s)
{
    if (s & FFX_API_RESOURCE_STATE_UNORDERED_ACCESS)
        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (s & FFX_API_RESOURCE_STATE_RENDER_TARGET)
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (s & FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT)
        return D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (s == FFX_API_RESOURCE_STATE_GENERIC_READ)
        return D3D12_RESOURCE_STATE_GENERIC_READ;
    if (s & FFX_API_RESOURCE_STATE_COPY_SRC)
        return D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (s & FFX_API_RESOURCE_STATE_COPY_DEST)
        return D3D12_RESOURCE_STATE_COPY_DEST;
    if (s & FFX_API_RESOURCE_STATE_PRESENT)
        return D3D12_RESOURCE_STATE_PRESENT;
    D3D12_RESOURCE_STATES r = D3D12_RESOURCE_STATE_COMMON;
    if (s & FFX_API_RESOURCE_STATE_COMPUTE_READ)
        r |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (s & FFX_API_RESOURCE_STATE_PIXEL_READ)
        r |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    return r;
}

bool Float(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R11G11B10_FLOAT || f == DXGI_FORMAT_R32G32B32A32_FLOAT ||
           f == DXGI_FORMAT_R16G16B16A16_TYPELESS || f == DXGI_FORMAT_R32G32B32A32_TYPELESS || f == DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
}

void STDMETHODCALLTYPE Executa(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* l)
{
    oExecuta(q, n, l);
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_lista)
        return;
    for (UINT i = 0; i < n; ++i)
        if (l[i] == g_lista)
        {
            g_cli.Submetido(q); // a lista que tem os passes deste quadro foi para a fila: sinaliza a cerca do jogo
            g_lista = nullptr;
            break;
        }
}

// ExecuteCommandLists e o mesmo codigo para toda fila do d3d12.dll: o gancho vai no endereco, nao na vtable
void GanchoFila(ID3D12Device* d)
{
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> q;
    if (FAILED(d->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))))
    {
        Log("nao criou a fila de amostra");
        return;
    }
    void** vt = *reinterpret_cast<void***>(q.Get());
    oExecuta = reinterpret_cast<PfnExecuta>(vt[10]);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&oExecuta), reinterpret_cast<PVOID>(&Executa));
    const LONG r = DetourTransactionCommit();
    Log("gancho em ExecuteCommandLists: %s", r == NO_ERROR ? "ok" : "falhou");
}

// ---- geracao de quadros ----
// o swapchain da AMD vai para esta criacao: o primeiro do jogo, ou um novo depois que o jogo soltou o anterior (troca de
// modo de tela, HDR...) -- o antigo, preso so pelo modulo, sai antes; vivo, ele ocupa a janela e a criacao do jogo falha
// (E_ACCESSDENIED: o "problema com o driver de video" do TLOU2). Com o anterior ainda em uso o novo segue sem geracao
bool LiberaParaCriar()
{
    if (!g_ger.TemSwapchain())
        return true;
    if (!g_ger.SwapchainSolto())
        return false;
    g_ger.SoltaSwapchain();
    Log("geracao: o jogo soltou o swapchain anterior (troca de modo de tela?); o novo tambem vai ser o da AMD");
    return true;
}

HRESULT STDMETHODCALLTYPE CriaScHwnd(IDXGIFactory2* f, IUnknown* d, HWND h, const DXGI_SWAP_CHAIN_DESC1* sd,
                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** sc)
{
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> fila;
    if (!t_criandoSc && g_lancado && d && sd && SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&fila))) && LiberaParaCriar())
    {
        t_criandoSc = true;
        const HRESULT r = g_ger.CriaSwapchain(f, fila.Get(), h, sd, fs, sc);
        t_criandoSc = false;
        if (SUCCEEDED(r))
        {
            Log("geracao: o jogo recebeu o swapchain da AMD (%ux%u fmt %d, %u buffers)", sd->Width, sd->Height,
                (int)sd->Format, sd->BufferCount);
            return r;
        }
        Log("geracao: o swapchain da AMD falhou (%s); o jogo segue sem geracao", g_ger.UltimoErro());
    }
    return oCriaScHwnd(f, d, h, sd, fs, o, sc);
}

HRESULT STDMETHODCALLTYPE CriaSc(IDXGIFactory* f, IUnknown* d, DXGI_SWAP_CHAIN_DESC* sd, IDXGISwapChain** sc)
{
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> fila;
    if (!t_criandoSc && g_lancado && d && sd && SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&fila))) && LiberaParaCriar())
    {
        t_criandoSc = true;
        const HRESULT r = g_ger.CriaSwapchain(f, fila.Get(), sd, sc);
        t_criandoSc = false;
        if (SUCCEEDED(r))
        {
            Log("geracao: o jogo recebeu o swapchain da AMD (%ux%u fmt %d, %u buffers)", sd->BufferDesc.Width,
                sd->BufferDesc.Height, (int)sd->BufferDesc.Format, sd->BufferCount);
            return r;
        }
        Log("geracao: o swapchain da AMD falhou (%s); o jogo segue sem geracao", g_ger.UltimoErro());
    }
    return oCriaSc(f, d, sd, sc);
}

// as funcoes de criar swapchain sao o mesmo codigo para toda fabrica do dxgi.dll: o gancho vai no endereco
void GanchoFabrica(IUnknown* fab)
{
    std::call_once(g_ganchoFabrica, [&] {
        oCriaSc = reinterpret_cast<PfnCriaSc>((*reinterpret_cast<void***>(fab))[10]);
        Microsoft::WRL::ComPtr<IDXGIFactory2> f2;
        if (SUCCEEDED(fab->QueryInterface(IID_PPV_ARGS(&f2))))
            oCriaScHwnd = reinterpret_cast<PfnCriaScHwnd>((*reinterpret_cast<void***>(f2.Get()))[15]);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(reinterpret_cast<PVOID*>(&oCriaSc), reinterpret_cast<PVOID>(&CriaSc));
        if (oCriaScHwnd)
            DetourAttach(reinterpret_cast<PVOID*>(&oCriaScHwnd), reinterpret_cast<PVOID>(&CriaScHwnd));
        const LONG r = DetourTransactionCommit();
        Log("geracao: ganchos na criacao do swapchain: %s", r == NO_ERROR ? "ok" : "falharam");
    });
}

HRESULT WINAPI CriaFab(REFIID r, void** f)
{
    const HRESULT h = oCriaFab(r, f);
    if (SUCCEEDED(h) && f && *f)
        GanchoFabrica(static_cast<IUnknown*>(*f));
    return h;
}
HRESULT WINAPI CriaFab1(REFIID r, void** f)
{
    const HRESULT h = oCriaFab1(r, f);
    if (SUCCEEDED(h) && f && *f)
        GanchoFabrica(static_cast<IUnknown*>(*f));
    return h;
}
HRESULT WINAPI CriaFab2(UINT fl, REFIID r, void** f)
{
    const HRESULT h = oCriaFab2(fl, r, f);
    if (SUCCEEDED(h) && f && *f)
        GanchoFabrica(static_cast<IUnknown*>(*f));
    return h;
}

// o jogo nasceu suspenso: engancha a criacao das fabricas DXGI antes de ele criar qualquer uma e avisa o overlay, que
// entao solta o jogo
DWORD WINAPI PreparaGeracao(void*)
{
    if (HMODULE dxgi = LoadLibraryW(L"dxgi.dll"))
    {
        oCriaFab = reinterpret_cast<PfnCriaFab>(GetProcAddress(dxgi, "CreateDXGIFactory"));
        oCriaFab1 = reinterpret_cast<PfnCriaFab>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
        oCriaFab2 = reinterpret_cast<PfnCriaFab2>(GetProcAddress(dxgi, "CreateDXGIFactory2"));
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        if (oCriaFab)
            DetourAttach(reinterpret_cast<PVOID*>(&oCriaFab), reinterpret_cast<PVOID>(&CriaFab));
        if (oCriaFab1)
            DetourAttach(reinterpret_cast<PVOID*>(&oCriaFab1), reinterpret_cast<PVOID>(&CriaFab1));
        if (oCriaFab2)
            DetourAttach(reinterpret_cast<PVOID*>(&oCriaFab2), reinterpret_cast<PVOID>(&CriaFab2));
        const LONG r = DetourTransactionCommit();
        Log("jogo aberto pelo overlay (geracao %s no lancamento): ganchos nas fabricas DXGI %s",
            g_geracaoLancamento >= 2 ? "ligada" : "desligada", r == NO_ERROR ? "ok" : "falharam");
    }
    else
        Log("geracao: dxgi.dll nao carregou");
    wchar_t nome[96];
    dlss5ov::NomeEventoPronto(nome, 96, GetCurrentProcessId());
    if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, nome))
    {
        SetEvent(ev);
        CloseHandle(ev);
    }
    return 0;
}

// a cada despacho do FSR: a profundidade, o movimento e a camera deste quadro para a geracao (ela parte do backbuffer
// final, que ja tem a edicao da rede)
uint64_t g_quadrosGeracao = 0; // quadros do jogo que passaram pela geracao (diagnostico)

void GeracaoQuadro(ffxContext* ctx, const ffxDispatchDescUpscale* u, ID3D12GraphicsCommandList* lista)
{
    if (!g_ger.TemSwapchain())
        return;
    uint32_t flags = 0, mrw = 0, mrh = 0;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (ctx)
        {
            if (auto it = g_flags.find(*ctx); it != g_flags.end())
                flags = it->second;
            if (auto im = g_maxRender.find(*ctx); im != g_maxRender.end())
                mrw = im->second.first, mrh = im->second.second;
        }
    }
    const bool conhecidas = (flags & 0x80000000u) != 0;
    dlss5ov::QuadroGeracao g;
    g.prof = static_cast<ID3D12Resource*>(u->depth.resource);
    g.profEstado = Estado(u->depth.state);
    g.mov = static_cast<ID3D12Resource*>(u->motionVectors.resource);
    g.movEstado = Estado(u->motionVectors.state);
    g.renderW = u->renderSize.width;
    g.renderH = u->renderSize.height;
    g.maxRenderW = mrw;
    g.maxRenderH = mrh;
    g.movNaTela = (flags & FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) != 0;
    g.movComJitter = (flags & FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION) != 0;
    g.jitterX = u->jitterOffset.x;
    g.jitterY = u->jitterOffset.y;
    g.movEscX = u->motionVectorScale.x;
    g.movEscY = u->motionVectorScale.y;
    g.perto = u->cameraNear;
    g.longe = u->cameraFar;
    g.fovY = u->cameraFovAngleVertical;
    g.msQuadro = u->frameTimeDelta > 0.f ? u->frameTimeDelta : 16.6f;
    // sem as flags (o modulo chegou depois do contexto): o padrao do UE5, Z invertido e infinito
    g.invertida = conhecidas ? (flags & FFX_UPSCALE_ENABLE_DEPTH_INVERTED) != 0 : true;
    g.infinita = conhecidas ? (flags & FFX_UPSCALE_ENABLE_DEPTH_INFINITE) != 0
                            : (u->cameraFar <= 0.f || u->cameraFar > 1e6f || u->cameraFar < u->cameraNear);
    g.reset = u->reset;
    // o pedido do overlay conectado; sem ele (ainda nao ligou a captura, ou fechou), o do lancamento
    dlss5ov::Cabecalho* cab = g_clienteOk ? g_cli.Cab() : nullptr;
    const uint32_t pedida = cab && g_cli.OverlayVivo() ? cab->geracaoQuadros : g_geracaoLancamento;
    if (cab)
        cab->geracaoPronta = 1;
    static int estadoDito = 0;
    g_ger.Quadro(lista, g, std::min<uint32_t>(pedida, 4));
    ++g_quadrosGeracao;
    if (cab)
    {
        cab->geracaoEmUso = pedida >= 2 ? g_ger.EmUso() : 0;
        cab->hzMonitor = g_ger.Hz();
        cab->hzMaximo = g_ger.HzMaximo();
        cab->fpsJogo10 = uint32_t(g_ger.FpsJogo() * 10.0 + 0.5);
    }
    if (const int est = g_ger.EstadoContexto(); est != estadoDito && est >= 1)
    {
        estadoDito = est;
        if (est == 1)
            Log("geracao: compilando os shaders da AMD em segundo plano (o jogo segue sem geracao ate terminar)");
        else if (est == 2)
            Log("geracao: pronta (shaders da AMD compilados em %.1f s)", g_ger.MsCompilacao() / 1000.0);
        else
            Log("geracao: falhou: %s", g_ger.UltimoErro());
    }
    static ULONGLONG marca = 0;
    static uint64_t antes = 0, quadrosAntes = 0;
    const ULONGLONG agora = GetTickCount64();
    if (!marca)
        marca = agora;
    if (agora - marca >= 5000)
    {
        const double s = double(agora - marca) / 1000.0, jogo = double(g_quadrosGeracao - quadrosAntes) / s,
                     gerados = double(g_ger.Gerados() - antes) / s;
        double media = 0, desvio = 0, maior = 0;
        const uint32_t n = g_ger.Ritmo(media, desvio, maior);
        Log("geracao: pedida %ux, em uso %ux (monitor %u Hz, aceita ate %u), jogo %.0f quadros/s + %.0f gerados/s = %.0f "
            "apresentados/s | ritmo: %u apresentacoes, a cada %.2f ms (desvio %.2f, maior %.2f)",
            pedida >= 2 ? std::min<uint32_t>(pedida, 4) : 1, pedida >= 2 ? g_ger.EmUso() : 1, g_ger.Hz(), g_ger.HzMaximo(),
            jogo, gerados, jogo + gerados, n, media, desvio, maior);
        antes = g_ger.Gerados();
        quadrosAntes = g_quadrosGeracao;
        marca = agora;
    }
}

ffxReturnCode_t CriaContexto(ffxContext* ctx, ffxCreateContextDescHeader* desc, const ffxAllocationCallbacks* mem)
{
    const ffxReturnCode_t r = oCriaContexto(ctx, desc, mem);
    if (r == FFX_API_RETURN_OK && ctx)
        for (auto* h = desc; h; h = h->pNext)
            if (h->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE)
            {
                const auto* u = reinterpret_cast<const ffxCreateContextDescUpscale*>(h);
                std::lock_guard<std::mutex> lk(g_mx);
                g_flags[*ctx] = u->flags | 0x80000000u; // bit alto: flags conhecidas
                g_maxRender[*ctx] = {u->maxRenderSize.width, u->maxRenderSize.height};
                Log("contexto de upscale criado: flags 0x%x, render ate %ux%u, saida %ux%u", u->flags,
                    u->maxRenderSize.width, u->maxRenderSize.height, u->maxUpscaleSize.width, u->maxUpscaleSize.height);
            }
    return r;
}

ffxReturnCode_t Despacha(ffxContext* ctx, const ffxDispatchDescHeader* desc)
{
    const ffxReturnCode_t r = oDespacha(ctx, desc);
    if (r != FFX_API_RETURN_OK || !desc || desc->type != FFX_API_DISPATCH_DESC_TYPE_UPSCALE)
        return r;
    const auto* u = reinterpret_cast<const ffxDispatchDescUpscale*>(desc);
    auto* lista = static_cast<ID3D12GraphicsCommandList*>(u->commandList);
    auto* cor = static_cast<ID3D12Resource*>(u->output.resource);
    auto* mov = static_cast<ID3D12Resource*>(u->motionVectors.resource);
    auto* prof = static_cast<ID3D12Resource*>(u->depth.resource);
    if (!lista || !cor || !mov || !prof)
        return r;
    GeracaoQuadro(ctx, u, lista);
    if (g_clienteOk)
        g_cli.EsperaEdicao(g_mx); // sem atraso: a edicao do quadro anterior, sem segurar o mutex (o Execute usa)
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_clienteOk)
    {
        if (g_clienteFalhou)
            return r;
        if (FAILED(lista->GetDevice(IID_PPV_ARGS(&g_dev))))
            return r;
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        const char* nome = strrchr(exe, '\\') ? strrchr(exe, '\\') + 1 : exe;
        g_clienteOk = g_cli.Inicia(g_dev, nome);
        if (!g_clienteOk && dlss5ov::ClienteBuffer::Compilando())
        {
            g_dev->Release(); // GetDevice acima: refaz no proximo quadro
            g_dev = nullptr;
            static bool dito = false;
            if (!dito)
                Log("cliente: os shaders do modulo ainda compilam em segundo plano; o jogo segue e o cliente liga quando terminar");
            dito = true;
            return r;
        }
        g_clienteFalhou = !g_clienteOk;
        Log(g_clienteOk ? "cliente pronto (%s)" : "cliente falhou: %s", g_clienteOk ? nome : g_cli.UltimoErro());
        if (!g_clienteOk)
            return r;
        GanchoFila(g_dev);
    }
    uint32_t flags = 0, maxRenderW = 0, maxRenderH = 0;
    if (ctx)
    {
        auto it = g_flags.find(*ctx);
        if (it != g_flags.end())
            flags = it->second;
        auto im = g_maxRender.find(*ctx);
        if (im != g_maxRender.end())
            maxRenderW = im->second.first, maxRenderH = im->second.second;
    }
    const bool conhecidas = (flags & 0x80000000u) != 0;
    dlss5ov::EntradasJogo e;
    e.cor = cor;
    e.corEstado = Estado(u->output.state);
    e.corValidoW = u->upscaleSize.width;
    e.corValidoH = u->upscaleSize.height;
    e.mov = mov;
    e.movEstado = Estado(u->motionVectors.state);
    e.movEscalaX = u->motionVectorScale.x;
    e.movEscalaY = u->motionVectorScale.y;
    const bool movSaida = (flags & FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) != 0;
    e.movEspacoW = movSaida ? u->upscaleSize.width : u->renderSize.width;
    e.movEspacoH = movSaida ? u->upscaleSize.height : u->renderSize.height;
    e.movValidoW = e.movEspacoW;
    e.movValidoH = e.movEspacoH;
    e.prof = prof;
    e.profEstado = Estado(u->depth.state);
    e.profValidoW = u->renderSize.width;
    e.profValidoH = u->renderSize.height;
    e.maxRenderW = maxRenderW;
    e.maxRenderH = maxRenderH;
    e.exposicao = static_cast<ID3D12Resource*>(u->exposure.resource);
    e.expEstado = Estado(u->exposure.state);
    e.preExposicao = u->preExposure > 0 ? u->preExposure : 1.f;
    e.jitterX = u->jitterOffset.x;
    e.jitterY = u->jitterOffset.y;
    e.perto = u->cameraNear;
    e.longe = u->cameraFar;
    e.fovY = u->cameraFovAngleVertical;
    e.reset = u->reset;
    // sem as flags (o modulo chegou depois de o jogo criar o contexto): o padrao do UE5 (Z invertido e infinito) e a
    // cor em ponto flutuante = HDR linear
    e.profInvertida = (flags & FFX_UPSCALE_ENABLE_DEPTH_INVERTED) != 0;
    e.profInvertidaConhecida = conhecidas;
    e.profInfinita = conhecidas ? (flags & FFX_UPSCALE_ENABLE_DEPTH_INFINITE) != 0
                                : (u->cameraFar <= 0.f || u->cameraFar > 1e6f || u->cameraFar < u->cameraNear);
    e.hdr = conhecidas ? (flags & FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE) != 0 : Float(cor->GetDesc().Format);
    if (g_cli.Quadro(lista, e))
        g_lista = lista;
    {
        // diagnostico (a cada 5 s): o que o jogo de verdade faz e o teste nao tem -- despachos por segundo, tamanho de render
        // variando (resolucao dinamica), resets, jitter, a saida trocando de textura e a idade da edicao aplicada
        static ULONGLONG marca = 0;
        static uint64_t despachos = 0, resets = 0, trocasSaida = 0;
        static uint32_t rwMin = ~0u, rwMax = 0, rhMin = ~0u, rhMax = 0;
        static float jMax = 0.f;
        static ID3D12Resource* ultimaSaida = nullptr;
        ++despachos;
        resets += e.reset ? 1 : 0;
        trocasSaida += cor != ultimaSaida ? 1 : 0;
        ultimaSaida = cor;
        rwMin = std::min(rwMin, u->renderSize.width), rwMax = std::max(rwMax, u->renderSize.width);
        rhMin = std::min(rhMin, u->renderSize.height), rhMax = std::max(rhMax, u->renderSize.height);
        jMax = std::max(jMax, std::max(std::fabs(e.jitterX), std::fabs(e.jitterY)));
        const ULONGLONG agora = GetTickCount64();
        if (!marca)
            marca = agora;
        if (agora - marca >= 5000)
        {
            const auto& d = g_cli.diag;
            Log("FSR em %.1f s: %llu despachos (%.0f/s), render %ux%u..%ux%u, resets %llu, saida trocou %llu vezes, jitter max "
                "%.2f | cliente: %llu quadros, %llu exportados, %llu com edicao (idade media %.1f, max %llu quadros) | "
                "sem atraso: %llu esperas, media %.1f ms, max %.1f ms, %llu estouradas",
                (agora - marca) / 1000.0, (unsigned long long)despachos, despachos * 1000.0 / double(agora - marca), rwMin,
                rhMin, rwMax, rhMax, (unsigned long long)resets, (unsigned long long)trocasSaida, jMax,
                (unsigned long long)d.quadros, (unsigned long long)d.exportados, (unsigned long long)d.aplicados,
                d.aplicados ? double(d.idadeSoma) / double(d.aplicados) : 0.0, (unsigned long long)d.idadeMax,
                (unsigned long long)d.esperas, d.esperas ? d.esperaMs / double(d.esperas) : 0.0, d.esperaMsMax,
                (unsigned long long)d.estouros);
            g_cli.diag = {};
            despachos = resets = trocasSaida = 0;
            rwMin = rhMin = ~0u, rwMax = rhMax = 0;
            jMax = 0.f;
            marca = agora;
        }
    }
    {
        // diagnostico do mouse: quantos eventos brutos o jogo leu (e quantos foram para o painel)
        static ULONGLONG ultimo = 0;
        static LONG vistos = 0;
        const ULONGLONG agora = GetTickCount64();
        dlss5ov::Cabecalho* cb = g_cli.Cab();
        if (cb && agora - ultimo > 5000 && (g_rawVistos != vistos || cb->painelAberto))
        {
            Log("mouse: %ld eventos brutos lidos pelo jogo em 5 s; painel %s; %lu para o painel no total",
                g_rawVistos - vistos, cb->painelAberto ? "aberto" : "fechado", (unsigned long)cb->mouseEventos);
            vistos = g_rawVistos;
            ultimo = agora;
        }
    }
    static bool primeiro = true;
    if (primeiro)
    {
        primeiro = false;
        const auto dc = cor->GetDesc();
        Log("primeiro quadro: saida %llux%u fmt %d, render %ux%u, movimento escala %.3f,%.3f, perto %.3f longe %.3g, "
            "fov vertical %.1f graus%s, flags %s 0x%x", (unsigned long long)dc.Width, dc.Height, (int)dc.Format,
            u->renderSize.width, u->renderSize.height, u->motionVectorScale.x, u->motionVectorScale.y, u->cameraNear,
            u->cameraFar, u->cameraFovAngleVertical * 57.29578f,
            dlss5ov::TanMeioFov(u->cameraFovAngleVertical) > 0.f ? " (distancia radial)" : " (desconhecido: Z da camera)",
            conhecidas ? "conhecidas" : "desconhecidas (padrao do UE5)", flags & 0x7fffffffu);
    }
    return r;
}

// procura a DLL do upscaler do jogo (pode carregar depois: o jogo so carrega o FSR quando ele e escolhido no menu)
DWORD WINAPI Instala(void*)
{
    // os shaders do modulo compilam enquanto o jogo carrega (no primeiro quadro do FSR parariam o jogo por 14-25 s)
    dlss5ov::ClienteBuffer::PreCompila();
    // entrada primeiro: o painel do overlay pode abrir antes de o jogo usar o FSR
    GanchosEntrada();
    // FSR SDK 2 (FSR 3.1.4+/4): o jogo chama o carregador, que repassa ao upscaler -- e o upgrade de FSR do driver troca o
    // upscaler do jogo pelo dele por ali (Cyberpunk: o gancho no upscaler do jogo nunca era chamado). So um deles e
    // enganchado (os dois duplicariam os passes): o carregador primeiro, depois o upscaler direto e o FSR 3.1 antigo
    const wchar_t* candidatos[] = {L"amd_fidelityfx_loader_dx12.dll", L"amd_fidelityfx_upscaler_dx12.dll",
                                   L"amd_fidelityfx_dx12.dll"};
    for (int volta = 0;; ++volta)
    {
        GanchoMensagens(); // a janela do jogo pode aparecer depois
        for (const wchar_t* n : candidatos)
            if (HMODULE m = GetModuleHandleW(n))
            {
                oCriaContexto = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(m, "ffxCreateContext"));
                oDespacha = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(m, "ffxDispatch"));
                if (!oCriaContexto || !oDespacha)
                    continue;
                DetourTransactionBegin();
                DetourUpdateThread(GetCurrentThread());
                DetourAttach(reinterpret_cast<PVOID*>(&oCriaContexto), reinterpret_cast<PVOID>(&CriaContexto));
                DetourAttach(reinterpret_cast<PVOID*>(&oDespacha), reinterpret_cast<PVOID>(&Despacha));
                const LONG r = DetourTransactionCommit();
                char nome[MAX_PATH];
                WideCharToMultiByte(CP_UTF8, 0, n, -1, nome, MAX_PATH, nullptr, nullptr);
                Log("ganchos em %s: %s", nome, r == NO_ERROR ? "ok" : "falharam");
                return 0;
            }
        if (volta == 0)
            Log("o jogo ainda nao carregou o FSR (escolha FSR no menu grafico); esperando...");
        Sleep(500);
    }
}

} // namespace

// ---- instalada na pasta do jogo como dxgi.dll: as funcoes da dxgi do Windows ----
extern "C" void* g_dxgiFn[17]; // ProxyDxgi.asm
namespace {
const char* const kDxgiNomes[17] = {"ApplyCompatResolutionQuirking", "CompatString", "CompatValue", "DXGID3D10CreateDevice",
                                    "DXGID3D10CreateLayeredDevice", "DXGID3D10GetLayeredDeviceSize",
                                    "DXGID3D10RegisterLayers", "DXGIDeclareAdapterRemovalSupport",
                                    "DXGIDisableVBlankVirtualization", "DXGIDumpJournal", "DXGIGetDebugInterface1",
                                    "DXGIReportAdapterConfiguration", "PIXBeginCapture", "PIXEndCapture",
                                    "PIXGetCaptureState", "SetAppCompatStringPointer", "UpdateHMDEmulationStatus"};
PfnCriaFab oSisFab = nullptr, oSisFab1 = nullptr;
PfnCriaFab2 oSisFab2 = nullptr;
std::once_flag g_resolveDxgi;

// Na pasta do jogo a dxgi.dll e carregada por todo programa dali (o relatorio de erros, o lancador...). O modulo so age
// no executavel em que o overlay a instalou (DLSS5Overlay.alvo ao lado dela, um executavel por linha); sem a lista, em
// todos menos os auxiliares conhecidos. Nos outros ela so repassa a dxgi do Windows, sem ganchos nem log
bool EhOJogo(const wchar_t* dll)
{
    wchar_t eu[MAX_PATH];
    GetModuleFileNameW(nullptr, eu, MAX_PATH);
    const wchar_t* b = wcsrchr(eu, L'\\');
    const std::wstring exe = b ? b + 1 : eu;
    std::wstring alvo(dll);
    alvo = alvo.substr(0, alvo.find_last_of(L"\\/")) + L"\\DLSS5Overlay.alvo";
    if (FILE* f = _wfopen(alvo.c_str(), L"r"))
    {
        bool achou = false;
        char linha[MAX_PATH];
        while (!achou && fgets(linha, sizeof linha, f))
        {
            std::string l(linha);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' '))
                l.pop_back();
            wchar_t w[MAX_PATH] = {};
            MultiByteToWideChar(CP_UTF8, 0, l.c_str(), -1, w, MAX_PATH);
            achou = !l.empty() && !_wcsicmp(w, exe.c_str());
        }
        fclose(f);
        return achou;
    }
    std::wstring e = exe;
    for (auto& ch : e)
        ch = towlower(ch);
    for (const wchar_t* x : {L"crs-", L"crash", L"report", L"launcher", L"unins", L"setup", L"redist", L"updater", L"helper"})
        if (e.find(x) != std::wstring::npos)
            return false;
    return true;
}
} // namespace

extern "C" void ResolveDxgiSistema()
{
    std::call_once(g_resolveDxgi, [] {
        wchar_t p[MAX_PATH];
        GetSystemDirectoryW(p, MAX_PATH);
        wcscat_s(p, L"\\dxgi.dll");
        HMODULE d = LoadLibraryW(p);
        if (!d)
        {
            Log("dxgi do Windows nao carregou");
            return;
        }
        for (int i = 0; i < 17; ++i)
            g_dxgiFn[i] = reinterpret_cast<void*>(GetProcAddress(d, kDxgiNomes[i]));
        oSisFab = reinterpret_cast<PfnCriaFab>(GetProcAddress(d, "CreateDXGIFactory"));
        oSisFab1 = reinterpret_cast<PfnCriaFab>(GetProcAddress(d, "CreateDXGIFactory1"));
        oSisFab2 = reinterpret_cast<PfnCriaFab2>(GetProcAddress(d, "CreateDXGIFactory2"));
    });
}

// as fabricas do jogo passam pelo modulo: a criacao do swapchain e enganchada nelas
extern "C" HRESULT WINAPI PxCreateDXGIFactory(REFIID r, void** f)
{
    ResolveDxgiSistema();
    const HRESULT h = oSisFab ? oSisFab(r, f) : E_NOTIMPL;
    if (SUCCEEDED(h) && f && *f && g_lancado)
        GanchoFabrica(static_cast<IUnknown*>(*f));
    return h;
}
extern "C" HRESULT WINAPI PxCreateDXGIFactory1(REFIID r, void** f)
{
    ResolveDxgiSistema();
    const HRESULT h = oSisFab1 ? oSisFab1(r, f) : E_NOTIMPL;
    if (SUCCEEDED(h) && f && *f && g_lancado)
        GanchoFabrica(static_cast<IUnknown*>(*f));
    return h;
}
extern "C" HRESULT WINAPI PxCreateDXGIFactory2(UINT fl, REFIID r, void** f)
{
    ResolveDxgiSistema();
    const HRESULT h = oSisFab2 ? oSisFab2(fl, r, f) : E_NOTIMPL;
    if (SUCCEEDED(h) && f && *f && g_lancado)
        GanchoFabrica(static_cast<IUnknown*>(*f));
    return h;
}

// marcador: o overlay reconhece a dxgi.dll instalada por ele (e a versao do protocolo dela)
extern "C" uint32_t DLSS5Ov_Modulo()
{
    return dlss5ov::kVersao;
}

BOOL APIENTRY DllMain(HMODULE m, DWORD motivo, LPVOID)
{
    if (motivo == DLL_PROCESS_ATTACH)
    {
        g_eu = m;
        DisableThreadLibraryCalls(m);
        wchar_t p[MAX_PATH];
        GetModuleFileNameW(m, p, MAX_PATH);
        const wchar_t* base = wcsrchr(p, L'\\');
        g_proxy = base && !_wcsicmp(base + 1, L"dxgi.dll");
        if (g_proxy && !EhOJogo(p))
            return TRUE; // outro programa da pasta do jogo: so a dxgi do Windows (ProxyDxgi.asm)
        std::wstring s(p);
        s = s.substr(0, s.find_last_of(L"\\/")) + L"\\DLSS5Buffer.log";
        g_log = _wfsopen(s.c_str(), L"w", 0x40 /* _SH_DENYNO */);
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        // uma copia so por processo (instalada como dxgi.dll e depois injetada, por exemplo): a segunda nao faz nada
        wchar_t nome[96];
        dlss5ov::NomeModulo(nome, 96, GetCurrentProcessId());
        CreateEventW(nullptr, TRUE, FALSE, nome); // fica aberto ate o processo sair
        if (GetLastError() == ERROR_ALREADY_EXISTS)
            return TRUE;
        Log("DLSS5Buffer.dll carregada em %s (pid %lu)%s", exe, GetCurrentProcessId(),
            g_proxy ? " como dxgi.dll da pasta do jogo" : "");
        if (g_proxy)
        {
            // instalada no jogo: entrou antes do DirectX, a geracao funciona sem abrir pelo overlay. O pedido vem do
            // painel quando o overlay conecta (DLSS5_GERACAO, so para testes, vale antes disso)
            g_lancado = true;
            if (const wchar_t* t = _wgetenv(L"DLSS5_GERACAO"))
                g_geracaoLancamento = uint32_t(_wtoi(t));
        }
        // aberto pelo overlay (o jogo esta suspenso esperando o evento pronto): o pedido de geracao de quadros
        dlss5ov::NomeLancamento(nome, 96, GetCurrentProcessId());
        bool lancado = false;
        if (HANDLE mp = OpenFileMappingW(FILE_MAP_READ, FALSE, nome))
        {
            if (const auto* l = static_cast<const dlss5ov::Lancamento*>(MapViewOfFile(mp, FILE_MAP_READ, 0, 0,
                                                                                         sizeof(dlss5ov::Lancamento))))
            {
                lancado = g_lancado = true;
                if (l->versao == dlss5ov::kVersao)
                    g_geracaoLancamento = l->geracao;
                else
                    Log("lancamento de outra versao do overlay (%u, esta %u): sem geracao", l->versao, dlss5ov::kVersao);
                UnmapViewOfFile(l);
            }
            CloseHandle(mp);
        }
        if (lancado && !g_proxy) // como dxgi.dll as fabricas ja passam pelo modulo (PxCreateDXGIFactory*)
            if (HANDLE t = CreateThread(nullptr, 0, PreparaGeracao, nullptr, 0, nullptr))
                CloseHandle(t);
        if (HANDLE t = CreateThread(nullptr, 0, Instala, nullptr, 0, nullptr))
            CloseHandle(t);
    }
    return TRUE;
}
