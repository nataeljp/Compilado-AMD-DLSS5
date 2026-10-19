#include "Temporal.h"
#include "Shaders.h"
#include <algorithm>

namespace ov {

namespace {
// edicao da resposta no espaco do quadro que a rede viu (t0): composicao de referencia - original
const char* kEdicao = R"(
// espaco comprimido (x / (1 + luma), como o TAA dos jogos): o realce em HDR nao domina o acumulo nem o reposicionamento
float3 Comprime(float3 x) { return x / (1.0 + max(Lum(x), 0.0)); }
float3 Expande(float3 c) { return c / max(1.0 - Lum(c), 1e-3); }
cbuffer C : register(b0) { uint w; uint h; uint comp; float sInt; float sCor; float sGuarda; float exposicao; uint pad; };
Texture2D<float4> fed : register(t0);  // proxy que a rede viu
Texture2D<float4> resp : register(t1); // resposta da rede (no espaco do proxy)
Texture2D<float4> orig : register(t2); // o quadro de t0 no tamanho da rede (linear, branco de papel = 1)
RWTexture2D<float4> edicao : register(u0);

[numthreads(8, 8, 1)] void CSEdicao(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    float3 O = orig.Load(int3(p, 0)).rgb;
    float3 P = fed.Load(int3(p, 0)).rgb / exposicao;
    float3 M = resp.Load(int3(p, 0)).rgb / exposicao;
    float3 e = comp == 1 ? ComporReferencia(O, P, M, sInt, sCor, sGuarda) - O : M - P;
    edicao[p] = float4(Comprime(O + e) - Comprime(O), 1.0); // o acumulo e no espaco comprimido
}
)";

// deslocamento acumulado do quadro atual ate t0 (o quadro que a rede viu): D_t(p) = v_t(p) + D_{t-1}(p + v_t(p)).
// z = menor confianca do fluxo no caminho, w = 1 enquanto o caminho fica dentro da imagem
const char* kAcumula = R"(
cbuffer C : register(b0) { uint w; uint h; uint zera; uint temMov; };
Texture2D<float4> mov : register(t0);  // pixels ate o quadro anterior
Texture2D<float> conf : register(t1);
Texture2D<float4> ant : register(t2);  // deslocamento acumulado ate o quadro anterior
RWTexture2D<float4> novo : register(u0);
SamplerState s : register(s0);

[numthreads(8, 8, 1)] void CSAcumula(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    float2 v = temMov ? mov.Load(int3(p, 0)).xy : float2(0, 0);
    float c = temMov ? conf.Load(int3(p, 0)) : 1.0;
    float2 uv = (float2(p) + 0.5 + v) / float2(w, h);
    bool dentro = all(uv > 0.0) && all(uv < 1.0);
    float4 a = zera ? float4(0, 0, 1, 1) : (dentro ? ant.SampleLevel(s, uv, 0) : float4(0, 0, 0, 0));
    novo[p] = float4(v + a.xy, min(c, a.z), dentro ? a.w : 0.0);
}
)";

