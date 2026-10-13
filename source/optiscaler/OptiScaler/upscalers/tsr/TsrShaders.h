#pragma once
// [lmxxf2] TSR proprio: upscaler temporal escrito do zero seguindo as etapas que a Epic descreve publicamente para o
// TSR da Unreal Engine 5 (heuristica de paralaxe -> heuristica de sombreamento/estabilidade -> anti-aliasing espacial
// -> atualizacao do historico). Nenhum codigo da Unreal: so a ideia de cada etapa, implementada aqui.
//
// Convencoes (as mesmas do FSR, conferidas em ffx_fsr2_upsample.h): a amostra k do quadro de render fica em
// k + 0,5 - jitter (pixels de render); o vetor de movimento aponta do pixel atual para onde ele estava no quadro
// anterior (anterior = atual + mv * escala).
//
// Passes:
//   1 CSParalaxe   (render)  dilata o movimento pela amostra mais proxima em 3x3, reprojeta a profundidade linear do
//                            quadro anterior e marca desoclusao (o historico daquele pixel era de outra superficie).
//   2 CSAntiAlias  (render)  anti-aliasing espacial leve (estilo subpixel do FXAA), usado onde nao ha historico valido.
//   3 CSHistorico  (saida)   reconstroi a cor atual na resolucao de saida (gaussiana sobre as amostras com jitter),
//                            reprojeta o historico (Catmull-Rom), recorta por variancia em YCoCg (rejeicao de
//                            sombreamento: corta o rastro de folhagem sem vetor) e mistura pelo numero de amostras
//                            acumuladas; mistura em espaco comprimido (c / (1 + max)) para nao piscar em brilhos.
//                            Rejeicao com deteccao de cintilacao: um segundo historico guarda por pixel a luminancia
//                            media, o sinal da ultima diferenca e um placar de cintilacao. Luminancia que cruza a
//                            media de um lado para o outro quadro a quadro e cintilacao (aliasing especular, ruido,
//                            a edicao da rede oscilando): ali a caixa de recorte abre e o historico segura. Mudanca
//                            para um lado so e conteudo novo: o historico e cortado. Com movimento rapido a
//                            protecao some (estabilizar algo que anda deixaria rastro).
inline constexpr char kTsrShader[] = R"(
cbuffer C : register(b0)
{
    uint2  renderSize;
    uint2  displaySize;
    float2 jitter;
    float2 mvScale;
    uint2  mvSize;
    float  exposure;
    uint   flags;          // 1 depth invertida, 2 reset, 4 vetores na resolucao de render, 8 textura de exposicao,
                           // 16 saida sRGB (escrever codificado)
    float  maxSamples;
    float  clipGamma;
    float  parallaxSens;
    float  aaStrength;
    float2 prevJitter;
    float2 mvToRender;
    float  flickerStrength; // 0 = sem deteccao de cintilacao
    float  flickerMaxVel;   // px de render: acima disso a protecao contra cintilacao some
    float  frameRatio;      // tempo deste quadro / 16,67 ms: a deteccao de cintilacao conta em tempo, nao em quadros
};

SamplerState sLin : register(s0);
SamplerState sPt  : register(s1);

static const float kTiny = 1e-5;

