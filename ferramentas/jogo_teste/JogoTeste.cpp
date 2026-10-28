// Jogo de teste da captura de buffer: faz o papel do jogo com o mesmo ClienteBuffer que vai dentro dele (sem ganchos).
// Le uma sequencia de gera_sintetico_buffer.py (cor, movimento e profundidade exatos), entrega cada quadro ao cliente
// como o upscaler do jogo entregaria e grava a cor que sai (com a edicao do overlay somada).
// uso: JogoTeste.exe <sequencia> <saida> [--voltas N] [--fps F] [--laco]   (grava so a ultima volta)
// --laco: a sequencia e um laco (gera_sintetico_buffer.py --laco): so o primeiro quadro avisa corte de cena
// --render R: como o Gears (UE5 + FSR): movimento e profundidade na resolucao de render (R x a da cor), o movimento em
//   unidade de tela (uv) com a escala = tamanho de render -- o que o FSR recebe de um jogo com escala de resolucao
// --geracao: apresenta cada quadro (a cor com a edicao do overlay) num swapchain da AMD com geracao de quadros (Geracao.h),
//   com a profundidade e o movimento exatos da sequencia; --multi N: 2..4 (2x a 4x, padrao 2); --sem-overlay: roda sem
//   esperar o overlay (so a geracao)
// --como-jogo: faz o que um jogo faz, sem o cliente embutido -- cria o swapchain pela DXGI e chama o FSR pela DLL
//   assinada da AMD (amd_fidelityfx_dx12.dll do FidelityFX SDK): e o teste da DLSS5Buffer.dll de verdade (aberto
//   pelo overlay com a geracao, o swapchain vira o da AMD e o gancho do FSR alimenta a geracao)
// --depura: a camada de validacao do D3D12 (a do Agility SDK 1.614 que vem no FidelityFX SDK, copiada para .\D3D12\ ao
//   lado do exe); as mensagens de erro e aviso saem no stdout. --depura-gpu: tambem a validacao na GPU (lenta)
// --adiantado (com --como-jogo): como um motor com a thread de render a frente -- o quadro seguinte (o FSR dele) e
//   GRAVADO antes do Present do atual, mas so ENVIADO a GPU depois dele
// --recria Q (com --como-jogo): no quadro Q solta o swapchain e cria outro na mesma janela, como um jogo que troca de
//   modo de tela ou de HDR (o swapchain antigo preso por alguem faz a criacao falhar)
// --fg-jogo (com --como-jogo --cubo): como um jogo com a geracao FSR propria -- um contexto de geracao na DLL da AMD e, a
//   cada quadro, o preparo dela com a camera da cena (posicao e eixos), de onde o modulo tira a camera da geracao neural
#include "../../src/buffer/ClienteBuffer.h"
#include "../../src/buffer/Geracao.h"
#include "Cubo.h"
#include "../../externo/ffx/ffx_api.h"
#include "../../externo/ffx/ffx_upscale.h"
#include "../../externo/ffx/dx12/ffx_api_dx12.h"
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")
#include <winver.h>
// a geracao FSR "do jogo" (--fg-jogo): o contexto e o preparo antigo com a camera ligada (ffx_framegeneration.h, MIT)
struct FgJogoCria
{
    ffxCreateContextDescHeader header;
    uint32_t flags;
    FfxApiDimensions2D displaySize, maxRenderSize;
    uint32_t backBufferFormat;
};
struct FgJogoPreparo
{
    ffxDispatchDescHeader header;
    uint64_t frameID;
    uint32_t flags;
    void* commandList;
    FfxApiDimensions2D renderSize;
    FfxApiFloatCoords2D jitterOffset, motionVectorScale;
    float frameTimeDelta;
    bool unusedReset;
    float cameraNear, cameraFar, cameraFovAngleVertical, viewSpaceToMetersFactor;
    FfxApiResource depth, motionVectors;
};
struct FgJogoCamera
{
    ffxConfigureDescHeader header;
    float cameraPosition[3], cameraUp[3], cameraRight[3], cameraForward[3];
};
#pragma comment(lib, "version.lib")
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

static void Ck(HRESULT h, const char* o)
{
    if (FAILED(h))
    {
        fprintf(stderr, "falhou: %s (0x%08lx)\n", o, (unsigned long)h);
        ExitProcess(1);
    }
}

// --raw: o mouse por entrada bruta, sem mensagens nem cursor (RIDEV_NOLEGACY), como o Gears; conta o que o jogo recebe
static long g_rawDx = 0, g_rawDy = 0, g_rawCliques = 0, g_rawEventos = 0;
static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_INPUT)
    {
        RAWINPUT ri{};
        UINT tam = sizeof ri;
        // so os eventos injetados (o teste usa mouse_event, sem dispositivo): o mouse fisico de quem esta usando o PC
        // nao entra na conta
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, &ri, &tam, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
            ri.header.dwType == RIM_TYPEMOUSE && ri.header.hDevice == nullptr)
        {
            ++g_rawEventos;
            g_rawDx += ri.data.mouse.lLastX;
            g_rawDy += ri.data.mouse.lLastY;
            if (ri.data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)
                ++g_rawCliques;
        }
    }
    if (m == WM_DESTROY)
        PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}

struct Tex
{
    ComPtr<ID3D12Resource> r;
    UINT bpp = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
};

// --hud (com --como-jogo): depois do FSR, como um jogo -- tonemap (compute) numa textura no formato da tela, um
// pos-efeito opcional (--pos grao: ruido na tela toda; --pos nitidez: so nas bordas, o resto igual bit a bit) e o HUD
// desenhado por cima com mistura (painel translucido com linhas, barra e mira); o resultado vai para o backbuffer.
// Testa a imagem sem HUD do modulo (jogo\SemHud.h): qual passe ele escolhe e se o HUD fica parado nos gerados
static const char kHud[] = R"(
cbuffer k : register(b0) { uint quadro, modo, w, h; };
Texture2D<float4> ent : register(t0);
RWTexture2D<float4> sai : register(u0);
[numthreads(8, 8, 1)] void CSTom(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= w || id.y >= h) return;
    if (id.y < 4 && id.x < 64) // o numero do quadro (16 bits, 4 px cada) no canto: o despejo diz que instante e
    {
        sai[id.xy] = ((quadro >> (id.x / 4)) & 1) ? float4(1, 1, 1, 1) : float4(0, 0, 0, 1);
        return;
    }
    const float3 c = max(ent[id.xy].rgb, 0);
    sai[id.xy] = float4(c / (1 + c), 1);
}
// a verdade (--verdade): a cena desenhada a 2x2 por pixel, a media e o mesmo tonemap
[numthreads(8, 8, 1)] void CSVerdade(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= w || id.y >= h) return;
    const uint2 p = id.xy * 2;
    const float3 c = (max(ent[p].rgb, 0) + max(ent[p + uint2(1, 0)].rgb, 0) + max(ent[p + uint2(0, 1)].rgb, 0) +
                      max(ent[p + uint2(1, 1)].rgb, 0)) * 0.25;
    sai[id.xy] = float4(c / (1 + c), 1);
}
float Ruido(uint2 p, uint q)
{
    uint n = p.x * 1973 + p.y * 9277 + q * 26699;
    n = (n << 13) ^ n;
    n = n * (n * n * 15731 + 789221) + 1376312589;
    return float(n & 0xffffff) / 16777216.0;
}
[numthreads(8, 8, 1)] void CSPos(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= w || id.y >= h) return;
    const float4 c = ent[id.xy];
    if (modo == 1)
    {
        sai[id.xy] = float4(saturate(c.rgb + (Ruido(id.xy, quadro) - 0.5) * 0.04), 1);
        return;
    }
    const int2 p = int2(id.xy);
    const float3 m = (ent[p + int2(1, 0)].rgb + ent[p - int2(1, 0)].rgb + ent[p + int2(0, 1)].rgb + ent[p - int2(0, 1)].rgb) * 0.25;
    const float3 d = c.rgb - m;
    sai[id.xy] = max(abs(d.r), max(abs(d.g), abs(d.b))) > 0.02 ? float4(saturate(c.rgb + d), 1) : c;
}
cbuffer r : register(b0) { float4 ret; float4 cor; };
float4 VSHud(uint v : SV_VertexID) : SV_Position
{
    const float2 t[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 0), float2(1, 1), float2(0, 1) };
    const float2 p = lerp(ret.xy, ret.zw, t[v]);
    return float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1);
}
float4 PSHud() : SV_Target { return cor; }
)";