// grade bilateral da edicao de t0 (HDRnet): por bloco de CELULA px e faixa de brilho, a edicao media que a rede deu e a
// cor media de quem a recebeu. Onde a edicao de t0 nao pode ser reposicionada (conteudo animado, oclusao, borda nova),
// ela sai da grade: a mesma regiao, com o mesmo brilho E cor parecida, recebe o que a rede fez ali (um branco nunca
// herda a edicao de um neon do mesmo brilho).
const char* kGrade = R"(
cbuffer C : register(b0) { uint w; uint h; uint gw; uint gh; };
Texture2D<float4> orig : register(t0); // o quadro de t0 (linear)
Texture2D<float4> ed : register(t1);   // a edicao de t0 (espaco comprimido)
RWTexture2D<float4> grade : register(u0);
RWTexture2D<float4> gradeCor : register(u1);
#define CELULA 16
#define FAIXAS 8
float Guia(float3 o) { float l = max(Lum(max(o, 0.0)), 0.0); return pow(l / (1.0 + l), 1.0 / 2.2); }
float3 CorP(float3 o) { o = max(o, 0.0); return pow(o / (1.0 + Lum(o)), 1.0 / 2.2); }
[numthreads(8, 8, 1)] void CSGrade(uint2 g : SV_DispatchThreadID)
{
    if (g.x >= gw || g.y >= gh) return;
    float4 acc[FAIXAS];
    float3 accC[FAIXAS];
    [unroll] for (int b = 0; b < FAIXAS; ++b) acc[b] = 0, accC[b] = 0;
    for (int y = 0; y < CELULA; ++y)
        for (int x = 0; x < CELULA; ++x)
        {
            int2 p = int2(g) * CELULA + int2(x, y);
            if (p.x >= (int)w || p.y >= (int)h) continue;
            float3 o = orig.Load(int3(p, 0)).rgb;
            float3 cp = CorP(o);
            float z = clamp(Guia(o) * FAIXAS - 0.5, 0.0, FAIXAS - 1.0);
            int z0 = (int)floor(z);
            float f = z - z0;
            float3 e = ed.Load(int3(p, 0)).rgb;
            [unroll] for (int b = 0; b < FAIXAS; ++b)
            {
                float wb = b == z0 ? 1.0 - f : (b == z0 + 1 ? f : 0.0);
                acc[b] += float4(e * wb, wb);
                accC[b] += cp * wb;
            }
        }
    [unroll] for (int b = 0; b < FAIXAS; ++b)
    {
        grade[int2(g.x * FAIXAS + b, g.y)] = float4(acc[b].rgb / max(acc[b].a, 1e-6), acc[b].a / (CELULA * CELULA));
        gradeCor[int2(g.x * FAIXAS + b, g.y)] = float4(accC[b] / max(acc[b].a, 1e-6), 0.0);
    }
}
)";

const char* kHlsl = R"(
cbuffer C : register(b0) { uint w; uint h; uint temResp; uint temHist; float maxN; float rejeicao; float folga; uint temMov;
                           uint temDesl; uint temConf; uint temEstat; uint temGrade; uint gw; uint gh; uint temDeslG; uint pad; };
// espaco comprimido (x / (1 + luma), como o TAA dos jogos): o realce em HDR nao domina o acumulo nem o reposicionamento
float3 Comprime(float3 x) { return x / (1.0 + max(Lum(x), 0.0)); }
float3 Expande(float3 c) { return c / max(1.0 - Lum(c), 1e-3); }

Texture2D<float4> fed : register(t0);     // proxy deste quadro
Texture2D<float4> edT0 : register(t1);    // edicao da resposta, no espaco de t0 (o quadro que a rede viu)
Texture2D<float4> mov : register(t2);     // movimento simulado (pixels ate o quadro anterior)
Texture2D<float4> histAnt : register(t3); // edicao acumulada anterior (a = quadros)
Texture2D<float4> fedAnt : register(t4);  // proxy do quadro anterior
Texture2D<float4> desl : register(t5);    // deslocamento acumulado ate t0 (xy), confianca (z), dentro (w)
Texture2D<float> conf : register(t6);     // confianca do fluxo (1 = ida e volta batem)
Texture2D<float4> fedT0 : register(t7);   // proxy de t0
Texture2D<float4> orig : register(t8);    // este quadro no tamanho da rede (linear)
Texture2D<float4> grade : register(t9);   // grade bilateral da edicao da ultima resposta (no espaco de t0 dela)
Texture2D<float4> deslG : register(t10);  // deslocamento acumulado ate o t0 da grade
Texture2D<float4> gradeCor : register(t11); // cor media (perceptual comprimida) de quem recebeu cada celula da grade
ByteAddressBuffer estatFluxo : register(t12); // somas do quadro (Fluxo::Estatisticas): [0] (1-conf)*1024, [3] pixels
RWTexture2D<float4> histNovo : register(u0);
RWTexture2D<float4> edLin : register(u1); // a edicao estabilizada em luz linear (o que a composicao soma)
SamplerState s : register(s0);

// cor perceptual no espaco comprimido: a mesma diferenca relativa pesa igual no escuro e no realce HDR
float3 CorP(float3 o) { o = max(o, 0.0); return pow(o / (1.0 + Lum(o)), 1.0 / 2.2); }
// diferenca de cor (nao so de brilho): um pixel branco nao passa por um neon do mesmo brilho
float DifCor(float3 a, float3 b) { float3 d = abs(CorP(a) - CorP(b)); return max(d.x, max(d.y, d.z)); }

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

