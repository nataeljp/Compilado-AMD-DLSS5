// DLSS5Buffer.dll: o modulo que o DLSS5 Overlay coloca no jogo aberto (Modo de Rede = captura de buffer).
// Nao roda rede nenhuma: so pega, depois do upscale, o que o jogo entrega ao FSR 3.1+ (cor sem HUD, profundidade,
// movimento, exposicao, camera) e conversa com o overlay pelo ClienteBuffer (buffer/Protocolo.h). Ganchos:
//   - ffxCreateContext / ffxDispatch da DLL do upscaler (amd_fidelityfx_upscaler_dx12.dll; FSR 3.1 antigo:
//     amd_fidelityfx_dx12.dll): depois do despacho de upscale, grava os passes do cliente na mesma lista;
//   - ID3D12CommandQueue::ExecuteCommandLists: quando essa lista e submetida, sinaliza a cerca do jogo;
//   - entrada (com o painel do overlay aberto sobre o jogo): GetRawInputData/GetRawInputBuffer e as mensagens de mouse
//     da janela do jogo -- o movimento e os cliques vao para o painel e o jogo recebe zero (a mira nao mexe).
// Registro em DLSS5Buffer.log ao lado da DLL.
#include "../buffer/ClienteBuffer.h"
#include "../buffer/Vram.h"
#include <dxgi1_4.h>
#include <intrin.h>
#include "ffx_api.h"
#include "ffx_upscale.h"
#include <detours.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <cwctype>
#include <string>
#include <unordered_map>
#include <vector>

void NotaProfFsr(ID3D12Resource* r); // Apresenta.cpp
void LimiteNoDespacho();              // Apresenta.cpp
bool OverlayDaApiVivo();              // Apresenta.cpp
namespace {

std::mutex g_mx;
// nunca destruido: na saida do processo o destrutor soltaria texturas e cercas num driver que ja saiu (amdxc64.dll
// abortava o processo, 0xc0000409)
dlss5ov::ClienteBuffer& g_cli = *new dlss5ov::ClienteBuffer;
bool g_clienteOk = false, g_clienteFalhou = false;
ID3D12Device* g_dev = nullptr;
ID3D12GraphicsCommandList* g_lista = nullptr; // a lista em que o cliente gravou (sinaliza quando for submetida)
std::unordered_map<ffxContext, uint32_t> g_flags; // flags de criacao de cada contexto de upscale
// o maior tamanho de render declarado na criacao de cada contexto (as texturas do cliente cabem nele)
std::unordered_map<ffxContext, std::pair<uint32_t, uint32_t>> g_maxRender;
FILE* g_log = nullptr;
HMODULE g_eu = nullptr;
// dispositivo removido (o DRED, junto do vigia): o dispositivo do FSR do jogo (segurado: nunca solto) e a lista do
// ultimo despacho do FSR (com os passes do modulo)
std::atomic<ID3D12Device*> g_devVigia{nullptr};
std::atomic<ID3D12CommandList*> g_listaFsrNativa{nullptr};
void LigaDred();
void RelataEnvio(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* l);
void ConfereDispositivo();

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

std::atomic<HWND> g_janelaJogo{nullptr}; // a janela do jogo (o vigia de travamento)
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
                {
                    g_cabNome = static_cast<dlss5ov::Cabecalho*>(
                        MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(dlss5ov::Cabecalho)));
                    CloseHandle(m); // a vista segura o mapeamento
                }
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
        g_janelaJogo = h;
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
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_lista)
        return;
    ID3D12CommandList* nativa = dlss5ov::ListaNativa(g_lista); // com o Streamline no meio (ClienteBuffer.h)
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

uint64_t g_quadros = 0; // quadros do jogo (despachos do FSR)
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

