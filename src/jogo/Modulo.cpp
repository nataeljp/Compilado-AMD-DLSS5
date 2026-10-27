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
//     para a geracao; depois do despacho, a imagem sem HUD (SemHud.h) tambem, para o HUD ficar parado nos gerados.
// Registro em DLSS5Buffer.log ao lado da DLL.
#include "../buffer/ClienteBuffer.h"
#include "../buffer/Geracao.h"
#include "SemHud.h"
#include "ffx_api.h"
#include "ffx_upscale.h"
#include <detours.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <cwctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

std::mutex g_mx;
// nunca destruido: instalada como dxgi.dll esta DLL carrega antes do d3d12 e sai depois dele na saida do processo -- o
// destrutor soltava texturas e cercas num driver que ja saiu (amdxc64.dll abortava o processo, 0xc0000409)
dlss5ov::ClienteBuffer& g_cli = *new dlss5ov::ClienteBuffer;
bool g_clienteOk = false, g_clienteFalhou = false;
ID3D12Device* g_dev = nullptr;
ID3D12GraphicsCommandList* g_lista = nullptr; // a lista em que o cliente gravou (sinaliza quando for submetida)
std::unordered_map<ffxContext, uint32_t> g_flags; // flags de criacao de cada contexto de upscale
// o maior tamanho de render declarado na criacao de cada contexto (as texturas do cliente cabem nele)
std::unordered_map<ffxContext, std::pair<uint32_t, uint32_t>> g_maxRender;
FILE* g_log = nullptr;
HMODULE g_eu = nullptr;
// dispositivo removido (o DRED, junto do vigia): o dispositivo do swapchain do jogo (segurado: nunca solto) e a lista do
// ultimo despacho do FSR (com os passes do modulo)
std::atomic<ID3D12Device*> g_devVigia{nullptr};
std::atomic<ID3D12CommandList*> g_listaFsrNativa{nullptr};
void LigaDred();
void ListaRecusada(ID3D12GraphicsCommandList* l, HRESULT r);
void RelataEnvio(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* l);
void ConfereDispositivo();
uint32_t GeracaoPedidaPainel();

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
PfnFfxConfigure oConfigura = nullptr;
// A geracao FSR 3 do proprio jogo (o swapchain de geracao da AMD dele e a configuracao dela): com ela ligada a nossa fica
// desligada -- duas geracoes empilhadas sao travada e fantasma. Muitos jogos criam o swapchain de geracao deles sempre
// (mesmo com a geracao desligada no menu) e o nosso fica dentro dele: so a configuracao diz se a deles esta ligada
std::atomic<bool> g_fgDoJogo{false};
thread_local bool t_fgSwapDoJogo = false; // dentro da criacao do swapchain de geracao do jogo
struct ConfiguraGeracaoJogo               // ffxConfigureDescFrameGeneration (ffx_api/ffx_framegeneration.h)
{
    ffxConfigureDescHeader header;
    void* swapChain;
    void* presentCallback;
    void* presentCallbackUserContext;
    void* frameGenerationCallback;
    void* frameGenerationCallbackUserContext;
    bool frameGenerationEnabled;
    bool allowAsyncWorkloads;
    FfxApiResource HUDLessColor;
    uint32_t flags;
    bool onlyPresentGenerated;
    FfxApiRect2D generationRect;
    uint64_t frameID;
};
struct RegistraUiJogo // ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 (ffx_api/dx12/ffx_api_dx12.h)
{
    ffxConfigureDescHeader header;
    FfxApiResource uiResource;
    uint32_t flags;
};
PfnFfxDestroyContext oDestroiContexto = nullptr;
// a UI que o jogo entrega a geracao FSR 3 dele: vai para o nosso swapchain enquanto a nossa gera no lugar (Configura)
std::mutex g_mxUi;
dlss5ov::UiDoJogo g_ui;
uint64_t g_uiTexturaQuadro = 0;  // o quadro da ultima textura de UI registrada pelo jogo
ffxContext g_ctxUi = nullptr;    // o contexto do swapchain de geracao do jogo em que ela foi registrada
RegistraUiJogo g_registroUi{};   // o ultimo registro (devolvido ao swapchain do jogo quando a nossa para)
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
bool g_proxy = false;                  // carregado da pasta do jogo como version.dll ou dxgi.dll
bool g_comoDxgi = false;               // ... como dxgi.dll (as fabricas passam por PxCreateDXGIFactory*)
std::atomic<HWND> g_janelaJogo{nullptr}; // a janela do swapchain do jogo (o vigia de travamento)
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
    // o envio que derrubou o dispositivo (uma lista sem fechar ou recusada ao fechar: o runtime remove na hora, antes de
    // a GPU fazer qualquer coisa, e o DRED nao mostra): quem enviou, o que e de onde
    static std::atomic<bool> removido{false};
    if (ID3D12Device* d = g_devVigia.load(); d && !removido.load(std::memory_order_relaxed) &&
                                             d->GetDeviceRemovedReason() != S_OK && !removido.exchange(true))
        RelataEnvio(q, n, l);
    g_ger.Submetido(l, n); // a geracao casa cada Present com o preparo ja enviado
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_lista)
        return;
    ID3D12CommandList* nativa = dlss5ov::ListaNativa(g_lista); // com o Streamline no meio (Geracao.h)
    for (UINT i = 0; i < n; ++i)
        if (dlss5ov::ListaNativa(l[i]) == nativa)
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
// modo de tela, HDR...: a AMD avisou a destruicao, Geracao.h). Com o anterior ainda vivo o novo segue sem geracao
bool LiberaParaCriar()
{
    return !g_ger.TemSwapchain();
}

// a imagem sem HUD (SemHud.h) vai para a geracao do quadro; DLSS5_SEM_HUD=0 desliga (teste A/B)
bool g_querSemHud = false; // a geracao esta em uso: abre a janela depois do despacho do upscaler
bool SemHudLigado()
{
    static const bool ligado = !(std::getenv("DLSS5_SEM_HUD") && std::getenv("DLSS5_SEM_HUD")[0] == '0');
    return ligado;
}

bool CapturaSemHud(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES s, uint32_t passe)
{
    return g_ger.SemHud(c, r, s, passe);
}