#define CELULA 16
#define FAIXAS 8
float Guia(float3 o) { float l = max(Lum(max(o, 0.0)), 0.0); return pow(l / (1.0 + l), 1.0 / 2.2); }
// a edicao que a grade preve para um pixel de brilho z na posicao pos (pixels, espaco de t0 da grade); 0 sem dados
float3 Fatia(float2 pos, float z01, float3 cp)
{
    float2 g = pos / CELULA - 0.5;
    int2 g0 = int2(floor(g));
    float2 f = g - g0;
    float zb = clamp(z01 * FAIXAS - 0.5, 0.0, FAIXAS - 1.0);
    int z0 = (int)floor(zb);
    float fz = zb - z0;
    float3 acc = 0;
    float ws = 0;
    [unroll] for (int j = 0; j < 2; ++j)
        [unroll] for (int i = 0; i < 2; ++i)
            [unroll] for (int k = 0; k < 2; ++k)
            {
                int2 c = clamp(g0 + int2(i, j), int2(0, 0), int2(gw - 1, gh - 1));
                int b = min(z0 + k, FAIXAS - 1);
                float4 t = grade.Load(int3(c.x * FAIXAS + b, c.y, 0));
                float3 dc = abs(gradeCor.Load(int3(c.x * FAIXAS + b, c.y, 0)).rgb - cp);
                float sc = max(dc.x, max(dc.y, dc.z)) / 0.1;
                float wt = (i ? f.x : 1.0 - f.x) * (j ? f.y : 1.0 - f.y) * (k ? fz : 1.0 - fz) * t.a * exp(-sc * sc);
                acc += t.rgb * wt;
                ws += wt;
            }
    // confianca: pelo menos ~2% dos pixels da vizinhanca com esse brilho; menos que isso, a edicao vai a zero
    float conf1 = saturate(ws / 0.02);
    if (conf1 > 0.999)
        return acc / ws;
    // sem cor parecida a 16 px (conteudo animado mudou de cor, borda): vizinhanca de ~80 px, tolerancia de cor maior
    float3 acc2 = 0;
    float ws2 = 0;
    int2 gc = int2(floor(pos / CELULA));
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
            [unroll] for (int k = 0; k < 2; ++k)
            {
                int2 c = clamp(gc + int2(dx, dy), int2(0, 0), int2(gw - 1, gh - 1));
                int b = min(z0 + k, FAIXAS - 1);
                float4 t = grade.Load(int3(c.x * FAIXAS + b, c.y, 0));
                float3 dc = abs(gradeCor.Load(int3(c.x * FAIXAS + b, c.y, 0)).rgb - cp);
                float sc = max(dc.x, max(dc.y, dc.z)) / 0.2;
                float wt = exp(-0.25 * (dx * dx + dy * dy)) * (k ? fz : 1.0 - fz) * t.a * exp(-sc * sc);
                acc2 += t.rgb * wt;
                ws2 += wt;
            }
    float3 e2 = acc2 / max(ws2, 1e-6) * saturate(ws2 / 0.05);
    return lerp(e2, acc / max(ws, 1e-6), conf1);
}

