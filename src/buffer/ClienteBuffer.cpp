#include "ClienteBuffer.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

#pragma comment(lib, "d3dcompiler.lib")

namespace dlss5ov {

namespace {
// Os tres passes do jogo. C(x) = x / (1 + luma) e E(y) = y / (1 - luma): os mesmos do overlay (Temporal.cpp).
const char* kHlsl = R"(
static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);
float Lum(float3 c) { return dot(c, LUMA); }
float3 Comprime(float3 x) { return x / (1.0 + max(Lum(x), 0.0)); }
float3 Expande(float3 c) { return c / max(1.0 - Lum(c), 1e-3); }
float3 Linear(float3 c) { return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 Srgb(float3 c) { c = saturate(c); return (c <= 0.0031308) ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
cbuffer K : register(b0)
{
    uint corW, corH, movW, movH;
    uint profW, profH, exporta, aplica;
    float movEscX, movEscY, preExp, escExp; // movEsc: MV do jogo -> pixels da cor
    uint temExp, srgb, invertida, infinita;
    float perto, longe;
    uint ativos, zera;  // acumuladores: os que seguem e os que comecam neste quadro (ancora = este quadro)
    uint jExporta;      // acumulador cujo deslocamento vai como movimento exportado (3 = so o deste quadro)
    float tanY, aspecto; // tan(fovY / 2) e largura / altura: distancia radial (0 = sem FOV: Z da camera)
    uint luzLog;        // a edicao lida (edicao) e logaritmica: log2 da razao de luz, nao a diferenca comprimida
    uint movVW, movVH, profVW, profVH; // a regiao valida do movimento e da profundidade do jogo neste quadro
    uint mostra;        // quadros repetidos: a saida e o quadro guardado da ancora + a edicao dela, sem movimento
    uint guarda;        // quadros repetidos: guarda a cor deste quadro (exportado) para mostrar quando a edicao chegar
    float peso;         // mesclado: quanto do processado seguinte (guardadaLe2 + edicao2) entra; 0 = so a ancora
    uint luzLog2;       // a luz da edicao2
};
Texture2D<float2> movJogo : register(t0);
Texture2D<float> profJogo : register(t1);
Texture2D<float> expJogo : register(t2);
Texture2D<float4> edicao : register(t3);
Texture2D<float2> accAnt0 : register(t4); // deslocamento acumulado ate cada ancora (pixels da cor, na resolucao do MV)
Texture2D<float2> accAnt1 : register(t5);
Texture2D<float2> accAnt2 : register(t6);
Texture2D<float2> accEd : register(t7);   // o acumulador da ancora da edicao aplicada (ja deste quadro)
Texture2D<uint> reconLe : register(t8);   // CSRecon: a menor distancia DESTE quadro que cai em cada pixel da ancora
Texture2D<float4> guardadaLe : register(t9); // quadros repetidos: a cor (x exposicao) do quadro da edicao mostrada
Texture2D<float4> guardadaLe2 : register(t10); // mesclado: a cor do processado seguinte
Texture2D<float4> edicao2 : register(t11);     // mesclado: a edicao dele
RWTexture2D<float4> corJogo : register(u0);
RWTexture2D<float4> corSlot : register(u1);
RWTexture2D<float2> movSlot : register(u2);
RWTexture2D<float> profSlot : register(u3);
RWTexture2D<float> expSlot : register(u4);
RWTexture2D<float2> accNovo0 : register(u5);
RWTexture2D<float2> accNovo1 : register(u6);
RWTexture2D<float2> accNovo2 : register(u7);
RWTexture2D<uint> recon : register(u8);   // a mesma, escrita (resolucao da profundidade)
RWTexture2D<float4> guardadaEsc : register(u9); // quadros repetidos: onde a cor deste quadro fica guardada
SamplerState lin : register(s0);

// Com resolucao dinamica o jogo entrega movimento e profundidade numa regiao valida que muda a cada quadro. As texturas
// daqui tem o tamanho maximo (movW x movH, profW x profH) e leem o pixel valido do jogo mais proximo; com a regiao igual
// ao tamanho maximo e o proprio pixel.
float2 MovJogo(uint2 p)
{
    const uint2 v = uint2(movVW, movVH);
    return movJogo.Load(int3(min(uint2((float2(p) + 0.5) * float2(v) / float2(movW, movH)), v - 1), 0));
}
float ProfJogo(int2 q)
{
    const uint2 v = uint2(profVW, profVH);
    return profJogo.Load(int3(min(uint2((float2(q) + 0.5) * float2(v) / float2(profW, profH)), v - 1), 0));
}

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
// a mesma superficie entre o quadro da edicao (1-3 quadros atras) e este: a camera andando em volta do personagem muda
// a distancia dele ~10% nesse tempo (com Mesma, a edicao do personagem caia e vinha dos vizinhos: rastro). So descarta
// o que o movimento nao explica: a origem mais PERTO (o que estava na frente) a partir de 15-35%, mais longe a partir
// de 40-80%.
float MesmaAntes(float zOrigem, float zAgora)
{
    if (zOrigem >= CEU * 0.5 && zAgora >= CEU * 0.5) return 1.0;
    const float rn = max(zAgora - zOrigem, 0.0) / max(zOrigem, 1e-6);
    const float rf = max(zOrigem - zAgora, 0.0) / max(zAgora, 1e-6);
    return 1.0 - max(smoothstep(0.15, 0.35, rn), smoothstep(0.4, 0.8, rf));
}
// Z da camera -> distancia RADIAL (a do alfa da edicao, Shaders.h/Exporta): o Z de um ponto fora do centro muda quando a
// camera gira; com o Z, girando rapido quase toda a tela parecia outra superficie e a edicao vinha dos vizinhos
float Radial(float z, float2 uv)
{
    if (tanY <= 0.0 || z >= CEU * 0.5) return z;
    const float2 n = (uv * 2.0 - 1.0) * float2(tanY * aspecto, tanY);
    return z * sqrt(1.0 + dot(n, n));
}

float Exposicao()
{
    return temExp ? clamp(expJogo.Load(int3(0, 0, 0)) * escExp / max(preExp, 1e-6), 1.0 / 256.0, 256.0) : 1.0;
}

// Movimento acumulado (resolucao do MV): para cada ancora (um quadro exportado), o deslocamento deste quadro ate ela,
// D(p) = mov(p) + D_anterior(p + mov(p)). O overlay recebe o deslocamento ate o quadro que ele processou por ultimo
// (o quadro exportado antes), e a edicao e trazida pela soma exata desde o quadro em que foi feita -- 1, 2 ou 3 quadros
// atras. (Com o movimento de um quadro so, a edicao de 2 quadros atras ficava desalinhada e pulsava.)
[numthreads(8, 8, 1)] void CSAcum(uint2 p : SV_DispatchThreadID)
{
    if (exporta && all(p == 0))
        expSlot[uint2(0, 0)] = Exposicao();
    if (p.x >= movW || p.y >= movH) return;
    float2 m = MovJogo(p) * float2(movEscX, movEscY);
    m = all(isfinite(m)) ? m : 0;
    const float2 uv = ((p + 0.5) * float2(corW, corH) / float2(movW, movH) + m) / float2(corW, corH);
    float2 a[3];
    a[0] = m + accAnt0.SampleLevel(lin, uv, 0);
    a[1] = m + accAnt1.SampleLevel(lin, uv, 0);
    a[2] = m + accAnt2.SampleLevel(lin, uv, 0);
    if (zera & 1) accNovo0[p] = 0; else if (ativos & 1) accNovo0[p] = a[0];
    if (zera & 2) accNovo1[p] = 0; else if (ativos & 2) accNovo1[p] = a[1];
    if (zera & 4) accNovo2[p] = 0; else if (ativos & 4) accNovo2[p] = a[2];
    if (exporta)
        movSlot[p] = jExporta < 3 ? a[jExporta] : m;
}

[numthreads(8, 8, 1)] void CSProf(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= profW || p.y >= profH) return;
    profSlot[p] = ProfJogo(p);
}

// Oclusao como no FSR (profundidade reconstruida): cada ponto deste quadro marca a distancia dele onde estava no quadro
// da edicao (a ancora); a edicao de la so vale para este pixel se nenhum ponto DESTE quadro, mais perto da camera, estava
// no mesmo lugar. Comparar com a distancia gravada na ancora deixava passar a edicao do personagem no chao perto dele
// (simulado na gravacao do Gears: fantasma 0,07 com a regra tolerante, 0,06 com a rigida; 0 assim).
[numthreads(8, 8, 1)] void CSLimpaRecon(uint2 p : SV_DispatchThreadID)
{
    if (p.x < profW && p.y < profH) recon[p] = 0xFFFFFFFFu;
}
[numthreads(8, 8, 1)] void CSRecon(uint2 q : SV_DispatchThreadID)
{
    if (!aplica || q.x >= profW || q.y >= profH) return;
    const float2 uv = (q + 0.5) / float2(profW, profH);
    float2 m = accEd.SampleLevel(lin, uv, 0);
    if (!all(isfinite(m))) return;
    const float z = Radial(Dist(ProfJogo(q)), uv);
    const float2 u = (uv * float2(corW, corH) + m) * float2(profW, profH) / float2(corW, corH);
    const int2 b = int2(floor(u - 0.5));
    [unroll] for (int j = 0; j < 2; ++j)
        [unroll] for (int i = 0; i < 2; ++i)
        {
            const int2 t = b + int2(i, j);
            if (all(t >= 0) && all(t < int2(profW, profH)))
                InterlockedMin(recon[t], asuint(z));
        }
}
// peso da edicao que esta em qAnc (pixels da cor, no quadro da ancora) para um ponto deste quadro a distancia z, com o
// pixel de profundidade pz (folga da inclinacao: num chao rasante o vizinho de 1 pixel ja esta alguns % mais perto)
float NaoOculto(float2 qAnc, float z, uint2 pz)
{
    const float2 u = qAnc * float2(profW, profH) / float2(corW, corH);
    const int2 b = int2(floor(u - 0.5));
    const int2 lim = int2(profW, profH) - 1;
    float zmin = CEU * 4.0;
    [unroll] for (int j = 0; j < 2; ++j)
        [unroll] for (int i = 0; i < 2; ++i)
        {
            const uint v = reconLe.Load(int3(clamp(b + int2(i, j), 0, lim), 0));
            if (v != 0xFFFFFFFFu) zmin = min(zmin, asfloat(v));
        }
    if (zmin >= CEU * 2.0) return 1.0;
    if (z >= CEU * 0.5) return zmin < CEU * 0.5 ? 0.0 : 1.0;
    const float zp = Dist(ProfJogo(pz));
    float g = 0.0;
    [unroll] for (int k = 0; k < 4; ++k)
    {
        const int2 o = int2(k == 0 ? 1 : k == 1 ? -1 : 0, k == 2 ? 1 : k == 3 ? -1 : 0);
        const float zv = Dist(ProfJogo(clamp(int2(pz) + o, 0, lim)));
        if (zv < CEU * 0.5 && zp < CEU * 0.5) g = max(g, abs(zv - zp) / max(zp, 1e-6));
    }
    return 1.0 - smoothstep(0.05 + 2.0 * g, 0.15 + 2.0 * g, max(z - zmin, 0.0) / max(zmin, 1e-6));
}

// a cor (x exposicao) com a edicao do overlay. Luz linear: somada no espaco comprimido, a edicao encolhe ate nenhum
// canal ficar negativo (cortar canal por canal mudava o matiz). Logaritmica: a razao de luz multiplicada (piso 1/512;
// sem edicao o pixel fica igual). Nas duas o brilho para em 4x o original + 4, como no overlay.
float3 Editado(float3 ca, float3 ev, uint log)
{
    if (log)
    {
        float3 y = max(ca + max(ca, 1.0 / 512.0) * (exp2(ev) - 1.0), 0.0);
        const float lo = max(Lum(ca), 0.0) * 4.0 + 4.0, ly = Lum(y);
        if (ly > lo) y *= lo / ly;
        return y;
    }
    const float3 ag = Comprime(ca);
    float tg = 1.0;
    [unroll] for (int i = 0; i < 3; ++i)
        if (ev[i] < 0.0) tg = min(tg, ag[i] / max(-ev[i], 1e-6));
    float3 yg = ag + ev * saturate(tg);
    const float lo = max(Lum(ca), 0.0) * 4.0 + 4.0, limG = lo / (1.0 + lo), lyg = Lum(yg);
    if (lyg > limG) yg *= limG / lyg;
    return max(Expande(yg), 0.0);
}

[numthreads(8, 8, 1)] void CSCor(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= corW || p.y >= corH) return;
    const float4 c4 = corJogo[p];
    float3 c = srgb ? Linear(c4.rgb) : c4.rgb;
    c = all(isfinite(c)) ? c : 0;
    const float e = Exposicao();
    const float3 ce = max(c * e, 0);
    if (exporta)
        corSlot[p] = float4(ce, 1);
    if (guarda)
        guardadaEsc[p] = float4(ce, 1);
    if (mostra)
    {
        // quadro repetido: o quadro guardado da ancora com a edicao feita nele, pixel a pixel (nada trazido pelo movimento);
        // mesclado: misturado com o processado seguinte (tambem com a edicao dele) na proporcao `peso`
        float3 og = Editado(guardadaLe.Load(int3(p, 0)).rgb, edicao.Load(int3(p, 0)).rgb, luzLog);
        if (peso > 0.0)
            og = lerp(og, Editado(guardadaLe2.Load(int3(p, 0)).rgb, edicao2.Load(int3(p, 0)).rgb, luzLog2), peso);
        og /= e;
        corJogo[p] = float4(srgb ? Srgb(og) : og, c4.a);
        return;
    }
    if (!aplica) return;
    // a edicao foi feita no quadro da ancora: este pixel estava em p + D (movimento acumulado desde ela)
    float2 m = accEd.SampleLevel(lin, (p + 0.5) / float2(corW, corH), 0);
    m = all(isfinite(m)) ? m : 0;
    // so a mesma superficie: a edicao leva no alfa log2 da distancia de cada pixel no quadro dela
    const uint2 pp = min(uint2((p + 0.5) * float2(profW, profH) / float2(corW, corH)), uint2(profW - 1, profH - 1));
    const float zc = Radial(Dist(ProfJogo(pp)), (p + 0.5) / float2(corW, corH));
    // fora do quadro (camera girando rapido: conteudo novo entrando pela borda) nao ha edicao para trazer: prender na
    // borda esticava a ultima coluna/linha (faixas). Vai para os vizinhos.
    const float2 q0 = p + 0.5 + m;
    const bool fora = any(q0 < 0.0) || any(q0 > float2(corW, corH));
    const float2 q = clamp(q0, 0.5, float2(corW, corH) - 0.5);
    const float2 b = q - 0.5;
    const int2 i0 = int2(floor(b));
    const float2 fr = b - i0;
    const int2 lim = int2(corW, corH) - 1;
    float3 ed = 0;
    float soma = 0;
    const float livre = fora ? 1.0 : NaoOculto(q0, zc, pp);
    [unroll] for (int k = 0; k < 4; ++k)
    {
        const int2 o = int2(k & 1, k >> 1);
        const float4 v = edicao.Load(int3(clamp(i0 + o, 0, lim), 0));
        const float wv = (o.x ? fr.x : 1.0 - fr.x) * (o.y ? fr.y : 1.0 - fr.y) * MesmaAntes(exp2(v.a), zc) * livre;
        ed += v.rgb * wv;
        soma += wv;
    }
    if (fora)
    {
        // entrou pela borda (no quadro da edicao o ponto estava fora da imagem): nao ha edicao para ele. A da borda
        // (mesma superficie) some em ~48 px de 1080p (escala com a saida); os vizinhos em aneis de raio dobrado viravam
        // faixas verticais
        const float2 dentroQ = max(max(-q0, q0 - float2(corW, corH)), 0.0) * (1080.0 / max(float(corH), 1.0));
        ed = soma > 1e-4 ? ed / soma * exp(-dot(dentroQ, dentroQ) / (48.0 * 48.0)) : 0;
        soma = 1.0;
    }
    else if (soma < 0.05)
    {
        // fundo recem-descoberto (ali, no quadro da edicao, havia outra superficie): os VIZINHOS deste quadro na mesma
        // superficie, cada um com a edicao reprojetada dele (valida: a distancia bate). A faixa e estreita perto da
        // borda; procurar em volta da posicao antiga caia dentro da area que o objeto cobria (centenas de pixels).
        ed = 0;
        soma = 0;
        float achou = 0.0;
        const float2 tamC = float2(corW, corH);
        [loop] for (float r = 4.0; r <= 512.0 && achou < 2.0; r *= 2.0)
        {
            float s1 = 0;
            [unroll] for (int k = 0; k < 16; ++k)
            {
                float2 dir;
                sincos(k * 0.3926991 + r * 0.29, dir.y, dir.x);
                const int2 t = clamp(int2(float2(p) + 0.5 + dir * r), 0, lim);
                const uint2 tp = min(uint2((t + 0.5) * float2(profW, profH) / tamC), uint2(profW - 1, profH - 1));
                const float zt = Radial(Dist(ProfJogo(tp)), (t + 0.5) / tamC);
                const float wz = Mesma(zt, zc);
                if (wz <= 0.0) continue;
                float2 mt = accEd.SampleLevel(lin, (t + 0.5) / tamC, 0);
                mt = all(isfinite(mt)) ? mt : 0;
                const float2 qf = float2(t) + 0.5 + mt;
                if (any(qf < 0.0) || any(qf > tamC)) continue; // o vizinho tambem veio de fora do quadro
                const int2 qt = clamp(int2(qf), 0, lim);
                const float4 v = edicao.Load(int3(qt, 0));
                const float wv = wz * MesmaAntes(exp2(v.a), zt) * NaoOculto(qf, zt, tp) / r;
                ed += v.rgb * wv;
                s1 += wv;
            }
            soma += s1;
            if (s1 > 0.5 / r || achou > 0.0)
                achou += 1.0;
        }
    }
    ed = soma > 1e-4 ? ed / soma : 0;
    const float3 o = Editado(ce, ed, luzLog) / e;
    corJogo[p] = float4(srgb ? Srgb(o) : o, c4.a);
}
)";

