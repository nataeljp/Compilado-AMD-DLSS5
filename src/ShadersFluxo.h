// HLSL do fluxo optico (cs_5_1)
#pragma once

namespace ov::hlsl {

inline const char* Fluxo = R"(
cbuffer C : register(b0) { uint lw; uint lh; uint gw; uint gh; int raio; uint temGrosso; float lambda; uint pad; };
SamplerState s : register(s0);

// ---- luminancia perceptual do quadro (nivel 0 = metade do tamanho da rede, media 2x2) ----
Texture2D<float4> cor : register(t0);
RWTexture2D<float> lumaOut : register(u0);
float Luma(float3 c) { return pow(max(dot(c, float3(0.2126, 0.7152, 0.0722)), 0.0), 1.0 / 2.2); }
[numthreads(8, 8, 1)] void CSLuma(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= lw || p.y >= lh) return;
    // gw x gh = tamanho do quadro de origem
    float2 uv = (float2(p) + 0.5) / float2(lw, lh);
    lumaOut[p] = Luma(cor.SampleLevel(s, uv, 0).rgb);
}
)";

// ---- nivel seguinte da piramide (media 2x2 por amostra bilinear no centro) ----
inline const char* ReduzLuma = R"(
cbuffer C : register(b0) { uint lw; uint lh; uint gw; uint gh; int raio; uint temGrosso; float lambda; uint pad; };
SamplerState s : register(s0);
Texture2D<float> lumaIn : register(t0);
RWTexture2D<float> lumaOut : register(u0);
[numthreads(8, 8, 1)] void CSReduzLuma(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= lw || p.y >= lh) return;
    float2 uv = (float2(p) + 0.5) / float2(lw, lh);
    lumaOut[p] = lumaIn.SampleLevel(s, uv, 0);
}
)";

// busca: v tal que Lanterior(p + v) ~ Latual(p); custo = soma |diferenca| numa janela 5x5 (passo 2) + lambda*|v - previsto|
inline const char* Busca = R"(
cbuffer C : register(b0) { uint lw; uint lh; uint gw; uint gh; int raio; uint temGrosso; float lambda; uint pad; };
SamplerState s : register(s0);
Texture2D<float> La : register(t0);   // atual
Texture2D<float> Lp : register(t1);   // anterior
Texture2D<float2> grosso : register(t2);
RWTexture2D<float2> saida : register(u0);

float Custo(int2 p, float2 v)
{
    float c = 0;
    [unroll] for (int j = -2; j <= 2; j += 2)
        [unroll] for (int i = -2; i <= 2; i += 2)
        {
            int2 q = clamp(p + int2(i, j), int2(0, 0), int2(lw - 1, lh - 1));
            float a = La.Load(int3(q, 0));
            float b = Lp.SampleLevel(s, (float2(q) + 0.5 + v) / float2(lw, lh), 0);
            c += abs(a - b);
        }
    return c;
}

[numthreads(8, 8, 1)] void CSBusca(uint2 pu : SV_DispatchThreadID)
{
    if (pu.x >= lw || pu.y >= lh) return;
    int2 p = int2(pu);
    float2 v0 = 0;
    if (temGrosso)
        v0 = 2.0 * grosso.SampleLevel(s, (float2(p) + 0.5) / float2(lw, lh), 0);
    float2 melhor = v0;
    float cm = Custo(p, v0);
    // candidato parado (HUD, fundo fixo)
    float c0 = Custo(p, 0) + lambda * length(v0);
    if (c0 < cm) { cm = c0; melhor = 0; }
    [loop] for (int dy = -raio; dy <= raio; ++dy)
        [loop] for (int dx = -raio; dx <= raio; ++dx)
        {
            if (dx == 0 && dy == 0) continue;
            float2 v = v0 + float2(dx, dy);
            float c = Custo(p, v) + lambda * length(v - v0);
            if (c < cm) { cm = c; melhor = v; }
        }
    // sub-pixel: passos de 1/2 e 1/4 em volta do melhor
    float passo = 0.5;
    [unroll] for (int k = 0; k < 2; ++k)
    {
        float2 base = melhor;
        [unroll] for (int j = -1; j <= 1; ++j)
            [unroll] for (int i = -1; i <= 1; ++i)
            {
                if (i == 0 && j == 0) continue;
                float2 v = base + float2(i, j) * passo;
                float c = Custo(p, v) + lambda * length(v - v0);
                if (c < cm) { cm = c; melhor = v; }
            }
        passo *= 0.5;
    }
    saida[pu] = melhor;
}
)";

