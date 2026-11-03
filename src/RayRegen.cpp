#include "RayRegen.h"
#include "../externo/fidelityfx_sdk_240/denoisers/include/ffx_denoiser.h"
#include "../externo/fidelityfx_sdk_240/api/include/dx12/ffx_api_dx12.h"
#include <algorithm>
#include <cstring>

namespace ov {

namespace {
const D3D12_RESOURCE_STATES SRV = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// A rede tem uma curva de tom: clareia o escuro e segura o claro. O filtro espacial mistura esses ganhos entre vizinhos: o escuro perde, a luz ganha (medido na gravacao do Cyberpunk: -14% e
// +38%). Como o albedo no manual da AMD (5.1.1), o que vem do material sai antes do filtro e volta depois: a curva ganho x brilho deste quadro sai, ele filtra so o resto, e ela volta pixel a pixel.
#define KCURVA_COMUM R"(
static const uint CURVA_N = 32;
static const float CURVA_MIN = -10.0, CURVA_PASSOS = 2.0; // 2^-10 a 2^6, meio stop por caixa
float Lum(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
static const float PISO = 1.0 / 512.0;
float GanhoRede(float3 e, float l, uint luz) { return luz == 0 ? max((l + Lum(e)) / l, PISO) : exp2(Lum(e)); }
)"
const char* kCurvaZera = R"(
RWTexture2D<int> caixas : register(u0);
[numthreads(32, 2, 1)] void CSZera(uint2 p : SV_DispatchThreadID) { caixas[p] = 0; }
)";
const char* kCurvaSoma = KCURVA_COMUM R"(
cbuffer C : register(b0) { uint w; uint h; uint luz; uint p0; };
Texture2D<float4> ed : register(t0);
Texture2D<float4> orig : register(t1);
RWTexture2D<int> caixas : register(u0);
[numthreads(8, 8, 1)] void CSSoma(uint2 p : SV_DispatchThreadID)
{
    // um de cada quatro pixels: a soma cabe em 32 bits ate 8K
    const uint2 q = p * 2;
    if (q.x >= w || q.y >= h) return;
    const float l = max(Lum(max(orig.Load(int3(q, 0)).rgb, 0.0)), PISO);
    const float g = log2(GanhoRede(ed.Load(int3(q, 0)).rgb, l, luz));
    const uint c = uint(clamp((log2(l) - CURVA_MIN) * CURVA_PASSOS, 0.0, float(CURVA_N - 1)));
    InterlockedAdd(caixas[uint2(c, 0)], int(clamp(g, -8.0, 8.0) * 64.0));
    InterlockedAdd(caixas[uint2(c, 1)], 1);
}
)";
const char* kCurvaFecha = R"(
cbuffer C : register(b0) { uint temAnt; uint p0; uint p1; uint p2; };
Texture2D<int> caixas : register(t0);
RWTexture2D<float> curva : register(u0);
// caixa com poucos pixels nao define a curva: vale a vizinha; devagar entre quadros, senao a curva cintila
[numthreads(1, 1, 1)] void CSFecha()
{
    float v[32];
    int ultimo = -1;
    for (int i = 0; i < 32; ++i)
    {
        const int n = caixas.Load(int3(i, 1, 0));
        if (n < 32) { v[i] = 1e9; continue; }
        v[i] = float(caixas.Load(int3(i, 0, 0))) / (64.0 * float(n));
        if (ultimo >= 0)
            for (int k = ultimo + 1; k < i; ++k)
                v[k] = lerp(v[ultimo], v[i], float(k - ultimo) / float(i - ultimo));
        else
            for (int k = 0; k < i; ++k)
                v[k] = v[i];
        ultimo = i;
    }
    for (int i = 0; i < 32; ++i)
    {
        const float a = ultimo < 0 ? 0.0 : (i > ultimo ? v[ultimo] : v[i]);
        curva[uint2(i, 0)] = temAnt ? lerp(curva[uint2(i, 0)], a, 0.2) : a;
    }
}
)";

// a imagem pronta como luz e albedo branco: separar por canal dava valores instaveis nas cores saturadas (um canal quase zero) e mudava as cores
const char* kEntra = KCURVA_COMUM R"(
cbuffer C : register(b0) { uint w; uint h; uint luz; uint modo; uint p0; uint p1; uint p2; uint p3; };
Texture2D<float4> ed : register(t0);
Texture2D<float4> orig : register(t1);
Texture2D<float> curva : register(t2);
float CurvaEm(float l)
{
    const float x = clamp((log2(l) - CURVA_MIN) * CURVA_PASSOS - 0.5, 0.0, float(CURVA_N - 1));
    const uint a = uint(x);
    return exp2(lerp(curva.Load(int3(a, 0, 0)), curva.Load(int3(min(a + 1, CURVA_N - 1), 0, 0)), x - float(a)));
}
RWTexture2D<float4> sinal : register(u0);
RWTexture2D<float4> albedo : register(u1);
[numthreads(8, 8, 1)] void CSEntra(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    const float3 e = ed.Load(int3(p, 0)).rgb, o = max(orig.Load(int3(p, 0)).rgb, 0.0);
    if (modo == 0)
    {
        sinal[p] = float4(max(luz == 0 ? o + e : max(o, PISO) * exp2(e), 0.0), 1.0);
        albedo[p] = 1.0;
        return;
    }
    // so o brilho que a rede muda: o matiz da edicao fica, e o jogo vai como material
    const float l = max(Lum(o), PISO);
    const float g = GanhoRede(e, l, luz) / CurvaEm(l);
    sinal[p] = float4(g, g, g, 1.0);
    albedo[p] = modo == 2 ? float4(1.0, 1.0, 1.0, 1.0) : float4(sqrt(saturate(o / (1.0 + o))), 1.0);
}
)";

