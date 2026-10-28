// HLSL dos passes do overlay (compilados na inicializacao, cs_5_1)
//
// Referencia da imagem da NVIDIA (o que o DLSS 5 + RenoDX mostram), descrita em
// zmodelerlover/dlss5-neural-amd (MIT), docs/nvidia-parity.md e docs/styles-model-abc.md:
//  - a rede recebe um PROXY do quadro: SDR como esta; HDR por uma ponte (SoftKnee do quadro / branco de papel);
//  - a resposta volta por uma razao de luz em dois ramos (proxy -> original), intensidade, piso 1/512, protecao
//    bilateral (2,0) e mistura de cor;
//  - Model B/C = controle de estilo da rede (estilo/128) + gradacao final do kernel cg2r_post_process da NVIDIA:
//    exposicao, contraste por curva S e saturacao HSL, escaladas pelo LocalToneStrength.
// ACES: o ajuste RRT+ODT de Stephen Hill que o RenoDX usa (clshortfuse/renodx, MIT, tonemap.hlsl ACESFittedAP1).
// Implementacao propria; nenhum codigo GPL.
#pragma once

namespace ov::hlsl {

// funcoes comuns (prefixadas nos passes que as usam)
inline const char* Comum = R"(
static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);
float Lum(float3 c) { return dot(c, LUMA); }
float Max3(float3 c) { return max(c.r, max(c.g, c.b)); }
float Min3(float3 c) { return min(c.r, min(c.g, c.b)); }
float3 Linear(float3 c) { return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 Srgb(float3 l) { l = saturate(l); return (l <= 0.0031308) ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055; }

// como a edicao (resultado r sobre o original o) chega a outra resolucao ou a outro quadro: 0 linear, a diferenca
// somada; 1 logaritmica, log2 da razao multiplicada (piso 1/512); 2 mista, clarear como luz somada e escurecer pela
// razao. No mesmo pixel as tres dao o mesmo resultado. Levada pelo movimento do jogo ao quadro seguinte, onde o fio de
// cabelo nao cai no mesmo pixel, a razao de um fio escuro que a rede clareou 10x multiplicava o ceu claro ao lado (o
// grao branco na borda do cabelo); somada, a mesma luz vira o halo dela. Escurecer pela razao nao passa do preto (somar
// o escurecimento de um claro num escuro cortava em preto). Medido levando a edicao ao quadro seguinte (4 reproducoes
// 2160p): erro 2,9-3,6x menor que a log, pintas claras na borda 0,09-2,34% log, 0-0,25% mista. Na ampliacao para a
// tela a mista vale como a log (Temporal.h LuzNaTela).
static const float PISO_LUZ = 1.0 / 512.0;
float3 CodificaLuz(float3 o, float3 r, uint luz)
{
    const float3 razao = log2(max(r, PISO_LUZ) / max(o, PISO_LUZ));
    return luz == 0 ? r - o : luz == 1 ? razao : (r >= o ? r - o : razao);
}
float3 AplicaLuz(float3 o, float3 e, uint luz, float forca)
{
    const float3 somada = o + forca * e, multiplicada = o + max(o, PISO_LUZ) * (exp2(forca * e) - 1.0);
    return luz == 0 ? somada : luz == 1 ? multiplicada : (e >= 0.0 ? somada : multiplicada);
}

// joelho suave: identidade ate JOELHO, depois aproxima 1 de forma exponencial (C1, inversivel)
static const float JOELHO = 0.75;
float Joelho(float x)
{
    return x <= JOELHO ? x : JOELHO + (1.0 - JOELHO) * (1.0 - exp(-(x - JOELHO) / (1.0 - JOELHO)));
}
float JoelhoInv(float y)
{
    y = min(y, 0.99999);
    return y <= JOELHO ? y : JOELHO - (1.0 - JOELHO) * log(1.0 - (y - JOELHO) / (1.0 - JOELHO));
}
// pelo canal maximo: o matiz fica e o resultado cabe em [0,1]
float3 SoftKnee(float3 c)
{
    c = max(c, 0.0);
    float m = Max3(c);
    return m > JOELHO ? c * (Joelho(m) / m) : c;
}
float3 SoftKneeInv(float3 c)
{
    c = max(c, 0.0);
    float m = Max3(c);
    return m > JOELHO ? c * (JoelhoInv(m) / m) : c;
}
// ponte SoftKnee em HDR (sanduiche): a tela SDR equivalente de uma luz linear e a folga, o que passa do teto da inversa
// (realces acima de ~3,3x o branco), devolvida na saida para nao cortar o pico HDR. Calculadas em float32 na leitura:
// guardar a tela comprimida em FP16 e desfazer o joelho depois ampliava o degrau do FP16 (entre 2x e 2,5x o branco
// sobravam 4 niveis, com degraus de ~17%: o banding em volta do sol).
float3 Sanduiche(float3 l, out float folga)
{
    l = max(l, 0.0);
    const float3 sdr = SoftKnee(l);
    folga = max(Lum(l), 1e-6) / max(Lum(SoftKneeInv(sdr)), 1e-6);
    return sdr;
}

// ACES (RRT+ODT para SDR 100 nits), ajuste de Stephen Hill como no RenoDX (MIT)
float3 AcesFittedAP1(float3 color)
{
    const float3x3 ENTRADA = { 0.59719, 0.35458, 0.04823,
                               0.07600, 0.90834, 0.01566,
                               0.02840, 0.13383, 0.83777 };
    const float3x3 SAIDA = { 1.60475, -0.53108, -0.07367,
                            -0.10208,  1.10813, -0.00605,
                            -0.00327, -0.07276,  1.07602 };
    color = mul(ENTRADA, max(color, 0.0));
    float3 a = color * (color + 0.0245786) - 0.000090537;
    float3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
    return saturate(mul(SAIDA, a / b));
}

// curva filmica do jogo (ACES de Narkowicz, perto do filmic do UE): cena -> tela linear, e a inversa exata.
// No jogo o DLSS 5 roda ANTES do tonemap: a rede ve a luz de cena e a curva do jogo vem depois (pe que escurece o preto,
// ombro que sobe o claro sem estourar). No overlay (quadro ja com tonemap) a ponte 3 desfaz a curva na entrada e a
// refaz na saida.
float3 Filmica(float3 x)
{
    x = max(x, 0.0);
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}
float3 FilmicaInv(float3 y)
{
    y = clamp(y, 0.0, 0.9999);
    float3 a = 2.43 * y - 2.51, b = 0.59 * y - 0.03, c = 0.14 * y;
    return (-b - sqrt(max(b * b - 4.0 * a * c, 0.0))) / (2.0 * a);
}

// ponte do quadro (linear, branco de papel = 1) para o proxy que a rede ve: 0 direto, 1 SoftKnee, 2 ACES,
// 3 luz de cena sem corte (o codificador do runtime aplica o joelho e o sRGB da NVIDIA), 4 luz como chega, sem corte
// nem curva. A 4 e o contrato do modo 1 do codificador da NVIDIA (native_codec_encode.hlsl: luz / branco de papel,
// ombro acima de 0,75, sRGB): na captura de buffer a cor do jogo ja e luz de cena antes do tonemap, e so o ombro do
// codificador deve comprimir os realces. Com a SoftKnee (a automatica em HDR) ou a direta, todo realce acima de ~1,25x o
// branco chegava a rede como o mesmo 0,974 (sRGB) e nada nunca chegava a 1,0.
float3 Proxy(float3 o, uint ponte)
{
    if (ponte == 1) return SoftKnee(o);
    if (ponte == 2) return AcesFittedAP1(o);
    if (ponte == 3 || ponte == 4) return max(o, 0.0);
    return saturate(o);
}

// luzes / sombras / cor antes da rede (a ideia do Shadow-Light-Scattering.fx, sem o corte): ganho pela luma de tela
// acima/abaixo dos limiares e saturacao. Vai na entrada da rede e no resultado; o "original" fica intacto.
float3 PreAjuste(float3 l, float3 tela, float luz, float sombra, float lim, float limS, float cor)
{
    if (luz != 0.0 || sombra != 0.0)
    {
        float L = dot(saturate(tela), float3(0.299, 0.587, 0.114));
        float t = clamp(lim, 0.01, 0.99), ts = clamp(limS, 0.01, 0.99);
        l *= (1.0 + luz * max(0.0, L - t)) * max(0.0, 1.0 - sombra * max(0.0, ts - L) / ts);
    }
    if (cor != 0.0)
    {
        float Y = Lum(l);
        l = max(Y + (l - Y) * (1.0 + cor), 0.0);
    }
    return l;
}
// valor de exibicao (0..1) de um valor linear do overlay, para os limiares: a curva do jogo, o SoftKnee (HDR) ou direto
float3 TelaDe(float3 l, bool hdrv, bool filmicav)
{
    return Srgb(filmicav ? Filmica(l) : (hdrv ? SoftKnee(l) : saturate(l)));
}

// composicao de referencia (NVIDIA / RenoDX): O original, P proxy, M resposta da rede (unidades lineares)
float3 ComporReferencia(float3 O, float3 P, float3 M, float s, float cor, float guarda)
{
    O = max(O, 0.0); P = max(P, 0.0); M = max(M, 0.0);
    float yo = Lum(O), yp = Lum(P), ym = Lum(M);
    // do proxy de volta ao original: mais escuro que o proxy -> escala; mais claro -> soma a folga que o proxy
    // nao carregou (realces comprimidos pela ponte) a luz da resposta
    float razao = (yo <= yp) ? yo / max(yp, 1e-6) : (ym + (yo - yp)) / max(ym, 1e-6);
    float3 up = lerp(O, M * razao, saturate(s));
    const float piso = 1.0 / 512.0;
    float rho0 = max(Lum(up), piso) / max(yo, piso);
    float rho = s > 1.0 ? pow(rho0, s) : rho0;
    float b = clamp(rho, 1.0 / guarda, guarda);
    return lerp(O * b, up * (b / rho0), saturate(cor));
}

// gradacao do Model B/C (kernel cg2r_post_process da NVIDIA), em valores de tela [0,1]
float3 GradacaoModelo(float3 c, float kExp, float kCon, float kSat)
{
    c = saturate(c * exp2(kExp));
    c = saturate(c + kCon * (c * c * (3.0 - 2.0 * c) - c));
    float mx = Max3(c), mn = Min3(c);
    float L = 0.5 * (mx + mn), d = mx - mn;
    if (d > 1e-6)
    {
        float S = L > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
        float S2 = saturate(S * (1.0 + kSat));
        c = L + (S2 / S) * (c - L);
    }
    return saturate(c);
}
)";