DXGI_FORMAT Leitura(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return f;
    }
}

bool Srgb(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

bool Compila(const char* entrada, ID3DBlob** b, std::string& erro)
{
    Microsoft::WRL::ComPtr<ID3DBlob> e;
    if (SUCCEEDED(D3DCompile(kHlsl, strlen(kHlsl), "dlss5ov_jogo", nullptr, nullptr, entrada, "cs_5_1",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, b, &e)))
        return true;
    erro = std::string("shader ") + entrada + ": " + (e ? (const char*)e->GetBufferPointer() : "?");
    return false;
}

// os shaders do modulo, compilados em segundo plano desde o carregamento da DLL: no primeiro quadro do FSR (na thread
// de renderizacao do jogo) a compilacao levava 14-25 s com o jogo parado
const char* const kEntradas[5] = {"CSAcum", "CSProf", "CSCor", "CSLimpaRecon", "CSRecon"};
ID3DBlob* g_blobs[5] = {}; // nunca soltos: na saida do processo o d3dcompiler ja saiu antes desta DLL (como dxgi.dll ela sai por ultimo)
std::string g_erroCompila;
std::atomic<int> g_compila{0}; // 0 nada, 1 compilando, 2 pronto, 3 falhou
std::once_flag g_compilaUma;
} // namespace