float MaxC(float3 c) { return max(c.r, max(c.g, c.b)); }
float3 ToTm(float3 c) { return c / (1.0 + MaxC(c)); }
float3 FromTm(float3 c) { return c / max(1.0 - MaxC(c), kTiny); }
float3 ToYCoCg(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
float3 FromYCoCg(float3 y) { return float3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }
float Lin(float d) { return (flags & 1u) ? 1.0 / max(d, 1e-7) : 1.0 / max(1.0 - d, 1e-7); }

#if defined(TSR_PARALAXE)
Texture2D<float>  depthTex     : register(t0);
Texture2D<float4> mvTex        : register(t1);
Texture2D<float>  prevLinDepth : register(t2);
RWTexture2D<float4> velOut     : register(u0); // xy = deslocamento ate o quadro anterior (px de render), z = desoclusao
RWTexture2D<float>  linOut     : register(u1); // profundidade linear mais proxima (para o proximo quadro)

[numthreads(8, 8, 1)] void CSParalaxe(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= renderSize)) return;
    int2 p = int2(id.xy);
    int2 last = int2(renderSize) - 1;
    float best = (flags & 1u) ? -1.0 : 2.0;
    int2 bp = p;
    float dMin = 1e30, dMax = 0.0;
    [unroll] for (int y = -1; y <= 1; y++)
    [unroll] for (int x = -1; x <= 1; x++)
    {
        int2 q = clamp(p + int2(x, y), int2(0, 0), last);
        float d = depthTex.Load(int3(q, 0));
        if (!isfinite(d)) d = (flags & 1u) ? 0.0 : 1.0;
        bool closer = (flags & 1u) ? (d > best) : (d < best);
        if (closer) { best = d; bp = q; }
        float l = Lin(d);
        dMin = min(dMin, l); dMax = max(dMax, l);
    }
    float2 mvAt = (float2(bp) + 0.5) / mvToRender; // em pixels da textura de vetores
    float2 mv = mvTex.Load(int3(clamp(int2(mvAt), int2(0, 0), int2(mvSize) - 1), 0)).xy;
    if (!all(isfinite(mv))) mv = float2(0.0, 0.0);
    float2 velPx = mv * mvScale * mvToRender;
    float cur = Lin(best);

    float reject = 0.0;
    float2 prevPos = float2(p) + 0.5 + velPx;
    if ((flags & 2u) != 0u || any(prevPos < 0.0) || any(prevPos > float2(renderSize)))
        reject = 1.0;
    else
    {
        float4 pd = prevLinDepth.GatherRed(sPt, prevPos / float2(renderSize));
        float pNear = min(min(pd.x, pd.y), min(pd.z, pd.w));
        float pFar = max(max(pd.x, pd.y), max(pd.z, pd.w));
        // tolerancia: um pouco relativa, mais a inclinacao local da superficie e o movimento
        float slope = (dMax - dMin) / max(cur, kTiny);
        float tol = (0.02 + 0.5 * slope + 0.002 * length(velPx)) / max(parallaxSens, 0.05);
        float lo = pNear * (1.0 - tol), hi = pFar * (1.0 + tol);
        float fora = cur < lo ? (lo - cur) / max(cur * tol, kTiny) : cur > hi ? (cur - hi) / max(cur * tol, kTiny) : 0.0;
        reject = saturate(fora);
    }
    velOut[p] = float4(velPx, reject, cur);
    linOut[p] = cur;
}
#endif

#if defined(TSR_ANTIALIAS)
Texture2D<float4> colorTex : register(t0);
Texture2D<float>  expoTex  : register(t4);
RWTexture2D<float4> aaOut  : register(u0);

float Expo()
{
    float e = exposure;
    if ((flags & 8u) != 0u) { float t = expoTex.Load(int3(0, 0, 0)); if (isfinite(t) && t > 0.0) e = t; }
    return e;
}

float3 Fetch(int2 q, float e)
{
    float3 c = colorTex.Load(int3(clamp(q, int2(0, 0), int2(renderSize) - 1), 0)).rgb;
    return all(isfinite(c)) ? max(c, 0.0) * e : float3(0.0, 0.0, 0.0);
}

[numthreads(8, 8, 1)] void CSAntiAlias(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= renderSize)) return;
    int2 p = int2(id.xy);
    float e = Expo();
    float3 c = Fetch(p, e);
    float3 n = Fetch(p + int2(0, -1), e), s = Fetch(p + int2(0, 1), e), w = Fetch(p + int2(-1, 0), e), o = Fetch(p + int2(1, 0), e);
    float3 nw = Fetch(p + int2(-1, -1), e), ne = Fetch(p + int2(1, -1), e), sw = Fetch(p + int2(-1, 1), e), se = Fetch(p + int2(1, 1), e);
    float3 L = float3(0.299, 0.587, 0.114);
    float lc = dot(ToTm(c), L), ln = dot(ToTm(n), L), ls = dot(ToTm(s), L), lw = dot(ToTm(w), L), lo = dot(ToTm(o), L);
    float lmin = min(lc, min(min(ln, ls), min(lw, lo))), lmax = max(lc, max(max(ln, ls), max(lw, lo)));
    float range = lmax - lmin;
    float3 res = c;
    if (range > max(0.03, 0.125 * lmax))
    {
        // passa-baixa 3x3 e quanto o centro destoa dele (subpixel): pixel isolado ou serrilhado recebe o borrado
        float3 lp = (2.0 * (ToTm(n) + ToTm(s) + ToTm(w) + ToTm(o)) + ToTm(nw) + ToTm(ne) + ToTm(sw) + ToTm(se)) / 12.0;
        float sub = saturate(abs(dot(lp, L) - lc) / range);
        sub = smoothstep(0.0, 1.0, sub);
        float b = sub * sub * 0.75 * aaStrength;
        res = FromTm(lerp(ToTm(c), lp, b));
    }
    aaOut[p] = float4(res / max(e, kTiny), 1.0);
}
#endif

