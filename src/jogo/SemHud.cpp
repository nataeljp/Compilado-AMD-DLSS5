// A imagem sem HUD (SemHud.h). Os descritores das texturas candidatas (tamanho e familia de formato do backbuffer) sao
// seguidos da criacao (Create*View, CopyDescriptors*) ate o uso: OMSetRenderTargets / BeginRenderPass + Draw, ou a
// tabela de compute (com as faixas de UAV da root signature) + Dispatch. Fora da janela (entre o upscaler e o Present)
// os ganchos das listas so olham um booleano.
#include "SemHud.h"
#include <windows.h>
#include <detours.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dxguid.lib")

namespace semhud {
namespace {

enum : uint8_t
{
    kRtv = 1,
    kUav = 2
};
struct Vista
{
    ID3D12Resource* r;
    uint8_t tipo;
};

// a leitura (todo Create*View e toda copia de descritores do jogo) nao bloqueia outra leitura
struct Leitura
{
    SRWLOCK& t;
    explicit Leitura(SRWLOCK& x) : t(x) { AcquireSRWLockShared(&t); }
    ~Leitura() { ReleaseSRWLockShared(&t); }
};
struct Escrita
{
    SRWLOCK& t;
    explicit Escrita(SRWLOCK& x) : t(x) { AcquireSRWLockExclusive(&t); }
    ~Escrita() { ReleaseSRWLockExclusive(&t); }
};

SRWLOCK g_tVistas = SRWLOCK_INIT;
std::map<SIZE_T, Vista> g_vistas; // endereco do descritor (CPU) -> textura candidata
std::atomic<bool> g_temVistas{false};
SIZE_T g_incRtv = 0, g_incRec = 0; // passo dos descritores RTV e CBV/SRV/UAV

std::atomic<UINT64> g_w{0};
std::atomic<UINT> g_h{0};
std::atomic<int> g_familia{-1};
std::atomic<bool> g_instalado{false}, g_aberta{false};
std::atomic<uint64_t> g_janela{0}; // qual janela (o estado das listas de outra janela nao vale)
std::atomic<uint32_t> g_indice{0}; // passes candidatos ja vistos nesta janela
FnCaptura g_captura = nullptr;
thread_local int t_proprio = 0;

// relato (o texto e feito na captura: depois a textura pode ja ter saido)
std::mutex g_mxRelato;
uint32_t g_capturas = 0;
ID3D12Resource* g_ultima = nullptr;
char g_textoUltima[192] = {};

// montes visiveis aos shaders: a tabela (GPU) aponta o descritor (CPU)
struct Monte
{
    ID3D12DescriptorHeap* h;
    UINT64 gpu;
    SIZE_T cpu;
    UINT64 tam;
};
SRWLOCK g_tMontes = SRWLOCK_INIT;
std::vector<Monte> g_montes;

// as faixas de UAV de cada tabela de cada root signature
struct Faixa
{
    UINT ini, n;
};
SRWLOCK g_tRs = SRWLOCK_INIT;
std::unordered_map<ID3D12RootSignature*, std::vector<std::vector<Faixa>>> g_rs;

// a lista em gravacao nesta thread
struct Lista
{
    ID3D12GraphicsCommandList* l = nullptr;
    ID3D12RootSignature* rs = nullptr;
    uint64_t janela = 0;
    ID3D12Resource* rt = nullptr;
    bool rtNovo = false; // ligado e ainda sem desenho: o proximo desenho e um passe candidato
    ID3D12Resource* uav[64] = {};
    uint64_t comUav = 0, uavNovo = 0;
};
thread_local Lista t_lista;
Lista& De(ID3D12GraphicsCommandList* l)
{
    if (t_lista.l != l)
    {
        t_lista = {};
        t_lista.l = l;
    }
    return t_lista;
}
Lista& NaJanela(Lista& e)
{
    const uint64_t j = g_janela.load(std::memory_order_relaxed);
    if (e.janela != j)
    {
        e.janela = j;
        e.rt = nullptr;
        e.rtNovo = false;
        e.comUav = e.uavNovo = 0;
    }
    return e;
}

// familias de formato que copiam entre si (CopyTextureRegion)
int Familia(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT: return 1;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT: return 2;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return 3;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return 4;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT: return 5;
    default: return -1;
    }
}

bool Candidata(ID3D12Resource* r)
{
    if (!r)
        return false;
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    const int f = Familia(d.Format);
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == g_w.load() && d.Height == g_h.load() &&
           d.DepthOrArraySize == 1 && d.SampleDesc.Count == 1 && f >= 0 && f == g_familia.load();
}

void Apaga(SIZE_T a, SIZE_T b)
{
    if (!g_temVistas.load(std::memory_order_relaxed))
        return;
    {
        Leitura l(g_tVistas);
        const auto it = g_vistas.lower_bound(a);
        if (it == g_vistas.end() || it->first >= b)
            return;
    }
    Escrita e(g_tVistas);
    g_vistas.erase(g_vistas.lower_bound(a), g_vistas.lower_bound(b));
    g_temVistas = !g_vistas.empty();
}

void Registra(SIZE_T h, ID3D12Resource* r, uint8_t tipo, bool plana)
{
    if (!t_proprio && plana && Candidata(r))
    {
        Escrita e(g_tVistas);
        g_vistas[h] = {r, tipo};
        g_temVistas = true;
        return;
    }
    Apaga(h, h + 1);
}

// [dst, dst + bytes) passa a ter os descritores de [src, src + bytes)
void Copia(SIZE_T dst, SIZE_T src, SIZE_T bytes)
{
    if (!bytes || !g_temVistas.load(std::memory_order_relaxed))
        return;
    std::vector<std::pair<SIZE_T, Vista>> v;
    bool noDestino = false;
    {
        Leitura l(g_tVistas);
        for (auto it = g_vistas.lower_bound(src); it != g_vistas.end() && it->first < src + bytes; ++it)
            v.emplace_back(dst + (it->first - src), it->second);
        const auto d = g_vistas.lower_bound(dst);
        noDestino = d != g_vistas.end() && d->first < dst + bytes;
    }
    if (v.empty() && !noDestino)
        return;
    Escrita e(g_tVistas);
    g_vistas.erase(g_vistas.lower_bound(dst), g_vistas.lower_bound(dst + bytes));
    for (const auto& [a, x] : v)
        g_vistas[a] = x;
    g_temVistas = !g_vistas.empty();
}

// ---- dispositivo ----
using PfnCbv = void(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using PfnSrv = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*,
                                        D3D12_CPU_DESCRIPTOR_HANDLE);
using PfnUav = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, ID3D12Resource*,
                                        const D3D12_UNORDERED_ACCESS_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using PfnRtv = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*,
                                        D3D12_CPU_DESCRIPTOR_HANDLE);