void InstalaSemHud(ID3D12CommandQueue* fila, IUnknown* sc)
{
    if (!g_devVigia.load())
    {
        ID3D12Device* d = nullptr;
        if (SUCCEEDED(fila->GetDevice(IID_PPV_ARGS(&d))))
            g_devVigia = d; // o vigia confere se a GPU removeu o dispositivo (segurado: nunca solto)
        semhud::DefineRecusa(&ListaRecusada);
    }
    static bool dito = false;
    if (!SemHudLigado())
    {
        if (!dito)
            Log("geracao: imagem sem HUD desligada (DLSS5_SEM_HUD=0): o HUD anda com a cena nos gerados");
        dito = true;
        return;
    }
    Microsoft::WRL::ComPtr<ID3D12Device> d;
    Microsoft::WRL::ComPtr<IDXGISwapChain> s;
    if (!sc || FAILED(fila->GetDevice(IID_PPV_ARGS(&d))) || FAILED(sc->QueryInterface(IID_PPV_ARGS(&s))))
        return;
    const char* e = semhud::Instala(d.Get(), s.Get(), &CapturaSemHud);
    if (!dito || e)
        Log("geracao: imagem sem HUD (o HUD parado nos gerados): %s", e ? e : "ganchos ok");
    dito = true;
}

HRESULT STDMETHODCALLTYPE CriaScHwnd(IDXGIFactory2* f, IUnknown* d, HWND h, const DXGI_SWAP_CHAIN_DESC1* sd,
                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** sc)
{
    if (h && !t_criandoSc)
        g_janelaJogo = h;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> fila;
    if (t_fgSwapDoJogo && !t_criandoSc)
        Log("geracao: o swapchain de geracao do proprio jogo (FSR 3 dele) cria o da janela: o nosso fica dentro dele");
    if (!t_criandoSc && g_lancado && d && sd && SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&fila))) && LiberaParaCriar())
    {
        t_criandoSc = true;
        const HRESULT r = g_ger.CriaSwapchain(f, fila.Get(), h, sd, fs, sc);
        t_criandoSc = false;
        if (SUCCEEDED(r))
        {
            Log("geracao: o jogo recebeu o swapchain da AMD (%ux%u fmt %d, %u buffers)", sd->Width, sd->Height,
                (int)sd->Format, sd->BufferCount);
            InstalaSemHud(fila.Get(), *sc);
            return r;
        }
        Log("geracao: o swapchain da AMD falhou (%s); o jogo segue sem geracao", g_ger.UltimoErro());
    }
    return oCriaScHwnd(f, d, h, sd, fs, o, sc);
}

HRESULT STDMETHODCALLTYPE CriaSc(IDXGIFactory* f, IUnknown* d, DXGI_SWAP_CHAIN_DESC* sd, IDXGISwapChain** sc)
{
    if (sd && sd->OutputWindow && !t_criandoSc)
        g_janelaJogo = sd->OutputWindow;
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
            InstalaSemHud(fila.Get(), *sc);
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
    LigaDred();
    const HRESULT h = oCriaFab(r, f);
    if (SUCCEEDED(h) && f && *f)
        GanchoFabrica(static_cast<IUnknown*>(*f));
    return h;
}
HRESULT WINAPI CriaFab1(REFIID r, void** f)
{
    LigaDred();
    const HRESULT h = oCriaFab1(r, f);
    if (SUCCEEDED(h) && f && *f)
        GanchoFabrica(static_cast<IUnknown*>(*f));
    return h;
}
HRESULT WINAPI CriaFab2(UINT fl, REFIID r, void** f)
{
    LigaDred();
    const HRESULT h = oCriaFab2(fl, r, f);
    if (SUCCEEDED(h) && f && *f)
        GanchoFabrica(static_cast<IUnknown*>(*f));
    return h;
}

// as fabricas DXGI do jogo passam pelo modulo (CreateDXGIFactory* da dxgi do Windows): a criacao do swapchain e
// enganchada nelas. Uma vez so
bool GanchaFabricasDxgi(HMODULE dxgi)
{
    static std::once_flag uma;
    static bool ok = false;
    std::call_once(uma, [&] {
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
        ok = DetourTransactionCommit() == NO_ERROR;
    });
    return ok;
}

// instalada como version.dll: a dxgi do Windows ja esta carregada (o jogo a importa) ou o carregador avisa quando ela
// carregar -- os ganchos entram antes de o jogo criar qualquer fabrica
struct AvisoCarga
{
    ULONG Flags;
    const UNICODE_STRING* FullDllName;
    const UNICODE_STRING* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
};
typedef VOID(CALLBACK* PfnAvisoCarga)(ULONG motivo, const AvisoCarga* d, PVOID ctx);
typedef NTSTATUS(NTAPI* PfnLdrRegistra)(ULONG flags, PfnAvisoCarga cb, PVOID ctx, PVOID* cookie);

std::wstring DxgiDoSistema()
{
    wchar_t p[MAX_PATH];
    GetSystemDirectoryW(p, MAX_PATH);
    wcscat_s(p, L"\\dxgi.dll");
    return p;
}

VOID CALLBACK DllCarregada(ULONG motivo, const AvisoCarga* d, PVOID)
{
    if (motivo != 1 || !d || !d->FullDllName || !d->FullDllName->Buffer) // 1 = carregada
        return;
    const std::wstring nome(d->FullDllName->Buffer, d->FullDllName->Length / sizeof(wchar_t));
    if (!_wcsicmp(nome.c_str(), DxgiDoSistema().c_str()))
        GanchaFabricasDxgi(static_cast<HMODULE>(d->DllBase));
}

void GanchaDxgiNaCarga()
{
    if (HMODULE dxgi = GetModuleHandleW(DxgiDoSistema().c_str()))
    {
        Log("geracao: ganchos nas fabricas DXGI (dxgi do Windows ja carregada): %s", GanchaFabricasDxgi(dxgi) ? "ok" : "falharam");
        return;
    }
    static PVOID cookie = nullptr;
    auto registra = reinterpret_cast<PfnLdrRegistra>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
    const bool ok = registra && registra(0, DllCarregada, nullptr, &cookie) >= 0;
    Log("geracao: os ganchos nas fabricas DXGI entram quando a dxgi do Windows carregar (aviso do carregador: %s)",
        ok ? "ok" : "falhou");
}

