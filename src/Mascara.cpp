#include "Mascara.h"
#include "Shaders.h"
#include <algorithm>

namespace ov {

namespace {
const D3D12_RESOURCE_STATES SRV =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr UINT kEW = Classificador::kEntW, kEH = Classificador::kEntH, kOW = Classificador::kSaiW,
               kOH = Classificador::kSaiH;

// a cor como o classificador ve (e o guia da borda compara): sRGB do quadro linear, joelho suave nos realces
const char* kCorG = R"(
float3 CorG(float3 x, float exposicao) { return Srgb(SoftKnee(max(x / exposicao, 0.0))); }
// a probabilidade do classificador vem borrada pela resolucao dele (640x360): em volta do personagem o fundo ganha meio
// "personagem". Afiada, a borda fica com a cor e a profundidade do quadro (o bilateral), nao com o borrao.
float4 Afia(float4 v, float a, float b) { return smoothstep(a, b, v); }
)";

// o quadro para o classificador (1280x720 sRGB 8 bits) e o guia de t0 (640x360: a cor media que ele viu e a distancia)
const char* kReduz = R"(
cbuffer C : register(b0) { uint temZ; float exposicao; float ganho; uint pad1; };
Texture2D<float4> fed : register(t0);
Texture2D<float> z : register(t1);
RWTexture2D<float4> red : register(u0);
RWTexture2D<float4> guia : register(u1);
SamplerState s : register(s0);
[numthreads(8, 8, 1)] void CSReduz(uint2 g : SV_DispatchThreadID)
{
    if (g.x >= 640 || g.y >= 360) return;
    float3 soma = 0;
    [unroll] for (uint j = 0; j < 2; ++j)
        [unroll] for (uint i = 0; i < 2; ++i)
        {
            const uint2 q = g * 2 + uint2(i, j);
            const float3 x = fed.SampleLevel(s, (float2(q) + 0.5) / float2(1280, 720), 0).rgb;
            red[q] = float4(CorG(x * ganho, exposicao), 1); // o classificador ve o quadro clareado (cena escura)
            soma += CorG(x, exposicao);                     // o guia compara com o quadro como ele e
        }
    float lz = 0;
    if (temZ)
    {
        uint zw, zh;
        z.GetDimensions(zw, zh);
        lz = log2(max(z.Load(int3(min(uint2((float2(g) + 0.5) * float2(zw, zh) / float2(640, 360)), uint2(zw, zh) - 1), 0)), 1e-6));
    }
    guia[g] = float4(soma * 0.25, lz);
}
)";

// deslocamento acumulado ate t0 (o quadro que o classificador viu): o mesmo de Temporal.cpp
const char* kAcumula = R"(
cbuffer C : register(b0) { uint w; uint h; uint zera; uint temMov; };
Texture2D<float4> mov : register(t0);
Texture2D<float> conf : register(t1);
Texture2D<float4> ant : register(t2);
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

// limpeza da classificacao na chegada, no espaco de t0: o classificador borra a borda (meio "personagem" no fundo em
// volta dele, que vira rastro quando ele anda). Cada ponto passa a ser a media dos vizinhos (9x9) da MESMA superficie --
// distancia da camera igual (captura de buffer) e cor parecida: o fundo em volta do personagem tem muito mais vizinhos de
// fundo (zero) que de borrao, e o personagem, de personagem.
const char* kLimpa = R"(
cbuffer C : register(b0) { uint temZ; float sigmaZ; float sigmaCor; uint pad; };
Texture2D<float4> obs : register(t0);
Texture2D<float4> guia : register(t1);
RWTexture2D<float4> limpa : register(u0);
[numthreads(8, 8, 1)] void CSLimpa(uint2 t : SV_DispatchThreadID)
{
    if (t.x >= 640 || t.y >= 360) return;
    const float4 g0 = guia.Load(int3(t, 0));
    float4 acc = 0;
    float ws = 0;
    [unroll] for (int j = -4; j <= 4; ++j)
        [unroll] for (int i = -4; i <= 4; ++i)
        {
            const int2 u = clamp(int2(t) + int2(i, j), int2(0, 0), int2(639, 359));
            const float4 gu = guia.Load(int3(u, 0));
            const float3 dc = gu.rgb - g0.rgb;
            float w = exp(-float(i * i + j * j) / 18.0) * exp(-dot(dc, dc) / (2.0 * sigmaCor * sigmaCor));
            if (temZ)
                w *= exp(-(gu.a - g0.a) * (gu.a - g0.a) / (2.0 * sigmaZ * sigmaZ));
            acc += obs.Load(int3(u, 0)) * w;
            ws += w;
        }
    limpa[t] = acc / max(ws, 1e-6);
}
)";

