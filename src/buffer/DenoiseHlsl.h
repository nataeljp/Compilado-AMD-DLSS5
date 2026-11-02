// Denoise fora da rede: a ideia do NRD da NVIDIA (RELAX: reconstrucao, acumulacao temporal, a-trous guiado), com o grao da rede
// no lugar do ruido dos raios. O NRD manda separar o material antes de filtrar ("All materials must be de-modulated before
// denoising"): aqui a imagem do jogo e o material e a edicao da rede e o sinal ruidoso. Em uso: hdr = 2, no overlay, sobre a
// edicao (com sinal, log2(saida / cor)) no tamanho da rede, onde o grao tem 1 pixel, com a imagem do jogo so como guia (temGuia).
// Padroes do NRDSettings.h: fastHistoryClampingSigmaScale 2 (Recorte) e diffusePhiLuminance 2 (Tolerancia).
// Opcao "Processar o jogo junto da rede" (dnJogo): sobre o quadro pronto (hdr 0 ou 1), no overlay. Serve onde o quadro de la vai para a tela:
//  - captura de buffer: dentro do jogo (ClienteBuffer), no proprio quadro, com a edicao ja somada. No overlay ele nao
//    serve: o que sai de la e somado ao quadro SEGUINTE, e o ruido de um quadro nao e o do outro (medido com grao novo a
//    cada quadro, contra a imagem limpa: pior que desligado em erro, cintilacao e bordas);
//  - captura de video e reproducao: no overlay, depois da composicao (o quadro de la e o que vai para a tela).
// Etapas (cada uma so roda com a forca acima de 0):
//  CSNormais: normais (e a inclinacao, no alfa) derivadas da distancia: o jogo nao entrega normais ao FSR.
//  CSReconstrucao: troca so o pixel fora da faixa dos 8 vizinhos da mesma superficie (pontos brancos), por mais de 1,5 desvio:
//    borda, linha e textura sempre tem um vizinho parecido e ficam intactas; a estimativa e a media 5x5 com pesos de
//    distancia, geometria e semelhanca com a vizinhanca.
//  CSAcumulacao: o resultado anterior reprojetado pelo movimento (Catmull-Rom: nao borra), preso a media +- recorte desvios
//    do 3x3 atual em YCoCg (sem rastro). Peso n / (n + 1) pelos quadros acumulados no pixel, ate 95% x forca, vezes a
//    confianca do movimento; onde ela cai (oclusao) o historico sai.
//  CSBordas: a-trous 5x5 (passo 1, 2, 4) que para em borda de profundidade e de normal. Sobre a edicao da rede, a borda de
//    luminancia e so a da imagem do jogo, e a forca nao depende dos quadros acumulados: com a camera parada o grao da rede e o
//    mesmo em todo quadro (correlacao 0,9998 entre quadros seguidos), o tempo nao o tira, e recuar com o historico fazia o grao
//    voltar quando a camera parava (de 0,087 a 0,136 em 38 quadros, com 0,157 desligado). A borda da propria edicao tambem nao
//    guia: o ruido estimado dela sobe onde a rede pos textura, e o filtro levava 3,7 vezes mais detalhe preso a cena do que o
//    que variava de quadro a quadro. Sobre o quadro pronto (sem guia), a luminancia dele guia e o historico reduz o filtro.
// Pesos e misturas em c / (1 + c) por canal na luz linear (c em brancos de papel), nos valores sRGB na captura SDR; na luz
// linear so a diferenca volta (o scRGB tem canais negativos fora do sRGB; sem mudanca, o pixel fica identico). Sem dados
// de raio (hit distance), que um quadro pronto nao tem. Na captura de video nao ha profundidade: so a luminancia guia.
#pragma once