// captura -> quadro inteiro linear (branco de papel = 1); quadro inteiro -> tamanho da rede: original e proxy
inline const char* Prepara = R"(
cbuffer C : register(b0) { uint fw; uint fh; uint dw; uint dh; uint hdr; float escala; uint ponte; float exposicao;
                           float preLuz; float preSombra; float preLimiar; float preLimiarSombra; float preCor; uint passa;
                           uint prePad1; uint prePad2; };
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
RWTexture2D<float4> dstProxy : register(u1);
SamplerState s : register(s0);

[numthreads(8, 8, 1)] void CSInteiro(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= fw || p.y >= fh) return;
    float4 c = src.Load(int3(p, 0));
    // reproducao de gravacao (--reproduz): a entrada ja e o quadro do overlay (luz de cena), sem conversao. No sanduiche o
    // gravado esta na tela SDR equivalente e o quadro inteiro guarda luz linear: volta pela inversa (CSReduz refaz o joelho)
    if (passa) { dst[p] = float4(hdr != 0 && ponte == 1 ? SoftKneeInv(c.rgb) : c.rgb, 1.0); return; }
    // HDR: scRGB dividido pelo branco de papel; SDR: sRGB -> linear (ponte 3: e desfaz a curva do jogo, luz de cena)
    float3 l = hdr ? max(c.rgb, 0.0) / escala : Linear(saturate(c.rgb));
    float folga = 1.0;
    if (ponte == 3)
    {
        if (hdr)
        {
            // HDR: o quadro vira uma tela SDR equivalente pelo SoftKnee (ombro ate o pico), e dela a luz de cena pela
            // inversa da curva; a folga guarda o que passou do teto da inversa (realces acima de ~2,7x o branco),
            // devolvida na saida para nao cortar o pico HDR
            const float3 cena = FilmicaInv(SoftKnee(l));
            folga = max(Lum(l), 1e-6) / max(Lum(SoftKneeInv(Filmica(cena))), 1e-6);
            l = cena;
        }
        else
            l = FilmicaInv(l);
    }
    // HDR pela ponte SoftKnee (a automatica em HDR): a cadeia inteira (rede, edicao, acumulo, ajustes) trabalha na tela SDR
    // equivalente que a rede ve, e so a saida volta ao HDR (sanduiche, como a ponte 3). Antes a edicao era aplicada como
    // razao de luz sobre o HDR expandido: nos realces (o ombro da curva) ela ficava menor do que a rede pretendia (medido
    // contra a NVIDIA: 43,8 dB em HDR contra 47,4 em SDR). O quadro inteiro guarda a luz linear; o joelho e a folga sao
    // feitos na leitura (Sanduiche, no CSReduz e no CSCompoe).
    // (luzes/sombras/cor antes da rede: no CSReduz e no resultado do CSCompoe; o quadro inteiro fica intacto)
    dst[p] = float4(l, folga);
}