// o jogo nasceu suspenso: engancha a criacao das fabricas DXGI antes de ele criar qualquer uma e avisa o overlay, que
// entao solta o jogo
DWORD WINAPI PreparaGeracao(void*)
{
    if (HMODULE dxgi = LoadLibraryW(L"dxgi.dll"))
    {
        const bool ok = GanchaFabricasDxgi(dxgi);
        Log("jogo aberto pelo overlay (geracao %s no lancamento): ganchos nas fabricas DXGI %s",
            g_geracaoLancamento >= 2 ? "ligada" : "desligada", ok ? "ok" : "falharam");
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
std::atomic<ULONGLONG> g_ultimoQuadro{0}; // GetTickCount64 do ultimo despacho do FSR (o vigia)
// engasgos: o maior intervalo entre quadros e quantos passaram de 2x a media (o log de 5 s)
double g_maiorMs = 0, g_mediaMs = 0;
uint32_t g_engasgos = 0;

void MedeIntervalo()
{
    static LARGE_INTEGER freq{}, ant{};
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    if (ant.QuadPart)
    {
        const double ms = double(t.QuadPart - ant.QuadPart) * 1000.0 / double(freq.QuadPart);
        if (ms < 1000.0)
        {
            g_maiorMs = std::max(g_maiorMs, ms);
            if (g_mediaMs > 0 && ms > 2.0 * g_mediaMs && ms > 8.0)
                ++g_engasgos;
            g_mediaMs = g_mediaMs > 0 ? g_mediaMs * 0.95 + ms * 0.05 : ms;
        }
    }
    ant = t;
    g_ultimoQuadro = GetTickCount64();
}

void GeracaoQuadro(ffxContext* ctx, const ffxDispatchDescUpscale* u, ID3D12GraphicsCommandList* lista)
{
    MedeIntervalo();
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
    const uint32_t pedida = g_fgDoJogo ? 0u // a do proprio jogo esta ligada (o painel pede 1x)
                                       : GeracaoPedidaPainel();
    if (cab)
        cab->geracaoPronta = 1;
    static int estadoDito = 0;
    g_ger.Quadro(lista, g, std::min<uint32_t>(pedida, 4));
    // a procura da imagem sem HUD so quando o jogo nao entrega a UI dele (Configura): com ela, o quadro ja chega sem UI
    g_querSemHud = SemHudLigado() && pedida >= 2 && g_ger.EmUso() >= 2 && !g_ger.UiDoJogoEmUso();
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
        double media = 0, desvio = 0, maior = 0, p99 = 0, aFrente = 0;
        uint64_t aFrenteMax = 0, semPreparo = 0;
        const uint32_t n = g_ger.Ritmo(media, desvio, maior, p99);
        if (cab) // o painel: quadros na tela (reais + gerados) e o 1% low destes 5 s (0 = sem apresentacoes da AMD)
        {
            cab->apresentados10 = uint32_t(double(n) / s * 10.0 + 0.5);
            cab->baixo1pct10 = p99 > 0 ? uint32_t(10000.0 / p99 + 0.5) : 0;
        }
        if (g_ger.Fila(aFrente, aFrenteMax, semPreparo))
            Log("geracao: o jogo prepara %.2f quadros a frente do que apresenta (maior %llu); %llu apresentacoes sem o "
                "preparo do proprio quadro",
                aFrente, (unsigned long long)aFrenteMax, (unsigned long long)semPreparo);
        Log("geracao: pedida %ux, em uso %ux (monitor %u Hz, aceita ate %u), jogo %.0f quadros/s (maior intervalo %.1f ms, "
            "%u engasgos > 2x a media) + %.0f gerados/s = %.0f apresentados/s | ritmo: %u apresentacoes, a cada %.2f ms "
            "(desvio %.2f, maior %.2f, 1%% low %.0f q/s) | N mudou %u vezes | preparo e geracao se encontraram %llu vezes",
            pedida >= 2 ? std::min<uint32_t>(pedida, 4) : 1, pedida >= 2 ? g_ger.EmUso() : 1, g_ger.Hz(), g_ger.HzMaximo(),
            jogo, g_maiorMs, g_engasgos, gerados, jogo + gerados, n, media, desvio, maior, p99 > 0 ? 1000.0 / p99 : 0.0,
            g_ger.TrocasN(), (unsigned long long)g_ger.Encontros());
        // as primeiras apresentacoes com geracao, uma a uma: qual quadro cada uma usou (Geracao::Rastro)
        const std::string rastro = g_ger.Rastro();
        for (size_t i = 0, f; i < rastro.size(); i = f + 1)
        {
            f = rastro.find('\n', i);
            if (f == std::string::npos)
                f = rastro.size();
            Log("geracao: %s", rastro.substr(i, f - i).c_str());
        }
        char hud[200];
        const uint32_t capturas = semhud::Relato(hud, sizeof hud);
        if (pedida >= 2 && SemHudLigado())
        {
            uint32_t passe = 0, passes = 0;
            double igual = 0, troca = 0;
            int fase = 0;
            g_ger.SemHudEstado(passe, passes, igual, troca, fase);
            static const char* const kFase[4] = {"procurando o passe", "conferida e em uso",
                                                 "nenhum passe confere (o HUD anda com a cena; procura de novo)",
                                                 "em uso (a mascara se confere quando a cena andar)"};
            Log("geracao: imagem sem HUD %s: passe %u de %u, %.1f%% da tela igual ao quadro final, mascara trocou %.2f%% "
                "com a cena andando; copiada em %u de %.0f quadros%s%s",
                kFase[std::clamp(fase, 0, 3)], passe, passes, igual * 100.0, troca * 100.0, capturas, jogo * s,
                capturas ? ", " : "",
                capturas ? hud
                         : " (nenhum passe do jogo escreveu uma textura do tamanho e formato da tela depois do upscaler)");
        }
        g_maiorMs = 0;
        g_engasgos = 0;
        antes = g_ger.Gerados();
        quadrosAntes = g_quadrosGeracao;
        marca = agora;
    }
}

ffxReturnCode_t CriaContexto(ffxContext* ctx, ffxCreateContextDescHeader* desc, const ffxAllocationCallbacks* mem)
{
    bool fgSwap = false, fg = false;
    for (auto* h = desc; h; h = h->pNext)
    {
        fgSwap |= h->type == 0x30001u || h->type == 0x30005u || h->type == 0x30006u; // FRAMEGENERATIONSWAPCHAIN_*_DX12
        fg |= h->type == 0x00020001u;                                                // FRAMEGENERATION
    }
    if (fgSwap || fg)
        Log("o jogo cria %s da AMD dele (FSR 3)", fgSwap ? "o swapchain de geracao" : "a geracao de quadros");
    t_fgSwapDoJogo = fgSwap;
    const ffxReturnCode_t r = oCriaContexto(ctx, desc, mem);
    t_fgSwapDoJogo = false;
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

// a geracao que o painel do overlay pede (sem o overlay conectado, a do lancamento). O batimento do overlay atrasa sob
// carga (rede grande + geracao a 4K): o pedido dele vale ate 5 s sem batimento -- com o limite de 1 s do OverlayVivo a
// nossa geracao (e a do jogo, que ela desliga) ligava e desligava
uint32_t GeracaoPedidaPainel()
{
    dlss5ov::Cabecalho* cab = g_clienteOk ? g_cli.Cab() : nullptr;
    if (cab && cab->ativo && GetTickCount64() - cab->batimentoOverlay < 5000)
        return cab->geracaoQuadros;
    return g_geracaoLancamento;
}

// A escolha do painel manda. Com 2x-4x pedido no overlay, a geracao FSR 3 do proprio jogo fica desligada: a configuracao
// que o jogo manda a AMD a cada quadro vai com frameGenerationEnabled = false (o swapchain de geracao dele so repassa os
// quadros ao nosso, que gera). Com 1x, a do jogo segue como o jogo quiser e a nossa fica desligada (duas geracoes
// empilhadas: travada e fantasma). No TLOU2 com a geracao dele ligada no menu, a nossa ficava desligada sem aviso
// o pedido do painel, valendo so depois de 1 s igual: no TLOU2 ele alternava entre 0x e 4x varias vezes por segundo (o
// overlay religando) e a geracao do jogo ligava e desligava junto
uint32_t PedidoEstavel()
{
    static uint32_t estavel = 0, visto = 0;
    static ULONGLONG desde = 0;
    const uint32_t painel = GeracaoPedidaPainel();
    const ULONGLONG agoraMs = GetTickCount64();
    if (painel != visto)
        visto = painel, desde = agoraMs;
    if (agoraMs - desde >= 1000)
        estavel = visto;
    return estavel;
}

// o que o jogo entrega como UI ao FSR 3 dele, no log quando muda (o HUD e as legendas dos gerados dependem disso)
void DizUi(const dlss5ov::UiDoJogo& u, bool nossa)
{
    static int dito = -1;
    const int modo = (u.textura ? 1 : 0) | (u.callback ? 2 : 0) | (u.semHud ? 4 : 0) | (nossa ? 8 : 0);
    if (modo == dito)
        return;
    dito = modo;
    Log("UI do jogo (HUD e legendas) para a geracao FSR 3 dele: %s%s%s%s -- %s", u.textura ? "textura de UI " : "",
        u.callback ? "callback que desenha a UI " : "", u.semHud ? "imagem sem HUD " : "",
        u.Tem() ? "" : "nenhuma (a UI esta na imagem: o modulo procura a imagem sem HUD)",
        !u.Tem() ? "" : nossa ? "vai para o nosso swapchain (composta parada em cada quadro, real e gerado)"
                              : "fica com o swapchain do jogo");
}

// A nossa gera no lugar da do jogo: o painel pede 2x-4x (estavel) e a nossa pode gerar -- o swapchain da AMD e o nosso
// (o modulo entrou antes do DirectX) e o contexto esta pronto. Injetado com o jogo ja aberto a nossa nao gera
bool NossaNoLugar()
{
    return PedidoEstavel() >= 2 && g_ger.TemSwapchain() && g_ger.EstadoContexto() == 2;
}

// a textura de UI do jogo de volta ao swapchain dele (nossa = false) ou tirada dele (true), quando a decisao muda e o
// jogo nao a registra a cada quadro (o registro dele, quando vem, ja passa por Configura)
void UiNoSwapchainDoJogo(bool nossa)
{
    static bool antes = false;
    if (nossa == antes)
        return;
    antes = nossa;
    std::lock_guard<std::mutex> lk(g_mxUi);
    if (!g_ctxUi || !g_ui.textura || g_quadrosGeracao - g_uiTexturaQuadro <= 2)
        return;
    RegistraUiJogo r = g_registroUi;
    if (nossa)
        r.uiResource.resource = nullptr;
    ffxContext c = g_ctxUi;
    oConfigura(&c, &r.header);
}

ffxReturnCode_t DestroiContexto(ffxContext* ctx, const ffxAllocationCallbacks* memCb)
{
    if (ctx)
    {
        std::lock_guard<std::mutex> lk(g_mxUi);
        if (*ctx == g_ctxUi) // o swapchain de geracao do jogo saindo: o registro de UI dele sai junto
            g_ctxUi = nullptr, g_ui.textura = nullptr;
    }
    return oDestroiContexto(ctx, memCb);
}

ffxReturnCode_t Configura(ffxContext* ctx, const ffxConfigureDescHeader* desc)
{
    ConfiguraGeracaoJogo* fg = nullptr;
    RegistraUiJogo* reg = nullptr;
    for (auto* h = desc; h; h = h->pNext)
        if (h->type == 0x00020002u) // FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION
            fg = reinterpret_cast<ConfiguraGeracaoJogo*>(const_cast<ffxConfigureDescHeader*>(h));
        else if (h->type == 0x00030002u) // FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12
            reg = reinterpret_cast<RegistraUiJogo*>(const_cast<ffxConfigureDescHeader*>(h));
    if (!fg && !reg)
        return oConfigura(ctx, desc);
    const bool nossa = NossaNoLugar();
    UiNoSwapchainDoJogo(nossa);
    // A UI do jogo (HUD, legendas): com a nossa no lugar, o swapchain do jogo -- por fora do nosso -- nao a compoe mais
    // (ela chegava ao nosso ja na imagem e era interpolada com a cena: HUD e legendas tremendo nos gerados); vai para o
    // nosso, que a compoe parada em cada quadro, como a geracao do jogo faria
    dlss5ov::UiDoJogo ui;
    {
        std::lock_guard<std::mutex> lk(g_mxUi);
        if (reg)
        {
            g_ui.textura = static_cast<ID3D12Resource*>(reg->uiResource.resource);
            g_ui.texturaEstado = reg->uiResource.state;
            g_ui.texturaFlags = reg->flags;
            g_uiTexturaQuadro = g_quadrosGeracao;
            g_ctxUi = ctx ? *ctx : nullptr;
            g_registroUi = *reg;
            g_registroUi.header.pNext = nullptr;
        }
        if (fg)
        {
            g_ui.callback = fg->presentCallback;
            g_ui.callbackCtx = fg->presentCallbackUserContext;
            g_ui.semHud = static_cast<ID3D12Resource*>(fg->HUDLessColor.resource);
            g_ui.semHudEstado = fg->HUDLessColor.state;
            g_ui.frameId = fg->frameID;
        }
        ui = g_ui;
    }
    g_ger.UiJogo(ui, nossa);
    DizUi(ui, nossa);
    if (!fg)
    {
        if (!nossa || !reg->uiResource.resource)
            return oConfigura(ctx, desc);
        void* r = reg->uiResource.resource; // so durante a chamada: o jogo nao ve a troca
        reg->uiResource.resource = nullptr;
        const ffxReturnCode_t rc = oConfigura(ctx, desc);
        reg->uiResource.resource = r;
        return rc;
    }
    const uint32_t estavel = PedidoEstavel();
    const bool nossaPode = g_ger.TemSwapchain() && g_ger.EstadoContexto() == 2;
    const bool jogoQuer = fg->frameGenerationEnabled, tira = jogoQuer && nossa;
    g_fgDoJogo = jogoQuer && !tira;
    // 0 a do jogo desligada, 1 ligada, 2 pedida pelo jogo e desligada pelo painel, 3 ligada com o painel pedindo a nossa
    // (que nao pode gerar)
    static std::atomic<int> dito{-1};
    const int agora = tira ? 2 : jogoQuer ? (estavel >= 2 && !nossaPode ? 3 : 1) : 0;
    if (dito.exchange(agora) != agora)
        Log(agora == 2   ? "o jogo pede a geracao FSR 3 dele, mas o painel pede %ux: a dele fica desligada e a nossa liga"
            : agora == 3 ? "o jogo ligou a geracao FSR 3 dele; o painel pede %ux, mas a nossa nao pode gerar (o modulo "
                           "entrou com o jogo aberto: instale na pasta do jogo e reabra) -- a do jogo segue ligada"
            : agora == 1 ? "o jogo ligou a geracao FSR 3 dele e o painel pede %ux: a nossa fica desligada"
                         : "a geracao FSR 3 do jogo esta desligada (painel: %ux)",
            estavel);
    if (!nossa)
        return oConfigura(ctx, desc);
    // so durante a chamada (o jogo nao ve a troca): a geracao dele desligada, e sem a UI (o callback dele desenharia o HUD
    // na imagem antes do nosso swapchain; a imagem sem HUD so serve a geracao)
    const ConfiguraGeracaoJogo antes = *fg;
    void* texturaAntes = reg ? reg->uiResource.resource : nullptr;
    fg->frameGenerationEnabled = false;
    fg->presentCallback = fg->presentCallbackUserContext = nullptr;
    fg->HUDLessColor.resource = nullptr;
    if (reg)
        reg->uiResource.resource = nullptr;
    const ffxReturnCode_t r = oConfigura(ctx, desc);
    fg->frameGenerationEnabled = antes.frameGenerationEnabled;
    fg->presentCallback = antes.presentCallback;
    fg->presentCallbackUserContext = antes.presentCallbackUserContext;
    fg->HUDLessColor.resource = antes.HUDLessColor.resource;
    if (reg)
        reg->uiResource.resource = texturaAntes;
    return r;
}

ffxReturnCode_t Despacha(ffxContext* ctx, const ffxDispatchDescHeader* desc)
{
    if (desc && desc->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE)
        semhud::Fecha(); // o upscaler de um quadro novo: a janela do anterior acabou (mesmo sem Present no meio)
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
    g_listaFsrNativa = dlss5ov::ListaNativa(lista); // o relato do dispositivo removido aponta se a lista e esta
    // depois deste despacho e do que o modulo grava nele, o primeiro passe do jogo que monta a cena final da a imagem sem
    // HUD (SemHud.h); o que o modulo grava aqui nao conta
    struct AbreNoFim
    {
        ~AbreNoFim()
        {
            if (g_querSemHud)
                semhud::Abre();
        }
    } abreNoFim;
    semhud::Proprio eu;
    // o gancho dos envios a GPU ja no primeiro quadro: a geracao casa cada Present com o preparo enviado antes dele (sem
    // isso ela se desalinha na partida e junta a imagem de um quadro com o movimento do seguinte)
    static std::once_flag filaUma;
    std::call_once(filaUma, [&] {
        Microsoft::WRL::ComPtr<ID3D12Device> d;
        if (SUCCEEDED(lista->GetDevice(IID_PPV_ARGS(&d))))
            GanchoFila(d.Get());
    });
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
                "sem atraso: %llu esperas, media %.1f ms, max %.1f ms, %llu estouradas (prazo %llu, batimento %llu, "
                "cerca %llu); sem esperar: %llu suspensos, %llu sem batimento",
                (agora - marca) / 1000.0, (unsigned long long)despachos, despachos * 1000.0 / double(agora - marca), rwMin,
                rhMin, rwMax, rhMax, (unsigned long long)resets, (unsigned long long)trocasSaida, jMax,
                (unsigned long long)d.quadros, (unsigned long long)d.exportados, (unsigned long long)d.aplicados,
                d.aplicados ? double(d.idadeSoma) / double(d.aplicados) : 0.0, (unsigned long long)d.idadeMax,
                (unsigned long long)d.esperas, d.esperas ? d.esperaMs / double(d.esperas) : 0.0, d.esperaMsMax,
                (unsigned long long)d.estouros, (unsigned long long)d.falhaPrazo, (unsigned long long)d.falhaBatimento,
                (unsigned long long)d.falhaCerca, (unsigned long long)d.suspensos, (unsigned long long)d.semBatimento);
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
// ---- vigia de travamento: se os quadros param ou a janela do jogo trava, o log ganha onde cada thread esta parada
// (modulo + deslocamento; os do modulo saem pelo DLSS5Buffer.map da compilacao) ----
void NomeEndereco(DWORD64 a, char* b, size_t n)
{
    HMODULE md = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(a), &md) &&
        md)
    {
        char p[MAX_PATH];
        GetModuleFileNameA(md, p, MAX_PATH);
        const char* nome = strrchr(p, '\\');
        snprintf(b, n, "%s+0x%llx", nome ? nome + 1 : p, (unsigned long long)(a - DWORD64(md)));
    }
    else
        snprintf(b, n, "0x%llx", (unsigned long long)a);
}