namespace dlss5ov {

// raiz: 24 constantes (b0), SRV t0..t7, UAV u0..u2
constexpr unsigned kDnConstantes = 24, kDnSrvs = 8, kDnUavs = 3;
struct ConstantesDenoise
{
    unsigned w, h, hdr;
    float escala, divisao, forca, sigma;
    unsigned passo, temHist, histValida, temGeo, grao, profW, profH, movW, movH;
    float movMulX, movMulY, tanY, aspecto;
    unsigned temGuia;
    float tolGuia;
    unsigned pad1, pad2;
};
static_assert(sizeof(ConstantesDenoise) == kDnConstantes * 4, "constantes: o mesmo layout do cbuffer do denoise");

inline const char* const kDenoiseHlsl = R"(
cbuffer C : register(b0)
{
    uint w; uint h; uint hdr; float escala;
    float divisao; float forca; float sigma; uint passo;
    uint temHist; uint histValida; uint temGeo; uint grao;
    uint profW; uint profH; uint movW; uint movH;
    float movMulX; float movMulY; float tanY; float aspecto;
    uint temGuia; float tolGuia; uint pad1; uint pad2;
};
Texture2D<float4> ent : register(t0);
Texture2D<float> prof : register(t1);
Texture2D<float4> hist : register(t2);
Texture2D<float2> mov : register(t3);
Texture2D<float> conf : register(t4);
Texture2D<float4> normais : register(t5);
Texture2D<float> histN : register(t6);  // quadros acumulados de cada pixel / 64 (R8: o historico em R11G11B10 nao tem alfa)
Texture2D<float4> guia : register(t7);  // a imagem do jogo no tamanho da rede: limpa, diz onde ha borda de verdade
RWTexture2D<float4> sai : register(u0);
RWTexture2D<float4> histNovo : register(u1);
RWTexture2D<float> histNNovo : register(u2);
int PassoGrao() { return clamp(int(grao), 1, 4); }
bool Rede(uint2 p) { return divisao < 0.0 || (float)p.x >= divisao * w + 1.0; }
int2 Prende(int2 q) { return clamp(q, int2(0, 0), int2(w, h) - 1); }
float3 Comp(float3 c)
{
    if (hdr == 2)
        return c; // a edicao da rede: ja com sinal e sem realce
    if (hdr)
    {
        c = max(c, 0.0) / escala;
        c = c / (1.0 + c);
    }
    return saturate(c);
}
float3 Inv(float3 r)
{
    r = min(r, 0.999);
    return r / (1.0 - r) * escala;
}
float3 Le(int2 q) { return Comp(ent.Load(int3(Prende(q), 0)).rgb); }
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
uint2 Escala(int2 q, uint2 t) { return min(uint2((float2(Prende(q)) + 0.5) * float2(t) / float2(w, h)), t - 1); }
void Grava(uint2 p, float4 c4, float3 r, float3 e)
{
    sai[p] = hdr == 1 ? float4(c4.rgb + Inv(r) - Inv(e), c4.a) : float4(r, c4.a);
}
float3 Limita(float3 c) { return hdr == 2 ? c : saturate(c); }
// log2 da luminancia: a mesma razao de luz conta igual no escuro e no claro. Em c / (1 + c) as bordas abaixo de 0,1 (metade da
// imagem) cabiam na tolerancia, e a edicao atravessava
float LumaGuia(int2 q)
{
    return log2(max(Luma(max(guia.Load(int3(Prende(q), 0)).rgb, 0.0)), 1.0 / 256.0));
}
// a borda da imagem do jogo: a edicao nao atravessa
float PesoGuia(float lg, int2 q) { return temGuia ? exp(-abs(LumaGuia(q) - lg) / tolGuia) : 1.0; }

// ---- geometria (so com a profundidade do jogo) ----
float ZProf(int2 q) { return min(prof.Load(int3(clamp(q, int2(0, 0), int2(profW, profH) - 1), 0)), 1e6); }
float3 Ponto(int2 q, float z)
{
    const float2 uv = (float2(q) + 0.5) / float2(profW, profH);
    const float ty = tanY > 0.0 ? tanY : 0.577; // sem o FOV do jogo: 60 graus (so compara normais vizinhas)
    return normalize(float3((uv * 2.0 - 1.0) * float2(ty * aspecto, ty), 1.0)) * z;
}
[numthreads(8, 8, 1)] void CSNormais(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= profW || p.y >= profH) return;
    const int2 q = int2(p);
    const float zc = ZProf(q), zl = ZProf(q - int2(1, 0)), zr = ZProf(q + int2(1, 0)), zu = ZProf(q - int2(0, 1)), zd = ZProf(q + int2(0, 1));
    const float dl = abs(zl - zc), dr = abs(zr - zc), du = abs(zu - zc), dd = abs(zd - zc);
    // em cada eixo o lado mais perto: o que nao atravessa uma borda
    const float3 pc = Ponto(q, zc);
    const float3 dx = dr < dl ? Ponto(q + int2(1, 0), zr) - pc : pc - Ponto(q - int2(1, 0), zl);
    const float3 dy = dd < du ? Ponto(q + int2(0, 1), zd) - pc : pc - Ponto(q - int2(0, 1), zu);
    float3 n = cross(dx, dy);
    const float l = length(n);
    n = l > 1e-12 ? n / l : float3(0.0, 0.0, -1.0);
    sai[p] = float4(n, max(min(dl, dr), min(du, dd)));
}
struct Geo
{
    float z, incl;
    float3 n;
};
Geo GeoDe(int2 q)
{
    Geo g;
    g.z = 0.0, g.incl = 0.0, g.n = 0.0;
    if (temGeo)
    {
        const int3 r = int3(Escala(q, uint2(profW, profH)), 0);
        const float4 v = normais.Load(r);
        g.z = min(prof.Load(r), 1e6), g.n = v.xyz, g.incl = v.a;
    }
    return g;
}
// a mesma superficie: a distancia cabe na inclinacao ate ali e a normal e parecida
float PesoGeo(Geo c, int2 q, float dist)
{
    if (!temGeo) return 1.0;
    const Geo o = GeoDe(q);
    const float tol = c.incl * dist * float(profW) / float(w) + 0.01 * c.z + 1e-4;
    return exp(-abs(c.z - o.z) / tol) * pow(saturate(dot(c.n, o.n)), 32.0);
}

