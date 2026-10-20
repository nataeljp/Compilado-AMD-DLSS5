// DLSS5Buffer.dll: o modulo que o DLSS5 Overlay coloca no jogo aberto (Modo de Rede = captura de buffer).
// Nao roda rede nenhuma: so pega, depois do upscale, o que o jogo entrega ao FSR 3.1+ (cor sem HUD, profundidade,
// movimento, exposicao, camera) e conversa com o overlay pelo ClienteBuffer (buffer/Protocolo.h). Ganchos:
//   - ffxCreateContext / ffxDispatch da DLL do upscaler (amd_fidelityfx_upscaler_dx12.dll; FSR 3.1 antigo:
//     amd_fidelityfx_dx12.dll): depois do despacho de upscale, grava os passes do cliente na mesma lista;
//   - ID3D12CommandQueue::ExecuteCommandLists: quando essa lista e submetida, sinaliza a cerca do jogo.
// Registro em DLSS5Buffer.log ao lado da DLL.
#include "../buffer/ClienteBuffer.h"
#include "ffx_api.h"
#include "ffx_upscale.h"
#include <detours.h>
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
    uint32_t flags = 0;
    if (ctx)
    {
        auto it = g_flags.find(*ctx);
        if (it != g_flags.end())
            flags = it->second;
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
    static bool primeiro = true;
    if (primeiro)
    {
        primeiro = false;
        const auto dc = cor->GetDesc();
        Log("primeiro quadro: saida %llux%u fmt %d, render %ux%u, movimento escala %.3f,%.3f, perto %.3f longe %.3g, "
            "flags %s 0x%x", (unsigned long long)dc.Width, dc.Height, (int)dc.Format, u->renderSize.width,
            u->renderSize.height, u->motionVectorScale.x, u->motionVectorScale.y, u->cameraNear, u->cameraFar,
            conhecidas ? "conhecidas" : "desconhecidas (padrao do UE5)", flags & 0x7fffffffu);
    }
    return r;
}

// procura a DLL do upscaler do jogo (pode carregar depois: o jogo so carrega o FSR quando ele e escolhido no menu)
DWORD WINAPI Instala(void*)
{
    const wchar_t* candidatos[] = {L"amd_fidelityfx_upscaler_dx12.dll", L"amd_fidelityfx_dx12.dll"};
    for (int volta = 0;; ++volta)
    {
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
