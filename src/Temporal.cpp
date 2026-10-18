#include "Temporal.h"
#include "Shaders.h"
#include <algorithm>

namespace ov {

namespace {
const char* kHlsl = R"(
cbuffer C : register(b0) { uint w; uint h; uint temResp; uint temHist; float maxN; float rejeicao; float folga; uint temMov;
                           uint comp; float sInt; float sCor; float sGuarda; uint temConf; uint pad0; uint pad1; uint pad2; };
Texture2D<float4> fed : register(t0);     // proxy que a rede viu
Texture2D<float4> resp : register(t1);    // resposta da rede (no espaco do proxy)
Texture2D<float4> mov : register(t2);     // movimento simulado (pixels ate o quadro anterior)
Texture2D<float4> histAnt : register(t3); // edicao acumulada anterior (a = quadros)
Texture2D<float4> fedAnt : register(t4);  // proxy do quadro anterior
Texture2D<float4> orig : register(t5);    // quadro no tamanho da rede (linear, branco de papel = 1)
Texture2D<float> conf : register(t6);     // confianca do fluxo (1 = ida e volta batem)
RWTexture2D<float4> histNovo : register(u0);
RWTexture2D<float4> edicaoCrua : register(u1);
SamplerState s : register(s0);

float LumaP(float3 c) { return pow(max(Lum(c), 0.0), 1.0 / 2.2); }

float3 CatmullRomT(Texture2D<float4> t, float2 uv, float2 tam)
{
    float2 pos = uv * tam;
    float2 c1 = floor(pos - 0.5) + 0.5;
    float2 f = pos - c1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 o12 = w2 / w12;
    float2 t0 = (c1 - 1.0) / tam, t3 = (c1 + 2.0) / tam, t12 = (c1 + o12) / tam;
    float3 r = 0;
    r += t.SampleLevel(s, float2(t0.x, t0.y), 0).rgb * w0.x * w0.y;
    r += t.SampleLevel(s, float2(t12.x, t0.y), 0).rgb * w12.x * w0.y;
    r += t.SampleLevel(s, float2(t3.x, t0.y), 0).rgb * w3.x * w0.y;
    r += t.SampleLevel(s, float2(t0.x, t12.y), 0).rgb * w0.x * w12.y;
    r += t.SampleLevel(s, float2(t12.x, t12.y), 0).rgb * w12.x * w12.y;
    r += t.SampleLevel(s, float2(t3.x, t12.y), 0).rgb * w3.x * w12.y;
    r += t.SampleLevel(s, float2(t0.x, t3.y), 0).rgb * w0.x * w3.y;
    r += t.SampleLevel(s, float2(t12.x, t3.y), 0).rgb * w12.x * w3.y;
    r += t.SampleLevel(s, float2(t3.x, t3.y), 0).rgb * w3.x * w3.y;
    return r;
}

// a edicao deste quadro: o que a composicao muda no original
float3 Edicao(int2 q)
{
    q = clamp(q, int2(0, 0), int2(w - 1, h - 1));
    float3 O = orig.Load(int3(q, 0)).rgb;
    float3 P = fed.Load(int3(q, 0)).rgb;
    float3 M = resp.Load(int3(q, 0)).rgb;
    if (comp == 1)
        return ComporReferencia(O, P, M, sInt, sCor, sGuarda) - O;
    return M - P;
}

[numthreads(8, 8, 1)] void CSTemporal(uint2 pu : SV_DispatchThreadID)
{
    if (pu.x >= w || pu.y >= h) return;
    int2 p = int2(pu);
    float2 tam = float2(w, h);
    float2 v = temMov ? mov.Load(int3(p, 0)).xy : float2(0, 0);
    float2 uvAnt = (float2(p) + 0.5 + v) / tam;
    bool dentro = all(uvAnt > 0.0) && all(uvAnt < 1.0);
    float3 hr = 0;
    float n = 0;
    if (temHist && temMov && dentro)
    {
        hr = CatmullRomT(histAnt, uvAnt, tam);
        n = histAnt.SampleLevel(s, uvAnt, 0).a;
        // rejeicao fotometrica: o ponto reprojetado tem de ter a mesma cor no quadro anterior
        float d = abs(LumaP(fed.Load(int3(p, 0)).rgb) - LumaP(fedAnt.SampleLevel(s, uvAnt, 0).rgb));
        n *= saturate(1.0 - d * rejeicao);
        // confianca do fluxo: oclusao / vetor que nao volta ao mesmo ponto derruba o historico
        if (temConf) n *= conf.Load(int3(p, 0));
    }
    if (temResp)
    {
        float3 e = Edicao(p);
        edicaoCrua[pu] = float4(e, 1.0);
        // limite pela vizinhanca 3x3 da edicao atual (contra fantasma)
        float3 mn = e, mx = e;
        [unroll] for (int j = -1; j <= 1; ++j)
            [unroll] for (int i = -1; i <= 1; ++i)
            {
                float3 q = Edicao(p + int2(i, j));
                mn = min(mn, q);
                mx = max(mx, q);
            }
        hr = clamp(hr, mn - folga, mx + folga);
        n = min(n + 1.0, maxN);
        histNovo[pu] = float4(lerp(hr, e, 1.0 / n), n);
    }
    else
    {
        // quadro sem rede: a edicao anterior segue o movimento (onde o fluxo nao confia, some aos poucos)
        float k = temConf ? conf.Load(int3(p, 0)) : 1.0;
        histNovo[pu] = float4(hr * k, max(n, 1.0));
    }
}
)";

struct K
{
    UINT w, h, temResp, temHist;
    float maxN, rejeicao, folga;
    UINT temMov;
    UINT comp;
    float sInt, sCor, sGuarda;
    UINT temConf, pad0, pad1, pad2;
};
const D3D12_RESOURCE_STATES SRV =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
} // namespace