using PfnCopia = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT,
                                          const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using PfnCopiaSimples = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE,
                                                 D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
using PfnRs = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
PfnCbv oCbv = nullptr;
PfnSrv oSrv = nullptr;
PfnUav oUav = nullptr;
PfnRtv oRtv = nullptr;
PfnCopia oCopia = nullptr;
PfnCopiaSimples oCopiaSimples = nullptr;
PfnRs oRs = nullptr;

void STDMETHODCALLTYPE Cbv(ID3D12Device* d, const D3D12_CONSTANT_BUFFER_VIEW_DESC* v, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    oCbv(d, v, h);
    Apaga(h.ptr, h.ptr + 1);
}

void STDMETHODCALLTYPE Srv(ID3D12Device* d, ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* v,
                           D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    oSrv(d, r, v, h);
    Apaga(h.ptr, h.ptr + 1);
}

void STDMETHODCALLTYPE Uav(ID3D12Device* d, ID3D12Resource* r, ID3D12Resource* contador,
                           const D3D12_UNORDERED_ACCESS_VIEW_DESC* v, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    oUav(d, r, contador, v, h);
    Registra(h.ptr, r, kUav, !v || (v->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2D && v->Texture2D.MipSlice == 0));
}

void STDMETHODCALLTYPE Rtv(ID3D12Device* d, ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* v,
                           D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    oRtv(d, r, v, h);
    Registra(h.ptr, r, kRtv, !v || (v->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D && v->Texture2D.MipSlice == 0));
}

SIZE_T Passo(D3D12_DESCRIPTOR_HEAP_TYPE t)
{
    return t == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ? g_incRtv : t == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ? g_incRec : 0;
}

void STDMETHODCALLTYPE CopiaSimples(ID3D12Device* d, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src,
                                    D3D12_DESCRIPTOR_HEAP_TYPE t)
{
    oCopiaSimples(d, n, dst, src, t);
    if (const SIZE_T inc = Passo(t))
        Copia(dst.ptr, src.ptr, SIZE_T(n) * inc);
}