// desenrola uma pilha a partir do contexto (a thread ja foi solta: parada numa espera a pilha nao muda; se ela andou, a
// leitura pode falhar e o __except corta)
int Desenrola(CONTEXT c, DWORD64* pcs, int max)
{
    int k = 0;
    __try
    {
        while (k < max && c.Rip)
        {
            pcs[k++] = c.Rip;
            DWORD64 base = 0;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
            if (!f)
            {
                c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp); // funcao folha: o retorno no topo da pilha
                c.Rsp += 8;
            }
            else
            {
                void* hd = nullptr;
                DWORD64 ef = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &hd, &ef, nullptr);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return k;
}

void RelataPilhas(const char* motivo)
{
    Log("VIGIA: %s -- onde cada thread esta:", motivo);
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (s == INVALID_HANDLE_VALUE)
        return;
    THREADENTRY32 te{sizeof te};
    const DWORD pid = GetCurrentProcessId(), eu = GetCurrentThreadId();
    for (BOOL ok = Thread32First(s, &te); ok; ok = Thread32Next(s, &te))
    {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == eu)
            continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                              te.th32ThreadID);
        if (!t)
            continue;
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL;
        bool pegou = false;
        if (SuspendThread(t) != DWORD(-1)) // so o instante do contexto: nada e alocado com ela parada
        {
            pegou = GetThreadContext(t, &c) != 0;
            ResumeThread(t);
        }
        char nome[64] = "";
        PWSTR w = nullptr;
        if (SUCCEEDED(GetThreadDescription(t, &w)) && w)
        {
            WideCharToMultiByte(CP_UTF8, 0, w, -1, nome, sizeof nome, nullptr, nullptr);
            LocalFree(w);
        }
        CloseHandle(t);
        if (!pegou)
            continue;
        DWORD64 pcs[20];
        const int n = Desenrola(c, pcs, 20);
        char linha[2048];
        int p = snprintf(linha, sizeof linha, "  thread %lu '%s':", te.th32ThreadID, nome);
        for (int i = 0; i < n && p < int(sizeof linha) - 80; ++i)
        {
            char e[96];
            NomeEndereco(pcs[i], e, sizeof e);
            p += snprintf(linha + p, sizeof linha - p, " %s", e);
        }
        Log("%s", linha);
    }
    CloseHandle(s);
}