// camera olhando para -Z (como o sample): profundidade negativa
const char* kGeo = R"(
cbuffer C : register(b0) { uint w; uint h; uint temDist; uint invertida; float tanY; uint temAnt; float perto; float longe; uint infinita; uint movW; uint movH; float escX; float escY; float jitU; float jitV; uint p0; };
Texture2D<float> dist : register(t0);
Texture2D<float2> mov : register(t1);
Texture2D<float> profAnt : register(t2);
RWTexture2D<float> prof : register(u0);
RWTexture2D<float4> movZ : register(u1);
SamplerState amostra : register(s0);
// a profundidade como o jogo escreveu -> Z da camera (a conta do Fluxo). Sem perto e longe conhecidos, a forma e real e a escala e relativa
float Linear(float d)
{
    if (!isfinite(d)) return 1e6;
    const float e = 1e-7;
    float z;
    if (infinita || longe <= perto)
        z = perto / max(invertida ? d : 1.0 - d, e);
    else if (invertida)
        z = perto * longe / max(perto + d * (longe - perto), e);
    else
        z = perto * longe / max(longe - d * (longe - perto), e);
    return min(z, 1e6);
}
float Z(float2 uv, int2 q)
{
    if (temDist == 0) return 10.0;
    if (temDist == 3) return -profAnt.Load(int3(q, 0)); // faltou a deste quadro: a ultima, em vez de pular para a fixa
    // interpolada na profundidade que o jogo escreveu: ela e linear na tela para um plano, o Z nao. O pixel mais proximo, com a profundidade
    // menor que a edicao, faz degraus: as normais saem em listras e o historico cai em faixas
    if (temDist == 2) return Linear(dist.SampleLevel(amostra, uv, 0));
    // gravacao: a distancia radial; o SDK pede o Z da camera
    return min(dist.SampleLevel(amostra, uv, 0), 1e6) / length(float3((uv.x * 2.0 - 1.0) * tanY * float(w) / float(h), (1.0 - uv.y * 2.0) * tanY, 1.0));
}
// o movimento do jogo vem no tamanho de render: interpolado so entre pixels da mesma superficie (no silhueta nao mistura o personagem com o fundo)
float2 Movimento(float2 uv, float zq)
{
    if (movW == w && movH == h) return mov.Load(int3(clamp(int2(uv * float2(w, h)), int2(0, 0), int2(w, h) - 1), 0));
    const float2 t = uv * float2(movW, movH) - 0.5;
    const int2 b = int2(floor(t));
    const float2 f = t - float2(b);
    float2 soma = 0.0;
    float pesos = 0.0;
    [unroll] for (int j = 0; j < 2; ++j)
        [unroll] for (int i = 0; i < 2; ++i)
        {
            const int2 q = clamp(b + int2(i, j), int2(0, 0), int2(movW, movH) - 1);
            const float zv = temDist == 2 ? Linear(dist.SampleLevel(amostra, (float2(q) + 0.5) / float2(movW, movH), 0)) : zq;
            const float mesma = abs(zv - zq) <= 0.05 * zq ? 1.0 : 1e-3;
            const float p = (i ? f.x : 1.0 - f.x) * (j ? f.y : 1.0 - f.y) * mesma + 1e-6;
            soma += mov.Load(int3(q, 0)) * p;
            pesos += p;
        }
    return soma / pesos;
}
[numthreads(8, 8, 1)] void CSGeo(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    const float2 uv = (float2(p) + 0.5) / float2(w, h);
    // a cor do FSR nao tem o jitter do quadro; a profundidade e o movimento tem: o que esta em uv na cor esta em uv + jitter neles
    const float2 uvJ = uv + float2(jitU, jitV);
    const float zq = Z(uvJ, int2(p));
    prof[p] = -zq;
    float2 m = Movimento(uvJ, zq) * float2(escX, escY);
    m = all(isfinite(m)) && all(abs(m) < 2.0 * float(max(w, h))) ? m : 0.0; // o Cyberpunk mandou +-65504 num quadro
    // o movimento com a variacao da profundidade linear no Z: anterior - atual, no ponto de onde o pixel veio
    const float2 a = (float2(p) + 0.5 + m) / float2(w, h);
    const bool dentro = temAnt && temDist && temDist != 3 && all(a >= 0.0) && all(a <= 1.0);
    movZ[p] = float4(m, dentro ? profAnt.SampleLevel(amostra, a, 0) + zq : 0.0, 0.0);
}
)";