// estado da mascara (rgba = pele, cabelo, personagem, objeto; o resto e cenario): o do quadro anterior segue o movimento
// (onde o movimento nao e confiavel -- fundo recem-descoberto -- volta ao cenario) e a classificacao nova, trazida de
// t0 pelo deslocamento acumulado, entra com um bilateral conjunto: dos 4x4 vizinhos da classificacao, pesam os de cor e
// distancia parecidas com as deste pixel -- a borda cai onde a imagem (e a profundidade do jogo) muda, nao no bloco de
// 640x360.
const char* kEstado = R"(
cbuffer C : register(b0)
{
    uint w; uint h; uint temMov; uint temConf; uint nova; uint temZ; uint valido; uint pad0;
    float mistura; float exposicao; float sigmaCor; float sigmaZ;
};
Texture2D<float4> ant : register(t0);
Texture2D<float4> mov : register(t1);
Texture2D<float> conf : register(t2);
Texture2D<float4> obs : register(t3);
Texture2D<float4> guia : register(t4);
Texture2D<float4> desl : register(t5);
Texture2D<float4> fed : register(t6);
Texture2D<float> zAgora : register(t7);
Texture2D<float> sabeAnt : register(t8);     // quanto o estado anterior sabia de cada pixel
RWTexture2D<float4> novo : register(u0);
RWTexture2D<float> conhecido : register(u1); // 1 = veio do movimento ou da classificacao; 0 = nenhum dos dois
SamplerState s : register(s0);
[numthreads(8, 8, 1)] void CSEstado(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    const float2 tam = float2(w, h);
    float4 prev = 0;
    float vPrev = 0;
    if (valido)
    {
        const float2 v = temMov ? mov.Load(int3(p, 0)).xy : float2(0, 0);
        const float2 uv = (float2(p) + 0.5 + v) / tam;
        const bool dentro = all(uv > 0.0) && all(uv < 1.0);
        // o "nao sei" tambem viaja com o movimento: um pedaco que nunca foi classificado nao vira "sei que e cenario"
        const float c = dentro ? (temConf ? smoothstep(0.2, 0.6, conf.Load(int3(p, 0))) : 1.0) : 0.0;
        // o ponto mais proximo, nunca a media de vizinhos: na borda do personagem a media poe "meio personagem" no
        // fundo ao lado dele a cada quadro, e essa faixa fica para tras quando ele anda (o rastro)
        const int2 pa = clamp(int2(uv * tam), int2(0, 0), int2(w, h) - 1);
        prev = dentro ? ant.Load(int3(pa, 0)) * c : 0;
        vPrev = dentro ? c * sabeAnt.Load(int3(pa, 0)) : 0.0;
    }
    float4 r = prev;
    float sabe = vPrev;
    if (nova)
    {
        const float4 d = desl.Load(int3(p, 0));
        const float2 q = float2(p) + 0.5 + d.xy;          // posicao em t0 (pixels da rede)
        const float2 o = q * float2(640, 360) / tam - 0.5; // posicao na classificacao
        const float3 cg = CorG(fed.Load(int3(p, 0)).rgb, exposicao);
        const float zc = temZ ? log2(max(zAgora.Load(int3(p, 0)), 1e-6)) : 0;
        float4 acc = 0;
        float ws = 0;
        const int2 b = int2(floor(o));
        [unroll] for (int j = -1; j <= 2; ++j)
            [unroll] for (int i = -1; i <= 2; ++i)
            {
                const int2 t = clamp(b + int2(i, j), int2(0, 0), int2(639, 359));
                const float2 dd = abs(o - float2(t));
                const float wsp = max(0.0, 1.0 - dd.x / 1.5) * max(0.0, 1.0 - dd.y / 1.5);
                const float4 gt = guia.Load(int3(t, 0));
                const float3 dc = cg - gt.rgb;
                float wt = wsp * exp(-dot(dc, dc) / (2.0 * sigmaCor * sigmaCor));
                if (temZ)
                    wt *= exp(-(zc - gt.a) * (zc - gt.a) / (2.0 * sigmaZ * sigmaZ));
                acc += Afia(obs.Load(int3(t, 0)), 0.25, 0.75) * wt;
                ws += wt;
            }
        // Nenhum vizinho da mesma superficie (cor e distancia diferentes de todos): este pixel estava coberto em t0 --
        // fundo que o personagem acabou de descobrir. A classificacao de t0 ali e a do personagem (o rastro): nao entra,
        // fica o que seguiu o movimento (que ja voltou ao cenario onde o movimento nao e confiavel). Idem fora da imagem
        // em t0 (borda nova) e onde o caminho ate t0 passou por movimento sem confianca.
        const float achou = smoothstep(0.02, 0.15, ws);
        // perto da borda do quadro de t0 o classificador erra (o personagem cortado pela borda sai "nao e personagem"):
        // ali a classificacao vale pouco e nao apaga o que o movimento e o preenchimento sabem
        const float borda = smoothstep(4.0, 48.0, min(min(q.x, q.y), min(tam.x - q.x, tam.y - q.y)));
        const float k = (valido && vPrev > 0.5 ? mistura : 1.0) * achou * borda * (d.w > 0.5 ? 1.0 : 0.0) *
                        (temConf ? smoothstep(0.2, 0.6, d.z) : 1.0);
        const float4 alvo = ws > 1e-4 ? acc / ws : prev;
        // ver o personagem vale mais que nao ver: o detector perde o personagem de costas num quadro e acha no outro; o
        // que a classificacao deixa de ver some devagar (entre uma e outra o movimento leva a mascara). Objeto: igual.
        const float3 kp = (alvo.rgb < prev.rgb) ? k * 0.4 : k;
        r = lerp(prev, alvo, float4(kp, k));
        sabe = max(sabe, k);
    }
    novo[p] = r;
    conhecido[p] = sabe;
}
)";

