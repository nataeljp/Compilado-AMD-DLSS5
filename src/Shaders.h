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
// 3 luz de cena sem corte (o codificador do runtime aplica o joelho e o sRGB da NVIDIA)
float3 Proxy(float3 o, uint ponte)
{
    if (ponte == 1) return SoftKnee(o);
    if (ponte == 2) return AcesFittedAP1(o);
    if (ponte == 3) return max(o, 0.0);
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
    // reproducao de gravacao (--reproduz): a entrada ja e o quadro do overlay (luz de cena), sem conversao
    if (passa) { dst[p] = float4(c.rgb, 1.0); return; }
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
    // (luzes/sombras/cor antes da rede: no CSReduz e no resultado do CSCompoe; o quadro inteiro fica intacto)
    dst[p] = float4(l, folga);
}

[numthreads(8, 8, 1)] void CSReduz(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= dw || p.y >= dh) return;
    float2 esc = float2(fw, fh) / float2(dw, dh);
    int n = clamp((int)ceil(max(esc.x, esc.y)), 1, 6);
    float2 base = float2(p) * esc;
    float4 a = 0;
    [loop] for (int j = 0; j < n; ++j)
        [loop] for (int i = 0; i < n; ++i)
        {
            float2 q = base + (float2(i, j) + 0.5) * esc / n;
            a += src.SampleLevel(s, q / float2(fw, fh), 0);
        }
    float3 o = a.rgb / (n * n);
    o = PreAjuste(o, TelaDe(o, hdr != 0, ponte == 3), preLuz, preSombra, preLimiar, preLimiarSombra, preCor);
    dst[p] = float4(o, 1.0);
    // exposicao: o ponto de operacao da rede (o proxy ve o quadro x exposicao; a composicao desfaz)
    dstProxy[p] = float4(Proxy(o * exposicao, ponte), 1.0);
}
)";

// quadro inteiro + forca * edicao (ampliada por Catmull-Rom) -> gradacao do modelo -> tela
// modo: 0 rede, 1 original, 2 comparacao (esquerda original | direita rede), 3 so a edicao (x4), 4 movimento
inline const char* Compoe = R"(
cbuffer C : register(b0) { uint fw; uint fh; uint dw; uint dh; float forca; uint modo; float divisao; uint usaEdicao;
                           uint hdr; float escala; float kExp; float kCon; float kSat; uint grada; uint filmica; uint pad1;
                           float preLuz; float preSombra; float preLimiar; float preLimiarSombra; float preCor; uint pad2;
                           uint pad3; uint pad4; };
Texture2D<float4> inteiro : register(t0);
Texture2D<float4> fed : register(t1);
Texture2D<float4> edicao : register(t2);
Texture2D<float4> mov : register(t3);
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
    if (!grade) return max(lin, 0.0) * escala;
    float3 c = GradacaoModelo(Srgb(SoftKnee(lin)), kExp, kCon, kSat);
    return SoftKneeInv(Linear(c)) * escala;
}

[numthreads(8, 8, 1)] void CSCompoe(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= fw || p.y >= fh) return;
    const float4 oi = inteiro.Load(int3(p, 0));
    float3 o = oi.rgb;
    float3 e = 0;
    if (usaEdicao)
    {
        if (dw == fw && dh == fh)
            e = edicao.Load(int3(p, 0)).rgb;
        else
            e = CatmullRom(edicao, (float2(p) + 0.5) / float2(fw, fh), float2(dw, dh));
    }
    // o resultado parte do quadro com o mesmo ajuste que a rede viu; o lado "original" fica intacto
    float3 r = PreAjuste(o, TelaDe(o, hdr != 0, filmica != 0), preLuz, preSombra, preLimiar, preLimiarSombra, preCor) +
               forca * e;
    bool orig = modo == 1 || (modo == 2 && (float)p.x < divisao * fw);
    float3 c = orig ? Tela(o, false, oi.a) : Tela(r, grada != 0, oi.a);
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
    if (modo == 2 && abs((float)p.x - divisao * fw) < 1.0) c = float3(1, 1, 0) * hs;
    saida[p] = float4(c, 1.0);
}
)";

} // namespace ov::hlsl
