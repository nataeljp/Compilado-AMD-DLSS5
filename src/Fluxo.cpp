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
const D3D12_RESOURCE_STATES SRV =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
} // namespace

void Fluxo::Cria(ID3D12Device* d)
{
    pLuma.Cria(d, hlsl::Fluxo, "CSLuma", 1, 1, 8, "fluxo luma");
    pReduzLuma.Cria(d, hlsl::ReduzLuma, "CSReduzLuma", 1, 1, 8, "fluxo reduz");
    pBusca.Cria(d, hlsl::Busca, "CSBusca", 3, 1, 8, "fluxo busca");
    pMediana.Cria(d, hlsl::Mediana, "CSMediana", 1, 1, 8, "fluxo mediana");
    pAmplia.Cria(d, hlsl::Amplia, "CSAmplia", 1, 1, 8, "fluxo amplia");
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
        nivelCampo[n].Cria(d, luma[0][n].w, luma[0][n].h, DXGI_FORMAT_R16G16_FLOAT, L"fluxo campo");
        nivelTmp[n].Cria(d, luma[0][n].w, luma[0][n].h, DXGI_FORMAT_R16G16_FLOAT, L"fluxo mediana");
    }
    rascunho.Cria(d, 1, 1, DXGI_FORMAT_R16G16_FLOAT, L"fluxo rascunho");
    mov.Cria(d, w, h, DXGI_FORMAT_R16G16_FLOAT, L"movimento");
    temAnterior = false;
    Log("fluxo", Fmt("fluxo optico %ux%u, %u niveis (nivel 0 = %ux%u)", w, h, niveis, luma[0][0].w, luma[0][0].h));
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

    // do grosso ao fino: busca (prevista pelo nivel de baixo) + mediana vetorial em cada nivel
    for (int n = int(niveis) - 1; n >= 0; --n)
    {
        auto& la = luma[a][n];
        auto& lp = luma[p][n];
        Tex& sai = nivelCampo[n];
        sai.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const bool temGrosso = n + 1 < int(niveis);
        Tex& grosso = temGrosso ? nivelCampo[n + 1] : nivelCampo[n];
        if (temGrosso)
            grosso.Para(c, SRV);
        K k{la.w, la.h, temGrosso ? grosso.w : 1u, temGrosso ? grosso.h : 1u, n == int(niveis) - 1 ? 4 : 1,
            temGrosso ? 1u : 0u, 0.02f, 0};
        // no nivel mais grosso o proprio `sai` entra como "grosso" (nao e lido: temGrosso = 0), mas o SRV e o UAV do
        // mesmo recurso nao podem coexistir: usa o rascunho
        ID3D12Resource* srvGrosso = temGrosso ? grosso.r.Get() : rascunho.r.Get();
        if (!temGrosso)
            rascunho.Para(c, SRV);
        pBusca.Roda(c, desc, d,
                    {{la.r.Get(), DXGI_FORMAT_R16_FLOAT}, {lp.r.Get(), DXGI_FORMAT_R16_FLOAT},
                     {srvGrosso, DXGI_FORMAT_R16G16_FLOAT}},
                    {{sai.r.Get(), sai.fmt}}, &k, (la.w + 7) / 8, (la.h + 7) / 8);
        // mediana: sai -> rascunho do mesmo tamanho -> de volta
        sai.Para(c, SRV);
        Tex& tmp = nivelTmp[n];
        tmp.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        pMediana.Roda(c, desc, d, {{sai.r.Get(), sai.fmt}}, {{tmp.r.Get(), tmp.fmt}}, &k, (la.w + 7) / 8,
                      (la.h + 7) / 8);
        tmp.Para(c, D3D12_RESOURCE_STATE_COPY_SOURCE);
        sai.Para(c, D3D12_RESOURCE_STATE_COPY_DEST);
        c->CopyResource(sai.r.Get(), tmp.r.Get());
        sai.Para(c, SRV);
    }
    // nivel 0 -> tamanho da rede
    mov.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    K k{w, h, nivelCampo[0].w, nivelCampo[0].h, 0, 0, 0, 0};
    pAmplia.Roda(c, desc, d, {{nivelCampo[0].r.Get(), DXGI_FORMAT_R16G16_FLOAT}}, {{mov.r.Get(), mov.fmt}}, &k,
                 (w + 7) / 8, (h + 7) / 8);
    mov.Para(c, SRV);
    return true;
}

} // namespace ov
