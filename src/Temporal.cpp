#include "Temporal.h"
#include <algorithm>

namespace ov {

namespace {
const char* kHlsl = R"(
cbuffer C : register(b0) { uint w; uint h; uint temResp; uint temHist; float maxN; float rejeicao; float folga; uint temMov; };
Texture2D<float4> fed : register(t0);
Texture2D<float4> resp : register(t1);
Texture2D<float4> mov : register(t2);
Texture2D<float4> histAnt : register(t3);
Texture2D<float4> fedAnt : register(t4);
RWTexture2D<float4> histNovo : register(u0);
SamplerState s : register(s0);

float Luma(float3 c) { return pow(max(dot(c, float3(0.2126, 0.7152, 0.0722)), 0.0), 1.0 / 2.2); }

float3 CatmullRom(Texture2D<float4> t, float2 uv, float2 tam)
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

float3 Edicao(int2 q)
{
    q = clamp(q, int2(0, 0), int2(w - 1, h - 1));
    return resp.Load(int3(q, 0)).rgb - fed.Load(int3(q, 0)).rgb;
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
        hr = CatmullRom(histAnt, uvAnt, tam);
        n = histAnt.SampleLevel(s, uvAnt, 0).a;
        // rejeicao fotometrica: o ponto reprojetado tem de ter a mesma cor no quadro anterior
        float d = abs(Luma(fed.Load(int3(p, 0)).rgb) - Luma(fedAnt.SampleLevel(s, uvAnt, 0).rgb));
        n *= saturate(1.0 - d * rejeicao);
    }
    if (temResp)
    {
        float3 e = Edicao(p);
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
        // quadro sem rede: a edicao anterior segue o movimento
        histNovo[pu] = float4(hr, max(n, 1.0));
    }
}
)";

struct K
{
    UINT w, h, temResp, temHist;
    float maxN, rejeicao, folga;
    UINT temMov;
};
const D3D12_RESOURCE_STATES SRV =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
} // namespace

void Temporal::Cria(ID3D12Device* d) { passe.Cria(d, kHlsl, "CSTemporal", 5, 1, 8, "temporal"); }

void Temporal::Garante(ID3D12Device* d, UINT w, UINT h)
{
    if (hist[0].w == w && hist[0].h == h)
        return;
    for (auto& t : hist)
        t.Cria(d, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"historico da edicao");
    valido = false;
}

Tex* Temporal::Roda(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex& fed, Tex& fedAnt,
                    ID3D12Resource* resp, DXGI_FORMAT fmtResp, Tex* mov, const ParamsTemporal& p)
{
    if (!resp && !valido)
        return nullptr; // sem rede e sem historico: nada a mostrar
    Tex& ant = hist[atual];
    Tex& novo = hist[atual ^ 1];
    ant.Para(c, SRV);
    fedAnt.Para(c, SRV);
    novo.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    K k{fed.w, fed.h, resp ? 1u : 0u, valido ? 1u : 0u, float(std::max(1, p.maxQuadros)), p.rejeicao, p.folga,
        mov ? 1u : 0u};
    passe.Roda(c, desc, d,
               {{fed.r.Get(), fed.fmt},
                {resp ? resp : fed.r.Get(), resp ? fmtResp : fed.fmt},
                mov ? std::pair<ID3D12Resource*, DXGI_FORMAT>{mov->r.Get(), mov->fmt}
                    : std::pair<ID3D12Resource*, DXGI_FORMAT>{fed.r.Get(), fed.fmt},
                {ant.r.Get(), ant.fmt},
                {fedAnt.r.Get(), fedAnt.fmt}},
               {{novo.r.Get(), novo.fmt}}, &k, (fed.w + 7) / 8, (fed.h + 7) / 8);
    novo.Para(c, SRV);
    atual ^= 1;
    valido = true;
    return &novo;
}

} // namespace ov
