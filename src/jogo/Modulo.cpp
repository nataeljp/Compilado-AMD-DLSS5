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
#include "ffx_api.h"
#include "ffx_upscale.h"
#include <detours.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
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
        g_log = _wfsopen(s.c_str(), L"w", 0x40 /* _SH_DENYNO */);
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        Log("DLSS5Buffer.dll carregada em %s (pid %lu)", exe, GetCurrentProcessId());
        if (HANDLE t = CreateThread(nullptr, 0, Instala, nullptr, 0, nullptr))
            CloseHandle(t);
    }
    return TRUE;
}
