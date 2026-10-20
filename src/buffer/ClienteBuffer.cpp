#include "ClienteBuffer.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

namespace dlss5ov {

namespace {
// Os tres passes do jogo. C(x) = x / (1 + luma) e E(y) = y / (1 - luma): os mesmos do overlay (Temporal.cpp).
const char* kHlsl = R"(
static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);
float Lum(float3 c) { return dot(c, LUMA); }
float3 Comprime(float3 x) { return x / (1.0 + max(Lum(x), 0.0)); }
float3 Expande(float3 c) { return c / max(1.0 - Lum(c), 1e-3); }
float3 Linear(float3 c) { return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 Srgb(float3 c) { c = saturate(c); return (c <= 0.0031308) ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
cbuffer K : register(b0)
{
    uint corW, corH, movW, movH;
    uint profW, profH, exporta, aplica;
    float movEscX, movEscY, preExp, escExp; // movEsc: MV do jogo -> pixels da cor
    uint temExp, srgb, invertida, infinita;
    float perto, longe, pad0, pad1;
};
Texture2D<float2> movJogo : register(t0);
Texture2D<float> profJogo : register(t1);
Texture2D<float> expJogo : register(t2);
Texture2D<float4> edicao : register(t3);
RWTexture2D<float4> corJogo : register(u0);
RWTexture2D<float4> corSlot : register(u1);
RWTexture2D<float2> movSlot : register(u2);
RWTexture2D<float> profSlot : register(u3);
RWTexture2D<float> expSlot : register(u4);
SamplerState lin : register(s0);

static const float CEU = 1e6;
// profundidade do jogo -> distancia da camera (a mesma conta no overlay e no jogo: Fluxo/DoJogo, Exporta, CSCor)
float Dist(float d)
{
    if (!isfinite(d)) return CEU;
    const float e = 1e-7;
    float z;
    if (perto > 0.0)
    {
        if (invertida)
            z = (infinita || longe <= perto) ? perto / max(d, e) : perto * longe / max(perto + d * (longe - perto), e);
        else
            z = (infinita || longe <= perto) ? perto / max(1.0 - d, e) : perto * longe / max(longe - d * (longe - perto), e);
    }
    else
        z = invertida ? 1.0 / max(d, e) : 1.0 / max(1.0 - d, e);
    return min(z, CEU);
}
// mesma superficie: distancias relativas perto (as duas no ceu tambem)
float Mesma(float za, float zb)
{
    if (za >= CEU * 0.5 && zb >= CEU * 0.5) return 1.0;
    return 1.0 - smoothstep(0.04, 0.15, abs(za - zb) / max(min(za, zb), 1e-6));
}

float Exposicao()
{
    return temExp ? clamp(expJogo.Load(int3(0, 0, 0)) * escExp / max(preExp, 1e-6), 1.0 / 256.0, 256.0) : 1.0;
}

[numthreads(8, 8, 1)] void CSMov(uint2 p : SV_DispatchThreadID)
{
    if (all(p == 0))
        expSlot[uint2(0, 0)] = Exposicao();
    if (p.x >= movW || p.y >= movH) return;
    float2 m = movJogo.Load(int3(p, 0)) * float2(movEscX, movEscY);
    movSlot[p] = all(isfinite(m)) ? m : 0;
}

[numthreads(8, 8, 1)] void CSProf(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= profW || p.y >= profH) return;
    profSlot[p] = profJogo.Load(int3(p, 0));
}

[numthreads(8, 8, 1)] void CSCor(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= corW || p.y >= corH) return;
    const float4 c4 = corJogo[p];
    float3 c = srgb ? Linear(c4.rgb) : c4.rgb;
    c = all(isfinite(c)) ? c : 0;
    const float e = Exposicao();
    const float3 ce = max(c * e, 0);
    if (exporta)
        corSlot[p] = float4(ce, 1);
    if (!aplica) return;
    // a edicao foi feita no quadro anterior: este pixel estava em p + mov (vetores do jogo, do atual ao anterior)
    const uint2 pm = min(uint2((p + 0.5) * float2(movW, movH) / float2(corW, corH)), uint2(movW - 1, movH - 1));
    float2 m = movJogo.Load(int3(pm, 0)) * float2(movEscX, movEscY);
    m = all(isfinite(m)) ? m : 0;
    // so a mesma superficie: a edicao leva no alfa log2 da distancia de cada pixel no quadro dela
    const uint2 pp = min(uint2((p + 0.5) * float2(profW, profH) / float2(corW, corH)), uint2(profW - 1, profH - 1));
    const float zc = Dist(profJogo.Load(int3(pp, 0)));
    const float2 q = clamp(p + 0.5 + m, 0.5, float2(corW, corH) - 0.5);
    const float2 b = q - 0.5;
    const int2 i0 = int2(floor(b));
    const float2 fr = b - i0;
    const int2 lim = int2(corW, corH) - 1;
    float3 ed = 0;
    float soma = 0;
    [unroll] for (int k = 0; k < 4; ++k)
    {
        const int2 o = int2(k & 1, k >> 1);
        const float4 v = edicao.Load(int3(clamp(i0 + o, 0, lim), 0));
        const float wv = (o.x ? fr.x : 1.0 - fr.x) * (o.y ? fr.y : 1.0 - fr.y) * Mesma(exp2(v.a), zc);
        ed += v.rgb * wv;
        soma += wv;
    }
    if (soma < 0.05)
    {
        // fundo recem-descoberto (ali, no quadro da edicao, havia outra superficie): media suave da mesma superficie em
        // volta -- anel de 16 direcoes que cresce ate 128 px; usa o primeiro raio com amostras e o seguinte, peso 1/r.
        // (uma amostra por direcao copiava o detalhe de outro lugar e virava listras)
        ed = 0;
        soma = 0;
        float achou = 0.0;
        [loop] for (float r = 4.0; r <= 128.0 && achou < 2.0; r *= 2.0)
        {
            float s1 = 0;
            [unroll] for (int k = 0; k < 16; ++k)
            {
                float2 dir;
                sincos(k * 0.3926991 + r * 0.1, dir.y, dir.x);
                const float4 v = edicao.Load(int3(clamp(int2(q + dir * r), 0, lim), 0));
                const float wv = Mesma(exp2(v.a), zc) / r;
                ed += v.rgb * wv;
                s1 += wv;
            }
            soma += s1;
            if (s1 > 0.5 / r || achou > 0.0)
                achou += 1.0;
        }
    }
    ed = soma > 1e-4 ? ed / soma : 0;
    float3 o = Expande(Comprime(ce) + ed);
    o = clamp(o, 0.0, 4.0 * ce + 4.0); // o mesmo teto do overlay: realce nunca explode
    o /= e;
    corJogo[p] = float4(srgb ? Srgb(o) : o, c4.a);
}
)";

