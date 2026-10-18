// HLSL dos passes do overlay (compilados na inicializacao, cs_5_1)
#pragma once

namespace ov::hlsl {

// captura (B8G8R8A8 sRGB) -> quadro inteiro linear FP16; e quadro inteiro -> tamanho da rede (filtro de caixa)
inline const char* Prepara = R"(
cbuffer C : register(b0) { uint fw; uint fh; uint dw; uint dh; uint hdr; float escala; uint pad0; uint pad1; };
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
SamplerState s : register(s0);
float3 Linear(float3 c) { return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

[numthreads(8, 8, 1)] void CSInteiro(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= fw || p.y >= fh) return;
    float4 c = src.Load(int3(p, 0));
    // HDR: scRGB linear dividido pelo branco SDR do Windows (o branco de papel vira 1); SDR: sRGB -> linear
    dst[p] = float4(hdr ? max(c.rgb, 0.0) / escala : Linear(saturate(c.rgb)), 1.0);
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
    dst[p] = float4(a.rgb / (n * n), 1.0);
}
)";

// quadro inteiro + forca * (resposta - alimentado), ampliados por Catmull-Rom; saida sRGB 8 bits
// modo: 0 rede, 1 original, 2 comparacao (esquerda original | direita rede), 3 so a edicao (x4, cinza = 0)
inline const char* Compoe = R"(
cbuffer C : register(b0) { uint fw; uint fh; uint dw; uint dh; float forca; uint modo; float divisao; uint usaEdicao; uint hdr; float escala; };
Texture2D<float4> inteiro : register(t0);
Texture2D<float4> fed : register(t1);
Texture2D<float4> resp : register(t2);
Texture2D<float4> mov : register(t3);
RWTexture2D<float4> saida : register(u0);
SamplerState s : register(s0);

float3 Srgb(float3 l) { l = saturate(l); return (l <= 0.0031308) ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055; }

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

[numthreads(8, 8, 1)] void CSCompoe(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= fw || p.y >= fh) return;
    float3 o = inteiro.Load(int3(p, 0)).rgb;
    float3 e;
    // usaEdicao: `resp` ja e a edicao estabilizada (Temporal); senao, resposta - alimentado
    if (dw == fw && dh == fh)
        e = usaEdicao ? resp.Load(int3(p, 0)).rgb : resp.Load(int3(p, 0)).rgb - fed.Load(int3(p, 0)).rgb;
    else
    {
        float2 uv = (float2(p) + 0.5) / float2(fw, fh);
        float2 tam = float2(dw, dh);
        e = usaEdicao ? CatmullRom(resp, uv, tam) : CatmullRom(resp, uv, tam) - CatmullRom(fed, uv, tam);
    }
    float3 r = o + forca * e;
    bool orig = modo == 1 || (modo == 2 && (float)p.x < divisao * fw);
    float3 c = orig ? o : r;
    if (modo == 3) c = 0.5 + 4.0 * e;
    if (modo == 4)
    {
        // movimento em cores: matiz = direcao, brilho = tamanho (8 px = cheio)
        float2 v = mov.SampleLevel(s, (float2(p) + 0.5) / float2(fw, fh), 0).xy;
        float a = atan2(v.y, v.x) / 6.2831853 + 0.5;
        float3 h = saturate(abs(frac(a + float3(0, 2.0 / 3, 1.0 / 3)) * 6 - 3) - 1);
        c = lerp(float3(0.5, 0.5, 0.5), h, saturate(length(v) / 8.0));
    }
    if (modo == 2 && abs((float)p.x - divisao * fw) < 1.0) c = float3(1, 1, 0);
    if (hdr)
        saida[p] = float4((modo >= 3 ? saturate(c) : max(c, 0.0)) * escala, 1.0); // de volta a scRGB
    else
        saida[p] = float4(modo >= 3 ? saturate(c) : Srgb(c), 1.0);
}
)";

} // namespace ov::hlsl