// a taxa fisica do sinal de video do monitor `gdi` (QueryDisplayConfig com QDC_VIRTUAL_REFRESH_RATE_AWARE): com a taxa
// dinamica do Windows 11 a area de trabalho fica "virtual" a 60 Hz e a tela segue a 120 -- EnumDisplaySettings so ve os 60
// (a geracao saia desligada num monitor de 120 Hz com FreeSync). 0 = sem a informacao
uint32_t TaxaDoSinal(const wchar_t* gdi)
{
    const UINT32 fl = QDC_ONLY_ACTIVE_PATHS | 0x40; // QDC_VIRTUAL_REFRESH_RATE_AWARE (Windows 11)
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(fl, &np, &nm) != ERROR_SUCCESS || !np || !nm)
        return 0;
    std::vector<DISPLAYCONFIG_PATH_INFO> pa(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> mo(nm);
    if (QueryDisplayConfig(fl, &np, pa.data(), &nm, mo.data(), nullptr) != ERROR_SUCCESS)
        return 0;
    for (UINT32 i = 0; i < np; ++i)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size = sizeof sn;
        sn.header.adapterId = pa[i].sourceInfo.adapterId;
        sn.header.id = pa[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS || _wcsicmp(sn.viewGdiDeviceName, gdi))
            continue;
        const UINT32 k = pa[i].targetInfo.modeInfoIdx;
        if (k < nm && mo[k].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)
        {
            const DISPLAYCONFIG_RATIONAL& v = mo[k].targetMode.targetVideoSignalInfo.vSyncFreq;
            return v.Denominator ? uint32_t(double(v.Numerator) / v.Denominator + 0.5) : 0;
        }
    }
    return 0;
}

