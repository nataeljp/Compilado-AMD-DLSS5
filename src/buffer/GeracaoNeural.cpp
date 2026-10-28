#include "GeracaoNeural.h"
#include "../../externo/ffx/ffx_api.h"
#include "../../externo/ffx/dx12/ffx_api_dx12.h"
#include <d3dcompiler.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace dlss5ov {

namespace {
// a parte da geracao de quadros da API do FidelityFX (ffx_framegeneration.h, SDK 2.3 / Frame Generation 4.0.1, MIT) que o
// modulo usa: os mesmos tipos e layouts, os ponteiros de funcao como void*
constexpr uint64_t kCriaFg = 0x00020001u;      // FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION
constexpr uint64_t kConfiguraFg = 0x00020002u; // FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION
constexpr uint64_t kDespachaFg = 0x00020003u;  // FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION
constexpr uint64_t kPreparo = 0x00020004u;     // FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE (antigo)
constexpr uint64_t kCamera = 0x0002000au;      // FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO
constexpr uint64_t kPreparoV2 = 0x0002000cu;   // FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2
constexpr uint64_t kVersaoFg = 0x0002000eu;    // FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION
constexpr uint32_t kSemNotificarSwapchain = 1u << 3; // FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY
constexpr uint32_t kVersaoApi = (4u << 22) | (0u << 12) | 1u; // FFX_FRAMEGENERATION_VERSION (4.0.1)

struct CriaFg
{
    ffxCreateContextDescHeader header;
    uint32_t flags;
    FfxApiDimensions2D displaySize;
    FfxApiDimensions2D maxRenderSize;
    uint32_t backBufferFormat;
};
struct VersaoFg
{
    ffxCreateContextDescHeader header;
    uint32_t version;
};
struct ConfiguraFg
{
    ffxConfigureDescHeader header;
    void* swapChain;
    void* presentCallback;
    void* presentCallbackUserContext;
    void* frameGenerationCallback;
    void* frameGenerationCallbackUserContext;
    bool frameGenerationEnabled;
    bool allowAsyncWorkloads;
    FfxApiResource HUDLessColor;
    uint32_t flags;
    bool onlyPresentGenerated;
    FfxApiRect2D generationRect;
    uint64_t frameID;
};
struct DespachaFg
{
    ffxDispatchDescHeader header;
    void* commandList;
    FfxApiResource presentColor;
    FfxApiResource outputs[4];
    uint32_t numGeneratedFrames;
    bool reset;
    uint32_t backbufferTransferFunction;
    float minMaxLuminance[2];
    FfxApiRect2D generationRect;
    uint64_t frameID;
};
struct PreparoV2
{
    ffxDispatchDescHeader header;
    uint64_t frameID;
    uint32_t flags;
    void* commandList;
    FfxApiDimensions2D renderSize;
    FfxApiFloatCoords2D jitterOffset;
    FfxApiFloatCoords2D motionVectorScale;
    float frameTimeDelta;
    bool reset;
    float cameraNear, cameraFar, cameraFovAngleVertical, viewSpaceToMetersFactor;
    FfxApiResource depth;
    FfxApiResource motionVectors;
    float cameraPosition[3], cameraUp[3], cameraRight[3], cameraForward[3];
};
struct CameraInfo // ligada depois do preparo antigo (FSR 3.1.4/3.1.5)
{
    ffxConfigureDescHeader header;
    float cameraPosition[3], cameraUp[3], cameraRight[3], cameraForward[3];
};

void Copia(const float* de, float* para)
{
    memcpy(para, de, 3 * sizeof(float));
}

// a camera no meio do caminho entre duas (o 4x: o quadro M entre o real anterior e este)
CameraJogo Meio(const CameraJogo& x, const CameraJogo& y)
{
    CameraJogo m;
    auto meio = [](const float* a, const float* b, float* o, bool normaliza) {
        for (int i = 0; i < 3; ++i)
            o[i] = 0.5f * (a[i] + b[i]);
        const float l = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
        if (normaliza && l > 1e-6f)
            for (int i = 0; i < 3; ++i)
                o[i] /= l;
    };
    meio(x.pos, y.pos, m.pos, false), meio(x.cima, y.cima, m.cima, true);
    meio(x.direita, y.direita, m.direita, true), meio(x.frente, y.frente, m.frente, true);
    return m;
}
} // namespace