// poucos pontos da profundidade como o jogo escreveu: Z invertido ou nao, medido na CPU com folga
const char* kBrutos = R"(
cbuffer C : register(b0) { uint gw; uint gh; uint p0; uint p1; };
Texture2D<float> dist : register(t0);
Texture2D<float> prof : register(t1);
RWTexture2D<float2> brutos : register(u0);
SamplerState amostra : register(s0);
[numthreads(8, 8, 1)] void CSBrutos(uint2 g : SV_DispatchThreadID)
{
    const float2 uv = (float2(g) + 0.5) / float2(gw, gh);
    if (g.x < gw && g.y < gh) brutos[g] = float2(dist.SampleLevel(amostra, uv, 0), -prof.SampleLevel(amostra, uv, 0));
}
)";

// as normais da profundidade; sem ela, de frente
const char* kNormais = R"(
cbuffer C : register(b0) { uint w; uint h; float tanY; uint reais; float limLuz; uint p0; uint p1; uint p2; };
Texture2D<float> prof : register(t0);
Texture2D<float4> orig : register(t1);
RWTexture2D<float4> normais : register(u0);
float Z(int2 q) { return -prof.Load(int3(clamp(q, int2(0, 0), int2(w, h) - 1), 0)); }
float3 Ponto(int2 q)
{
    const float2 uv = (float2(q) + 0.5) / float2(w, h);
    return float3((uv.x * 2.0 - 1.0) * tanY * float(w) / float(h), (1.0 - uv.y * 2.0) * tanY, -1.0) * Z(q);
}
// o codigo do manual da AMD (5.3.1)
float2 NormalToOctahedronUv(float3 N)
{
    N.xy /= abs(N.x) + abs(N.y) + abs(N.z);
    const float2 k = sign(N.xy);
    if (N.z < 0.0) N.xy = (1.0 - abs(N.yx)) * k;
    return N.xy * 0.5 + 0.5;
}
[numthreads(8, 8, 1)] void CSNormais(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    const int2 q = int2(p);
    float3 n = float3(0.0, 0.0, 1.0);
    if (reais)
    {
        const float zc = Z(q), zl = Z(q - int2(1, 0)), zr = Z(q + int2(1, 0)), zu = Z(q - int2(0, 1)), zd = Z(q + int2(0, 1));
        const float3 c = Ponto(q);
        const float3 dx = abs(zr - zc) < abs(zl - zc) ? Ponto(q + int2(1, 0)) - c : c - Ponto(q - int2(1, 0));
        const float3 dy = abs(zd - zc) < abs(zu - zc) ? c - Ponto(q + int2(0, 1)) : Ponto(q - int2(0, 1)) - c;
        const float3 m = cross(dx, dy);
        if (dot(m, m) > 1e-20) n = normalize(m);
        if (n.z < 0.0) n = -n;
    }
    // material type (manual 5.4): o SDK nao mistura pixels de materiais diferentes. Luz e cena separadas: o ganho do escuro em volta vazava para a luz
    const float l = dot(max(orig.Load(int3(p, 0)).rgb, 0.0), float3(0.2126, 0.7152, 0.0722));
    normais[p] = float4(NormalToOctahedronUv(n), 0.5, l >= limLuz ? 1.0 / 3.0 : 0.0);
}
)";

// a luz em escala grande nao e ruido: a media de cada bloco depois do filtro volta a ser a de antes
const char* kBaixa = R"(
cbuffer C : register(b0) { uint w; uint h; uint p0; uint p1; };
Texture2D<float4> sinal : register(t0);
Texture2D<float4> filtrado : register(t1);
RWTexture2D<float2> baixa : register(u0);
[numthreads(8, 8, 1)] void CSBaixa(uint2 g : SV_DispatchThreadID)
{
    float2 s = 0.0;
    float n = 0.0;
    for (uint y = 0; y < 16; ++y)
        for (uint x = 0; x < 16; ++x)
        {
            const uint2 q = g * 16 + uint2(x, y);
            if (q.x >= w || q.y >= h) continue;
            s += float2(sinal.Load(int3(q, 0)).g, filtrado.Load(int3(q, 0)).g);
            n += 1.0;
        }
    baixa[g] = n > 0.0 ? s / n : float2(1.0, 1.0);
}
)";