// a taxa do monitor da janela do jogo e a maior que ele aceita nesta resolucao (nesta thread, nunca na de render)
void MedeTaxa(HWND j)
{
    static std::wstring monitor;
    static DWORD w = 0, h = 0;
    static uint32_t maximo = 0;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (!j || !GetMonitorInfoW(MonitorFromWindow(j, MONITOR_DEFAULTTONEAREST), &mi) ||
        !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        return;
    const uint32_t sinal = TaxaDoSinal(mi.szDevice);
    const uint32_t hz = sinal ? sinal : dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 0;
    // o compositor da area de trabalho (janela comum, sem tela cheia: as apresentacoes com vsync saem no ritmo dele -- com
    // a taxa dinamica do Windows ele pode estar a 60 com o sinal a 120)
    DWM_TIMING_INFO ti{};
    ti.cbSize = sizeof ti;
    const uint32_t dwm = SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.rateRefresh.uiDenominator
                             ? uint32_t(double(ti.rateRefresh.uiNumerator) / ti.rateRefresh.uiDenominator + 0.5)
                             : 0;
    static uint32_t ditoHz = 0, ditoDwm = 0;
    if (hz != ditoHz || dwm != ditoDwm)
    {
        Log("monitor: sinal %u Hz, compositor da area de trabalho %u Hz", hz, dwm);
        ditoHz = hz;
        ditoDwm = dwm;
    }
    if (monitor != mi.szDevice || w != dm.dmPelsWidth || h != dm.dmPelsHeight)
    {
        monitor = mi.szDevice;
        w = dm.dmPelsWidth;
        h = dm.dmPelsHeight;
        maximo = hz;
        DEVMODEW d{};
        d.dmSize = sizeof d;
        for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &d); ++i)
            if (d.dmPelsWidth == w && d.dmPelsHeight == h)
                maximo = std::max<uint32_t>(maximo, d.dmDisplayFrequency);
    }
    g_ger.DefineTaxa(hz, maximo);
}

// acessos invalidos (o TLOU2 caiu com 0xc0000005 no proprio codigo, com o overlay conectando): onde foi e a pilha, no log.
// So registra (o jogo pode tratar o dele); no maximo 5
// ---- dispositivo removido ----
// O TLOU2 para (de proposito: escreve o codigo em 0x0) com DXGI_ERROR_INVALID_CALL quando GetDeviceRemovedReason diz
// que a GPU removeu o dispositivo por uma chamada invalida -- um comando invalido numa lista enviada. O DRED da
// Microsoft (Device Removed Extended Data), ligado antes de o jogo criar o dispositivo, guarda o ultimo comando que cada
// lista completou e a falha de pagina; o vigia confere o dispositivo a cada 0,5 s (e o acesso invalido tambem) e, se
// caiu, poe tudo no log. As listas que o runtime recusa ao fechar (um comando invalido gravado nelas) tambem vao para o
// log: executada, uma delas derruba o dispositivo
void LigaDred()
{
    static std::once_flag uma;
    std::call_once(uma, [] {
        Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> s;
        const bool ok = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&s)));
        if (ok)
        {
            s->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            s->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }
        Log("DRED (o que a GPU fazia se o dispositivo cair): %s", ok ? "ligado" : "indisponivel");
    });
}

const char* NomeOp(D3D12_AUTO_BREADCRUMB_OP op)
{
    static const char* const k[] = {
        "SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced", "ExecuteIndirect", "Dispatch",
        "CopyBufferRegion", "CopyTextureRegion", "CopyResource", "CopyTiles", "ResolveSubresource", "ClearRenderTargetView",
        "ClearUnorderedAccessView", "ClearDepthStencilView", "ResourceBarrier", "ExecuteBundle", "Present", "ResolveQueryData",
        "BeginSubmission", "EndSubmission", "DecodeFrame", "ProcessFrames", "AtomicCopyBufferUINT", "AtomicCopyBufferUINT64",
        "ResolveSubresourceRegion", "WriteBufferImmediate", "DecodeFrame1", "SetProtectedResourceSession", "DecodeFrame2",
        "ProcessFrames1", "BuildRaytracingAccelerationStructure", "EmitRaytracingAccelerationStructurePostbuildInfo",
        "CopyRaytracingAccelerationStructure", "DispatchRays", "InitializeMetaCommand", "ExecuteMetaCommand", "EstimateMotion",
        "ResolveMotionVectorHeap", "SetPipelineState1", "InitializeExtensionCommand", "ExecuteExtensionCommand",
        "DispatchMesh", "EncodeFrame", "ResolveEncoderOutputMetadata", "Barrier", "BeginCommandList", "DispatchGraph",
        "SetProgram"};
    return unsigned(op) < std::size(k) ? k[op] : "?";
}