void STDMETHODCALLTYPE CopiaFaixas(ID3D12Device* d, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* dIni, const UINT* dTam,
                                   UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE* sIni, const UINT* sTam,
                                   D3D12_DESCRIPTOR_HEAP_TYPE t)
{
    oCopia(d, nd, dIni, dTam, ns, sIni, sTam, t);
    const SIZE_T inc = Passo(t);
    if (!inc || !g_temVistas.load(std::memory_order_relaxed) || !dIni || !sIni)
        return;
    // as duas listas de faixas andam juntas, em pedacos continuos nas duas
    UINT i = 0, j = 0, oi = 0, oj = 0;
    while (i < nd && j < ns)
    {
        const UINT td = dTam ? dTam[i] : 1, ts = sTam ? sTam[j] : 1;
        const UINT n = std::min(td - std::min(oi, td), ts - std::min(oj, ts));
        Copia(dIni[i].ptr + SIZE_T(oi) * inc, sIni[j].ptr + SIZE_T(oj) * inc, SIZE_T(n) * inc);
        oi += n, oj += n;
        if (oi >= td)
            ++i, oi = 0;
        if (oj >= ts)
            ++j, oj = 0;
    }
}

HRESULT STDMETHODCALLTYPE CriaRs(ID3D12Device* d, UINT no, const void* blob, SIZE_T tam, REFIID riid, void** ppv)
{
    const HRESULT r = oRs(d, no, blob, tam, riid, ppv);
    if (FAILED(r) || !ppv || !*ppv || riid != __uuidof(ID3D12RootSignature))
        return r;
    Microsoft::WRL::ComPtr<ID3D12VersionedRootSignatureDeserializer> des;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* v = nullptr;
    if (FAILED(D3D12CreateVersionedRootSignatureDeserializer(blob, tam, IID_PPV_ARGS(&des))) ||
        FAILED(des->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &v)) || !v)
        return r;
    const D3D12_ROOT_SIGNATURE_DESC1& rd = v->Desc_1_1;
    std::vector<std::vector<Faixa>> p(rd.NumParameters);
    for (UINT i = 0; i < rd.NumParameters; ++i)
    {
        const D3D12_ROOT_PARAMETER1& par = rd.pParameters[i];
        if (par.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            continue;
        UINT pos = 0;
        for (UINT k = 0; k < par.DescriptorTable.NumDescriptorRanges; ++k)
        {
            const D3D12_DESCRIPTOR_RANGE1& f = par.DescriptorTable.pDescriptorRanges[k];
            const UINT ini =
                f.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND ? pos : f.OffsetInDescriptorsFromTableStart;
            // faixa sem fim (sem limite: o heap inteiro, bindless) nao diz qual textura o passe usa
            const UINT n = f.NumDescriptors == UINT_MAX ? 0 : f.NumDescriptors;
            if (f.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV && n)
                p[i].push_back({ini, std::min<UINT>(n, 64)});
            pos = ini + n;
        }
    }
    Escrita e(g_tRs);
    g_rs[static_cast<ID3D12RootSignature*>(*ppv)] = std::move(p);
    return r;
}

// ---- listas ----
using PfnFecha = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
PfnFecha oFecha = nullptr;
std::atomic<void (*)(ID3D12GraphicsCommandList*, HRESULT)> g_recusa{nullptr};

HRESULT STDMETHODCALLTYPE FechaLista(ID3D12GraphicsCommandList* l)
{
    const HRESULT r = oFecha(l);
    if (FAILED(r))
        if (auto f = g_recusa.load())
            f(l, r);
    return r;
}

using PfnDesenha = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using PfnDesenhaIdx = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using PfnDespacha = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using PfnMontes = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
using PfnRsCompute = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using PfnTabela = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
using PfnAlvos = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                          const D3D12_CPU_DESCRIPTOR_HANDLE*);
using PfnPasse = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
                                          const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*, D3D12_RENDER_PASS_FLAGS);
PfnDesenha oDesenha = nullptr;
PfnDesenhaIdx oDesenhaIdx = nullptr;
PfnDespacha oDespacha = nullptr;
PfnMontes oMontes = nullptr;
PfnRsCompute oRsCompute = nullptr;
PfnTabela oTabela = nullptr;
PfnAlvos oAlvos = nullptr;
PfnPasse oPasse = nullptr;

