// Captura da API, lado do jogo: engancha o Present da DXGI (o ponto comum do D3D11 e do D3D12) e copia o back buffer para uma
// textura compartilhada. So engancha quando o overlay pede: na captura de buffer nada disto roda, a nao ser com o teto de FPS.
#include "../buffer/LimiteFps.h"
#include "../buffer/ProtocoloApi.h"
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <detours.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

void LogModulo(const char* fmt, ...);

namespace {
using Microsoft::WRL::ComPtr;
using dlss5ov::CabApi;

CabApi* g_cab = nullptr;
std::mutex g_mx;
uint32_t g_geracao = 0;
uint64_t g_n = 0;
std::vector<HANDLE> g_handles;
ULONGLONG g_vivoEm = 0, g_tentaEm = 0, g_alvoVistoEm = 0;

void Erro(const char* oque, HRESULT hr)
{
    snprintf(g_cab->erro, sizeof g_cab->erro, "%s: 0x%08lx", oque, (unsigned long)hr);
    g_tentaEm = GetTickCount64() + 2000; // nao tenta de novo a cada quadro
    static char dito[160] = {};
    if (strcmp(dito, g_cab->erro))
    {
        strcpy_s(dito, g_cab->erro);
        LogModulo("captura da API: %s", g_cab->erro);
    }
}

void FechaHandles()
{
    for (HANDLE h : g_handles)
        CloseHandle(h);
    g_handles.clear();
}

// uma textura compartilhada nao carrega a vista sRGB: copia os mesmos bytes, e o overlay decodifica
DXGI_FORMAT SemSrgb(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
    default: return f;
    }
}

void Publica(UINT w, UINT h, DXGI_FORMAT f, uint32_t api)
{
    g_n = 0;
    g_cab->w = w, g_cab->h = h, g_cab->fmt = f, g_cab->api = api;
    g_cab->quadro[0] = g_cab->quadro[1] = 0;
    g_cab->consumido = 0;
    g_cab->erro[0] = 0;
    MemoryBarrier();
    g_cab->geracao = g_geracao;
    LogModulo("captura da API: D3D%u, back buffer %ux%u, formato %d", api, w, h, int(f));
}

// ---- D3D12 ----
struct Estado12
{
    ComPtr<ID3D12Device> dev;
    ID3D12CommandQueue* fila = nullptr; // so comparacao: a fila vem do jogo a cada quadro
    ComPtr<ID3D12CommandAllocator> al[3];
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> local, cerca;
    UINT64 valor[3] = {}, ultimo = 0;
    UINT idx = 0;
    ComPtr<ID3D12Resource> tex[dlss5ov::kSlotsApi];
    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
} g_12;

// as listas ja enviadas ainda podem usar as texturas
void EsperaLocal12()
{
    if (!g_12.local || g_12.local->GetCompletedValue() >= g_12.ultimo)
        return;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ev && SUCCEEDED(g_12.local->SetEventOnCompletion(g_12.ultimo, ev)))
        WaitForSingleObject(ev, 1000);
    if (ev)
        CloseHandle(ev);
}

void Solta12()
{
    g_cab->geracao = 0;
    EsperaLocal12();
    FechaHandles();
    for (auto& t : g_12.tex)
        t.Reset();
    g_12.cerca.Reset();
    g_12.w = g_12.h = 0;
}

// jogo no limite da VRAM: "the creation APIs will return failure" acima do orcamento; sem residencia a criacao passa
HRESULT CriaTex12(const D3D12_RESOURCE_DESC& rd, ComPtr<ID3D12Resource>& t)
{
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    HRESULT hr = g_12.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t));
    if (hr != E_OUTOFMEMORY)
        return hr;
    hr = g_12.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t));
    if (FAILED(hr))
        return hr;
    ID3D12Pageable* p[] = {t.Get()};
    return g_12.dev->MakeResident(1, p);
}