bool CameraDoPreparo(const void* desc, CameraJogo& cam)
{
    const auto* h = static_cast<const ffxApiHeader*>(desc);
    if (!h)
        return false;
    if (h->type == kPreparoV2)
    {
        const auto* p = static_cast<const PreparoV2*>(desc);
        Copia(p->cameraPosition, cam.pos), Copia(p->cameraUp, cam.cima), Copia(p->cameraRight, cam.direita);
        Copia(p->cameraForward, cam.frente);
        return true;
    }
    if (h->type != kPreparo)
        return false;
    for (const ffxApiHeader* n = h->pNext; n; n = n->pNext)
        if (n->type == kCamera)
        {
            const auto* c = reinterpret_cast<const CameraInfo*>(n);
            Copia(c->cameraPosition, cam.pos), Copia(c->cameraUp, cam.cima), Copia(c->cameraRight, cam.direita);
            Copia(c->cameraForward, cam.frente);
            return true;
        }
    return false;
}

bool GeracaoNeural::Cria(const ApiAmd& api_, ID3D12Device* dev, UINT w, UINT h, UINT mrw, UINT mrh, DXGI_FORMAT fmt,
                         uint32_t flags)
{
    Destroi();
    api = api_;
    erro[0] = provedor[0] = 0;
    if (!api.Tem() || !dev)
        return snprintf(erro, sizeof erro, "sem a API da AMD (o loader do SDK 2 na pasta do jogo)"), false;
    const auto consulta = reinterpret_cast<PfnFfxQuery>(api.consulta);
    const auto cria = reinterpret_cast<PfnFfxCreateContext>(api.cria);
    // o provedor neural (4.x) entre as versoes que o loader oferece nesta placa (fora do RDNA 4 so ha o 3.1.x)
    ffxQueryDescGetVersions qv{};
    qv.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    qv.createDescType = kCriaFg;
    qv.device = dev;
    uint64_t n = 0;
    qv.outputCount = &n;
    consulta(nullptr, &qv.header);
    std::vector<uint64_t> ids(static_cast<size_t>(n));
    std::vector<const char*> nomes(static_cast<size_t>(n));
    qv.versionIds = ids.data();
    qv.versionNames = nomes.data();
    if (n)
        consulta(nullptr, &qv.header);
    uint64_t escolhido = 0;
    for (size_t i = 0; i < ids.size(); ++i)
        if (nomes[i] && nomes[i][0] >= '4' && nomes[i][0] <= '9')
        {
            escolhido = ids[i];
            snprintf(provedor, sizeof provedor, "%s", nomes[i]);
            break;
        }
    if (!escolhido)
        return snprintf(erro, sizeof erro, "o loader nao oferece a geracao neural (%llu versoes; so RDNA 4)",
                        (unsigned long long)n),
               false;
    CriaFg cd{};
    cd.header.type = kCriaFg;
    cd.flags = flags;
    cd.displaySize = {w, h};
    cd.maxRenderSize = {mrw, mrh};
    cd.backBufferFormat = ffxApiGetSurfaceFormatDX12(fmt);
    ffxCreateBackendDX12Desc bd{};
    bd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    bd.device = dev;
    ffxOverrideVersion ov{};
    ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    ov.versionId = escolhido;
    VersaoFg vd{};
    vd.header.type = kVersaoFg;
    vd.version = kVersaoApi;
    cd.header.pNext = &bd.header;
    bd.header.pNext = &ov.header;
    ov.header.pNext = &vd.header;
    ffxContext ca = nullptr, cb = nullptr;
    ffxReturnCode_t r = cria(&ca, &cd.header, nullptr);
    if (r == FFX_API_RETURN_OK)
        r = cria(&cb, &cd.header, nullptr);
    if (r != FFX_API_RETURN_OK)
    {
        const auto destroi = reinterpret_cast<PfnFfxDestroyContext>(api.destroi);
        if (ca)
            destroi(&ca, nullptr);
        return snprintf(erro, sizeof erro, "contexto da geracao neural %s: codigo %u", provedor, r), false;
    }
    a = ca, b = cb, telaW = w, telaH = h, ultimoA = ultimoB = 0;
    this->dev = dev;
    if (!CriaUi())
        snprintf(erro, sizeof erro, "o passe do HUD do 4x nao compilou (4x fica sem a neural)");
    return true;
}