void Temporal::Cria(ID3D12Device* d)
{
    const std::string fonte = std::string(hlsl::Comum) + kHlsl;
    passe.Cria(d, fonte.c_str(), "CSTemporal", 7, 2, 16, "temporal");
}

void Temporal::Garante(ID3D12Device* d, UINT w, UINT h)
{
    if (hist[0].w == w && hist[0].h == h)
        return;
    for (auto& t : hist)
        t.Cria(d, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"historico da edicao");
    crua.Cria(d, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"edicao crua");
    valido = false;
}

Tex* Temporal::Roda(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex& orig, Tex& fed, Tex& fedAnt,
                    ID3D12Resource* resp, DXGI_FORMAT fmtResp, Tex* mov, Tex* confianca, const ParamsTemporal& p)
{
    if (!resp && !valido)
        return nullptr; // sem rede e sem historico: nada a mostrar
    Tex& ant = hist[atual];
    Tex& novo = hist[atual ^ 1];
    ant.Para(c, SRV);
    fedAnt.Para(c, SRV);
    novo.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    crua.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    K k{fed.w,
        fed.h,
        resp ? 1u : 0u,
        valido ? 1u : 0u,
        float(std::max(1, p.maxQuadros)),
        p.rejeicao,
        p.folga,
        mov ? 1u : 0u,
        (UINT)std::clamp(p.composicao, 0, 1),
        std::clamp(p.intensidade, 0.f, 2.f),
        std::clamp(p.cor, 0.f, 1.f),
        std::clamp(p.guarda, 1.f, 16.f),
        confianca ? 1u : 0u,
        0,
        0,
        0};
    using Par = std::pair<ID3D12Resource*, DXGI_FORMAT>;
    passe.Roda(c, desc, d,
               {{fed.r.Get(), fed.fmt},
                resp ? Par{resp, fmtResp} : Par{fed.r.Get(), fed.fmt},
                mov ? Par{mov->r.Get(), mov->fmt} : Par{fed.r.Get(), fed.fmt},
                {ant.r.Get(), ant.fmt},
                {fedAnt.r.Get(), fedAnt.fmt},
                {orig.r.Get(), orig.fmt},
                confianca ? Par{confianca->r.Get(), confianca->fmt} : Par{fed.r.Get(), fed.fmt}},
               {{novo.r.Get(), novo.fmt}, {crua.r.Get(), crua.fmt}}, &k, (fed.w + 7) / 8, (fed.h + 7) / 8);
    novo.Para(c, SRV);
    atual ^= 1;
    valido = true;
    return &novo;
}

} // namespace ov