bool Prepara12(ID3D12CommandQueue* q, UINT w, UINT h, DXGI_FORMAT f)
{
    if (g_12.fila != q)
    {
        Solta12();
        g_12.fila = nullptr;
        for (auto& a : g_12.al)
            a.Reset();
        g_12.cl.Reset();
        g_12.local.Reset();
        g_12.dev.Reset();
        g_12.ultimo = 0;
        for (auto& v : g_12.valor)
            v = 0;
        HRESULT hr = q->GetDevice(IID_PPV_ARGS(&g_12.dev));
        const D3D12_COMMAND_LIST_TYPE tipo = q->GetDesc().Type;
        for (int i = 0; SUCCEEDED(hr) && i < 3; ++i)
            hr = g_12.dev->CreateCommandAllocator(tipo, IID_PPV_ARGS(&g_12.al[i]));
        if (SUCCEEDED(hr))
            hr = g_12.dev->CreateCommandList(0, tipo, g_12.al[0].Get(), nullptr, IID_PPV_ARGS(&g_12.cl));
        if (SUCCEEDED(hr))
            hr = g_12.cl->Close();
        if (SUCCEEDED(hr))
            hr = g_12.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_12.local));
        if (FAILED(hr))
            return Erro("lista de comandos", hr), false;
        g_12.fila = q;
    }
    if (g_12.tex[0] && g_12.w == w && g_12.h == h && g_12.fmt == f)
        return true;
    Solta12();
    ++g_geracao;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.Format = f;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    const DWORD pid = GetCurrentProcessId();
    wchar_t nome[128];
    HANDLE c = nullptr;
    for (uint32_t s = 0; s < dlss5ov::kSlotsApi; ++s)
    {
        HRESULT hr = CriaTex12(rd, g_12.tex[s]);
        if (FAILED(hr))
            return Erro("textura", hr), Solta12(), false;
        dlss5ov::NomeRecursoApi(nome, 128, pid, g_geracao, L"cor", s);
        hr = g_12.dev->CreateSharedHandle(g_12.tex[s].Get(), nullptr, GENERIC_ALL, nome, &c);
        if (FAILED(hr))
            return Erro("nome da textura", hr), Solta12(), false;
        g_handles.push_back(c);
    }
    HRESULT hr = g_12.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_12.cerca));
    dlss5ov::NomeRecursoApi(nome, 128, pid, g_geracao, L"cerca", 0);
    if (SUCCEEDED(hr))
        hr = g_12.dev->CreateSharedHandle(g_12.cerca.Get(), nullptr, GENERIC_ALL, nome, &c);
    if (FAILED(hr))
        return Erro("cerca", hr), Solta12(), false;
    g_handles.push_back(c);
    g_12.w = w, g_12.h = h, g_12.fmt = f;
    Publica(w, h, f, 12);
    return true;
}

void Copia12(IDXGISwapChain* sc, ID3D12CommandQueue* q)
{
    ComPtr<IDXGISwapChain3> sc3;
    ComPtr<ID3D12Resource> bb;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) || FAILED(sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb))))
        return;
    const D3D12_RESOURCE_DESC d = bb->GetDesc();
    if (!Prepara12(q, UINT(d.Width), d.Height, SemSrgb(d.Format)))
        return;
    const UINT s = UINT((g_n + 1) % dlss5ov::kSlotsApi), i = g_12.idx;
    if (g_cab->consumido < g_cab->quadro[s] || g_12.local->GetCompletedValue() < g_12.valor[i])
        return; // o overlay ainda le o slot, ou a GPU ainda usa a lista: este quadro nao vai
    if (FAILED(g_12.al[i]->Reset()) || FAILED(g_12.cl->Reset(g_12.al[i].Get(), nullptr)))
        return;
    D3D12_RESOURCE_BARRIER b{};
    b.Transition = {bb.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE};
    g_12.cl->ResourceBarrier(1, &b);
    g_12.cl->CopyResource(g_12.tex[s].Get(), bb.Get());
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_12.cl->ResourceBarrier(1, &b);
    if (FAILED(g_12.cl->Close()))
        return;
    ID3D12CommandList* l[] = {g_12.cl.Get()};
    q->ExecuteCommandLists(1, l);
    const uint64_t n = ++g_n;
    q->Signal(g_12.cerca.Get(), n);
    q->Signal(g_12.local.Get(), ++g_12.ultimo);
    g_12.valor[i] = g_12.ultimo;
    g_12.idx = (i + 1) % 3;
    g_cab->quadro[s] = n;
}

