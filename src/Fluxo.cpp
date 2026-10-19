#include "Fluxo.h"
#include "ShadersFluxo.h"
#include <algorithm>

namespace ov {

namespace {
struct K
{
    UINT lw, lh, gw, gh;
    int raio;
    UINT temGrosso;
    float lambda;
    UINT pad;
};
struct KBusca
{
    UINT lw, lh, gw, gh;
    int raio;
    UINT temGrosso;
    float lambda;
    UINT janela, usaGlobal;
    float escalaGlobal, meiaGlobal;
    UINT pad;
};
const D3D12_RESOURCE_STATES SRV =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
} // namespace

void Fluxo::Cria(ID3D12Device* d)
{
    pLuma.Cria(d, hlsl::Fluxo, "CSLuma", 1, 1, 8, "fluxo luma");
    pReduzLuma.Cria(d, hlsl::ReduzLuma, "CSReduzLuma", 1, 1, 8, "fluxo reduz");
    pBusca.Cria(d, hlsl::Busca, "CSBusca", 4, 1, 12, "fluxo busca");
    pGlobal.Cria(d, hlsl::Global, "CSGlobal", 1, 1, 8, "fluxo global");
    pMediana.Cria(d, hlsl::Mediana, "CSMediana", 1, 1, 8, "fluxo mediana");
    pAmplia.Cria(d, hlsl::Amplia, "CSAmplia", 1, 1, 8, "fluxo amplia");
    pConfianca.Cria(d, hlsl::Confianca, "CSConfianca", 2, 1, 8, "fluxo confianca");
    pEstat.Cria(d, hlsl::Estatisticas, "CSEstatisticas", 4, 1, 8, "fluxo estatisticas");
    pGradiente.Cria(d, hlsl::Gradiente, "CSGradiente", 1, 1, 8, "fluxo gradiente");
    pRefina.Cria(d, hlsl::Refina, "CSRefina", 4, 1, 8, "fluxo refina");
    auto buf = [&](D3D12_HEAP_TYPE t, D3D12_RESOURCE_STATES e, D3D12_RESOURCE_FLAGS f, ComPtr<ID3D12Resource>& r) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = t;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 256;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = f;
        Ck(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, e, nullptr, IID_PPV_ARGS(&r)), "buffer(estat)");
    };
    buf(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, bufEstat);
    buf(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, bufZero);
    buf(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, bufLeitura);
    void* m = nullptr;
    bufZero->Map(0, nullptr, &m);
    memset(m, 0, 256);
    bufZero->Unmap(0, nullptr);
}