const char* kSai = KCURVA_COMUM R"(
cbuffer C : register(b0) { uint w; uint h; uint luz; float forca; uint modo; uint bw; uint bh; uint p2; };
Texture2D<float4> filtrado : register(t0);
Texture2D<float4> orig : register(t1);
Texture2D<float4> ed : register(t2);
Texture2D<float> curva : register(t3);
Texture2D<float2> baixa : register(t4);
RWTexture2D<float4> res : register(u0);
SamplerState amostra : register(s0);
float CurvaEm(float l)
{
    const float x = clamp((log2(l) - CURVA_MIN) * CURVA_PASSOS - 0.5, 0.0, float(CURVA_N - 1));
    const uint a = uint(x);
    return exp2(lerp(curva.Load(int3(a, 0, 0)), curva.Load(int3(min(a + 1, CURVA_N - 1), 0, 0)), x - float(a)));
}
[numthreads(8, 8, 1)] void CSSai(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    const float4 e = ed.Load(int3(p, 0));
    const float3 r = max(filtrado.Load(int3(p, 0)).rgb, 0.0), o = max(orig.Load(int3(p, 0)).rgb, 0.0);
    float3 n;
    if (modo == 0)
        n = luz == 0 ? r - o : log2(max(r, PISO) / max(o, PISO));
    else
    {
        const float l = max(Lum(o), PISO);
        const float2 b = baixa.SampleLevel(amostra, (float2(p) + 0.5) / (16.0 * float2(bw, bh)), 0);
        const float g = max(r.g * clamp(b.x / max(b.y, PISO), 0.5, 2.0), PISO) * CurvaEm(l);
        const float ant = GanhoRede(e.rgb, l, luz);
        n = luz == 0 ? (o + e.rgb) * (g / ant) - o : e.rgb + log2(g / ant);
    }
    res[p] = float4(lerp(e.rgb, n, forca), e.a);
}
)";
} // namespace

RayRegen::~RayRegen() { Fecha(); }

void RayRegen::Fecha()
{
    if (ctx && fnDestroi)
    {
        ffxContext c = ctx;
        reinterpret_cast<PfnFfxDestroyContext>(fnDestroi)(&c, nullptr);
    }
    ctx = nullptr;
    ctxW = ctxH = 0;
}

void RayRegen::Cria(ID3D12Device* d)
{
    pCurvaZera.Cria(d, kCurvaZera, "CSZera", 0, 1, 0, "ray regeneration: curva, zera");
    pCurvaSoma.Cria(d, kCurvaSoma, "CSSoma", 2, 1, 4, "ray regeneration: curva, soma");
    pCurvaFecha.Cria(d, kCurvaFecha, "CSFecha", 1, 1, 4, "ray regeneration: curva");
    pEntra.Cria(d, kEntra, "CSEntra", 3, 2, 8, "ray regeneration: entrada");
    pGeo.Cria(d, kGeo, "CSGeo", 3, 2, 16, "ray regeneration: geometria");
    pBrutos.Cria(d, kBrutos, "CSBrutos", 2, 1, 4, "ray regeneration: pontos da profundidade");
    pNormais.Cria(d, kNormais, "CSNormais", 2, 1, 8, "ray regeneration: normais");
    pBaixa.Cria(d, kBaixa, "CSBaixa", 2, 1, 4, "ray regeneration: luz grande");
    pSai.Cria(d, kSai, "CSSai", 5, 1, 8, "ray regeneration: saida");
}

bool RayRegen::Abre(Gpu& gpu, const std::wstring& pasta, UINT w, UINT h, bool comDepuracao)
{
    if (!dll)
    {
        dll = LoadLibraryW((pasta + L"\\amd_fidelityfx_loader_dx12.dll").c_str());
        if (dll)
        {
            fnCria = reinterpret_cast<void*>(GetProcAddress(dll, "ffxCreateContext"));
            fnDestroi = reinterpret_cast<void*>(GetProcAddress(dll, "ffxDestroyContext"));
            fnDespacha = reinterpret_cast<void*>(GetProcAddress(dll, "ffxDispatch"));
            fnConfigura = reinterpret_cast<void*>(GetProcAddress(dll, "ffxConfigure"));
            fnConsulta = reinterpret_cast<void*>(GetProcAddress(dll, "ffxQuery"));
        }
        if (!fnCria || !fnDestroi || !fnDespacha)
        {
            Log("denoise", "Ray Regeneration: amd_fidelityfx_loader_dx12.dll nao carregou (fica ao lado do exe)");
            return false;
        }
    }
    gpu.Espera(); // o contexto velho ainda pode estar em uso
    Fecha();
    ffxCreateContextDescDenoiser d{};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
    d.version = FFX_DENOISER_VERSION;
    d.maxRenderSize = {w, h};
    d.signalFlags = FFX_DENOISER_SIGNAL_DIRECT_DIFFUSE;
    d.flags = comDepuracao ? FFX_DENOISER_ENABLE_DEBUGGING : 0u; // gasta mais memoria: so com a visao ligada
    ffxCreateBackendDX12Desc b{};
    b.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    b.device = gpu.dev.Get();
    d.header.pNext = &b.header;
    ffxContext c = nullptr;
    const ffxReturnCode_t r = reinterpret_cast<PfnFfxCreateContext>(fnCria)(&c, &d.header, nullptr);
    if (r != FFX_API_RETURN_OK || !c)
    {
        Log("denoise", Fmt("Ray Regeneration: o contexto nao criou (codigo %u): exige RX 9000 ou mais nova", unsigned(r)));
        return false;
    }
    ctx = c;
    ctxDepura = comDepuracao;
    ctxW = w, ctxH = h;
    indice = 0;
    for (int i = 0; i < kParamsRayRegen; ++i)
    {
        ffxQueryDescDenoiserGetDefaultKeyValue q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_DENOISER_GET_DEFAULT_KEYVALUE;
        q.key = uint64_t(i + 1);
        q.count = 1;
        q.data = &padrao[i];
        if (fnConsulta)
            reinterpret_cast<PfnFfxQuery>(fnConsulta)(&c, &q.header);
        aplicado[i] = padrao[i];
    }
    Log("denoise", Fmt("Ray Regeneration ligado: %ux%u; padroes: normal %.3f, estabilidade %.3f, radiancia maxima %.1f, recorte %.3f, gaussiana %.3f, oclusao %.3f", w, h,
                       padrao[0], padrao[1], padrao[2], padrao[3], padrao[4], padrao[5]));
    return true;
}