void ClienteBuffer::PreCompila()
{
    std::call_once(g_compilaUma, [] {
        g_compila = 1;
        std::thread([] {
            bool ok = true;
            for (int i = 0; i < 5 && ok; ++i)
                ok = Compila(kEntradas[i], &g_blobs[i], g_erroCompila);
            g_compila = ok ? 2 : 3;
        }).detach();
    });
}

bool ClienteBuffer::Compilando()
{
    return g_compila <= 1;
}

bool ClienteBuffer::Inicia(ID3D12Device* d, const char* exe)
{
    PreCompila();
    if (g_compila == 1)
    {
        erro = "compilando os shaders do modulo";
        return false; // o chamador tenta de novo no proximo quadro (Compilando())
    }
    if (g_compila == 3)
    {
        erro = g_erroCompila;
        return false;
    }
    dev = d;
    wchar_t nome[96];
    NomeMapa(nome, 96, GetCurrentProcessId());
    mapa = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Cabecalho), nome);
    if (!mapa)
    {
        erro = "CreateFileMapping";
        return false;
    }
    cab = static_cast<Cabecalho*>(MapViewOfFile(mapa, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Cabecalho)));
    if (!cab)
    {
        erro = "MapViewOfFile";
        return false;
    }
    memset(cab, 0, sizeof(Cabecalho));
    cab->versao = kVersao;
    cab->tamanho = sizeof(Cabecalho);
    cab->pidJogo = GetCurrentProcessId();
    strncpy_s(cab->exe, exe ? exe : "", _TRUNCATE);
    cab->aplica = 1;
    NomeEventoPublica(nome, 96, GetCurrentProcessId());
    evPublica = CreateEventW(nullptr, FALSE, FALSE, nome);
    evCerca = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // raiz: 32 constantes, SRV t0..t11, UAV u0..u9, amostrador linear com borda presa
    D3D12_DESCRIPTOR_RANGE r[2]{};
    r[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 12, 0, 0, 0};
    r[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 10, 0, 0, 0};
    D3D12_ROOT_PARAMETER p[3]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.Num32BitValues = 32;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[1].DescriptorTable = {1, &r[0]};
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[2].DescriptorTable = {1, &r[1]};
    D3D12_STATIC_SAMPLER_DESC s{};
    s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rd{3, p, 1, &s, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Com<ID3DBlob> ser, e;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &ser, &e)) ||
        FAILED(d->CreateRootSignature(0, ser->GetBufferPointer(), ser->GetBufferSize(), IID_PPV_ARGS(&raiz))))
    {
        erro = "root signature";
        return false;
    }
    Com<ID3D12PipelineState>* pso[5] = {&psoMov, &psoProf, &psoCor, &psoLimpaRecon, &psoRecon};
    for (int i = 0; i < 5; ++i)
    {
        ID3DBlob* b = g_blobs[i];
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = raiz.Get();
        pd.CS = {b->GetBufferPointer(), b->GetBufferSize()};
        if (FAILED(d->CreateComputePipelineState(&pd, IID_PPV_ARGS(&*pso[i]))))
        {
            erro = "pso";
            return false;
        }
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kDescQuadro * 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))))
    {
        erro = "descriptor heap";
        return false;
    }
    passoDesc = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (FAILED(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&cercaLocal))))
    {
        erro = "cerca local";
        return false;
    }
    return true;
}