[numthreads(8, 8, 1)] void CSReduz(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= dw || p.y >= dh) return;
    float2 esc = float2(fw, fh) / float2(dw, dh);
    int n = clamp((int)ceil(max(esc.x, esc.y)), 1, 6);
    float2 base = float2(p) * esc;
    const bool sanduiche = hdr != 0 && ponte == 1; // a cadeia trabalha na tela SDR equivalente (joelho por amostra)
    float4 a = 0;
    [loop] for (int j = 0; j < n; ++j)
        [loop] for (int i = 0; i < n; ++i)
        {
            float2 q = base + (float2(i, j) + 0.5) * esc / n;
            float4 t = src.SampleLevel(s, q / float2(fw, fh), 0);
            float folga;
            if (sanduiche) t.rgb = Sanduiche(t.rgb, folga);
            a += t;
        }
    float3 o = a.rgb / (n * n);
    o = PreAjuste(o, TelaDe(o, hdr != 0 && !sanduiche, ponte == 3), preLuz, preSombra, preLimiar, preLimiarSombra, preCor);
    dst[p] = float4(o, 1.0);
    // exposicao: o ponto de operacao da rede (o proxy ve o quadro x exposicao; a composicao desfaz)
    dstProxy[p] = float4(Proxy(o * exposicao, sanduiche ? 0u : ponte), 1.0);
}
)";