// a cada despacho do FSR: os quadros por segundo do jogo para o painel, o pedido de gravacao e o log de 5 s
void ContaQuadro()
{
    MedeIntervalo();
    ++g_quadros;
    if (dlss5ov::Cabecalho* cab = g_clienteOk ? g_cli.Cab() : nullptr; cab && g_mediaMs > 0)
        cab->fpsJogo10 = uint32_t(10000.0 / g_mediaMs + 0.5);
    // a gravacao do overlay (Ctrl+Shift+G) pede a cor do jogo na pasta dela: arquivo em %TEMP% com o nosso pid
    static ULONGLONG visto = 0;
    if (GetTickCount64() - visto >= 1000)
    {
        visto = GetTickCount64();
        // a janela apareceu depois do FSR, ou o jogo a recriou (troca de modo de tela): o gancho de mensagens acompanha
        if (g_ganchoMsg && !IsWindow(g_janelaJogo.load()))
        {
            UnhookWindowsHookEx(g_ganchoMsg);
            g_ganchoMsg = nullptr;
        }
        GanchoMensagens();
        wchar_t tmp[MAX_PATH], arq[MAX_PATH + 40];
        GetTempPathW(MAX_PATH, tmp);
        swprintf(arq, MAX_PATH + 40, L"%sDLSS5Ov.despejo.%lu", tmp, GetCurrentProcessId());
        FILE* f = nullptr;
        if (!_wfopen_s(&f, arq, L"rb") && f)
        {
            char pasta[1024] = {};
            fread(pasta, 1, sizeof pasta - 1, f);
            fclose(f);
            DeleteFileW(arq);
            std::lock_guard<std::mutex> lk(g_mx);
            g_cli.DespejaEm(pasta); // a cor do jogo antes e depois da edicao, e a edicao, no tamanho do jogo
        }
    }
    static ULONGLONG marca = 0;
    static uint64_t quadrosAntes = 0;
    const ULONGLONG agora = GetTickCount64();
    if (!marca)
        marca = agora;
    if (agora - marca >= 5000)
    {
        Log("jogo %.0f quadros/s (maior intervalo %.1f ms, %u engasgos > 2x a media)",
            double(g_quadros - quadrosAntes) * 1000.0 / double(agora - marca), g_maiorMs, g_engasgos);
        g_maiorMs = 0;
        g_engasgos = 0;
        quadrosAntes = g_quadros;
        marca = agora;
    }
}

// o que o jogo pede a API da AMD, uma vez por tipo: mostra se o upscale passa mesmo por aqui (FSR ligado no menu, outro
// caminho, ou um tipo novo do SDK que o modulo nao conhece)
void DizTipo(const char* onde, uint64_t tipo)
{
    static std::mutex mx;
    static std::vector<uint64_t> vistos;
    std::lock_guard<std::mutex> lk(mx);
    if (vistos.size() >= 64 || std::find(vistos.begin(), vistos.end(), tipo) != vistos.end())
        return;
    vistos.push_back(tipo);
    Log("API da AMD: %s tipo 0x%llx", onde, (unsigned long long)tipo);
}

ffxReturnCode_t CriaContexto(ffxContext* ctx, ffxCreateContextDescHeader* desc, const ffxAllocationCallbacks* mem)
{
    for (auto* h = desc; h; h = h->pNext)
        DizTipo("criacao", h->type);
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

// sem as flags (o contexto foi criado antes de o modulo entrar): o FSR pede profundidade na resolucao de render, entao
// so o movimento na saida tem textura do tamanho da saida e maior que a da profundidade. Motor que aloca tudo no tamanho
// da saida e desenha no canto tem as duas iguais: movimento de render
constexpr bool MovNaSaida(uint32_t renderW, uint32_t saidaW, uint32_t saidaH, uint64_t movW, uint32_t movH, uint64_t profW)
{
    return renderW < saidaW && movW >= saidaW && movH >= saidaH && profW < movW;
}
static_assert(MovNaSaida(1920, 3840, 2160, 3840, 2160, 1920));   // movimento na saida, profundidade no render
static_assert(!MovNaSaida(1920, 3840, 2160, 1920, 1080, 1920));  // os dois no render
static_assert(!MovNaSaida(1920, 3840, 2160, 3840, 2160, 3840));  // tudo alocado no tamanho da saida
static_assert(!MovNaSaida(3840, 3840, 2160, 3840, 2160, 1920));  // sem upscale os dois espacos sao o mesmo
// o cabecalho da AMD e o contrato com a DLL do jogo (o modulo le estes campos da memoria que o jogo passa): FSR 3.1 ao 4.x
static_assert(sizeof(ffxDispatchDescUpscale) == 432 && offsetof(ffxDispatchDescUpscale, motionVectors) == 120 &&
              offsetof(ffxDispatchDescUpscale, output) == 312 && offsetof(ffxDispatchDescUpscale, renderSize) == 376 &&
              offsetof(ffxDispatchDescUpscale, reset) == 408 && offsetof(ffxDispatchDescUpscale, flags) == 428);

// ---- o orcamento de VRAM que o jogo ve ----
// As quedas com a geracao de quadros sao falha de pagina num recurso que existe e nao e nosso, sempre na mesma regiao da memoria da GPU, com o jogo a 94-96% do orcamento. Abrir o overlay encolhe esse
// orcamento (medido no jogo de teste: 13,7 -> 11,8 GB). Enquanto o overlay estiver ligado, o jogo ve o orcamento sem esse desconto e nao entra num caminho de falta de memoria que sem o overlay ele
// nao veria. Nada aqui limita a rede ou o overlay. A causa ainda nao esta provada: o log conta o que o jogo faz (Evict, tiles) e, se cair, o nome do recurso diz quem foi
using PfnOrcamento = HRESULT(STDMETHODCALLTYPE*)(IDXGIAdapter3*, UINT, DXGI_MEMORY_SEGMENT_GROUP, DXGI_QUERY_VIDEO_MEMORY_INFO*);
using PfnResidencia = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, ID3D12Pageable* const*);
using PfnTiles = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Resource*, UINT, const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*, ID3D12Heap*, UINT,
                                          const D3D12_TILE_RANGE_FLAGS*, const UINT*, const UINT*, D3D12_TILE_MAPPING_FLAGS);