[numthreads(8, 8, 1)] void CSTemporal(uint2 pu : SV_DispatchThreadID)
{
    if (pu.x >= w || pu.y >= h) return;
    int2 p = int2(pu);
    float2 tam = float2(w, h);
    float2 v = temMov ? mov.Load(int3(p, 0)).xy : float2(0, 0);
    float2 uvAnt = (float2(p) + 0.5 + v) / tam;
    bool dentro = all(uvAnt > 0.0) && all(uvAnt < 1.0);
    // corte de cena (troca de camera, menu, tela de carregamento): a confianca ida-e-volta desaba na tela inteira
    // (medido no Cyberpunk: 0,01 no corte; >= 0,29 no giro de camera mais rapido). Como no TAA, o historico e a grade
    // (da cena anterior) saem; a resposta atrasada, que e da cena anterior, tambem. Fica o original ate a rede responder.
    bool corte = false;
    if (temEstat)
    {
        uint np = estatFluxo.Load(12);
        corte = np > 0 && float(estatFluxo.Load(0)) / 1024.0 / float(np) > 0.88;
    }
    float3 hr = 0;
    float n = 0;
    float foto = 1;
    if (temHist && temMov && dentro && !corte)
    {
        hr = CatmullRomT(histAnt, uvAnt, tam);
        n = histAnt.SampleLevel(s, uvAnt, 0).a;
        float d = DifCor(fed.Load(int3(p, 0)).rgb, fedAnt.SampleLevel(s, uvAnt, 0).rgb);
        foto = saturate(1.0 - d * rejeicao);
        n *= foto;
        // ida e volta so pesa onde a cor tambem diverge: onde a cor do quadro anterior reprojetado bate, o historico
        // vale (em area lisa o vetor e arbitrario, nao confere, e zerar ali so trazia cintilacao: 0,129 -> 0,102 medido)
        if (temConf) n *= lerp(conf.Load(int3(p, 0)), 1.0, foto);
    }
    float3 eg = 0;
    if (temGrade && !corte)
    {
        float4 dg = temDeslG ? deslG.Load(int3(p, 0)) : float4(0, 0, 1, 1);
        float3 o = orig.Load(int3(p, 0)).rgb;
        eg = Fatia(float2(p) + 0.5 + dg.xy, Guia(o), CorP(o));
    }
    if (temResp)
    {
        // a resposta chega com atraso: a edicao de t0 vem pelo deslocamento acumulado ate aqui. Onde o caminho sai da
        // imagem, ou o quadro de t0 naquele ponto nao bate com o atual (oclusao, luz, conteudo animado), a edicao
        // reposicionada nao entra: vale a da grade (o que a rede fez ali com esse brilho), nunca um fantasma.
        // Sem atraso (D = 0) a reposicionada e exata e entra inteira, como antes.
        float4 dd = temDesl ? desl.Load(int3(p, 0)) : float4(0, 0, 1, 1);
        float2 q0 = float2(p) + 0.5 + dd.xy;
        float2 uv0 = q0 / tam;
        float peso = 0;
        float3 e = 0;
        if (dd.w > 0.5 && all(uv0 > 0.0) && all(uv0 < 1.0) && !(corte && temDesl))
        {
            e = edT0.SampleLevel(s, uv0, 0).rgb;
            int2 c0 = int2(floor(q0));
            float3 mn = e, mx = e;
            [unroll] for (int j = -1; j <= 1; ++j)
                [unroll] for (int i = -1; i <= 1; ++i)
                {
                    float3 q = edT0.Load(int3(clamp(c0 + int2(i, j), int2(0, 0), int2(w - 1, h - 1)), 0)).rgb;
                    mn = min(mn, q);
                    mx = max(mx, q);
                }
            hr = clamp(hr, mn - folga, mx + folga);
            float d = DifCor(fed.Load(int3(p, 0)).rgb, fedT0.SampleLevel(s, uv0, 0).rgb);
            peso = saturate(1.0 - d * rejeicao);
        }
        if (temGrade)
        {
            // a resposta entra inteira: reposicionada onde o quadro de t0 bate com o atual, pela grade onde nao bate
            e = lerp(eg, e, peso);
            peso = 1.0;
        }
        float nn = min(n + peso, maxN);
        hr = lerp(hr, e, peso / max(nn, 1.0));
        n = nn;
    }
    else
    {
        // o historico so vale onde a cor do quadro anterior reprojetado bate; onde nao bate cede a previsao da grade.
        // (a confianca ida-e-volta nao salva historico de cor diferente: num corte ela dava alta por acaso e a edicao da
        // cena anterior ficava pintada na nova)
        float k = foto;
        hr = temGrade ? lerp(eg, hr, k) : hr * k;
        n = max(n, 1.0);
    }
    histNovo[pu] = float4(hr, n);
    // de volta a luz linear. A edicao veio de outro pixel/quadro (reprojecao, resposta atrasada, grade): somada a um
    // realce mais claro, a luma comprimida passaria de 1 e c / (1 - luma) explodiria (~1000x, pontos brancos). Limite:
    // o brilho editado chega no maximo a 4x o original + 4 (a rede reilumina, nao cria sol); sem efeito no caso exato.
    float3 O = orig.Load(int3(p, 0)).rgb;
    float3 c = Comprime(O) + hr;
    float lo = max(Lum(O), 0.0) * 4.0 + 4.0;
    float lim = lo / (1.0 + lo);
    float lc = Lum(c);
    if (lc > lim) c *= lim / lc;
    edLin[pu] = float4(Expande(c) - O, n);
}
)";