void RelataRemocao(ID3D12Device* d, HRESULT motivo)
{
    Log("DISPOSITIVO REMOVIDO pela GPU: motivo 0x%08lx%s", (unsigned long)motivo,
        motivo == DXGI_ERROR_INVALID_CALL    ? " (chamada invalida)"
        : motivo == DXGI_ERROR_DEVICE_HUNG   ? " (GPU travada)"
        : motivo == DXGI_ERROR_DEVICE_RESET  ? " (GPU reiniciada)"
        : motivo == DXGI_ERROR_DEVICE_REMOVED ? " (removido)"
                                              : "");
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (FAILED(d->QueryInterface(IID_PPV_ARGS(&dred))))
    {
        Log("DRED: sem dados");
        return;
    }
    ID3D12CommandList* fsr = g_listaFsrNativa.load();
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{};
    int listas = 0;
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc)))
        for (const D3D12_AUTO_BREADCRUMB_NODE* n = bc.pHeadAutoBreadcrumbNode; n && listas < 12; n = n->pNext)
        {
            const UINT feitos = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            if (feitos >= n->BreadcrumbCount)
                continue; // esta terminou
            ++listas;
            char ops[1200];
            int p = 0;
            for (UINT i = feitos > 4 ? feitos - 4 : 0; i < n->BreadcrumbCount && i < feitos + 10 && p < int(sizeof ops) - 80; ++i)
                p += snprintf(ops + p, sizeof ops - p, "%s%s", i == feitos ? " >> " : " ", NomeOp(n->pCommandHistory[i]));
            Log("DRED: lista %p \"%ls\"%s na fila \"%ls\": %u de %u comandos feitos (>> = o que nao terminou):%s",
                n->pCommandList, n->pCommandListDebugNameW ? n->pCommandListDebugNameW : L"",
                n->pCommandList && dlss5ov::ListaNativa(n->pCommandList) == fsr ? " (a do FSR, com os passes do modulo)" : "",
                n->pCommandQueueDebugNameW ? n->pCommandQueueDebugNameW : L"", feitos, n->BreadcrumbCount, ops);
        }
    if (!listas)
        Log("DRED: nenhuma lista pela metade");
    D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)) && pf.PageFaultVA)
    {
        Log("DRED: falha de pagina da GPU em 0x%llx", (unsigned long long)pf.PageFaultVA);
        int k = 0;
        for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadExistingAllocationNode; a && k < 8; a = a->pNext, ++k)
            Log("DRED:   ali, existente: \"%ls\" (tipo %d)", a->ObjectNameW ? a->ObjectNameW : L"", int(a->AllocationType));
        k = 0;
        for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadRecentFreedAllocationNode; a && k < 8; a = a->pNext, ++k)
            Log("DRED:   ali, solta ha pouco: \"%ls\" (tipo %d)", a->ObjectNameW ? a->ObjectNameW : L"", int(a->AllocationType));
    }
}

void RelataEnvio(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* l)
{
    auto nome = [](ID3D12Object* o, wchar_t* b, UINT tam) {
        b[0] = 0;
        if (o && FAILED(o->GetPrivateData(WKPDID_D3DDebugObjectNameW, &tam, b)))
            b[0] = 0;
        return b;
    };
    wchar_t nq[64];
    const D3D12_COMMAND_QUEUE_DESC qd = q->GetDesc();
    char linha[2048];
    int p = snprintf(linha, sizeof linha, "O ENVIO QUE DERRUBOU O DISPOSITIVO: fila %p \"%ls\" (tipo %d), %u listas:", q,
                     nome(q, nq, sizeof nq - 2), int(qd.Type), n);
    ID3D12CommandList* fsr = g_listaFsrNativa.load();
    for (UINT i = 0; i < n && i < 16 && p < int(sizeof linha) - 160; ++i)
    {
        wchar_t nl[64];
        p += snprintf(linha + p, sizeof linha - p, " [%p \"%ls\" tipo %d%s]", l[i], nome(l[i], nl, sizeof nl - 2),
                      l[i] ? int(l[i]->GetType()) : -1,
                      l[i] && dlss5ov::ListaNativa(l[i]) == fsr ? " a do FSR, com os passes do modulo" : "");
    }
    Log("%s", linha);
    void* pilha[24];
    const USHORT k = RtlCaptureStackBackTrace(1, 24, pilha, nullptr);
    p = snprintf(linha, sizeof linha, "  quem enviou:");
    for (USHORT i = 0; i < k && p < int(sizeof linha) - 100; ++i)
    {
        char s[96];
        NomeEndereco(DWORD64(pilha[i]), s, sizeof s);
        p += snprintf(linha + p, sizeof linha - p, " %s", s);
    }
    Log("%s", linha);
    ConfereDispositivo();
}

void ConfereDispositivo()
{
    static std::atomic<bool> dito{false};
    ID3D12Device* d = g_devVigia.load();
    if (!d || dito)
        return;
    const HRESULT r = d->GetDeviceRemovedReason();
    if (r == S_OK || dito.exchange(true))
        return;
    RelataRemocao(d, r);
}

// uma lista que o runtime recusou ao fechar (SemHud.h): o comando invalido esta nela
void ListaRecusada(ID3D12GraphicsCommandList* l, HRESULT r)
{
    static std::atomic<int> n{0};
    if (n++ >= 8)
        return;
    wchar_t nome[64] = {};
    UINT tam = sizeof nome - sizeof(wchar_t);
    if (FAILED(l->GetPrivateData(WKPDID_D3DDebugObjectNameW, &tam, nome)))
        nome[0] = 0;
    Log("LISTA RECUSADA ao fechar (0x%08lx): %p \"%ls\"%s -- enviada, ela derruba o dispositivo", (unsigned long)r, l, nome,
        dlss5ov::ListaNativa(l) == g_listaFsrNativa.load() ? " (a do FSR, com os passes do modulo)" : "");
}