#if defined(TSR_HISTORICO)
Texture2D<float4> colorTex : register(t0);
Texture2D<float4> aaTex    : register(t1);
Texture2D<float4> velTex   : register(t2);
Texture2D<float4> histTex  : register(t3);
Texture2D<float>  expoTex  : register(t4);
Texture2D<float4> metaTex  : register(t5); // x luminancia media, y placar de cintilacao, z sinal da ultima diferenca
RWTexture2D<float4> histOut : register(u0);
RWTexture2D<float4> outTex  : register(u1);
RWTexture2D<float4> metaOut : register(u2);

float Expo()
{
    float e = exposure;
    if ((flags & 8u) != 0u) { float t = expoTex.Load(int3(0, 0, 0)); if (isfinite(t) && t > 0.0) e = t; }
    return e;
}

float4 HistCatmull(float2 uv)
{
    float2 size = float2(displaySize);
    float2 pos = uv * size;
    float2 tc = floor(pos - 0.5) + 0.5;
    float2 f = pos - tc;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 tc0 = (tc - 1.0) / size, tc3 = (tc + 2.0) / size, tc12 = (tc + w2 / w12) / size;
    float a0 = w12.x * w0.y, a1 = w0.x * w12.y, a2 = w12.x * w12.y, a3 = w3.x * w12.y, a4 = w12.x * w3.y;
    float4 r = histTex.SampleLevel(sLin, float2(tc12.x, tc0.y), 0) * a0
             + histTex.SampleLevel(sLin, float2(tc0.x, tc12.y), 0) * a1
             + histTex.SampleLevel(sLin, float2(tc12.x, tc12.y), 0) * a2
             + histTex.SampleLevel(sLin, float2(tc3.x, tc12.y), 0) * a3
             + histTex.SampleLevel(sLin, float2(tc12.x, tc3.y), 0) * a4;
    r /= max(a0 + a1 + a2 + a3 + a4, kTiny);
    r.a = histTex.SampleLevel(sLin, uv, 0).a; // contagem sem o sobressinal do bicubico
    return max(r, 0.0);
}

float3 ClipAabb(float3 h, float3 bmin, float3 bmax)
{
    float3 c = 0.5 * (bmax + bmin);
    float3 ext = 0.5 * (bmax - bmin) + 1e-4;
    float3 v = h - c;
    float3 a = abs(v / ext);
    float m = max(a.x, max(a.y, a.z));
    return m > 1.0 ? c + v / m : h;
}