// mediana vetorial 3x3 (o vetor que minimiza a soma das distancias aos outros 8): tira os erros isolados
inline const char* Mediana = R"(
cbuffer C : register(b0) { uint lw; uint lh; uint gw; uint gh; int raio; uint temGrosso; float lambda; uint pad; };
Texture2D<float2> ent : register(t0);
RWTexture2D<float2> sai : register(u0);
[numthreads(8, 8, 1)] void CSMediana(uint2 pu : SV_DispatchThreadID)
{
    if (pu.x >= lw || pu.y >= lh) return;
    int2 p = int2(pu);
    float2 v[9];
    int n = 0;
    [unroll] for (int j = -1; j <= 1; ++j)
        [unroll] for (int i = -1; i <= 1; ++i)
            v[n++] = ent.Load(int3(clamp(p + int2(i, j), int2(0, 0), int2(lw - 1, lh - 1)), 0));
    float melhor = 1e30;
    float2 r = v[4];
    [unroll] for (int a = 0; a < 9; ++a)
    {
        float d = 0;
        [unroll] for (int b = 0; b < 9; ++b) d += length(v[a] - v[b]);
        if (d < melhor) { melhor = d; r = v[a]; }
    }
    sai[pu] = r;
}
)";

// do nivel 0 (metade) para o tamanho da rede, em pixels da rede
inline const char* Amplia = R"(
cbuffer C : register(b0) { uint lw; uint lh; uint gw; uint gh; int raio; uint temGrosso; float lambda; uint pad; };
SamplerState s : register(s0);
Texture2D<float2> campo : register(t0);
RWTexture2D<float2> mov : register(u0);
[numthreads(8, 8, 1)] void CSAmplia(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= lw || p.y >= lh) return;
    float2 uv = (float2(p) + 0.5) / float2(lw, lh);
    mov[p] = campo.SampleLevel(s, uv, 0) * float2(float(lw) / gw, float(lh) / gh);
}
)";

// confianca do movimento: o vetor de ida (atual -> anterior) somado ao de volta no ponto de chegada deve dar ~0.
// Ate 0,5 px de erro: 1; a partir de 2 px: 0 (oclusao, entrada/saida de quadro, transparencia, casamento errado)
inline const char* Confianca = R"(
cbuffer C : register(b0) { uint lw; uint lh; uint gw; uint gh; int raio; uint temGrosso; float lambda; uint pad; };
SamplerState s : register(s0);
Texture2D<float2> ida : register(t0);
Texture2D<float2> volta : register(t1);
RWTexture2D<float> conf : register(u0);
[numthreads(8, 8, 1)] void CSConfianca(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= lw || p.y >= lh) return;
    float2 v = ida.Load(int3(p, 0));
    float2 q = float2(p) + 0.5 + v;
    float2 tam = float2(lw, lh);
    if (any(q < 0.0) || any(q > tam)) { conf[p] = 0.0; return; }
    float2 b = volta.SampleLevel(s, q / tam, 0);
    float erro = length(v + b);
    // tolerancia cresce um pouco com a velocidade (o sub-pixel erra mais em movimento rapido)
    float tol = 0.5 + 0.05 * length(v);
    conf[p] = saturate(1.0 - (erro - tol) / 1.5);
}
)";

// estatisticas do quadro para o ritmo automatico: somas (ponto fixo) de (1 - confianca), velocidade e mudanca de luz
// depois de compensar o movimento. Grupo 16x16 soma em memoria compartilhada; um atomico por grupo.
inline const char* Estatisticas = R"(
cbuffer C : register(b0) { uint lw; uint lh; uint gw; uint gh; int raio; uint temGrosso; float lambda; uint pad; };
SamplerState s : register(s0);
Texture2D<float2> mov : register(t0);
Texture2D<float> conf : register(t1);
Texture2D<float4> fed : register(t2);
Texture2D<float4> fedAnt : register(t3);
RWByteAddressBuffer soma : register(u0); // [0] (1-conf)*1024, [1] |v|*4, [2] |dL|*1024, [3] pixels
groupshared uint g0, g1, g2, g3;
float LumaP(float3 c) { return pow(max(dot(c, float3(0.2126, 0.7152, 0.0722)), 0.0), 1.0 / 2.2); }
[numthreads(16, 16, 1)] void CSEstatisticas(uint2 p : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    if (gi == 0) { g0 = 0; g1 = 0; g2 = 0; g3 = 0; }
    GroupMemoryBarrierWithGroupSync();
    // amostra 1 de cada 2x2 (barato e suficiente para medias)
    uint2 q = p * 2;
    if (q.x < lw && q.y < lh)
    {
        float2 v = mov.Load(int3(q, 0));
        float c = conf.Load(int3(q, 0));
        float2 uv = (float2(q) + 0.5 + v) / float2(lw, lh);
        float dl = abs(LumaP(fed.Load(int3(q, 0)).rgb) - LumaP(fedAnt.SampleLevel(s, uv, 0).rgb));
        InterlockedAdd(g0, (uint)round(saturate(1.0 - c) * 1024.0));
        InterlockedAdd(g1, (uint)round(min(length(v), 255.0) * 4.0));
        InterlockedAdd(g2, (uint)round(saturate(dl) * 1024.0));
        InterlockedAdd(g3, 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0)
    {
        soma.InterlockedAdd(0, g0);
        soma.InterlockedAdd(4, g1);
        soma.InterlockedAdd(8, g2);
        soma.InterlockedAdd(12, g3);
    }
}
)";

} // namespace ov::hlsl