// ---- D3D11 ----
struct Estado11
{
    ComPtr<ID3D11Device5> dev;
    ComPtr<ID3D11DeviceContext4> ctx;
    ID3D11Device* origem = nullptr; // so comparacao
    ComPtr<ID3D11Fence> cerca;
    ComPtr<ID3D11Texture2D> tex[dlss5ov::kSlotsApi];
    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
} g_11;

void Solta11()
{
    g_cab->geracao = 0;
    if (g_11.ctx)
        g_11.ctx->Flush();
    FechaHandles();
    for (auto& t : g_11.tex)
        t.Reset();
    g_11.cerca.Reset();
    g_11.w = g_11.h = 0;
}

bool Prepara11(ID3D11Device* d, UINT w, UINT h, DXGI_FORMAT f)
{
    if (g_11.origem != d)
    {
        Solta11();
        g_11.origem = nullptr;
        g_11.ctx.Reset();
        g_11.dev.Reset();
        ComPtr<ID3D11DeviceContext> c;
        d->GetImmediateContext(&c);
        HRESULT hr = d->QueryInterface(IID_PPV_ARGS(&g_11.dev));
        if (SUCCEEDED(hr))
            hr = c.As(&g_11.ctx);
        if (FAILED(hr))
            return Erro("dispositivo D3D11.4", hr), false;
        g_11.origem = d;
    }
    if (g_11.tex[0] && g_11.w == w && g_11.h == h && g_11.fmt == f)
        return true;
    Solta11();
    ++g_geracao;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = f;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    const DWORD pid = GetCurrentProcessId();
    wchar_t nome[128];
    HANDLE c = nullptr;
    for (uint32_t s = 0; s < dlss5ov::kSlotsApi; ++s)
    {
        ComPtr<IDXGIResource1> r;
        HRESULT hr = g_11.dev->CreateTexture2D(&td, nullptr, &g_11.tex[s]);
        if (SUCCEEDED(hr))
            hr = g_11.tex[s].As(&r);
        dlss5ov::NomeRecursoApi(nome, 128, pid, g_geracao, L"cor", s);
        if (SUCCEEDED(hr))
            hr = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nome, &c);
        if (FAILED(hr))
            return Erro("textura", hr), Solta11(), false;
        g_handles.push_back(c);
    }
    HRESULT hr = g_11.dev->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_11.cerca));
    dlss5ov::NomeRecursoApi(nome, 128, pid, g_geracao, L"cerca", 0);
    if (SUCCEEDED(hr))
        hr = g_11.cerca->CreateSharedHandle(nullptr, GENERIC_ALL, nome, &c);
    if (FAILED(hr))
        return Erro("cerca", hr), Solta11(), false;
    g_handles.push_back(c);
    g_11.w = w, g_11.h = h, g_11.fmt = f;
    Publica(w, h, f, 11);
    return true;
}

void Copia11(IDXGISwapChain* sc, ID3D11Device* d)
{
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb))))
        return;
    D3D11_TEXTURE2D_DESC bd{};
    bb->GetDesc(&bd);
    const DXGI_FORMAT f = SemSrgb(bd.Format);
    if (!Prepara11(d, bd.Width, bd.Height, f))
        return;
    const UINT s = UINT((g_n + 1) % dlss5ov::kSlotsApi);
    if (g_cab->consumido < g_cab->quadro[s])
        return;
    if (bd.SampleDesc.Count > 1)
        g_11.ctx->ResolveSubresource(g_11.tex[s].Get(), 0, bb.Get(), 0, f);
    else
        g_11.ctx->CopyResource(g_11.tex[s].Get(), bb.Get());
    const uint64_t n = ++g_n;
    g_11.ctx->Signal(g_11.cerca.Get(), n);
    g_11.ctx->Flush();
    g_cab->quadro[s] = n;
}

