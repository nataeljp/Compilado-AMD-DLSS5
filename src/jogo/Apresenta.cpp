// Captura da API, lado do jogo: engancha o Present da DXGI (o ponto comum do D3D11 e do D3D12) e copia o back buffer para uma
// textura compartilhada. So engancha quando o overlay pede: na captura de buffer nada disto roda, a nao ser com o teto de FPS.
#include "../buffer/LimiteFps.h"
#include "../buffer/ProtocoloApi.h"
#include "../buffer/Vram.h"
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcommon.h>
#include <detours.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
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
uint64_t g_presents = 0; // a cada Present, copiando ou nao

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
    for (uint32_t i = 0; i < dlss5ov::kSlotsProf; ++i)
        g_cab->quadroProf[i] = 0;
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
    D3D12_HEAP_PROPERTIES ram = dlss5ov::HeapDaRam();
    HRESULT hr = dlss5ov::QuerRam() ? g_12.dev->CreateCommittedResource(&ram, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t)) : E_FAIL;
    static HRESULT ditoRam = S_FALSE;
    if (dlss5ov::QuerRam() && hr != ditoRam)
    {
        ditoRam = hr;
        LogModulo("captura da API: texturas %s (0x%08lx)", SUCCEEDED(hr) ? "na RAM" : "na VRAM", (unsigned long)hr);
    }
    if (SUCCEEDED(hr))
        return hr;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    hr = g_12.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t));
    if (hr != E_OUTOFMEMORY)
        return hr;
    hr = g_12.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t));
    if (FAILED(hr))
        return hr;
    ID3D12Pageable* p[] = {t.Get()};
    return g_12.dev->MakeResident(1, p);
}

void AposentaProf12();

