#include "Comum.h"
#include <cstdarg>
#include <chrono>
#include <map>
#include <mutex>
#include <share.h>

namespace ov {

static FILE* g_log = nullptr;
static std::mutex g_logMutex;

std::string Fmt(const char* f, ...)
{
    char b[4096];
    va_list a;
    va_start(a, f);
    vsnprintf(b, sizeof b, f, a);
    va_end(a);
    return b;
}

std::string Utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Largo(const std::string& s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static std::string Escapa(const std::string& s)
{
    std::string o;
    for (char c : s)
    {
        if (c == '"' || c == '\\')
            o += '\\', o += c;
        else if (c == '\n')
            o += "\\n";
        else if ((unsigned char)c < 0x20)
            o += ' ';
        else
            o += c;
    }
    return o;
}

void LogAbre(const std::wstring& pasta)
{
    std::lock_guard<std::mutex> l(g_logMutex);
    if (!g_log)
        g_log = _wfsopen((pasta + L"\\overlay.jsonl").c_str(), L"w", _SH_DENYNO); // legivel com o programa aberto
}

void Log(const char* tipo, const std::string& msg)
{
    using namespace std::chrono;
    const double t = duration<double>(steady_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> l(g_logMutex);
    const std::string linha = Fmt("{\"t\":%.3f,\"tipo\":\"%s\",\"msg\":\"%s\"}\n", t, tipo, Escapa(msg).c_str());
    if (g_log)
    {
        fputs(linha.c_str(), g_log);
        fflush(g_log);
    }
    OutputDebugStringA(linha.c_str());
}

// ---------------- Gpu ----------------
bool Gpu::Memoria(UINT64& uso, UINT64& orcamento, UINT64& naRam) const
{
    ComPtr<IDXGIAdapter3> a3;
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, ram{};
    if (!adaptador || FAILED(adaptador.As(&a3)) ||
        FAILED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) ||
        FAILED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &ram)))
        return false;
    uso = local.CurrentUsage;
    orcamento = local.Budget;
    naRam = ram.CurrentUsage;
    return true;
}

void Gpu::Cria()
{
    Ck(CreateDXGIFactory2(0, IID_PPV_ARGS(&fabrica)), "CreateDXGIFactory2");
    // a placa AMD de maior desempenho (a rede so roda em RDNA 4 pelo HIP)
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> a;
        if (fabrica->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)) ==
            DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (d.VendorId != 0x1002 || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            continue;
        if (SUCCEEDED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev))))
        {
            adaptador = a;
            luid = d.AdapterLuid;
            nome = d.Description;
            break;
        }
    }
    if (!dev)
        throw std::runtime_error("nenhuma placa AMD com D3D12 encontrada");
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Ck(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&fila)), "CreateCommandQueue");
    Ck(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&cerca)), "CreateFence");
    evento = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Log("gpu", "placa: " + Utf8(nome));
}

UINT64 Gpu::Sinaliza()
{
    ++valor;
    Ck(fila->Signal(cerca.Get(), valor), "Signal");
    return valor;
}

void Gpu::EsperaValor(UINT64 v)
{
    if (cerca->GetCompletedValue() >= v)
        return;
    Ck(cerca->SetEventOnCompletion(v, evento), "SetEventOnCompletion");
    WaitForSingleObject(evento, INFINITE);
}

void Gpu::Espera() { EsperaValor(Sinaliza()); }