void RayRegen::Recursos(ID3D12Device* dev, UINT w, UINT h)
{
    sinal.Cria(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"ray regeneration: sinal");
    saida.Cria(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"ray regeneration: saida");
    resultado.Cria(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"ray regeneration: edicao");
    albedo.Cria(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, L"ray regeneration: albedo");
    prof.Cria(dev, w, h, DXGI_FORMAT_R32_FLOAT, L"ray regeneration: profundidade");
    normais.Cria(dev, w, h, DXGI_FORMAT_R10G10B10A2_UNORM, L"ray regeneration: normais");
    profAnterior.Cria(dev, w, h, DXGI_FORMAT_R32_FLOAT, L"ray regeneration: profundidade anterior");
    movZ.Cria(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"ray regeneration: movimento");
    baixa.Cria(dev, (w + 15) / 16, (h + 15) / 16, DXGI_FORMAT_R32G32_FLOAT, L"ray regeneration: luz grande");
    if (!curva.r)
    {
        caixas.Cria(dev, 32, 2, DXGI_FORMAT_R32_SINT, L"ray regeneration: curva, caixas");
        curva.Cria(dev, 32, 1, DXGI_FORMAT_R32_FLOAT, L"ray regeneration: curva");
    }
    if (!brutos.r)
    {
        brutos.Cria(dev, kBrutosW, kBrutosH, DXGI_FORMAT_R32G32_FLOAT, L"ray regeneration: pontos da profundidade");
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = UINT64(256) * kBrutosH;
        rd.Height = 1;
        rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Ck(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&brutosLeitura)), "buffer(pontos da profundidade)");
    }
    temAnterior = false;
}

