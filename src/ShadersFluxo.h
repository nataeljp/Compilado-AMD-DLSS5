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

// busca: v tal que Lanterior(p + v) ~ Latual(p); custo = soma |diferenca| numa janela (9x9 passo 2 nos niveis grossos,
// 5x5 passo 2 no fino) + lambda*|v - previsto|. Candidatos: a previsao do nivel de baixo, parado (HUD) e o movimento
// global (modelo afim robusto do nivel de 1/8): em textura repetitiva (tabuas, grades) a busca local casa com o periodo
// vizinho; o candidato global, que casa igual, desempata.
inline const char* Busca = R"(
cbuffer C : register(b0) { uint lw; uint lh; uint gw; uint gh; int raio; uint temGrosso; float lambda; uint janela;
                           uint usaGlobal; float escalaGlobal; float meiaGlobal; uint pad2; };
SamplerState s : register(s0);
Texture2D<float> La : register(t0);   // atual
Texture2D<float> Lp : register(t1);   // anterior
Texture2D<float2> grosso : register(t2);
Texture2D<float4> modelo : register(t3); // [0] = (a, b, e, -), [1] = (c, d, f, valido): v = (a x + b y + e, c x + d y + f)
RWTexture2D<float2> saida : register(u0);

float Custo(int2 p, float2 v)
{
    float c = 0;
    // janela 0: 5x5 passo 2; 1: 9x9 passo 2 (mais contexto contra textura repetitiva; medido: grosseiros 21% -> 15%)
    int r = janela == 0 ? 2 : 4, st = 2;
    [loop] for (int j = -r; j <= r; j += st)
        [loop] for (int i = -r; i <= r; i += st)
        {
            int2 q = clamp(p + int2(i, j), int2(0, 0), int2(lw - 1, lh - 1));
            float a = La.Load(int3(q, 0));
            float b = Lp.SampleLevel(s, (float2(q) + 0.5 + v) / float2(lw, lh), 0);
            c += abs(a - b);
        }
    return c / float((2 * r / st + 1) * (2 * r / st + 1)) * 9.0;
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
    // candidato global: ganha o empate (3%) contra a previsao local e vira o centro da busca
    if (usaGlobal)
    {
        float4 m0 = modelo.Load(int3(0, 0, 0)), m1 = modelo.Load(int3(1, 0, 0));
        if (m1.w > 0.5)
        {
            float2 xn = (float2(p) + 0.5 - 0.5 * float2(lw, lh)) / (escalaGlobal * meiaGlobal);
            float2 vg = escalaGlobal * float2(dot(m0.xyz, float3(xn, 1.0)), dot(m1.xyz, float3(xn, 1.0)));
            float cg = Custo(p, vg);
            if (cg <= cm * 1.03 + 1e-4) { cm = cg; melhor = vg; v0 = vg; }
        }
    }
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

// movimento global: modelo afim ajustado aos vetores de um nivel grosso por minimos quadrados reponderados (Tukey),
// num grupo so: o HUD e os objetos com movimento proprio viram descarte. Coordenadas normalizadas pela meia largura.
inline const char* Global = R"(
cbuffer C : register(b0) { uint lw; uint lh; uint gw; uint gh; int raio; uint temGrosso; float lambda; uint pad; };
Texture2D<float2> campo : register(t0);
RWTexture2D<float4> modelo : register(u0);
groupshared float acc[256][12];
groupshared float3 solX, solY;
groupshared float valido;
[numthreads(256, 1, 1)] void CSGlobal(uint gi : SV_GroupIndex)
{
    float3 mx = 0, my = 0;
    const float meia = 0.5 * float(lw);
    const uint n = lw * lh;
    [unroll] for (int it = 0; it < 6; ++it) // desenrolado: barreira de grupo fora de laco dinamico (FXC X3663)
    {
        float a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, b0 = 0, b1 = 0, b2 = 0, c0 = 0, c1 = 0, c2 = 0;
        for (uint i = gi; i < n; i += 256)
        {
            uint2 p = uint2(i % lw, i / lw);
            float2 v = campo.Load(int3(p, 0));
            float2 x = (float2(p) + 0.5 - 0.5 * float2(lw, lh)) / meia;
            float wgt = 1.0;
            if (it > 0)
            {
                float2 m = float2(dot(mx, float3(x, 1.0)), dot(my, float3(x, 1.0)));
                float r = length(v - m) / (it < 3 ? 4.0 : 2.0);
                wgt = r < 1.0 ? (1.0 - r * r) * (1.0 - r * r) : 0.0;
            }
            a0 += wgt * x.x * x.x; a1 += wgt * x.x * x.y; a2 += wgt * x.x; a3 += wgt * x.y * x.y; a4 += wgt * x.y; a5 += wgt;
            b0 += wgt * x.x * v.x; b1 += wgt * x.y * v.x; b2 += wgt * v.x;
            c0 += wgt * x.x * v.y; c1 += wgt * x.y * v.y; c2 += wgt * v.y;
        }
        acc[gi][0] = a0; acc[gi][1] = a1; acc[gi][2] = a2; acc[gi][3] = a3; acc[gi][4] = a4; acc[gi][5] = a5;
        acc[gi][6] = b0; acc[gi][7] = b1; acc[gi][8] = b2; acc[gi][9] = c0; acc[gi][10] = c1; acc[gi][11] = c2;
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint st = 128; st > 0; st >>= 1)
        {
            if (gi < st)
                [unroll] for (int k = 0; k < 12; ++k) acc[gi][k] += acc[gi + st][k];
            GroupMemoryBarrierWithGroupSync();
        }
        if (gi == 0)
        {
            float3x3 M = float3x3(acc[0][0], acc[0][1], acc[0][2], acc[0][1], acc[0][3], acc[0][4], acc[0][2], acc[0][4], acc[0][5]);
            float det = determinant(M);
            // pelo menos 20% dos vetores concordando
            valido = (abs(det) > 1e-9 && acc[0][5] > 0.2 * float(n)) ? 1.0 : 0.0;
            if (valido > 0.5)
            {
                float3x3 C = float3x3(M[1][1] * M[2][2] - M[1][2] * M[2][1], M[0][2] * M[2][1] - M[0][1] * M[2][2], M[0][1] * M[1][2] - M[0][2] * M[1][1],
                                      M[1][2] * M[2][0] - M[1][0] * M[2][2], M[0][0] * M[2][2] - M[0][2] * M[2][0], M[0][2] * M[1][0] - M[0][0] * M[1][2],
                                      M[1][0] * M[2][1] - M[1][1] * M[2][0], M[0][1] * M[2][0] - M[0][0] * M[2][1], M[0][0] * M[1][1] - M[0][1] * M[1][0]);
                C /= det;
                solX = mul(C, float3(acc[0][6], acc[0][7], acc[0][8]));
                solY = mul(C, float3(acc[0][9], acc[0][10], acc[0][11]));
            }
            else { solX = 0; solY = 0; }
        }
        GroupMemoryBarrierWithGroupSync();
        mx = solX; my = solY;
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0)
    {
        modelo[uint2(0, 0)] = float4(mx, 0.0);
        modelo[uint2(1, 0)] = float4(my, valido);
    }
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

// gradiente (diferencas centrais) da luminancia em resolucao cheia: o "modelo" do refinamento de Lucas-Kanade
inline const char* Gradiente = R"(
cbuffer C : register(b0) { uint lw; uint lh; uint gw; uint gh; int raio; uint temGrosso; float lambda; uint pad; };
Texture2D<float> L : register(t0);
RWTexture2D<float2> grad : register(u0);
[numthreads(8, 8, 1)] void CSGradiente(uint2 pu : SV_DispatchThreadID)
{
    if (pu.x >= lw || pu.y >= lh) return;
    int2 p = int2(pu), m = int2(lw - 1, lh - 1);
    float gx = L.Load(int3(clamp(p + int2(1, 0), 0, m), 0)) - L.Load(int3(clamp(p - int2(1, 0), 0, m), 0));
    float gy = L.Load(int3(clamp(p + int2(0, 1), 0, m), 0)) - L.Load(int3(clamp(p - int2(0, 1), 0, m), 0));
    grad[pu] = 0.5 * float2(gx, gy);
}
)";