struct K
{
    UINT w, h, temResp, temHist;
    float maxN, rejeicao, folga;
    UINT temMov, temDesl, temConf, temEstat, temGrade, gw, gh, temDeslG, pad;
};
struct KGrade
{
    UINT w, h, gw, gh;
};
const UINT kCelula = 16, kFaixas = 8;
struct KEdicao
{
    UINT w, h, comp;
    float sInt, sCor, sGuarda, exposicao;
    UINT pad;
};
struct KAcumula
{
    UINT w, h, zera, temMov;
};
const D3D12_RESOURCE_STATES SRV =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
} // namespace

void Temporal::Cria(ID3D12Device* d)
{
    passe.Cria(d, (std::string(hlsl::Comum) + kHlsl).c_str(), "CSTemporal", 13, 2, 16, "temporal");
    passeGrade.Cria(d, (std::string(hlsl::Comum) + kGrade).c_str(), "CSGrade", 2, 2, 4, "grade");
    passeEdicao.Cria(d, (std::string(hlsl::Comum) + kEdicao).c_str(), "CSEdicao", 3, 1, 8, "edicao");
    passeAcumula.Cria(d, (std::string(hlsl::Comum) + kAcumula).c_str(), "CSAcumula", 3, 1, 4, "acumula");
}

void Temporal::Garante(ID3D12Device* d, UINT w, UINT h)
{
    if (hist[0].w == w && hist[0].h == h)
        return;
    for (auto& t : hist)
        t.Cria(d, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"historico da edicao");
    for (auto& t : desl)
        t.Cria(d, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"deslocamento acumulado");
    crua.Cria(d, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"edicao crua");
    linear.Cria(d, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"edicao estabilizada (linear)");
    for (auto& t : deslG)
        t.Cria(d, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"deslocamento ate a grade");
    gw = (w + kCelula - 1) / kCelula;
    gh = (h + kCelula - 1) / kCelula;
    grade.Cria(d, gw * kFaixas, gh, DXGI_FORMAT_R16G16B16A16_FLOAT, L"grade bilateral da edicao");
    gradeCor.Cria(d, gw * kFaixas, gh, DXGI_FORMAT_R16G16B16A16_FLOAT, L"cor da grade bilateral");
    gradeValida = false;
    valido = false;
}

void Temporal::Edicao(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex& origT0, Tex& fedT0,
                      Tex& resp, const ParamsTemporal& p)
{
    origT0.Para(c, SRV);
    fedT0.Para(c, SRV);
    resp.Para(c, SRV);
    crua.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    KEdicao k{crua.w,
              crua.h,
              (UINT)std::clamp(p.composicao, 0, 1),
              std::clamp(p.intensidade, 0.f, 2.f),
              std::clamp(p.cor, 0.f, 1.f),
              std::clamp(p.guarda, 1.f, 16.f),
              std::max(p.exposicao, 1e-3f),
              0};
    passeEdicao.Roda(c, desc, d, {{fedT0.r.Get(), fedT0.fmt}, {resp.r.Get(), resp.fmt}, {origT0.r.Get(), origT0.fmt}},
                     {{crua.r.Get(), crua.fmt}}, &k, (crua.w + 7) / 8, (crua.h + 7) / 8);
    crua.Para(c, SRV);
}

Tex* Temporal::AcumulaEm(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex (&par)[2], UINT& qual,
                         Tex* mov, Tex* confianca, bool zera)
{
    Tex& ant = par[qual];
    Tex& novo = par[qual ^ 1];
    ant.Para(c, SRV);
    novo.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    KAcumula k{novo.w, novo.h, zera ? 1u : 0u, mov ? 1u : 0u};
    using Par = std::pair<ID3D12Resource*, DXGI_FORMAT>;
    passeAcumula.Roda(c, desc, d,
                      {mov ? Par{mov->r.Get(), mov->fmt} : Par{ant.r.Get(), ant.fmt},
                       (mov && confianca) ? Par{confianca->r.Get(), confianca->fmt} : Par{ant.r.Get(), ant.fmt},
                       {ant.r.Get(), ant.fmt}},
                      {{novo.r.Get(), novo.fmt}}, &k, (novo.w + 7) / 8, (novo.h + 7) / 8);
    novo.Para(c, SRV);
    qual ^= 1;
    return &novo;
}

Tex* Temporal::Acumula(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex* mov, Tex* confianca,
                       bool zera)
{
    return AcumulaEm(c, desc, d, desl, deslAtual, mov, confianca, zera);
}