void RayRegen::Prepara(ID3D12GraphicsCommandList* c, Descritores& desc, Gpu& gpu, Tex& orig, const EntradaRayRegen& e)
{
    prepPronto = false;
    if (falhou || !e.mov)
        return;
    const UINT w = orig.w, h = orig.h;
    if (prof.w != w || prof.h != h)
    {
        gpu.Espera(); // as texturas velhas ainda podem estar em uso
        Recursos(gpu.dev.Get(), w, h);
    }
    Tex* dist = e.dist;
    // sem a profundidade deste quadro, a ultima por 30 quadros: pular para a fixa e voltar derruba o historico duas vezes
    fonte = dist ? (e.distCrua ? 2 : 1) : 0;
    if (dist)
        semProf = 0;
    else if (fonteReal && temAnterior && !e.salto && ++semProf <= 30)
        fonte = 3;
    const bool real = fonte != 0;
    prepReset = e.salto || !temAnterior || real != fonteReal;
    fonteReal = real;
    prepTan = e.tanY > 0.f ? e.tanY : 0.577f;
    // o jogo diz (buffer), ou mede do quadro anterior
    MedePontos(e.invertidaConhecida);
    if (e.invertidaConhecida)
        invertida = e.invertida;
    const UINT gx = (w + 7) / 8, gy = (h + 7) / 8;
    using Par = std::pair<ID3D12Resource*, DXGI_FORMAT>;
    orig.Para(c, SRV);
    e.mov->Para(c, SRV);
    if (dist)
        dist->Para(c, SRV);
    prof.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    movZ.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    profAnterior.Para(c, SRV);
    const Par pDist = dist ? Par{dist->r.Get(), dist->fmt} : Par{orig.r.Get(), orig.fmt};
    const float jitU = dist && e.distCrua ? e.jitterX / float(dist->w) : 0.f, jitV = dist && e.distCrua ? e.jitterY / float(dist->h) : 0.f;
    struct { UINT w, h, temDist, inv; float tanY; UINT temAnt; float perto, longe; UINT infinita, movW, movH; float escX, escY, jitU, jitV; UINT p0; } kg{
        w, h, UINT(fonte), invertida ? 1u : 0u, prepTan, prepReset ? 0u : 1u, e.perto > 0.f ? e.perto : 0.1f, e.longe, e.perto > 0.f && !e.infinita ? 0u : 1u, e.mov->w, e.mov->h, e.escMovX, e.escMovY, jitU, jitV, 0};
    pGeo.Roda(c, desc, gpu.dev.Get(), {pDist, Par{e.mov->r.Get(), e.mov->fmt}, Par{profAnterior.r.Get(), profAnterior.fmt}},
              {Par{prof.r.Get(), prof.fmt}, Par{movZ.r.Get(), movZ.fmt}}, &kg, gx, gy);
    prof.Para(c, SRV);
    movZ.Para(c, SRV);
    if (fonte != 0 && fonte != 3 && (comVista || (fonte == 2 && !e.invertidaConhecida)))
    {
        brutos.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        struct { UINT gw, gh, p0, p1; } kb{kBrutosW, kBrutosH, 0, 0};
        pBrutos.Roda(c, desc, gpu.dev.Get(), {pDist, Par{prof.r.Get(), prof.fmt}}, {Par{brutos.r.Get(), brutos.fmt}}, &kb, (kBrutosW + 7) / 8, (kBrutosH + 7) / 8);
        brutos.Para(c, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION de{}, para{};
        de.pResource = brutos.r.Get();
        de.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        para.pResource = brutosLeitura.Get();
        para.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        para.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32_FLOAT, kBrutosW, kBrutosH, 1, 256};
        c->CopyTextureRegion(&para, 0, 0, 0, &de, nullptr);
        brutosGravados = true;
    }
    prepPronto = true;
}

// Z invertido: a cena e o ceu ficam perto de 0. Um quadro de parede encostada nao vira a escala
void RayRegen::MedePontos(bool invertidaConhecida)
{
    if (!brutosGravados)
        return;
    brutosGravados = false;
    void* mapa = nullptr;
    D3D12_RANGE faixa{0, SIZE_T(256) * kBrutosH};
    if (FAILED(brutosLeitura->Map(0, &faixa, &mapa)))
        return;
    int baixos = 0;
    float zs[kBrutosW * kBrutosH], ds[kBrutosW * kBrutosH];
    for (UINT y = 0; y < kBrutosH; ++y)
    {
        const float* l = reinterpret_cast<const float*>(static_cast<const char*>(mapa) + size_t(y) * 256);
        for (UINT x = 0; x < kBrutosW; ++x)
        {
            baixos += l[x * 2] < 0.5f;
            ds[y * kBrutosW + x] = l[x * 2];
            zs[y * kBrutosW + x] = l[x * 2 + 1];
        }
    }
    D3D12_RANGE nada{0, 0};
    brutosLeitura->Unmap(0, &nada);
    std::nth_element(zs, zs + kBrutosW * kBrutosH / 2, zs + kBrutosW * kBrutosH);
    zTipico = zs[kBrutosW * kBrutosH / 2];
    // cobertura: tudo menos o valor mais comum (o de limpeza do buffer: ceu, ou nada desenhado)
    std::sort(ds, ds + kBrutosW * kBrutosH);
    int run = 1, maior = 1;
    for (UINT i = 1; i < kBrutosW * kBrutosH; ++i)
    {
        run = ds[i] - ds[i - 1] <= 1e-6f ? run + 1 : 1;
        maior = std::max(maior, run);
    }
    cobertura = 1.f - float(maior) / float(kBrutosW * kBrutosH);
    coberturaNova = true;
    if (invertidaConhecida)
        return;
    const bool agora = baixos * 2 > int(kBrutosW * kBrutosH);
    contraInvertida = agora != invertida ? contraInvertida + 1 : 0;
    if (contraInvertida >= 30 || quadrosFiltro <= 1)
    {
        invertida = agora;
        contraInvertida = 0;
    }
}