// o HUD do real nos gerados do 4x: onde o final difere da imagem sem HUD, o final; o resto fica como gerado
static const char kRepro[] = R"(
cbuffer K : register(b0)
{
    float2 escB, escA; uint2 renderB, renderA; uint2 tela; float t, limite; uint flags; uint3 _p;
};
Texture2D<float4> corB : register(t0);
Texture2D<float2> movB : register(t1);
Texture2D<float> profB : register(t2);
Texture2D<float4> corA : register(t3);
Texture2D<float2> movA : register(t4);
Texture2D<float> profA : register(t5);
Texture2D<float4> final_ : register(t6);
Texture2D<float4> semHud : register(t7);
RWTexture2D<uint> chave : register(u0);
RWTexture2D<float4> rep : register(u1);
RWTexture2D<float4> saida : register(u2);
static const uint VAZIO = 0xffffffffu;
uint2 R(int2 q, uint2 render) { return min(uint2((float2(q) + 0.5) * float2(render) / float2(tela)), render - 1); }
// a chave: a profundidade (perto = menor) com o bit baixo = o real (0 ganha o empate: o mais perto no tempo)
uint Chave(float z, uint real)
{
    const float d = (flags & 1) ? 1.0 - z : z;
    const uint pref = t >= 0.5 ? 0u : 1u; // 0 = atual, 1 = anterior
    return (asuint(saturate(d)) & ~1u) | (real == pref ? 0u : 1u);
}
bool Destino(int2 q, bool atual, out int2 d, out uint k)
{
    if (atual)
    {
        const uint2 r = R(q, renderB);
        d = int2(floor(float2(q) + 0.5 + (1.0 - t) * movB[r] * escB));
        k = Chave(profB[r], 0);
    }
    else
    {
        const uint2 r = R(q, renderA);
        d = int2(floor(float2(q) + 0.5 - t * movA[r] * escA));
        k = Chave(profA[r], 1);
    }
    return all(d >= 0) && all(d < int2(tela));
}
[numthreads(8, 8, 1)] void CSLimpa(uint2 p : SV_DispatchThreadID)
{
    if (all(p < tela)) chave[p] = VAZIO;
}
[numthreads(8, 8, 1)] void CSEspalhaB(uint2 p : SV_DispatchThreadID)
{
    int2 d; uint k;
    if (all(p < tela) && Destino(int2(p), true, d, k)) InterlockedMin(chave[d], k);
}
[numthreads(8, 8, 1)] void CSEspalhaA(uint2 p : SV_DispatchThreadID)
{
    int2 d; uint k;
    if (all(p < tela) && (flags & 4) && Destino(int2(p), false, d, k)) InterlockedMin(chave[d], k);
}
[numthreads(8, 8, 1)] void CSCorB(uint2 p : SV_DispatchThreadID)
{
    int2 d; uint k;
    if (all(p < tela) && Destino(int2(p), true, d, k) && chave[d] == k) rep[d] = corB[p];
}
[numthreads(8, 8, 1)] void CSCorA(uint2 p : SV_DispatchThreadID)
{
    int2 d; uint k;
    if (all(p < tela) && (flags & 4) && Destino(int2(p), false, d, k) && chave[d] == k) rep[d] = corA[p];
}
[numthreads(8, 8, 1)] void CSResolve(uint2 p : SV_DispatchThreadID)
{
    if (any(p >= tela)) return;
    if ((flags & 2) && any(final_[p].rgb != semHud[p].rgb)) return; // o HUD: o da geracao (o real, parado)
    const int2 ultimo = int2(tela) - 1;
    // o maior movimento perto (a borda do que anda tambem quebra na geracao)
    float m = 0;
    for (int j = -8; j <= 8; j += 8)
        for (int i = -8; i <= 8; i += 8)
            m = max(m, length(movB[R(clamp(int2(p) + int2(i, j), 0, ultimo), renderB)] * escB));
    const float w = limite > 0 ? saturate((m - limite) / limite) : 1.0;
    if (w <= 0) return;
    float4 c;
    if (chave[p] != VAZIO)
        c = rep[p];
    else
    {
        // buraco: o fundo vizinho (a maior profundidade valida no anel mais proximo que tiver alguma)
        uint melhor = 0; int2 onde = int2(p); bool achou = false;
        for (int r = 1; r <= 8 && !achou; ++r)
            for (int j = -r; j <= r; ++j)
                for (int i = -r; i <= r; ++i)
                {
                    if (max(abs(i), abs(j)) != r) continue;
                    const int2 q = clamp(int2(p) + int2(i, j), 0, ultimo);
                    const uint k = chave[q];
                    if (k != VAZIO && k >= melhor) melhor = k, onde = q, achou = true;
                }
        c = achou ? rep[onde] : corB[p];
    }
    saida[p] = lerp(saida[p], c, w);
}
)";