PfnOrcamento oOrcamento = nullptr;
PfnResidencia oResidente = nullptr, oTira = nullptr;
PfnTiles oTiles = nullptr;
size_t g_tamEu = 0;
struct ContaResidencia
{
    std::atomic<uint64_t> perguntas{0}, tira{0}, tiraObj{0}, tiraEm{0}, residente{0}, residenteObj{0}, tiles{0}, tilesSemHeap{0};
    std::atomic<UINT64> real{0}, visto{0}, uso{0};
} g_res;

void LogResidencia(const char* quando)
{
    const uint64_t em = g_res.tiraEm.load();
    char ha[40] = "nunca";
    if (em)
        snprintf(ha, sizeof ha, "ha %llu ms", (unsigned long long)(GetTickCount64() - em));
    Log("residencia%s: o jogo perguntou o orcamento %llu vezes (real %llu MB, ele ve %llu, usa %llu); Evict %llu vezes, %llu objetos, o ultimo %s; MakeResident %llu vezes, %llu objetos; "
        "UpdateTileMappings %llu vezes, %llu soltando",
        quando, (unsigned long long)g_res.perguntas.exchange(0), (unsigned long long)(g_res.real.load() >> 20), (unsigned long long)(g_res.visto.load() >> 20),
        (unsigned long long)(g_res.uso.load() >> 20), (unsigned long long)g_res.tira.exchange(0), (unsigned long long)g_res.tiraObj.exchange(0), ha,
        (unsigned long long)g_res.residente.exchange(0), (unsigned long long)g_res.residenteObj.exchange(0), (unsigned long long)g_res.tiles.exchange(0),
        (unsigned long long)g_res.tilesSemHeap.exchange(0));
}

bool OverlayLigado()
{
    if (OverlayDaApiVivo())
        return true;
    const dlss5ov::Cabecalho* c = g_clienteOk ? g_cli.Cab() : g_cabNome;
    return c && GetTickCount64() - c->batimentoOverlay < 5000;
}

HRESULT STDMETHODCALLTYPE Orcamento(IDXGIAdapter3* a, UINT no, DXGI_MEMORY_SEGMENT_GROUP grupo, DXGI_QUERY_VIDEO_MEMORY_INFO* info)
{
    const HRESULT hr = oOrcamento(a, no, grupo, info);
    if (FAILED(hr) || !info || grupo != DXGI_MEMORY_SEGMENT_GROUP_LOCAL)
        return hr;
    // o log do modulo ve o numero real
    const auto* ra = static_cast<const uint8_t*>(_ReturnAddress());
    const auto* eu = reinterpret_cast<const uint8_t*>(g_eu);
    if (ra >= eu && ra < eu + g_tamEu)
        return hr;
    ++g_res.perguntas;
    g_res.real = info->Budget;
    g_res.uso = info->CurrentUsage;
    if (OverlayLigado())
    {
        DXGI_ADAPTER_DESC d{};
        if (SUCCEEDED(a->GetDesc(&d)))
            info->Budget = std::max(info->Budget, std::min(info->Budget + dlss5ov::kVramDoOverlay, UINT64(d.DedicatedVideoMemory)));
    }
    g_res.visto = info->Budget;
    return hr;
}