Tex* RayRegen::Filtra(ID3D12GraphicsCommandList* c, Descritores& desc, Gpu& gpu, const std::wstring& pasta, Tex& edicao, Tex& orig, UINT luz, float forca, int modo, const float* params, int vista, UINT vistaW, UINT vistaH, float branco)
{
    comVista = vista > 0;
    if (falhou)
        estado = "denoise indisponivel: veja o registro";
    if (falhou || !prepPronto)
        return nullptr;
    prepPronto = false;
    const UINT w = edicao.w, h = edicao.h;
    if (orig.w != w || orig.h != h || prof.w != w || prof.h != h)
    {
        static bool dito = false;
        if (!dito)
            Log("denoise", Fmt("Ray Regeneration pulado: edicao %ux%u, jogo %ux%u, profundidade %ux%u", w, h, orig.w, orig.h, prof.w, prof.h));
        dito = true;
        return nullptr;
    }
    if (!ctx || ctxW != w || ctxH != h || ctxDepura != (vista > 0))
    {
        if (!Abre(gpu, pasta, w, h, vista > 0))
        {
            falhou = true;
            return nullptr;
        }
        prepReset = true; // historico novo
        limiteVisao = 0.f;
    }
    for (int i = 0; i < kParamsRayRegen; ++i)
    {
        // negativo = o padrao da AMD
        const float v = params[i] < 0.f ? padrao[i] : params[i];
        if (v == aplicado[i] || !fnConfigura)
            continue;
        ffxConfigureDescDenoiserKeyValue k{};
        k.header.type = FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE;
        k.key = uint64_t(i + 1);
        k.count = 1;
        k.data = &v;
        ffxContext cc = ctx;
        reinterpret_cast<PfnFfxConfigure>(fnConfigura)(&cc, &k.header);
        aplicado[i] = v;
    }
    ++quadrosFiltro;
    faltas += fonte == 3;
    resets += prepReset;
    if (quadrosFiltro % 60 == 0)
        Log("denoise", Fmt("de %u quadros: faltou a profundidade em %u, historico recomecou em %u (fonte %d)", quadrosFiltro, faltas, resets, fonte));
    // a visao "Profundidade" da AMD normaliza por um teto (1024 por padrao): com a cena a 10 unidades, ou em escala relativa, ela fica preta
    if (comVista && zTipico > 0.f && fnConfigura && (limiteVisao <= 0.f || zTipico * 4.f > limiteVisao * 2.f || zTipico * 4.f < limiteVisao * 0.5f))
    {
        limiteVisao = zTipico * 4.f;
        FfxApiFloatBounds lim{0.f, limiteVisao};
        ffxConfigureDescDenoiserKeyValue k{};
        k.header.type = FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE;
        k.key = FFX_API_CONFIGURE_DENOISER_KEY_DEBUG_VIEW_LINEAR_DEPTH_BOUNDS;
        k.count = 1;
        k.data = &lim;
        ffxContext cc = ctx;
        reinterpret_cast<PfnFfxConfigure>(fnConfigura)(&cc, &k.header);
    }
    estado = fonte == 0 ? std::string("denoise sem profundidade") : fonte == 3 ? std::string("faltou a profundidade deste quadro") : std::string();
    using Par = std::pair<ID3D12Resource*, DXGI_FORMAT>;
    const UINT gx = (w + 7) / 8, gy = (h + 7) / 8;
    edicao.Para(c, SRV);
    orig.Para(c, SRV);
    sinal.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    albedo.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (modo != 0)
    {
        caixas.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        pCurvaZera.Roda(c, desc, gpu.dev.Get(), {}, {Par{caixas.r.Get(), caixas.fmt}}, nullptr, 1, 1);
        caixas.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); // barreira UAV: zeradas antes da soma
        struct { UINT w, h, luz, p0; } ks0{w, h, luz, 0};
        pCurvaSoma.Roda(c, desc, gpu.dev.Get(), {Par{edicao.r.Get(), edicao.fmt}, Par{orig.r.Get(), orig.fmt}}, {Par{caixas.r.Get(), caixas.fmt}}, &ks0, (w / 2 + 8) / 8, (h / 2 + 8) / 8);
        caixas.Para(c, SRV);
        curva.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        struct { UINT temAnt, p0, p1, p2; } kf{prepReset ? 0u : 1u, 0, 0, 0};
        pCurvaFecha.Roda(c, desc, gpu.dev.Get(), {Par{caixas.r.Get(), caixas.fmt}}, {Par{curva.r.Get(), curva.fmt}}, &kf, 1, 1);
    }
    curva.Para(c, SRV);
    struct { UINT w, h, luz, modo, p0, p1, p2, p3; } ke{w, h, luz, UINT(modo), 0, 0, 0, 0};
    pEntra.Roda(c, desc, gpu.dev.Get(), {Par{edicao.r.Get(), edicao.fmt}, Par{orig.r.Get(), orig.fmt}, Par{curva.r.Get(), curva.fmt}},
                {Par{sinal.r.Get(), sinal.fmt}, Par{albedo.r.Get(), albedo.fmt}}, &ke, gx, gy);
    // luz = acima de 40% do branco de papel (na "imagem inteira" nao ha ganho para vazar)
    struct { UINT w, h; float tanY; UINT reais; float limLuz; UINT p0, p1, p2; } kn{w, h, prepTan, params[0] > 0.f && fonte != 0 ? 1u : 0u, modo != 0 ? 0.4f * branco : 1e30f, 0, 0, 0};
    normais.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    pNormais.Roda(c, desc, gpu.dev.Get(), {Par{prof.r.Get(), prof.fmt}, Par{orig.r.Get(), orig.fmt}}, {Par{normais.r.Get(), normais.fmt}}, &kn, gx, gy);
    sinal.Para(c, SRV);
    albedo.Para(c, SRV);
    normais.Para(c, SRV);
    saida.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ffxDispatchDescDenoiser d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER;
    d.commandList = c;
    d.linearDepth = ffxApiGetResourceDX12(prof.r.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.motionVectors = ffxApiGetResourceDX12(movZ.r.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.normals = ffxApiGetResourceDX12(normais.r.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.diffuseAlbedo = ffxApiGetResourceDX12(albedo.r.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.motionVectorScale = {1.f / float(w), 1.f / float(h), 1.f}; // o movimento vem em pixels, atual -> anterior
    // camera identidade; a projecao com o FOV que as normais acima usam
    float identidade[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float f = 1.f / prepTan, perto = 0.1f, longe = 1e6f;
    float proj[16] = {f * float(h) / float(w), 0, 0, 0, 0, f, 0, 0, 0, 0, longe / (perto - longe), -1, 0, 0, perto * longe / (perto - longe), 0};
    static_assert(sizeof(d.view) == sizeof(identidade), "matriz 4x4 de float");
    std::memcpy(&d.view, identidade, sizeof identidade);
    std::memcpy(&d.projection, proj, sizeof proj);
    d.linearDepthBounds = {0.f, 1e7f};
    d.renderSize = {w, h};
    d.frameIndex = indice++;
    d.flags = prepReset ? FFX_DENOISER_DISPATCH_RESET : 0u;
    ffxDispatchDescDenoiserDirectDiffuse s{};
    s.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE;
    s.signal.input = ffxApiGetResourceDX12(sinal.r.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    s.signal.output = ffxApiGetResourceDX12(saida.r.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.header.pNext = &s.header;
    ffxDispatchDescDenoiserDebugView dv{};
    if (vista > 0)
    {
        if (depura.w != vistaW || depura.h != vistaH)
            depura.Cria(gpu.dev.Get(), vistaW, vistaH, DXGI_FORMAT_R16G16B16A16_FLOAT, L"ray regeneration: depuracao");
        depura.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dv.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DEBUG_VIEW;
        dv.output = ffxApiGetResourceDX12(depura.r.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        dv.outputSize = {vistaW, vistaH};
        // 1 = visao geral; 2 em diante = um painel em tela cheia, na ordem do manual
        dv.mode = vista == 1 ? FFX_API_DENOISER_DEBUG_VIEW_MODE_OVERVIEW : FFX_API_DENOISER_DEBUG_VIEW_MODE_FULLSCREEN_VIEWPORT;
        dv.viewportIndex = vista == 1 ? 0u : UINT(vista - 2);
        s.header.pNext = &dv.header;
    }
    ffxContext ct = ctx;
    const ffxReturnCode_t r = reinterpret_cast<PfnFfxDispatch>(fnDespacha)(&ct, &d.header);
    if (r != FFX_API_RETURN_OK)
    {
        Log("denoise", Fmt("Ray Regeneration: o despacho falhou (codigo %u); a edicao segue sem filtro", unsigned(r)));
        falhou = true;
        return nullptr;
    }
    if (vista > 0)
        depura.Para(c, SRV);
    saida.Para(c, SRV);
    // a profundidade deste quadro e a anterior do proximo
    prof.Para(c, D3D12_RESOURCE_STATE_COPY_SOURCE);
    profAnterior.Para(c, D3D12_RESOURCE_STATE_COPY_DEST);
    c->CopyResource(profAnterior.r.Get(), prof.r.Get());
    temAnterior = true;
    resultado.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    baixa.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (modo != 0)
    {
        struct { UINT w, h, p0, p1; } kb{w, h, 0, 0};
        pBaixa.Roda(c, desc, gpu.dev.Get(), {Par{sinal.r.Get(), sinal.fmt}, Par{saida.r.Get(), saida.fmt}}, {Par{baixa.r.Get(), baixa.fmt}}, &kb, (baixa.w + 7) / 8, (baixa.h + 7) / 8);
    }
    baixa.Para(c, SRV);
    struct { UINT w, h, luz; float forca; UINT modo, bw, bh, p2; } ks{w, h, luz, std::clamp(forca, 0.f, 1.f), UINT(modo), baixa.w, baixa.h, 0};
    pSai.Roda(c, desc, gpu.dev.Get(), {Par{saida.r.Get(), saida.fmt}, Par{orig.r.Get(), orig.fmt}, Par{edicao.r.Get(), edicao.fmt}, Par{curva.r.Get(), curva.fmt}, Par{baixa.r.Get(), baixa.fmt}},
              {Par{resultado.r.Get(), resultado.fmt}}, &ks, gx, gy);
    resultado.Para(c, SRV);
    return &resultado;
}

} // namespace ov