// Pixel sem informacao (parte do personagem que acabou de entrar na tela, ou que estava coberta no quadro que o
// classificador viu, ou fundo recem-descoberto): herda dos vizinhos conhecidos da MESMA superficie (distancia da camera
// igual, captura de buffer) -- anel que cresce de 4 a 256 px, peso 1/r. Sem profundidade (video) fica como esta.
const char* kPreenche = R"(
cbuffer C : register(b0) { uint w; uint h; uint temZ; uint pad; };
Texture2D<float4> bruto : register(t0);
Texture2D<float> conhecido : register(t1);
Texture2D<float> zAgora : register(t2);
RWTexture2D<float4> novo : register(u0);
RWTexture2D<float> sabeNovo : register(u1);
[numthreads(8, 8, 1)] void CSPreenche(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    const float4 v = bruto.Load(int3(p, 0));
    const float sabe = conhecido.Load(int3(p, 0));
    if (!temZ || sabe > 0.95)
    {
        novo[p] = v;
        sabeNovo[p] = sabe;
        return;
    }
    const float zc = log2(max(zAgora.Load(int3(p, 0)), 1e-6));
    const int2 lim = int2(w, h) - 1;
    float4 acc = 0;
    float ws = 0, achou = 0;
    [loop] for (float r = 4.0; r <= 256.0 && achou < 2.0; r *= 2.0)
    {
        float s1 = 0;
        [unroll] for (int k = 0; k < 12; ++k)
        {
            float2 dir;
            sincos(k * 0.5235988 + r * 0.37, dir.y, dir.x);
            const int2 t = clamp(int2(float2(p) + 0.5 + dir * r), int2(0, 0), lim);
            const float st = conhecido.Load(int3(t, 0));
            if (st < 0.9) continue; // so de quem sabe de verdade (um palpite herdado nao se espalha)
            const float dz = abs(log2(max(zAgora.Load(int3(t, 0)), 1e-6)) - zc);
            const float wv = (1.0 - smoothstep(0.03, 0.08, dz)) / r;
            acc += bruto.Load(int3(t, 0)) * wv;
            s1 += wv;
        }
        ws += s1;
        if (s1 > 0.3 / r || achou > 0.0)
            achou += 1.0;
    }
    novo[p] = ws > 1e-6 ? lerp(acc / ws, v, sabe) : v;
    sabeNovo[p] = ws > 1e-6 ? max(sabe, 0.9) : sabe; // herdado da mesma superficie: passa a valer (e a viajar)
}
)";

