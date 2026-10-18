// DLSS5 Overlay: captura a janela do jogo (Windows Graphics Capture), roda a rede DLSS 5 (lmxxf, MIT) sobre a imagem e
// mostra o resultado num overlay por cima da janela. Nada e injetado no jogo.
//
// Atalhos:  Ctrl+Alt+D  liga/desliga no jogo em primeiro plano
//           Ctrl+Alt+N  modo: rede -> original -> comparacao -> so a edicao
//           Ctrl+Alt+S  estilo 0 -> 1 -> 2
//           Ctrl+Alt+Q  sai
// Teste:    DLSS5Overlay.exe --alvo "titulo" --quadros N --grava saida.rgba
#include "Comum.h"
#include "Captura.h"
#include "Config.h"
#include "Fluxo.h"
#include "Temporal.h"
#include "Overlay.h"
#include "Rede.h"
#include "Shaders.h"
#include <shellapi.h>
#include <winrt/base.h>
#include <algorithm>
#include <chrono>
#include <thread>

using namespace ov;

namespace {

std::wstring PastaDoExe()
{
    wchar_t b[MAX_PATH];
    GetModuleFileNameW(nullptr, b, MAX_PATH);
    std::wstring s(b);
    return s.substr(0, s.find_last_of(L"\\/"));
}

struct Busca
{
    std::wstring titulo;
    HWND achou = nullptr;
};

BOOL CALLBACK PorTitulo(HWND h, LPARAM l)
{
    auto* b = reinterpret_cast<Busca*>(l);
    if (!IsWindowVisible(h))
        return TRUE;
    wchar_t t[512] = {};
    GetWindowTextW(h, t, 512);
    if (wcsstr(t, b->titulo.c_str()))
    {
        b->achou = h;
        return FALSE;
    }
    return TRUE;
}

struct App
{
    Gpu gpu;
    Descritores desc;
    Rede rede;
    Fluxo fluxo;
    Temporal temporal;
    Captura cap;
    Overlay ov;
    Config cfg;
    Passe pInteiro, pReduz, pCompoe;
    Tex inteiro, fed, saida, fedAnt, saidaAnt;
    std::wstring dumpPasta;
    ComPtr<ID3D12CommandAllocator> al[2];
    ComPtr<ID3D12GraphicsCommandList> cl[2];
    std::wstring pasta;
    bool redeOk = false;
    bool zeraHistorico = true;
    UINT64 quadros = 0;
    // teste
    int quadrosTeste = 0;
    std::wstring gravaArquivo;