// Refinamento sub-pixel de Lucas-Kanade em resolucao cheia (o nucleo do DIS de Kroeger et al. 2016 e do KLT, os
// rastreadores de celular): parte do vetor da busca em meia resolucao (quantizado a ~0,5 px) e faz 3 passos de
// Gauss-Newton numa janela 5x5 gaussiana, com o residuo centrado (compensa mudanca de brilho local). So aceita onde
// ha textura nas duas direcoes e o casamento melhora; senao fica o vetor da busca.
inline const char* Refina = R"(
cbuffer C : register(b0) { uint lw; uint lh; uint gw; uint gh; int raio; uint temGrosso; float lambda; uint pad; };
SamplerState s : register(s0);
Texture2D<float> Lt : register(t0);   // modelo: o quadro de onde o vetor sai
Texture2D<float2> Gt : register(t1);  // gradiente do modelo
Texture2D<float> La : register(t2);   // alvo: o quadro para onde o vetor aponta
Texture2D<float2> v0 : register(t3);  // vetor da busca
RWTexture2D<float2> saida : register(u0);
static const float SIG2 = 2.0 * 1.5 * 1.5;

[numthreads(8, 8, 1)] void CSRefina(uint2 pu : SV_DispatchThreadID)
{
    if (pu.x >= lw || pu.y >= lh) return;
    int2 p = int2(pu), m = int2(lw - 1, lh - 1);
    float2 tam = float2(lw, lh);
    float2 vi = v0.Load(int3(p, 0));
    float t[25];
    float2 g[25];
    float3 G = 0;
    float2 sg = 0;
    float sw = 0;
    int k = 0;
    [unroll] for (int j = -2; j <= 2; ++j)
        [unroll] for (int i = -2; i <= 2; ++i)
        {
            int2 q = clamp(p + int2(i, j), 0, m);
            float w = exp(-(i * i + j * j) / SIG2);
            t[k] = Lt.Load(int3(q, 0));
            g[k] = Gt.Load(int3(q, 0));
            G += w * float3(g[k].x * g[k].x, g[k].x * g[k].y, g[k].y * g[k].y);
            sg += w * g[k];
            sw += w;
            ++k;
        }
    G -= float3(sg.x * sg.x, sg.x * sg.y, sg.y * sg.y) / sw;
    float det = G.x * G.z - G.y * G.y;
    float tr = G.x + G.z;
    float menor = 0.5 * (tr - sqrt(max(tr * tr - 4.0 * det, 0.0)));
    if (menor < 2e-5) { saida[pu] = vi; return; }
    float2 v = vi;
    float custoIni = 0;
    [unroll] for (int it = 0; it <= 3; ++it)
    {
        float2 b = 0;
        float sr = 0, srr = 0;
        k = 0;
        [unroll] for (int j2 = -2; j2 <= 2; ++j2)
            [unroll] for (int i2 = -2; i2 <= 2; ++i2)
            {
                int2 q = clamp(p + int2(i2, j2), 0, m);
                float w = exp(-(i2 * i2 + j2 * j2) / SIG2);
                float r = La.SampleLevel(s, (float2(q) + 0.5 + v) / tam, 0) - t[k];
                b += w * r * g[k];
                sr += w * r;
                srr += w * r * r;
                ++k;
            }
        if (it == 0) custoIni = srr - sr * sr / sw;
        if (it == 3) { if (srr - sr * sr / sw > custoIni || length(v - vi) > 2.0) v = vi; break; }
        b -= sg * sr / sw;
        float2 dv = -float2(G.z * b.x - G.y * b.y, G.x * b.y - G.y * b.x) / det;
        v += clamp(dv, -1.0, 1.0);
    }
    saida[pu] = v;
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

// Captura de buffer: movimento e profundidade do proprio jogo (o que ele entrega ao upscaler) no lugar do fluxo optico.
// Movimento: o do vizinho mais perto da camera (3x3), como no TAA -- a borda do objeto da frente leva o movimento dele.
// Cada ponto deste quadro tambem espalha a distancia dele para onde estava no quadro anterior (recon, a mais perto
// vence): a confianca (ConfJogo) compara so distancias DESTE quadro, como a profundidade reconstruida do FSR.
inline const char* DoJogo = R"(
cbuffer K : register(b0)
{
    uint w; uint h; uint movW; uint movH;
    uint profW; uint profH; uint temAnt; uint invertida;
    float corW; float corH; float perto; float longe;
    uint infinita; float tanY; float aspecto; uint pad2;
};
Texture2D<float2> movJogo : register(t0); // pixels da cor, do atual ao anterior
Texture2D<float> profJogo : register(t1);
RWTexture2D<float2> mov : register(u0);   // pixels da rede
RWTexture2D<float> zAtual : register(u1);
RWByteAddressBuffer contagem : register(u2); // [0] pixels com profundidade < 0,5, [4] pixels (amostra 1 em 16x16)
RWTexture2D<uint> recon : register(u3);   // por pixel do quadro anterior: a menor distancia deste quadro que cai nele

static const float CEU = 1e6;
// profundidade do jogo -> distancia da camera (linear com perto/longe do FSR; sem camera, proporcional a ela)
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
// Z da camera -> distancia RADIAL (do olho ao ponto) com o FOV do jogo: o Z de um ponto fora do centro muda quando a
// camera gira (Z = distancia x cosseno do angulo); girando rapido, ~70% da tela perdia a confianca (Gears). A distancia
// radial nao muda num giro. Sem FOV (0), fica o Z.
float Radial(float z, float2 uv)
{
    if (tanY <= 0.0 || z >= CEU * 0.5) return z;
    const float2 n = (uv * 2.0 - 1.0) * float2(tanY * aspecto, tanY);
    return z * sqrt(1.0 + dot(n, n));
}

[numthreads(8, 8, 1)] void CSDoJogo(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    const float2 uv = (p + 0.5) / float2(w, h);
    const int2 lim = int2(profW, profH) - 1;
    const int2 pp = clamp(int2(uv * float2(profW, profH)), 0, lim);
    float zMin = CEU * 2.0;
    int2 perto3 = pp;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
        [unroll] for (int dx = -1; dx <= 1; ++dx)
        {
            const int2 q = clamp(pp + int2(dx, dy), 0, lim);
            const float z = Dist(profJogo.Load(int3(q, 0)));
            if (z < zMin) { zMin = z; perto3 = q; }
        }
    const float dc = profJogo.Load(int3(pp, 0));
    if ((p.x & 15) == 0 && (p.y & 15) == 0 && isfinite(dc))
    {
        contagem.InterlockedAdd(0, dc < 0.5 ? 1u : 0u);
        contagem.InterlockedAdd(4, 1u);
    }
    const float zc = Radial(Dist(dc), uv);
    const float2 uvM = (perto3 + 0.5) / float2(profW, profH);
    const int2 pm = clamp(int2(uvM * float2(movW, movH)), 0, int2(movW, movH) - 1);
    float2 m = movJogo.Load(int3(pm, 0)) * float2(float(w) / corW, float(h) / corH);
    m = all(isfinite(m)) ? m : 0;
    mov[p] = m;
    zAtual[p] = zc;
    // os 4 pixels do quadro anterior em volta de onde este ponto estava (distancia positiva: a ordem de asuint e a do float)
    const int2 b = int2(floor(p + m));
    [unroll] for (int j = 0; j < 2; ++j)
        [unroll] for (int i = 0; i < 2; ++i)
        {
            const int2 q = b + int2(i, j);
            if (all(q >= 0) && all(q < int2(w, h)))
                InterlockedMin(recon[q], asuint(zc));
        }
}
)";