void ClienteBuffer::Encerra()
{
    if (cab)
    {
        cab->geracao = 0;
        UnmapViewOfFile(cab);
        cab = nullptr;
    }
    if (mapa)
        CloseHandle(mapa);
    mapa = nullptr;
    for (HANDLE* h : {&evPublica, &evCerca})
    {
        if (*h)
            CloseHandle(*h);
        *h = nullptr;
    }
}

bool ClienteBuffer::OverlayVivo() const
{
    return cab && cab->ativo && GetTickCount64() - cab->batimentoOverlay < 1000;
}

bool ClienteBuffer::CriaTex(Com<ID3D12Resource>& t, UINT w, UINT h, DXGI_FORMAT f, const wchar_t* qual, uint32_t slot)
{
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.Format = f;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    t.Reset();
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(&t))))
        return false;
    wchar_t nome[128];
    NomeRecurso(nome, 128, GetCurrentProcessId(), geracao, qual, slot);
    HANDLE h_ = nullptr;
    if (FAILED(dev->CreateSharedHandle(t.Get(), nullptr, GENERIC_ALL, nome, &h_)))
        return false;
    handles.push_back(h_); // o nome vale enquanto houver um handle aberto
    return true;
}

bool ClienteBuffer::CriaCerca(Com<ID3D12Fence>& f, const wchar_t* qual)
{
    f.Reset();
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f))))
        return false;
    wchar_t nome[128];
    NomeRecurso(nome, 128, GetCurrentProcessId(), geracao, qual, 0);
    HANDLE h_ = nullptr;
    if (FAILED(dev->CreateSharedHandle(f.Get(), nullptr, GENERIC_ALL, nome, &h_)))
        return false;
    handles.push_back(h_);
    return true;
}