LONG CALLBACK Excecao(EXCEPTION_POINTERS* e)
{
    static std::atomic<int> vistos{0};
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || vistos++ >= 5)
        return EXCEPTION_CONTINUE_SEARCH;
    ConfereDispositivo(); // o TLOU2 para assim quando o dispositivo cai: o DRED antes de o processo sair
    DWORD64 pcs[20];
    const int n = Desenrola(*e->ContextRecord, pcs, 20);
    char linha[2048];
    int p = snprintf(linha, sizeof linha, "ACESSO INVALIDO (%s 0x%llx) na thread %lu:",
                     e->ExceptionRecord->ExceptionInformation[0] ? "escrita em" : "leitura de",
                     (unsigned long long)e->ExceptionRecord->ExceptionInformation[1], GetCurrentThreadId());
    for (int i = 0; i < n && p < int(sizeof linha) - 80; ++i)
    {
        char s[96];
        NomeEndereco(pcs[i], s, sizeof s);
        p += snprintf(linha + p, sizeof linha - p, " %s", s);
    }
    Log("%s", linha);
    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI Vigia(void*)
{
    AddVectoredExceptionHandler(0, Excecao);
    int relatos = 0, voltas = 0;
    bool relatado = false;
    ULONGLONG travadaDesde = 0;
    for (;;)
    {
        Sleep(500);
        const ULONGLONG agora = GetTickCount64(), ult = g_ultimoQuadro.load();
        const HWND j = g_janelaJogo.load();
        if (voltas++ % 4 == 0)
            MedeTaxa(j);
        ConfereDispositivo();
        static bool tinha = false; // o swapchain da AMD saiu (o jogo o soltou: troca de modo de tela ou saida)
        if (tinha && !g_ger.TemSwapchain())
            Log("geracao: o jogo soltou o swapchain da AMD (o contexto saiu junto)");
        tinha = g_ger.TemSwapchain();
        if (relatos >= 3)
            continue;
        const bool semQuadro = ult && agora - ult > 4000;
        const bool travada = j && IsWindow(j) && IsHungAppWindow(j);
        travadaDesde = travada ? (travadaDesde ? travadaDesde : agora) : 0;
        if (!semQuadro && !travada)
        {
            relatado = false;
            continue;
        }
        if (relatado || (!semQuadro && agora - travadaDesde < 3000))
            continue;
        char motivo[160];
        snprintf(motivo, sizeof motivo, "%s%s", semQuadro ? "sem quadro do FSR ha mais de 4 s" : "",
                 travada ? (semQuadro ? " e a janela do jogo nao responde" : "a janela do jogo nao responde ha 3 s") : "");
        RelataPilhas(motivo);
        relatado = true;
        ++relatos;
    }
    return 0;
}

DWORD WINAPI Instala(void*)
{
    LigaDred(); // antes de o jogo criar o dispositivo
    if (HANDLE t = CreateThread(nullptr, 0, Vigia, nullptr, 0, nullptr))
        CloseHandle(t);
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
                oConfigura = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(m, "ffxConfigure"));
                oDestroiContexto = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(m, "ffxDestroyContext"));
                if (!oCriaContexto || !oDespacha)
                    continue;
                DetourTransactionBegin();
                DetourUpdateThread(GetCurrentThread());
                DetourAttach(reinterpret_cast<PVOID*>(&oCriaContexto), reinterpret_cast<PVOID>(&CriaContexto));
                DetourAttach(reinterpret_cast<PVOID*>(&oDespacha), reinterpret_cast<PVOID>(&Despacha));
                if (oConfigura)
                    DetourAttach(reinterpret_cast<PVOID*>(&oConfigura), reinterpret_cast<PVOID>(&Configura));
                if (oDestroiContexto)
                    DetourAttach(reinterpret_cast<PVOID*>(&oDestroiContexto), reinterpret_cast<PVOID>(&DestroiContexto));
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

// ---- instalada na pasta do jogo como version.dll ou dxgi.dll: as funcoes da DLL do Windows de mesmo nome ----
extern "C" void* g_dxgiFn[17]; // ProxyDxgi.asm
extern "C" void* g_verFn[15];
namespace {
const char* const kDxgiNomes[17] = {"ApplyCompatResolutionQuirking", "CompatString", "CompatValue", "DXGID3D10CreateDevice",
                                    "DXGID3D10CreateLayeredDevice", "DXGID3D10GetLayeredDeviceSize",
                                    "DXGID3D10RegisterLayers", "DXGIDeclareAdapterRemovalSupport",
                                    "DXGIDisableVBlankVirtualization", "DXGIDumpJournal", "DXGIGetDebugInterface1",
                                    "DXGIReportAdapterConfiguration", "PIXBeginCapture", "PIXEndCapture",
                                    "PIXGetCaptureState", "SetAppCompatStringPointer", "UpdateHMDEmulationStatus"};
const char* const kVerNomes[15] = {"GetFileVersionInfoA", "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",
                                   "GetFileVersionInfoExW", "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExA",
                                   "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW", "GetFileVersionInfoW",
                                   "VerFindFileA", "VerFindFileW", "VerInstallFileA", "VerInstallFileW", "VerQueryValueA",
                                   "VerQueryValueW"};
PfnCriaFab oSisFab = nullptr, oSisFab1 = nullptr;
PfnCriaFab2 oSisFab2 = nullptr;
std::once_flag g_resolveDxgi, g_resolveVersion;

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

extern "C" void ResolveVersionSistema()
{
    std::call_once(g_resolveVersion, [] {
        wchar_t p[MAX_PATH];
        GetSystemDirectoryW(p, MAX_PATH);
        wcscat_s(p, L"\\version.dll");
        if (HMODULE d = LoadLibraryW(p))
            for (int i = 0; i < 15; ++i)
                g_verFn[i] = reinterpret_cast<void*>(GetProcAddress(d, kVerNomes[i]));
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

// marcador: o overlay reconhece a DLL instalada por ele (version.dll ou dxgi.dll) e a versao do protocolo dela
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
        g_comoDxgi = base && !_wcsicmp(base + 1, L"dxgi.dll");
        g_proxy = g_comoDxgi || (base && !_wcsicmp(base + 1, L"version.dll"));
        if (g_proxy && !EhOJogo(p))
            return TRUE; // outro programa da pasta do jogo: so a DLL do Windows (ProxyDxgi.asm)
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
            g_comoDxgi ? " como dxgi.dll da pasta do jogo" : g_proxy ? " como version.dll da pasta do jogo" : "");
        if (g_proxy)
        {
            // instalada no jogo: entrou antes do DirectX, a geracao funciona sem abrir pelo overlay. O pedido vem do
            // painel quando o overlay conecta (DLSS5_GERACAO, so para testes, vale antes disso)
            g_lancado = true;
            if (const wchar_t* t = _wgetenv(L"DLSS5_GERACAO"))
                g_geracaoLancamento = uint32_t(_wtoi(t));
            if (!g_comoDxgi) // como version.dll: as fabricas da dxgi do Windows sao enganchadas (como dxgi.dll ja passam aqui)
                GanchaDxgiNaCarga();
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
        if (lancado && !g_proxy) // da pasta do jogo as fabricas ja passam pelo modulo
            if (HANDLE t = CreateThread(nullptr, 0, PreparaGeracao, nullptr, 0, nullptr))
                CloseHandle(t);
        if (HANDLE t = CreateThread(nullptr, 0, Instala, nullptr, 0, nullptr))
            CloseHandle(t);
    }
    return TRUE;
}