void EnganchaOrcamento()
{
    Microsoft::WRL::ComPtr<IDXGIFactory4> f;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> a3;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) || FAILED(f->EnumAdapters1(0, &a)) || FAILED(a.As(&a3)))
    {
        Log("orcamento de VRAM: gancho nao entrou (sem o adaptador)");
        return;
    }
    const auto* eu = reinterpret_cast<const uint8_t*>(g_eu);
    g_tamEu = reinterpret_cast<const IMAGE_NT_HEADERS*>(eu + reinterpret_cast<const IMAGE_DOS_HEADER*>(eu)->e_lfanew)->OptionalHeader.SizeOfImage;
    oOrcamento = reinterpret_cast<PfnOrcamento>((*reinterpret_cast<void***>(a3.Get()))[14]); // IDXGIAdapter3::QueryVideoMemoryInfo
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&oOrcamento), reinterpret_cast<PVOID>(&Orcamento));
    Log("orcamento de VRAM: gancho %s", DetourTransactionCommit() == NO_ERROR ? "ok" : "falhou");
}

// ---- quem e o recurso da falha de pagina ----
// O jogo nao da nome aos recursos dele, e o DRED so diz o nome. O que a AMD cria ja tem nome ("AMD FSR Replacement BackBuffer"...); o que e nosso tambem ("DLSS5Overlay")
void Nomeia(void* r, const wchar_t* nome)
{
    if (!r)
        return;
    static std::mutex mx;
    static std::unordered_map<void*, bool> vistos;
    std::lock_guard<std::mutex> lk(mx);
    if (vistos.size() < 4096 && vistos.emplace(r, true).second)
        static_cast<ID3D12Resource*>(r)->SetName(nome);
}

HRESULT STDMETHODCALLTYPE Residente(ID3D12Device* d, UINT n, ID3D12Pageable* const* p)
{
    ++g_res.residente;
    g_res.residenteObj += n;
    for (UINT i = 0; i < n && i < 64; ++i)
        if (p[i])
            p[i]->SetName(L"DLSS5: voltou para a VRAM (MakeResident)");
    return oResidente(d, n, p);
}

HRESULT STDMETHODCALLTYPE Tira(ID3D12Device* d, UINT n, ID3D12Pageable* const* p)
{
    ++g_res.tira;
    g_res.tiraObj += n;
    g_res.tiraEm = GetTickCount64();
    for (UINT i = 0; i < n && i < 64; ++i)
        if (p[i])
            p[i]->SetName(L"DLSS5: o jogo tirou da VRAM (Evict)");
    return oTira(d, n, p);
}

void STDMETHODCALLTYPE Tiles(ID3D12CommandQueue* q, ID3D12Resource* r, UINT nr, const D3D12_TILED_RESOURCE_COORDINATE* c, const D3D12_TILE_REGION_SIZE* t, ID3D12Heap* h, UINT nf,
                             const D3D12_TILE_RANGE_FLAGS* f, const UINT* ini, const UINT* qt, D3D12_TILE_MAPPING_FLAGS fl)
{
    ++g_res.tiles;
    if (!h)
        ++g_res.tilesSemHeap;
    Nomeia(r, L"DLSS5: recurso com tiles");
    oTiles(q, r, nr, c, t, h, nf, f, ini, qt, fl);
}

void GanchoResidencia(ID3D12Device* d)
{
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> q;
    D3D12_COMMAND_QUEUE_DESC qd{};
    void** vt = *reinterpret_cast<void***>(d);
    oResidente = reinterpret_cast<PfnResidencia>(vt[34]);
    oTira = reinterpret_cast<PfnResidencia>(vt[35]);
    const bool temFila = SUCCEEDED(d->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)));
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&oResidente), reinterpret_cast<PVOID>(&Residente));
    DetourAttach(reinterpret_cast<PVOID*>(&oTira), reinterpret_cast<PVOID>(&Tira));
    if (temFila)
    {
        oTiles = reinterpret_cast<PfnTiles>((*reinterpret_cast<void***>(q.Get()))[8]); // UpdateTileMappings
        DetourAttach(reinterpret_cast<PVOID*>(&oTiles), reinterpret_cast<PVOID>(&Tiles));
    }
    Log("ganchos de residencia (Evict, MakeResident, UpdateTileMappings): %s", DetourTransactionCommit() == NO_ERROR ? "ok" : "falharam");
}