bool ClienteBuffer::Garante(const EntradasJogo& e)
{
    const D3D12_RESOURCE_DESC dc = e.cor->GetDesc(), dm = e.mov->GetDesc(), dp = e.prof->GetDesc();
    auto val = [](uint32_t v, UINT64 total) { return UINT(v && v <= total ? v : total); };
    const UINT cw = val(e.corValidoW, dc.Width), ch = val(e.corValidoH, dc.Height);
    // movimento e profundidade: a regiao valida deste quadro vai para os shaders (MovJogo/ProfJogo); as texturas tem o
    // tamanho maximo e so sao recriadas se ele crescer (antes, cada mudanca da resolucao dinamica recriava tudo)
    movValW = val(e.movValidoW, dm.Width), movValH = val(e.movValidoH, dm.Height);
    profValW = val(e.profValidoW, dp.Width), profValH = val(e.profValidoH, dp.Height);
    auto maximo = [](UINT valido, uint32_t declarado, UINT64 total) {
        return UINT(std::min<UINT64>(std::max<UINT64>(valido, declarado), total));
    };
    UINT mw = maximo(movValW, e.maxRenderW, dm.Width), mh = maximo(movValH, e.maxRenderH, dm.Height);
    UINT pw = maximo(profValW, e.maxRenderW, dp.Width), ph = maximo(profValH, e.maxRenderH, dp.Height);
    const bool mesmaCor = (temp != nullptr) == !(dc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
                          (!temp || (temp->GetDesc().Width == dc.Width && temp->GetDesc().Height == dc.Height));
    const bool mesmaSaida = geracao && cw == corW && ch == corH && dc.Format == fmtCor && dm.Format == fmtMov &&
                            dp.Format == fmtProf && mesmaCor;
    if (mesmaSaida && mw <= movW && mh <= movH && pw <= profW && ph <= profH)
        return true;
    if (mesmaSaida)
    {
        // so cresce: o maximo visto ate aqui continua valendo
        mw = std::max(mw, movW), mh = std::max(mh, movH);
        pw = std::max(pw, profW), ph = std::max(ph, profH);
    }
    cab->geracao = 0; // o overlay solta a geracao velha
    for (HANDLE h_ : handles)
        CloseHandle(h_);
    handles.clear();
    if (geracao)
    {
        Velho v;
        v.valor = valorLocal; // o ultimo quadro gravado com esta geracao
        auto guarda = [&](const Com<ID3D12Resource>& r) {
            if (r)
                v.itens.emplace_back(r.Get());
        };
        for (uint32_t i = 0; i < kSlotsEntrada; ++i)
            guarda(cor[i]), guarda(mov[i]), guarda(prof[i]), guarda(exp[i]);
        for (uint32_t i = 0; i < kSlotsEdicao; ++i)
            guarda(edicao[i]);
        for (auto& par : acc)
            for (auto& r : par)
                guarda(r);
        guarda(recon);
        guarda(temp);
        for (auto& g : guardada)
            guarda(g);
        for (ID3D12Fence* f : {cercaJogo.Get(), cercaOverlay.Get()})
            if (f)
                v.itens.emplace_back(f);
        lixeira.push_back(std::move(v));
    }
    ++geracao;
    corW = cw, corH = ch, fmtCor = dc.Format;
    movW = mw, movH = mh, fmtMov = dm.Format;
    profW = pw, profH = ph, fmtProf = dp.Format;
    bool ok = CriaCerca(cercaJogo, L"cercaJogo") && CriaCerca(cercaOverlay, L"cercaOverlay");
    for (uint32_t i = 0; ok && i < kSlotsEntrada; ++i)
        ok = CriaTex(cor[i], UINT(corW), corH, DXGI_FORMAT_R16G16B16A16_FLOAT, L"cor", i) &&
             CriaTex(mov[i], movW, movH, DXGI_FORMAT_R32G32_FLOAT, L"mov", i) &&
             CriaTex(prof[i], profW, profH, DXGI_FORMAT_R32_FLOAT, L"prof", i) &&
             CriaTex(exp[i], 1, 1, DXGI_FORMAT_R32_FLOAT, L"exp", i);
    for (uint32_t i = 0; ok && i < kSlotsEdicao; ++i)
        ok = CriaTex(edicao[i], UINT(corW), corH, DXGI_FORMAT_R16G16B16A16_FLOAT, L"edicao", i);
    for (int j = 0; ok && j < 3; ++j)
        for (int i = 0; ok && i < 2; ++i)
        {
            // deslocamento acumulado ate cada ancora (local, nao compartilhado)
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = movW;
            rd.Height = movH;
            rd.DepthOrArraySize = rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_R32G32_FLOAT; // FP16 tinha passo de 0,25 px perto de 300 px: desalinhava o detalhe
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            acc[j][i].Reset();
            ok = SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                        IID_PPV_ARGS(&acc[j][i])));
            estadoAcc[j][i] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
    if (ok)
    {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = profW;
        rd.Height = profH;
        rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R32_UINT;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        recon.Reset();
        ok = SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                    nullptr, IID_PPV_ARGS(&recon)));
        estadoRecon = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        // quadros repetidos: a cor dos 2 ultimos quadros exportados (local, nao compartilhada)
        rd.Width = UINT(corW);
        rd.Height = corH;
        rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        for (int i = 0; ok && i < 2; ++i)
        {
            guardada[i].Reset();
            ok = SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                        nullptr, IID_PPV_ARGS(&guardada[i])));
            estadoGuardada[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            quadroGuardado[i] = 0;
        }
    }
    temp.Reset();
    if (ok && !(dc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
    {
        // a cor do jogo nao aceita UAV: soma numa copia (mesma familia de formato) e copia de volta
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd = dc;
        rd.Format = Leitura(dc.Format);
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        rd.MipLevels = 1;
        ok = SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    IID_PPV_ARGS(&temp)));
    }
    if (!ok)
    {
        erro = "texturas compartilhadas";
        geracao = 0;
        return false;
    }
    for (auto& s : cab->entrada)
        s = {};
    for (auto& s : cab->edicao)
        s = {};
    for (auto& l : cab->lido)
        l = 0;
    cab->consumido = 0;
    cab->corW = UINT(corW), cab->corH = corH, cab->movW = movW, cab->movH = movH, cab->profW = profW, cab->profH = profH;
    quadro = pendente = esperaSuspensaAte = 0;
    Esquece();
    MemoryBarrier();
    cab->geracao = geracao;
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE ClienteBuffer::Cpu(UINT i) const
{
    auto h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(i) * passoDesc;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE ClienteBuffer::Gpu(UINT i) const
{
    auto h = heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(i) * passoDesc;
    return h;
}

void ClienteBuffer::Srv(UINT i, ID3D12Resource* r, DXGI_FORMAT f)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = f;
    v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(r, &v, Cpu(i));
}

void ClienteBuffer::Uav(UINT i, ID3D12Resource* r, DXGI_FORMAT f)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
    v.Format = f;
    v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(r, nullptr, &v, Cpu(i));
}

void ClienteBuffer::Barreira(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a,
                             D3D12_RESOURCE_STATES b)
{
    if (!r)
        return;
    if (a == b)
    {
        // UAV -> UAV: barreira de UAV (as escritas de um despacho terminam antes do proximo ler/escrever)
        if (a == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            D3D12_RESOURCE_BARRIER u{};
            u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            u.UAV.pResource = r;
            c->ResourceBarrier(1, &u);
        }
        return;
    }
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    c->ResourceBarrier(1, &x);
}

void ClienteBuffer::Esquece()
{
    for (auto& a : ancora)
        a = 0;
    jUlt = -1;
    ultimaExport = 0;
    esperouPor = 0;
    quadroGuardado[0] = quadroGuardado[1] = 0; // corte de cena: o quadro guardado e da cena velha
    novoMostrado = chegouEm = 0;
}