// quadro inteiro + forca * edicao (ampliada por Catmull-Rom) -> gradacao do modelo -> tela
// modo: 0 rede, 1 original, 2 comparacao (esquerda original | direita rede), 3 so a edicao (x4), 4 movimento,
// 5 saida crua da ultima resposta (o quadro que a rede viu + a edicao dela)
inline const char* Compoe = R"(
cbuffer C : register(b0) { uint fw; uint fh; uint dw; uint dh; float forca; uint modo; float divisao; uint usaEdicao;
                           uint hdr; float escala; float kExp; float kCon; float kSat; uint grada; uint filmica; uint sanduiche;
                           float preLuz; float preSombra; float preLimiar; float preLimiarSombra; float preCor; uint pad2;
                           uint pad3; uint pad4;
                           float satR; float satY; float satG; float satC; float satB; float satM;
                           float luzR; float luzY; float luzG; float luzC; float luzB; float luzM;
                           uint usaMatiz; uint temCrua; uint luz; uint padA1; };
Texture2D<float4> inteiro : register(t0);
Texture2D<float4> fed : register(t1);
Texture2D<float4> edicao : register(t2);
Texture2D<float4> mov : register(t3);
Texture2D<float4> origT0 : register(t4); // modo 5: o quadro que a rede viu (t0), no tamanho da rede
Texture2D<float4> cruaT0 : register(t5); // modo 5: a edicao da resposta no espaco de t0 (comprimido x / (1 + luma))
RWTexture2D<float4> saida : register(u0);
SamplerState s : register(s0);

