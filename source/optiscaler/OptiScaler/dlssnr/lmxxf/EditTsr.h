#pragma once
// [lmxxf2] TSR da rede DLSS5: o acumulo temporal no estilo do TSR da UE5 (escrito do zero; nenhum codigo da Unreal)
// aplicado na EDICAO da rede, nao no quadro do jogo. O upscaler do jogo (FSR4 etc.) continua o mesmo; o quadro dele
// fica intacto e so a edicao da rede (resposta - entrada, na resolucao da rede) e acumulada.
//
// Por que: a rede roda numa resolucao menor que a saida (o runtime limitava a 1920x1080; com resolucao livre ela pode
// ser qualquer fracao da saida). O caminho antigo esticava a RESPOSTA por bilinear e a somava como "edicao" sobre o
// quadro (edicao = up(resposta) - quadro), entao o resultado era up(resposta): o detalhe 4K do FSR4 sumia ("blur
// independente da definicao"). Aqui a edicao e medida na resolucao da rede (resposta - o que a rede recebeu) e o quadro
// 4K fica intacto.
//
// A ideia do "feed em MP/s" do TSR: a rede e um renderizador de baixa resolucao que entrega mw x mh pixels por resposta;
// a entrada dela recebe um jitter subpixel diferente a cada alimentacao (Halton 2,3), e este passe acumula as amostras
// de cada resposta na resolucao de saida, cada uma com o peso da distancia (em pixels de SAIDA) ate o centro do pixel,
// entao a edicao converge para a resolucao de saida com o tempo. Vazao = mw x mh x respostas/s; 1 amostra por pixel de
// saida a cada 1000 / ((mw*mh)/(w*h) * respostas/s) ms.
//
// Dois passes, ambos na resolucao de saida w x h:
//   CSIntegra   (so quando chega resposta; no espaco do quadro N-1, porque a resposta e de N-1): junta a resposta ao
//               historico (pesos pela distancia; historico recortado pela vizinhanca da resposta nova, com a caixa
//               mais larga onde o pixel cintila); sem historico, o preenchimento espacial (a edicao esticada).
//   CSReprojeta (todo quadro, quadro N): reprojeta o historico pelos vetores do jogo (dilatados pela amostra mais
//               proxima em 3x3, Catmull-Rom), heuristica de paralaxe com a propria profundidade linear do quadro
//               anterior, rejeicao de sombreamento cor crua contra cor crua (luminancia comprimida do quadro cru N em
//               3x3 contra a guardada no historico, com tolerancia ao ruido aprendida por pixel) e deteccao de
//               cintilacao (sinal que cruza a media; fatores convertidos pelo tempo do quadro, VRR); a edicao some onde
//               o historico e rejeitado (sem rastro) e o quadro sai cru + edicao, com o limite do residuo e sem canal
//               negativo (as regras de TemporalStability.h: boundResidual / fitResidual).
//
// Convencoes do backend (LmxxfBackend.cpp): prev = p + 0,5 + mv * (csx, csy) + (jitterDx, jitterDy), em pixels do
// quadro (a do LateEditWarpShader e do passe temporal); a amostra k da rede fica no quadro em (k + 0,5 + jit) * w/mw.
// Incluido dentro do namespace anonimo de LmxxfBackend.cpp, depois de Texture(): usa Check, Barrier, kSrv, kUav, kFp16.

inline constexpr char EditTsrShader[] = R"(
cbuffer C : register(b0)
{
    uint outW; uint outH; uint netW; uint netH;
    float jitX; float jitY; float mvX; float mvY;
    float gridDx; float gridDy; float maxSamples; float clipGamma;
    float parallaxSens; float flickerStrength; float flickerMaxVel; float frameRatio;
    float residualCap; uint flags; float kernelK; uint persist;
    float resurrect; float pad0; float pad1; float pad2;
};
// persist: bit 0/1 = o quadro persistente 0/1 ja foi gravado; resurrect: 1 = ressurreicao ligada
// flags: 1 profundidade invertida, 2 recomeca (historico vazio), 4 sem profundidade (autocorrecao), 8 sem vetores
SamplerState sLin : register(s0);
SamplerState sPt  : register(s1);

static const float kTiny = 1e-5;