// grupo e peso no alfa da entrada da rede: a = g + 0,99 w (g = o grupo mais provavel, w = 1 - cenario)
const char* kCodifica = R"(
cbuffer C : register(b0) { uint w; uint h; uint valido; uint pad; };
Texture2D<float4> estado : register(t0);
RWTexture2D<float4> entrada : register(u0);
[numthreads(8, 8, 1)] void CSCodifica(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    float a = 0;
    if (valido)
    {
        // o peso e a media 5x5 (a mascara pontilhada viraria manchas na rede); o grupo, o do proprio pixel
        float somaV = 0;
        [unroll] for (int j = -2; j <= 2; ++j)
            [unroll] for (int i = -2; i <= 2; ++i)
            {
                const float4 v = estado.Load(int3(clamp(int2(p) + int2(i, j) * 2, int2(0, 0), int2(w, h) - 1), 0));
                somaV += v.r + v.g + v.b + v.a;
            }
        const float4 m = estado.Load(int3(p, 0));
        const float soma = somaV / 25.0;
        uint g = 1;
        float melhor = m.r;
        if (m.g > melhor) { g = 2; melhor = m.g; }
        if (m.b > melhor) { g = 3; melhor = m.b; }
        if (m.a > melhor) { g = 4; melhor = m.a; }
        // o peso firme: a probabilidade do classificador fica em 0,7-0,9 dentro do personagem (e o grupo renderia menos)
        const float wv = smoothstep(0.15, 0.65, soma);
        a = wv > 0.004 ? float(g) + 0.99 * wv : 0.0;
    }
    float4 v = entrada[p];
    v.a = a;
    entrada[p] = v;
}
)";

// Mostrar mascara: os grupos pintados por cima (pele vermelha, cabelo amarelo, personagem azul, objeto verde)
const char* kMostra = R"(
cbuffer C : register(b0) { uint sw; uint sh; uint hdr; float escala; };
Texture2D<float4> estado : register(t0);
RWTexture2D<float4> saida : register(u0);
SamplerState s : register(s0);
[numthreads(8, 8, 1)] void CSMostra(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= sw || p.y >= sh) return;
    const float4 m = estado.SampleLevel(s, (float2(p) + 0.5) / float2(sw, sh), 0);
    const float soma = m.r + m.g + m.b + m.a;
    if (soma <= 0.01) return;
    float3 cor = (m.r * float3(1.0, 0.22, 0.18) + m.g * float3(1.0, 0.85, 0.1) + m.b * float3(0.2, 0.5, 1.0) +
                  m.a * float3(0.25, 1.0, 0.4)) / soma;
    if (hdr) cor = Linear(cor) * escala;
    float4 o = saida[p];
    o.rgb = lerp(o.rgb, cor, 0.55 * saturate(soma));
    saida[p] = o;
}
)";
} // namespace

void Mascara::Cria(ID3D12Device* d)
{
    const std::string base = std::string(hlsl::Comum) + kCorG;
    pReduz.Cria(d, (base + kReduz).c_str(), "CSReduz", 2, 2, 4, "mascara: reduz");
    pAcumula.Cria(d, (base + kAcumula).c_str(), "CSAcumula", 3, 1, 4, "mascara: acumula");
    pLimpa.Cria(d, (base + kLimpa).c_str(), "CSLimpa", 2, 1, 4, "mascara: limpa");
    pEstado.Cria(d, (base + kEstado).c_str(), "CSEstado", 9, 2, 12, "mascara: estado");
    pPreenche.Cria(d, (base + kPreenche).c_str(), "CSPreenche", 3, 2, 4, "mascara: preenche");
    pCodifica.Cria(d, (base + kCodifica).c_str(), "CSCodifica", 1, 1, 4, "mascara: codifica");
    pMostra.Cria(d, (base + kMostra).c_str(), "CSMostra", 1, 1, 4, "mascara: mostra");
}