DXGI_FORMAT Leitura(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return f;
    }
}

bool Srgb(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

bool Compila(const char* entrada, ID3DBlob** b, std::string& erro)
{
    Microsoft::WRL::ComPtr<ID3DBlob> e;
    if (SUCCEEDED(D3DCompile(kHlsl, strlen(kHlsl), "dlss5ov_jogo", nullptr, nullptr, entrada, "cs_5_1",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, b, &e)))
        return true;
    erro = std::string("shader ") + entrada + ": " + (e ? (const char*)e->GetBufferPointer() : "?");
    return false;
}
} // namespace

bool ClienteBuffer::Inicia(ID3D12Device* d, const char* exe)
{
    dev = d;
    wchar_t nome[96];
    NomeMapa(nome, 96, GetCurrentProcessId());
    mapa = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Cabecalho), nome);
    if (!mapa)
    {
        erro = "CreateFileMapping";
        return false;
    }
    cab = static_cast<Cabecalho*>(MapViewOfFile(mapa, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Cabecalho)));
    if (!cab)
    {
        erro = "MapViewOfFile";
        return false;
    }
    memset(cab, 0, sizeof(Cabecalho));
    cab->versao = kVersao;
    cab->tamanho = sizeof(Cabecalho);
    cab->pidJogo = GetCurrentProcessId();
    strncpy_s(cab->exe, exe ? exe : "", _TRUNCATE);
    cab->aplica = 1;

    // raiz: 16 constantes, SRV t0..t3, UAV u0..u4, amostrador linear com borda presa
    D3D12_DESCRIPTOR_RANGE r[2]{};
    r[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0};
    r[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 5, 0, 0, 0};
    D3D12_ROOT_PARAMETER p[3]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.Num32BitValues = 20;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[1].DescriptorTable = {1, &r[0]};
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[2].DescriptorTable = {1, &r[1]};
    D3D12_STATIC_SAMPLER_DESC s{};
    s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rd{3, p, 1, &s, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Com<ID3DBlob> ser, e;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &ser, &e)) ||
        FAILED(d->CreateRootSignature(0, ser->GetBufferPointer(), ser->GetBufferSize(), IID_PPV_ARGS(&raiz))))
    {
        erro = "root signature";
        return false;
    }
    const char* ent[3] = {"CSMov", "CSProf", "CSCor"};
    Com<ID3D12PipelineState>* pso[3] = {&psoMov, &psoProf, &psoCor};
    for (int i = 0; i < 3; ++i)
    {
        Com<ID3DBlob> b;
        if (!Compila(ent[i], &b, erro))
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = raiz.Get();
        pd.CS = {b->GetBufferPointer(), b->GetBufferSize()};
        if (FAILED(d->CreateComputePipelineState(&pd, IID_PPV_ARGS(&*pso[i]))))
        {
            erro = "pso";
            return false;
        }
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 9 * 3 * 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))))
    {
        erro = "descriptor heap";
        return false;
    }
    passoDesc = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