bool MonitorHdr(Gpu& g, HWND janela, float& escalaSdr)
{
    escalaSdr = 1.f;
    HMONITOR mon = MonitorFromWindow(janela, MONITOR_DEFAULTTONEAREST);
    bool hdr = false;
    bool achou = false;
    for (UINT a = 0; !achou; ++a)
    {
        ComPtr<IDXGIAdapter1> ad;
        if (g.fabrica->EnumAdapters1(a, &ad) == DXGI_ERROR_NOT_FOUND)
            break;
        for (UINT o = 0;; ++o)
        {
            ComPtr<IDXGIOutput> out;
            if (ad->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND)
                break;
            ComPtr<IDXGIOutput6> o6;
            DXGI_OUTPUT_DESC1 d{};
            if (FAILED(out.As(&o6)) || FAILED(o6->GetDesc1(&d)) || d.Monitor != mon)
                continue;
            hdr = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            achou = true;
            break;
        }
    }
    if (!hdr)
        return false;
    // branco SDR: DisplayConfig do caminho cuja fonte e este monitor (SDRWhiteLevel em milesimos de 80 nits)
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, &mi);
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS)
        return true;
    std::vector<DISPLAYCONFIG_PATH_INFO> caminhos(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modos(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, caminhos.data(), &nm, modos.data(), nullptr) != ERROR_SUCCESS)
        return true;
    for (UINT32 i = 0; i < np; ++i)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME fonte{};
        fonte.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        fonte.header.size = sizeof fonte;
        fonte.header.adapterId = caminhos[i].sourceInfo.adapterId;
        fonte.header.id = caminhos[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&fonte.header) != ERROR_SUCCESS || wcscmp(fonte.viewGdiDeviceName, mi.szDevice))
            continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL br{};
        br.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        br.header.size = sizeof br;
        br.header.adapterId = caminhos[i].targetInfo.adapterId;
        br.header.id = caminhos[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&br.header) == ERROR_SUCCESS && br.SDRWhiteLevel > 0)
            escalaSdr = float(br.SDRWhiteLevel) / 1000.f;
        break;
    }
    return true;
}