float3 ToYCoCg(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
float3 FromYCoCg(float3 y) { return float3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float3 ClipAabb(float3 h, float3 bmin, float3 bmax)
{
    float3 c = 0.5 * (bmax + bmin);
    float3 ext = 0.5 * (bmax - bmin) + 1e-6;
    float3 v = h - c;
    float3 a = abs(v / ext);
    float m = max(a.x, max(a.y, a.z));
    return m > 1.0 ? c + v / m : h;
}

#if defined(EDIT_TSR_INTEGRA)
Texture2D<float4> editNet  : register(t0); // edicao da resposta, na resolucao da rede (unidades do jogo)
Texture2D<float4> histPrev : register(t1); // historico no espaco de N-1: rgb edicao, a peso acumulado
Texture2D<float4> metaPrev : register(t2); // x luminancia, y placar de cintilacao, z sinal, w ruido
RWTexture2D<float4> histOut : register(u0);

float3 EditAt(int2 k)
{
    k = clamp(k, int2(0, 0), int2(int(netW) - 1, int(netH) - 1));
    float3 e = editNet.Load(int3(k, 0)).rgb;
    return all(isfinite(e)) ? e : float3(0.0, 0.0, 0.0);
}

[numthreads(8, 8, 1)] void CSIntegra(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= outW || id.y >= outH) return;
    float2 netSz = float2(float(netW), float(netH));
    float2 s = float2(float(outW), float(outH)) / netSz; // pixels de saida por pixel da rede
    float2 jit = float2(jitX, jitY);
    float2 c = float2(id.xy) + 0.5;
    float2 u = c / s - jit;                              // coordenada continua na rede (centro do texel k em k + 0,5)
    int2 k0 = int2(floor(u));
    int2 lastN = int2(int(netW) - 1, int(netH) - 1);

    // amostras da resposta em volta do pixel, pesadas pela distancia em pixels de SAIDA
    float3 soma = 0.0, m1 = 0.0, m2 = 0.0;
    float wsoma = 0.0;
    [unroll] for (int y = -1; y <= 1; y++)
    [unroll] for (int x = -1; x <= 1; x++)
    {
        int2 k = clamp(k0 + int2(x, y), int2(0, 0), lastN);
        float3 e = EditAt(k);
        float2 d = (float2(k) + 0.5 + jit) * s - c;
        float w = exp(-kernelK * dot(d, d));
        soma += e * w;
        wsoma += w;
        float3 yc = ToYCoCg(e);
        m1 += yc;
        m2 += yc * yc;
    }
    float3 espacial = editNet.SampleLevel(sLin, u / netSz, 0).rgb;
    if (!all(isfinite(espacial))) espacial = EditAt(k0);
    float3 media = m1 / 9.0;
    float3 desvio = sqrt(max(m2 / 9.0 - media * media, 0.0));

    float4 h = histPrev.Load(int3(id.xy, 0));
    if ((flags & 2u) != 0u || !all(isfinite(h))) h = float4(0.0, 0.0, 0.0, 0.0);
    float4 meta = metaPrev.Load(int3(id.xy, 0));
    float prot = all(isfinite(meta)) ? saturate(meta.y) * flickerStrength : 0.0;

    // o historico nao pode fugir do que a resposta nova diz em volta do pixel (corta atraso); onde o pixel cintila a
    // caixa abre e o historico segura (a resposta da rede oscilando nao vira tremor)
    float g = clipGamma * 2.0 * (1.0 + 2.0 * prot);
    float3 hy = ToYCoCg(h.rgb);
    float3 hc = ClipAabb(hy, media - g * desvio - 1e-3, media + g * desvio + 1e-3);
    float dist = length(hc - hy) / max(length(desvio) + 1e-3, 1e-3);
    float wh = max(h.a, 0.0) * (1.0 - 0.5 * saturate(dist * 0.5) * (1.0 - prot));
    float3 hist = FromYCoCg(hc);

    float wt = wh + wsoma;
    float3 acc = wt > kTiny ? (hist * wh + soma) / wt : espacial;
    // pouca cobertura acumulada: ainda e quase so a edicao esticada (some conforme as amostras com jitter chegam)
    float3 res = lerp(espacial, acc, saturate(wt));
    if (!all(isfinite(res))) { res = espacial; wt = 0.0; }
    histOut[id.xy] = float4(res, min(wt, maxSamples));
}
#endif

#if defined(EDIT_TSR_REPROJETA)
Texture2D<float4> histSrc   : register(t0); // historico no espaco de N-1 (ja integrado se chegou resposta)
Texture2D<float4> metaPrev  : register(t1);
Texture2D<float>  depthPrev : register(t2); // profundidade linear de N-1 (nossa)
Texture2D<float4> colorTex  : register(t3); // quadro cru N, unidades do jogo
Texture2D<float4> mvTex     : register(t4); // vetores na resolucao do quadro
Texture2D<float>  depthTex  : register(t5); // profundidade do jogo na resolucao do quadro
Texture2D<float>  expoTex   : register(t6); // exposicao da rede (1x1)
Texture2D<float4> perEdit0  : register(t7); // quadros persistentes (ressurreicao): edicao e peso
Texture2D<float4> perRaw0   : register(t8); // e a cor crua do quadro em que foram gravados
Texture2D<float4> perEdit1  : register(t9);
Texture2D<float4> perRaw1   : register(t10);
RWTexture2D<float4> histOut  : register(u0);
RWTexture2D<float4> metaOut  : register(u1);
RWTexture2D<float>  depthOut : register(u2);
RWTexture2D<float4> outTex   : register(u3);

float Lin(float d) { return (flags & 1u) ? 1.0 / max(d, 1e-7) : 1.0 / max(1.0 - d, 1e-7); }

float3 Raw(int2 q, int2 last)
{
    float3 c = colorTex.Load(int3(clamp(q, int2(0, 0), last), 0)).rgb;
    return all(isfinite(c)) ? c : float3(0.0, 0.0, 0.0);
}
// luminancia comprimida l / (1 + l), l na exposicao da rede (branco de papel ~1): HDR nao vira outlier
float LumT(float3 c, float e)
{
    float l = max(Luma(max(c, 0.0)) * e, 0.0);
    return l / (1.0 + l);
}

// cor no espaco comprimido c / (1 + max), na exposicao da rede
float3 Tm3(float3 c, float e)
{
    c = max(c, 0.0) * e;
    return c / (1.0 + max(c.r, max(c.g, c.b)));
}
// diferenca media da vizinhanca 3x3 da cor crua de agora contra a gravada num quadro persistente
float PatchDiff(Texture2D<float4> snap, int2 p, int2 last, float e)
{
    float d = 0.0;
    [unroll] for (int y = -1; y <= 1; y++)
    [unroll] for (int x = -1; x <= 1; x++)
    {
        int2 q = clamp(p + int2(x, y), int2(0, 0), last);
        float3 s = snap.Load(int3(q, 0)).rgb;
        if (!all(isfinite(s))) return 1e9;
        float3 v = abs(Tm3(Raw(q, last), e) - Tm3(s, e));
        d += max(v.r, max(v.g, v.b));
    }
    return d / 9.0;
}

float4 HistCatmull(float2 uv)
{
    float2 size = float2(float(outW), float(outH));
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
    float3 r = histSrc.SampleLevel(sLin, float2(tc12.x, tc0.y), 0).rgb * a0
             + histSrc.SampleLevel(sLin, float2(tc0.x, tc12.y), 0).rgb * a1
             + histSrc.SampleLevel(sLin, float2(tc12.x, tc12.y), 0).rgb * a2
             + histSrc.SampleLevel(sLin, float2(tc3.x, tc12.y), 0).rgb * a3
             + histSrc.SampleLevel(sLin, float2(tc12.x, tc3.y), 0).rgb * a4;
    r /= max(a0 + a1 + a2 + a3 + a4, kTiny);
    float wgt = histSrc.SampleLevel(sLin, uv, 0).a; // peso sem o sobressinal do bicubico
    // a edicao e assinada (escurece e clareia): o sobressinal do bicubico fica preso entre os 4 vizinhos
    float3 lo = 1e30, hi = -1e30;
    float2 b = floor(pos - 0.5);
    [unroll] for (int j = 0; j < 4; j++)
    {
        float2 o = float2(j & 1, j >> 1);
        float3 v = histSrc.SampleLevel(sPt, (b + o + 0.5) / size, 0).rgb;
        lo = min(lo, v);
        hi = max(hi, v);
    }
    return float4(clamp(r, lo, hi), max(wgt, 0.0));
}

// TemporalStability.h: o limite do residuo escala o trio inteiro (nunca por canal: canal sozinho vira matiz errada) e
// nenhum canal de quadro + edicao fica negativo (o ciano sob o carro)
float3 BoundResidual(float3 r, float3 b, float cap)
{
    if (cap <= 0.0) return r;
    float bound = cap * max(max(max(abs(b.r), abs(b.g)), abs(b.b)), 0.02);
    float mag = max(max(abs(r.r), abs(r.g)), abs(r.b));
    return (mag > bound) ? r * (bound / mag) : r;
}
float3 FitResidual(float3 b, float3 r)
{
    float a = 1.0;
    [unroll] for (int i = 0; i < 3; ++i)
        if (r[i] < -1e-6) a = min(a, -b[i] / r[i]);
    return r * saturate(a);
}

[numthreads(8, 8, 1)] void CSReprojeta(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= outW || id.y >= outH) return;
    int2 p = int2(id.xy);
    int2 last = int2(int(outW) - 1, int(outH) - 1);
    float2 size = float2(float(outW), float(outH));
    float e = expoTex.Load(int3(0, 0, 0));
    if (!(isfinite(e) && e > 0.0)) e = 1.0;
    const bool recomeca = (flags & 2u) != 0u;
    const bool semProf = (flags & 4u) != 0u;

    // 1. paralaxe: a amostra mais proxima em 3x3 dita o vetor (borda de objeto na frente nao arrasta o fundo)
    float best = (flags & 1u) ? -1.0 : 2.0;
    int2 bp = p;
    float dMin = 1e30, dMax = 0.0;
    [unroll] for (int y = -1; y <= 1; y++)
    [unroll] for (int x = -1; x <= 1; x++)
    {
        int2 q = clamp(p + int2(x, y), int2(0, 0), last);
        float d = depthTex.Load(int3(q, 0));
        if (!isfinite(d)) d = (flags & 1u) ? 0.0 : 1.0;
        bool perto = (flags & 1u) ? (d > best) : (d < best);
        if (perto) { best = d; bp = q; }
        float l = Lin(d);
        dMin = min(dMin, l);
        dMax = max(dMax, l);
    }
    float2 m = (flags & 8u) ? float2(0.0, 0.0) : mvTex.Load(int3(bp, 0)).xy * float2(mvX, mvY);
    if (!all(isfinite(m))) m = float2(0.0, 0.0);
    float2 prev = float2(p) + 0.5 + m + float2(gridDx, gridDy);
    bool fora = recomeca || any(prev < 0.0) || any(prev > size);
    float2 prevUv = prev / size;

    float rejP = fora ? 1.0 : 0.0;
    float atualLin = Lin(best);
    if (!fora && !semProf && parallaxSens > 0.0)
    {
        float4 pd = depthPrev.GatherRed(sPt, prevUv);
        if (all(isfinite(pd)) && all(pd > 0.0))
        {
            float pNear = min(min(pd.x, pd.y), min(pd.z, pd.w));
            float pFar = max(max(pd.x, pd.y), max(pd.z, pd.w));
            float slope = (dMax - dMin) / max(atualLin, kTiny);
            float tol = (0.02 + 0.5 * slope + 0.002 * length(m)) / max(parallaxSens, 0.05);
            float lo = pNear * (1.0 - tol), hi = pFar * (1.0 + tol);
            float sai = atualLin < lo ? (lo - atualLin) / max(atualLin * tol, kTiny)
                      : atualLin > hi ? (atualLin - hi) / max(atualLin * tol, kTiny) : 0.0;
            rejP = saturate(sai);
        }
    }

    // 2. historico reprojetado
    float4 H = fora ? float4(0.0, 0.0, 0.0, 0.0) : HistCatmull(prevUv);
    if (!all(isfinite(H))) H = float4(0.0, 0.0, 0.0, 0.0);

    // 3. sombreamento, cor crua contra cor crua: a luminancia que o historico guardou tem que caber na faixa da
    //    vizinhanca 3x3 do quadro cru atual (folha andando sem vetor, desoclusao sem profundidade, luz mudando)
    float lumC = 0.0, lumMin = 1.0, lumMax = 0.0;
    [unroll] for (int y2 = -1; y2 <= 1; y2++)
    [unroll] for (int x2 = -1; x2 <= 1; x2++)
    {
        float l = LumT(Raw(p + int2(x2, y2), last), e);
        lumMin = min(lumMin, l);
        lumMax = max(lumMax, l);
        if (x2 == 0 && y2 == 0) lumC = l;
    }
    float4 meta = fora ? float4(lumC, 0.0, 0.0, 0.01) : metaPrev.SampleLevel(sPt, prevUv, 0);
    if (!all(isfinite(meta))) meta = float4(lumC, 0.0, 0.0, 0.01);
    float ruido = clamp(meta.w, 0.0, 0.2);
    float dif = lumC - meta.x;
    float foraFaixa = max(max(meta.x - lumMax, lumMin - meta.x), 0.0);
    float tolS = 0.006 + 2.0 * ruido;
    float rejS = fora ? 1.0 : saturate((foraFaixa - tolS) / tolS);

    // 4. cintilacao: a luminancia que cruza a media de um lado para o outro e oscilacao, nao conteudo novo; ali a
    //    rejeicao de sombreamento afrouxa. Fatores de 60 fps convertidos pelo tempo real do quadro (VRR / FreeSync).
    //    Movimento rapido desliga a protecao (segurar o que anda deixaria rastro).
    float limiar = 0.004 + ruido;
    float sinal = abs(dif) > limiar ? sign(dif) : 0.0;
    bool cruzou = sinal * meta.z < 0.0;
    float placar = fora ? 0.0 : saturate(cruzou ? meta.y + 0.34 : meta.y * pow(sinal != 0.0 ? 0.6 : 0.92, frameRatio));
    float veloc = length(m);
    float prot = placar * flickerStrength * (1.0 - saturate(veloc / max(flickerMaxVel, 0.01)));
    rejS *= 1.0 - prot;

    float rej = max(rejP, rejS);
    float fica = 1.0 - rej;
    float3 edit = H.rgb * fica; // rejeitado = sem edicao (sem rastro); a proxima resposta preenche
    float peso = H.a * fica;

    // 5. ressurreicao (o "Resurrection" do TSR, sem as matrizes de camera, que o jogo nao entrega ao upscaler): onde o
    //    historico foi descartado, um quadro persistente (gravado a cada 31 quadros, dois guardados) devolve a edicao
    //    do mesmo lugar da tela se a vizinhanca 3x3 da cor crua de agora bate com a gravada: objeto que passou na
    //    frente e saiu, coisa que voltou a aparecer com a camera parada. Camera andando: nao bate e nada volta.
    bool volta = false;
    if (resurrect > 0.5 && !recomeca && persist != 0u && rej > 0.3)
    {
        const float tolR = 0.004 + 1.5 * ruido;
        float d0 = (persist & 1u) != 0u ? PatchDiff(perRaw0, p, last, e) : 1e9;
        float d1 = (persist & 2u) != 0u ? PatchDiff(perRaw1, p, last, e) : 1e9;
        float4 snap = d0 <= d1 ? perEdit0.Load(int3(p, 0)) : perEdit1.Load(int3(p, 0));
        if (min(d0, d1) < tolR && all(isfinite(snap)) && snap.a >= 1.0)
        {
            edit = snap.rgb;
            peso = max(peso, 0.5 * min(snap.a, maxSamples)); // volta com metade da confianca que tinha
            volta = true;
        }
    }

    float ruidoNovo = (fora || rejP > 0.5) ? 0.01 : lerp(ruido, min(abs(dif), 0.2), 1.0 - pow(0.85, frameRatio));
    float lumNova = (rej > 0.5 || volta) ? lumC : lerp(meta.x, lumC, 1.0 - pow(0.65, frameRatio));
    metaOut[p] = float4(lumNova, placar, sinal != 0.0 ? sinal : (fora ? 0.0 : meta.z), ruidoNovo);
    histOut[p] = float4(edit, peso);
    float dAqui = depthTex.Load(int3(p, 0));
    depthOut[p] = semProf || !isfinite(dAqui) ? 0.0 : Lin(dAqui);

    // 5. quadro cru + edicao
    float4 raw = colorTex.Load(int3(p, 0));
    if (!all(isfinite(raw.rgb))) raw.rgb = 0.0;
    if (!isfinite(raw.a)) raw.a = 1.0;
    float3 r = FitResidual(raw.rgb, BoundResidual(edit, raw.rgb, residualCap));
    outTex[p] = float4(raw.rgb + r, raw.a);
}
#endif
)";