bool Mascara::Inicia(const std::wstring& pastaMl, ID3D12Device* d)
{
    if (cls.Pronto())
        return true;
    if (!cls.Inicia(pastaMl, d))
    {
        Log("mascara", "classificador nao carregou: " + cls.Erro());
        return false;
    }
    Log("mascara", "classificador pronto (ONNX Runtime + DirectML): cena.onnx, partes.onnx");
    return true;
}

void Mascara::Garante(ID3D12Device* d, UINT w_, UINT h_)
{
    if (!reduzida.r)
    {
        reduzida.Cria(d, kEW, kEH, DXGI_FORMAT_R8G8B8A8_UNORM, L"mascara: quadro do classificador");
        guia.Cria(d, kOW, kOH, DXGI_FORMAT_R16G16B16A16_FLOAT, L"mascara: guia de t0");
        obs.Cria(d, kOW, kOH, DXGI_FORMAT_R8G8B8A8_UNORM, L"mascara: classificacao", false);
        obsLimpa.Cria(d, kOW, kOH, DXGI_FORMAT_R16G16B16A16_FLOAT, L"mascara: classificacao limpa");
        const auto rd = reduzida.r->GetDesc();
        UINT64 total = 0;
        d->GetCopyableFootprints(&rd, 0, 1, 0, &pegada, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Ck(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                      IID_PPV_ARGS(&leitura)),
           "mascara: leitura");
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        bd.Width = UINT64(kOW) * 4 * kOH; // 2560 bytes por linha: ja alinhado a 256
        Ck(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                      IID_PPV_ARGS(&envio)),
           "mascara: envio");
    }
    if (w == w_ && h == h_)
        return;
    w = w_;
    h = h_;
    descarta = emVoo; // o deslocamento ate t0 recomeca do zero: a classificacao em voo nao tem como chegar
    for (auto& t : desl)
        t.Cria(d, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"mascara: deslocamento ate t0");
    for (auto& t : estado)
        t.Cria(d, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"mascara: estado");
    bruto.Cria(d, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"mascara: estado antes do preenchimento");
    conhecido.Cria(d, w, h, DXGI_FORMAT_R8_UNORM, L"mascara: pixel conhecido");
    for (auto& t : sabe)
        t.Cria(d, w, h, DXGI_FORMAT_R8_UNORM, L"mascara: o que o estado sabe");
    valido = false;
}