void ClienteBuffer::EsperaEdicao(std::mutex& mx)
{
    Com<ID3D12Fence> cerca;
    uint64_t alvo = 0;
    {
        std::lock_guard<std::mutex> lk(mx);
        // so no quadro logo depois de uma exportacao (quadro = o ultimo gravado; este sera quadro + 1)
        if (!cab || !evPublica || !evCerca || !geracao || !cercaOverlay || !cab->espera || !cab->aplica ||
            !ultimaExport || ultimaExport != quadro)
            return;
        if (!OverlayVivo() || quadro < esperaSuspensaAte) // devia esperar e nao espera: o log conta por que
        {
            ++(OverlayVivo() ? diag.suspensos : diag.semBatimento);
            return;
        }
        alvo = ultimaExport;
        cerca = cercaOverlay;
    }
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    const ULONGLONG fim = GetTickCount64() + kPrazoEspera;
    bool ok = false;
    int falha = 0; // 1 prazo, 2 batimento, 3 cerca
    for (;;)
    {
        // a edicao de `alvo` (ou mais nova) publicada: o overlay grava a cerca antes do quadro
        uint64_t v = 0;
        for (const SlotEdicao& se : cab->edicao)
            if (se.quadro >= alvo)
            {
                MemoryBarrier();
                if ((v = se.cerca) != 0)
                    break;
            }
        const ULONGLONG agora = GetTickCount64();
        const DWORD resta = agora < fim ? DWORD(fim - agora) : 0;
        if (v)
        {
            if (cerca->GetCompletedValue() < v && resta && SUCCEEDED(cerca->SetEventOnCompletion(v, evCerca)))
                WaitForSingleObject(evCerca, resta);
            ok = cerca->GetCompletedValue() >= v;
            falha = ok ? 0 : 3;
            break;
        }
        if (!resta || !OverlayVivo() || !cab->espera)
        {
            falha = !resta ? 1 : 2;
            break;
        }
        WaitForSingleObject(evPublica, resta); // um disparo velho so faz conferir de novo
    }
    QueryPerformanceCounter(&t1);
    const double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
    std::lock_guard<std::mutex> lk(mx);
    esperouPor = alvo;
    esperaOk = ok;
    ++diag.esperas;
    diag.esperaMs += ms;
    diag.esperaMsMax = std::max(diag.esperaMsMax, ms);
    diag.falhaPrazo += falha == 1, diag.falhaBatimento += falha == 2, diag.falhaCerca += falha == 3;
    if (ok)
        estourosSeguidos = 0;
    else
    {
        ++diag.estouros;
        if (++estourosSeguidos >= 2)
        {
            esperaSuspensaAte = quadro + 300;
            estourosSeguidos = 0;
        }
    }
}