// o passe que acabou de ser gravado escreveu a candidata: a geracao copia o passe escolhido
void Captura(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES estado, uint8_t tipo)
{
    const uint32_t indice = g_indice.fetch_add(1);
    if (indice >= 8)
        return;
    ++t_proprio;
    const bool ok = g_captura && g_captura(l, r, estado, indice);
    --t_proprio;
    if (!ok)
        return;
    std::lock_guard<std::mutex> lk(g_mxRelato);
    ++g_capturas;
    if (r == g_ultima)
        return;
    g_ultima = r;
    wchar_t nome[64] = {};
    UINT tam = sizeof nome - sizeof(wchar_t);
    if (FAILED(r->GetPrivateData(WKPDID_D3DDebugObjectNameW, &tam, nome)))
        nome[0] = 0;
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    snprintf(g_textoUltima, sizeof g_textoUltima, "%s de \"%ls\" %llux%u fmt %d",
             tipo == kRtv ? "alvo de desenho" : "UAV de compute", nome, (unsigned long long)d.Width, d.Height,
             int(d.Format));
}

void STDMETHODCALLTYPE Desenha(ID3D12GraphicsCommandList* l, UINT a, UINT b, UINT c, UINT d)
{
    oDesenha(l, a, b, c, d);
    if (!g_aberta.load(std::memory_order_relaxed) || t_proprio)
        return;
    if (Lista& e = NaJanela(De(l)); e.rt && e.rtNovo)
    {
        e.rtNovo = false;
        Captura(l, e.rt, D3D12_RESOURCE_STATE_RENDER_TARGET, kRtv);
    }
}

void STDMETHODCALLTYPE DesenhaIdx(ID3D12GraphicsCommandList* l, UINT a, UINT b, UINT c, INT d, UINT e5)
{
    oDesenhaIdx(l, a, b, c, d, e5);
    if (!g_aberta.load(std::memory_order_relaxed) || t_proprio)
        return;
    if (Lista& e = NaJanela(De(l)); e.rt && e.rtNovo)
    {
        e.rtNovo = false;
        Captura(l, e.rt, D3D12_RESOURCE_STATE_RENDER_TARGET, kRtv);
    }
}

void STDMETHODCALLTYPE Despacha(ID3D12GraphicsCommandList* l, UINT x, UINT y, UINT z)
{
    oDespacha(l, x, y, z);
    if (!g_aberta.load(std::memory_order_relaxed) || t_proprio)
        return;
    Lista& e = NaJanela(De(l));
    if (const uint64_t novas = e.comUav & e.uavNovo)
    {
        e.uavNovo = 0;
        Captura(l, e.uav[std::countr_zero(novas)], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kUav);
    }
}

void STDMETHODCALLTYPE Montes(ID3D12GraphicsCommandList* l, UINT n, ID3D12DescriptorHeap* const* h)
{
    oMontes(l, n, h);
    for (UINT i = 0; h && i < n; ++i)
    {
        if (!h[i])
            continue;
        const D3D12_DESCRIPTOR_HEAP_DESC d = h[i]->GetDesc();
        if (d.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !(d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))
            continue;
        const UINT64 gpu = h[i]->GetGPUDescriptorHandleForHeapStart().ptr;
        {
            Leitura lt(g_tMontes);
            if (std::any_of(g_montes.begin(), g_montes.end(), [&](const Monte& m) { return m.h == h[i] && m.gpu == gpu; }))
                continue;
        }
        const UINT64 tam = UINT64(d.NumDescriptors) * g_incRec;
        Escrita e(g_tMontes);
        // o mesmo ponteiro (heap solto e outro no lugar) ou a mesma faixa da GPU: o antigo sai
        std::erase_if(g_montes, [&](const Monte& m) { return m.h == h[i] || (gpu < m.gpu + m.tam && m.gpu < gpu + tam); });
        g_montes.push_back({h[i], gpu, h[i]->GetCPUDescriptorHandleForHeapStart().ptr, tam});
    }
}

void STDMETHODCALLTYPE RsCompute(ID3D12GraphicsCommandList* l, ID3D12RootSignature* rs)
{
    oRsCompute(l, rs);
    Lista& e = De(l);
    e.rs = rs;
    e.comUav = 0;
}