// Catmull-Rom em 9 amostras bilineares (MJP, dominio publico)
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

// valor de saida: SDR em sRGB (UNORM); HDR em scRGB. A gradacao do modelo roda em valores de tela [0,1]:
// no HDR, no dominio da ponte (SoftKnee + sRGB) e volta. folga: realces HDR acima do teto da curva (ponte 3)
float3 Tela(float3 lin, bool grade, float folga)
{
    if (!hdr)
    {
        float3 c = Srgb(filmica ? Filmica(lin) : lin);
        return grade ? GradacaoModelo(c, kExp, kCon, kSat) : c;
    }
    if (filmica)
    {
        // HDR com a curva do jogo: luz de cena -> tela SDR equivalente -> (gradacao) -> SoftKnee inverso -> scRGB
        float3 d = Filmica(lin);
        if (grade) d = Linear(GradacaoModelo(Srgb(d), kExp, kCon, kSat));
        return SoftKneeInv(d) * folga * escala;
    }
    if (sanduiche)
    {
        // ponte SoftKnee em HDR: a cadeia esta na tela SDR equivalente; volta ao HDR pela inversa exata + folga
        float3 d = max(lin, 0.0);
        if (grade) d = Linear(GradacaoModelo(Srgb(saturate(d)), kExp, kCon, kSat));
        return SoftKneeInv(d) * folga * escala;
    }
    if (!grade) return max(lin, 0.0) * escala;
    float3 c = GradacaoModelo(Srgb(SoftKnee(lin)), kExp, kCon, kSat);
    return SoftKneeInv(Linear(c)) * escala;
}

// cor por matiz depois da rede, em seis eixos (0 vermelho, 1 amarelo, 2 verde, 3 ciano, 4 azul, 5 magenta): saturacao e
// brilho de cada faixa. Os pesos sao triangulos entre eixos vizinhos (somam 1), e o brilho pesa pelo croma: cinza e
// branco nao mudam. Recebe luz linear (primarias Rec.709, branco de papel = 1): SDR, HDR e luz de cena da ponte 3; no
// sanduiche o CSCompoe desfaz o joelho antes, para o ajuste valer igual nos realces HDR.
float3 CorPorMatiz(float3 c)
{
    c = max(c, 0.0);
    const float mx = Max3(c), d = mx - Min3(c);
    if (d <= 1e-6 * max(mx, 1e-6)) return c;
    float h = mx == c.r ? (c.g - c.b) / d : (mx == c.g ? 2.0 + (c.b - c.r) / d : 4.0 + (c.r - c.g) / d);
    h = h < 0.0 ? h + 6.0 : h;
    const float sat[6] = {satR, satY, satG, satC, satB, satM};
    const float luz[6] = {luzR, luzY, luzG, luzC, luzB, luzM};
    float s = 0, l = 0;
    [unroll] for (int k = 0; k < 6; ++k)
    {
        float dist = abs(h - k);
        dist = min(dist, 6.0 - dist);
        const float w = saturate(1.0 - dist);
        s += w * sat[k];
        l += w * luz[k];
    }
    const float Y = Lum(c);
    c = Y + (c - Y) * max(1.0 + s, 0.0);
    c *= max(1.0 + l * (d / max(mx, 1e-6)), 0.0);
    return max(c, 0.0);
}