// As estruturas da geracao de quadros, as do ffx_framegeneration.h e do ffx_api_dx12.h do SDK (este modulo compila com os cabecalhos so do upscale)
struct ConfigFg
{
    ffxConfigureDescHeader header;
    void* swapChain;
    void* presentCallback;
    void* presentCallbackContexto;
    void* geracaoCallback;
    void* geracaoCallbackContexto;
    bool ligada;
    bool assincrona;
    FfxApiResource semHud;
};
struct PreparaFg
{
    ffxDispatchDescHeader header;
    uint64_t frameID;
    uint32_t flags;
    void* lista;
    FfxApiDimensions2D render;
    FfxApiFloatCoords2D jitter, escalaMov;
    float dt;
    bool reset;
    float perto, longe, fov, metros;
    FfxApiResource prof, mov;
};
struct UiFg
{
    ffxConfigureDescHeader header;
    FfxApiResource ui;
    uint32_t flags;
};
static_assert(offsetof(ConfigFg, semHud) == 64 && offsetof(PreparaFg, prof) == 88 && offsetof(PreparaFg, mov) == 136 && offsetof(UiFg, ui) == 16);
constexpr uint64_t kTipoConfigFg = 0x00020002u, kTipoPreparaFg = 0x00020004u, kTipoUiFg = 0x00030002u;

PfnFfxConfigure oConfigura = nullptr;
ffxReturnCode_t Configura(ffxContext* ctx, const ffxConfigureDescHeader* desc)
{
    for (const auto* h = desc; h; h = h->pNext)
    {
        DizTipo("configuracao", h->type);
        if (h->type == kTipoConfigFg)
        {
            const auto* c = reinterpret_cast<const ConfigFg*>(h);
            Nomeia(c->semHud.resource, L"DLSS5: jogo, sem HUD para a geracao");
            static int dito = -1;
            const int agora = (c->ligada ? 1 : 0) | (c->assincrona ? 2 : 0) | (c->semHud.resource ? 4 : 0);
            if (agora != dito)
            {
                dito = agora;
                Log("geracao de quadros pelo jogo: %s, %s, cor sem HUD %s", c->ligada ? "ligada" : "desligada", c->assincrona ? "assincrona" : "na fila do jogo", c->semHud.resource ? "entregue" : "nao entregue");
            }
        }
        else if (h->type == kTipoUiFg)
            Nomeia(reinterpret_cast<const UiFg*>(h)->ui.resource, L"DLSS5: jogo, UI para a geracao");
    }
    return oConfigura(ctx, desc);
}

void NomeiaEntradas(const ffxDispatchDescHeader* desc)
{
    for (const auto* h = desc; h; h = h->pNext)
        if (h->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE)
        {
            const auto* u = reinterpret_cast<const ffxDispatchDescUpscale*>(h);
            Nomeia(u->color.resource, L"DLSS5: jogo, cor para o FSR");
            Nomeia(u->depth.resource, L"DLSS5: jogo, profundidade para o FSR");
            Nomeia(u->motionVectors.resource, L"DLSS5: jogo, movimento para o FSR");
            Nomeia(u->exposure.resource, L"DLSS5: jogo, exposicao para o FSR");
            Nomeia(u->reactive.resource, L"DLSS5: jogo, reativa para o FSR");
            Nomeia(u->transparencyAndComposition.resource, L"DLSS5: jogo, transparencia para o FSR");
            Nomeia(u->output.resource, L"DLSS5: jogo, saida do FSR");
        }
        else if (h->type == kTipoPreparaFg)
        {
            const auto* p = reinterpret_cast<const PreparaFg*>(h);
            Nomeia(p->prof.resource, L"DLSS5: jogo, profundidade para a geracao");
            Nomeia(p->mov.resource, L"DLSS5: jogo, movimento para a geracao");
        }
}