void ClienteBuffer::Encerra()
{
    if (cab)
    {
        cab->geracao = 0;
        UnmapViewOfFile(cab);
        cab = nullptr;
    }
    if (mapa)
        CloseHandle(mapa);
    mapa = nullptr;
}

bool ClienteBuffer::OverlayVivo() const
{
    return cab && cab->ativo && GetTickCount64() - cab->batimentoOverlay < 1000;
}

bool ClienteBuffer::CriaTex(Com<ID3D12Resource>& t, UINT w, UINT h, DXGI_FORMAT f, const wchar_t* qual, uint32_t slot)
{
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.Format = f;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    t.Reset();
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(&t))))
        return false;
    wchar_t nome[128];
    NomeRecurso(nome, 128, GetCurrentProcessId(), geracao, qual, slot);
    HANDLE h_ = nullptr;
    if (FAILED(dev->CreateSharedHandle(t.Get(), nullptr, GENERIC_ALL, nome, &h_)))
        return false;
    handles.push_back(h_); // o nome vale enquanto houver um handle aberto
    return true;
}

bool ClienteBuffer::CriaCerca(Com<ID3D12Fence>& f, const wchar_t* qual)
{
    f.Reset();
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f))))
        return false;
    wchar_t nome[128];
    NomeRecurso(nome, 128, GetCurrentProcessId(), geracao, qual, 0);
    HANDLE h_ = nullptr;
    if (FAILED(dev->CreateSharedHandle(f.Get(), nullptr, GENERIC_ALL, nome, &h_)))
        return false;
    handles.push_back(h_);
    return true;
}