uint64_t ClienteBuffer::Quadro(ID3D12GraphicsCommandList* c, const EntradasJogo& e)
{
    if (!cab || !e.cor || !e.mov || !e.prof)
        return 0;
    if (!lixeira.empty())
    {
        const uint64_t feito = cercaLocal->GetCompletedValue();
        lixeira.erase(std::remove_if(lixeira.begin(), lixeira.end(), [&](const Velho& v) { return v.valor <= feito; }),
                      lixeira.end());
    }
    cab->batimentoJogo = GetTickCount64();
    if (!OverlayVivo())
    {
        estavaVivo = false; // as cadeias de movimento param aqui: recomecam quando o overlay voltar
        return 0;
    }
    if (!Garante(e))
        return 0;
    if (!estavaVivo || e.reset)
        Esquece();
    estavaVivo = true;
    const uint64_t n = ++quadro;
    cab->quadroJogo = n;
    // camera: o FSR passa perto/longe trocados no Z invertido e o infinito como 3,4e38 (UE5: perto = FLT_MAX, longe = 1)
    float perto = e.perto, longe = e.longe;
    bool infinita = e.profInfinita;
    if (longe > 0.f && perto > longe)
        std::swap(perto, longe);
    if (!(longe > 0.f) || !(longe < 1e30f))
        infinita = true; // longe 0 (desconhecido) ou 3,4e38: plano distante no infinito
    if (!(perto > 0.f) || !(perto < 1e30f))
        perto = 0.f; // sem camera: distancia proporcional (basta para comparar)
    cab->profInvertida = e.profInvertidaConhecida ? (e.profInvertida ? 1u : 0u) : 2u;
    cab->profInfinita = infinita;
    cab->hdr = e.hdr;

    // exporta um quadro so quando o overlay terminou o anterior (nenhum quadro exportado fica sem processar: o movimento
    // dos que nao saem vai somado no acumulador) e o slot esta livre
    // Sem atraso: o quadro que esperou a edicao da exportacao anterior so exporta no modo 2 (todo quadro; com a edicao
    // dela publicada o overlay ja pegou aquele quadro, mesmo antes de marcar consumido); no modo 1 a rede fica a cada 2.
    const uint32_t s = uint32_t(n % kSlotsEntrada);
    const bool esperou = esperouPor && esperouPor + 1 == n;
    const bool editada = esperou && esperaOk && esperouPor == ultimaExport;
    // Quadros repetidos (sem atraso, repete = N): so 1 de cada N quadros e exportado, e a cor dele fica guardada; a saida
    // de todo quadro e o quadro guardado mais novo cuja edicao ja chegou, com a edicao feita nele, sem movimento nenhum
    // (o quadro processado entra antes do quadro novo e se repete ate o proximo; nada de edicao velha num quadro novo).
    // Com 1 (todo quadro processado) fica o quadro atual com a edicao do anterior pelo movimento exato (sem atraso de tela)
    const uint32_t pedido = cab->repete;
    const uint32_t repete = cab->espera && pedido >= 2 ? std::min(pedido, 4u) : 0u;
    bool exporta = (cab->consumido >= ultimaExport || (editada && cab->espera == 2)) &&
                   cab->consumido >= cab->entrada[s].quadro && !(esperou && cab->espera == 1) &&
                   !(repete && ultimaExport && n < ultimaExport + repete);
    int iA = -1, kA = -1; // a copia guardada mostrada e o slot da edicao dela
    int iB = -1, kB = -1; // mesclado: o processado seguinte (o mais novo), misturado por cima de iA
    float peso = 0.f;
    if (repete && cab->aplica)
    {
        // a edicao de cada copia guardada, se ja chegou
        int slot[2] = {-1, -1};
        const uint64_t pronto = cercaOverlay->GetCompletedValue();
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < int(kSlotsEdicao); ++j)
            {
                const SlotEdicao& se = cab->edicao[j];
                if (quadroGuardado[i] && se.quadro == quadroGuardado[i] && se.cerca && pronto >= se.cerca)
                    slot[i] = j;
            }
        int novo = -1; // a copia mais nova com edicao
        for (int i = 0; i < 2; ++i)
            if (slot[i] >= 0 && (novo < 0 || quadroGuardado[i] > quadroGuardado[novo]))
                novo = i;
        if (novo >= 0)
        {
            if (quadroGuardado[novo] != novoMostrado)
                novoMostrado = quadroGuardado[novo], chegouEm = n;
            iA = novo, kA = slot[novo];
            // mesclado: nos N-1 quadros depois de a edicao do novo chegar, a mistura do anterior com ele (50/50, 40/60,
            // 30/70 do anterior/novo); depois o novo sozinho. O novo aparece N quadros depois de feito
            const int velho = 1 - novo;
            const uint64_t fase = n - chegouEm;
            static const float kPesos[3] = {0.5f, 0.6f, 0.7f};
            if (cab->gerado == 1 && slot[velho] >= 0 && quadroGuardado[velho] < quadroGuardado[novo] && fase + 1 < repete)
            {
                iB = novo, kB = slot[novo];
                iA = velho, kA = slot[velho];
                peso = kPesos[std::min<uint64_t>(fase, 2)];
            }
        }
    }
    // a copia escrita por este quadro: nunca uma mostrada (misturando, as duas estao em uso: a exportacao fica para o
    // proximo quadro)
    if (iB >= 0)
        exporta = false;
    const int iG = !(repete && exporta) ? -1 : iA >= 0 ? 1 - iA : (quadroGuardado[0] <= quadroGuardado[1] ? 0 : 1);
    // a edicao publicada mais nova cuja escrita ja terminou, se a ancora dela ainda tem acumulador
    int k = -1, jEd = -1;
    if (cab->aplica && !repete)
    {
        const uint64_t pronto = cercaOverlay->GetCompletedValue();
        for (int i = 0; i < int(kSlotsEdicao); ++i)
        {
            const SlotEdicao& se = cab->edicao[i];
            if (se.quadro && se.cerca && pronto >= se.cerca && (k < 0 || se.quadro > cab->edicao[k].quadro))
                k = i;
        }
        if (k >= 0)
            for (int j = 0; j < 3; ++j)
                if (ancora[j] && ancora[j] == cab->edicao[k].quadro)
                    jEd = j;
        if (jEd < 0)
            k = -1;
    }
    uint32_t ativos = 0, zera = 0;
    for (int j = 0; j < 3; ++j)
        if (ancora[j])
            ativos |= 1u << j;
    const int jExp = exporta ? jUlt : -1; // movimento exportado: ate a exportacao anterior
    int jNovo = -1;
    if (exporta)
    {
        // a nova ancora (este quadro) vai num acumulador que nao e o da edicao nem o da ultima exportacao: um livre, ou o
        // de ancora mais velha (com 3 sempre sobra um)
        for (int j = 0; j < 3; ++j)
        {
            if (j == jEd || j == jUlt)
                continue;
            if (jNovo < 0 || (ancora[jNovo] && (!ancora[j] || ancora[j] < ancora[jNovo])))
                jNovo = j;
        }
        zera |= 1u << jNovo;
        ativos &= ~(1u << jNovo);
    }
    if (!exporta && k < 0 && kA < 0 && !ativos)
        return 0;
    ++diag.quadros;
    if (exporta)
        ++diag.exportados;
    if (kB >= 0)
        cab->lido[kB] = n;
    for (const int kk : {k, kA})
        if (kk >= 0)
        {
            cab->lido[kk] = n;
            cab->quadroAplicado = n;
            const uint64_t idade = n - cab->edicao[kk].quadro;
            ++diag.aplicados;
            diag.idadeSoma += idade;
            diag.idadeMax = std::max(diag.idadeMax, idade);
        }
    if (iG >= 0)
        quadroGuardado[iG] = n;
    if (exporta)
    {
        cab->entrada[s].quadro = n;
        cab->entrada[s].jitterX = e.jitterX;
        cab->entrada[s].jitterY = e.jitterY;
        cab->entrada[s].reset = e.reset || jExp < 0;
        cab->entrada[s].perto = perto;
        cab->entrada[s].longe = longe;
        cab->entrada[s].fovY = e.fovY;
    }
    MemoryBarrier();

    // descritores deste quadro: 3 conjuntos (o jogo pode ter ate 3 quadros na GPU); SRV t0..t11, UAV u0..u9
    const UINT tS = (proxDesc % 3) * kDescQuadro, tU = tS + 12;
    ++proxDesc;
    const UINT ant = pp, novo = pp ^ 1;
    const DXGI_FORMAT fMov = Leitura(fmtMov), fProf = Leitura(fmtProf), fCor = Leitura(fmtCor);
    const DXGI_FORMAT fAcc = DXGI_FORMAT_R32G32_FLOAT;
    Srv(tS + 0, e.mov, fMov);
    Srv(tS + 1, e.prof, fProf);
    Srv(tS + 2, e.exposicao, e.exposicao ? Leitura(e.exposicao->GetDesc().Format) : DXGI_FORMAT_R32_FLOAT);
    const int kLe = kA >= 0 ? kA : k; // o slot de edicao lido neste quadro
    Srv(tS + 3, kLe >= 0 ? edicao[kLe].Get() : nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (int j = 0; j < 3; ++j)
        Srv(tS + 4 + j, acc[j][ant].Get(), fAcc);
    Srv(tS + 7, jEd >= 0 ? acc[jEd][novo].Get() : nullptr, fAcc);
    Srv(tS + 8, recon.Get(), DXGI_FORMAT_R32_UINT);
    Srv(tS + 9, iA >= 0 ? guardada[iA].Get() : nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT);
    Srv(tS + 10, iB >= 0 ? guardada[iB].Get() : nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT);
    Srv(tS + 11, kB >= 0 ? edicao[kB].Get() : nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ID3D12Resource* alvo = temp ? temp.Get() : e.cor;
    Uav(tU + 0, alvo, fCor);
    Uav(tU + 1, exporta ? cor[s].Get() : nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT);
    Uav(tU + 2, exporta ? mov[s].Get() : nullptr, DXGI_FORMAT_R32G32_FLOAT);
    Uav(tU + 3, exporta ? prof[s].Get() : nullptr, DXGI_FORMAT_R32_FLOAT);
    Uav(tU + 4, exporta ? exp[s].Get() : nullptr, DXGI_FORMAT_R32_FLOAT);
    for (int j = 0; j < 3; ++j)
        Uav(tU + 5 + j, acc[j][novo].Get(), fAcc);
    Uav(tU + 8, recon.Get(), DXGI_FORMAT_R32_UINT);
    Uav(tU + 9, iG >= 0 ? guardada[iG].Get() : nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT);

    const UINT mw = e.movEspacoW ? e.movEspacoW : movW, mh = e.movEspacoH ? e.movEspacoH : movH;
    struct
    {
        UINT corW, corH, movW, movH, profW, profH, exporta, aplica;
        float movEscX, movEscY, preExp, escExp;
        UINT temExp, srgb, invertida, infinita;
        float perto, longe;
        UINT ativos, zera, jExporta;
        float tanY, aspecto;
        UINT luzLog;
        UINT movVW, movVH, profVW, profVH;
        UINT mostra, guarda;
        float peso;
        UINT luzLog2;
    } k_{UINT(corW), corH, movW, movH, profW, profH, exporta ? 1u : 0u, k >= 0 ? 1u : 0u,
         e.movEscalaX * float(corW) / float(mw), e.movEscalaY * float(corH) / float(mh), e.preExposicao,
         e.escalaExposicao, e.exposicao ? 1u : 0u, Srgb(fmtCor) ? 1u : 0u,
         (e.profInvertidaConhecida ? e.profInvertida : cab->profInvertidaUsada != 0) ? 1u : 0u, infinita ? 1u : 0u, perto,
         longe, ativos, zera, jExp >= 0 ? UINT(jExp) : 3u, TanMeioFov(e.fovY), float(corW) / float(corH),
         kLe >= 0 ? cab->edicao[kLe].luz : 0u, movValW, movValH, profValW, profValH, iA >= 0 ? 1u : 0u,
         iG >= 0 ? 1u : 0u, iB >= 0 ? peso : 0.f, kB >= 0 ? cab->edicao[kB].luz : 0u};

    const D3D12_RESOURCE_STATES SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto estado = [&](int j, UINT i, D3D12_RESOURCE_STATES para) {
        Barreira(c, acc[j][i].Get(), estadoAcc[j][i], para);
        estadoAcc[j][i] = para;
    };
    for (int j = 0; j < 3; ++j)
    {
        estado(j, ant, SRV);
        estado(j, novo, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    Barreira(c, e.mov, e.movEstado, SRV);
    Barreira(c, e.prof, e.profEstado, SRV);
    Barreira(c, e.exposicao, e.expEstado, SRV);
    if (temp)
    {
        Barreira(c, e.cor, e.corEstado, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barreira(c, temp.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        c->CopyResource(temp.Get(), e.cor);
        Barreira(c, temp.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    else
        Barreira(c, e.cor, e.corEstado, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (exporta)
        for (ID3D12Resource* r : {cor[s].Get(), mov[s].Get(), prof[s].Get(), exp[s].Get()})
            Barreira(c, r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (kLe >= 0)
        Barreira(c, edicao[kLe].Get(), D3D12_RESOURCE_STATE_COMMON, SRV);
    if (kB >= 0)
        Barreira(c, edicao[kB].Get(), D3D12_RESOURCE_STATE_COMMON, SRV);
    auto estadoG = [&](int i, D3D12_RESOURCE_STATES para) {
        Barreira(c, guardada[i].Get(), estadoGuardada[i], para);
        estadoGuardada[i] = para;
    };
    if (iA >= 0)
        estadoG(iA, SRV);
    if (iB >= 0)
        estadoG(iB, SRV);
    if (iG >= 0)
        estadoG(iG, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap* hs[] = {heap.Get()};
    c->SetDescriptorHeaps(1, hs);
    c->SetComputeRootSignature(raiz.Get());
    static_assert(sizeof(k_) == 32 * 4, "constantes: o mesmo layout do cbuffer K");
    c->SetComputeRoot32BitConstants(0, 32, &k_, 0);
    c->SetComputeRootDescriptorTable(1, Gpu(tS));
    c->SetComputeRootDescriptorTable(2, Gpu(tU));
    c->SetPipelineState(psoMov.Get()); // CSAcum: acumuladores e o movimento exportado
    c->Dispatch((movW + 7) / 8, (movH + 7) / 8, 1);
    if (exporta)
    {
        c->SetPipelineState(psoProf.Get());
        c->Dispatch((profW + 7) / 8, (profH + 7) / 8, 1);
    }
    if (jEd >= 0)
        estado(jEd, novo, SRV); // CSCor le o acumulador da edicao ja deste quadro
    if (k >= 0)
    {
        // profundidade reconstruida na ancora da edicao: zera, espalha (InterlockedMin) e passa a leitura
        Barreira(c, recon.Get(), estadoRecon, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        c->SetPipelineState(psoLimpaRecon.Get());
        c->Dispatch((profW + 7) / 8, (profH + 7) / 8, 1);
        Barreira(c, recon.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        c->SetPipelineState(psoRecon.Get());
        c->Dispatch((profW + 7) / 8, (profH + 7) / 8, 1);
        Barreira(c, recon.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, SRV);
        estadoRecon = SRV;
    }
    if (exporta || k >= 0 || iA >= 0)
    {
        c->SetPipelineState(psoCor.Get());
        c->Dispatch(UINT((corW + 7) / 8), (corH + 7) / 8, 1);
    }

    if (kLe >= 0)
        Barreira(c, edicao[kLe].Get(), SRV, D3D12_RESOURCE_STATE_COMMON);
    if (kB >= 0)
        Barreira(c, edicao[kB].Get(), SRV, D3D12_RESOURCE_STATE_COMMON);
    if (exporta)
        for (ID3D12Resource* r : {cor[s].Get(), mov[s].Get(), prof[s].Get(), exp[s].Get()})
            Barreira(c, r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    if (temp)
    {
        Barreira(c, temp.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barreira(c, e.cor, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        if (k >= 0 || iA >= 0)
            c->CopyResource(e.cor, temp.Get());
        Barreira(c, e.cor, D3D12_RESOURCE_STATE_COPY_DEST, e.corEstado);
        Barreira(c, temp.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    else
        Barreira(c, e.cor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, e.corEstado);
    Barreira(c, e.exposicao, SRV, e.expEstado);
    Barreira(c, e.prof, SRV, e.profEstado);
    Barreira(c, e.mov, SRV, e.movEstado);
    pp = novo;
    if (exporta)
    {
        ancora[jNovo] = n;
        jUlt = jNovo;
        ultimaExport = n;
    }
    if (exporta || kLe >= 0)
        pendente = n; // o overlay espera a cerca do jogo: quadro exportado e leitura do slot de edicao
    pendenteLocal = ++valorLocal; // a fila de descarte sabe quando a GPU terminou este quadro
    return n;
}

void ClienteBuffer::Submetido(ID3D12CommandQueue* q)
{
    if (pendente && cercaJogo && q)
    {
        q->Signal(cercaJogo.Get(), pendente);
        pendente = 0;
    }
    if (pendenteLocal && cercaLocal && q)
    {
        q->Signal(cercaLocal.Get(), pendenteLocal);
        pendenteLocal = 0;
    }
}

} // namespace dlss5ov
