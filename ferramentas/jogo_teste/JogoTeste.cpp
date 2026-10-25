// Jogo de teste da captura de buffer: faz o papel do jogo com o mesmo ClienteBuffer que vai dentro dele (sem ganchos).
// Le uma sequencia de gera_sintetico_buffer.py (cor, movimento e profundidade exatos), entrega cada quadro ao cliente
// como o upscaler do jogo entregaria e grava a cor que sai (com a edicao do overlay somada).
// uso: JogoTeste.exe <sequencia> <saida> [--voltas N] [--fps F] [--laco]   (grava so a ultima volta)
// --laco: a sequencia e um laco (gera_sintetico_buffer.py --laco): so o primeiro quadro avisa corte de cena
// --render R: como o Gears (UE5 + FSR): movimento e profundidade na resolucao de render (R x a da cor), o movimento em
//   unidade de tela (uv) com a escala = tamanho de render -- o que o FSR recebe de um jogo com escala de resolucao
#include "../../src/buffer/ClienteBuffer.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
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
    bool raw = false, laco = false;
    std::wstring modulo;
    for (int i = 3; i + 1 < argc; ++i)
    {
        if (!wcscmp(argv[i], L"--voltas"))
            voltas = _wtoi(argv[i + 1]);
        if (!wcscmp(argv[i], L"--fps"))
            fps = _wtoi(argv[i + 1]);
        if (!wcscmp(argv[i], L"--modulo"))
            modulo = argv[i + 1];
        if (!wcscmp(argv[i], L"--render"))
            render = std::clamp(float(_wtof(argv[i + 1])), 0.25f, 1.f);
    }
    for (int i = 3; i < argc; ++i)
    {
        raw |= !wcscmp(argv[i], L"--raw");
        laco |= !wcscmp(argv[i], L"--laco");
    }
    CreateDirectoryW(saida.c_str(), nullptr);
    // tamanho e quadros: meta.jsonl
    FILE* f = nullptr;
    _wfopen_s(&f, (seq + L"\\meta.jsonl").c_str(), L"r");
    if (!f)
        return 2;
    char linha[4096];
    fgets(linha, sizeof linha, f);
    fclose(f);
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
    HWND jan = CreateWindowW(wc.lpszClassName, L"jogo teste buffer", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 640, 360,
                             nullptr, nullptr, wc.hInstance, nullptr);
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
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> fila;
    Ck(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&fila)), "fila");
    ComPtr<ID3D12CommandAllocator> al;
    ComPtr<ID3D12GraphicsCommandList> cl;
    Ck(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al)), "al");
    Ck(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(&cl)), "cl");
    cl->Close();
    ComPtr<ID3D12Fence> cerca;
    UINT64 valor = 0;
    Ck(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&cerca)), "cerca");
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    auto cria = [&](Tex& t, DXGI_FORMAT fmt, UINT bpp, bool uav, UINT tw, UINT th) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = tw;
        rd.Height = th;
        rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.Format = fmt;
        rd.SampleDesc.Count = 1;
        rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        Ck(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t.r)),
           "textura");
        dev->GetCopyableFootprints(&rd, 0, 1, 0, &t.fp, nullptr, nullptr, nullptr);
        t.bpp = bpp;
    };
    Tex cor, mov, prof;
    cria(cor, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, true, W, H);
    cria(mov, DXGI_FORMAT_R32G32_FLOAT, 8, false, RW, RH);
    cria(prof, DXGI_FORMAT_R32_FLOAT, 4, false, RW, RH);
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

    dlss5ov::ClienteBuffer cli;
    std::mutex mx; // o modulo no jogo protege o cliente com um mutex (o Execute vem de outras threads); aqui so a forma
    if (!cli.Inicia(dev.Get(), "JogoTeste.exe"))
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
    for (int i = 0; i < 6000 && !cli.OverlayVivo(); ++i)
    {
        bomba();
        Sleep(10);
    }
    if (!cli.OverlayVivo())
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
    uint64_t aplicados = 0, gravados = 0;
    FILE* mapa = nullptr; // quadro do jogo -> quadro da sequencia (alinha a gravacao do overlay)
    _wfopen_s(&mapa, (saida + L"\\mapa.jsonl").c_str(), L"w");
    const auto passo = std::chrono::microseconds(1000000 / std::max(fps, 1));
    auto proximo = std::chrono::steady_clock::now();
    const auto inicio = proximo;
    for (int v = 0; v < voltas; ++v)
        for (UINT i = 0; i < N; ++i)
        {
            bomba();
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
            barreira(cor.r.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            barreira(mov.r.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            barreira(prof.r.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Ck(cl->Close(), "close");
            ID3D12CommandList* l[] = {cl.Get()};
            fila->ExecuteCommandLists(1, l);
            cli.Submetido(fila.Get());
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
            proximo += passo;
            std::this_thread::sleep_until(proximo);
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
    if (mapa)
        fclose(mapa);
    if (raw)
        printf("mouse injetado recebido pelo jogo (entrada bruta): %ld eventos, dx %ld dy %ld, %ld cliques\n", g_rawEventos,
               g_rawDx, g_rawDy, g_rawCliques);
    cli.Encerra();
    return 0;
}
