// Cena 3D do jogo de teste (--cubo), com movimento de jogo: uma sala xadrez fechada (piso, teto e 4 paredes: toda
// direcao tem geometria com movimento certo), a camera girando sem parar (--giro, padrao 120 graus/s: uma virada rapida
// de mouse, ~126 px por quadro a 20 q/s), 6 cubos em roda girando rapido e 2 cubos cruzando na frente deles no sentido
// contrario (o que fica escondido e aparece de repente: onde a geracao faz fantasma). Profundidade de verdade (Z
// invertido e infinito, como o contexto do FSR do jogo de teste) e o movimento de cada pixel pela posicao anterior do
// objeto e da camera (o que um motor entrega ao FSR: anterior - atual, em uv). O tempo vem do numero do quadro, entao
// qualquer instante pode ser desenhado de novo: a verdade dos gerados. --pontilhado: os 2 cubos que cruzam saem
// pontilhados (metade dos pixels, sorteio novo a cada instante), como a transparencia do cabelo nos jogos (TLOU2): o
// pixel ora e o objeto, ora o fundo, e o TAA do FSR junta -- onde a geracao trocava o movimento do jogo pelo do fluxo
// optico e colava blocos do fundo. --fundo-liso: a sala sem o xadrez (o ceu e a neve lisos atras do cabelo no TLOU2,
// onde qualquer bloco do fluxo optico "casa").
#pragma once
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstring>

class CenaCubo
{
  public:
    static constexpr DXGI_FORMAT kCor = DXGI_FORMAT_R16G16B16A16_FLOAT, kMov = DXGI_FORMAT_R32G32_FLOAT;
    static constexpr int kObjetos = 14; // 6 da sala, 6 cubos em roda, 2 cruzando
    float giro = 120.f;                 // graus por segundo da camera
    bool pontilhado = false;            // os cubos que cruzam pontilhados (--pontilhado)
    bool fundoLiso = false;             // a sala sem o xadrez (--fundo-liso)
    float cobertura = 0.5f;             // fracao dos pixels do pontilhado que ficam (--cobertura; fios soltos ~0,25)