// a UAV candidata que a tabela liga (pelas faixas de UAV da root signature; sem ela, o primeiro descritor da tabela)
ID3D12Resource* UavDaTabela(ID3D12RootSignature* rs, UINT idx, UINT64 gpu)
{
    SIZE_T cpu = 0;
    {
        Leitura l(g_tMontes);
        for (const Monte& m : g_montes)
            if (gpu >= m.gpu && gpu < m.gpu + m.tam)
            {
                cpu = m.cpu + SIZE_T(gpu - m.gpu);
                break;
            }
    }
    if (!cpu)
        return nullptr;
    std::vector<Faixa> faixas{{0, 1}};
    {
        Leitura l(g_tRs);
        if (const auto it = g_rs.find(rs); it != g_rs.end())
        {
            if (idx >= it->second.size())
                return nullptr;
            faixas = it->second[idx];
        }
    }
    Leitura l(g_tVistas);
    for (const Faixa& f : faixas)
    {
        const SIZE_T a = cpu + SIZE_T(f.ini) * g_incRec, b = a + SIZE_T(f.n) * g_incRec;
        for (auto it = g_vistas.lower_bound(a); it != g_vistas.end() && it->first < b; ++it)
            if (it->second.tipo == kUav)
                return it->second.r;
    }
    return nullptr;
}

void STDMETHODCALLTYPE Tabela(ID3D12GraphicsCommandList* l, UINT idx, D3D12_GPU_DESCRIPTOR_HANDLE g)
{
    oTabela(l, idx, g);
    if (!g_aberta.load(std::memory_order_relaxed) || t_proprio || idx >= 64)
        return;
    Lista& e = NaJanela(De(l));
    ID3D12Resource* r = g.ptr && g_temVistas.load(std::memory_order_relaxed) ? UavDaTabela(e.rs, idx, g.ptr) : nullptr;
    e.uav[idx] = r;
    e.comUav = r ? e.comUav | (1ull << idx) : e.comUav & ~(1ull << idx);
    e.uavNovo = r ? e.uavNovo | (1ull << idx) : e.uavNovo & ~(1ull << idx);
}

ID3D12Resource* RtvCandidata(SIZE_T h)
{
    if (!g_temVistas.load(std::memory_order_relaxed))
        return nullptr;
    Leitura l(g_tVistas);
    const auto it = g_vistas.find(h);
    return it != g_vistas.end() && it->second.tipo == kRtv ? it->second.r : nullptr;
}

void STDMETHODCALLTYPE Alvos(ID3D12GraphicsCommandList* l, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rt, BOOL faixa,
                             const D3D12_CPU_DESCRIPTOR_HANDLE* ds)
{
    oAlvos(l, n, rt, faixa, ds);
    if (!g_aberta.load(std::memory_order_relaxed) || t_proprio)
        return;
    Lista& e = NaJanela(De(l));
    e.rt = nullptr;
    for (UINT i = 0; rt && i < n && !e.rt; ++i)
        e.rt = RtvCandidata(faixa ? rt[0].ptr + SIZE_T(i) * g_incRtv : rt[i].ptr);
    e.rtNovo = e.rt != nullptr;
}

void STDMETHODCALLTYPE Passe(ID3D12GraphicsCommandList4* l, UINT n, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rt,
                             const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds, D3D12_RENDER_PASS_FLAGS f)
{
    oPasse(l, n, rt, ds, f);
    if (!g_aberta.load(std::memory_order_relaxed) || t_proprio)
        return;
    Lista& e = NaJanela(De(l));
    e.rt = nullptr;
    for (UINT i = 0; rt && i < n && !e.rt; ++i)
        e.rt = RtvCandidata(rt[i].cpuDescriptor.ptr);
    e.rtNovo = e.rt != nullptr;
}

// ---- swapchain da geracao ----
using PfnPresent = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PfnPresent1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using PfnResize = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PfnResize1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*,
                                               IUnknown* const*);
PfnPresent oPresent = nullptr;
PfnPresent1 oPresent1 = nullptr;
PfnResize oResize = nullptr;
PfnResize1 oResize1 = nullptr;

void Alvo(IDXGISwapChain* sc)
{
    DXGI_SWAP_CHAIN_DESC d{};
    if (FAILED(sc->GetDesc(&d)))
        return;
    g_w = d.BufferDesc.Width;
    g_h = d.BufferDesc.Height;
    g_familia = Familia(d.BufferDesc.Format);
}

HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* s, UINT i, UINT f)
{
    Fecha();
    Proprio eu; // o que a AMD grava na apresentacao (a geracao e as copias dela)
    return oPresent(s, i, f);
}

HRESULT STDMETHODCALLTYPE Present1(IDXGISwapChain1* s, UINT i, UINT f, const DXGI_PRESENT_PARAMETERS* p)
{
    Fecha();
    Proprio eu;
    return oPresent1(s, i, f, p);
}

HRESULT STDMETHODCALLTYPE Resize(IDXGISwapChain* s, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl)
{
    const HRESULT r = oResize(s, n, w, h, f, fl);
    if (SUCCEEDED(r))
        Alvo(s); // antes de o jogo refazer as texturas no tamanho novo
    return r;
}