// zera a profundidade reconstruida (nenhum ponto deste quadro caiu ali = nada na frente)
inline const char* LimpaRecon = R"(
cbuffer K : register(b0) { uint w; uint h; };
RWTexture2D<uint> recon : register(u0);
[numthreads(8, 8, 1)] void CSLimpaRecon(uint2 p : SV_DispatchThreadID)
{
    if (p.x < w && p.y < h) recon[p] = 0xFFFFFFFFu;
}
)";

// Confianca da captura de buffer: o historico deste pixel (o que estava em p + mov no quadro anterior) ainda e desta
// superficie? Antes comparava a distancia deste quadro com a do anterior: com a camera andando em volta do personagem
// (Gears, 2-3 m), a distancia dele muda 8-10% por quadro -- a confianca caia no corpo todo, o historico saia e a edicao
// dele virava estimativa atrasada (o rastro). Agora, como no FSR: ha um ponto DESTE quadro, mais perto da camera, que
// estava naquele lugar do quadro anterior? Entao este fundo acabou de aparecer. As duas distancias sao deste quadro;
// a camera andando nao entra. Contra o quadro anterior fica so diferenca grande (o oclusor que saiu de vista).
inline const char* ConfJogo = R"(
cbuffer K : register(b0) { uint w; uint h; uint temAnt; uint pad0; };
Texture2D<float2> mov : register(t0);   // pixels da rede, do atual ao anterior
Texture2D<float> zAtual : register(t1); // distancia radial deste quadro
Texture2D<float> zAnt : register(t2);   // a do quadro anterior
Texture2D<uint> recon : register(t3);   // DoJogo: a menor distancia deste quadro que cai em cada pixel do anterior
RWTexture2D<float> conf : register(u0);
SamplerState s : register(s0);
static const float CEU = 1e6;