    bool Cria(ID3D12Device* d)
    {
        dev = d;
        Microsoft::WRL::ComPtr<ID3DBlob> vs, ps, err, ser;
        if (FAILED(D3DCompile(kHlsl, sizeof kHlsl - 1, "cubo", nullptr, nullptr, "VSCubo", "vs_5_1", 0, 0, &vs, &err)) ||
            FAILED(D3DCompile(kHlsl, sizeof kHlsl - 1, "cubo", nullptr, nullptr, "PSCubo", "ps_5_1", 0, 0, &ps, &err)))
        {
            fprintf(stderr, "cubo: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p.Constants.Num32BitValues = 56;
        D3D12_ROOT_SIGNATURE_DESC rd{1, &p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &ser, &err)) ||
            FAILED(d->CreateRootSignature(0, ser->GetBufferPointer(), ser->GetBufferSize(), IID_PPV_ARGS(&raiz))))
            return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC g{};
        g.pRootSignature = raiz.Get();
        g.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        g.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        for (auto& m : g.BlendState.RenderTarget)
            m.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        g.SampleMask = UINT_MAX;
        g.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        g.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        g.RasterizerState.DepthClipEnable = TRUE;
        g.DepthStencilState.DepthEnable = TRUE;
        g.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        g.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER; // Z invertido: perto = 1
        g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        g.NumRenderTargets = 2;
        g.RTVFormats[0] = kCor;
        g.RTVFormats[1] = kMov;
        g.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        g.SampleDesc.Count = 1;
        if (FAILED(d->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&pso))))
            return false;
        D3D12_DESCRIPTOR_HEAP_DESC hr{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(d->CreateDescriptorHeap(&hr, IID_PPV_ARGS(&rtv))) || FAILED(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv))))
            return false;
        incRtv = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        incDsv = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        return true;
    }

    // os alvos de uma vaga (0 o jogo, 1 a verdade): cor kCor, movimento kMov, profundidade R32_TYPELESS (DSV D32)
    void Alvos(int vaga, ID3D12Resource* cor, ID3D12Resource* mov, ID3D12Resource* prof)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE r = Rtv(vaga, 0);
        dev->CreateRenderTargetView(cor, nullptr, r);
        dev->CreateRenderTargetView(mov, nullptr, Rtv(vaga, 1));
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dev->CreateDepthStencilView(prof, &dv, Dsv(vaga));
    }

    // desenha o instante t (s) nos alvos da vaga (cor e movimento em RENDER_TARGET, profundidade em DEPTH_WRITE), numa
    // area w x h; o movimento e desde o instante tAnt
    void Desenha(ID3D12GraphicsCommandList* c, int vaga, UINT w, UINT h, double t, double tAnt) const
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE alvos[2] = {Rtv(vaga, 0), Rtv(vaga, 1)};
        const D3D12_CPU_DESCRIPTOR_HANDLE prof = Dsv(vaga);
        const float ceu[4] = {0.5f, 0.7f, 1.0f, 1}, zero[4] = {};
        c->ClearRenderTargetView(alvos[0], ceu, 0, nullptr);
        c->ClearRenderTargetView(alvos[1], zero, 0, nullptr);
        c->ClearDepthStencilView(prof, D3D12_CLEAR_FLAG_DEPTH, 0.f, 0, 0, nullptr);
        c->OMSetRenderTargets(2, alvos, FALSE, &prof);
        const D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
        const D3D12_RECT sr{0, 0, LONG(w), LONG(h)};
        c->RSSetViewports(1, &vp);
        c->RSSetScissorRects(1, &sr);
        c->SetGraphicsRootSignature(raiz.Get());
        c->SetPipelineState(pso.Get());
        c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const float aspecto = float(w) / float(h);
        const M4 vpAtual = Camera(t, aspecto), vpAnt = Camera(tAnt, aspecto);
        for (int o = 0; o < kObjetos; ++o)
        {
            float cor[4];
            const M4 m = Objeto(o, t, cor), mAnt = Objeto(o, tAnt, cor);
            if (pontilhado && o >= 12) // cabelo: escuro sobre o fundo claro
                cor[0] = 0.12f, cor[1] = 0.08f, cor[2] = 0.05f;
            struct
            {
                M4 mvp, mvpAnt, mundo;
                float cor[4];
                float pont, semente, pad[2];
            } k{Mul(vpAtual, m), Mul(vpAnt, mAnt), m, {cor[0], cor[1], cor[2], cor[3]}, pontilhado && o >= 12 ? cobertura : 0.f,
                float(std::fmod(t * 1000.0, 65536.0)), {fundoLiso && o < 6 ? 1.f : 0.f, 0.f}};
            c->SetGraphicsRoot32BitConstants(0, 56, &k, 0);
            c->DrawInstanced(o < 6 ? 6 : 36, 1, 0, 0);
        }
    }

  private:
    struct M4
    {
        float m[4][4];
    };
    static M4 Id()
    {
        M4 r{};
        for (int i = 0; i < 4; ++i)
            r.m[i][i] = 1;
        return r;
    }
    static M4 Mul(const M4& a, const M4& b)
    {
        M4 r{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    r.m[i][j] += a.m[i][k] * b.m[k][j];
        return r;
    }
    static M4 Desloca(float x, float y, float z)
    {
        M4 r = Id();
        r.m[0][3] = x, r.m[1][3] = y, r.m[2][3] = z;
        return r;
    }
    static M4 Escala(float x, float y, float z)
    {
        M4 r = Id();
        r.m[0][0] = x, r.m[1][1] = y, r.m[2][2] = z;
        return r;
    }
    static M4 GiraX(float a)
    {
        M4 r = Id();
        r.m[1][1] = std::cos(a), r.m[1][2] = -std::sin(a), r.m[2][1] = std::sin(a), r.m[2][2] = std::cos(a);
        return r;
    }
    static M4 GiraY(float a)
    {
        M4 r = Id();
        r.m[0][0] = std::cos(a), r.m[0][2] = std::sin(a), r.m[2][0] = -std::sin(a), r.m[2][2] = std::cos(a);
        return r;
    }
    // a camera no meio da sala girando sem parar (giro graus/s), um pouco para baixo e balancando de leve; projecao com Z
    // invertido e infinito (perto 0,1, 60 graus)
    M4 Camera(double t, float aspecto) const
    {
        const double a = double(giro) * 3.14159265358979 / 180.0 * t, inc = -0.18;
        const float ex = 0.6f * float(std::sin(0.9 * t)), ey = 2.2f + 0.15f * float(std::sin(1.7 * t)), ez = 0.f;
        float z[3] = {float(std::sin(a) * std::cos(inc)), float(std::sin(inc)), float(std::cos(a) * std::cos(inc))};
        float x[3] = {z[2], 0.f, -z[0]}; // cima (0,1,0) x z
        const float nx = std::sqrt(x[0] * x[0] + x[2] * x[2]);
        x[0] /= nx, x[2] /= nx;
        const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
        M4 v = Id();
        const float e[3] = {ex, ey, ez};
        for (int j = 0; j < 3; ++j)
            v.m[0][j] = x[j], v.m[1][j] = y[j], v.m[2][j] = z[j];
        for (int i = 0; i < 3; ++i)
            v.m[i][3] = -(v.m[i][0] * e[0] + v.m[i][1] * e[1] + v.m[i][2] * e[2]);
        const float f = 1.f / std::tan(0.5f * 1.0472f), perto = 0.1f;
        M4 p{};
        p.m[0][0] = f / aspecto, p.m[1][1] = f, p.m[2][3] = perto, p.m[3][2] = 1.f;
        return Mul(p, v);
    }
    // 0..5 a sala (quadrados [-1,1] no plano xy, normal -z virada para dentro): piso, teto, paredes +z, -z, +x, -x;
    // 6..11 cubos em roda (raio 7) girando rapido; 12..13 cubos cruzando na roda de dentro (raio 4) no sentido contrario
    // ao da camera. cor[3] = 0 plano, 1 cubo
    static M4 Objeto(int o, double t, float* cor)
    {
        const float s = float(t), q = 1.5707963f, L = 30.f, A = 6.f;
        cor[3] = o < 6 ? 0.f : 1.f;
        switch (o)
        {
        case 0:
            cor[0] = 0.8f, cor[1] = 0.75f, cor[2] = 0.6f;
            return Mul(GiraX(q), Escala(L, L, 1));
        case 1:
            cor[0] = 0.7f, cor[1] = 0.7f, cor[2] = 0.7f;
            return Mul(Desloca(0, 2 * A, 0), Mul(GiraX(-q), Escala(L, L, 1)));
        case 2:
            cor[0] = 0.6f, cor[1] = 0.65f, cor[2] = 0.85f;
            return Mul(Desloca(0, A, L), Escala(L, A, 1));
        case 3:
            cor[0] = 0.85f, cor[1] = 0.6f, cor[2] = 0.6f;
            return Mul(Desloca(0, A, -L), Mul(GiraY(2 * q), Escala(L, A, 1)));
        case 4:
            cor[0] = 0.6f, cor[1] = 0.85f, cor[2] = 0.6f;
            return Mul(Desloca(L, A, 0), Mul(GiraY(q), Escala(L, A, 1)));
        case 5:
            cor[0] = 0.85f, cor[1] = 0.8f, cor[2] = 0.5f;
            return Mul(Desloca(-L, A, 0), Mul(GiraY(-q), Escala(L, A, 1)));
        default:
            break;
        }
        static const float kCores[8][3] = {{1, 0.3f, 0.2f}, {0.2f, 0.9f, 0.3f}, {0.3f, 0.4f, 1},   {1, 0.85f, 0.2f},
                                           {0.8f, 0.3f, 1}, {0.2f, 0.9f, 0.9f}, {1, 1, 1}, {1, 0.5f, 0.1f}};
        const int i = o - 6;
        for (int k = 0; k < 3; ++k)
            cor[k] = kCores[i][k];
        if (i < 6) // em roda, girando e quicando
        {
            const float ang = float(i) * 1.0471976f;
            return Mul(Desloca(7.f * std::sin(ang), 1.1f + 0.6f * std::sin(3.f * s + float(i)), 7.f * std::cos(ang)),
                       Mul(GiraY(4.f * s + float(i)), Mul(GiraX(3.f * s), Escala(1.3f, 1.3f, 1.3f))));
        }
        const float ang = -3.f * s + float(i) * 3.14159265f; // contra o giro da camera
        return Mul(Desloca(4.f * std::sin(ang), 1.f, 4.f * std::cos(ang)), Mul(GiraY(-6.f * s), Escala(0.9f, 0.9f, 0.9f)));
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(int vaga, int i) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtv->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(vaga * 2 + i) * incRtv;
        return h;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv(int vaga) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = dsv->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(vaga) * incDsv;
        return h;
    }

    ID3D12Device* dev = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> raiz;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv, dsv;
    UINT incRtv = 0, incDsv = 0;

    static constexpr char kHlsl[] = R"(
cbuffer o : register(b0) { row_major float4x4 mvp; row_major float4x4 mvpAnt; row_major float4x4 mundo; float4 corBase;
                           float pont; float semente; float liso; float pad; };
struct V { float4 pos : SV_Position; float4 atual : TEXCOORD0; float4 ant : TEXCOORD1; float3 n : NORMAL; float3 local : TEXCOORD2; float3 w : TEXCOORD3; };
static const float3 kCubo[8] = { float3(-.5,-.5,-.5), float3(.5,-.5,-.5), float3(.5,.5,-.5), float3(-.5,.5,-.5),
                                 float3(-.5,-.5,.5), float3(.5,-.5,.5), float3(.5,.5,.5), float3(-.5,.5,.5) };
static const uint kFaces[36] = { 0,1,2, 0,2,3,  5,4,7, 5,7,6,  4,0,3, 4,3,7,  1,5,6, 1,6,2,  3,2,6, 3,6,7,  4,5,1, 4,1,0 };
static const float3 kNormais[6] = { float3(0,0,-1), float3(0,0,1), float3(-1,0,0), float3(1,0,0), float3(0,1,0), float3(0,-1,0) };
V VSCubo(uint v : SV_VertexID)
{
    float3 p, n;
    if (corBase.w < 0.5)
    {
        const float2 q[6] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(1,-1), float2(1,1), float2(-1,1) };
        p = float3(q[v], 0);
        n = float3(0, 0, -1);
    }
    else
    {
        p = kCubo[kFaces[v]];
        n = kNormais[v / 6];
    }
    V o;
    o.atual = mul(mvp, float4(p, 1));
    o.ant = mul(mvpAnt, float4(p, 1));
    o.pos = o.atual;
    o.n = mul((float3x3)mundo, n);
    o.local = p;
    o.w = mul(mundo, float4(p, 1)).xyz;
    return o;
}
struct S { float4 cor : SV_Target0; float2 mov : SV_Target1; };
S PSCubo(V i)
{
    if (pont > 0.0)
    {
        // pontilhado: a fracao pont dos pixels fica, sorteio (PCG) por pixel e por instante; o descartado deixa o fundo (cor,
        // profundidade e movimento dele)
        uint h = (uint(i.pos.x) * 1973u + uint(i.pos.y) * 9277u + uint(semente) * 26699u) | 1u;
        h = h * 747796405u + 2891336453u;
        h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
        if (float(((h >> 22u) ^ h) & 65535u) / 65536.0 >= pont) discard;
    }
    const float3 c = corBase.w < 0.5 ? i.w : i.local * 4;
    const int3 k = int3(floor(c + 0.0001));
    const float xadrez = liso > 0.5 ? 1.4 : ((k.x + k.y + k.z) & 1) ? 1.0 : 0.3;
    const float luz = 0.3 + 0.7 * saturate(dot(normalize(i.n), normalize(float3(0.4, 1.0, -0.6))));
    S s;
    s.cor = float4(corBase.rgb * xadrez * luz * 2.0, 1);
    s.mov = (i.ant.xy / i.ant.w - i.atual.xy / i.atual.w) * float2(0.5, -0.5);
    return s;
}
)";
};
