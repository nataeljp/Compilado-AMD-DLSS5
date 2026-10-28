// Teste da geracao de quadros da AMD pela API do FidelityFX (loader 2.x + amd_fidelityfx_framegeneration_dx12.dll): o
// provedor neural (FSR Frame Generation 4.0.x, RDNA 4) ou o analitico (FSR 3.1.x), fora de jogo e sem swapchain, na cena
// 3D do jogo de teste (Cubo.h). Pergunta: o provedor gera mais de 1 quadro por real (numGeneratedFrames 2..3; a API tem
// outputs[4])? Grava o anterior, o real, os gerados e a verdade de cada instante (a cena a 2x2 por pixel, mesmo tonemap),
// no formato do despejo do modulo (nome_WxH_pitchP_fmtF.raw), e meta.json com o quadro.
// uso: TesteFgMl.exe <pasta das DLLs da AMD> <saida> [--n N] [--quadro Q] [--fps F] [--versao texto] [--prepare1]
//      [--pontilhado] [--fundo-liso] [--cobertura C] [--repete K]
// --repete K: K chamadas independentes da geracao por quadro real (1 quadro cada), o preparo de cada uma com o tempo
//   do quadro deslocado em k x 10 ns: gerado<k> sai da chamada k (a ideia de tirar mais de 1 quadro do provedor neural)
// --recursivo: 4x com dois contextos -- A gera o meio M (t = 0,5) entre os reais; B recebe real, M, real, M... como
//   quadros seguidos (movimento do real pela metade, camera no meio do caminho) e gera t = 0,25 e t = 0,75
#include "../jogo_teste/Cubo.h"
#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_framegeneration.h"
#include "ffx_api/dx12/ffx_api_dx12.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
using Microsoft::WRL::ComPtr;

// o que a API 4.0.1 tem a mais que os headers 1.1.4 (texto do FidelityFX SDK, MIT): versao da API na criacao e o preparo V2
#define TESTE_DESC_TYPE_FRAMEGENERATION_VERSION 0x0002000eu
#define TESTE_DESC_TYPE_FRAMEGENERATION_PREPARE_V2 0x0002000cu
struct DescVersaoFg
{
    ffxCreateContextDescHeader header;
    uint32_t version;
};
struct DescPreparoV2
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

static void Mensagem(uint32_t tipo, const wchar_t* m)
{
    fwprintf(stdout, L"  [AMD %s] %s\n", tipo == FFX_API_MESSAGE_TYPE_ERROR ? L"erro" : L"aviso", m);
}

static const char kTom[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer k : register(b0) { uint escala; };
[numthreads(8, 8, 1)] void CSTom(uint2 p : SV_DispatchThreadID)
{
    uint w, h;
    dst.GetDimensions(w, h);
    if (p.x >= w || p.y >= h) return;
    float3 c = 0;
    for (uint j = 0; j < escala; ++j)
        for (uint i = 0; i < escala; ++i)
            c += max(src[p * escala + uint2(i, j)].rgb, 0.0);
    c /= float(escala * escala);
    dst[p] = float4(pow(c / (1.0 + c), 1.0 / 2.2), 1.0);
}
)";