[numthreads(8, 8, 1)] void CSCompoe(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= fw || p.y >= fh) return;
    const float4 oi = inteiro.Load(int3(p, 0));
    float3 o = oi.rgb;
    float folga = oi.a;
    if (sanduiche) o = Sanduiche(oi.rgb, folga);
    float3 e = 0;
    if (usaEdicao)
    {
        if (dw == fw && dh == fh)
            e = edicao.Load(int3(p, 0)).rgb;
        else
            e = CatmullRom(edicao, (float2(p) + 0.5) / float2(fw, fh), float2(dw, dh));
    }
    // o resultado parte do quadro com o mesmo ajuste que a rede viu; o lado "original" fica intacto
    const float3 base = PreAjuste(o, TelaDe(o, hdr != 0 && sanduiche == 0, filmica != 0), preLuz, preSombra, preLimiar,
                                  preLimiarSombra, preCor);
    // a edicao na luz dela (CodificaLuz) -- sem edicao (e = 0) o pixel fica igual
    float3 r = AplicaLuz(base, e, luz, forca);
    if (luz != 0) e = r - base; // "So a edicao" mostra a diferenca em todos os modos
    if (usaMatiz) r = sanduiche ? SoftKnee(CorPorMatiz(SoftKneeInv(r))) : CorPorMatiz(r);
    bool orig = modo == 1 || (modo == 2 && (float)p.x < divisao * fw);
    float3 c = orig ? Tela(o, false, folga) : Tela(r, grada != 0, folga);
    float hs = hdr ? escala : 1.0;
    if (modo == 3) c = saturate(0.5 + 4.0 * e) * hs;
    if (modo == 4)
    {
        // movimento em cores: matiz = direcao, brilho = tamanho (8 px = cheio)
        float2 v = mov.SampleLevel(s, (float2(p) + 0.5) / float2(fw, fh), 0).xy;
        float a = atan2(v.y, v.x) / 6.2831853 + 0.5;
        float3 h = saturate(abs(frac(a + float3(0, 2.0 / 3, 1.0 / 3)) * 6 - 3) - 1);
        c = lerp(float3(0.5, 0.5, 0.5), h, saturate(length(v) / 8.0)) * hs;
    }
    if (modo == 5 && temCrua)
    {
        // a resposta como a rede a deu, no quadro que ela viu: separa o que e da rede do que e do movimento (fluxo,
        // acumulo, grade). Com atraso, ela fica para tras do quadro atual em movimento.
        const float2 uv = (float2(p) + 0.5) / float2(fw, fh);
        const bool igual = dw == fw && dh == fh;
        const float3 ot = max(igual ? origT0.Load(int3(p, 0)).rgb : CatmullRom(origT0, uv, float2(dw, dh)), 0.0);
        const float3 ec = igual ? cruaT0.Load(int3(p, 0)).rgb : CatmullRom(cruaT0, uv, float2(dw, dh));
        float3 cc = ot / (1.0 + Lum(ot)) + ec;
        // o mesmo limite da volta a luz linear do Temporal (a luma comprimida perto de 1 explodiria)
        const float lo = Lum(ot) * 4.0 + 4.0, lim = lo / (1.0 + lo), lc = Lum(cc);
        if (lc > lim) cc *= lim / lc;
        c = Tela(max(cc / max(1.0 - Lum(cc), 1e-3), 0.0), grada != 0, 1.0);
    }
    if (modo == 2 && abs((float)p.x - divisao * fw) < 1.0) c = float3(1, 1, 0) * hs;
    saida[p] = float4(c, 1.0);
}
)";

