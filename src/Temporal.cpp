#include "Temporal.h"
#include <algorithm>

namespace ov {

namespace {
const char* kHlsl = R"(
cbuffer C : register(b0) { uint w; uint h; uint temResp; uint temHist; float maxN; float rejeicao; float folga; uint temMov;
                           uint comp; float rdxT; float rdxC; float rdxG; };
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

// ---- composicao de cor no estilo RenoDX (clshortfuse, MIT: UpgradeToneMap + correcao de matiz) -- implementacao propria:
// razao de luminancia resposta/original elevada ao Detalhe e limitada em [1/G, G]; Cor mistura o matiz do original na
// luz nova com a cor da rede, e acima de 1 aumenta o croma em OkLab (constantes de Bjorn Ottosson).
static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);
float3 OkLab(float3 c)
{
    float3 lms = float3(dot(c, float3(0.4122214708, 0.5363325363, 0.0514459929)),
                        dot(c, float3(0.2119034982, 0.6806995451, 0.1073969566)),
                        dot(c, float3(0.0883024619, 0.2817188376, 0.6299787005)));
    lms = sign(lms) * pow(abs(lms), 1.0 / 3.0);
    return float3(dot(lms, float3(0.2104542553, 0.7936177850, -0.0040720468)),
                  dot(lms, float3(1.9779984951, -2.4285922050, 0.4505937099)),
                  dot(lms, float3(0.0259040371, 0.7827717662, -0.8086757660)));
}
float3 DeOkLab(float3 L)
{
    float3 lms = float3(dot(L, float3(1.0, 0.3963377774, 0.2158037573)),
                        dot(L, float3(1.0, -0.1055613458, -0.0638541728)),
                        dot(L, float3(1.0, -0.0894841775, -1.2914855480)));
    lms = lms * lms * lms;
    return float3(dot(lms, float3(4.0767416621, -3.3077115913, 0.2309699292)),
                  dot(lms, float3(-1.2684380046, 2.6097574011, -0.3413193965)),
                  dot(lms, float3(-0.0041960863, -0.7034186147, 1.7076147010)));
}
float3 CompoeRenoDX(float3 O, float3 A)
{
    O = max(O, 0.0);
    A = max(A, 0.0);
    const float piso = 1.0 / 512.0; // branco de papel = 1 nas unidades da rede
    float yo = dot(O, LUMA), ya = dot(A, LUMA);
    float rho = max(ya, piso) / max(yo, piso);
    if (rdxT != 1.0) rho = pow(rho, rdxT);
    rho = clamp(rho, 1.0 / rdxG, rdxG);
    float3 luz = O * rho;
    float3 cor = ya > 1e-5 ? A * (dot(luz, LUMA) / ya) : luz;
    float3 r = lerp(luz, cor, saturate(rdxC));
    if (rdxC > 1.0)
    {
        float3 L = OkLab(r);
        L.yz *= rdxC;
        r = max(DeOkLab(L), 0.0);
    }
    return r;
}

float3 Edicao(int2 q)
{
    q = clamp(q, int2(0, 0), int2(w - 1, h - 1));
    float3 o = fed.Load(int3(q, 0)).rgb;
    float3 a = resp.Load(int3(q, 0)).rgb;
    if (comp == 1) a = CompoeRenoDX(o, a);
    return a - o;
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
    UINT comp;
    float rdxT, rdxC, rdxG;
};
const D3D12_RESOURCE_STATES SRV =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
} // namespace

void Temporal::Cria(ID3D12Device* d) { passe.Cria(d, kHlsl, "CSTemporal", 5, 1, 12, "temporal"); }

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
    K k{fed.w,
        fed.h,
        resp ? 1u : 0u,
        valido ? 1u : 0u,
        float(std::max(1, p.maxQuadros)),
        p.rejeicao,
        p.folga,
        mov ? 1u : 0u,
        (UINT)std::clamp(p.composicao, 0, 1),
        std::clamp(p.rdxDetalhe, 0.f, 2.f),
        std::clamp(p.rdxCor, 0.f, 2.f),
        std::clamp(p.rdxGuarda, 1.f, 16.f)};
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