    void Inicia()
    {
        pasta = PastaDoExe();
        LogAbre(pasta);
        cfg.Carrega(pasta + L"\\config.json");
        gpu.Cria();
        desc.Cria(gpu.dev.Get(), 512);
        for (int i = 0; i < 2; ++i)
        {
            Ck(gpu.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al[i])), "allocator");
            Ck(gpu.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al[i].Get(), nullptr, IID_PPV_ARGS(&cl[i])),
               "list");
            cl[i]->Close();
        }
        pInteiro.Cria(gpu.dev.Get(), hlsl::Prepara, "CSInteiro", 1, 1, 4, "inteiro");
        pReduz.Cria(gpu.dev.Get(), hlsl::Prepara, "CSReduz", 1, 1, 4, "reduz");
        pCompoe.Cria(gpu.dev.Get(), hlsl::Compoe, "CSCompoe", 4, 1, 8, "compoe");
        fluxo.Cria(gpu.dev.Get());
        temporal.Cria(gpu.dev.Get());
        redeOk = rede.Inicia(gpu, pasta + L"\\runtime");
        if (!redeOk)
            Log("app", "a rede nao subiu: o overlay mostra o quadro original");
        cap.Cria(gpu);
        if (!ov.Cria(gpu, GetModuleHandleW(nullptr)))
            throw std::runtime_error("overlay nao foi criado");
    }

    void Encerra()
    {
        gpu.Espera();
        rede.Encerra();
        cap.Para();
        ov.Destroi();
        cfg.Salva(pasta + L"\\config.json");
    }

    void Liga(HWND h)
    {
        if (!h || h == ov.Janela())
            return;
        if (cap.Inicia(h))
            zeraHistorico = true;
    }

    void Desliga()
    {
        cap.Para();
        ov.Mostra(false);
    }

    // tamanho em que a rede roda: altura configurada (ou a da janela, se menor), largura pela proporcao, pares
    void TamanhoRede(UINT fw, UINT fh, UINT& dw, UINT& dh) const
    {
        const UINT alvo = (UINT)std::clamp(cfg.alturaRede, 360, 2160);
        dh = std::min(fh, alvo);
        dw = (UINT)std::lround(double(fw) * dh / fh);
        dw = std::min<UINT>(dw & ~1u, 3840);
        dh = std::min<UINT>(dh & ~1u, 2176);
    }

    void Garante(UINT fw, UINT fh)
    {
        UINT dw, dh;
        TamanhoRede(fw, fh, dw, dh);
        if (inteiro.w == fw && inteiro.h == fh && fed.w == dw && fed.h == dh)
            return;
        gpu.Espera();
        inteiro.Cria(gpu.dev.Get(), fw, fh, DXGI_FORMAT_R16G16B16A16_FLOAT, L"inteiro");
        fed.Cria(gpu.dev.Get(), dw, dh, DXGI_FORMAT_R16G16B16A16_FLOAT, L"alimentado", true, true);
        saida.Cria(gpu.dev.Get(), fw, fh, DXGI_FORMAT_R8G8B8A8_UNORM, L"saida");
        fedAnt.Cria(gpu.dev.Get(), dw, dh, DXGI_FORMAT_R16G16B16A16_FLOAT, L"alimentado anterior", false);
        if (!dumpPasta.empty())
            saidaAnt.Cria(gpu.dev.Get(), fw, fh, DXGI_FORMAT_R8G8B8A8_UNORM, L"saida anterior", false);
        fluxo.Garante(gpu.dev.Get(), dw, dh);
        fluxo.Reinicia();
        temporal.Garante(gpu.dev.Get(), dw, dh);
        temporal.Reinicia();
        zeraHistorico = true;
        Log("app", Fmt("janela %ux%u, rede %ux%u", fw, fh, dw, dh));
    }

    bool Quadro()
    {
        Captura::Quadro q;
        if (!cap.Pega(q))
            return false;
        const auto t0 = std::chrono::steady_clock::now();
        RECT r{};
        if (IsIconic(cap.Alvo()) || !Captura::RetanguloCliente(cap.Alvo(), r))
        {
            ov.Mostra(false);
            return false;
        }
        Garante(q.w, q.h);
        // o overlay cobre a area cliente; se o tamanho ainda nao bate com o capturado, espera o proximo quadro
        r.right = r.left + LONG(q.w);
        r.bottom = r.top + LONG(q.h);
        ov.Posiciona(r);
        desc.Reinicia();

        const D3D12_RESOURCE_STATES SRV =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        // ---- lista 1: entrada ----
        Ck(al[0]->Reset(), "reset0");
        Ck(cl[0]->Reset(al[0].Get(), nullptr), "list0");
        auto* c = cl[0].Get();
        Ck(gpu.fila->Wait(cap.Cerca(), q.cercaValor), "Wait(captura)");
        Barreira(c, q.tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        inteiro.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        UINT k[4] = {inteiro.w, inteiro.h, fed.w, fed.h};
        pInteiro.Roda(c, desc, gpu.dev.Get(), {{q.tex, DXGI_FORMAT_B8G8R8A8_UNORM}},
                      {{inteiro.r.Get(), inteiro.fmt}}, k, (inteiro.w + 7) / 8, (inteiro.h + 7) / 8);
        inteiro.Para(c, SRV);
        fed.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        pReduz.Roda(c, desc, gpu.dev.Get(), {{inteiro.r.Get(), inteiro.fmt}}, {{fed.r.Get(), fed.fmt}}, k,
                    (fed.w + 7) / 8, (fed.h + 7) / 8);
        fed.Para(c, SRV);
        Barreira(c, q.tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        // movimento simulado (fluxo optico contra o quadro anterior): alimenta o historico temporal da rede
        const bool temMov = cfg.movimento && fluxo.Calcula(c, desc, gpu.dev.Get(), fed.r.Get(), fed.fmt);
        const int ritmo = std::clamp(cfg.ritmo, 1, 4);
        const bool vezDaRede = !temporal.Valido() || !temMov || (quadros % (UINT64)ritmo) == 0;
        const bool usaRede = redeOk && rede.Pronta() && cfg.modo != 1 && vezDaRede;
        bool preparou = false;
        if (usaRede)
            preparou = rede.Prepara(c, fed.r.Get(), fed.estado, fed.w, fed.h,
                                    temMov ? fluxo.Movimento().r.Get() : nullptr, fluxo.Movimento().estado,
                                    zeraHistorico || !temMov, cfg.rede);
        Ck(c->Close(), "close0");
        ID3D12CommandList* l0[] = {c};
        gpu.fila->ExecuteCommandLists(1, l0);
        if (preparou)
            rede.Enfileira(gpu.fila.Get());

        // ---- lista 2: saida, composicao, apresentacao ----
        Ck(al[1]->Reset(), "reset1");
        Ck(cl[1]->Reset(al[1].Get(), nullptr), "list1");
        c = cl[1].Get();
        ID3D12Resource* resp = preparou ? rede.Saida(c) : nullptr;
        DXGI_FORMAT fmtResp = fed.fmt;
        if (resp)
        {
            const auto d = resp->GetDesc();
            fmtResp = d.Format;
            if (quadros == 0)
                Log("rede", Fmt("resposta %llux%u formato %d dim %d", (unsigned long long)d.Width, d.Height,
                                (int)d.Format, (int)d.Dimension));
            if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width != fed.w || d.Height != fed.h)
            {
                Log("rede", "resposta fora do formato esperado: desligando a rede");
                redeOk = false;
                resp = nullptr;
            }
        }
        // edicao estabilizada no tempo (e reprojetada nos quadros sem rede)
        Tex* edicao = (cfg.modo != 1 && redeOk)
                          ? temporal.Roda(c, desc, gpu.dev.Get(), fed, fedAnt, resp, fmtResp,
                                          temMov ? &fluxo.Movimento() : nullptr, cfg.temporal)
                          : nullptr;
        saida.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        struct
        {
            UINT fw, fh, dw, dh;
            float forca;
            UINT modo;
            float divisao;
            UINT usaEdicao;
        } kc{inteiro.w, inteiro.h, fed.w, fed.h, edicao ? cfg.forca : 0.f, (UINT)cfg.modo, cfg.divisao, edicao ? 1u : 0u};
        pCompoe.Roda(c, desc, gpu.dev.Get(),
                     {{inteiro.r.Get(), inteiro.fmt},
                      {fed.r.Get(), fed.fmt},
                      edicao ? std::pair<ID3D12Resource*, DXGI_FORMAT>{edicao->r.Get(), edicao->fmt}
                             : std::pair<ID3D12Resource*, DXGI_FORMAT>{fed.r.Get(), fed.fmt},
                      temMov ? std::pair<ID3D12Resource*, DXGI_FORMAT>{fluxo.Movimento().r.Get(), fluxo.Movimento().fmt}
                             : std::pair<ID3D12Resource*, DXGI_FORMAT>{fed.r.Get(), fed.fmt}},
                     {{saida.r.Get(), saida.fmt}}, &kc, (saida.w + 7) / 8, (saida.h + 7) / 8);
        saida.Para(c, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (!dumpPasta.empty() && !(quadrosTeste > 0 && quadros + 1 >= (UINT64)quadrosTeste))
        {
            saidaAnt.Para(c, D3D12_RESOURCE_STATE_COPY_DEST);
            c->CopyResource(saidaAnt.r.Get(), saida.r.Get());
        }
        ID3D12Resource* bb = ov.Buffer();
        const bool apresenta = bb && ov.Largura() == saida.w && ov.Altura() == saida.h;
        if (apresenta)
        {
            Barreira(c, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            c->CopyResource(bb, saida.r.Get());
            Barreira(c, bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        }
        if (!dumpPasta.empty() && quadrosTeste > 0 && quadros + 1 >= (UINT64)quadrosTeste)
            ultimoTemMov = temMov; // o ultimo quadro do dump: fedAnt guarda o anterior
        else
        {
            fed.Para(c, D3D12_RESOURCE_STATE_COPY_SOURCE);
            fedAnt.Para(c, D3D12_RESOURCE_STATE_COPY_DEST);
            c->CopyResource(fedAnt.r.Get(), fed.r.Get());
            fed.Para(c, SRV);
        }
        Ck(c->Close(), "close1");
        ID3D12CommandList* l1[] = {c};
        gpu.fila->ExecuteCommandLists(1, l1);
        if (apresenta)
        {
            ov.Apresenta();
            ov.Mostra(true);
        }
        gpu.Espera();
        if (preparou)
        {
            rede.Aposenta();
            zeraHistorico = false;
        }
        if ((quadros == 3 || quadros % 300 == 0) && preparou)
            Log("rede", rede.Status());
        ++quadros;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        somaMs += ms;
        maxMs = std::max(maxMs, ms);
        if (quadros % 60 == 0)
        {
            Log("tempo", Fmt("quadros %llu: media %.2f ms, pior %.2f ms (captura -> rede -> overlay)",
                             (unsigned long long)quadros, somaMs / 60.0, maxMs));
            somaMs = maxMs = 0;
        }
        return true;
    }
    double somaMs = 0, maxMs = 0;
    bool ultimoTemMov = false;

    void GravaSaida(const std::wstring& arq) { GravaTex(saida, arq, 4); }

    // grava uma textura crua (bpp bytes por pixel) para conferencia
    void GravaTex(Tex& t, const std::wstring& arq, UINT bpp)
    {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        const auto d = t.r->GetDesc();
        gpu.dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> rb;
        Ck(gpu.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&rb)),
           "readback");
        Ck(al[0]->Reset(), "reset");
        Ck(cl[0]->Reset(al[0].Get(), nullptr), "list");
        t.Para(cl[0].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src{t.r.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        cl[0]->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        cl[0]->Close();
        ID3D12CommandList* l[] = {cl[0].Get()};
        gpu.fila->ExecuteCommandLists(1, l);
        gpu.Espera();
        unsigned char* p = nullptr;
        rb->Map(0, nullptr, (void**)&p);
        FILE* f = nullptr;
        _wfopen_s(&f, arq.c_str(), L"wb");
        if (f)
        {
            for (UINT y = 0; y < t.h; ++y)
                fwrite(p + fp.Offset + size_t(y) * fp.Footprint.RowPitch, 1, size_t(t.w) * bpp, f);
            fclose(f);
        }
        rb->Unmap(0, nullptr);
        Log("teste", Fmt("gravado %ux%u: %s", t.w, t.h, Utf8(arq).c_str()));
    }
};

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    App app;
    std::wstring alvoTitulo;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (!wcscmp(argv[i], L"--alvo"))
            alvoTitulo = argv[++i];
        else if (!wcscmp(argv[i], L"--quadros"))
            app.quadrosTeste = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--grava"))
            app.gravaArquivo = argv[++i];
        else if (!wcscmp(argv[i], L"--dump"))
            app.dumpPasta = argv[++i];
    }
    try
    {
        app.Inicia();
    }
    catch (const std::exception& e)
    {
        Log("erro", e.what());
        MessageBoxA(nullptr, e.what(), "DLSS5 Overlay", MB_ICONERROR);
        return 1;
    }
    RegisterHotKey(nullptr, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'D');
    RegisterHotKey(nullptr, 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'N');
    RegisterHotKey(nullptr, 3, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'S');
    RegisterHotKey(nullptr, 4, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q');
    if (!alvoTitulo.empty())
    {
        Busca b{alvoTitulo};
        EnumWindows(PorTitulo, reinterpret_cast<LPARAM>(&b));
        if (!b.achou)
            Log("teste", "janela nao encontrada: " + Utf8(alvoTitulo));
        app.Liga(b.achou);
    }
    Log("app", "pronto: Ctrl+Alt+D liga no jogo em primeiro plano");
    using clk = std::chrono::steady_clock;
    auto ultimo = clk::now();
    bool rodando = true;
    while (rodando)
    {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
        {
            if (m.message == WM_QUIT)
                rodando = false;
            if (m.message == WM_HOTKEY)
            {
                switch (m.wParam)
                {
                case 1:
                    if (app.cap.Ativa())
                        app.Desliga();
                    else
                        app.Liga(GetForegroundWindow());
                    break;
                case 2:
                    app.cfg.modo = (app.cfg.modo + 1) % 5;
                    Log("app", Fmt("modo %d", app.cfg.modo));
                    break;
                case 3:
                    app.cfg.rede.estilo = float((int(app.cfg.rede.estilo + 0.5f) + 1) % 3);
                    app.zeraHistorico = true;
                    Log("app", Fmt("estilo %.0f", app.cfg.rede.estilo));
                    break;
                case 4:
                    rodando = false;
                    break;
                }
            }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (!rodando)
            break;
        if (!app.cap.Ativa())
        {
            app.ov.Mostra(false);
            if (app.quadrosTeste > 0 && !alvoTitulo.empty())
                break; // teste sem alvo
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
            continue;
        }
        HANDLE ev = app.cap.EventoQuadro();
        MsgWaitForMultipleObjects(1, &ev, FALSE, 100, QS_ALLINPUT);
        if (app.cfg.fpsMax > 0)
        {
            const auto minimo = std::chrono::microseconds(1000000 / app.cfg.fpsMax);
            const auto agora = clk::now();
            if (agora - ultimo < minimo)
                std::this_thread::sleep_for(minimo - (agora - ultimo));
        }
        try
        {
            if (app.Quadro())
            {
                ultimo = clk::now();
                if (app.quadrosTeste > 0 && (int)app.quadros >= app.quadrosTeste)
                {
                    if (!app.gravaArquivo.empty())
                        app.GravaSaida(app.gravaArquivo);
                    if (!app.dumpPasta.empty())
                    {
                        app.GravaTex(app.fed, app.dumpPasta + L"\\fed_atual.f16", 8);
                        app.GravaTex(app.fedAnt, app.dumpPasta + L"\\fed_anterior.f16", 8);
                        app.GravaTex(app.saida, app.dumpPasta + L"\\saida_atual.rgba", 4);
                        app.GravaTex(app.saidaAnt, app.dumpPasta + L"\\saida_anterior.rgba", 4);
                        if (app.ultimoTemMov)
                            app.GravaTex(app.fluxo.Movimento(), app.dumpPasta + L"\\movimento.f16", 4);
                    }
                    break;
                }
            }
        }
        catch (const std::exception& e)
        {
            Log("erro", e.what());
            app.Desliga();
        }
    }
    app.Encerra();
    return 0;
}