// Nitidez depois da composicao, pelo algoritmo do CAS (Contrast Adaptive Sharpening, AMD FidelityFX, licenca MIT;
// escrito aqui a partir do algoritmo publicado): por canal, o peso dos 4 vizinhos em cruz e negativo e cai onde o 3x3 ja
// tem contraste alto (amp = sqrt(min(min, 2 - max) / max) com min e max da cruz somados aos do 3x3) -- reforca o detalhe
// fraco sem halo nas bordas fortes. nitidez: 0 = sem efeito, 1 = o maximo do CAS (pico -1/5); entre os dois o pico do
// CAS (-1 / lerp(8, 5, n)) vezes n, para comecar do zero sem salto. SDR: nos valores sRGB da saida (o CAS espera [0,1]);
// HDR: em c / (1 + c) por canal (c em brancos de papel) -- o realce acima do branco nao satura.
// So no lado da rede: o modo Original, a metade original da comparacao e os modos de diagnostico ficam como estao.
inline const char* Nitidez = R"(
cbuffer C : register(b0) { uint w; uint h; uint hdr; float nitidez; uint modo; float divisao; float escala; uint pad; };
Texture2D<float4> ent : register(t0);
RWTexture2D<float4> sai : register(u0);
float3 Le(int2 q)
{
    float3 c = ent.Load(int3(clamp(q, int2(0, 0), int2(w, h) - 1), 0)).rgb;
    if (hdr)
    {
        c = max(c, 0.0) / escala;
        c = c / (1.0 + c);
    }
    return saturate(c);
}
[numthreads(8, 8, 1)] void CSNitidez(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    const float4 c4 = ent.Load(int3(p, 0));
    const bool rede = modo == 0 || modo == 5 || (modo == 2 && (float)p.x >= divisao * w + 1.0);
    if (!rede)
    {
        sai[p] = c4;
        return;
    }
    const int2 q = int2(p);
    // a b c
    // d e f
    // g k i
    const float3 a = Le(q + int2(-1, -1)), b = Le(q + int2(0, -1)), c = Le(q + int2(1, -1));
    const float3 d = Le(q + int2(-1, 0)), e = Le(q), f = Le(q + int2(1, 0));
    const float3 g = Le(q + int2(-1, 1)), k = Le(q + int2(0, 1)), i = Le(q + int2(1, 1));
    float3 mn = min(min(min(d, e), min(f, b)), k);
    float3 mx = max(max(max(d, e), max(f, b)), k);
    mn += min(mn, min(min(a, c), min(g, i)));
    mx += max(mx, max(max(a, c), max(g, i)));
    const float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-5)));
    const float n = saturate(nitidez);
    const float3 wv = amp * (-n / lerp(8.0, 5.0, n));
    const float3 r = saturate((b * wv + d * wv + f * wv + k * wv + e) / (1.0 + 4.0 * wv));
    if (!hdr)
    {
        sai[p] = float4(r, c4.a);
        return;
    }
    // HDR: so a diferenca volta a luz linear e soma no valor original (o scRGB tem canais negativos fora do sRGB, que
    // o dominio [0,1] cortaria; numa area lisa a diferenca e zero e o pixel fica identico)
    const float3 r1 = min(r, 0.999), e1 = min(e, 0.999);
    sai[p] = float4(c4.rgb + (r1 / (1.0 - r1) - e1 / (1.0 - e1)) * escala, c4.a);
}
)";

// captura de buffer: a edicao que o jogo soma no quadro seguinte, no espaco comprimido (C(x) = x / (1 + luma), o mesmo
// do Temporal e do modulo do jogo): C(saida) - C(cor do jogo); na luz logaritmica e na mista, CodificaLuz
// No alfa vai log2 da distancia da camera do pixel: o jogo so reprojeta a edicao de um ponto para a MESMA superficie
// (onde o objeto acabou de sair, o vetor aponta para o objeto; sem o teste, a edicao dele virava rastro no fundo).
inline const char* Exporta = R"(
cbuffer C : register(b0) { uint w; uint h; uint profW; uint profH; float perto; float longe; uint invertida; uint infinita;
                           float tanY; float aspecto; uint luz; uint pad1; };