[numthreads(8, 8, 1)] void CSReconstrucao(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    const float4 c4 = ent.Load(int3(p, 0));
    if (!Rede(p))
    {
        sai[p] = c4;
        return;
    }
    const int2 q = int2(p);
    const float3 e = Comp(c4.rgb);
    const float lc = Luma(e);
    const Geo gc = GeoDe(q);
    const int sg = PassoGrao();
    const float lg = temGuia ? LumaGuia(q) : 0.0;
    float m = 0, m2 = 0, nv = 0, lmin = 1e9, lmax = -1e9;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i)
            if (i || j)
            {
                const float l = Luma(Le(q + int2(i, j) * sg));
                const float wz = PesoGeo(gc, q + int2(i, j) * sg, length(float2(i, j)) * sg) * PesoGuia(lg, q + int2(i, j) * sg);
                m += wz * l, m2 += wz * l * l, nv += wz;
                if (wz > 0.1)
                    lmin = min(lmin, l), lmax = max(lmax, l);
            }
    if (nv < 1e-3 || lmax < lmin)
    {
        sai[p] = c4;
        return;
    }
    m /= nv, m2 /= nv;
    const float d = sqrt(max(m2 - m * m, 0.0)) + 0.004;
    // a distancia para a media tirava detalhe de imagem limpa (rampa de borda do upscaler): so o que sai da faixa dos vizinhos
    const float k = smoothstep(1.5, 3.0, max(lc - lmax, lmin - lc) / d) * saturate(forca);
    if (k <= 0.0)
    {
        sai[p] = c4;
        return;
    }
    float3 s = 0;
    float ws = 0;
    for (int j = -2; j <= 2; ++j)
        for (int i = -2; i <= 2; ++i)
            if (i || j)
            {
                const float3 x = Le(q + int2(i, j) * sg);
                const float dl = (Luma(x) - m) / d, dist = length(float2(i, j));
                const float wt = exp(-0.5 * (dist * dist / 2.25 + dl * dl)) * PesoGeo(gc, q + int2(i, j) * sg, dist * sg) * PesoGuia(lg, q + int2(i, j) * sg);
                s += wt * x, ws += wt;
            }
    Grava(p, c4, ws > 1e-5 ? lerp(e, s / ws, k) : e, e);
}