void Fluxo::Estatisticas(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex& fed, Tex& fedAnt)
{
    Barreira(c, bufEstat.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    c->CopyBufferRegion(bufEstat.Get(), 0, bufZero.Get(), 0, 16);
    Barreira(c, bufEstat.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    fedAnt.Para(c, SRV);
    K k{w, h, 0, 0, 0, 0, 0, 0};
    const UINT gx = ((w + 1) / 2 + 15) / 16, gy = ((h + 1) / 2 + 15) / 16;
    pEstat.Roda(c, desc, d,
                {{mov.r.Get(), mov.fmt}, {conf.r.Get(), conf.fmt}, {fed.r.Get(), fed.fmt}, {fedAnt.r.Get(), fedAnt.fmt}},
                {{bufEstat.Get(), DXGI_FORMAT_UNKNOWN}}, &k, gx, gy);
    Barreira(c, bufEstat.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    c->CopyBufferRegion(bufLeitura.Get(), 0, bufEstat.Get(), 0, 16);
    Barreira(c, bufEstat.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    estatGravada = true;
}

Fluxo::Estat Fluxo::LeEstatisticas()
{
    Estat e;
    if (!estatGravada)
        return e;
    estatGravada = false;
    uint32_t* v = nullptr;
    D3D12_RANGE r{0, 16};
    if (FAILED(bufLeitura->Map(0, &r, (void**)&v)))
        return e;
    const double n = v[3] ? double(v[3]) : 1.0;
    e.semConfianca = float(v[0] / 1024.0 / n);
    e.velocidade = float(v[1] / 4.0 / n);
    e.mudancaLuz = float(v[2] / 1024.0 / n);
    e.valida = v[3] > 0;
    D3D12_RANGE nada{0, 0};
    bufLeitura->Unmap(0, &nada);
    return e;
}

void Fluxo::Garante(ID3D12Device* d, UINT w_, UINT h_)
{
    if (w_ == w && h_ == h)
        return;
    w = w_;
    h = h_;
    UINT lw = std::max(1u, w / 2), lh = std::max(1u, h / 2);
    niveis = 0;
    for (UINT n = 0; n < kMaxNiveis; ++n)
    {
        for (int q = 0; q < 2; ++q)
            luma[q][n].Cria(d, lw, lh, DXGI_FORMAT_R16_FLOAT, L"fluxo luma");
        ++niveis;
        if (lw <= 40 || lh <= 24)
            break;
        lw = std::max(1u, lw / 2);
        lh = std::max(1u, lh / 2);
    }
    for (UINT n = 0; n < niveis; ++n)
    {
        const UINT nw = luma[0][n].w, nh = luma[0][n].h;
        campoIda[n].Cria(d, nw, nh, DXGI_FORMAT_R16G16_FLOAT, L"fluxo campo ida");
        tmpIda[n].Cria(d, nw, nh, DXGI_FORMAT_R16G16_FLOAT, L"fluxo mediana ida");
        campoVolta[n].Cria(d, nw, nh, DXGI_FORMAT_R16G16_FLOAT, L"fluxo campo volta");
        tmpVolta[n].Cria(d, nw, nh, DXGI_FORMAT_R16G16_FLOAT, L"fluxo mediana volta");
    }
    rascunho.Cria(d, 1, 1, DXGI_FORMAT_R16G16_FLOAT, L"fluxo rascunho");
    modeloIda.Cria(d, 2, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, L"fluxo modelo global ida");
    modeloVolta.Cria(d, 2, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, L"fluxo modelo global volta");
    nivelGlobal = 0;
    while (nivelGlobal + 1 < niveis && luma[0][nivelGlobal].w > 320)
        ++nivelGlobal;
    mov.Cria(d, w, h, DXGI_FORMAT_R16G16_FLOAT, L"movimento");
    movBruto.Cria(d, w, h, DXGI_FORMAT_R16G16_FLOAT, L"movimento da busca");
    for (int q = 0; q < 2; ++q)
    {
        lumaCheia[q].Cria(d, w, h, DXGI_FORMAT_R16_FLOAT, L"fluxo luma cheia");
        gradCheia[q].Cria(d, w, h, DXGI_FORMAT_R16G16_FLOAT, L"fluxo gradiente");
    }
    movVolta.Cria(d, w, h, DXGI_FORMAT_R16G16_FLOAT, L"movimento de volta");
    conf.Cria(d, w, h, DXGI_FORMAT_R16_FLOAT, L"confianca do movimento");
    temAnterior = false;
    Log("fluxo", Fmt("fluxo optico %ux%u, %u niveis (nivel 0 = %ux%u), ida e volta", w, h, niveis, luma[0][0].w,
                     luma[0][0].h));
}

void Fluxo::Busca(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, UINT de, UINT para, Tex* campos,
                  Tex* tmps, Tex& saida, Tex& modelo)
{
    // do grosso ao fino: busca (prevista pelo nivel de baixo) + mediana vetorial em cada nivel
    for (int n = int(niveis) - 1; n >= 0; --n)
    {
        auto& la = luma[de][n];
        auto& lp = luma[para][n];
        Tex& sai = campos[n];
        sai.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const bool temGrosso = n + 1 < int(niveis);
        Tex& grosso = temGrosso ? campos[n + 1] : campos[n];
        if (temGrosso)
            grosso.Para(c, SRV);
        const bool usaGlobal = n < int(nivelGlobal);
        KBusca k{la.w, la.h, temGrosso ? grosso.w : 1u, temGrosso ? grosso.h : 1u, n == int(niveis) - 1 ? 4 : 1,
                 temGrosso ? 1u : 0u, 0.02f, n == 0 ? 0u : 1u, usaGlobal ? 1u : 0u,
                 float(la.w) / float(luma[0][nivelGlobal].w), 0.5f * float(luma[0][nivelGlobal].w), 0};
        modelo.Para(c, SRV);
        // no nivel mais grosso nao ha previsao; o SRV e o UAV do mesmo recurso nao podem coexistir: usa o rascunho
        ID3D12Resource* srvGrosso = temGrosso ? grosso.r.Get() : rascunho.r.Get();
        if (!temGrosso)
            rascunho.Para(c, SRV);
        pBusca.Roda(c, desc, d,
                    {{la.r.Get(), DXGI_FORMAT_R16_FLOAT}, {lp.r.Get(), DXGI_FORMAT_R16_FLOAT},
                     {srvGrosso, DXGI_FORMAT_R16G16_FLOAT}, {modelo.r.Get(), modelo.fmt}},
                    {{sai.r.Get(), sai.fmt}}, &k, (la.w + 7) / 8, (la.h + 7) / 8);
        sai.Para(c, SRV);
        Tex& tmp = tmps[n];
        tmp.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        pMediana.Roda(c, desc, d, {{sai.r.Get(), sai.fmt}}, {{tmp.r.Get(), tmp.fmt}}, &k, (la.w + 7) / 8,
                      (la.h + 7) / 8);
        tmp.Para(c, D3D12_RESOURCE_STATE_COPY_SOURCE);
        sai.Para(c, D3D12_RESOURCE_STATE_COPY_DEST);
        c->CopyResource(sai.r.Get(), tmp.r.Get());
        sai.Para(c, SRV);
        if (n == int(nivelGlobal) && nivelGlobal > 0)
        {
            // modelo global a partir deste nivel, para os niveis mais finos
            modelo.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            K kg{sai.w, sai.h, 0, 0, 0, 0, 0, 0};
            pGlobal.Roda(c, desc, d, {{sai.r.Get(), DXGI_FORMAT_R16G16_FLOAT}}, {{modelo.r.Get(), modelo.fmt}}, &kg, 1, 1);
            modelo.Para(c, SRV);
        }
    }
    // nivel 0 -> tamanho da rede
    saida.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    K k{w, h, campos[0].w, campos[0].h, 0, 0, 0, 0};
    pAmplia.Roda(c, desc, d, {{campos[0].r.Get(), DXGI_FORMAT_R16G16_FLOAT}}, {{saida.r.Get(), saida.fmt}}, &k,
                 (w + 7) / 8, (h + 7) / 8);
    saida.Para(c, SRV);
}

void Fluxo::Refina(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, UINT de, UINT para, Tex& bruto,
                   Tex& saida)
{
    lumaCheia[de].Para(c, SRV);
    gradCheia[de].Para(c, SRV);
    lumaCheia[para].Para(c, SRV);
    bruto.Para(c, SRV);
    saida.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    K k{w, h, 0, 0, 0, 0, 0, 0};
    pRefina.Roda(c, desc, d,
                 {{lumaCheia[de].r.Get(), DXGI_FORMAT_R16_FLOAT},
                  {gradCheia[de].r.Get(), DXGI_FORMAT_R16G16_FLOAT},
                  {lumaCheia[para].r.Get(), DXGI_FORMAT_R16_FLOAT},
                  {bruto.r.Get(), DXGI_FORMAT_R16G16_FLOAT}},
                 {{saida.r.Get(), saida.fmt}}, &k, (w + 7) / 8, (h + 7) / 8);
    saida.Para(c, SRV);
}

bool Fluxo::Calcula(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, ID3D12Resource* cor,
                    DXGI_FORMAT fmtCor)
{
    const UINT a = atual, p = atual ^ 1;
    // piramide do quadro atual
    {
        auto& t = luma[a][0];
        t.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        K k{t.w, t.h, w, h, 0, 0, 0, 0};
        pLuma.Roda(c, desc, d, {{cor, fmtCor}}, {{t.r.Get(), t.fmt}}, &k, (t.w + 7) / 8, (t.h + 7) / 8);
        t.Para(c, SRV);
    }
    {
        // luminancia e gradiente em resolucao cheia (modelo do refinamento)
        auto& t = lumaCheia[a];
        t.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        K k{w, h, w, h, 0, 0, 0, 0};
        pLuma.Roda(c, desc, d, {{cor, fmtCor}}, {{t.r.Get(), t.fmt}}, &k, (w + 7) / 8, (h + 7) / 8);
        t.Para(c, SRV);
        auto& g = gradCheia[a];
        g.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        pGradiente.Roda(c, desc, d, {{t.r.Get(), DXGI_FORMAT_R16_FLOAT}}, {{g.r.Get(), g.fmt}}, &k, (w + 7) / 8,
                        (h + 7) / 8);
        g.Para(c, SRV);
    }
    for (UINT n = 1; n < niveis; ++n)
    {
        auto& t = luma[a][n];
        t.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        K k{t.w, t.h, luma[a][n - 1].w, luma[a][n - 1].h, 0, 0, 0, 0};
        pReduzLuma.Roda(c, desc, d, {{luma[a][n - 1].r.Get(), DXGI_FORMAT_R16_FLOAT}}, {{t.r.Get(), t.fmt}}, &k,
                        (t.w + 7) / 8, (t.h + 7) / 8);
        t.Para(c, SRV);
    }
    const bool tinha = temAnterior;
    temAnterior = true;
    atual ^= 1;
    if (!tinha)
        return false;
    Busca(c, desc, d, a, p, campoIda, tmpIda, movBruto, modeloIda);         // atual -> anterior
    Busca(c, desc, d, p, a, campoVolta, tmpVolta, movVolta, modeloVolta);   // anterior -> atual
    // so a ida e refinada: a volta serve a confianca, que so pesa onde a cor diverge (refinar as duas: mesma
    // qualidade medida, +1 ms a 1440p)
    Refina(c, desc, d, a, p, movBruto, mov);
    // confianca: ida + volta(no ponto de chegada) ~ 0
    conf.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    K k{w, h, 0, 0, 0, 0, 0, 0};
    pConfianca.Roda(c, desc, d, {{mov.r.Get(), mov.fmt}, {movVolta.r.Get(), movVolta.fmt}}, {{conf.r.Get(), conf.fmt}},
                    &k, (w + 7) / 8, (h + 7) / 8);
    conf.Para(c, SRV);
    return true;
}

} // namespace ov