void Mascara::Quadro(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex& fed, Tex* mov, Tex* conf,
                     Tex* z, float exposicao, UINT64 quadro, int taxa)
{
    if (!cls.Pronto())
        return;
    quadroAtual = quadro;
    Garante(d, fed.w, fed.h);
    using Par = std::pair<ID3D12Resource*, DXGI_FORMAT>;
    const Par pFed{fed.r.Get(), fed.fmt};
    // 1. deslocamento deste quadro ate t0 (so com um quadro no classificador)
    Tex* D = nullptr;
    if (emVoo)
    {
        Tex& ant = desl[qualDesl];
        Tex& nv = desl[qualDesl ^ 1];
        ant.Para(c, SRV);
        nv.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (mov)
            mov->Para(c, SRV);
        if (conf)
            conf->Para(c, SRV);
        const UINT k[4] = {nv.w, nv.h, zeraDesl ? 1u : 0u, mov ? 1u : 0u};
        pAcumula.Roda(c, desc, d,
                      {mov ? Par{mov->r.Get(), mov->fmt} : Par{ant.r.Get(), ant.fmt},
                       (mov && conf) ? Par{conf->r.Get(), conf->fmt} : Par{ant.r.Get(), ant.fmt}, {ant.r.Get(), ant.fmt}},
                      {{nv.r.Get(), nv.fmt}}, k, (nv.w + 7) / 8, (nv.h + 7) / 8);
        nv.Para(c, SRV);
        qualDesl ^= 1;
        zeraDesl = false;
        D = &nv;
    }
    // 2. classificacao nova (do quadro t0, que ja passou)
    bool nova = false;
    uint64_t id = 0;
    const bool chegou = emVoo && D && cls.Pega(resultado, id);
    if (chegou)
        emVoo = false;
    if (chegou && descarta)
        descarta = false; // da cena de antes do corte
    else if (chegou)
    {
        void* p = nullptr;
        Ck(envio->Map(0, nullptr, &p), "mascara: Map envio");
        memcpy(p, resultado.data(), resultado.size());
        envio->Unmap(0, nullptr);
        obs.Para(c, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = obs.r.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = envio.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kOW, kOH, 1, kOW * 4};
        c->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        obs.Para(c, SRV);
        guia.Para(c, SRV);
        obsLimpa.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        struct
        {
            UINT temZ;
            float sigmaZ, sigmaCor;
            UINT pad;
        } kl{guiaComZ ? 1u : 0u, 0.15f, guiaComZ ? 0.25f : 0.08f, 0};
        pLimpa.Roda(c, desc, d, {{obs.r.Get(), obs.fmt}, {guia.r.Get(), guia.fmt}}, {{obsLimpa.r.Get(), obsLimpa.fmt}},
                    &kl, (kOW + 7) / 8, (kOH + 7) / 8);
        obsLimpa.Para(c, SRV);
        nova = true;
    }
    // 3. estado: o anterior pelo movimento + a classificacao nova
    if (valido || nova)
    {
        Tex& ant = estado[qualEstado];
        Tex& nv = estado[qualEstado ^ 1];
        Tex& sAnt = sabe[qualEstado];
        Tex& sNv = sabe[qualEstado ^ 1];
        ant.Para(c, SRV);
        sAnt.Para(c, SRV);
        obsLimpa.Para(c, SRV);
        bruto.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        conhecido.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        guia.Para(c, SRV);
        fed.Para(c, SRV);
        if (z)
            z->Para(c, SRV);
        struct
        {
            UINT w, h, temMov, temConf, nova, temZ, valido, pad;
            float mistura, exposicao, sigmaCor, sigmaZ;
        } k{w, h, mov ? 1u : 0u, (mov && conf) ? 1u : 0u, nova ? 1u : 0u, z ? 1u : 0u, valido ? 1u : 0u, 0,
            0.75f, std::max(exposicao, 1e-3f), 0.1f, 0.12f};
        pEstado.Roda(c, desc, d,
                     {{ant.r.Get(), ant.fmt}, mov ? Par{mov->r.Get(), mov->fmt} : pFed,
                      (mov && conf) ? Par{conf->r.Get(), conf->fmt} : pFed, {obsLimpa.r.Get(), obsLimpa.fmt},
                      {guia.r.Get(), guia.fmt}, D ? Par{D->r.Get(), D->fmt} : pFed, pFed,
                      z ? Par{z->r.Get(), z->fmt} : pFed, {sAnt.r.Get(), sAnt.fmt}},
                     {{bruto.r.Get(), bruto.fmt}, {conhecido.r.Get(), conhecido.fmt}}, &k, (w + 7) / 8, (h + 7) / 8);
        bruto.Para(c, SRV);
        conhecido.Para(c, SRV);
        nv.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        sNv.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const UINT kp[4] = {w, h, z ? 1u : 0u, 0};
        pPreenche.Roda(c, desc, d, {{bruto.r.Get(), bruto.fmt}, {conhecido.r.Get(), conhecido.fmt},
                                     z ? Par{z->r.Get(), z->fmt} : Par{conhecido.r.Get(), conhecido.fmt}},
                       {{nv.r.Get(), nv.fmt}, {sNv.r.Get(), sNv.fmt}}, kp, (w + 7) / 8, (h + 7) / 8);
        nv.Para(c, SRV);
        sNv.Para(c, SRV);
        qualEstado ^= 1;
        valido = true;
    }
    // 4. este quadro para o classificador
    // `taxa` classificacoes por segundo, no maximo (cada uma custa GPU e CPU; entre elas o movimento leva a mascara)
    const auto agora = std::chrono::steady_clock::now();
    const bool vez = std::chrono::duration<double>(agora - tUltimoEnvio).count() >= 1.0 / std::clamp(taxa, 1, 30);
    if (!emVoo && !pendente && cls.Livre() && vez)
    {
        tUltimoEnvio = agora;
        fed.Para(c, SRV);
        if (z)
            z->Para(c, SRV);
        reduzida.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        guia.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // ganho pela mediana do quadro anterior (o que o classificador recebeu, ja com o ganho de entao): mira 0,35 sRGB
        {
            const float med = std::max(cls.Mediana(), 0.004f);
            const float alvo = std::pow(0.35f / med, 2.2f); // em luz linear, aproximado
            ganhoReduz = std::clamp(ganhoReduz * std::pow(alvo, 0.5f), 1.f, 64.f);
        }
        struct
        {
            UINT temZ;
            float exposicao, ganho;
            UINT pad1;
        } k{z ? 1u : 0u, std::max(exposicao, 1e-3f), ganhoReduz, 0};
        guiaComZ = z != nullptr;
        pReduz.Roda(c, desc, d, {pFed, z ? Par{z->r.Get(), z->fmt} : pFed},
                    {{reduzida.r.Get(), reduzida.fmt}, {guia.r.Get(), guia.fmt}}, &k, (kOW + 7) / 8, (kOH + 7) / 8);
        guia.Para(c, SRV);
        reduzida.Para(c, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = leitura.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = pegada;
        src.pResource = reduzida.r.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        c->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        reduzida.Para(c, SRV);
        pendente = true;
        idPendente = quadro;
        ultimoEnvio = quadro;
    }
}

void Mascara::DepoisDoQuadro()
{
    if (!pendente)
        return;
    pendente = false;
    void* p = nullptr;
    const D3D12_RANGE r{0, SIZE_T(pegada.Footprint.RowPitch) * kEH};
    if (FAILED(leitura->Map(0, &r, &p)))
        return;
    if (cls.Envia(static_cast<const uint8_t*>(p) + pegada.Offset, pegada.Footprint.RowPitch, idPendente))
    {
        emVoo = true;
        idEmVoo = idPendente;
        zeraDesl = true; // o quadro seguinte acumula a partir de t0
    }
    const D3D12_RANGE nada{0, 0};
    leitura->Unmap(0, &nada);
}

void Mascara::Codifica(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex& entrada)
{
    if (!cls.Pronto() || w != entrada.w || h != entrada.h)
        return;
    Tex& e = estado[qualEstado];
    e.Para(c, SRV);
    entrada.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const UINT k[4] = {w, h, valido ? 1u : 0u, 0};
    pCodifica.Roda(c, desc, d, {{e.r.Get(), e.fmt}}, {{entrada.r.Get(), entrada.fmt}}, k, (w + 7) / 8, (h + 7) / 8);
    entrada.Para(c, D3D12_RESOURCE_STATE_COMMON);
}

void Mascara::Mostra(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex& saida, bool hdr,
                     float escala)
{
    if (!valido)
        return;
    Tex& e = estado[qualEstado];
    e.Para(c, SRV);
    const auto antes = saida.estado;
    saida.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    struct
    {
        UINT sw, sh, hdr;
        float escala;
    } k{saida.w, saida.h, hdr ? 1u : 0u, escala};
    pMostra.Roda(c, desc, d, {{e.r.Get(), e.fmt}}, {{saida.r.Get(), saida.fmt}}, &k, (saida.w + 7) / 8,
                 (saida.h + 7) / 8);
    saida.Para(c, antes);
}

std::string Mascara::Status() const
{
    if (!cls.Pronto())
    {
        const std::string e = cls.Erro();
        return e.empty() ? "classificador desligado" : "classificador: " + e;
    }
    return Fmt("%d personagem(ns), classificacao em %.0f ms", cls.Pessoas(), cls.Ms());
}

} // namespace ov