ffxReturnCode_t Despacha(ffxContext* ctx, const ffxDispatchDescHeader* desc)
{
    if (desc)
    {
        DizTipo("despacho", desc->type);
        NomeiaEntradas(desc);
    }
    const ffxReturnCode_t r = oDespacha(ctx, desc);
    if (r != FFX_API_RETURN_OK || !desc || desc->type != FFX_API_DISPATCH_DESC_TYPE_UPSCALE)
        return r;
    const auto* u = reinterpret_cast<const ffxDispatchDescUpscale*>(desc);
    auto* lista = static_cast<ID3D12GraphicsCommandList*>(u->commandList);
    auto* cor = static_cast<ID3D12Resource*>(u->output.resource);
    auto* mov = static_cast<ID3D12Resource*>(u->motionVectors.resource);
    auto* prof = static_cast<ID3D12Resource*>(u->depth.resource);
    NotaProfFsr(prof);
    LimiteNoDespacho();
    if (!lista || !cor || !mov || !prof)
        return r;
    g_listaFsrNativa = dlss5ov::ListaNativa(lista); // o relato do dispositivo removido aponta se a lista e esta
    // o gancho dos envios a GPU no primeiro quadro; o vigia confere se a GPU removeu este dispositivo
    static std::once_flag filaUma;
    std::call_once(filaUma, [&] {
        Microsoft::WRL::ComPtr<ID3D12Device> d;
        if (SUCCEEDED(lista->GetDevice(IID_PPV_ARGS(&d))))
        {
            d.Get()->AddRef(); // segurado: nunca solto
            g_devVigia = d.Get();
            GanchoFila(d.Get());
            GanchoResidencia(d.Get());
        }
    });
    ContaQuadro();
    if (g_clienteOk)
        g_cli.EsperaEdicao(g_mx); // sem atraso: a edicao do quadro anterior, sem segurar o mutex (o Execute usa)
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_clienteOk)
    {
        static ULONGLONG proximaTentativa = 0;
        if (g_clienteFalhou || GetTickCount64() < proximaTentativa)
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
        Log(g_clienteOk ? "cliente pronto (%s)" : "cliente falhou: %s", g_clienteOk ? nome : g_cli.UltimoErro());
        if (!g_clienteOk)
        {
            // uma falha momentanea (memoria, handles) nao desliga a captura ate reabrir o jogo: tenta de novo em 5 s;
            // falha de compilacao nao passa sozinha
            g_cli.Encerra();
            g_dev->Release();
            g_dev = nullptr;
            proximaTentativa = GetTickCount64() + 5000;
            g_clienteFalhou = dlss5ov::ClienteBuffer::CompilacaoFalhou();
            return r;
        }
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
    const D3D12_RESOURCE_DESC dMov = mov->GetDesc();
    const bool movSaida = conhecidas ? (flags & FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) != 0
                                     : MovNaSaida(u->renderSize.width, u->upscaleSize.width, u->upscaleSize.height, dMov.Width,
                                                  dMov.Height, prof->GetDesc().Width);
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
        // por que a captura nao liga: sem isto, "cliente: 0 quadros" era tudo o que o registro dizia
        static std::string erroDito;
        static uint32_t foraDito = 0;
        if (erroDito != g_cli.ErroTexturas())
        {
            erroDito = g_cli.ErroTexturas();
            if (!erroDito.empty())
                Log("texturas nao criadas: %s; tenta de novo a cada 2 s", g_cli.Cab()->erroJogo);
        }
        if (foraDito != g_cli.ForaDoOrcamento())
        {
            foraDito = g_cli.ForaDoOrcamento();
            if (foraDito)
                Log("%u texturas criadas acima do orcamento de VRAM do jogo (sem elas a captura nao liga)", foraDito);
        }
    }
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
            if (d.denoise || g_cli.ErroDenoise()[0])
                Log("denoise: %llu quadros em %.1f s%s%s", (unsigned long long)d.denoise, (agora - marca) / 1000.0,
                    g_cli.ErroDenoise()[0] ? "; falhou: " : "", g_cli.ErroDenoise());
            if (const std::string v = g_cli.Vram(); !v.empty())
                Log("memoria%s", v.c_str());
            LogResidencia("");
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
            "fov vertical %.1f graus%s, flags %s 0x%x, movimento na resolucao de %s", (unsigned long long)dc.Width, dc.Height,
            (int)dc.Format, u->renderSize.width, u->renderSize.height, u->motionVectorScale.x, u->motionVectorScale.y,
            u->cameraNear, u->cameraFar, u->cameraFovAngleVertical * 57.29578f,
            dlss5ov::TanMeioFov(u->cameraFovAngleVertical) > 0.f ? " (distancia radial)" : " (desconhecido: Z da camera)",
            conhecidas ? "conhecidas" : "deduzidas dos recursos (o contexto ja existia)", flags & 0x7fffffffu,
            movSaida ? "saida" : "render");
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