Texture2D<float4> cor : register(t0);   // a cor que o jogo mandou (linear x exposicao)
Texture2D<float4> saida : register(t1); // o resultado no mesmo espaco
Texture2D<float> prof : register(t2);   // a profundidade que o jogo mandou
RWTexture2D<float4> edicao : register(u0);
float3 Comprime(float3 x) { return x / (1.0 + max(Lum(x), 0.0)); }
static const float CEU = 1e6;
// profundidade do jogo -> distancia da camera (a mesma conta no overlay e no jogo: Fluxo/DoJogo, Exporta, CSCor)
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
// mesma superficie: distancias relativas perto (as duas no ceu tambem)
float Mesma(float za, float zb)
{
    if (za >= CEU * 0.5 && zb >= CEU * 0.5) return 1.0;
    return 1.0 - smoothstep(0.04, 0.15, abs(za - zb) / max(min(za, zb), 1e-6));
}
// Z da camera -> distancia RADIAL (do olho ao ponto), com o FOV que o jogo passa ao FSR. O Z de um ponto fora do centro
// muda quando a camera gira (Z = distancia x cosseno do angulo): girando rapido, ~70% da tela parecia outra superficie e
// a edicao vinha dos vizinhos (o letreiro do Gears pintado no carro). A distancia radial nao muda num giro. Sem FOV, Z.
float Radial(float z, float2 uv)
{
    if (tanY <= 0.0 || z >= CEU * 0.5) return z;
    const float2 n = (uv * 2.0 - 1.0) * float2(tanY * aspecto, tanY);
    return z * sqrt(1.0 + dot(n, n));
}
[numthreads(8, 8, 1)] void CSExporta(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    const float3 o = max(cor.Load(int3(p, 0)).rgb, 0.0);
    float3 r = max(saida.Load(int3(p, 0)).rgb, 0.0);
    r = all(isfinite(r)) ? r : o;
    const uint2 pp = min(uint2((p + 0.5) * float2(profW, profH) / float2(w, h)), uint2(profW - 1, profH - 1));
    const float z = Radial(Dist(prof.Load(int3(pp, 0))), (p + 0.5) / float2(w, h));
    // luz linear: a diferenca no espaco comprimido; logaritmica e mista: CodificaLuz
    const float3 ed = luz != 0 ? CodificaLuz(o, r, luz) : Comprime(r) - Comprime(o);
    edicao[p] = float4(ed, log2(max(z, 1e-6)));
}
)";

// guias do historico da propria rede (DLSS5_TEMPORAL_MODE=1, LmxxfNrSetHistoryGuides): o runtime traz a resposta anterior
// da rede ate este quadro pelo movimento e a da como a entrada de historico dela (a mistura e a cabeca de historico do
// bloco 70 que decide). modo 0: parado (movimento zero); 1: a textura (xy = pixels da rede ate o quadro do envio anterior,
// w < 0,5 = o caminho saiu da imagem); 2: sem historico (NaN: o runtime descarta o pixel). Profundidade: 1 / (1 + distancia)
// (maior = mais perto), so com a distancia do jogo.
inline const char* Guias = R"(
cbuffer C : register(b0) { uint w; uint h; uint modo; uint temDist; };
Texture2D<float4> mov : register(t0);
Texture2D<float> dist : register(t1);
RWTexture2D<float2> guiaMov : register(u0);
RWTexture2D<float> guiaProf : register(u1);
[numthreads(8, 8, 1)] void CSGuias(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    const float nan = asfloat(0x7fc00000u);
    float2 v = float2(0, 0);
    if (modo == 1)
    {
        const float4 d = mov.Load(int3(p, 0));
        v = d.w >= 0.5 && all(isfinite(d.xy)) ? d.xy : float2(nan, nan);
    }
    else if (modo != 0)
        v = float2(nan, nan);
    guiaMov[p] = v;
    guiaProf[p] = temDist ? 1.0 / (1.0 + max(dist.Load(int3(p, 0)), 0.0)) : 0.0;
}
)";

} // namespace ov::hlsl