// o Agility SDK em .\D3D12\ (so com a pasta presente; sem ela o D3D12 do Windows)
extern "C" __declspec(dllexport) extern const UINT D3D12SDKVersion = 614;
extern "C" __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "uso: JogoTeste <sequencia> <saida> [--voltas N] [--fps F]\n");
        return 1;
    }
    const std::wstring seq = argv[1], saida = argv[2];
    int voltas = 3, fps = 30;
    float render = 1.f;
    bool raw = false, laco = false, geracao = false, semOverlay = false, comoJogo = false;
    std::wstring modulo;
    int pos = 0; // --pos: 0 nenhum, 1 grao, 2 nitidez (com --hud)
    uint32_t multi = 2;
    UINT64 recria = 0, trava = 0;
    // --fps-depois Q F: a partir do quadro Q o jogo passa a F quadros/s (o N da geracao tem de acompanhar o ritmo novo)
    UINT64 fpsDepoisQ = 0;
    int fpsDepois = 0;
    // --invalida Q (com --como-jogo): no quadro Q envia a lista do jogo sem fechar -- o runtime remove o dispositivo por
    // chamada invalida (DXGI_ERROR_INVALID_CALL), como no TLOU2: testa o relato do modulo
    UINT64 invalida = 0;
    for (int i = 3; i + 1 < argc; ++i)
    {
        if (!wcscmp(argv[i], L"--trava")) // --trava Q: no quadro Q a thread do jogo para 8 s (o vigia do modulo)
            trava = UINT64(_wtoi64(argv[i + 1]));
        if (!wcscmp(argv[i], L"--recria"))
            recria = UINT64(_wtoi64(argv[i + 1]));
        if (!wcscmp(argv[i], L"--invalida"))
            invalida = UINT64(_wtoi64(argv[i + 1]));
        if (!wcscmp(argv[i], L"--multi"))
            multi = uint32_t(std::clamp(_wtoi(argv[i + 1]), 2, 4));
        if (!wcscmp(argv[i], L"--voltas"))
            voltas = _wtoi(argv[i + 1]);
        if (!wcscmp(argv[i], L"--fps-depois") && i + 2 < argc)
            fpsDepoisQ = UINT64(_wtoi64(argv[i + 1])), fpsDepois = _wtoi(argv[i + 2]);
        if (!wcscmp(argv[i], L"--fps"))
            fps = _wtoi(argv[i + 1]);
        if (!wcscmp(argv[i], L"--modulo"))
            modulo = argv[i + 1];
        if (!wcscmp(argv[i], L"--render"))
            render = std::clamp(float(_wtof(argv[i + 1])), 0.25f, 1.f);
        if (!wcscmp(argv[i], L"--pos"))
            pos = !wcscmp(argv[i + 1], L"grao") ? 1 : !wcscmp(argv[i + 1], L"nitidez") ? 2 : 0;
    }
    bool hud = false, cubo = false, fgJogo = false;
    UINT64 verdade = 0; // --verdade Q (com --cubo --hud): grava a verdade dos instantes Q-1 + k/N (k = 0..N) e sai
    float giro = 120.f; // --giro G (com --cubo): graus por segundo da camera
    bool pontilhado = false; // --pontilhado (com --cubo): os cubos que cruzam pontilhados, como cabelo
    bool fundoLiso = false;  // --fundo-liso (com --cubo): a sala sem o xadrez
    float cobertura = 0.5f;  // --cobertura C (com --pontilhado): fracao dos pixels que ficam
    for (int i = 3; i < argc; ++i)
    {
        hud |= !wcscmp(argv[i], L"--hud");
        cubo |= !wcscmp(argv[i], L"--cubo");
        fgJogo |= !wcscmp(argv[i], L"--fg-jogo");
        pontilhado |= !wcscmp(argv[i], L"--pontilhado");
        fundoLiso |= !wcscmp(argv[i], L"--fundo-liso");
        if (!wcscmp(argv[i], L"--cobertura") && i + 1 < argc)
            cobertura = float(_wtof(argv[i + 1]));
        if (!wcscmp(argv[i], L"--verdade") && i + 1 < argc)
            verdade = UINT64(_wtoi64(argv[i + 1]));
        if (!wcscmp(argv[i], L"--giro") && i + 1 < argc)
            giro = float(_wtof(argv[i + 1]));
    }
    for (int i = 3; i < argc; ++i)
    {
        raw |= !wcscmp(argv[i], L"--raw");
        laco |= !wcscmp(argv[i], L"--laco");
        geracao |= !wcscmp(argv[i], L"--geracao");
        semOverlay |= !wcscmp(argv[i], L"--sem-overlay");
        comoJogo |= !wcscmp(argv[i], L"--como-jogo");
    }
    semOverlay |= comoJogo; // o cliente e o da DLL injetada
    GetFileVersionInfoSizeW(L"kernel32.dll", nullptr); // importa a version.dll, como o TLOU2 (a DLL instalada com esse nome)
    bool depura = false, depuraGpu = false, adiantado = false;
    // --apresenta-em-thread (com --como-jogo): o Present numa thread propria, como o TLOU2 -- o quadro seguinte (com o
    // despacho do FSR e o preparo da geracao) e gravado enquanto o anterior esta no Present (que gera os quadros)
    bool apresentaEmThread = false;
    for (int i = 3; i < argc; ++i)
    {
        apresentaEmThread |= !wcscmp(argv[i], L"--apresenta-em-thread");
        adiantado |= !wcscmp(argv[i], L"--adiantado");
        depura |= !wcscmp(argv[i], L"--depura");
        depuraGpu |= !wcscmp(argv[i], L"--depura-gpu");
    }
    depura |= depuraGpu;
    if (depura)
    {
        ComPtr<ID3D12Debug1> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
        {
            dbg->EnableDebugLayer();
            dbg->SetEnableGPUBasedValidation(depuraGpu);
            printf("depuracao do D3D12 ligada%s\n", depuraGpu ? " (com validacao na GPU)" : "");
        }
        else
            printf("depuracao do D3D12 indisponivel (falta .\\D3D12\\d3d12SDKLayers.dll)\n");
    }
    CreateDirectoryW(saida.c_str(), nullptr);
    // tamanho e quadros: meta.jsonl (a cena 3D, --cubo: 1920x1080, 240 quadros por volta, a animacao segue entre voltas)
    char linha[4096] = "{\"rede_w\": 1920, \"rede_h\": 1080, \"quadros\": 240, \"perto\": 0.1, \"fovY\": 1.0472}";
    if (!cubo)
    {
        FILE* f = nullptr;
        _wfopen_s(&f, (seq + L"\\meta.jsonl").c_str(), L"r");
        if (!f)
            return 2;
        fgets(linha, sizeof linha, f);
        fclose(f);
    }
    auto num = [&](const char* k) {
        const char* p = strstr(linha, k);
        return p ? atof(p + strlen(k)) : 0.0;
    };
    const UINT W = UINT(num("\"rede_w\":")), H = UINT(num("\"rede_h\":")), N = UINT(num("\"quadros\":"));
    const float perto = float(num("\"perto\":"));
    const float fovY = float(num("\"fovY\":")); // so nas sequencias com a camera girando (gera_sintetico_buffer --giro)
    const UINT RW = render < 1.f ? UINT(W * render + 0.5f) : W, RH = render < 1.f ? UINT(H * render + 0.5f) : H;
    printf("sequencia %ux%u, %u quadros, %d voltas a %d fps; movimento e profundidade %ux%u%s\n", W, H, N, voltas, fps, RW,
           RH, render < 1.f ? " (uv, como o UE5)" : "");

    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"JogoTesteBuffer";
    RegisterClassW(&wc);
    HWND jan = CreateWindowW(wc.lpszClassName, L"jogo teste buffer", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40,
                             geracao ? 960 : 640, geracao ? 540 : 360, nullptr, nullptr, wc.hInstance, nullptr);
    if (raw)
    {
        RAWINPUTDEVICE d{0x01, 0x02, RIDEV_NOLEGACY, jan};
        if (!RegisterRawInputDevices(&d, 1, sizeof d))
            fprintf(stderr, "RegisterRawInputDevices falhou\n");
    }
    if (!modulo.empty() && !LoadLibraryW(modulo.c_str()))
        fprintf(stderr, "o modulo nao carregou\n");

    ComPtr<ID3D12Device> dev;
    Ck(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)), "D3D12CreateDevice");
    if (depura)
    {
        ComPtr<ID3D12InfoQueue1> iq;
        DWORD cookie = 0;
        if (SUCCEEDED(dev.As(&iq)))
            iq->RegisterMessageCallback(
                [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY sev, D3D12_MESSAGE_ID id, LPCSTR d, void*) {
                    static std::mutex m;
                    static int n = 0;
                    std::lock_guard<std::mutex> lk(m);
                    if (sev <= D3D12_MESSAGE_SEVERITY_WARNING && n++ < 200)
                    {
                        printf("D3D12 %s %d: %s\n", sev <= D3D12_MESSAGE_SEVERITY_ERROR ? "ERRO" : "aviso", int(id), d);
                        fflush(stdout);
                    }
                },
                D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
        printf("mensagens do D3D12: %s\n", cookie ? "ligadas" : "sem ID3D12InfoQueue1");
    }
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> fila;
    Ck(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&fila)), "fila");
    ComPtr<ID3D12CommandAllocator> al;
    ComPtr<ID3D12GraphicsCommandList> cl;
    Ck(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al)), "al");
    Ck(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(&cl)), "cl");
    cl->Close();
    ComPtr<ID3D12CommandAllocator> al2; // --adiantado: a lista da apresentacao do quadro anterior
    ComPtr<ID3D12GraphicsCommandList> cl2;
    Ck(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al2)), "al2");
    Ck(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al2.Get(), nullptr, IID_PPV_ARGS(&cl2)), "cl2");
    cl2->Close();
    ComPtr<ID3D12Fence> cerca;
    UINT64 valor = 0;
    Ck(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&cerca)), "cerca");
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    auto cria = [&](Tex& t, DXGI_FORMAT fmt, UINT bpp, bool uav, UINT tw, UINT th,
                    D3D12_RESOURCE_FLAGS extra = D3D12_RESOURCE_FLAG_NONE) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = tw;
        rd.Height = th;
        rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.Format = fmt;
        rd.SampleDesc.Count = 1;
        rd.Flags = (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE) | extra;
        Ck(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t.r)),
           "textura");
        dev->GetCopyableFootprints(&rd, 0, 1, 0, &t.fp, nullptr, nullptr, nullptr);
        t.bpp = bpp;
    };
    Tex cor, mov, prof, saidaFsr, saidaFsr2;
    cria(cor, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, true, W, H, cubo ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE);
    if (comoJogo)
    {
        cria(saidaFsr, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, true, W, H);
        cria(saidaFsr2, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, true, W, H); // --adiantado: a do quadro anterior
    }
    cria(mov, DXGI_FORMAT_R32G32_FLOAT, 8, false, RW, RH, cubo ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE);
    if (cubo) // profundidade de verdade: o alvo de profundidade, lido pelo FSR
        cria(prof, DXGI_FORMAT_R32_TYPELESS, 4, false, RW, RH, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    else
        cria(prof, DXGI_FORMAT_R32_FLOAT, 4, false, RW, RH);
    CenaCubo cena;
    if (cubo)
    {
        if (!cena.Cria(dev.Get()))
            return 10;
        cena.giro = giro;
        cena.pontilhado = pontilhado;
        cena.fundoLiso = fundoLiso;
        cena.cobertura = cobertura;
        cena.Alvos(0, cor.r.Get(), mov.r.Get(), prof.r.Get());
        printf("cena 3D: sala fechada, camera girando %.0f graus/s, cubos girando e cruzando (profundidade e movimento de "
               "verdade)\n", giro);
    }
    // um buffer de upload para os tres e um de leitura para a saida
    const UINT64 tc = UINT64(cor.fp.Footprint.RowPitch) * H, tm = UINT64(mov.fp.Footprint.RowPitch) * RH,
                 tp = UINT64(prof.fp.Footprint.RowPitch) * RH;
    auto buf = [&](D3D12_HEAP_TYPE tipo, UINT64 tam, D3D12_RESOURCE_STATES e, ComPtr<ID3D12Resource>& b) {
        D3D12_HEAP_PROPERTIES hp{tipo};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = tam;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Ck(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, e, nullptr, IID_PPV_ARGS(&b)), "buffer");
    };
    ComPtr<ID3D12Resource> up, le;
    buf(D3D12_HEAP_TYPE_UPLOAD, tc + tm + tp, D3D12_RESOURCE_STATE_GENERIC_READ, up);
    buf(D3D12_HEAP_TYPE_READBACK, tc, D3D12_RESOURCE_STATE_COPY_DEST, le);

    // geracao de quadros: o swapchain da AMD no lugar do do jogo, criado como o jogo criaria o dele
    dlss5ov::Geracao ger;
    ComPtr<IDXGISwapChain3> sc;
    if (geracao)
    {
        ComPtr<IDXGIFactory2> fab;
        Ck(CreateDXGIFactory2(0, IID_PPV_ARGS(&fab)), "CreateDXGIFactory2");
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = W;
        sd.Height = H;
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // a cor linear (scRGB) copiada direto
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 3;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(ger.CriaSwapchain(fab.Get(), fila.Get(), jan, &sd, nullptr, &sc1)))
        {
            fprintf(stderr, "geracao: %s\n", ger.UltimoErro());
            return 5;
        }
        sc1.As(&sc);
        sc->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
        printf("geracao de quadros: swapchain da AMD criado (%ux%u scRGB)\n", W, H);
    }

    // como um jogo: o swapchain pela DXGI (a DLSS5Buffer.dll aberta com a geracao troca pelo da AMD) e o FSR pela DLL
    // da AMD (a DLSS5Buffer.dll engancha ffxCreateContext/ffxDispatch dela)
    DXGI_SWAP_CHAIN_DESC1 sdJogo{};
    // --hud: o pos e o HUD, criados depois do swapchain (como num jogo: o modulo segue os descritores desde a criacao)
    Tex hudA, hudB;
    ComPtr<ID3D12DescriptorHeap> hudDesc, hudRtv;
    ComPtr<ID3D12RootSignature> hudRaizC, hudRaizG;
    ComPtr<ID3D12PipelineState> hudTom, hudPos, hudDesenho, hudVerdade;
    UINT hudInc = 0, hudIncRtv = 0;
    PfnFfxCreateContext fsrCria = nullptr;
    PfnFfxDispatch fsrDespacha = nullptr;
    ffxContext fsr = nullptr, fgDoJogo = nullptr;
    if (comoJogo)
    {
        wchar_t eu[MAX_PATH];
        GetModuleFileNameW(nullptr, eu, MAX_PATH);
        std::wstring raiz(eu);
        raiz = raiz.substr(0, raiz.find_last_of(L"\\/")); // a pasta do exe (obj)
        // ao lado do exe; senao a do SDK no repositorio (obj\..\externo)
        HMODULE ffx = LoadLibraryW((raiz + L"\\amd_fidelityfx_dx12.dll").c_str());
        raiz = raiz.substr(0, raiz.find_last_of(L"\\/"));
        if (!ffx)
            ffx = LoadLibraryW((raiz + L"\\externo\\fidelityfx_sdk\\PrebuiltSignedDLL\\amd_fidelityfx_dx12.dll").c_str());
        if (!ffx)
        {
            fprintf(stderr, "como jogo: amd_fidelityfx_dx12.dll nao carregou\n");
            return 6;
        }
        fsrCria = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(ffx, "ffxCreateContext"));
        fsrDespacha = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(ffx, "ffxDispatch"));
        ffxCreateBackendDX12Desc be{};
        be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        be.device = dev.Get();
        ffxCreateContextDescUpscale cd{};
        cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        cd.header.pNext = &be.header;
        cd.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE | FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
        cd.maxRenderSize = {RW, RH};
        cd.maxUpscaleSize = {W, H};
        if (!fsrCria || !fsrDespacha || fsrCria(&fsr, &cd.header, nullptr) != FFX_API_RETURN_OK)
        {
            fprintf(stderr, "como jogo: o contexto do FSR nao foi criado\n");
            return 7;
        }
        ComPtr<IDXGIFactory2> fab;
        Ck(CreateDXGIFactory2(0, IID_PPV_ARGS(&fab)), "CreateDXGIFactory2");
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = W;
        sd.Height = H;
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 3;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sdJogo = sd;
        ComPtr<IDXGISwapChain1> sc1;
        Ck(fab->CreateSwapChainForHwnd(fila.Get(), jan, &sd, nullptr, nullptr, &sc1), "CreateSwapChainForHwnd");
        sc1.As(&sc);
        sc->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
        printf("como jogo: FSR pela amd_fidelityfx_dx12.dll e swapchain pela DXGI (%ux%u)\n", W, H);
        if (fgJogo && cubo)
        {
            // a geracao do "jogo": o contexto existe para o preparo com a camera passar pela DLL (o modulo, com a geracao
            // dele pedida, desliga a do jogo)
            FgJogoCria fc{};
            fc.header.type = 0x00020001u; // FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION
            fc.header.pNext = &be.header;
            fc.flags = (1u << 3) | (1u << 4) | (1u << 5); // profundidade invertida e infinita, HDR
            fc.displaySize = {W, H};
            fc.maxRenderSize = {RW, RH};
            fc.backBufferFormat = ffxApiGetSurfaceFormatDX12(DXGI_FORMAT_R16G16B16A16_FLOAT);
            const ffxReturnCode_t rf = fsrCria(&fgDoJogo, &fc.header, nullptr);
            printf("como jogo: geracao FSR do jogo (para o preparo com a camera): %s\n", rf == FFX_API_RETURN_OK ? "ok" : "falhou");
        }
        if (hud)
        {
            auto compila = [&](const char* e, const char* alvo) {
                ComPtr<ID3DBlob> b, err;
                if (FAILED(D3DCompile(kHud, sizeof kHud - 1, "hud", nullptr, nullptr, e, alvo, 0, 0, &b, &err)))
                {
                    fprintf(stderr, "hud: %s\n", err ? (const char*)err->GetBufferPointer() : e);
                    ExitProcess(8);
                }
                return b;
            };
            for (Tex* t : {&hudA, &hudB})
            {
                D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
                D3D12_RESOURCE_DESC rd{};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                rd.Width = W;
                rd.Height = H;
                rd.DepthOrArraySize = rd.MipLevels = 1;
                rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                rd.SampleDesc.Count = 1;
                rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                Ck(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(&t->r)),
                   "hud: textura");
            }
            hudA.r->SetName(L"hud: cena (tonemap)");
            hudB.r->SetName(L"hud: pos-efeito");
            // compute: [SRV t0, UAV u0] numa tabela so (a faixa de UAV da root signature diz onde ela esta)
            D3D12_DESCRIPTOR_RANGE fx[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND}};
            D3D12_ROOT_PARAMETER pc[2]{};
            pc[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            pc[0].Constants.Num32BitValues = 4;
            pc[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            pc[1].DescriptorTable = {2, fx};
            D3D12_ROOT_PARAMETER pg{};
            pg.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            pg.Constants.Num32BitValues = 8;
            auto raiz = [&](const D3D12_ROOT_SIGNATURE_DESC& d, ComPtr<ID3D12RootSignature>& rs) {
                ComPtr<ID3DBlob> s, e;
                Ck(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &s, &e), "hud: raiz");
                Ck(dev->CreateRootSignature(0, s->GetBufferPointer(), s->GetBufferSize(), IID_PPV_ARGS(&rs)), "hud: raiz");
            };
            raiz({2, pc, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE}, hudRaizC);
            raiz({1, &pg, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE}, hudRaizG);
            auto computa = [&](const char* e, ComPtr<ID3D12PipelineState>& pso) {
                auto b = compila(e, "cs_5_1");
                D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
                d.pRootSignature = hudRaizC.Get();
                d.CS = {b->GetBufferPointer(), b->GetBufferSize()};
                Ck(dev->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso)), "hud: pso");
            };
            computa("CSTom", hudTom);
            computa("CSPos", hudPos);
            computa("CSVerdade", hudVerdade);
            auto vs = compila("VSHud", "vs_5_1"), ps = compila("PSHud", "ps_5_1");
            D3D12_GRAPHICS_PIPELINE_STATE_DESC g{};
            g.pRootSignature = hudRaizG.Get();
            g.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
            g.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
            auto& m = g.BlendState.RenderTarget[0]; // o HUD misturado por cima da cena (alfa)
            m.BlendEnable = TRUE;
            m.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            m.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            m.BlendOp = m.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            m.SrcBlendAlpha = D3D12_BLEND_ONE;
            m.DestBlendAlpha = D3D12_BLEND_ZERO;
            m.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            g.SampleMask = UINT_MAX;
            g.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            g.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            g.RasterizerState.DepthClipEnable = TRUE;
            g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            g.NumRenderTargets = 1;
            g.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
            g.SampleDesc.Count = 1;
            Ck(dev->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&hudDesenho)), "hud: pso do desenho");
            // descritores: [0] SRV da saida do FSR, [1] UAV de A, [2] SRV de A, [3] UAV de B, [4] SRV da verdade (2x2 por
            // pixel, --verdade), [5] UAV de A; alvos: A, B
            D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 6, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
            Ck(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&hudDesc)), "hud: heap");
            D3D12_DESCRIPTOR_HEAP_DESC hr{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            Ck(dev->CreateDescriptorHeap(&hr, IID_PPV_ARGS(&hudRtv)), "hud: heap de alvos");
            hudInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            hudIncRtv = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            auto cpu = [&](int i) {
                D3D12_CPU_DESCRIPTOR_HANDLE h = hudDesc->GetCPUDescriptorHandleForHeapStart();
                h.ptr += SIZE_T(i) * hudInc;
                return h;
            };
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Texture2D.MipLevels = 1;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
            uv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            dev->CreateShaderResourceView(saidaFsr.r.Get(), &sv, cpu(0));
            dev->CreateUnorderedAccessView(hudA.r.Get(), nullptr, &uv, cpu(1));
            dev->CreateShaderResourceView(hudA.r.Get(), &sv, cpu(2));
            dev->CreateUnorderedAccessView(hudB.r.Get(), nullptr, &uv, cpu(3));
            dev->CreateUnorderedAccessView(hudA.r.Get(), nullptr, &uv, cpu(5));
            D3D12_CPU_DESCRIPTOR_HANDLE rh = hudRtv->GetCPUDescriptorHandleForHeapStart();
            dev->CreateRenderTargetView(hudA.r.Get(), nullptr, rh);
            rh.ptr += hudIncRtv;
            dev->CreateRenderTargetView(hudB.r.Get(), nullptr, rh);
            printf("hud: tonemap%s e o HUD por cima\n", pos == 1 ? ", grao" : pos == 2 ? ", nitidez" : "");
        }
    }

    dlss5ov::ClienteBuffer cli;
    std::mutex mx; // o modulo no jogo protege o cliente com um mutex (o Execute vem de outras threads); aqui so a forma
    dlss5ov::ClienteBuffer::PreCompila();
    while (!comoJogo && dlss5ov::ClienteBuffer::Compilando())
        Sleep(20);
    if (!comoJogo && !cli.Inicia(dev.Get(), "JogoTeste.exe")) // como jogo: o cliente e o da DLL injetada
    {
        fprintf(stderr, "cliente: %s\n", cli.UltimoErro());
        return 3;
    }
    auto bomba = [] {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    };
    printf("esperando o overlay (Ctrl+Shift+D na janela 'jogo teste buffer', Modo de Rede = buffer)...\n");
    fflush(stdout);
    for (int i = 0; i < 6000 && !semOverlay && !cli.OverlayVivo(); ++i)
    {
        bomba();
        Sleep(10);
    }
    if (!semOverlay && !cli.OverlayVivo())
    {
        fprintf(stderr, "o overlay nao apareceu em 60 s\n");
        return 4;
    }
    std::vector<char> lin(size_t(W) * 8);
    auto le_arquivo = [&](const wchar_t* sufixo, UINT i, unsigned char* dst, UINT bpp, UINT pitch) {
        wchar_t n[64];
        swprintf(n, 64, L"\\%04u_%s.raw", i, sufixo);
        FILE* a = nullptr;
        _wfopen_s(&a, (seq + n).c_str(), L"rb");
        if (!a)
            return false;
        for (UINT y = 0; y < H; ++y)
            fread(dst + size_t(y) * pitch, 1, size_t(W) * bpp, a);
        fclose(a);
        return true;
    };
    auto barreira = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER x{};
        x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
        cl->ResourceBarrier(1, &x);
    };
    // --hud: o HUD, parado na tela, desenhado com mistura no alvo (0 = A, 1 = B, ja em RENDER_TARGET): painel
    // translucido com 6 linhas de texto, barra de vida e mira
    auto desenhaHud = [&](int alvo) {
        D3D12_CPU_DESCRIPTOR_HANDLE rt = hudRtv->GetCPUDescriptorHandleForHeapStart();
        rt.ptr += SIZE_T(alvo) * hudIncRtv;
        cl->OMSetRenderTargets(1, &rt, FALSE, nullptr);
        const D3D12_VIEWPORT vp{0, 0, float(W), float(H), 0, 1};
        const D3D12_RECT sr{0, 0, LONG(W), LONG(H)};
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sr);
        cl->SetGraphicsRootSignature(hudRaizG.Get());
        cl->SetPipelineState(hudDesenho.Get());
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        float ret[10][8] = {{0.02f, 0.03f, 0.32f, 0.40f, 0.10f, 0.15f, 0.25f, 0.60f},
                            {0.30f, 0.90f, 0.70f, 0.95f, 0.80f, 0.20f, 0.20f, 0.90f},
                            {0.497f, 0.47f, 0.503f, 0.53f, 1, 1, 1, 1},
                            {0.48f, 0.497f, 0.52f, 0.503f, 1, 1, 1, 1}};
        for (int l = 0; l < 6; ++l)
        {
            const float y = 0.07f + 0.05f * float(l);
            const float linha[8] = {0.04f, y, 0.20f + 0.015f * float(l), y + 0.018f, 0.9f, 0.9f, 0.9f, 1.f};
            std::copy(linha, linha + 8, ret[4 + l]);
        }
        for (const auto& x : ret)
        {
            cl->SetGraphicsRoot32BitConstants(0, 8, x, 0);
            cl->DrawInstanced(6, 1, 0, 0);
        }
    };
    if (verdade && cubo && hud && comoJogo)
    {
        // --verdade Q: os instantes Q-1 + k/N (k = 0..N; N = --multi) desenhados a 2x2 por pixel, com o mesmo tonemap e
        // o mesmo HUD, gravados como o despejo da geracao (verdade<k>_WxH_pitchP_fmt10.raw na pasta de saida)
        Tex corV, movV, profV;
        cria(corV, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, false, 2 * W, 2 * H, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        cria(movV, DXGI_FORMAT_R32G32_FLOAT, 8, false, 2 * W, 2 * H, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        cria(profV, DXGI_FORMAT_R32_TYPELESS, 4, false, 2 * W, 2 * H, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        cena.Alvos(1, corV.r.Get(), movV.r.Get(), profV.r.Get());
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE hv = hudDesc->GetCPUDescriptorHandleForHeapStart();
        hv.ptr += SIZE_T(4) * hudInc;
        dev->CreateShaderResourceView(corV.r.Get(), &sv, hv);
        D3D12_GPU_DESCRIPTOR_HANDLE gv = hudDesc->GetGPUDescriptorHandleForHeapStart();
        gv.ptr += UINT64(4) * hudInc;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        const D3D12_RESOURCE_DESC da = hudA.r->GetDesc();
        dev->GetCopyableFootprints(&da, 0, 1, 0, &fp, nullptr, nullptr, &total);
        ComPtr<ID3D12Resource> leV;
        buf(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST, leV);
        for (uint32_t k = 0; k <= multi; ++k)
        {
            const double t = (double(verdade) - 1.0 + double(k) / multi) / fps;
            Ck(al->Reset(), "reset");
            Ck(cl->Reset(al.Get(), nullptr), "list");
            barreira(corV.r.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            barreira(movV.r.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            barreira(profV.r.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            cena.Desenha(cl.Get(), 1, 2 * W, 2 * H, t, t - 1.0 / fps);
            barreira(corV.r.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            barreira(movV.r.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
            barreira(profV.r.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COMMON);
            barreira(hudA.r.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ID3D12DescriptorHeap* hs[] = {hudDesc.Get()};
            cl->SetDescriptorHeaps(1, hs);
            cl->SetComputeRootSignature(hudRaizC.Get());
            const uint32_t kc[4] = {0, 0, W, H};
            cl->SetComputeRoot32BitConstants(0, 4, kc, 0);
            cl->SetPipelineState(hudVerdade.Get());
            cl->SetComputeRootDescriptorTable(1, gv);
            cl->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
            barreira(hudA.r.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RENDER_TARGET);
            desenhaHud(0);
            barreira(hudA.r.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{leV.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            dst.PlacedFootprint = fp;
            D3D12_TEXTURE_COPY_LOCATION src{hudA.r.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barreira(hudA.r.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            barreira(corV.r.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Ck(cl->Close(), "close");
            ID3D12CommandList* lv[] = {cl.Get()};
            fila->ExecuteCommandLists(1, lv);
            Ck(fila->Signal(cerca.Get(), ++valor), "signal");
            cerca->SetEventOnCompletion(valor, ev);
            WaitForSingleObject(ev, INFINITE);
            void* mp = nullptr;
            Ck(leV->Map(0, nullptr, &mp), "map");
            wchar_t arq[128];
            swprintf(arq, 128, L"\\verdade%u_%ux%u_pitch%u_fmt10.raw", k, W, H, fp.Footprint.RowPitch);
            FILE* fv = nullptr;
            _wfopen_s(&fv, (saida + arq).c_str(), L"wb");
            if (fv)
            {
                fwrite(mp, 1, size_t(fp.Footprint.RowPitch) * H, fv);
                fclose(fv);
            }
            leV->Unmap(0, nullptr);
        }
        printf("verdade: quadro %llu, %u instantes gravados\n", (unsigned long long)verdade, multi + 1);
        return 0;
    }
    uint64_t aplicados = 0, gravados = 0;
    FILE* mapa = nullptr; // quadro do jogo -> quadro da sequencia (alinha a gravacao do overlay)
    _wfopen_s(&mapa, (saida + L"\\mapa.jsonl").c_str(), L"w");
    auto passo = std::chrono::microseconds(1000000 / std::max(fps, 1));
    // --apresenta-em-thread: a thread de apresentacao recebe o pedido e faz o Present; a de render so espera por ele antes
    // de tocar no backbuffer seguinte
    HANDLE pedeApresentar = CreateEventW(nullptr, FALSE, FALSE, nullptr), apresentou = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<bool> sairApresentador{false};
    bool apresentando = false;
    std::thread apresentador;
    if (apresentaEmThread && sc)
        apresentador = std::thread([&] {
            // um atraso aleatorio de 0 a 3 ms antes do Present: as duas threads fora de fase, como no TLOU2. Mesmo assim o
            // encontro e raro (2 em 2400 quadros, contados pelo modulo): o Present da AMD espera o ritmo das apresentacoes
            // antes de chamar a geracao, e a thread de render espera esse Present antes do backbuffer seguinte
            std::minstd_rand sorteio(12345);
            while (WaitForSingleObject(pedeApresentar, INFINITE) == WAIT_OBJECT_0 && !sairApresentador)
            {
                const auto ate = std::chrono::steady_clock::now() + std::chrono::microseconds(sorteio() % 3000);
                while (std::chrono::steady_clock::now() < ate)
                    YieldProcessor();
                sc->Present(0, 0);
                SetEvent(apresentou);
            }
        });
    auto esperaApresentacao = [&] {
        if (apresentando)
            WaitForSingleObject(apresentou, INFINITE), apresentando = false;
    };
    auto proximo = std::chrono::steady_clock::now();
    const auto inicio = proximo;
    for (int v = 0; v < voltas; ++v)
        for (UINT i = 0; i < N; ++i)
        {
            bomba();
            if (cubo)
            {
                // a cena 3D no instante deste quadro (o tempo e o numero do quadro), com o movimento desde o anterior
                const UINT64 qc = UINT64(v) * N + i;
                Ck(al->Reset(), "reset");
                Ck(cl->Reset(al.Get(), nullptr), "list");
                barreira(cor.r.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
                barreira(mov.r.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
                barreira(prof.r.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                cena.Desenha(cl.Get(), 0, RW, RH, double(qc) / fps, (double(qc) - 1.0) / fps);
                barreira(cor.r.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                barreira(mov.r.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                barreira(prof.r.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            else
            {
            unsigned char* m = nullptr;
            up->Map(0, nullptr, (void**)&m);
            le_arquivo(L"orig", i, m, 8, cor.fp.Footprint.RowPitch);
            if (render < 1.f)
            {
                // resolucao de render: o ponto mais proximo; o movimento vira uv (o FSR multiplica pela escala)
                static std::vector<float> mvc, pfc;
                mvc.resize(size_t(W) * H * 2);
                pfc.resize(size_t(W) * H);
                le_arquivo(L"mv", i, reinterpret_cast<unsigned char*>(mvc.data()), 8, W * 8);
                le_arquivo(L"prof", i, reinterpret_cast<unsigned char*>(pfc.data()), 4, W * 4);
                for (UINT y = 0; y < RH; ++y)
                {
                    float* dm = reinterpret_cast<float*>(m + tc + size_t(y) * mov.fp.Footprint.RowPitch);
                    float* dp = reinterpret_cast<float*>(m + tc + tm + size_t(y) * prof.fp.Footprint.RowPitch);
                    const UINT sy = std::min(UINT((y + 0.5f) * H / RH), H - 1);
                    for (UINT x = 0; x < RW; ++x)
                    {
                        const UINT sx = std::min(UINT((x + 0.5f) * W / RW), W - 1);
                        const size_t k = size_t(sy) * W + sx;
                        dm[2 * x] = mvc[2 * k] / float(W);
                        dm[2 * x + 1] = mvc[2 * k + 1] / float(H);
                        dp[x] = pfc[k];
                    }
                }
            }
            else
            {
                le_arquivo(L"mv", i, m + tc, 8, mov.fp.Footprint.RowPitch);
                le_arquivo(L"prof", i, m + tc + tm, 4, prof.fp.Footprint.RowPitch);
            }
            up->Unmap(0, nullptr);
            Ck(al->Reset(), "reset");
            Ck(cl->Reset(al.Get(), nullptr), "list");
            UINT64 base = 0;
            for (Tex* t : {&cor, &mov, &prof})
            {
                barreira(t->r.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION d{t->r.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                D3D12_TEXTURE_COPY_LOCATION s{up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                s.PlacedFootprint = t->fp;
                s.PlacedFootprint.Offset = base;
                cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
                base += UINT64(t->fp.Footprint.RowPitch) * t->fp.Footprint.Height;
            }
            // como o upscaler deixaria: a saida em UAV, as entradas para leitura
            barreira(cor.r.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            barreira(mov.r.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            barreira(prof.r.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            if (comoJogo)
            {
                // o FSR do "jogo": entrada na resolucao de render, saida na da tela (a DLSS5Buffer.dll age aqui)
                barreira(cor.r.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                // --adiantado: este quadro e gravado agora e so enviado depois do Present do anterior (saidas alternadas)
                const UINT64 q = UINT64(v) * N + i;
                Tex& saidaAgora = adiantado && (q & 1) ? saidaFsr2 : saidaFsr;
                Tex& saidaTela = saidaAgora;
                barreira(saidaAgora.r.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                ffxDispatchDescUpscale u{};
                u.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
                u.commandList = cl.Get();
                u.color = ffxApiGetResourceDX12(cor.r.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
                u.depth = ffxApiGetResourceDX12(prof.r.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
                u.motionVectors = ffxApiGetResourceDX12(mov.r.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
                u.output = ffxApiGetResourceDX12(saidaAgora.r.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
                // o movimento em uv (a cena 3D e --render < 1) vira pixels pela escala; o da sequencia ja vem em pixels
                u.motionVectorScale = {cubo || render < 1.f ? float(RW) : 1.f, cubo || render < 1.f ? float(RH) : 1.f};
                u.renderSize = {RW, RH};
                u.upscaleSize = {W, H};
                u.frameTimeDelta = 1000.f / float(std::max(fps, 1));
                u.preExposure = 1.f;
                u.reset = cubo ? q == 0 : i == 0 && (!laco || v == 0); // a cena 3D segue entre as voltas
                u.cameraNear = perto > 0.f ? perto : 0.1f;
                u.cameraFar = 1e7f;
                u.cameraFovAngleVertical = fovY > 0.f ? fovY : 1.0472f;
                u.viewSpaceToMetersFactor = 1.f;
                if (fsrDespacha(&fsr, &u.header) != FFX_API_RETURN_OK && v == 0 && i == 0)
                    fprintf(stderr, "como jogo: ffxDispatch falhou\n");
                if (fgDoJogo)
                {
                    // o preparo da geracao do "jogo" com a camera deste quadro (o que o modulo captura)
                    FgJogoPreparo pp{};
                    pp.header.type = 0x00020004u; // FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE
                    pp.frameID = q + 1;
                    pp.commandList = cl.Get();
                    pp.renderSize = {RW, RH};
                    pp.motionVectorScale = u.motionVectorScale;
                    pp.frameTimeDelta = u.frameTimeDelta;
                    pp.cameraNear = u.cameraNear, pp.cameraFar = u.cameraFar;
                    pp.cameraFovAngleVertical = u.cameraFovAngleVertical, pp.viewSpaceToMetersFactor = 1.f;
                    pp.depth = u.depth, pp.motionVectors = u.motionVectors;
                    FgJogoCamera cm{};
                    cm.header.type = 0x0002000au; // FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO
                    cena.CameraMundo(double(q) / fps, cm.cameraPosition, cm.cameraUp, cm.cameraRight, cm.cameraForward);
                    pp.header.pNext = &cm.header;
                    fsrDespacha(&fgDoJogo, &pp.header); // so o preparo: a geracao do jogo nunca e configurada
                }
                barreira(saidaAgora.r.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
                if (adiantado)
                {
                    barreira(cor.r.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                    barreira(mov.r.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                    barreira(prof.r.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                    Ck(cl->Close(), "close");
                    if (q > 0)
                    {
                        // o quadro anterior: a copia da saida dele para o backbuffer e o Present (este ja foi gravado)
                        Tex& anterior = (q & 1) ? saidaFsr : saidaFsr2;
                        Ck(al2->Reset(), "reset2");
                        Ck(cl2->Reset(al2.Get(), nullptr), "list2");
                        ComPtr<ID3D12Resource> bbAnt;
                        sc->GetBuffer(sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bbAnt));
                        D3D12_RESOURCE_BARRIER x[2] = {};
                        x[0].Transition = {anterior.r.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                                           D3D12_RESOURCE_STATE_COPY_SOURCE};
                        x[1].Transition = {bbAnt.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT,
                                           D3D12_RESOURCE_STATE_COPY_DEST};
                        cl2->ResourceBarrier(2, x);
                        cl2->CopyResource(bbAnt.Get(), anterior.r.Get());
                        for (auto& b : x)
                            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
                        cl2->ResourceBarrier(2, x);
                        Ck(cl2->Close(), "close2");
                        ID3D12CommandList* lb[] = {cl2.Get()};
                        fila->ExecuteCommandLists(1, lb);
                        sc->Present(0, 0);
                    }
                    ID3D12CommandList* la[] = {cl.Get()};
                    fila->ExecuteCommandLists(1, la); // este quadro, gravado antes do Present acima, vai depois dele
                    Ck(fila->Signal(cerca.Get(), ++valor), "signal");
                    cerca->SetEventOnCompletion(valor, ev);
                    WaitForSingleObject(ev, INFINITE);
                    if (fpsDepoisQ && UINT64(v) * N + i + 1 == fpsDepoisQ)
                        passo = std::chrono::microseconds(1000000 / std::max(fpsDepois, 1));
                    proximo += passo;
                    std::this_thread::sleep_until(proximo);
                    continue;
                }
                ID3D12Resource* final_ = saidaTela.r.Get(); // o que vai para o backbuffer
                Tex* alvoHud = nullptr;
                if (hud)
                {
                    // o pos do "jogo" depois do FSR: tonemap -> [pos-efeito] -> HUD por cima
                    ID3D12DescriptorHeap* hs[] = {hudDesc.Get()};
                    cl->SetDescriptorHeaps(1, hs);
                    cl->SetComputeRootSignature(hudRaizC.Get());
                    const uint32_t k[4] = {uint32_t(q), uint32_t(pos), W, H};
                    cl->SetComputeRoot32BitConstants(0, 4, k, 0);
                    auto gpu = [&](int n) {
                        D3D12_GPU_DESCRIPTOR_HANDLE h = hudDesc->GetGPUDescriptorHandleForHeapStart();
                        h.ptr += UINT64(n) * hudInc;
                        return h;
                    };
                    barreira(saidaTela.r.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    barreira(hudA.r.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    cl->SetPipelineState(hudTom.Get());
                    cl->SetComputeRootDescriptorTable(1, gpu(0));
                    cl->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                    barreira(saidaTela.r.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                    alvoHud = &hudA;
                    if (pos)
                    {
                        barreira(hudA.r.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                        barreira(hudB.r.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        cl->SetPipelineState(hudPos.Get());
                        cl->SetComputeRootDescriptorTable(1, gpu(2));
                        cl->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                        barreira(hudA.r.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                        alvoHud = &hudB;
                    }
                    barreira(alvoHud->r.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    desenhaHud(alvoHud == &hudB ? 1 : 0);
                    barreira(alvoHud->r.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    final_ = alvoHud->r.Get();
                }
                else
                    barreira(saidaTela.r.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                esperaApresentacao(); // o backbuffer seguinte so existe depois do Present do anterior
                ComPtr<ID3D12Resource> bb;
                sc->GetBuffer(sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
                barreira(bb.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
                cl->CopyResource(bb.Get(), final_);
                barreira(bb.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
                barreira(final_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                barreira(cor.r.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                barreira(mov.r.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                barreira(prof.r.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                // enviar a lista sem fechar: o runtime remove o dispositivo por chamada invalida (sem a camada de validacao,
                // comandos mal formados como uma copia entre formatos diferentes passam e nao derrubam nada)
                const bool aqui = invalida && q + 1 == invalida;
                const HRESULT fechou = aqui ? S_FALSE : cl->Close();
                if (!aqui)
                    Ck(fechou, "close");
                ID3D12CommandList* lj[] = {cl.Get()};
                fila->ExecuteCommandLists(1, lj);
                if (aqui)
                {
                    printf("invalida: Close 0x%08lx; dispositivo 0x%08lx\n", (unsigned long)fechou,
                           (unsigned long)dev->GetDeviceRemovedReason());
                    fflush(stdout);
                    Sleep(3000); // o vigia do modulo relata
                    printf("invalida: dispositivo 0x%08lx depois de 3 s\n", (unsigned long)dev->GetDeviceRemovedReason());
                    fflush(stdout);
                    ExitProcess(9);
                }
                if (apresentador.joinable())
                {
                    Ck(fila->Signal(cerca.Get(), ++valor), "signal"); // so a lista do quadro (o Present corre a parte)
                    apresentando = true;
                    SetEvent(pedeApresentar);
                }
                else
                {
                    sc->Present(0, 0);
                    Ck(fila->Signal(cerca.Get(), ++valor), "signal");
                }
                cerca->SetEventOnCompletion(valor, ev);
                WaitForSingleObject(ev, INFINITE);
                if (trava && UINT64(v) * N + i + 1 == trava)
                    Sleep(8000);
                if (recria && UINT64(v) * N + i + 1 == recria)
                {
                    esperaApresentacao();
                    // como um jogo que troca o modo de tela: solta o swapchain e cria outro na mesma janela
                    sc.Reset();
                    for (int k = 0; k < 20; ++k) // a DXGI solta a janela um pouco depois (como um jogo entre telas)
                    {
                        bomba();
                        Sleep(10);
                    }
                    ComPtr<IDXGIFactory2> fab;
                    Ck(CreateDXGIFactory2(0, IID_PPV_ARGS(&fab)), "CreateDXGIFactory2");
                    ComPtr<IDXGISwapChain1> sc1;
                    const HRESULT hr = fab->CreateSwapChainForHwnd(fila.Get(), jan, &sdJogo, nullptr, nullptr, &sc1);
                    if (FAILED(hr))
                    {
                        printf("recriar o swapchain falhou: 0x%08lx\n", (unsigned long)hr);
                        fflush(stdout);
                        return 8;
                    }
                    sc1.As(&sc);
                    sc->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
                    printf("swapchain recriado no quadro %llu\n", (unsigned long long)recria);
                    fflush(stdout);
                }
                if (fpsDepoisQ && UINT64(v) * N + i + 1 == fpsDepoisQ)
                    passo = std::chrono::microseconds(1000000 / std::max(fpsDepois, 1));
                proximo += passo;
                std::this_thread::sleep_until(proximo);
                continue;
            }
            dlss5ov::EntradasJogo e;
            e.cor = cor.r.Get();
            e.corEstado = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            e.mov = mov.r.Get();
            e.prof = prof.r.Get();
            e.movEspacoW = RW, e.movEspacoH = RH;
            if (render < 1.f)
                e.movEscalaX = float(RW), e.movEscalaY = float(RH); // uv -> pixels de render, como o UE5 passa ao FSR
            e.perto = perto;
            e.fovY = fovY;
            e.profInvertida = e.profInfinita = true;
            e.reset = i == 0 && (!laco || v == 0);
            cli.EsperaEdicao(mx);
            const uint64_t n = cli.Quadro(cl.Get(), e);
            if (mapa && n)
                fprintf(mapa, "{\"jogo\":%llu,\"seq\":%u,\"volta\":%d}\n", (unsigned long long)n, i, v);
            const bool grava = v == voltas - 1;
            barreira(cor.r.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (grava)
            {
                D3D12_TEXTURE_COPY_LOCATION d{le.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                d.PlacedFootprint = cor.fp;
                D3D12_TEXTURE_COPY_LOCATION s{cor.r.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            }
            if (sc)
            {
                // o quadro vai para o backbuffer do swapchain da AMD; a profundidade e o movimento, para a geracao
                ComPtr<ID3D12Resource> bb;
                sc->GetBuffer(sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
                barreira(bb.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
                cl->CopyResource(bb.Get(), cor.r.Get());
                barreira(bb.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
                dlss5ov::QuadroGeracao g;
                g.prof = prof.r.Get();
                g.profEstado = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                g.mov = mov.r.Get();
                g.movEstado = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                g.renderW = RW, g.renderH = RH;
                g.movEscX = render < 1.f ? float(RW) : 1.f;
                g.movEscY = render < 1.f ? float(RH) : 1.f;
                g.perto = perto > 0.f ? perto : 0.1f;
                g.longe = 0.f;
                g.fovY = fovY;
                g.msQuadro = 1000.f / float(std::max(fps, 1));
                g.invertida = g.infinita = true;
                g.hdr = true;
                g.reset = e.reset;
                if (!ger.Quadro(cl.Get(), g, multi) && v == 0 && i == 0)
                    fprintf(stderr, "geracao: %s\n", ger.UltimoErro());
            }
            barreira(cor.r.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            barreira(mov.r.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            barreira(prof.r.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Ck(cl->Close(), "close");
            ID3D12CommandList* l[] = {cl.Get()};
            fila->ExecuteCommandLists(1, l);
            cli.Submetido(fila.Get());
            if (sc)
                sc->Present(0, 0);
            Ck(fila->Signal(cerca.Get(), ++valor), "signal");
            cerca->SetEventOnCompletion(valor, ev);
            WaitForSingleObject(ev, INFINITE);
            if (n && cli.Aplicado() == n)
                ++aplicados;
            if (grava)
            {
                unsigned char* r = nullptr;
                le->Map(0, nullptr, (void**)&r);
                wchar_t nome[64];
                swprintf(nome, 64, L"\\%04u_saida.raw", i);
                FILE* a = nullptr;
                _wfopen_s(&a, (saida + nome).c_str(), L"wb");
                if (a)
                {
                    for (UINT y = 0; y < H; ++y)
                        fwrite(r + size_t(y) * cor.fp.Footprint.RowPitch, 1, size_t(W) * 8, a);
                    fclose(a);
                    ++gravados;
                }
                D3D12_RANGE nada{0, 0};
                le->Unmap(0, &nada);
            }
            if (fpsDepoisQ && UINT64(v) * N + i + 1 == fpsDepoisQ)
                passo = std::chrono::microseconds(1000000 / std::max(fpsDepois, 1));
            proximo += passo;
            std::this_thread::sleep_until(proximo);
        }
    esperaApresentacao();
    if (apresentador.joinable())
    {
        sairApresentador = true;
        SetEvent(pedeApresentar);
        apresentador.join();
    }
    printf("quadros com a edicao do overlay: %llu de %u; gravados %llu\n", (unsigned long long)aplicados, N * voltas,
           (unsigned long long)gravados);
    const double seg = std::chrono::duration<double>(std::chrono::steady_clock::now() - inicio).count();
    const auto& d = cli.diag;
    printf("%.1f quadros/s (pedido %d); exportados %llu, idade media da edicao %.2f quadros; sem atraso: %llu esperas, "
           "media %.1f ms, max %.1f ms, %llu estouradas\n",
           double(N) * voltas / std::max(seg, 1e-3), fps, (unsigned long long)d.exportados,
           d.aplicados ? double(d.idadeSoma) / double(d.aplicados) : 0.0, (unsigned long long)d.esperas,
           d.esperas ? d.esperaMs / double(d.esperas) : 0.0, d.esperaMsMax, (unsigned long long)d.estouros);
    if (sc && !comoJogo)
    {
        DXGI_FRAME_STATISTICS st{};
        const HRESULT hs = sc->GetFrameStatistics(&st);
        printf("geracao: %llu quadros gerados pela AMD em %.1f s (%.1f/s); apresentados %.1f/s (jogo %.1f/s + gerados)%s\n",
               (unsigned long long)ger.Gerados(), seg, ger.Gerados() / std::max(seg, 1e-3),
               (double(N) * voltas + double(ger.Gerados())) / std::max(seg, 1e-3), double(N) * voltas / std::max(seg, 1e-3),
               SUCCEEDED(hs) ? "" : " [sem estatisticas do swapchain]");
        if (SUCCEEDED(hs))
            printf("geracao: estatisticas do swapchain: PresentCount %u, PresentRefreshCount %u\n", st.PresentCount,
                   st.PresentRefreshCount);
    }
    if (mapa)
        fclose(mapa);
    if (raw)
        printf("mouse injetado recebido pelo jogo (entrada bruta): %ld eventos, dx %ld dy %ld, %ld cliques\n", g_rawEventos,
               g_rawDx, g_rawDy, g_rawCliques);
    if (!comoJogo)
        cli.Encerra();
    if (comoJogo && sc)
    {
        // como um jogo que fecha: solta o swapchain e da um tempo antes de sair (o log do modulo mostra a saida dele)
        sc.Reset();
        printf("swapchain solto\n");
        fflush(stdout);
        Sleep(1500);
    }
    return 0;
}