float3 LinToSrgb(float3 c)
{
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

[numthreads(8, 8, 1)] void CSHistorico(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= displaySize)) return;
    float e = Expo();
    float2 o = float2(id.xy) + 0.5;
    float2 r = o * float2(renderSize) / float2(displaySize); // centro da saida no espaco de render
    int2 k0 = int2(floor(r + jitter));                       // amostra cuja posicao (k + 0,5 - jitter) e a mais proxima
    int2 last = int2(renderSize) - 1;

    // 1. cor atual reconstruida na saida + estatistica da vizinhanca (YCoCg, espaco comprimido)
    float3 soma = 0.0, m1 = 0.0, m2 = 0.0, mn = 1e9, mx = -1e9;
    float wsoma = 0.0, wmax = 0.0;
    [unroll] for (int y = -1; y <= 1; y++)
    [unroll] for (int x = -1; x <= 1; x++)
    {
        int2 k = clamp(k0 + int2(x, y), int2(0, 0), last);
        float3 c = colorTex.Load(int3(k, 0)).rgb;
        c = all(isfinite(c)) ? max(c, 0.0) * e : float3(0.0, 0.0, 0.0);
        float3 t = ToTm(c);
        float2 d = (float2(k0 + int2(x, y)) + 0.5 - jitter) - r;
        float w = exp(-2.0 * dot(d, d));
        soma += t * w; wsoma += w; wmax = max(wmax, w);
        float3 yc = ToYCoCg(t);
        m1 += yc; m2 += yc * yc; mn = min(mn, yc); mx = max(mx, yc);
    }
    float3 atual = soma / max(wsoma, kTiny);
    float3 media = m1 / 9.0;
    float3 desvio = sqrt(max(m2 / 9.0 - media * media, 0.0));

    // 2. paralaxe (do passe 1) e reprojecao do historico
    int2 rk = clamp(int2(r), int2(0, 0), last);
    float4 vel = velTex.Load(int3(rk, 0));
    float2 prevUv = o / float2(displaySize) + vel.xy / float2(renderSize);
    bool fora = (flags & 2u) != 0u || any(prevUv < 0.0) || any(prevUv > 1.0);
    float4 hist = fora ? float4(0.0, 0.0, 0.0, 0.0) : HistCatmull(prevUv);
    float n = fora ? 0.0 : hist.a;
    n *= 1.0 - saturate(vel.z);

    // 3a. cintilacao: luminancia atual contra a media que o pixel vinha tendo (reprojetada como o historico)
    float veloc = length(vel.xy);
    float lumAtual = dot(atual, float3(0.299, 0.587, 0.114));
    float4 meta = fora ? float4(lumAtual, 0.0, 0.0, 0.0) : metaTex.SampleLevel(sPt, prevUv, 0);
    if (!all(isfinite(meta))) meta = float4(lumAtual, 0.0, 0.0, 0.0);
    float dif = lumAtual - meta.x;
    float limiar = 0.01 + 0.5 * dot(desvio, float3(1.0, 0.0, 0.0)); // ruido proprio da vizinhanca (luma do YCoCg)
    float sinal = abs(dif) > limiar ? sign(dif) : 0.0;
    bool cruzou = sinal * meta.z < 0.0;
    // fatores calibrados a 60 fps e convertidos pelo tempo real do quadro (FreeSync / VRR: muda a cada quadro)
    float placar = saturate(cruzou ? meta.y + 0.34 : meta.y * pow(sinal != 0.0 ? 0.6 : 0.92, frameRatio));
    float protecao = placar * flickerStrength * (1.0 - saturate(veloc / max(flickerMaxVel, 0.01)));
    float4 metaNovo = float4(lerp(meta.x, lumAtual, 1.0 - pow(0.65, frameRatio)), placar, sinal != 0.0 ? sinal : meta.z, 0.0);

    // 3b. sombreamento: recorte por variancia; parado = caixa mais larga (estavel), rapido = mais justa (sem rastro);
    //     cintilando = caixa bem mais larga e quase sem corte de memoria
    float g = clipGamma * lerp(1.25, 0.75, saturate(veloc / 8.0)) * (1.0 + 2.0 * protecao);
    float3 hy = ToYCoCg(hist.rgb);
    float3 bmin = max(mn, media - g * desvio), bmax = min(mx, media + g * desvio);
    float3 hc = ClipAabb(hy, bmin, bmax);
    float distancia = length(hc - hy) / max(length(desvio) + 0.01, 0.01);
    n *= 1.0 - 0.5 * saturate(distancia * 0.5) * (1.0 - protecao);
    float3 histC = FromYCoCg(hc);

    // 4. sem historico valido a cor atual vem do anti-aliasing espacial
    float3 aa = aaTex.Load(int3(rk, 0)).rgb;
    aa = all(isfinite(aa)) ? ToTm(max(aa, 0.0) * e) : atual;
    float3 atualUso = lerp(atual, aa, saturate(1.0 - n) * saturate(aaStrength));

    // 5. mistura pela contagem de amostras (cobertura da amostra atual sobre o pixel de saida)
    float alfa = saturate(wmax / (n + wmax));
    alfa = max(alfa, 1.0 / (maxSamples + 1.0));
    float3 res = lerp(histC, atualUso, alfa);
    float nSai = min(n + wmax, maxSamples);
    histOut[id.xy] = float4(res, nSai);
    metaOut[id.xy] = metaNovo;

    float3 lin = FromTm(res) / max(e, kTiny);
    if ((flags & 16u) != 0u) lin = LinToSrgb(lin);
    outTex[id.xy] = float4(lin, 1.0);
}
#endif
)";