// acessos invalidos (o TLOU2 caiu com 0xc0000005 no proprio codigo, com o overlay conectando): onde foi e a pilha, no log.
// So registra (o jogo pode tratar o dele); no maximo 5
// ---- dispositivo removido ----
// O TLOU2 para (de proposito: escreve o codigo em 0x0) com DXGI_ERROR_INVALID_CALL quando GetDeviceRemovedReason diz
// que a GPU removeu o dispositivo por uma chamada invalida -- um comando invalido numa lista enviada. O DRED da
// Microsoft (Device Removed Extended Data) so vale para o dispositivo criado depois de ligado: com o jogo ja aberto, o
// dele fica sem (o log diz "sem dados"). Guarda o ultimo comando que cada lista completou e a falha de pagina; o vigia
// confere o dispositivo a cada 0,5 s (e o acesso invalido tambem) e, se caiu, poe tudo no log. As listas que o runtime recusa ao fechar (um comando invalido gravado nelas) tambem vao para o
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
        Log("DRED (o que a GPU fazia se o dispositivo cair): %s", ok ? "ligado para os dispositivos criados daqui em diante" : "indisponivel");
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
    LogResidencia(" na queda");
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
    int relatos = 0;
    bool relatado = false;
    ULONGLONG travadaDesde = 0;
    for (;;)
    {
        Sleep(500);
        const ULONGLONG agora = GetTickCount64(), ult = g_ultimoQuadro.load();
        const HWND j = g_janelaJogo.load();
        ConfereDispositivo();
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
    EnganchaOrcamento();
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
                if (!oCriaContexto || !oDespacha)
                    continue;
                DetourTransactionBegin();
                DetourUpdateThread(GetCurrentThread());
                DetourAttach(reinterpret_cast<PVOID*>(&oCriaContexto), reinterpret_cast<PVOID>(&CriaContexto));
                DetourAttach(reinterpret_cast<PVOID*>(&oDespacha), reinterpret_cast<PVOID>(&Despacha));
                if (oConfigura)
                    DetourAttach(reinterpret_cast<PVOID*>(&oConfigura), reinterpret_cast<PVOID>(&Configura));
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

// o Log para os outros arquivos do modulo
void LogModulo(const char* f, ...)
{
    char b[512];
    va_list a;
    va_start(a, f);
    vsnprintf(b, sizeof b, f, a);
    va_end(a);
    Log("%s", b);
}
void IniciaCapturaApi();

BOOL APIENTRY DllMain(HMODULE m, DWORD motivo, LPVOID)
{
    if (motivo == DLL_PROCESS_ATTACH)
    {
        g_eu = m;
        DisableThreadLibraryCalls(m);
        wchar_t p[MAX_PATH];
        GetModuleFileNameW(m, p, MAX_PATH);
        std::wstring s(p);
        s = s.substr(0, s.find_last_of(L"\\/")) + L"\\DLSS5Buffer.log";
        // o do jogo anterior fica: abrir outro jogo depois de uma queda apagava a unica prova
        MoveFileExW(s.c_str(), (s.substr(0, s.size() - 3) + L"anterior.log").c_str(), MOVEFILE_REPLACE_EXISTING);
        g_log = _wfsopen(s.c_str(), L"w", 0x40 /* _SH_DENYNO */);
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        // uma copia so por processo: a segunda nao faz nada (o evento fica aberto ate o processo sair: e como o overlay ve
        // que o modulo esta aqui)
        wchar_t nome[96];
        dlss5ov::NomeModulo(nome, 96, GetCurrentProcessId());
        HANDLE presente = CreateEventW(nullptr, TRUE, FALSE, nome);
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            if (presente)
                CloseHandle(presente);
            return TRUE;
        }
        Log("DLSS5Buffer.dll carregada em %s (pid %lu)", exe, GetCurrentProcessId());
        IniciaCapturaApi();
        if (HANDLE t = CreateThread(nullptr, 0, Instala, nullptr, 0, nullptr))
            CloseHandle(t);
    }
    return TRUE;
}