HRESULT STDMETHODCALLTYPE Resize1(IDXGISwapChain3* s, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl, const UINT* m,
                                  IUnknown* const* q)
{
    const HRESULT r = oResize1(s, n, w, h, f, fl, m, q);
    if (SUCCEEDED(r))
        Alvo(s);
    return r;
}

template <class T> void Liga(T& original, void* f, void* gancho)
{
    original = reinterpret_cast<T>(f);
    DetourAttach(reinterpret_cast<PVOID*>(&original), gancho);
}

} // namespace

void DefineRecusa(void (*recusa)(ID3D12GraphicsCommandList* l, HRESULT r))
{
    g_recusa = recusa;
}

Proprio::Proprio() { ++t_proprio; }
Proprio::~Proprio() { --t_proprio; }

const char* Instala(ID3D12Device* d, IDXGISwapChain* sc, FnCaptura captura)
{
    if (!d || !sc)
        return "sem dispositivo ou swapchain";
    Alvo(sc);
    g_captura = captura;
    static const char* erro = "nao instalou";
    static std::once_flag uma;
    std::call_once(uma, [&] {
        g_incRtv = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        g_incRec = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> al;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> l;
        if (FAILED(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al))) ||
            FAILED(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(&l))))
        {
            erro = "nao criou a lista de amostra";
            return;
        }
        l->Close();
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> l4;
        l.As(&l4);
        Microsoft::WRL::ComPtr<IDXGISwapChain3> sc3;
        sc->QueryInterface(IID_PPV_ARGS(&sc3));
        void** vd = *reinterpret_cast<void***>(d);
        void** vl = *reinterpret_cast<void***>(l.Get());
        void** vs = *reinterpret_cast<void***>(sc);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        Liga(oRs, vd[16], reinterpret_cast<void*>(&CriaRs));
        Liga(oCbv, vd[17], reinterpret_cast<void*>(&Cbv));
        Liga(oSrv, vd[18], reinterpret_cast<void*>(&Srv));
        Liga(oUav, vd[19], reinterpret_cast<void*>(&Uav));
        Liga(oRtv, vd[20], reinterpret_cast<void*>(&Rtv));
        Liga(oCopia, vd[23], reinterpret_cast<void*>(&CopiaFaixas));
        Liga(oCopiaSimples, vd[24], reinterpret_cast<void*>(&CopiaSimples));
        Liga(oFecha, vl[9], reinterpret_cast<void*>(&FechaLista));
        Liga(oDesenha, vl[12], reinterpret_cast<void*>(&Desenha));
        Liga(oDesenhaIdx, vl[13], reinterpret_cast<void*>(&DesenhaIdx));
        Liga(oDespacha, vl[14], reinterpret_cast<void*>(&Despacha));
        Liga(oMontes, vl[28], reinterpret_cast<void*>(&Montes));
        Liga(oRsCompute, vl[29], reinterpret_cast<void*>(&RsCompute));
        Liga(oTabela, vl[31], reinterpret_cast<void*>(&Tabela));
        Liga(oAlvos, vl[46], reinterpret_cast<void*>(&Alvos));
        if (l4)
            Liga(oPasse, vl[68], reinterpret_cast<void*>(&Passe));
        Liga(oPresent, vs[8], reinterpret_cast<void*>(&Present));
        Liga(oResize, vs[13], reinterpret_cast<void*>(&Resize));
        Liga(oPresent1, vs[22], reinterpret_cast<void*>(&Present1));
        if (sc3)
            Liga(oResize1, vs[39], reinterpret_cast<void*>(&Resize1));
        if (DetourTransactionCommit() != NO_ERROR)
        {
            erro = "os ganchos falharam";
            return;
        }
        erro = nullptr;
        g_instalado = true;
    });
    return erro;
}

void Abre()
{
    if (!g_instalado.load(std::memory_order_relaxed))
        return;
    g_janela.fetch_add(1, std::memory_order_relaxed);
    g_indice = 0;
    g_aberta = true;
}

void Fecha()
{
    g_aberta = false;
}

uint32_t Relato(char* texto, size_t n)
{
    std::lock_guard<std::mutex> lk(g_mxRelato);
    const uint32_t c = g_capturas;
    g_capturas = 0;
    if (n)
        snprintf(texto, n, "%s", c ? g_textoUltima : "");
    return c;
}

} // namespace semhud