// ---------------- Descritores ----------------
void Descritores::Cria(ID3D12Device* d, UINT n)
{
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = n;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Ck(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap");
    passo = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    total = n;
    usado = 0;
}

UINT Descritores::Reserva(UINT n)
{
    if (usado + n > total)
        throw std::runtime_error("heap de descritores cheio");
    const UINT i = usado;
    usado += n;
    return i;
}

D3D12_CPU_DESCRIPTOR_HANDLE Descritores::Cpu(UINT i) const
{
    auto h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(i) * passo;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE Descritores::Gpu_(UINT i) const
{
    auto h = heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(i) * passo;
    return h;
}

// ---------------- Tex ----------------
// o tamanho de cada textura viva (para separar no painel a VRAM das texturas do overlay da do resto do processo). Nunca
// e solto: Tex estatico pode ser destruido depois do fim do programa
struct TexVivas
{
    std::mutex mx;
    std::map<const Tex*, UINT64> m;
};
static TexVivas& Vivas()
{
    static auto* v = new TexVivas;
    return *v;
}

UINT64 Tex::BytesVivos()
{
    std::lock_guard<std::mutex> lk(Vivas().mx);
    UINT64 s = 0;
    for (const auto& v : Vivas().m)
        s += v.second;
    return s;
}

Tex::~Tex()
{
    std::lock_guard<std::mutex> lk(Vivas().mx);
    Vivas().m.erase(this);
}

void Tex::Cria(ID3D12Device* d, UINT w_, UINT h_, DXGI_FORMAT f, const wchar_t* nome, bool uav, bool rt)
{
    w = w_;
    h = h_;
    fmt = f;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = f;
    rd.SampleDesc.Count = 1;
    rd.Flags = (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE) |
               (rt ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE);
    estado = D3D12_RESOURCE_STATE_COMMON;
    r.Reset();
    Ck(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, estado, nullptr, IID_PPV_ARGS(&r)),
       "CreateCommittedResource(tex)");
    if (nome)
        r->SetName(nome);
    const UINT64 bytes = d->GetResourceAllocationInfo(0, 1, &rd).SizeInBytes;
    std::lock_guard<std::mutex> lk(Vivas().mx);
    Vivas().m[this] = bytes;
}

void Barreira(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    if (a == b)
    {
        if (a == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            D3D12_RESOURCE_BARRIER u{};
            u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            u.UAV.pResource = r;
            c->ResourceBarrier(1, &u);
        }
        return;
    }
    D3D12_RESOURCE_BARRIER t{};
    t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    c->ResourceBarrier(1, &t);
}

void Tex::Para(ID3D12GraphicsCommandList* c, D3D12_RESOURCE_STATES s)
{
    Barreira(c, r.Get(), estado, s);
    estado = s;
}

// ---------------- Passe ----------------
namespace {
std::mutex g_mxPasses;
std::vector<std::shared_future<void>> g_passes;
constexpr UINT kFlagsShader = D3DCOMPILE_OPTIMIZATION_LEVEL3;

const std::wstring& PastaCacheShaders()
{
    static const std::wstring pasta = [] {
        wchar_t m[MAX_PATH]{};
        GetModuleFileNameW(nullptr, m, MAX_PATH);
        std::wstring s = m;
        s = s.substr(0, s.find_last_of(L"\\/")) + L"\\cache";
        CreateDirectoryW(s.c_str(), nullptr);
        s += L"\\shaders";
        CreateDirectoryW(s.c_str(), nullptr);
        return s;
    }();
    return pasta;
}

uint64_t Fnv(uint64_t h, const void* p, size_t n)
{
    for (size_t i = 0; i < n; ++i)
    {
        h ^= static_cast<const uint8_t*>(p)[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::wstring ArquivoCache(const std::string& hlsl, const std::string& entrada)
{
    static const char kVersao[] = "dlss5ov-cs_5_1-v1";
    uint64_t h = Fnv(14695981039346656037ull, kVersao, sizeof kVersao);
    h = Fnv(h, hlsl.data(), hlsl.size() + 1);
    h = Fnv(h, entrada.data(), entrada.size() + 1);
    h = Fnv(h, &kFlagsShader, sizeof kFlagsShader);
    wchar_t n[32];
    swprintf_s(n, L"%016llx.cso", static_cast<unsigned long long>(h));
    return PastaCacheShaders() + L"\\" + n;
}

bool LeCache(const std::wstring& arq, std::vector<char>& b)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, arq.c_str(), L"rb") || !f)
        return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b.resize(n > 0 ? size_t(n) : 0);
    const bool ok = n > 4 && fread(b.data(), 1, b.size(), f) == b.size() && memcmp(b.data(), "DXBC", 4) == 0;
    fclose(f);
    return ok;
}

void GravaCache(const std::wstring& arq, const std::vector<char>& b)
{
    const std::wstring tmp = arq + L"." + std::to_wstring(GetCurrentThreadId()) + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") || !f)
        return;
    const bool ok = fwrite(b.data(), 1, b.size(), f) == b.size();
    fclose(f);
    if (!ok || !MoveFileExW(tmp.c_str(), arq.c_str(), MOVEFILE_REPLACE_EXISTING))
        DeleteFileW(tmp.c_str());
}

std::vector<char> Compila(const std::string& hlsl, const std::string& entrada, const std::string& nome)
{
    const auto t0 = std::chrono::steady_clock::now();
    ComPtr<ID3DBlob> cs, erro;
    const HRESULT hr = D3DCompile(hlsl.data(), hlsl.size(), nome.c_str(), nullptr, nullptr, entrada.c_str(), "cs_5_1",
                                  kFlagsShader, 0, &cs, &erro);
    if (FAILED(hr))
        throw std::runtime_error(Fmt("shader %s: %s", nome.c_str(), erro ? (const char*)erro->GetBufferPointer() : "?"));
    Log("shader", Fmt("%s compilado em %.2f s", nome.c_str(),
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()));
    const char* p = static_cast<const char*>(cs->GetBufferPointer());
    return std::vector<char>(p, p + cs->GetBufferSize());
}
} // namespace

void EsperaPasses()
{
    std::vector<std::shared_future<void>> v;
    {
        std::lock_guard<std::mutex> l(g_mxPasses);
        v.swap(g_passes);
    }
    std::exception_ptr erro;
    for (auto& f : v)
    {
        try
        {
            f.get();
        }
        catch (...)
        {
            if (!erro)
                erro = std::current_exception();
        }
    }
    if (erro)
        std::rethrow_exception(erro);
}

void Passe::Espera()
{
    if (pronto.valid())
    {
        auto f = std::move(pronto);
        pronto = {};
        f.get();
    }
}

void Passe::Cria(ID3D12Device* d, const char* hlsl, const char* entrada, UINT srvs, UINT uavs, UINT constantes,
                 const char* nome_)
{
    Espera();
    nome = nome_;
    nSrv = srvs;
    nUav = uavs;
    nConst = constantes;
    // o texto vem de temporarios do chamador: copia antes de ir para a thread
    pronto = std::async(std::launch::async, [this, d, codigo = std::string(hlsl), ent = std::string(entrada), srvs,
                                             uavs, constantes] { Monta(d, codigo, ent, srvs, uavs, constantes); })
                 .share();
    std::lock_guard<std::mutex> l(g_mxPasses);
    g_passes.push_back(pronto);
}

void Passe::Monta(ID3D12Device* d, const std::string& hlsl, const std::string& entrada, UINT srvs, UINT uavs,
                  UINT constantes)
{
    const std::wstring arq = ArquivoCache(hlsl, entrada);
    std::vector<char> cs;
    const bool doCache = LeCache(arq, cs);
    if (!doCache)
    {
        cs = Compila(hlsl, entrada, nome);
        GravaCache(arq, cs);
    }
    ComPtr<ID3DBlob> erro;

    D3D12_DESCRIPTOR_RANGE r[2]{};
    D3D12_ROOT_PARAMETER p[3]{};
    UINT np = 0;
    p[np].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[np].Constants.Num32BitValues = constantes ? constantes : 1;
    p[np].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    ++np;
    if (srvs)
    {
        r[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        r[0].NumDescriptors = srvs;
        p[np].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[np].DescriptorTable = {1, &r[0]};
        ++np;
    }
    if (uavs)
    {
        r[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        r[1].NumDescriptors = uavs;
        p[np].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[np].DescriptorTable = {1, &r[1]};
        ++np;
    }
    D3D12_STATIC_SAMPLER_DESC s{};
    s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{np, p, 1, &s, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> ser;
    Ck(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &ser, &erro), "SerializeRootSignature");
    Ck(d->CreateRootSignature(0, ser->GetBufferPointer(), ser->GetBufferSize(), IID_PPV_ARGS(&raiz)),
       "CreateRootSignature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = raiz.Get();
    pd.CS = {cs.data(), cs.size()};
    const HRESULT hp = d->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
    if (FAILED(hp) && doCache)
    {
        // cache estragado: compila de novo
        Log("shader", Fmt("%s: cache recusado, recompilando", nome.c_str()));
        cs = Compila(hlsl, entrada, nome);
        GravaCache(arq, cs);
        pd.CS = {cs.data(), cs.size()};
        Ck(d->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)), "CreateComputePipelineState");
    }
    else
        Ck(hp, "CreateComputePipelineState");
}

void Passe::Roda(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d,
                 const std::vector<std::pair<ID3D12Resource*, DXGI_FORMAT>>& srvs,
                 const std::vector<std::pair<ID3D12Resource*, DXGI_FORMAT>>& uavs, const void* constantes, UINT gx,
                 UINT gy)
{
    Espera();
    if (srvs.size() != nSrv || uavs.size() != nUav)
        throw std::runtime_error("passe " + nome + ": numero de recursos errado");
    const UINT base = desc.Reserva(nSrv + nUav);
    for (UINT i = 0; i < nSrv; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        const auto rd = srvs[i].first->GetDesc();
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            // buffer cru (ByteAddressBuffer)
            v.Format = DXGI_FORMAT_R32_TYPELESS;
            v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            v.Buffer.NumElements = UINT(rd.Width / 4);
            v.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        }
        else
        {
            v.Format = srvs[i].second;
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Texture2D.MipLevels = 1;
        }
        d->CreateShaderResourceView(srvs[i].first, &v, desc.Cpu(base + i));
    }
    for (UINT i = 0; i < nUav; ++i)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
        const auto rd = uavs[i].first->GetDesc();
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            // buffer cru (RWByteAddressBuffer)
            v.Format = DXGI_FORMAT_R32_TYPELESS;
            v.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            v.Buffer.NumElements = UINT(rd.Width / 4);
            v.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        }
        else
        {
            v.Format = uavs[i].second;
            v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        }
        d->CreateUnorderedAccessView(uavs[i].first, nullptr, &v, desc.Cpu(base + nSrv + i));
    }
    ID3D12DescriptorHeap* hs[] = {desc.heap.Get()};
    c->SetDescriptorHeaps(1, hs);
    c->SetComputeRootSignature(raiz.Get());
    c->SetPipelineState(pso.Get());
    UINT ip = 0;
    if (nConst)
        c->SetComputeRoot32BitConstants(ip, nConst, constantes, 0);
    ++ip;
    if (nSrv)
        c->SetComputeRootDescriptorTable(ip++, desc.Gpu_(base));
    if (nUav)
        c->SetComputeRootDescriptorTable(ip++, desc.Gpu_(base + nSrv));
    c->Dispatch(gx, gy, 1);
}

} // namespace ov