bool Reprojecao::Cria(ID3D12Device* d)
{
    dev = d;
    static const char* entradas[6] = {"CSLimpa", "CSEspalhaB", "CSEspalhaA", "CSCorB", "CSCorA", "CSResolve"};
    Microsoft::WRL::ComPtr<ID3DBlob> cs[6], err, ser;
    for (int i = 0; i < 6; ++i)
        if (FAILED(D3DCompile(kRepro, sizeof kRepro - 1, "reprojecao", nullptr, nullptr, entradas[i], "cs_5_1", 0, 0, &cs[i],
                              &err)))
            return false;
    D3D12_DESCRIPTOR_RANGE rsr{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0, 0, 0}, ru{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 0};
    D3D12_ROOT_PARAMETER rp[3]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants = {0, 0, 16};
    rp[1].ParameterType = rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable = {1, &rsr};
    rp[2].DescriptorTable = {1, &ru};
    D3D12_ROOT_SIGNATURE_DESC rsd{3, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &ser, &err)) ||
        FAILED(dev->CreateRootSignature(0, ser->GetBufferPointer(), ser->GetBufferSize(), IID_PPV_ARGS(&rs))))
        return false;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ps[6];
    for (int i = 0; i < 6; ++i)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{rs.Get(), {cs[i]->GetBufferPointer(), cs[i]->GetBufferSize()}};
        if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&ps[i]))))
            return false;
    }
    // 11 descritores por gerado (8 SRV + 3 UAV), um anel para os quadros em voo
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 11 * 32, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))))
        return false;
    inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (int i = 0; i < 6; ++i)
        pso[i] = ps[i];
    return true;
}

static DXGI_FORMAT Tipado(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return f;
    }
}

bool Reprojecao::Garante(Microsoft::WRL::ComPtr<ID3D12Resource>& r, UINT w, UINT h, DXGI_FORMAT f, bool uav)
{
    if (r)
    {
        const D3D12_RESOURCE_DESC d = r->GetDesc();
        if (d.Width == w && d.Height == h && d.Format == f)
            return true;
    }
    D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, f, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                          uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE};
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    r = nullptr;
    return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                  uav ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                                      : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                  nullptr, IID_PPV_ARGS(&r)));
}