// ---- a fila do D3D12 ----
// a DXGI nao documenta como pedir a fila ao swapchain (GetDevice recusa nas versoes atuais; confere no swapchain auxiliar). A
// copia tem de ir na fila que desenhou o quadro, depois do que ela enviou: com outra, o quadro pode nao ter terminado. Toda fila
// DIRECT que enviou listas fica guardada (com a referencia: o jogo pode solta-la).
bool g_filaDoSwap = false;
constexpr int kMaxFilas = 16;
std::atomic<ID3D12CommandQueue*> g_filas[kMaxFilas] = {};
std::atomic<int> g_nFilas{0};
std::atomic<ID3D12CommandQueue*> g_ultimaFila{nullptr};
std::mutex g_mxFilas;
thread_local ID3D12CommandQueue* t_ultimaFila = nullptr;
using PfnExecuta = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
PfnExecuta oExecuta = nullptr;
void STDMETHODCALLTYPE GExecuta(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* l)
{
    if (q != t_ultimaFila && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        {
            std::lock_guard<std::mutex> lk(g_mxFilas);
            const int total = g_nFilas.load();
            int i = 0;
            while (i < total && g_filas[i].load() != q)
                ++i;
            if (i == total)
            {
                q->AddRef();
                if (total < kMaxFilas)
                {
                    g_filas[total].store(q);
                    g_nFilas.store(total + 1);
                }
            }
        }
        t_ultimaFila = q;
        g_ultimaFila.store(q);
    }
    oExecuta(q, n, l);
}

// o swapchain guarda a fila em que foi criado, num deslocamento que muda com a versao do Windows: procura nele as filas conhecidas
ID3D12CommandQueue* FilaDentroDoSwap(IDXGISwapChain* sc)
{
    ID3D12CommandQueue* conhecidas[kMaxFilas];
    const int total = g_nFilas.load();
    for (int i = 0; i < total; ++i)
        conhecidas[i] = g_filas[i].load();
    __try
    {
        void* const* p = reinterpret_cast<void* const*>(sc);
        for (size_t k = 0; k < 0x800 / sizeof(void*); ++k)
            for (int i = 0; i < total; ++i)
                if (p[k] == conhecidas[i])
                    return conhecidas[i];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return nullptr;
}

ID3D12CommandQueue* FilaDoPresent(IDXGISwapChain* sc)
{
    static IDXGISwapChain* visto = nullptr;
    static ID3D12CommandQueue* dele = nullptr;
    static bool dito = false;
    if (sc != visto || !dele)
    {
        visto = sc;
        dele = FilaDentroDoSwap(sc);
    }
    ID3D12CommandQueue* q = dele ? dele : t_ultimaFila ? t_ultimaFila : g_ultimaFila.load();
    if (q && !dito)
    {
        dito = true;
        LogModulo("captura da API: fila do D3D12 %s", dele ? "achada no swapchain" : "pela ultima lista enviada");
    }
    return q;
}

// ---- o Present ----
thread_local int t_dentro = 0; // Present1 pode passar pelo Present
dlss5ov::LimiteFps g_limite;

void NoPresent(IDXGISwapChain* sc, UINT flags)
{
    if (!g_cab || t_dentro || (flags & DXGI_PRESENT_TEST))
        return;
    std::unique_lock<std::mutex> lk(g_mx, std::try_to_lock);
    if (!lk.owns_lock())
        return;
    const ULONGLONG agora = GetTickCount64();
    g_cab->batimentoJogo = agora;
    if (agora - g_cab->batimentoOverlay >= 1000)
    {
        // o overlay fechou: a VRAM volta para o jogo
        if (agora - g_vivoEm > 5000 && g_cab->geracao)
            g_cab->api == 12 ? Solta12() : Solta11();
        return;
    }
    if (agora - g_vivoEm >= 1000)
        g_alvoVistoEm = agora; // o overlay voltou: a janela alvo tem 1 s para aparecer
    g_vivoEm = agora;
    if (agora < g_tentaEm)
        return;
    // a janela alvo primeiro; outro swapchain (janela escolhida errada, jogo que apresenta em outra) so se ela parou
    DXGI_SWAP_CHAIN_DESC sd{};
    HWND raiz = nullptr;
    if (SUCCEEDED(sc->GetDesc(&sd)) && sd.OutputWindow)
        raiz = GetAncestor(sd.OutputWindow, GA_ROOT);
    if (raiz && raiz == reinterpret_cast<HWND>(uintptr_t(g_cab->janela)))
        g_alvoVistoEm = agora;
    else if (agora - g_alvoVistoEm < 1000 || sd.BufferDesc.Width < 320 || sd.BufferDesc.Height < 200)
        return;
    g_cab->janelaSwap = uint64_t(reinterpret_cast<uintptr_t>(raiz));
    ComPtr<ID3D12Device> d12;
    ComPtr<ID3D11Device> d11;
    if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&d12))))
    {
        if (g_11.tex[0])
            Solta11();
        ComPtr<ID3D12CommandQueue> q;
        if (g_filaDoSwap)
            sc->GetDevice(IID_PPV_ARGS(&q));
        else
            q = FilaDoPresent(sc);
        if (q)
            Copia12(sc, q.Get());
    }
    else if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&d11))))
    {
        if (g_12.tex[0])
            Solta12();
        Copia11(sc, d11.Get());
    }
    else
        Erro("o swapchain nao e D3D11 nem D3D12", E_NOINTERFACE);
}