Tex* Temporal::AcumulaGrade(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex* mov, Tex* confianca)
{
    deslGAgora = gradeValida ? AcumulaEm(c, desc, d, deslG, deslGAtual, mov, confianca, zeraG) : nullptr;
    zeraG = false;
    return deslGAgora;
}

void Temporal::Grade(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex& origT0, Tex* deslocamento)
{
    origT0.Para(c, SRV);
    crua.Para(c, SRV);
    grade.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gradeCor.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    KGrade k{crua.w, crua.h, gw, gh};
    passeGrade.Roda(c, desc, d, {{origT0.r.Get(), origT0.fmt}, {crua.r.Get(), crua.fmt}},
                    {{grade.r.Get(), grade.fmt}, {gradeCor.r.Get(), gradeCor.fmt}}, &k, (gw + 7) / 8, (gh + 7) / 8);
    grade.Para(c, SRV);
    gradeCor.Para(c, SRV);
    gradeValida = true;
    if (deslocamento)
    {
        // o t0 da grade e o da resposta: o deslocamento ate ela, daqui em diante, parte do da resposta
        Tex& g = deslG[deslGAtual];
        deslocamento->Para(c, D3D12_RESOURCE_STATE_COPY_SOURCE);
        g.Para(c, D3D12_RESOURCE_STATE_COPY_DEST);
        c->CopyResource(g.r.Get(), deslocamento->r.Get());
        g.Para(c, SRV);
        deslocamento->Para(c, SRV);
        deslGAgora = &g;
        zeraG = false;
    }
    else
    {
        deslGAgora = nullptr; // t0 e este quadro
        zeraG = true;
    }
}

Tex* Temporal::Roda(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex& orig, Tex& fed, Tex& fedAnt,
                    bool chegou, Tex* fedT0, Tex* deslocamento, Tex* mov, Tex* confianca, ID3D12Resource* estatFluxo,
                    const ParamsTemporal& p)
{
    if (!chegou && !valido)
        return nullptr; // sem rede e sem historico: nada a mostrar
    Tex& ant = hist[atual];
    Tex& novo = hist[atual ^ 1];
    ant.Para(c, SRV);
    fedAnt.Para(c, SRV);
    novo.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    crua.Para(c, SRV);
    orig.Para(c, SRV);
    linear.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (fedT0)
        fedT0->Para(c, SRV);
    if (gradeValida)
        grade.Para(c, SRV), gradeCor.Para(c, SRV);
    if (deslGAgora)
        deslGAgora->Para(c, SRV);
    K k{fed.w,
        fed.h,
        chegou ? 1u : 0u,
        valido ? 1u : 0u,
        float(std::max(1, p.maxQuadros)),
        p.rejeicao,
        p.folga,
        mov ? 1u : 0u,
        deslocamento ? 1u : 0u,
        confianca ? 1u : 0u,
        (mov && estatFluxo) ? 1u : 0u,
        gradeValida ? 1u : 0u,
        gw,
        gh,
        deslGAgora ? 1u : 0u,
        0};
    using Par = std::pair<ID3D12Resource*, DXGI_FORMAT>;
    const Par vazio{fed.r.Get(), fed.fmt};
    passe.Roda(c, desc, d,
               {vazio,
                {crua.r.Get(), crua.fmt},
                mov ? Par{mov->r.Get(), mov->fmt} : vazio,
                {ant.r.Get(), ant.fmt},
                {fedAnt.r.Get(), fedAnt.fmt},
                deslocamento ? Par{deslocamento->r.Get(), deslocamento->fmt} : vazio,
                confianca ? Par{confianca->r.Get(), confianca->fmt} : vazio,
                fedT0 ? Par{fedT0->r.Get(), fedT0->fmt} : vazio,
                {orig.r.Get(), orig.fmt},
                gradeValida ? Par{grade.r.Get(), grade.fmt} : vazio,
                deslGAgora ? Par{deslGAgora->r.Get(), deslGAgora->fmt} : vazio,
                gradeValida ? Par{gradeCor.r.Get(), gradeCor.fmt} : vazio,
                {estatFluxo, DXGI_FORMAT_UNKNOWN}},
               {{novo.r.Get(), novo.fmt}, {linear.r.Get(), linear.fmt}}, &k, (fed.w + 7) / 8, (fed.h + 7) / 8);
    novo.Para(c, SRV);
    linear.Para(c, SRV);
    atual ^= 1;
    valido = true;
    return &linear;
}

} // namespace ov