void Reprojecao::Gera(ID3D12GraphicsCommandList* c, uint64_t id, const QuadroNeural& q, bool profInvertida,
                      ID3D12Resource* final_, ID3D12Resource* semHud, ID3D12Resource* const* saidas, uint32_t m,
                      float limite, bool reset)
{
    if (!Pronta() || !final_ || !q.mov || !q.prof || !q.renderW || !q.renderH || !m || !saidas[0])
        return void(temAnterior = false);
    ID3D12Resource* fonte = semHud ? semHud : final_;
    const D3D12_RESOURCE_DESC fd = fonte->GetDesc(), sd = saidas[0]->GetDesc();
    const UINT tw = UINT(sd.Width), th = sd.Height;
    if (!Garante(chave, tw, th, DXGI_FORMAT_R32_UINT, true) || !Garante(rep, tw, th, Tipado(sd.Format), true) ||
        !Garante(anterior, UINT(fd.Width), fd.Height, fd.Format, false))
        return void(temAnterior = false);
    const bool comA = temAnterior && !reset && id == idAnterior + 1 && qAnterior.mov && qAnterior.prof;
    auto srv = [&](D3D12_CPU_DESCRIPTOR_HANDLE& h, ID3D12Resource* r) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = Tipado(r->GetDesc().Format);
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(r, &v, h);
        h.ptr += inc;
    };
    auto uav = [&](D3D12_CPU_DESCRIPTOR_HANDLE& h, ID3D12Resource* r) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = Tipado(r->GetDesc().Format);
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(r, nullptr, &u, h);
        h.ptr += inc;
    };
    ID3D12DescriptorHeap* hs[] = {heap.Get()};
    c->SetDescriptorHeaps(1, hs);
    c->SetComputeRootSignature(rs.Get());
    D3D12_RESOURCE_BARRIER ub[3] = {};
    for (auto& b : ub)
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub[0].UAV.pResource = chave.Get(), ub[1].UAV.pResource = rep.Get();
    for (uint32_t k = 0; k < m; ++k)
    {
        if (!saidas[k])
            continue;
        const UINT base = (proximo++ % 32) * 11;
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(base) * inc;
        srv(h, fonte), srv(h, q.mov), srv(h, q.prof);
        srv(h, comA ? anterior.Get() : fonte), srv(h, comA ? qAnterior.mov : q.mov), srv(h, comA ? qAnterior.prof : q.prof);
        srv(h, final_), srv(h, semHud ? semHud : final_);
        uav(h, chave.Get()), uav(h, rep.Get()), uav(h, saidas[k]);
        struct
        {
            float escBx, escBy, escAx, escAy;
            uint32_t rbw, rbh, raw, rah, tw, th;
            float t, limite;
            uint32_t flags, p0, p1, p2;
        } kc{q.movEscX * tw / q.renderW, q.movEscY * th / q.renderH,
             comA ? qAnterior.movEscX * tw / qAnterior.renderW : 0.f, comA ? qAnterior.movEscY * th / qAnterior.renderH : 0.f,
             q.renderW, q.renderH, comA ? qAnterior.renderW : q.renderW, comA ? qAnterior.renderH : q.renderH, tw, th,
             float(k + 1) / float(m + 1), limite,
             (profInvertida ? 1u : 0u) | (semHud ? 2u : 0u) | (comA ? 4u : 0u), 0, 0, 0};
        c->SetComputeRoot32BitConstants(0, 16, &kc, 0);
        D3D12_GPU_DESCRIPTOR_HANDLE g = heap->GetGPUDescriptorHandleForHeapStart();
        g.ptr += UINT64(base) * inc;
        c->SetComputeRootDescriptorTable(1, g);
        g.ptr += UINT64(8) * inc;
        c->SetComputeRootDescriptorTable(2, g);
        ub[2].UAV.pResource = saidas[k];
        c->ResourceBarrier(3, ub); // a geracao terminou de escrever a saida
        for (int i = 0; i < 6; ++i)
        {
            c->SetPipelineState(pso[i].Get());
            c->Dispatch((tw + 7) / 8, (th + 7) / 8, 1);
            c->ResourceBarrier(i == 5 ? 3 : 2, ub);
        }
    }
    // este real e o anterior do seguinte
    D3D12_RESOURCE_BARRIER b[2] = {};
    b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition = {fonte, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COPY_SOURCE};
    b[1].Transition = {anterior.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST};
    c->ResourceBarrier(2, b);
    c->CopyResource(anterior.Get(), fonte);
    for (auto& x : b)
    {
        const D3D12_RESOURCE_STATES s = x.Transition.StateBefore;
        x.Transition.StateBefore = x.Transition.StateAfter, x.Transition.StateAfter = s;
    }
    c->ResourceBarrier(2, b);
    qAnterior = q, idAnterior = id, temAnterior = true;
}

static const char kUi[] = R"(
Texture2D<float4> final_ : register(t0);
Texture2D<float4> semHud : register(t1);
RWTexture2D<float4> saida : register(u0);
[numthreads(8, 8, 1)] void CSUi(uint2 p : SV_DispatchThreadID)
{
    uint w, h;
    saida.GetDimensions(w, h);
    if (p.x >= w || p.y >= h) return;
    const float4 f = final_[p];
    if (any(f.rgb != semHud[p].rgb))
        saida[p] = f;
}
)";