[numthreads(8, 8, 1)] void CSConfJogo(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    const float2 m = mov.Load(int3(p, 0));
    const float zc = zAtual.Load(int3(p, 0));
    const float2 pa = p + 0.5 + m;
    float c = 0.0;
    if (temAnt && all(pa >= 0.0) && all(pa <= float2(w, h)))
    {
        // 1) oclusao so com distancias deste quadro: o mais perto que caiu nos mesmos 4 pixels (este ponto tambem caiu)
        const int2 b = int2(floor(pa - 0.5));
        float zmin = CEU * 4.0;
        [unroll] for (int j = 0; j < 2; ++j)
            [unroll] for (int i = 0; i < 2; ++i)
            {
                const uint v = recon.Load(int3(clamp(b + int2(i, j), 0, int2(w, h) - 1), 0));
                if (v != 0xFFFFFFFFu) zmin = min(zmin, asfloat(v));
            }
        // folga da inclinacao: num chao rasante, o vizinho de 1 pixel ja esta alguns % mais perto (mesma superficie)
        float g = 0.0;
        [unroll] for (int k = 0; k < 4; ++k)
        {
            const int2 o = int2(k == 0 ? 1 : k == 1 ? -1 : 0, k == 2 ? 1 : k == 3 ? -1 : 0);
            const float zv = zAtual.Load(int3(clamp(int2(p) + o, 0, int2(w, h) - 1), 0));
            if (zv < CEU * 0.5 && zc < CEU * 0.5) g = max(g, abs(zv - zc) / max(zc, 1e-6));
        }
        float oc = 0.0;
        if (zmin < CEU * 2.0)
            oc = zc >= CEU * 0.5 ? (zmin < CEU * 0.5 ? 1e9 : 0.0) : max(zc - zmin, 0.0) / max(zmin, 1e-6);
        c = 1.0 - smoothstep(0.05 + 2.0 * g, 0.15 + 2.0 * g, oc);
        // 2) contra o quadro anterior, so o que a camera andando nao explica: antes mais perto (oclusao) a partir de
        //    15-35%; antes mais longe (a camera chegou perto, ou o objeto) so a partir de 40-80%
        const float4 za = zAnt.GatherRed(s, pa / float2(w, h));
        float pior = 1.0;
        [unroll] for (int i = 0; i < 4; ++i)
        {
            const float rn = max(zc - za[i], 0.0) / max(za[i], 1e-6);
            const float rf = max(za[i] - zc, 0.0) / max(zc, 1e-6);
            const bool ceu = zc >= CEU * 0.5 && za[i] >= CEU * 0.5;
            pior = min(pior, ceu ? 0.0 : max(smoothstep(0.15, 0.35, rn), smoothstep(0.4, 0.8, rf)));
        }
        c *= 1.0 - pior;
    }
    conf[p] = c;
}
)";

} // namespace ov::hlsl
