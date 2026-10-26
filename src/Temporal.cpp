#include "Temporal.h"
#include "Shaders.h"
#include <algorithm>

namespace ov {

namespace {
// edicao da resposta: composicao de referencia - original, no espaco comprimido x / (1 + luma) (como o TAA dos jogos)
const char* kEdicao = R"(
float3 Comprime(float3 x) { return x / (1.0 + max(Lum(x), 0.0)); }
cbuffer C : register(b0) { uint w; uint h; uint comp; float sInt; float sCor; float sGuarda; float exposicao; uint pad; };
Texture2D<float4> fed : register(t0);  // proxy que a rede viu
Texture2D<float4> resp : register(t1); // resposta da rede (no espaco do proxy)
Texture2D<float4> orig : register(t2); // o quadro no tamanho da rede (linear, branco de papel = 1)
RWTexture2D<float4> edicao : register(u0);

[numthreads(8, 8, 1)] void CSEdicao(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    float3 O = orig.Load(int3(p, 0)).rgb;
    float3 P = fed.Load(int3(p, 0)).rgb / exposicao;
    float3 M = resp.Load(int3(p, 0)).rgb / exposicao;
    float3 e = comp == 1 ? ComporReferencia(O, P, M, sInt, sCor, sGuarda) - O : M - P;
    edicao[p] = float4(Comprime(O + e) - Comprime(O), 1.0);
}
)";

// de volta a luz linear, com teto: somada a um realce mais claro, a luma comprimida passaria de 1 e c / (1 - luma)
// explodiria. O brilho editado chega no maximo a 4x o original + 4 (a rede reilumina, nao cria sol).
const char* kLinear = R"(
float3 Comprime(float3 x) { return x / (1.0 + max(Lum(x), 0.0)); }
float3 Expande(float3 c) { return c / max(1.0 - Lum(c), 1e-3); }
cbuffer C : register(b0) { uint w; uint h; uint pad0; uint pad1; };
Texture2D<float4> crua : register(t0);
Texture2D<float4> orig : register(t1);
RWTexture2D<float4> edLin : register(u0);

[numthreads(8, 8, 1)] void CSLinear(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    float3 O = orig.Load(int3(p, 0)).rgb;
    float3 c = Comprime(O) + crua.Load(int3(p, 0)).rgb;
    float lo = max(Lum(O), 0.0) * 4.0 + 4.0;
    float lim = lo / (1.0 + lo);
    float lc = Lum(c);
    if (lc > lim) c *= lim / lc;
    edLin[p] = float4(Expande(c) - O, 1.0);
}
)";

struct KEdicao
{
    UINT w, h, comp;
    float sInt, sCor, sGuarda, exposicao;
    UINT pad;
};
const D3D12_RESOURCE_STATES SRV =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
} // namespace

void Temporal::Cria(ID3D12Device* d)
{
    passeEdicao.Cria(d, (std::string(hlsl::Comum) + kEdicao).c_str(), "CSEdicao", 3, 1, 8, "edicao");
    passeLinear.Cria(d, (std::string(hlsl::Comum) + kLinear).c_str(), "CSLinear", 2, 1, 4, "edicao linear");
}

void Temporal::Garante(ID3D12Device* d, UINT w, UINT h)
{
    if (crua.w == w && crua.h == h)
        return;
    crua.Cria(d, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"edicao crua");
    linear.Cria(d, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"edicao (linear)");
    valido = false;
}

Tex* Temporal::Edicao(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex& orig, Tex& fed, Tex& resp,
                      const ParamsTemporal& p)
{
    orig.Para(c, SRV);
    fed.Para(c, SRV);
    resp.Para(c, SRV);
    crua.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    KEdicao k{crua.w,
              crua.h,
              (UINT)std::clamp(p.composicao, 0, 1),
              std::clamp(p.intensidade, 0.f, 2.f), // as faixas da NVIDIA: acima de 2 a luz se inverte (medido)
              std::clamp(p.cor, 0.f, 1.f),
              std::clamp(p.guarda, 1.f, 16.f),
              std::max(p.exposicao, 1e-3f),
              0};
    passeEdicao.Roda(c, desc, d, {{fed.r.Get(), fed.fmt}, {resp.r.Get(), resp.fmt}, {orig.r.Get(), orig.fmt}},
                     {{crua.r.Get(), crua.fmt}}, &k, (crua.w + 7) / 8, (crua.h + 7) / 8);
    crua.Para(c, SRV);
    linear.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const UINT kl[4] = {linear.w, linear.h, 0, 0};
    passeLinear.Roda(c, desc, d, {{crua.r.Get(), crua.fmt}, {orig.r.Get(), orig.fmt}}, {{linear.r.Get(), linear.fmt}},
                     kl, (linear.w + 7) / 8, (linear.h + 7) / 8);
    linear.Para(c, SRV);
    valido = true;
    return &linear;
}

} // namespace ov