bool GeracaoNeural::CriaUi()
{
    Microsoft::WRL::ComPtr<ID3DBlob> cs, err, ser;
    if (FAILED(D3DCompile(kUi, sizeof kUi - 1, "hud_4x", nullptr, nullptr, "CSUi", "cs_5_1", 0, 0, &cs, &err)))
        return false;
    D3D12_DESCRIPTOR_RANGE rs{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0}, ru{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0}; // a tabela do UAV ja comeca no slot dele
    D3D12_ROOT_PARAMETER rp[2]{};
    rp[0].ParameterType = rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable = {1, &rs};
    rp[1].DescriptorTable = {1, &ru};
    D3D12_ROOT_SIGNATURE_DESC rsd{2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &ser, &err)) ||
        FAILED(dev->CreateRootSignature(0, ser->GetBufferPointer(), ser->GetBufferSize(), IID_PPV_ARGS(&rsUi))))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{rsUi.Get(), {cs->GetBufferPointer(), cs->GetBufferSize()}};
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&psoUi))))
        return false;
    // 3 descritores por saida, um anel para os quadros em voo
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3 * 32, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heapUi))))
        return false;
    incUi = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

void GeracaoNeural::PoeUi(ID3D12GraphicsCommandList* c, ID3D12Resource* final_, ID3D12Resource* semHud, ID3D12Resource* saida)
{
    const UINT base = (proximoUi++ % 32) * 3;
    D3D12_CPU_DESCRIPTOR_HANDLE h = heapUi->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(base) * incUi;
    auto srv = [&](ID3D12Resource* r) {
        const D3D12_RESOURCE_DESC d = r->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = d.Format;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(r, &v, h);
        h.ptr += incUi;
    };
    srv(final_), srv(semHud);
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
    u.Format = saida->GetDesc().Format;
    u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(saida, nullptr, &u, h);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = saida; // a geracao da AMD terminou de escrever
    c->ResourceBarrier(1, &b);
    ID3D12DescriptorHeap* hs[] = {heapUi.Get()};
    c->SetDescriptorHeaps(1, hs);
    c->SetComputeRootSignature(rsUi.Get());
    c->SetPipelineState(psoUi.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE g = heapUi->GetGPUDescriptorHandleForHeapStart();
    g.ptr += UINT64(base) * incUi;
    c->SetComputeRootDescriptorTable(0, g);
    g.ptr += UINT64(2) * incUi;
    c->SetComputeRootDescriptorTable(1, g);
    c->Dispatch((telaW + 7) / 8, (telaH + 7) / 8, 1);
    c->ResourceBarrier(1, &b);
}

void GeracaoNeural::Destroi()
{
    const auto destroi = reinterpret_cast<PfnFfxDestroyContext>(api.destroi);
    for (void** c : {&a, &b})
        if (*c && destroi)
        {
            ffxContext x = *c;
            destroi(&x, nullptr);
            *c = nullptr;
        }
}

bool GeracaoNeural::Gera(ID3D12GraphicsCommandList* c, uint64_t id, const QuadroNeural& q, const CameraJogo& camAnterior,
                         ID3D12Resource* cor, uint32_t corEstado, ID3D12Resource* semHud, uint32_t semHudEstado,
                         ID3D12Resource* const* saidas, uint32_t saidaEstado, uint32_t n, bool reset,
                         uint32_t transferencia, float lumMin, float lumMax)
{
    if (!a || !c || !cor || !q.prof || !q.mov || (n != 1 && n != 3) || (n == 3 && !b))
        return false;
    const auto configura = reinterpret_cast<PfnFfxConfigure>(api.configura);
    const auto despacha = reinterpret_cast<PfnFfxDispatch>(api.despacha);
    // um quadro num contexto: configura, preparo V2 (o movimento x escMov) e a geracao de 1 quadro entre o anterior dele
    // e este
    auto um = [&](void* ctx, uint64_t fid, ID3D12Resource* fonte, uint32_t estadoFonte, ID3D12Resource* hudless,
                  ID3D12Resource* destino, float escMov, const CameraJogo& cam, float jx, float jy, float ms,
                  bool rs) -> ffxReturnCode_t {
        ffxContext cx = ctx;
        ConfiguraFg cf{};
        cf.header.type = kConfiguraFg;
        cf.frameGenerationEnabled = true;
        cf.flags = kSemNotificarSwapchain;
        cf.generationRect = {0, 0, int32_t(telaW), int32_t(telaH)};
        cf.frameID = fid;
        if (hudless)
            cf.HUDLessColor = ffxApiGetResourceDX12(hudless, semHudEstado);
        ffxReturnCode_t r = configura(&cx, &cf.header);
        PreparoV2 pp{};
        pp.header.type = kPreparoV2;
        pp.frameID = fid;
        pp.commandList = c;
        pp.renderSize = {q.renderW, q.renderH};
        pp.jitterOffset = {jx, jy};
        pp.motionVectorScale = {q.movEscX * escMov, q.movEscY * escMov};
        pp.frameTimeDelta = ms;
        pp.reset = rs;
        pp.cameraNear = q.perto;
        pp.cameraFar = q.longe;
        pp.cameraFovAngleVertical = q.fovY;
        pp.viewSpaceToMetersFactor = 1.f;
        pp.depth = ffxApiGetResourceDX12(q.prof, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        pp.motionVectors = ffxApiGetResourceDX12(q.mov, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        Copia(cam.pos, pp.cameraPosition), Copia(cam.cima, pp.cameraUp), Copia(cam.direita, pp.cameraRight);
        Copia(cam.frente, pp.cameraForward);
        r |= despacha(&cx, &pp.header);
        DespachaFg dg{};
        dg.header.type = kDespachaFg;
        dg.commandList = c;
        dg.presentColor = ffxApiGetResourceDX12(fonte, estadoFonte);
        dg.outputs[0] = ffxApiGetResourceDX12(destino, saidaEstado);
        dg.numGeneratedFrames = 1;
        dg.reset = rs;
        dg.backbufferTransferFunction = transferencia;
        dg.minMaxLuminance[0] = lumMin, dg.minMaxLuminance[1] = lumMax;
        dg.generationRect = {0, 0, int32_t(telaW), int32_t(telaH)};
        dg.frameID = fid;
        return r | despacha(&cx, &dg.header);
    };
    if (semHud && !psoUi)
        return false; // sem o passe do HUD, o 4x moveria o HUD: fica a analitica
    // A: o meio entre o real anterior e este (2x: o unico gerado, com a imagem sem HUD para a AMD por o HUD parado; 4x:
    // o do meio, da imagem sem HUD -- o HUD entra no fim)
    const bool resetA = reset || id != ultimoA + 1;
    ID3D12Resource* meio = n == 3 ? saidas[1] : saidas[0];
    ID3D12Resource* fonte = semHud ? semHud : cor;
    const uint32_t estadoFonte = semHud ? semHudEstado : corEstado;
    ffxReturnCode_t r = um(a, id, fonte, estadoFonte, nullptr, meio, 1.f, q.cam, q.jitterX, q.jitterY,
                           q.msQuadro, resetA);
    ultimoA = id;
    if (n == 3)
    {
        // B: o meio como o quadro seguinte ao real anterior (0,25), depois este real (0,75), com o movimento pela metade
        const uint64_t idM = 2 * id - 1, idR = 2 * id;
        const bool resetB = resetA || idM != ultimoB + 1;
        r |= um(b, idM, meio, saidaEstado, nullptr, saidas[0], 0.5f, Meio(camAnterior, q.cam), 0.f, 0.f, 0.5f * q.msQuadro,
                resetB);
        r |= um(b, idR, fonte, estadoFonte, nullptr, saidas[2], 0.5f, q.cam, q.jitterX, q.jitterY, 0.5f * q.msQuadro, false);
        ultimoB = idR;
    }
    if (semHud && r == FFX_API_RETURN_OK)
        for (uint32_t k = 0; k < n; ++k)
            PoeUi(c, cor, semHud, saidas[k]);
    if (r != FFX_API_RETURN_OK)
        snprintf(erro, sizeof erro, "geracao neural do quadro %llu: codigo %u", (unsigned long long)id, r);
    return r == FFX_API_RETURN_OK;
}

} // namespace dlss5ov