using PfnPresent = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PfnPresent1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using PfnEspaco = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
// o swapchain do D3D11 e o do D3D12 podem ter as funcoes em enderecos diferentes: um gancho para cada
PfnPresent oPresent[2] = {};
PfnPresent1 oPresent1[2] = {};
PfnEspaco oEspaco[2] = {};
template <int I> HRESULT STDMETHODCALLTYPE GPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    NoPresent(sc, flags);
    ++t_dentro;
    const HRESULT hr = oPresent[I](sc, sync, flags);
    --t_dentro;
    if (!t_dentro && !(flags & DXGI_PRESENT_TEST))
        g_limite.Espera(g_cab);
    return hr;
}
template <int I> HRESULT STDMETHODCALLTYPE GPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p)
{
    NoPresent(sc, flags);
    ++t_dentro;
    const HRESULT hr = oPresent1[I](sc, sync, flags, p);
    --t_dentro;
    if (!t_dentro && !(flags & DXGI_PRESENT_TEST))
        g_limite.Espera(g_cab);
    return hr;
}
// HDR10 ou SDR em 10 bits: so o SetColorSpace1 diz
template <int I> HRESULT STDMETHODCALLTYPE GEspaco(IDXGISwapChain3* sc, DXGI_COLOR_SPACE_TYPE e)
{
    const HRESULT hr = oEspaco[I](sc, e);
    if (SUCCEEDED(hr) && g_cab)
        g_cab->espacoCor = uint32_t(e) + 1;
    return hr;
}

std::vector<void*> g_enganchados;
template <class Pfn> void Anexa(void* f, Pfn& original, void* gancho)
{
    for (void* e : g_enganchados)
        if (e == f)
            return;
    g_enganchados.push_back(f);
    original = reinterpret_cast<Pfn>(f);
    DetourAttach(reinterpret_cast<PVOID*>(&original), gancho);
}

// os enderecos saem da tabela de um swapchain auxiliar, numa janela escondida
void Engancha(int i, IDXGISwapChain1* sc)
{
    ComPtr<IDXGISwapChain3> sc3;
    void** vt = *reinterpret_cast<void***>(sc);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    Anexa(vt[8], oPresent[i], i ? reinterpret_cast<void*>(&GPresent<1>) : reinterpret_cast<void*>(&GPresent<0>));
    Anexa(vt[22], oPresent1[i], i ? reinterpret_cast<void*>(&GPresent1<1>) : reinterpret_cast<void*>(&GPresent1<0>));
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))))
    {
        void** vt3 = *reinterpret_cast<void***>(sc3.Get());
        Anexa(vt3[38], oEspaco[i], i ? reinterpret_cast<void*>(&GEspaco<1>) : reinterpret_cast<void*>(&GEspaco<0>));
    }
    const LONG r = DetourTransactionCommit();
    LogModulo("captura da API: gancho no Present do %s: %s", i ? "D3D12" : "D3D11", r == NO_ERROR ? "ok" : "falhou");
}