bool Prepara12(ID3D12CommandQueue* q, UINT w, UINT h, DXGI_FORMAT f)
{
    const bool trocouFila = g_12.fila != q;
    if (trocouFila)
    {
        Solta12();
        g_12.fila = nullptr;
        for (auto& a : g_12.al)
            a.Reset();
        g_12.cl.Reset();
        g_12.local.Reset();
        ComPtr<ID3D12Device> devAntes = g_12.dev; // vivo ate a comparacao: o endereco nao pode ser reaproveitado por outro dispositivo
        g_12.dev.Reset();
        g_12.ultimo = 0;
        for (auto& v : g_12.valor)
            v = 0;
        HRESULT hr = q->GetDevice(IID_PPV_ARGS(&g_12.dev));
        // outro dispositivo: as texturas da profundidade sao do antigo (copiar para elas numa lista do novo e invalido)
        if (devAntes && (FAILED(hr) || g_12.dev.Get() != devAntes.Get()))
            AposentaProf12();
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
    {
        char vram[64];
        dlss5ov::TextoVramJogo(g_12.dev.Get(), vram, sizeof vram);
        // o motivo: as quedas vieram logo depois de recriar, e o log nao dizia por que recriou
        LogModulo("captura da API: criando as texturas (%s, %ux%u formato %d); %s",
                  trocouFila ? "a fila do Present mudou" : !g_12.tex[0] ? "nao havia texturas" : "o tamanho ou o formato mudou", w, h, int(f), vram);
    }
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
        g_12.tex[s]->SetName(L"DLSS5Overlay"); // numa falha de pagina da GPU o DRED diz se a memoria e nossa
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

// a cor e a profundidade do mesmo quadro saem juntas, ou nao saem: profundidade de outro quadro derruba o historico do denoiser
uint64_t g_profUltimaCopia = 0, g_semProf = 0;
void DescartaProfDoQuadro()
{
    const uint64_t n = g_n + 1;
    for (uint32_t p = 0; p < dlss5ov::kSlotsProf; ++p)
        if (g_cab->quadroProf[p] == n)
            g_cab->quadroProf[p] = 0;
}
bool ProfProntaOuDesiste()
{
    const uint64_t n = g_n + 1;
    if (!g_cab->querProf || !g_profUltimaCopia || g_presents - g_profUltimaCopia > 60)
        return true; // este jogo ou esta tela nao tem profundidade: a cor nao espera por ela
    for (uint32_t p = 0; p < dlss5ov::kSlotsProf; ++p)
        if (g_cab->quadroProf[p] == n)
        {
            g_semProf = 0;
            return true;
        }
    return ++g_semProf > 3; // o overlay segura a ultima profundidade se passar disso
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
    {
        DescartaProfDoQuadro(); // o overlay ainda le o slot, ou a GPU ainda usa a lista: este quadro nao vai, nem a profundidade dele
        return;
    }
    if (!ProfProntaOuDesiste())
        return;
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

// ---- a profundidade do jogo no D3D12 ----
// Copiada na propria lista do jogo, no momento em que ele sai de DEPTH_WRITE: e o unico instante em que o estado do recurso e sabido com
// certeza (e funciona com o modulo injetado depois: nao precisa ter visto o CreateDepthStencilView). A lista e GRAVADA antes de se saber de
// que quadro ela e (um motor com thread de render grava o proximo quadro antes do Present deste): o slot so ganha o numero do quadro quando
// a lista e ENVIADA, e um slot enviado nunca e reescrito. Cada saida de escrita do buffer escolhido vai para um slot novo; vale a ultima
// enviada antes do Present, a mais completa.
using PfnBarreira = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
PfnBarreira oBarreira = nullptr;
std::mutex g_mxProf;
ComPtr<ID3D12Resource> g_prof12[dlss5ov::kSlotsProf];
std::vector<HANDLE> g_handlesProf;
uint32_t g_profGeracao = 0;
// Qual buffer e a cena? Tamanho e ordem nao dizem: no Cyberpunk ha um de 3840x2160 depois do FSR, e na boate o primeiro do quadro, do tamanho certo, so tinha
// tres objetos. Se o jogo usa FSR, e o que ele entrega ao FSR. Senao, o primeiro de um quadro inteiro, e o overlay olha o conteudo (FonteApi::AvaliaProf): vazio, pede
// outro, e este nao volta ate acabarem os candidatos. So comparacao, sem referencia. Sumiu por 120 quadros: outro pode entrar.
// Do mesmo tamanho so entra o que aparece num quadro em que nenhum dos escolhidos apareceu: motor que alterna buffers (nao o de outra passagem, que vem junto do principal)
constexpr int kMaxAlvosProf = 8;
const void* g_alvosProf[kMaxAlvosProf] = {};
int g_nAlvosProf = 0;
const void* g_candidatoProf = nullptr;
// alternar e repetir: o Cyberpunk mostrou um buffer do mesmo tamanho uma unica vez, e entrava como se o jogo alternasse
const void* g_candidatoAnt = nullptr;
uint64_t g_candidatoEm = 0;
int g_candidatoVezes = 0;
bool g_alvoNoQuadro = false;
uint64_t g_alvoArea = 0, g_alvoVisto = 0;
const void* g_ruinsProf[16] = {};
int g_nRuinsProf = 0;
uint64_t g_trocaEm = 0;
std::atomic<const void*> g_profFsr{nullptr};
std::atomic<uint64_t> g_profFsrEm{0};
std::atomic<uint64_t> g_despachosFsr{0};
uint64_t g_profFsrVisto = 0;
uint64_t g_querDesde = 0; // o Present em que a profundidade foi pedida: a escolha so comeca num quadro inteiro (pedida no meio dele, o primeiro visto seria o da etapa final)
// texturas trocadas: o jogo ainda pode ter listas gravadas ou na GPU com a copia para elas
struct ProfVelha
{
    ComPtr<ID3D12Resource> r;
    uint64_t ate;
};
std::vector<ProfVelha> g_profVelhas;
struct ProfPendente
{
    ID3D12CommandList* lista;
    UINT slot;
    uint64_t present;
};
std::vector<ProfPendente> g_profPendentes;
uint64_t g_profTentaEm = 0; // as texturas nao couberam: nao tenta de novo a cada barreira, com o jogo ja sem memoria
void ProfSemVram()
{
    g_profTentaEm = g_presents + 300;
    if (g_cab->profRecusa != dlss5ov::kProfSemVram)
        LogModulo("captura da API: sem VRAM para as texturas da profundidade");
    g_cab->profRecusa = dlss5ov::kProfSemVram;
}
std::atomic<int> g_nProfPendentes{0};

void RecusaProf(DXGI_FORMAT f)
{
    // um buffer de outra passagem, com o principal em uso, nao interessa
    if (g_nAlvosProf && g_presents - g_alvoVisto <= 120)
        return;
    g_cab->profRecusa = uint32_t(f);
    static int dito = -1;
    if (dito != int(f))
    {
        dito = int(f);
        LogModulo("captura da API: buffer de profundidade da tela no formato %d: nao da para copiar (so D32)", int(f));
    }
}

bool FsrVivo()
{
    static const bool semDica = GetEnvironmentVariableW(L"DLSS5_SEM_DICA", nullptr, 0) != 0; // teste: como um jogo sem FSR
    return !semDica && g_profFsr.load() && g_presents - g_profFsrEm.load() <= 30;
}

bool EscolheAlvoProf(const void* r, uint64_t area, UINT w, UINT h)
{
    if (FsrVivo())
    {
        if (r == g_profFsr.load())
        {
            if (g_nAlvosProf != 1 || g_alvosProf[0] != r)
                LogModulo("captura da API: buffer de profundidade: o que o jogo entrega ao FSR, %ux%u", w, h);
            g_alvosProf[0] = r;
            g_nAlvosProf = 1;
            g_alvoArea = area;
            g_cab->profExata = 1;
            g_profFsrVisto = g_presents;
            g_alvoNoQuadro = true;
            g_alvoVisto = g_presents;
            return true;
        }
        // ha motor que entrega ao FSR uma copia, que nunca passa por aqui: entao segue a escolha pelo conteudo
        if (g_profFsrVisto && g_presents - g_profFsrVisto <= 60)
            return false;
    }
    g_cab->profExata = 0;
    for (int i = 0; i < g_nRuinsProf; ++i)
        if (g_ruinsProf[i] == r)
            return false;
    bool conhecido = false;
    for (int i = 0; i < g_nAlvosProf && !conhecido; ++i)
        conhecido = g_alvosProf[i] == r;
    if (!conhecido)
    {
        if (!g_querDesde || g_presents <= g_querDesde)
            return false;
        const bool sumiu = g_presents - g_alvoVisto > 120;
        if (g_nAlvosProf && !sumiu)
        {
            if (area == g_alvoArea)
                g_candidatoProf = r;
            return false;
        }
        g_alvosProf[0] = r;
        g_nAlvosProf = 1;
        g_alvoArea = area;
        LogModulo("captura da API: buffer de profundidade escolhido: %ux%u", w, h);
    }
    g_alvoNoQuadro = true;
    g_alvoVisto = g_presents;
    return true;
}

// no Present: o candidato de um quadro sem nenhum dos escolhidos entra (o motor alterna buffers)
void FechaQuadroAlvoProf()
{
    if (!g_alvoNoQuadro && g_candidatoProf && g_nAlvosProf && g_nAlvosProf < kMaxAlvosProf)
    {
        g_candidatoVezes = g_candidatoProf == g_candidatoAnt && g_presents - g_candidatoEm <= 8 ? g_candidatoVezes + 1 : 1;
        g_candidatoAnt = g_candidatoProf;
        g_candidatoEm = g_presents;
        if (g_candidatoVezes >= 8)
        {
            g_alvosProf[g_nAlvosProf++] = g_candidatoProf;
            g_candidatoAnt = nullptr;
            LogModulo("captura da API: o jogo alterna buffers de profundidade: %d", g_nAlvosProf);
        }
    }
    g_alvoNoQuadro = false;
    g_candidatoProf = nullptr;
    if (g_cab->profTroca != g_cab->profTrocaFeita)
    {
        for (int i = 0; i < g_nAlvosProf && g_nRuinsProf < 16; ++i)
            g_ruinsProf[g_nRuinsProf++] = g_alvosProf[i];
        g_nAlvosProf = 0;
        g_alvoArea = 0;
        g_trocaEm = g_presents;
        g_cab->profTrocaFeita = g_cab->profTroca;
        LogModulo("captura da API: o overlay viu o buffer vazio: testando outro (%d ja testados)", g_nRuinsProf);
    }
    // nenhum outro apareceu: acabaram os candidatos, comeca de novo
    if (!g_nAlvosProf && g_nRuinsProf && g_presents - g_trocaEm > 8)
    {
        g_nRuinsProf = 0;
        g_trocaEm = g_presents;
        g_cab->profCiclos = g_cab->profCiclos + 1;
    }
    if (!g_cab->querProf)
        g_querDesde = 0;
    else if (!g_querDesde)
        g_querDesde = g_presents;
    for (size_t i = 0; i < g_profVelhas.size();)
        if (g_presents >= g_profVelhas[i].ate)
            g_profVelhas.erase(g_profVelhas.begin() + i);
        else
            ++i;
}

void SoltaProf12()
{
    std::lock_guard<std::mutex> lk(g_mxProf);
    g_cab->profGeracao = 0;
    g_profPendentes.clear();
    g_nProfPendentes.store(0);
    for (HANDLE x : g_handlesProf)
        CloseHandle(x);
    g_handlesProf.clear();
    for (auto& p : g_prof12)
        p.Reset();
    g_profVelhas.clear();
    g_nRuinsProf = 0;
    g_cab->profExata = 0;
    g_cab->profW = g_cab->profH = 0;
    g_nAlvosProf = 0;
    g_alvoArea = 0;
    g_profUltimaCopia = 0;
}

// sem soltar agora (uma lista do jogo ja gravada pode copiar para elas): CriaProf12 recria no dispositivo atual
void AposentaProf12()
{
    std::lock_guard<std::mutex> lk(g_mxProf);
    g_cab->profGeracao = 0;
    g_profPendentes.clear();
    g_nProfPendentes.store(0);
    for (HANDLE x : g_handlesProf)
        CloseHandle(x);
    g_handlesProf.clear();
    for (auto& p : g_prof12)
        if (p)
            g_profVelhas.push_back({std::move(p), g_presents + 16});
    g_cab->profW = g_cab->profH = 0;
}

bool CriaProf12(UINT w, UINT h)
{
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    g_cab->profGeracao = 0;
    g_profPendentes.clear();
    g_nProfPendentes.store(0);
    for (HANDLE x : g_handlesProf)
        CloseHandle(x);
    g_handlesProf.clear();
    // nao solta agora: uma lista do jogo ja gravada pode estar copiando para elas (solta-las aqui derrubava a GPU)
    for (auto& p : g_prof12)
        if (p)
            g_profVelhas.push_back({std::move(p), g_presents + 16});
    ++g_profGeracao;
    wchar_t nome[128];
    for (uint32_t s = 0; s < dlss5ov::kSlotsProf; ++s)
    {
        HANDLE c = nullptr;
        if (FAILED(CriaTex12(rd, g_prof12[s])))
            return false;
        dlss5ov::NomeRecursoApi(nome, 128, GetCurrentProcessId(), g_profGeracao, L"prof", s);
        g_prof12[s]->SetName(L"DLSS5Overlay");
        if (FAILED(g_12.dev->CreateSharedHandle(g_prof12[s].Get(), nullptr, GENERIC_ALL, nome, &c)))
            return false;
        g_handlesProf.push_back(c);
        g_cab->quadroProf[s] = 0;
    }
    g_cab->profW = w, g_cab->profH = h;
    g_cab->profSlots = dlss5ov::kSlotsProf;
    MemoryBarrier();
    g_cab->profGeracao = g_profGeracao;
    LogModulo("captura da API: profundidade do jogo %ux%u", w, h);
    return true;
}

void CopiaProf12(ID3D12GraphicsCommandList* l, const D3D12_RESOURCE_TRANSITION_BARRIER& b)
{
    if (!(b.StateBefore & D3D12_RESOURCE_STATE_DEPTH_WRITE) || !b.pResource || (b.Subresource != 0 && b.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES))
        return;
    // o Present pode soltar e recriar as texturas no meio desta funcao: uma leitura so do tamanho (zerado no meio dava 0/0)
    const UINT W = g_12.w, H = g_12.h;
    if (GetTickCount64() - g_cab->batimentoOverlay >= 1000 || !W || !H || l->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return;
    const D3D12_RESOURCE_DESC d = b.pResource->GetDesc();
    if (d.SampleDesc.Count != 1 || !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        return;
    // abaixo de metade da tela so o do FSR: o modo Ultra Desempenho renderiza a um terco
    if ((d.Width * 2 < W || d.Height * 2 < H) && b.pResource != g_profFsr.load())
        return;
    const double razao = double(d.Width) * H / (double(d.Height) * W);
    if (razao < 0.95 || razao > 1.05)
        return;
    std::lock_guard<std::mutex> lk(g_mxProf); // RecusaProf le o estado da escolha, escrito sob este mutex
    if (d.Format != DXGI_FORMAT_R32_TYPELESS && d.Format != DXGI_FORMAT_D32_FLOAT && d.Format != DXGI_FORMAT_R32G8X24_TYPELESS && d.Format != DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
    {
        RecusaProf(d.Format);
        return;
    }
    if (!EscolheAlvoProf(b.pResource, uint64_t(d.Width) * d.Height, UINT(d.Width), d.Height))
        return;
    if (!g_prof12[0] || g_cab->profW != UINT(d.Width) || g_cab->profH != d.Height)
    {
        if (g_presents < g_profTentaEm)
            return;
        if (!CriaProf12(UINT(d.Width), d.Height))
        {
            g_cab->profW = g_cab->profH = 0;
            ProfSemVram();
            return;
        }
    }
    g_cab->profRecusa = 0;
    // a mesma lista segue no slot dela (nada foi enviado); outra lista, um slot livre: nem publicado sem ler, nem de outra lista por enviar
    ID3D12CommandList* lista = l;
    int s = -1;
    for (const ProfPendente& p : g_profPendentes)
        if (p.lista == lista)
            s = int(p.slot);
    if (s < 0)
    {
        for (UINT p = 0; p < dlss5ov::kSlotsProf && s < 0; ++p)
        {
            bool ocupado = g_cab->quadroProf[p] > g_cab->consumido;
            for (const ProfPendente& e : g_profPendentes)
                ocupado |= e.slot == p;
            if (!ocupado)
                s = int(p);
        }
        if (s < 0)
            return;
        g_profPendentes.push_back({lista, UINT(s), g_presents});
        g_nProfPendentes.store(int(g_profPendentes.size()));
    }
    D3D12_RESOURCE_BARRIER x{};
    x.Transition = {b.pResource, b.Subresource, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE};
    oBarreira(l, 1, &x);
    D3D12_TEXTURE_COPY_LOCATION de{g_prof12[s].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, para = de;
    de.pResource = b.pResource;
    l->CopyTextureRegion(&para, 0, 0, 0, &de, nullptr);
    std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
    oBarreira(l, 1, &x);
}

void STDMETHODCALLTYPE GBarreira(ID3D12GraphicsCommandList* l, UINT n, const D3D12_RESOURCE_BARRIER* b)
{
    if (g_cab && g_cab->querProf)
        for (UINT i = 0; i < n; ++i)
            if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
                CopiaProf12(l, b[i].Transition);
    oBarreira(l, n, b);
}

// a lista saiu para a fila: o que ela copiou e do proximo Present
void NotaEnvioProf(UINT n, ID3D12CommandList* const* l)
{
    if (!g_nProfPendentes.load())
        return;
    std::lock_guard<std::mutex> lk(g_mxProf);
    const uint64_t q = g_n + 1;
    for (size_t e = 0; e < g_profPendentes.size();)
    {
        bool foi = false;
        for (UINT i = 0; i < n && !foi; ++i)
            foi = l[i] == g_profPendentes[e].lista;
        // gravada e nunca enviada (o jogo desistiu da lista): nao segura o slot
        if (!foi && g_presents - g_profPendentes[e].present <= 8)
        {
            ++e;
            continue;
        }
        if (foi)
        {
            for (uint32_t p = 0; p < dlss5ov::kSlotsProf; ++p)
                if (g_cab->quadroProf[p] == q)
                    g_cab->quadroProf[p] = 0; // a saida anterior deste quadro: vale esta, mais completa
            g_cab->quadroProf[g_profPendentes[e].slot] = q;
            g_profUltimaCopia = g_presents;
        }
        g_profPendentes.erase(g_profPendentes.begin() + e);
    }
    g_nProfPendentes.store(int(g_profPendentes.size()));
}

// ---- D3D11 ----
struct Estado11
{
    ComPtr<ID3D11Device5> dev;
    ComPtr<ID3D11DeviceContext4> ctx;
    ID3D11Device* origem = nullptr; // so comparacao
    ComPtr<ID3D11Fence> cerca;
    ComPtr<ID3D11Texture2D> tex[dlss5ov::kSlotsApi], prof[dlss5ov::kSlotsApi]; // copiadas juntas no Present: o mesmo slot da cor
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

// o primeiro buffer de profundidade ligado no contexto imediato num quadro inteiro, com a proporcao da tela: lido no Present
using PfnAlvos = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
PfnAlvos oAlvos = nullptr;
std::mutex g_mxDsv11;
ComPtr<ID3D11Resource> g_dsv11;
ID3D11DepthStencilView* g_dsvVisto = nullptr;
void STDMETHODCALLTYPE GAlvos(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* v, ID3D11DepthStencilView* dsv)
{
    if (dsv && dsv != g_dsvVisto && g_cab && g_cab->querProf)
    {
        g_dsvVisto = dsv;
        ComPtr<ID3D11Resource> r;
        dsv->GetResource(&r);
        ComPtr<ID3D11Texture2D> t;
        D3D11_TEXTURE2D_DESC d{};
        if (r && SUCCEEDED(r.As(&t)))
        {
            t->GetDesc(&d);
            const double razao = g_11.h ? double(d.Width) * g_11.h / (double(d.Height) * g_11.w) : 0.0;
            if (d.SampleDesc.Count == 1 && d.Width * 2 >= g_11.w && d.Height * 2 >= g_11.h && razao > 0.95 && razao < 1.05)
            {
                if (d.Format != DXGI_FORMAT_R32_TYPELESS && d.Format != DXGI_FORMAT_D32_FLOAT)
                {
                    std::lock_guard<std::mutex> lkp(g_mxProf);
                    RecusaProf(d.Format);
                }
                else
                {
                    std::lock_guard<std::mutex> lk(g_mxDsv11);
                    // aqui so ha um buffer por Present, e o principal pode seguir ligado sem aparecer de novo: nao ha como ver a alternancia
                    if (EscolheAlvoProf(r.Get(), uint64_t(d.Width) * d.Height, d.Width, d.Height))
                        g_dsv11 = r;
                    g_candidatoProf = nullptr;
                }
            }
        }
    }
    oAlvos(c, n, v, dsv);
}

void SoltaProf11()
{
    g_cab->profGeracao = 0;
    if (g_11.ctx)
        g_11.ctx->Flush();
    for (HANDLE x : g_handlesProf)
        CloseHandle(x);
    g_handlesProf.clear();
    for (auto& p : g_11.prof)
        p.Reset();
    g_cab->profW = g_cab->profH = 0;
    std::lock_guard<std::mutex> lk(g_mxDsv11);
    g_dsv11.Reset();
    g_dsvVisto = nullptr;
    g_nAlvosProf = 0;
    g_alvoArea = 0;
    g_profUltimaCopia = 0;
}

void CopiaProf11(UINT s, uint64_t n)
{
    ComPtr<ID3D11Resource> r;
    {
        std::lock_guard<std::mutex> lk(g_mxDsv11);
        r = g_dsv11;
    }
    ComPtr<ID3D11Texture2D> t;
    if (!g_cab->querProf || !r || FAILED(r.As(&t)))
        return;
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    if (!g_11.prof[0] || g_cab->profW != d.Width || g_cab->profH != d.Height)
    {
        if (g_presents < g_profTentaEm)
            return;
        g_cab->profW = g_cab->profH = 0;
        g_cab->profGeracao = 0;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = d.Width;
        td.Height = d.Height;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        for (HANDLE x : g_handlesProf)
            CloseHandle(x);
        g_handlesProf.clear();
        ++g_profGeracao;
        wchar_t nome[128];
        for (uint32_t i = 0; i < dlss5ov::kSlotsApi; ++i)
        {
            ComPtr<IDXGIResource1> dr;
            HANDLE c = nullptr;
            dlss5ov::NomeRecursoApi(nome, 128, GetCurrentProcessId(), g_profGeracao, L"prof", i);
            if (FAILED(g_11.dev->CreateTexture2D(&td, nullptr, &g_11.prof[i])) || FAILED(g_11.prof[i].As(&dr)) ||
                FAILED(dr->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nome, &c)))
            {
                ProfSemVram();
                return;
            }
            g_handlesProf.push_back(c);
        }
        g_cab->profW = d.Width, g_cab->profH = d.Height;
        g_cab->profSlots = dlss5ov::kSlotsApi;
        g_cab->profRecusa = 0;
        for (uint32_t i = 0; i < dlss5ov::kSlotsProf; ++i)
            g_cab->quadroProf[i] = 0;
        MemoryBarrier();
        g_cab->profGeracao = g_profGeracao;
        LogModulo("captura da API: profundidade do jogo %ux%u", d.Width, d.Height);
    }
    g_11.ctx->CopyResource(g_11.prof[s].Get(), r.Get());
    g_cab->quadroProf[s] = n;
    g_profUltimaCopia = g_presents;
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
    CopiaProf11(s, n);
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
std::vector<ID3D12CommandQueue*> g_filasAlem; // alem das 16: so a referencia, uma por fila (a ultima enviada pode ser uma delas)
thread_local ID3D12CommandQueue* t_ultimaFila = nullptr;
using PfnExecuta = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
PfnExecuta oExecuta = nullptr;
void STDMETHODCALLTYPE GExecuta(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* l)
{
    NotaEnvioProf(n, l);
    if (!g_filaDoSwap && q != t_ultimaFila && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        {
            std::lock_guard<std::mutex> lk(g_mxFilas);
            const int total = g_nFilas.load();
            int i = 0;
            while (i < total && g_filas[i].load() != q)
                ++i;
            if (i == total)
            {
                if (total < kMaxFilas)
                {
                    q->AddRef();
                    g_filas[total].store(q);
                    g_nFilas.store(total + 1);
                }
                else if (std::find(g_filasAlem.begin(), g_filasAlem.end(), q) == g_filasAlem.end())
                {
                    q->AddRef(); // uma por fila: antes era uma por envio
                    g_filasAlem.push_back(q);
                }
            }
        }
        t_ultimaFila = q;
        g_ultimaFila.store(q);
    }
    oExecuta(q, n, l);
}

// o swapchain guarda a fila em que foi criado, num deslocamento que muda com a versao do Windows: procura nele as filas conhecidas
// as filas conhecidas numa copia simples: FilaDentroDoSwap usa __try, que nao convive com objetos que tem destrutor (lock, vector)
int CopiaFilas(ID3D12CommandQueue** f, int max)
{
    std::lock_guard<std::mutex> lk(g_mxFilas);
    int n = 0;
    for (int i = 0; i < g_nFilas.load() && n < max; ++i)
        f[n++] = g_filas[i].load();
    for (ID3D12CommandQueue* q : g_filasAlem)
        if (n < max)
            f[n++] = q;
    return n;
}

ID3D12CommandQueue* FilaDentroDoSwap(IDXGISwapChain* sc)
{
    ID3D12CommandQueue* conhecidas[2 * kMaxFilas];
    const int total = CopiaFilas(conhecidas, 2 * kMaxFilas);
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
    if (sc != visto || !dele)
    {
        visto = sc;
        dele = FilaDentroDoSwap(sc);
    }
    // O back buffer so pode ser usado na fila em que o swapchain foi criado. Com o swapchain de geracao da AMD (FrameInterpolationSwapchainDX12.cpp) o real nasce na presentQueue, e essa e a fila
    // dele com a geracao ligada ou desligada; com ela desligada, porem, o Present vem da thread do jogo, cuja ultima fila e a do jogo: a "ultima lista enviada" seria a fila errada. Injetado
    // no meio, o gancho pode nao ter visto essa fila enviar ainda: espera ela aparecer, e so desiste (jogo em que a procura nao acha) depois de 60 quadros
    if (!dele && g_presents < 60)
        return nullptr;
    ID3D12CommandQueue* q = dele ? dele : t_ultimaFila ? t_ultimaFila : g_ultimaFila.load();
    static ID3D12CommandQueue* dita = nullptr;
    if (q && q != dita)
    {
        dita = q;
        wchar_t nome[64] = {};
        UINT tam = sizeof nome - sizeof(wchar_t);
        q->GetPrivateData(WKPDID_D3DDebugObjectNameW, &tam, nome);
        LogModulo("captura da API: fila do D3D12 \"%ls\" (%s)", nome, q == dele ? "achada no swapchain" : "pela ultima lista enviada");
    }
    return q;
}

// ---- o Present ----
// geracao de quadros do jogo = mais Presents que despachos do FSR
std::atomic<bool> g_fgAtiva{false};
void MedeGeracao()
{
    static uint64_t presents = 0, marcaP = 0, marcaD = 0;
    if (++presents - marcaP < 30)
        return;
    const uint64_t d = g_despachosFsr.load(), nd = d - marcaD, np = presents - marcaP;
    const bool fg = nd >= 5 && double(np) / double(nd) > 1.5;
    marcaP = presents;
    marcaD = d;
    if (fg != g_fgAtiva.load())
        LogModulo("geracao de quadros do jogo %s (%llu Presents, %llu despachos do FSR)", fg ? "ligada" : "desligada", (unsigned long long)np, (unsigned long long)nd);
    g_fgAtiva.store(fg);
}

thread_local int t_dentro = 0; // Present1 pode passar pelo Present
dlss5ov::LimiteFps g_limite;

// com nome: numa falha de pagina da GPU o DRED diz se e o back buffer real (o jogo nao ve este swapchain com a geracao de quadros)
void NomeiaBackBuffers(IDXGISwapChain* sc)
{
    static IDXGISwapChain* visto = nullptr;
    if (sc == visto)
        return;
    visto = sc;
    DXGI_SWAP_CHAIN_DESC sd{};
    if (FAILED(sc->GetDesc(&sd)))
        return;
    for (UINT i = 0; i < sd.BufferCount; ++i)
    {
        ComPtr<ID3D12Resource> b;
        if (SUCCEEDED(sc->GetBuffer(i, IID_PPV_ARGS(&b))))
            b->SetName(L"DLSS5: back buffer real");
    }
}

void NoPresent(IDXGISwapChain* sc, UINT flags)
{
    if (!g_cab || t_dentro || (flags & DXGI_PRESENT_TEST))
        return;
    std::unique_lock<std::mutex> lk(g_mx, std::try_to_lock);
    if (!lk.owns_lock())
        return;
    NomeiaBackBuffers(sc);
    MedeGeracao();
    const ULONGLONG agora = GetTickCount64();
    g_cab->batimentoJogo = agora;
    if (agora - g_cab->batimentoOverlay >= 1000)
    {
        // o overlay fechou: a VRAM volta para o jogo
        if (agora - g_vivoEm > 5000)
        {
            if (g_cab->geracao)
            {
                LogModulo("captura da API: overlay sem batimento ha %llu ms: as texturas voltam para o jogo",
                          (unsigned long long)(agora - g_cab->batimentoOverlay));
                g_cab->api == 12 ? Solta12() : Solta11();
            }
            if (g_prof12[0])
                SoltaProf12();
            if (g_11.prof[0])
                SoltaProf11();
        }
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
    // so o Present da janela copiada fecha o quadro: o de uma segunda janela o fechava no meio
    ++g_presents;
    {
        static ULONGLONG vramEm = 0;
        if (g_12.dev && agora - vramEm >= 2000)
        {
            vramEm = agora;
            char vram[64];
            dlss5ov::TextoVramJogo(g_12.dev.Get(), vram, sizeof vram);
            LogModulo("%s; geracao de quadros %s", vram, g_fgAtiva.load() ? "ligada" : "desligada");
        }
    }
    {
        std::lock_guard<std::mutex> lkp(g_mxProf);
        FechaQuadroAlvoProf();
    }
    // sem pedir profundidade por um tempo (nenhuma lista do jogo ainda a usa): a VRAM volta
    if (!g_cab->querProf && g_presents - g_profUltimaCopia > 16)
    {
        if (g_prof12[0])
            SoltaProf12();
        if (g_11.prof[0])
            SoltaProf11();
    }
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
    // com a geracao do jogo, esta e a thread de ritmo dela: dormir aqui atrapalha a geracao; o teto vai no despacho do FSR, nos quadros renderizados
    if (!t_dentro && !(flags & DXGI_PRESENT_TEST) && !g_fgAtiva.load())
        g_limite.Espera(g_cab);
    return hr;
}
template <int I> HRESULT STDMETHODCALLTYPE GPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p)
{
    NoPresent(sc, flags);
    ++t_dentro;
    const HRESULT hr = oPresent1[I](sc, sync, flags, p);
    --t_dentro;
    // com a geracao do jogo, esta e a thread de ritmo dela: dormir aqui atrapalha a geracao; o teto vai no despacho do FSR, nos quadros renderizados
    if (!t_dentro && !(flags & DXGI_PRESENT_TEST) && !g_fgAtiva.load())
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
    ComPtr<ID3D11DeviceContext> ctx;
    d->GetImmediateContext(&ctx);
    if (ctx)
    {
        void** vt = *reinterpret_cast<void***>(ctx.Get());
        oAlvos = reinterpret_cast<PfnAlvos>(vt[33]); // OMSetRenderTargets
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(reinterpret_cast<PVOID*>(&oAlvos), reinterpret_cast<PVOID>(&GAlvos));
        DetourTransactionCommit();
    }
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
    // sempre: e no envio que a profundidade ganha o numero do quadro
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
    {
        ComPtr<ID3D12CommandAllocator> al;
        ComPtr<ID3D12GraphicsCommandList> cl;
        if (SUCCEEDED(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al))) &&
            SUCCEEDED(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(&cl))))
        {
            cl->Close();
            void** vt = *reinterpret_cast<void***>(cl.Get());
            oBarreira = reinterpret_cast<PfnBarreira>(vt[26]); // ResourceBarrier
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(reinterpret_cast<PVOID*>(&oBarreira), reinterpret_cast<PVOID>(&GBarreira));
            DetourTransactionCommit();
        }
    }
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
// do despacho do FSR: com a geracao do jogo, o teto de FPS fica aqui
void LimiteNoDespacho()
{
    if (g_cab && g_fgAtiva.load())
        g_limite.Espera(g_cab);
}

// do despacho do FSR (Modulo.cpp): a profundidade que o jogo entrega a ele
void NotaProfFsr(ID3D12Resource* r)
{
    g_profFsr.store(r);
    g_profFsrEm.store(g_presents);
    g_despachosFsr.fetch_add(1);
}

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