struct App
{
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> fila;
    ComPtr<ID3D12CommandAllocator> alo;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> cerca;
    UINT64 valor = 0;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    void Espera()
    {
        fila->Signal(cerca.Get(), ++valor);
        cerca->SetEventOnCompletion(valor, ev);
        WaitForSingleObject(ev, INFINITE);
    }
    ComPtr<ID3D12Resource> Tex(UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st,
                               const D3D12_CLEAR_VALUE* cv = nullptr)
    {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, f, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, fl};
        ComPtr<ID3D12Resource> r;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, cv, IID_PPV_ARGS(&r))))
            fprintf(stderr, "textura %ux%u fmt %d falhou\n", w, h, int(f));
        return r;
    }
    void Barreira(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
    {
        if (a == b)
            return;
        D3D12_RESOURCE_BARRIER x{};
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
        cl->ResourceBarrier(1, &x);
    }
    // copia a textura (no estado st) para um arquivo nome_WxH_pitchP_fmtF.raw
    void Grava(ID3D12Resource* r, D3D12_RESOURCE_STATES st, const std::wstring& pasta, const wchar_t* nome)
    {
        const D3D12_RESOURCE_DESC d = r->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
        ComPtr<ID3D12Resource> buf;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buf));
        alo->Reset();
        cl->Reset(alo.Get(), nullptr);
        Barreira(r, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION de{buf.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, so{r, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        de.PlacedFootprint = fp;
        so.SubresourceIndex = 0;
        cl->CopyTextureRegion(&de, 0, 0, 0, &so, nullptr);
        Barreira(r, D3D12_RESOURCE_STATE_COPY_SOURCE, st);
        cl->Close();
        ID3D12CommandList* l[] = {cl.Get()};
        fila->ExecuteCommandLists(1, l);
        Espera();
        void* p = nullptr;
        buf->Map(0, nullptr, &p);
        wchar_t arq[512];
        swprintf(arq, 512, L"%s\\%s_%llux%u_pitch%u_fmt%d.raw", pasta.c_str(), nome, (unsigned long long)d.Width, d.Height,
                 fp.Footprint.RowPitch, int(d.Format));
        if (FILE* f = _wfopen(arq, L"wb"))
        {
            fwrite(p, 1, size_t(fp.Footprint.RowPitch) * d.Height, f);
            fclose(f);
        }
        buf->Unmap(0, nullptr);
    }
};

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        printf("uso: TesteFgMl.exe <pasta das DLLs da AMD> <saida> [--n N] [--quadro Q] [--fps F] [--versao texto] "
               "[--prepare1] [--pontilhado] [--fundo-liso] [--cobertura C]\n");
        return 1;
    }
    const std::wstring pastaDll = argv[1], saida = argv[2];
    UINT n = 3, quadroAlvo = 210, repete = 0;
    double fps = 20.0;
    std::string versaoPedida;
    bool prepare1 = false, pontilhado = false, liso = false, recursivo = false, semGeracao = false, semCamera = false;
    float cobertura = 0.5f;
    for (int i = 3; i < argc; ++i)
    {
        if (!wcscmp(argv[i], L"--n") && i + 1 < argc)
            n = UINT(_wtoi(argv[++i]));
        else if (!wcscmp(argv[i], L"--quadro") && i + 1 < argc)
            quadroAlvo = UINT(_wtoi(argv[++i]));
        else if (!wcscmp(argv[i], L"--fps") && i + 1 < argc)
            fps = _wtof(argv[++i]);
        else if (!wcscmp(argv[i], L"--versao") && i + 1 < argc)
        {
            char b[128];
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, b, 128, nullptr, nullptr);
            versaoPedida = b;
        }
        else if (!wcscmp(argv[i], L"--repete") && i + 1 < argc)
            repete = UINT(_wtoi(argv[++i]));
        else if (!wcscmp(argv[i], L"--cobertura") && i + 1 < argc)
            cobertura = float(_wtof(argv[++i]));
        prepare1 |= !wcscmp(argv[i], L"--prepare1");
        pontilhado |= !wcscmp(argv[i], L"--pontilhado");
        liso |= !wcscmp(argv[i], L"--fundo-liso");
        recursivo |= !wcscmp(argv[i], L"--recursivo");
        semGeracao |= !wcscmp(argv[i], L"--sem-geracao");
        semCamera |= !wcscmp(argv[i], L"--sem-camera"); // a camera zerada (jogo sem geracao propria)
    }
    n = n < 1 ? 1 : n > 3 ? 3 : n;
    if (repete)
        n = repete > 3 ? 3 : repete;
    if (recursivo)
        n = 3, repete = 0;
    CreateDirectoryW(saida.c_str(), nullptr);
    bool quatroK = false;
    for (int i = 3; i < argc; ++i)
        quatroK |= !wcscmp(argv[i], L"--4k");
    const UINT W = quatroK ? 3840 : 1920, H = quatroK ? 2160 : 1080; // --4k: a tela do TLOU2

    App a;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&a.dev))))
        return printf("D3D12CreateDevice falhou\n"), 2;
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    a.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&a.fila));
    a.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a.alo));
    a.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.alo.Get(), nullptr, IID_PPV_ARGS(&a.cl));
    a.cl->Close();
    a.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&a.cerca));

    // ---- a API da AMD pelo loader do jogo ----
    const std::wstring loader = pastaDll + L"\\amd_fidelityfx_loader_dx12.dll";
    SetDllDirectoryW(pastaDll.c_str());
    HMODULE m = LoadLibraryW(loader.c_str());
    if (!m)
        return printf("nao carregou %ls (%lu)\n", loader.c_str(), GetLastError()), 3;
    auto Cria = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(m, "ffxCreateContext"));
    auto Destroi = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(m, "ffxDestroyContext"));
    auto Configura = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(m, "ffxConfigure"));
    auto Consulta = reinterpret_cast<PfnFfxQuery>(GetProcAddress(m, "ffxQuery"));
    auto Despacha = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(m, "ffxDispatch"));
    if (!Cria || !Destroi || !Configura || !Consulta || !Despacha)
        return printf("o loader nao exporta a API\n"), 3;
    ffxConfigureDescGlobalDebug1 dbg{};
    dbg.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
    dbg.fpMessage = Mensagem;
    dbg.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE;
    Configura(nullptr, &dbg.header);

    // versoes da geracao de quadros que o loader oferece nesta placa
    ffxQueryDescGetVersions qv{};
    qv.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    qv.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    qv.device = a.dev.Get();
    uint64_t nv = 0;
    qv.outputCount = &nv;
    Consulta(nullptr, &qv.header);
    std::vector<uint64_t> ids(nv);
    std::vector<const char*> nomes(nv);
    qv.versionIds = ids.data();
    qv.versionNames = nomes.data();
    Consulta(nullptr, &qv.header);
    uint64_t escolhido = 0;
    printf("versoes da geracao de quadros: %llu\n", (unsigned long long)nv);
    for (uint64_t i = 0; i < nv; ++i)
    {
        printf("  [%llu] id 0x%llx  %s\n", (unsigned long long)i, (unsigned long long)ids[i], nomes[i]);
        if (!escolhido && (versaoPedida.empty() || strstr(nomes[i], versaoPedida.c_str())))
            escolhido = ids[i];
    }
    if (!escolhido)
        return printf("nenhuma versao com \"%s\"\n", versaoPedida.c_str()), 4;

    ffxCreateContextDescFrameGeneration cd{};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    cd.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE |
               FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS | FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
    cd.displaySize = {W, H};
    cd.maxRenderSize = {W, H};
    cd.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    ffxCreateBackendDX12Desc bd{};
    bd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    bd.device = a.dev.Get();
    DescVersaoFg vd{};
    vd.header.type = TESTE_DESC_TYPE_FRAMEGENERATION_VERSION;
    vd.version = (4u << 22) | (0u << 12) | 1u;
    ffxOverrideVersion ov{};
    ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    ov.versionId = escolhido;
    cd.header.pNext = &bd.header;
    bd.header.pNext = &ov.header;
    if (!prepare1)
        ov.header.pNext = &vd.header;
    ffxContext fg = nullptr;
    const ffxReturnCode_t rc = Cria(&fg, &cd.header, nullptr);
    ffxQueryGetProviderVersion pv{};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (rc == FFX_API_RETURN_OK)
        Consulta(&fg, &pv.header);
    printf("contexto: codigo %u, provedor %s\n", rc, pv.versionName ? pv.versionName : "?");
    if (rc != FFX_API_RETURN_OK)
        return 5;
    ffxContext fgB = nullptr;
    if (recursivo && Cria(&fgB, &cd.header, nullptr) != FFX_API_RETURN_OK)
        return printf("segundo contexto falhou\n"), 5;

    // ---- a cena (Cubo.h), a verdade a 2x2, o tonemap ----
    CenaCubo cena;
    if (!cena.Cria(a.dev.Get()))
        return printf("cena falhou\n"), 6;
    cena.pontilhado = pontilhado, cena.fundoLiso = liso, cena.cobertura = cobertura;
    const D3D12_CLEAR_VALUE cvProf{DXGI_FORMAT_D32_FLOAT, {}};
    const auto RT = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, UAV = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto cor = a.Tex(W, H, CenaCubo::kCor, RT, SRV), mov = a.Tex(W, H, CenaCubo::kMov, RT, SRV);
    auto prof = a.Tex(W, H, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, SRV, &cvProf);
    auto corV = a.Tex(2 * W, 2 * H, CenaCubo::kCor, RT, SRV), movV = a.Tex(2 * W, 2 * H, CenaCubo::kMov, RT, SRV);
    auto profV = a.Tex(2 * W, 2 * H, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, SRV, &cvProf);
    cena.Alvos(0, cor.Get(), mov.Get(), prof.Get());
    cena.Alvos(1, corV.Get(), movV.Get(), profV.Get());
    const auto UAVST = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    auto tela = a.Tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, UAV, SRV), telaV = a.Tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, UAV, UAVST);
    ComPtr<ID3D12Resource> saidas[3];
    for (auto& s : saidas)
        s = a.Tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, UAV, UAVST);

    ComPtr<ID3DBlob> cs, err, ser;
    if (FAILED(D3DCompile(kTom, sizeof kTom - 1, "tom", nullptr, nullptr, "CSTom", "cs_5_1", 0, 0, &cs, &err)))
        return printf("tom: %s\n", (const char*)err->GetBufferPointer()), 7;
    D3D12_DESCRIPTOR_RANGE rs{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, ru{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER rp[3]{};
    rp[0].ParameterType = rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable = {1, &rs};
    rp[1].DescriptorTable = {1, &ru};
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[2].Constants = {0, 0, 1};
    D3D12_ROOT_SIGNATURE_DESC rsd{3, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3D12RootSignature> raiz;
    D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &ser, &err);
    a.dev->CreateRootSignature(0, ser->GetBufferPointer(), ser->GetBufferSize(), IID_PPV_ARGS(&raiz));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{raiz.Get(), {cs->GetBufferPointer(), cs->GetBufferSize()}};
    ComPtr<ID3D12PipelineState> pso;
    a.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ComPtr<ID3D12DescriptorHeap> heap;
    a.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
    const UINT inc = a.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = [&](int i) { auto h = heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(i) * inc; return h; };
    auto gpu = [&](int i) { auto h = heap->GetGPUDescriptorHandleForHeapStart(); h.ptr += UINT64(i) * inc; return h; };
    a.dev->CreateShaderResourceView(cor.Get(), nullptr, cpu(0));
    a.dev->CreateUnorderedAccessView(tela.Get(), nullptr, nullptr, cpu(1));
    a.dev->CreateShaderResourceView(corV.Get(), nullptr, cpu(2));
    a.dev->CreateUnorderedAccessView(telaV.Get(), nullptr, nullptr, cpu(3));
    auto Tom = [&](int srv, int uav, UINT escala) {
        ID3D12DescriptorHeap* hs[] = {heap.Get()};
        a.cl->SetDescriptorHeaps(1, hs);
        a.cl->SetComputeRootSignature(raiz.Get());
        a.cl->SetPipelineState(pso.Get());
        a.cl->SetComputeRootDescriptorTable(0, gpu(srv));
        a.cl->SetComputeRootDescriptorTable(1, gpu(uav));
        a.cl->SetComputeRoot32BitConstant(2, escala, 0);
        a.cl->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
    };

    // um quadro numa geracao: configura, preparo V2 (movimento x escMov) e a geracao de 1 quadro entre o anterior dela e este
    auto UmQuadro = [&](ffxContext* ctx, uint64_t id, ID3D12Resource* fonte, uint32_t estadoFonte, ID3D12Resource* destino,
                        float escMov, const float* pos, const float* cima, const float* dir, const float* frente, float dtMs,
                        bool reset) -> ffxReturnCode_t {
        ffxConfigureDescFrameGeneration cf{};
        cf.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cf.frameGenerationEnabled = true;
        cf.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
        cf.generationRect = {0, 0, int32_t(W), int32_t(H)};
        cf.frameID = id;
        ffxReturnCode_t r = Configura(ctx, &cf.header);
        DescPreparoV2 pp{};
        pp.header.type = TESTE_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        pp.frameID = id, pp.commandList = a.cl.Get(), pp.renderSize = {W, H};
        pp.motionVectorScale = {float(W) * escMov, float(H) * escMov};
        pp.frameTimeDelta = dtMs, pp.reset = reset, pp.cameraNear = 0.1f, pp.cameraFar = 1e5f;
        pp.cameraFovAngleVertical = 1.0472f, pp.viewSpaceToMetersFactor = 1.f;
        pp.depth = ffxApiGetResourceDX12(prof.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        pp.motionVectors = ffxApiGetResourceDX12(mov.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        if (!semCamera)
            memcpy(pp.cameraPosition, pos, 12), memcpy(pp.cameraUp, cima, 12), memcpy(pp.cameraRight, dir, 12),
                memcpy(pp.cameraForward, frente, 12);
        r |= Despacha(ctx, &pp.header);
        ffxDispatchDescFrameGeneration dg{};
        dg.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
        dg.commandList = a.cl.Get();
        dg.presentColor = ffxApiGetResourceDX12(fonte, estadoFonte);
        dg.outputs[0] = ffxApiGetResourceDX12(destino, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        dg.numGeneratedFrames = 1;
        dg.reset = reset;
        dg.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        dg.minMaxLuminance[0] = 0.f, dg.minMaxLuminance[1] = 1.f;
        dg.generationRect = {0, 0, int32_t(W), int32_t(H)};
        dg.frameID = id;
        return r | Despacha(ctx, &dg.header);
    };
    // a camera no meio do caminho entre dois quadros (o que um jogo daria: so a de cada quadro real)
    auto CameraMeio = [&](double t0, double t1, float* pos, float* cima, float* dir, float* frente) {
        float p0[3], c0[3], d0[3], f0[3], p1[3], c1[3], d1[3], f1[3];
        cena.CameraMundo(t0, p0, c0, d0, f0);
        cena.CameraMundo(t1, p1, c1, d1, f1);
        auto meio = [](const float* x, const float* y, float* o, bool normaliza) {
            for (int i = 0; i < 3; ++i)
                o[i] = 0.5f * (x[i] + y[i]);
            if (normaliza)
            {
                const float l = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
                for (int i = 0; i < 3; ++i)
                    o[i] /= l;
            }
        };
        meio(p0, p1, pos, false), meio(c0, c1, cima, true), meio(d0, d1, dir, true), meio(f0, f1, frente, true);
    };

    // ---- os quadros: a cena, o tonemap, o preparo da AMD e a geracao de n quadros entre o anterior e este ----
    const auto RTST = D3D12_RESOURCE_STATE_RENDER_TARGET;
    UINT erros = 0;
    // tempo de cada quadro (a GPU termina antes do seguinte: e o tempo da cena + tonemap + geracao), dos 20 primeiros em diante
    LARGE_INTEGER freq, ini{}, fim{};
    QueryPerformanceFrequency(&freq);
    for (UINT q = 0; q <= quadroAlvo; ++q)
    {
        if (q == 20)
            QueryPerformanceCounter(&ini);
        const double t = q / fps, tAnt = (q - 1.0) / fps;
        a.alo->Reset();
        a.cl->Reset(a.alo.Get(), nullptr);
        a.Barreira(cor.Get(), SRV, RTST);
        a.Barreira(mov.Get(), SRV, RTST);
        a.Barreira(prof.Get(), SRV, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cena.Desenha(a.cl.Get(), 0, W, H, t, tAnt);
        a.Barreira(cor.Get(), RTST, SRV);
        a.Barreira(mov.Get(), RTST, SRV);
        a.Barreira(prof.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, SRV);
        a.Barreira(tela.Get(), SRV, UAVST);
        Tom(0, 1, 1);
        a.Barreira(tela.Get(), UAVST, SRV);

        ffxReturnCode_t rcC = 0, rcP = 0, rcD = 0;
        float pos[3], cima[3], dir[3], frente[3];
        cena.CameraMundo(t, pos, cima, dir, frente);
        const uint32_t UAVFFX = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, LEFFX = FFX_API_RESOURCE_STATE_COMPUTE_READ;
        const float dt = float(1000.0 / fps);
        if (semGeracao)
        {
        }
        else if (recursivo)
        {
            // A: o meio M (saidas[0]) entre o real anterior e este
            rcD |= UmQuadro(&fg, q, tela.Get(), LEFFX, saidas[0].Get(), 1.f, pos, cima, dir, frente, dt, q == 0);
            if (q == 0)
                rcD |= UmQuadro(&fgB, 0, tela.Get(), LEFFX, saidas[2].Get(), 0.5f, pos, cima, dir, frente, dt / 2, true);
            else
            {
                // B: M como o quadro seguinte ao real anterior (t = 0,25 sai em saidas[1]), depois este real (t = 0,75)
                float pm[3], cm[3], dm[3], fm[3];
                CameraMeio(tAnt, t, pm, cm, dm, fm);
                rcD |= UmQuadro(&fgB, 2 * uint64_t(q) - 1, saidas[0].Get(), UAVFFX, saidas[1].Get(), 0.5f, pm, cm, dm, fm,
                                dt / 2, false);
                rcD |= UmQuadro(&fgB, 2 * uint64_t(q), tela.Get(), LEFFX, saidas[2].Get(), 0.5f, pos, cima, dir, frente,
                                dt / 2, false);
            }
        }
        else
        {
        ffxConfigureDescFrameGeneration cf{};
        cf.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cf.frameGenerationEnabled = true;
        cf.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
        cf.generationRect = {0, 0, int32_t(W), int32_t(H)};
        cf.frameID = q;
        rcC = Configura(&fg, &cf.header);
        for (UINT chamada = 0; chamada < (repete ? n : 1u); ++chamada)
        {
        const float dtMs = float(1000.0 / fps + chamada * 1e-5); // + k x 10 ns
        const FfxApiResource rProf = ffxApiGetResourceDX12(prof.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        const FfxApiResource rMov = ffxApiGetResourceDX12(mov.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        if (prepare1)
        {
            ffxDispatchDescFrameGenerationPrepare pp{};
            pp.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
            pp.frameID = q, pp.commandList = a.cl.Get(), pp.renderSize = {W, H};
            pp.motionVectorScale = {float(W), float(H)};
            pp.frameTimeDelta = dtMs, pp.cameraNear = 0.1f, pp.cameraFar = 1e5f;
            pp.cameraFovAngleVertical = 1.0472f, pp.viewSpaceToMetersFactor = 1.f, pp.depth = rProf, pp.motionVectors = rMov;
            ffxDispatchDescFrameGenerationPrepareCameraInfo ci{};
            ci.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
            memcpy(ci.cameraPosition, pos, 12), memcpy(ci.cameraUp, cima, 12), memcpy(ci.cameraRight, dir, 12),
                memcpy(ci.cameraForward, frente, 12);
            pp.header.pNext = &ci.header;
            rcP |= Despacha(&fg, &pp.header);
        }
        else
        {
            DescPreparoV2 pp{};
            pp.header.type = TESTE_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
            pp.frameID = q, pp.commandList = a.cl.Get(), pp.renderSize = {W, H};
            pp.motionVectorScale = {float(W), float(H)};
            pp.frameTimeDelta = dtMs, pp.reset = q == 0 && chamada == 0, pp.cameraNear = 0.1f, pp.cameraFar = 1e5f;
            pp.cameraFovAngleVertical = 1.0472f, pp.viewSpaceToMetersFactor = 1.f, pp.depth = rProf, pp.motionVectors = rMov;
            if (!semCamera)
                memcpy(pp.cameraPosition, pos, 12), memcpy(pp.cameraUp, cima, 12), memcpy(pp.cameraRight, dir, 12),
                    memcpy(pp.cameraForward, frente, 12);
            rcP |= Despacha(&fg, &pp.header);
        }

        ffxDispatchDescFrameGeneration dg{};
        dg.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
        dg.commandList = a.cl.Get();
        dg.presentColor = ffxApiGetResourceDX12(tela.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        if (repete)
            dg.outputs[0] = ffxApiGetResourceDX12(saidas[chamada].Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        else
            for (UINT k = 0; k < n; ++k)
                dg.outputs[k] = ffxApiGetResourceDX12(saidas[k].Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        dg.numGeneratedFrames = repete ? 1 : n;
        dg.reset = q == 0 && chamada == 0;
        dg.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        dg.minMaxLuminance[0] = 0.f, dg.minMaxLuminance[1] = 1.f;
        dg.generationRect = {0, 0, int32_t(W), int32_t(H)};
        dg.frameID = q;
        rcD |= Despacha(&fg, &dg.header);
        }
        }
        a.cl->Close();
        ID3D12CommandList* l[] = {a.cl.Get()};
        a.fila->ExecuteCommandLists(1, l);
        a.Espera();
        if (rcC || rcP || rcD)
            ++erros;
        if (q < 3 || q == quadroAlvo || ((rcC || rcP || rcD) && erros <= 5))
            printf("quadro %u: configura %u, preparo %u, geracao %u (n = %u)\n", q, rcC, rcP, rcD, n);
        if (q == quadroAlvo - 2)
            QueryPerformanceCounter(&fim);
        if (q == quadroAlvo - 1)
            a.Grava(tela.Get(), SRV, saida, L"anterior");
    }
    if (quadroAlvo > 25)
        printf("tempo por quadro real (cena + tonemap + geracao): %.2f ms\n",
               1000.0 * double(fim.QuadPart - ini.QuadPart) / double(freq.QuadPart) / double(quadroAlvo - 22));
    a.Grava(tela.Get(), SRV, saida, L"real");
    for (UINT k = 0; k < n; ++k)
    {
        wchar_t nome[32];
        swprintf(nome, 32, L"gerado%u", k + 1);
        static const int kRec[3] = {1, 0, 2}; // recursivo: 0,25 em saidas[1], 0,5 em saidas[0], 0,75 em saidas[2]
        a.Grava(saidas[recursivo ? kRec[k] : k].Get(), UAVST, saida, nome);
    }
    // a verdade: os instantes Q-1 + k/(n+1), k = 0..n+1, a 2x2 por pixel e o mesmo tonemap
    for (UINT k = 0; k <= n + 1; ++k)
    {
        const double t = (double(quadroAlvo) - 1.0 + double(k) / double(n + 1)) / fps;
        a.alo->Reset();
        a.cl->Reset(a.alo.Get(), nullptr);
        a.Barreira(corV.Get(), SRV, RTST);
        a.Barreira(movV.Get(), SRV, RTST);
        a.Barreira(profV.Get(), SRV, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cena.Desenha(a.cl.Get(), 1, 2 * W, 2 * H, t, t - 1.0 / fps);
        a.Barreira(corV.Get(), RTST, SRV);
        a.Barreira(movV.Get(), RTST, SRV);
        a.Barreira(profV.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, SRV);
        Tom(2, 3, 2);
        a.cl->Close();
        ID3D12CommandList* l[] = {a.cl.Get()};
        a.fila->ExecuteCommandLists(1, l);
        a.Espera();
        wchar_t nome[32];
        swprintf(nome, 32, L"verdade%u", k);
        a.Grava(telaV.Get(), UAVST, saida, nome);
    }
    if (FILE* f = _wfopen((saida + L"\\meta.json").c_str(), L"w"))
    {
        fprintf(f, "{\"quadro\": %u, \"n\": %u, \"fps\": %g, \"provedor\": \"%s\", \"erros\": %u}\n", quadroAlvo, n, fps,
                pv.versionName ? pv.versionName : "?", erros);
        fclose(f);
    }
    printf("gravado em %ls (%u quadros com erro)\n", saida.c_str(), erros);
    Destroi(&fg, nullptr);
    if (fgB)
        Destroi(&fgB, nullptr);
    return 0;
}