bool ClienteBuffer::Garante(const EntradasJogo& e)
{
    const D3D12_RESOURCE_DESC dc = e.cor->GetDesc(), dm = e.mov->GetDesc(), dp = e.prof->GetDesc();
    auto val = [](uint32_t v, UINT64 total) { return UINT(v && v <= total ? v : total); };
    const UINT cw = val(e.corValidoW, dc.Width), ch = val(e.corValidoH, dc.Height);
    const UINT mw = val(e.movValidoW, dm.Width), mh = val(e.movValidoH, dm.Height);
    const UINT pw = val(e.profValidoW, dp.Width), ph = val(e.profValidoH, dp.Height);
    const bool mesmaCor = (temp != nullptr) == !(dc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
                          (!temp || (temp->GetDesc().Width == dc.Width && temp->GetDesc().Height == dc.Height));
    if (geracao && cw == corW && ch == corH && dc.Format == fmtCor && mw == movW && mh == movH && dm.Format == fmtMov &&
        pw == profW && ph == profH && dp.Format == fmtProf && mesmaCor)
        return true;
    cab->geracao = 0; // o overlay solta a geracao velha
    for (HANDLE h_ : handles)
        CloseHandle(h_);
    handles.clear();
    ++geracao;
    corW = cw, corH = ch, fmtCor = dc.Format;
    movW = mw, movH = mh, fmtMov = dm.Format;
    profW = pw, profH = ph, fmtProf = dp.Format;
    bool ok = CriaCerca(cercaJogo, L"cercaJogo") && CriaCerca(cercaOverlay, L"cercaOverlay");
    for (uint32_t i = 0; ok && i < kSlotsEntrada; ++i)
        ok = CriaTex(cor[i], UINT(corW), corH, DXGI_FORMAT_R16G16B16A16_FLOAT, L"cor", i) &&
             CriaTex(mov[i], movW, movH, DXGI_FORMAT_R16G16_FLOAT, L"mov", i) &&
             CriaTex(prof[i], profW, profH, DXGI_FORMAT_R32_FLOAT, L"prof", i) &&
             CriaTex(exp[i], 1, 1, DXGI_FORMAT_R32_FLOAT, L"exp", i);
    for (uint32_t i = 0; ok && i < kSlotsEdicao; ++i)
        ok = CriaTex(edicao[i], UINT(corW), corH, DXGI_FORMAT_R16G16B16A16_FLOAT, L"edicao", i);
    temp.Reset();
    if (ok && !(dc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
    {
        // a cor do jogo nao aceita UAV: soma numa copia (mesma familia de formato) e copia de volta
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd = dc;
        rd.Format = Leitura(dc.Format);
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        rd.MipLevels = 1;
        ok = SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    IID_PPV_ARGS(&temp)));
    }
    if (!ok)
    {
        erro = "texturas compartilhadas";
        geracao = 0;
        return false;
    }
    for (auto& s : cab->entrada)
        s = {};
    for (auto& s : cab->edicao)
        s = {};
    for (auto& l : cab->lido)
        l = 0;
    cab->consumido = 0;
    cab->corW = UINT(corW), cab->corH = corH, cab->movW = movW, cab->movH = movH, cab->profW = profW, cab->profH = profH;
    quadro = pendente = 0;
    MemoryBarrier();
    cab->geracao = geracao;
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE ClienteBuffer::Cpu(UINT i) const
{
    auto h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(i) * passoDesc;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE ClienteBuffer::Gpu(UINT i) const
{
    auto h = heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(i) * passoDesc;
    return h;
}

void ClienteBuffer::Srv(UINT i, ID3D12Resource* r, DXGI_FORMAT f)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = f;
    v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(r, &v, Cpu(i));
}

void ClienteBuffer::Uav(UINT i, ID3D12Resource* r, DXGI_FORMAT f)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
    v.Format = f;
    v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(r, nullptr, &v, Cpu(i));
}

void ClienteBuffer::Barreira(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a,
                             D3D12_RESOURCE_STATES b)
{
    if (!r || a == b)
        return;
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    c->ResourceBarrier(1, &x);
}

uint64_t ClienteBuffer::Quadro(ID3D12GraphicsCommandList* c, const EntradasJogo& e)
{
    if (!cab || !e.cor || !e.mov || !e.prof)
        return 0;
    cab->batimentoJogo = GetTickCount64();
    if (!OverlayVivo())
        return 0;
    if (!Garante(e))
        return 0;
    const uint64_t n = ++quadro;
    cab->quadroJogo = n;
    cab->profInvertida = e.profInvertidaConhecida ? (e.profInvertida ? 1u : 0u) : 2u;
    cab->profInfinita = e.profInfinita;
    cab->hdr = e.hdr;

    // exporta se o overlay ja copiou o que estava no slot
    const uint32_t s = uint32_t(n % kSlotsEntrada);
    const bool exporta = cab->consumido >= cab->entrada[s].quadro;
    // aplica a edicao publicada mais nova cuja escrita ja terminou (nunca uma velha demais: o movimento de um quadro so
    // reprojeta um quadro)
    int k = -1;
    if (cab->aplica)
    {
        const uint64_t pronto = cercaOverlay->GetCompletedValue();
        for (int i = 0; i < int(kSlotsEdicao); ++i)
        {
            const SlotEdicao& se = cab->edicao[i];
            if (se.quadro && se.cerca && pronto >= se.cerca && (k < 0 || se.quadro > cab->edicao[k].quadro))
                k = i;
        }
        if (k >= 0 && cab->edicao[k].quadro + 8 < n)
            k = -1;
    }
    if (!exporta && k < 0)
        return 0;
    if (k >= 0)
    {
        cab->lido[k] = n;
        cab->quadroAplicado = n;
    }
    if (exporta)
    {
        cab->entrada[s].quadro = n;
        cab->entrada[s].jitterX = e.jitterX;
        cab->entrada[s].jitterY = e.jitterY;
        cab->entrada[s].reset = e.reset;
        cab->entrada[s].perto = e.perto;
        cab->entrada[s].longe = e.longe;
        cab->entrada[s].fovY = e.fovY;
    }
    MemoryBarrier();

    // descritores deste quadro: 3 conjuntos (o jogo pode ter ate 3 quadros na GPU)
    const UINT base = (proxDesc % 3) * 36;
    ++proxDesc;
    const UINT tS = base, tU = base + 4; // SRV t0..t3, UAV u0..u4 (um conjunto por passe nao e preciso: mesmos)
    const DXGI_FORMAT fMov = Leitura(fmtMov), fProf = Leitura(fmtProf), fCor = Leitura(fmtCor);
    Srv(tS + 0, e.mov, fMov);
    Srv(tS + 1, e.prof, fProf);
    Srv(tS + 2, e.exposicao, e.exposicao ? Leitura(e.exposicao->GetDesc().Format) : DXGI_FORMAT_R32_FLOAT);
    Srv(tS + 3, k >= 0 ? edicao[k].Get() : nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ID3D12Resource* alvo = temp ? temp.Get() : e.cor;
    Uav(tU + 0, alvo, fCor);
    Uav(tU + 1, exporta ? cor[s].Get() : nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT);
    Uav(tU + 2, exporta ? mov[s].Get() : nullptr, DXGI_FORMAT_R16G16_FLOAT);
    Uav(tU + 3, exporta ? prof[s].Get() : nullptr, DXGI_FORMAT_R32_FLOAT);
    Uav(tU + 4, exporta ? exp[s].Get() : nullptr, DXGI_FORMAT_R32_FLOAT);

    const UINT mw = e.movEspacoW ? e.movEspacoW : movW, mh = e.movEspacoH ? e.movEspacoH : movH;
    struct
    {
        UINT corW, corH, movW, movH, profW, profH, exporta, aplica;
        float movEscX, movEscY, preExp, escExp;
        UINT temExp, srgb, invertida, infinita;
        float perto, longe, pad0, pad1;
    } k_{UINT(corW), corH, movW, movH, profW, profH, exporta ? 1u : 0u, k >= 0 ? 1u : 0u,
         e.movEscalaX * float(corW) / float(mw), e.movEscalaY * float(corH) / float(mh), e.preExposicao,
         e.escalaExposicao, e.exposicao ? 1u : 0u, Srgb(fmtCor) ? 1u : 0u,
         (e.profInvertidaConhecida ? e.profInvertida : cab->profInvertidaUsada != 0) ? 1u : 0u,
         e.profInfinita ? 1u : 0u, e.perto, e.longe, 0, 0};

    const D3D12_RESOURCE_STATES SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Barreira(c, e.mov, e.movEstado, SRV);
    Barreira(c, e.prof, e.profEstado, SRV);
    Barreira(c, e.exposicao, e.expEstado, SRV);
    if (temp)
    {
        Barreira(c, e.cor, e.corEstado, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barreira(c, temp.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        c->CopyResource(temp.Get(), e.cor);
        Barreira(c, temp.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    else
        Barreira(c, e.cor, e.corEstado, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (exporta)
        for (ID3D12Resource* r : {cor[s].Get(), mov[s].Get(), prof[s].Get(), exp[s].Get()})
            Barreira(c, r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (k >= 0)
        Barreira(c, edicao[k].Get(), D3D12_RESOURCE_STATE_COMMON, SRV);

    ID3D12DescriptorHeap* hs[] = {heap.Get()};
    c->SetDescriptorHeaps(1, hs);
    c->SetComputeRootSignature(raiz.Get());
    c->SetComputeRoot32BitConstants(0, 20, &k_, 0);
    c->SetComputeRootDescriptorTable(1, Gpu(tS));
    c->SetComputeRootDescriptorTable(2, Gpu(tU));
    if (exporta)
    {
        c->SetPipelineState(psoMov.Get());
        c->Dispatch((movW + 7) / 8, (movH + 7) / 8, 1);
        c->SetPipelineState(psoProf.Get());
        c->Dispatch((profW + 7) / 8, (profH + 7) / 8, 1);
    }
    c->SetPipelineState(psoCor.Get());
    c->Dispatch(UINT((corW + 7) / 8), (corH + 7) / 8, 1);

    if (k >= 0)
        Barreira(c, edicao[k].Get(), SRV, D3D12_RESOURCE_STATE_COMMON);
    if (exporta)
        for (ID3D12Resource* r : {cor[s].Get(), mov[s].Get(), prof[s].Get(), exp[s].Get()})
            Barreira(c, r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    if (temp)
    {
        Barreira(c, temp.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barreira(c, e.cor, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        if (k >= 0)
            c->CopyResource(e.cor, temp.Get());
        Barreira(c, e.cor, D3D12_RESOURCE_STATE_COPY_DEST, e.corEstado);
        Barreira(c, temp.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    else
        Barreira(c, e.cor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, e.corEstado);
    Barreira(c, e.exposicao, SRV, e.expEstado);
    Barreira(c, e.prof, SRV, e.profEstado);
    Barreira(c, e.mov, SRV, e.movEstado);
    pendente = n;
    return n;
}

void ClienteBuffer::Submetido(ID3D12CommandQueue* q)
{
    if (pendente && cercaJogo && q)
    {
        q->Signal(cercaJogo.Get(), pendente);
        pendente = 0;
    }
}

} // namespace dlss5ov