class EditTsr
{
  public:
    struct Params
    {
        UINT outW = 0, outH = 0, netW = 0, netH = 0;
        float jitX = 0.f, jitY = 0.f, mvX = 0.f, mvY = 0.f;
        float gridDx = 0.f, gridDy = 0.f, maxSamples = 16.f, clipGamma = 1.f;
        float parallax = 1.f, flicker = 0.75f, flickerMaxVel = 4.f, frameRatio = 1.f;
        float residualCap = 0.f;
        UINT flags = 0;
        float kernelK = 3.f;
        UINT persist = 0;      // preenchido pela classe: os quadros persistentes ja gravados
        float resurrect = 0.f; // 1 = ressurreicao ligada
        float pad0 = 0.f, pad1 = 0.f, pad2 = 0.f;
    };
    static_assert(sizeof(Params) == 24 * 4, "as constantes do shader sao 24 dwords");
    enum : UINT { kInvertida = 1u, kRecomeca = 2u, kSemProfundidade = 4u, kSemVetores = 8u };

    explicit EditTsr(ID3D12Device* d) : device(d)
    {
        D3D12_DESCRIPTOR_RANGE ranges[2] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSrvs, 0, 0, 0 },
                                             { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kUavs, 0, 0, kSrvs } };
        D3D12_ROOT_PARAMETER params[2] {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = { 2, ranges };
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = { 0, 0, kConstants };
        D3D12_STATIC_SAMPLER_DESC samp[2] {};
        for (UINT i = 0; i < 2; ++i)
        {
            samp[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
            samp[i].AddressU = samp[i].AddressV = samp[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samp[i].ShaderRegister = i;
            samp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        D3D12_ROOT_SIGNATURE_DESC rd {};
        rd.NumParameters = 2;
        rd.pParameters = params;
        rd.NumStaticSamplers = 2;
        rd.pStaticSamplers = samp;
        ComPtr<ID3DBlob> b, e;
        Check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &b, &e), "edit TSR root signature");
        Check(d->CreateRootSignature(0, b->GetBufferPointer(), b->GetBufferSize(), IID_PPV_ARGS(&root)), "edit TSR root signature");
        psoIntegra = Compila("EDIT_TSR_INTEGRA", "CSIntegra");
        psoReprojeta = Compila("EDIT_TSR_REPROJETA", "CSReprojeta");
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kRegioes * kBloco;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "edit TSR descriptor heap");
        passo = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // texturas do tamanho do quadro; true quando foram (re)criadas: o proximo quadro recomeca o historico. As antigas
    // vao para `enterra` (o cemiterio do backend): listas de quadros anteriores ainda podem estar usando na GPU.
    template <class Enterra> bool Garante(UINT w, UINT h, Enterra&& enterra)
    {
        if (out && w == largura && h == altura)
            return false;
        // std::addressof: o operator& do ComPtr devolve um ComPtrRef, nao o endereco
        ComPtr<ID3D12Resource>* todas[] = { std::addressof(hist[0]),    std::addressof(hist[1]),    std::addressof(meta[0]),
                                            std::addressof(meta[1]),    std::addressof(prof[0]),    std::addressof(prof[1]),
                                            std::addressof(histInt),    std::addressof(out),        std::addressof(perEdit[0]),
                                            std::addressof(perEdit[1]), std::addressof(perRaw[0]),  std::addressof(perRaw[1]) };
        for (auto* t : todas)
            if (*t)
                enterra(*t);
        for (UINT i = 0; i < 2; ++i)
        {
            hist[i] = Texture(device.Get(), w, h, kFp16);
            meta[i] = Texture(device.Get(), w, h, kFp16);
            prof[i] = Texture(device.Get(), w, h, DXGI_FORMAT_R32_FLOAT);
            perEdit[i] = Texture(device.Get(), w, h, kFp16);
            perRaw[i] = Texture(device.Get(), w, h, kFp16);
        }
        histInt = Texture(device.Get(), w, h, kFp16);
        out = Texture(device.Get(), w, h, kFp16);
        largura = w;
        altura = h;
        atual = 0;
        integrado = false;
        vazio = true;
        persist = 0;
        return true;
    }
    // memoria das texturas (MB): 2 historicos, 2 metadados, 2 profundidades, o integrado, a saida e 2 x 2 persistentes
    double Megabytes() const { return largura * altura * (2.0 * 8 + 2.0 * 8 + 2.0 * 4 + 8 + 8 + 4.0 * 8) / (1024.0 * 1024.0); }

    // a resposta chegou: junta a edicao dela (editNet, mw x mh, em kSrv) ao historico, no espaco do quadro N-1
    void Integra(ID3D12GraphicsCommandList* c, ID3D12Resource* editNet, Params k)
    {
        if (vazio)
            k.flags |= kRecomeca;
        ID3D12Resource* srv[kSrvs] = { editNet, hist[atual].Get(), meta[atual].Get() };
        const DXGI_FORMAT fmt[kSrvs] = { kFp16, kFp16, kFp16 };
        ID3D12Resource* uav[kUavs] = { histInt.Get() };
        const DXGI_FORMAT ufmt[kUavs] = { kFp16 };
        Barrier(c, histInt.Get(), kSrv, kUav);
        Despacha(c, psoIntegra.Get(), srv, fmt, uav, ufmt, k);
        Barrier(c, histInt.Get(), kUav, kSrv);
        integrado = true;
    }

    // todo quadro: reprojeta (e recorta) o historico para o quadro N e devolve quadro cru + edicao (em kSrv).
    // colour, motion, depth e expo ja em kSrv (o chamador cuida das barreiras dos recursos do jogo).
    ID3D12Resource* Reprojeta(ID3D12GraphicsCommandList* c, ID3D12Resource* colour, ID3D12Resource* motion, DXGI_FORMAT motionFmt,
                              ID3D12Resource* depth, DXGI_FORMAT depthFmt, ID3D12Resource* expo, Params k)
    {
        if (vazio)
            k.flags |= kRecomeca;
        k.persist = persist;
        const UINT prox = 1u - atual;
        ID3D12Resource* srv[kSrvs] = { integrado ? histInt.Get() : hist[atual].Get(), meta[atual].Get(), prof[atual].Get(),
                                       colour, motion, depth, expo, perEdit[0].Get(), perRaw[0].Get(), perEdit[1].Get(),
                                       perRaw[1].Get() };
        const DXGI_FORMAT fmt[kSrvs] = { kFp16, kFp16, DXGI_FORMAT_R32_FLOAT, kFp16, motionFmt, depthFmt, DXGI_FORMAT_R32_FLOAT,
                                         kFp16, kFp16, kFp16, kFp16 };
        ID3D12Resource* uav[kUavs] = { hist[prox].Get(), meta[prox].Get(), prof[prox].Get(), out.Get() };
        const DXGI_FORMAT ufmt[kUavs] = { kFp16, kFp16, DXGI_FORMAT_R32_FLOAT, kFp16 };
        for (auto* r : uav)
            Barrier(c, r, kSrv, kUav);
        Despacha(c, psoReprojeta.Get(), srv, fmt, uav, ufmt, k);
        for (auto* r : uav)
            Barrier(c, r, kUav, kSrv);
        atual = prox;
        integrado = false;
        vazio = false;
        return out.Get();
    }
    // ressurreicao: a cada kIntervalo quadros (o PersistentFrameInterval do TSR, 31), o historico que Reprojeta acabou
    // de escrever e o quadro cru deste quadro (`colour`, em kSrv) vao para um dos dois quadros persistentes, alternando
    void Grava(ID3D12GraphicsCommandList* c, ID3D12Resource* colour)
    {
        if (vazio || !out || (contador++ % kIntervalo) != 0u)
            return;
        const D3D12_RESOURCE_STATES kCopiaDe = D3D12_RESOURCE_STATE_COPY_SOURCE, kCopiaPara = D3D12_RESOURCE_STATE_COPY_DEST;
        ID3D12Resource* const h = hist[atual].Get();
        Barrier(c, h, kSrv, kCopiaDe);
        Barrier(c, colour, kSrv, kCopiaDe);
        Barrier(c, perEdit[slot].Get(), kSrv, kCopiaPara);
        Barrier(c, perRaw[slot].Get(), kSrv, kCopiaPara);
        c->CopyResource(perEdit[slot].Get(), h);
        c->CopyResource(perRaw[slot].Get(), colour);
        Barrier(c, perRaw[slot].Get(), kCopiaPara, kSrv);
        Barrier(c, perEdit[slot].Get(), kCopiaPara, kSrv);
        Barrier(c, colour, kCopiaDe, kSrv);
        Barrier(c, h, kCopiaDe, kSrv);
        persist |= 1u << slot;
        slot ^= 1u;
    }
    // o historico recomeca no proximo quadro (reset do jogo, troca de modo, rede calada); os persistentes tambem
    void Recomeca() { vazio = true; integrado = false; persist = 0; contador = 0; }

  private:
    static constexpr UINT kSrvs = 11, kUavs = 4, kBloco = kSrvs + kUavs, kRegioes = 16, kConstants = 24, kIntervalo = 31;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> psoIntegra, psoReprojeta;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT passo = 0, regiao = 0;
    ComPtr<ID3D12Resource> hist[2], meta[2], prof[2], histInt, out, perEdit[2], perRaw[2];
    UINT largura = 0, altura = 0, atual = 0, persist = 0, slot = 0, contador = 0;
    bool integrado = false, vazio = true;

    ComPtr<ID3D12PipelineState> Compila(const char* define, const char* entrada)
    {
        const D3D_SHADER_MACRO defs[] = { { define, "1" }, { nullptr, nullptr } };
        ComPtr<ID3DBlob> b, e;
        const HRESULT hr = DlssNr::SysCompiler::Compile(EditTsrShader, sizeof(EditTsrShader), "lmxxf2 TSR da rede", defs, nullptr,
                                                        entrada, "cs_5_0", 0, 0, &b, &e);
        if (FAILED(hr))
            throw std::runtime_error(std::string("TSR da rede: o shader ") + entrada + " nao compilou: " +
                                     (e ? std::string(static_cast<const char*>(e->GetBufferPointer()), e->GetBufferSize()) : std::string("?")));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = root.Get();
        pd.CS = { b->GetBufferPointer(), b->GetBufferSize() };
        ComPtr<ID3D12PipelineState> pso;
        Check(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)), "edit TSR pipeline");
        return pso;
    }

    void Despacha(ID3D12GraphicsCommandList* c, ID3D12PipelineState* pso, ID3D12Resource* const* srv, const DXGI_FORMAT* fmt,
                  ID3D12Resource* const* uav, const DXGI_FORMAT* ufmt, const Params& k)
    {
        const UINT base = regiao * kBloco;
        regiao = (regiao + 1) % kRegioes;
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(base) * passo;
        for (UINT i = 0; i < kSrvs; ++i, cpu.ptr += passo)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC s {};
            s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            s.Texture2D.MipLevels = 1;
            // slot sem recurso: vista nula com um formato valido (o shader do passe nao le esse registro)
            s.Format = srv[i] ? fmt[i] : DXGI_FORMAT_R16G16B16A16_FLOAT;
            device->CreateShaderResourceView(srv[i], &s, cpu);
        }
        for (UINT i = 0; i < kUavs; ++i, cpu.ptr += passo)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC u {};
            u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            u.Format = uav[i] ? ufmt[i] : DXGI_FORMAT_R16G16B16A16_FLOAT;
            device->CreateUnorderedAccessView(uav[i], nullptr, &u, cpu);
        }
        auto hh = heap.Get();
        c->SetDescriptorHeaps(1, &hh);
        c->SetComputeRootSignature(root.Get());
        c->SetPipelineState(pso);
        auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<UINT64>(base) * passo;
        c->SetComputeRootDescriptorTable(0, gpu);
        c->SetComputeRoot32BitConstants(1, kConstants, &k, 0);
        c->Dispatch((k.outW + 7) / 8, (k.outH + 7) / 8, 1);
    }
};