// o desvio do ruido no pixel, por segundas diferencas: dao zero numa rampa e ao longo de uma borda, e a menor entre as direcoes
// so tem o que aparece em todas. Variancia crescia na rampa que o upscaler deixa em cada borda e o filtro a atravessava (em
// 8K com FSR 4 tirava 11% das bordas de uma imagem limpa). No passo do grao da rede: amostras vizinhas sao manchas diferentes
float Ruido(int2 q)
{
    const int s = PassoGrao();
    float l[3][3];
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i)
            l[j][i] = Luma(Le(q + int2(i - 1, j - 1) * s));
    float hh = 0, vv = 0;
    for (int k = 0; k < 3; ++k)
    {
        const float dh = l[k][0] - 2.0 * l[k][1] + l[k][2], dv = l[0][k] - 2.0 * l[1][k] + l[2][k];
        hh += dh * dh, vv += dv * dv;
    }
    const float d1 = l[0][0] - 2.0 * l[1][1] + l[2][2], d2 = l[0][2] - 2.0 * l[1][1] + l[2][0];
    // ruido branco: cada diferenca tem variancia 6 sigma^2 e a menor destas, perto de 2,4 sigma^2
    return sqrt(min(min(hh, vv) / 3.0, 2.0 * min(d1 * d1, d2 * d2)) / 2.4);
}
float3 ParaYCoCg(float3 c) { return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }
float3 DeYCoCg(float3 c) { return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
// o historico por interpolacao cubica, em pixels
float3 HistCR(float2 q)
{
    const float2 t = q - 0.5, b = floor(t), f = t - b;
    const float c = 0.75; // cubica de Keys mais aguda que a Catmull-Rom (0,5): compensa o borrado de reamostrar quadro a quadro
    const float2 g = 1.0 - f;
    const float2 w0 = -c * f * g * g, w3 = -c * f * f * g, w1 = 1.0 + f * f * ((c - 3.0) + (2.0 - c) * f),
                 w2 = f * (c + f * ((3.0 - 2.0 * c) + (c - 2.0) * f));
    const float wx[4] = {w0.x, w1.x, w2.x, w3.x}, wy[4] = {w0.y, w1.y, w2.y, w3.y};
    float3 s = 0;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
            s += wx[i] * wy[j] * hist.Load(int3(Prende(int2(b) + int2(i - 1, j - 1)), 0)).rgb;
    return s;
}
[numthreads(8, 8, 1)] void CSAcumulacao(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    const float4 c4 = ent.Load(int3(p, 0));
    const float3 e = Comp(c4.rgb);
    if (!Rede(p))
    {
        sai[p] = c4;
        histNovo[p] = float4(e, 0.0);
        histNNovo[p] = 0.0;
        return;
    }
    const int2 q = int2(p);
    const int sg = PassoGrao();
    const uint2 tm = uint2(movW, movH);
    const float2 ant = float2(p) + 0.5 + mov.Load(int3(Escala(q, tm), 0)) * float2(movMulX, movMulY);
    const float cf = saturate(conf.Load(int3(Escala(q, tm), 0)));
    float n = 0;
    float3 r = e;
    if (histValida && all(ant >= 0.0) && all(ant < float2(w, h)) && cf >= 0.25)
    {
        float3 m = 0, m2 = 0;
        for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i)
            {
                const float3 x = ParaYCoCg(Le(q + int2(i, j) * sg));
                m += x, m2 += x * x;
            }
        m /= 9.0, m2 /= 9.0;
        const float3 d = sqrt(max(m2 - m * m, 0.0)) * sigma + 0.002;
        const float3 hb = ParaYCoCg(Limita(HistCR(ant)));
        const float3 hc = clamp(hb, m - d, m + d);
        n = histN.Load(int3(Prende(int2(ant)), 0)) * 64.0;
        // o que o recorte teve de mover nao era o mesmo ponto: o historico dele conta menos
        if (length((hb - hc) / d) > 1.0)
            n *= 0.5;
        // reamostrar o historico quadro a quadro borra: onde ele difere do atual mais do que o ruido explica e detalhe, e fica
        // o atual (e o historico dali recomeca, sem acumular o borrado)
        const float ruido = Ruido(q);
        const float det = 1.0 - smoothstep(1.0, 2.0, abs(hc.x - ParaYCoCg(e).x) / (2.0 * sigma * ruido + 0.002));
        n *= det;
        const float a = min(n / (n + 1.0), 0.95 * saturate(forca)) * cf;
        r = Limita(lerp(e, DeYCoCg(hc), a));
    }
    histNovo[p] = float4(r, 0.0);
    histNNovo[p] = min(n + 1.0, 64.0) / 64.0;
    Grava(p, c4, r, e);
}

[numthreads(8, 8, 1)] void CSBordas(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    const float4 c4 = ent.Load(int3(p, 0));
    if (!Rede(p))
    {
        sai[p] = c4;
        return;
    }
    const int2 q = int2(p);
    const float3 e = Comp(c4.rgb);
    const float lc = Luma(e);
    const Geo gc = GeoDe(q);
    const float tol = temGuia ? 1.0 : sigma * Ruido(q) + 0.002;
    const float lg = temGuia ? LumaGuia(q) : 0.0;
    static const float h5[3] = {0.375, 0.25, 0.0625};
    float3 s = 0;
    float ws = 0;
    for (int j = -2; j <= 2; ++j)
        for (int i = -2; i <= 2; ++i)
        {
            const int2 o = int2(i, j) * int(passo);
            const float3 x = Le(q + o);
            const float wt = h5[abs(i)] * h5[abs(j)] * (temGuia ? 1.0 : exp(-abs(Luma(x) - lc) / tol)) * PesoGeo(gc, q + o, length(float2(o))) * PesoGuia(lg, q + o);
            s += wt * x, ws += wt;
        }
    const float n = temHist ? histN.Load(int3(p, 0)) * 64.0 : 0.0;
    const float f = saturate(forca);
    // mistura linear com o original mal muda a parte de baixo do controle: 1 - (1 - f)^2
    Grava(p, c4, ws > 1e-5 ? lerp(e, s / ws, temGuia ? f * (2.0 - f) : f / (1.0 + 0.25 * n)) : e, e);
}
)";

} // namespace dlss5ov