HWND JanelaAuxiliar()
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DLSS5OvApiAuxiliar";
    RegisterClassW(&wc);
    return CreateWindowW(wc.lpszClassName, L"", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
}

ComPtr<IDXGISwapChain1> SwapchainAuxiliar(IUnknown* disp, IDXGIFactory2* f, HWND j)
{
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = sd.Height = 16;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc;
    f->CreateSwapChainForHwnd(disp, j, &sd, nullptr, nullptr, &sc);
    return sc;
}

bool Engancha11(IDXGIFactory2* f, HWND j)
{
    using Pfn = decltype(&D3D11CreateDevice);
    HMODULE m = GetModuleHandleW(L"d3d11.dll");
    Pfn cria = m ? reinterpret_cast<Pfn>(GetProcAddress(m, "D3D11CreateDevice")) : nullptr;
    ComPtr<ID3D11Device> d;
    if (!cria || FAILED(cria(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, nullptr, nullptr)))
        return false;
    ComPtr<IDXGISwapChain1> sc = SwapchainAuxiliar(d.Get(), f, j);
    if (!sc)
        return false;
    Engancha(0, sc.Get());
    return true;
}

bool Engancha12(IDXGIFactory2* f, HWND j)
{
    ComPtr<ID3D12Device> d;
    ComPtr<ID3D12CommandQueue> q, daDxgi;
    D3D12_COMMAND_QUEUE_DESC qd{};
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d))) || FAILED(d->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))))
        return false;
    ComPtr<IDXGISwapChain1> sc = SwapchainAuxiliar(q.Get(), f, j);
    if (!sc)
        return false;
    g_filaDoSwap = SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&daDxgi))) && daDxgi.Get() == q.Get();
    if (!g_filaDoSwap)
    {
        void** vt = *reinterpret_cast<void***>(q.Get());
        oExecuta = reinterpret_cast<PfnExecuta>(vt[10]);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(reinterpret_cast<PVOID*>(&oExecuta), reinterpret_cast<PVOID>(&GExecuta));
        DetourTransactionCommit();
    }
    if (g_filaDoSwap)
        LogModulo("captura da API: fila do D3D12 pelo GetDevice do swapchain");
    Engancha(1, sc.Get());
    return true;
}

DWORD WINAPI Trabalho(void*)
{
    // espera o overlay pedir; o jogo pode carregar a outra API depois
    bool fez11 = false, fez12 = false;
    HWND j = nullptr;
    ComPtr<IDXGIFactory2> f;
    for (;;)
    {
        Sleep(200);
        const ULONGLONG agora = GetTickCount64();
        if (agora - g_cab->batimentoOverlay >= 1000 && agora - g_cab->batimentoLimite >= 1000)
            continue;
        if (!f && FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))))
            continue;
        if (!j)
            j = JanelaAuxiliar();
        if (!fez12 && GetModuleHandleW(L"d3d12.dll"))
            fez12 = Engancha12(f.Get(), j);
        if (!fez11 && GetModuleHandleW(L"d3d11.dll"))
            fez11 = Engancha11(f.Get(), j);
        if (fez11 && fez12)
            break;
    }
    DestroyWindow(j);
    return 0;
}
} // namespace

// a memoria compartilhada sai antes da DLL terminar de carregar: o overlay abre logo depois da injecao
void IniciaCapturaApi()
{
    wchar_t nome[96];
    dlss5ov::NomeMapaApi(nome, 96, GetCurrentProcessId());
    HANDLE mapa = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(CabApi), nome);
    if (!mapa)
        return;
    g_cab = static_cast<CabApi*>(MapViewOfFile(mapa, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CabApi)));
    if (!g_cab)
        return;
    g_cab->versao = dlss5ov::kVersaoApi;
    g_cab->tamanho = sizeof(CabApi);
    if (HANDLE t = CreateThread(nullptr, 0, Trabalho, nullptr, 0, nullptr))
        CloseHandle(t);
}
