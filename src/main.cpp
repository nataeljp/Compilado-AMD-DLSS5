// DLSS5 Overlay: captura a janela do jogo (Windows Graphics Capture), roda a rede DLSS 5 (lmxxf, MIT) sobre a imagem e
// mostra o resultado num overlay por cima da janela. Nada e injetado no jogo. Opcional (Modo de Rede = captura de
// buffer): a DLSS5Buffer.dll no jogo aberto entrega cor sem HUD, profundidade e movimento do upscaler dele (FSR).
//
// Atalhos:  Ctrl+Shift+D  liga/desliga no jogo em primeiro plano
//           Ctrl+Shift+N  modo: rede -> original -> comparacao -> so a edicao
//           Ctrl+Shift+S  estilo 0 -> 1 -> 2
//           Ctrl+Shift+G  grava os proximos quadros em bruto (bin\gravacoes\), para analise
//           Ctrl+Shift+Q  sai
// Teste:    DLSS5Overlay.exe --alvo "titulo" --quadros N --grava saida.rgba
#include <DirectXPackedVector.h>
#include "Comum.h"
#include "Abertura.h"
#include "Captura.h"
#include "Config.h"
#include "FonteBuffer.h"
#include "Fluxo.h"
#include "Injecao.h"
#include "Temporal.h"
#include "Overlay.h"
#include "Painel.h"
#include "Mascara.h"
#include "Rede.h"
#include "Shaders.h"
#include <shellapi.h>
#include <wincodec.h>
#include <winrt/base.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
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
    Mascara mascara;           // mascara de desenvolvedor: pele, cabelo, personagem e objetos com controles proprios
    bool mascaraTentou = false; // o classificador (bin\ml) carrega na primeira vez que a mascara e ligada
    bool mascaraAtivaAntes = false;
    // a mascara so roda (classificador, rastreio) quando algo a usa: intensidade por grupo, ajuste na rede ou mostrar
    bool MascaraAtiva() const
    {
        return cfg.dev.liga && mascara.Pronta() && (cfg.dev.PorIntensidade() || cfg.dev.PorPixel() || cfg.dev.mostra);
    }
    ParamsRede ParamsDaRede() const
    {
        ParamsRede p = cfg.Rede();
        p.mascaraPixel = p.mascaraPixel && mascara.Pronta() && rede.TemMascara();
        return p;
    }
    Captura cap;
    Overlay ov;
    Painel painel;
    bool comPainel = true;
    double fps = 0, msMedio = 0;
    std::chrono::steady_clock::time_point marcaFps = std::chrono::steady_clock::now();
    UINT64 quadrosMarca = 0;
    Config cfg;
    Passe pInteiro, pReduz, pCompoe, pExporta;
    // ---- captura de buffer (Modo de Rede): a DLSS5Buffer.dll no jogo aberto entrega cor sem HUD, movimento e
    // profundidade do upscaler dele; a edicao volta para o jogo, que a soma no quadro seguinte ----
    FonteBuffer fb;
    bool buffer = false;        // ligado pela captura de buffer (sem WGC e sem a janela do overlay)
    HWND alvoBuffer = nullptr;
    std::string estadoBuffer;   // linha de estado no painel
    int fonteLigada = 0;        // a fonte com que ligou (trocar no painel religa)
    uint64_t quadroJogo = 0;    // numero do quadro do jogo em processamento (gravacao: alinha com o jogo)
    int gravaEm = 0;            // --grava-em N: comeca a gravar no quadro N (testes)
    bool invertidaUsada = true; // Z invertido em uso (do jogo, ou medido quando ele nao diz)
    bool Ligado() const { return cap.Ativa() || buffer; }
    HWND Alvo() const { return buffer ? alvoBuffer : cap.Alvo(); }
    Tex inteiro, orig, fed, saida, fedAnt, saidaAnt; // orig: quadro no tamanho da rede; fed: o proxy dele
    std::wstring dumpPasta;
    std::wstring pastaRuntime; // --runtime: outra pasta do runtime (testes)
    std::vector<uint32_t> pulaTeste; // --pula 1,2,3: blocos pulados fixos (medicao de custo por bloco)
    bool temPulaTeste = false;
    int alternaAltura = 0; // --alterna 720: modo imagem troca a altura da rede a cada 4 quadros e registra cada quadro
    int trocaRedeNoQuadro = -1; // --troca-rede N: modo imagem troca grande <-> pequena no quadro N (teste da troca ao vivo)
    bool redePequenaAtiva = false;
    std::wstring PastaRede() const { return pastaRuntime.empty() ? pasta + L"\\runtime" : pastaRuntime; }
    // troca grande <-> pequena ao vivo: a rede e recriada (o historico recomeca)
    void TrocaRede()
    {
        gpu.Espera();
        rede.Encerra();
        redeOk = rede.Inicia(gpu, PastaRede(), cfg.redePequena);
        redePequenaAtiva = cfg.redePequena;
        nivelAtual = -1;
        zeraHistorico = true;
        temporal.Reinicia();
        Log("rede", redeOk ? "rede trocada" : "a rede nao subiu depois da troca");
    }
    std::wstring configArq;    // --config: outro config.json (comparacoes); nao e regravado
    ComPtr<ID3D12CommandAllocator> al[2];
    ComPtr<ID3D12GraphicsCommandList> cl[2];
    std::wstring pasta;
    bool redeOk = false;
    bool zeraHistorico = true;
    UINT64 quadros = 0;
    // teste
    int quadrosTeste = 0;
    std::wstring gravaArquivo;

    Abertura* abertura = nullptr; // tela de abertura (so no uso normal; nos testes fica nula)
    void Etapa(const wchar_t* t)
    {
        if (abertura)
            abertura->Etapa(t);
    }
    void Inicia()
    {
        const auto t0 = std::chrono::steady_clock::now();
        auto marca = [&](const char* o) {
            Log("app", Fmt("%s em %.1f s", o, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()));
        };
        pasta = PastaDoExe();
        LogAbre(pasta);
        Etapa(L"Iniciando a GPU...");
        cfg.Carrega(configArq.empty() ? pasta + L"\\config.json" : configArq);
        gpu.Cria();
        desc.Cria(gpu.dev.Get(), 512);
        marca("GPU pronta");
        Etapa(L"Compilando os shaders...");
        for (int i = 0; i < 2; ++i)
        {
            Ck(gpu.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al[i])), "allocator");
            Ck(gpu.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al[i].Get(), nullptr, IID_PPV_ARGS(&cl[i])),
               "list");
            cl[i]->Close();
        }
        const std::string prepara = std::string(hlsl::Comum) + hlsl::Prepara;
        const std::string compoe = std::string(hlsl::Comum) + hlsl::Compoe;
        pInteiro.Cria(gpu.dev.Get(), prepara.c_str(), "CSInteiro", 1, 1, 16, "inteiro");
        pReduz.Cria(gpu.dev.Get(), prepara.c_str(), "CSReduz", 1, 2, 16, "reduz");
        pCompoe.Cria(gpu.dev.Get(), compoe.c_str(), "CSCompoe", 5, 1, 32, "compoe");
        pExporta.Cria(gpu.dev.Get(), (std::string(hlsl::Comum) + hlsl::Exporta).c_str(), "CSExporta", 3, 1, 8,
                      "edicao para o jogo");
        fluxo.Cria(gpu.dev.Get());
        temporal.Cria(gpu.dev.Get());
        mascara.Cria(gpu.dev.Get());
        EsperaPasses();
        if (cfg.temporalRede > 0)
        {
            // historico temporal da propria rede (o runtime le o ambiente que copiou ao carregar: tem de vir antes)
            SetEnvironmentVariableW(L"DLSS5_TEMPORAL_MODE", std::to_wstring(std::clamp(cfg.temporalRede, 0, 3)).c_str());
            SetEnvironmentVariableW(L"DLSS5_TEMPORAL_ENHANCE_STRENGTH",
                                    std::to_wstring(std::clamp(cfg.realceTemporal, 0.f, 2.f)).c_str());
            cfg.reusoVit = false; // incompativel com o historico
        }
        if (cfg.reusoVit)
        {
            // reuso adaptativo do ViT (lmxxf): o runtime le uma vez, ao criar a rede
            SetEnvironmentVariableW(L"DLSS5_VIT_ADAPTIVE", L"1");
            SetEnvironmentVariableW(L"DLSS5_VIT_REUSE_PERIOD", L"4");
            SetEnvironmentVariableW(L"DLSS5_HIP_GRAPH", L"0");
            SetEnvironmentVariableW(L"DLSS5_HIP_VIT_BYTE_STREAM", L"0");
        }
        marca("shaders prontos");
        Etapa(cfg.redePequena ? L"Carregando a rede DLSS 5 (pequena)..." : L"Carregando a rede DLSS 5...");
        redeOk = rede.Inicia(gpu, PastaRede(), cfg.redePequena);
        marca(redeOk ? "rede carregada" : "rede falhou");
        Etapa(L"Preparando a captura e o painel...");
        redePequenaAtiva = cfg.redePequena;
        if (!redeOk)
            Log("app", "a rede nao subiu: o overlay mostra o quadro original");
        cap.Cria(gpu);
        if (!ov.Cria(gpu, GetModuleHandleW(nullptr)))
            throw std::runtime_error("overlay nao foi criado");
        ov.OcultaDeGravacoes(cfg.ocultaDeGravacoes);
        if (comPainel && !painel.Cria(gpu, GetModuleHandleW(nullptr), pasta))
            throw std::runtime_error("painel nao foi criado");
    }

    void Encerra()
    {
        gpu.Espera();
        EncerraGravacao();
        rede.Encerra();
        cap.Para();
        FechaFonte();
        painel.Destroi();
        ov.Destroi();
        if (configArq.empty())
            cfg.Salva(perfilCarregado ? PerfilArq() : pasta + L"\\config.json");
    }

    // ---- perfis por jogo: bin\perfis\<exe>.json, carregado sozinho quando o overlay liga no mesmo exe ----
    std::wstring exeAlvo;          // processo do jogo capturado
    bool perfilCarregado = false;  // cfg veio do perfil do exe (e e nele que os ajustes sao gravados ao sair)
    static std::wstring NomeExe(HWND h)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        std::wstring r;
        if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
        {
            wchar_t b[MAX_PATH];
            DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(p, 0, b, &n))
            {
                r.assign(b, n);
                r = r.substr(r.find_last_of(L"\\/") + 1);
            }
            CloseHandle(p);
        }
        return r;
    }
    std::wstring PerfilArq() const
    {
        std::wstring n = exeAlvo;
        const auto pt = n.find_last_of(L'.');
        if (pt != std::wstring::npos)
            n.resize(pt);
        return pasta + L"\\perfis\\" + n + L".json";
    }
    bool PerfilExiste() const { return !exeAlvo.empty() && GetFileAttributesW(PerfilArq().c_str()) != INVALID_FILE_ATTRIBUTES; }
    void AplicaConfig(const Config& c)
    {
        cfg = c;
        zeraHistorico = true;
        temporal.Reinicia();
    }
    void CarregaPerfil(HWND h)
    {
        exeAlvo = NomeExe(h);
        if (!configArq.empty()) // testes com --config: o config do teste manda
            return;
        if (PerfilExiste())
        {
            Config c;
            c.Carrega(PerfilArq());
            AplicaConfig(c);
            perfilCarregado = true;
            Log("perfil", "carregado: " + Utf8(PerfilArq()));
        }
        else if (perfilCarregado)
        {
            // o jogo anterior tinha perfil e este nao: volta ao padrao (config.json), sem herdar o do outro jogo
            Config c;
            c.Carrega(pasta + L"\\config.json");
            AplicaConfig(c);
            perfilCarregado = false;
            Log("perfil", "sem perfil para " + Utf8(exeAlvo) + ": padrao (config.json)");
        }
    }

    // ---- como a Game Bar: o overlay e o painel so com o jogo (ou o proprio painel) em foco ----
    bool pausado = false;
    bool JogoEmFoco() const
    {
        HWND alvo = Alvo();
        if (!alvo || !IsWindow(alvo) || IsIconic(alvo) || !IsWindowVisible(alvo))
            return false;
        HWND fg = GetForegroundWindow();
        if (fg == alvo || fg == painel.Janela() || fg == ov.Janela())
            return true;
        if (!fg)
            return false;
        DWORD pa = 0, pf = 0;
        GetWindowThreadProcessId(alvo, &pa);
        GetWindowThreadProcessId(fg, &pf);
        return pa == pf; // outra janela do proprio jogo
    }
    // fora de foco (Alt+Tab, minimizado): o overlay some e nada e processado (a GPU fica para o que estiver na frente)
    void Pausa()
    {
        if (pausado)
            return;
        pausado = true;
        ov.Mostra(false);
    }
    // de volta: o que estava em voo e o historico sao de antes da pausa (o movimento parou de ser medido)
    void Retoma()
    {
        if (!pausado)
            return;
        pausado = false;
        fluxo.Reinicia();
        temporal.Reinicia();
        quadroCorte = quadros;
        zeraHistorico = true;
    }
    // o overlay acompanha a janela mesmo sem quadro novo (jogo parado, arrastando a janela); redimensionando (o quadro
    // ainda e do tamanho velho) ele some ate chegar o quadro do tamanho novo
    void SegueJanela()
    {
        RECT r{};
        if (imagem || buffer || !inteiro.w || !Captura::RetanguloCliente(cap.Alvo(), r))
            return;
        const LONG cw = r.right - r.left, ch = r.bottom - r.top;
        if (std::abs(cw - LONG(inteiro.w)) > 2 || std::abs(ch - LONG(inteiro.h)) > 2)
        {
            ov.Mostra(false);
            return;
        }
        r.right = r.left + LONG(inteiro.w);
        r.bottom = r.top + LONG(inteiro.h);
        ov.Posiciona(r, hdr);
    }

    void Liga(HWND h)
    {
        if (!h || h == ov.Janela() || h == painel.Janela())
            return;
        fonteLigada = cfg.fonte;
        if (cfg.fonte == 1)
        {
            LigaBuffer(h);
            return;
        }
        CarregaPerfil(h);
        painel.Ancora(h);
        painel.DefineJogo(h);
        float esc = 1.f;
        hdr = MonitorHdr(gpu, h, esc);
        escalaSdr = esc;
        Log("app", hdr ? Fmt("monitor em HDR: branco SDR = %.0f nits", esc * 80.f) : std::string("monitor em SDR"));
        if (cap.Inicia(h, hdr))
            zeraHistorico = true;
    }
    bool hdr = false;
    UINT pontePassada = 0;
    float escalaSdr = 1.f;
    float escalaUsada = 1.f;

    // o painel escreve na memoria compartilhada com o jogo (o mouse do painel): solta antes de ela sair. Sem isso o
    // Ctrl+Shift+D (desligar) deixava o painel com o ponteiro da memoria ja desfeita, e o quadro seguinte escrevia nela:
    // o overlay fechava sem erro (0xc0000005 em Painel::DefineMouseJogo)
    void FechaFonte()
    {
        painel.DefineMouseJogo(nullptr);
        fb.Fecha();
    }
    void Desliga()
    {
        cap.Para();
        if (buffer)
        {
            gpu.Espera();
            FechaFonte(); // sem batimento o modulo do jogo para de exportar e de somar
            buffer = false;
            alvoBuffer = nullptr;
            fluxo.Reinicia();
            temporal.Reinicia();
            zeraHistorico = true;
        }
        ov.Mostra(false);
    }
    // captura de buffer: coloca a DLSS5Buffer.dll no jogo aberto (sem gravar nada na pasta dele) e espera o modulo
    // aparecer (ele cria a memoria compartilhada no primeiro quadro do FSR/XeSS do jogo)
    void LigaBuffer(HWND h)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        CarregaPerfil(h);
        painel.Ancora(h);
        painel.DefineJogo(h);
        ResultadoInjecao r;
        painel.DefineMouseJogo(nullptr); // Abre fecha a memoria compartilhada de antes
        if (fb.Abre(gpu, pid))
            r.ok = true; // o modulo ja esta no jogo (religou, ou e o jogo de teste)
        else
            r = InjetaBuffer(pid, pasta + L"\\captura\\DLSS5Buffer.dll");
        if (!r.ok)
        {
            estadoBuffer = "nao conectou: " + r.motivo;
            Log("buffer", estadoBuffer);
            return;
        }
        if (!r.motivo.empty())
            Log("buffer", r.motivo);
        buffer = true;
        alvoBuffer = h;
        hdr = true; // a cor do jogo antes do tonemap e linear
        escalaSdr = 1.f;
        estadoBuffer = r.upscaler.empty() ? "esperando o jogo usar FSR ou XeSS (escolha no menu grafico do jogo)"
                                          : "esperando o primeiro quadro do " + r.upscaler;
        if (!r.motivo.empty())
            estadoBuffer += " | " + r.motivo;
        fluxo.Reinicia();
        temporal.Reinicia();
        zeraHistorico = true;
        ov.Mostra(false);
        Log("buffer", "ligado em " + Utf8(exeAlvo));
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
        const DXGI_FORMAT fmtSaida = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
        if (inteiro.w == fw && inteiro.h == fh && fed.w == dw && fed.h == dh && saida.fmt == fmtSaida)
            return;
        gpu.Espera();
        rede.Descarta(); // a resposta em voo e do tamanho antigo
        inteiro.Cria(gpu.dev.Get(), fw, fh, DXGI_FORMAT_R16G16B16A16_FLOAT, L"inteiro");
        orig.Cria(gpu.dev.Get(), dw, dh, DXGI_FORMAT_R16G16B16A16_FLOAT, L"original no tamanho da rede");
        fed.Cria(gpu.dev.Get(), dw, dh, DXGI_FORMAT_R16G16B16A16_FLOAT, L"proxy (alimentado)", true, true);
        saida.Cria(gpu.dev.Get(), fw, fh, fmtSaida, L"saida");
        fedAnt.Cria(gpu.dev.Get(), dw, dh, DXGI_FORMAT_R16G16B16A16_FLOAT, L"alimentado anterior", false);
        if (!dumpPasta.empty())
            saidaAnt.Cria(gpu.dev.Get(), fw, fh, fmtSaida, L"saida anterior", false);
        fluxo.Garante(gpu.dev.Get(), dw, dh);
        fluxo.Reinicia();
        temporal.Garante(gpu.dev.Get(), dw, dh);
        temporal.Reinicia();
        zeraHistorico = true;
        Log("app", Fmt("janela %ux%u, rede %ux%u", fw, fh, dw, dh));
    }

    // ---- modo imagem (comparacao com as fotos da NVIDIA): um arquivo no lugar da captura, sem overlay ----
    ComPtr<ID3D12Resource> imagem;
    UINT imgW = 0, imgH = 0;
    DXGI_FORMAT imgFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    // ---- reproducao (--reproduz pasta): os quadros gravados (Ctrl+Shift+G) entram como entrada, um por quadro ----
    bool reproducao = false;
    std::wstring pastaReproducao;
    int quadrosReproducao = 0;
    ComPtr<ID3D12Resource> uploadReproducao;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fpReproducao{};
    bool AbreReproducao(const std::wstring& p)
    {
        FILE* f = nullptr;
        _wfopen_s(&f, (p + L"\\meta.jsonl").c_str(), L"r");
        if (!f)
            return false;
        char linha[1024];
        std::string cab = fgets(linha, sizeof linha, f) ? linha : "";
        int n = 0;
        while (fgets(linha, sizeof linha, f))
            ++n;
        fclose(f);
        auto num = [&](const char* chave) {
            const auto i = cab.find(chave);
            return i == std::string::npos ? 0.0 : std::atof(cab.c_str() + i + strlen(chave));
        };
        imgW = UINT(num("\"rede_w\":"));
        imgH = UINT(num("\"rede_h\":"));
        if (!imgW || !imgH || !n)
            return false;
        hdr = num("\"hdr\":") > 0;
        escalaSdr = float(num("\"escala\":"));
        quadrosReproducao = n;
        pastaReproducao = p;
        reproducao = true;
        imgFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = imgW;
        rd.Height = imgH;
        rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.Format = imgFmt;
        rd.SampleDesc.Count = 1;
        Ck(gpu.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(&imagem)),
           "textura(reproducao)");
        UINT64 total = 0;
        gpu.dev->GetCopyableFootprints(&rd, 0, 1, 0, &fpReproducao, nullptr, nullptr, &total);
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Ck(gpu.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&uploadReproducao)),
           "upload(reproducao)");
        Log("reproducao", Fmt("%d quadros %ux%u de %s", n, imgW, imgH, Utf8(p).c_str()));
        return true;
    }
    // carrega o quadro gravado i na textura de entrada (GPU ociosa)
    void CarregaQuadroReproducao(int i)
    {
        wchar_t arq[64];
        swprintf(arq, 64, L"\\%04d_orig.raw", i);
        FILE* f = nullptr;
        _wfopen_s(&f, (pastaReproducao + arq).c_str(), L"rb");
        if (!f)
            throw std::runtime_error("quadro gravado nao encontrado");
        unsigned char* m = nullptr;
        uploadReproducao->Map(0, nullptr, (void**)&m);
        for (UINT y = 0; y < imgH; ++y)
            fread(m + fpReproducao.Offset + size_t(y) * fpReproducao.Footprint.RowPitch, 1, size_t(imgW) * 8, f);
        uploadReproducao->Unmap(0, nullptr);
        fclose(f);
        Ck(al[0]->Reset(), "reset");
        Ck(cl[0]->Reset(al[0].Get(), nullptr), "list");
        Barreira(cl[0].Get(), imagem.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{imagem.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION src{uploadReproducao.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint = fpReproducao;
        cl[0]->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barreira(cl[0].Get(), imagem.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        cl[0]->Close();
        ID3D12CommandList* l[] = {cl[0].Get()};
        gpu.fila->ExecuteCommandLists(1, l);
        gpu.Espera();
    }
    float hdrSintetico = 0; // --hdr-sintetico <nits>: a foto vira scRGB (SoftKnee inverso x branco) para testar o caminho HDR
    bool CarregaImagem(const std::wstring& arq)
    {
        ComPtr<IWICImagingFactory> f;
        ComPtr<IWICBitmapDecoder> dec;
        ComPtr<IWICBitmapFrameDecode> fr;
        ComPtr<IWICFormatConverter> conv;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) ||
            FAILED(f->CreateDecoderFromFilename(arq.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
            FAILED(dec->GetFrame(0, &fr)) || FAILED(f->CreateFormatConverter(&conv)) ||
            FAILED(conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                    WICBitmapPaletteTypeCustom)) ||
            FAILED(conv->GetSize(&imgW, &imgH)))
        {
            Log("imagem", "nao consegui ler " + Utf8(arq));
            return false;
        }
        std::vector<unsigned char> px(size_t(imgW) * imgH * 4);
        Ck(conv->CopyPixels(nullptr, imgW * 4, UINT(px.size()), px.data()), "CopyPixels");
        UINT bpp = 4;
        if (hdrSintetico > 0)
        {
            // HDR sintetico: linear * (o inverso do SoftKnee: o ombro vira folga acima do branco) * branco em scRGB
            const float esc = hdrSintetico / 80.f;
            std::vector<unsigned char> h(size_t(imgW) * imgH * 8);
            auto lin = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
            for (size_t i = 0; i < size_t(imgW) * imgH; ++i)
            {
                float c[3];
                for (int k = 0; k < 3; ++k)
                    c[k] = lin(px[i * 4 + k] / 255.f);
                const float m = std::max(c[0], std::max(c[1], c[2]));
                if (m > 0.75f)
                {
                    const float y = std::min(m, 0.99999f);
                    const float inv = 0.75f - 0.25f * std::log(1.f - (y - 0.75f) / 0.25f);
                    for (float& v : c)
                        v *= inv / m;
                }
                uint16_t* d = reinterpret_cast<uint16_t*>(&h[i * 8]);
                for (int k = 0; k < 3; ++k)
                    d[k] = DirectX::PackedVector::XMConvertFloatToHalf(c[k] * esc);
                d[3] = DirectX::PackedVector::XMConvertFloatToHalf(1.f);
            }
            px.swap(h);
            bpp = 8;
            imgFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
        }
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = imgW;
        rd.Height = imgH;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = imgFmt;
        rd.SampleDesc.Count = 1;
        Ck(gpu.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&imagem)),
           "textura(imagem)");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        gpu.dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> up;
        Ck(gpu.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&up)),
           "upload(imagem)");
        unsigned char* m = nullptr;
        up->Map(0, nullptr, (void**)&m);
        for (UINT y = 0; y < imgH; ++y)
            memcpy(m + fp.Offset + size_t(y) * fp.Footprint.RowPitch, px.data() + size_t(y) * imgW * bpp, size_t(imgW) * bpp);
        up->Unmap(0, nullptr);
        Ck(al[0]->Reset(), "reset");
        Ck(cl[0]->Reset(al[0].Get(), nullptr), "list");
        D3D12_TEXTURE_COPY_LOCATION dst{imagem.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION src{up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint = fp;
        cl[0]->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barreira(cl[0].Get(), imagem.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        cl[0]->Close();
        ID3D12CommandList* l[] = {cl[0].Get()};
        gpu.fila->ExecuteCommandLists(1, l);
        gpu.Espera();
        hdr = hdrSintetico > 0;
        escalaSdr = hdr ? hdrSintetico / 80.f : 1.f;
        Log("imagem", Fmt("imagem %ux%u%s: %s", imgW, imgH, hdr ? " (HDR sintetico)" : "", Utf8(arq).c_str()));
        return true;
    }

    // ---- profundidade da rede: niveis de blocos residuais pulados (do mais barato de perder ao mais caro) ----
    static std::vector<uint32_t> BlocosDoNivel(int n)
    {
        std::vector<uint32_t> v;
        auto faixa = [&](uint32_t a, uint32_t b) { for (uint32_t i = a; i <= b; ++i) v.push_back(i); };
        if (n >= 1 && n < 2)
            v = {42, 43, 46};
        if (n >= 2)
            faixa(40, 47);
        if (n >= 3)
            faixa(23, 30);
        if (n >= 4)
            faixa(31, 38);
        if (n >= 5)
            faixa(1, 4), faixa(66, 69);
        return v;
    }
    int nivelAtual = -1, nivelAlvo = 0, quadrosNoNivel = 0;
    void AjustaProfundidade(bool temMov)
    {
        // a rede pequena ja vem com os blocos dela (network-layout.txt); os niveis de profundidade sao da grande
        if (!rede.TemProfundidade() || redePequenaAtiva)
            return;
        if (temPulaTeste)
        {
            if (nivelAtual < 0 && !rede.DefineProfundidade(pulaTeste))
                Log("rede", "--pula recusado pelo runtime");
            nivelAtual = 99;
            return;
        }
        int alvo = std::clamp(cfg.profundidade, 0, 5);
        if (cfg.profundidadeAuto && temMov && estat.valida)
        {
            // velocidade em pixels de uma rede de 1080 linhas
            const float v = estat.velocidade * 1080.f / float(std::max(1u, fed.h));
            const int pelaVelocidade = v < 3.f ? 0 : v < 8.f ? 1 : v < 16.f ? 2 : v < 32.f ? 3 : v < 64.f ? 4 : 5;
            alvo = std::min(std::max(alvo, pelaVelocidade), std::clamp(cfg.profundidadeMax, 0, 5));
        }
        // histerese: fica mais raso na hora; volta a fundo so depois de 8 quadros no mesmo nivel pedido
        if (alvo != nivelAlvo)
        {
            nivelAlvo = alvo;
            quadrosNoNivel = 0;
        }
        else
            ++quadrosNoNivel;
        const bool muda = nivelAtual < 0 || nivelAlvo > nivelAtual || (nivelAlvo < nivelAtual && quadrosNoNivel >= 8);
        if (!muda)
            return;
        if (rede.DefineProfundidade(BlocosDoNivel(nivelAlvo)))
            nivelAtual = nivelAlvo;
        else if (nivelAtual < 0)
            Log("rede", Fmt("profundidade %d recusada pelo runtime", nivelAlvo));
    }

    bool Quadro()
    {
        if (redeOk && cfg.redePequena != redePequenaAtiva)
            TrocaRede();
        if (cfg.dev.liga && !mascaraTentou)
        {
            mascaraTentou = true;
            mascara.Inicia(pasta + L"\\ml", gpu.dev.Get());
        }
        if (MascaraAtiva() != mascaraAtivaAntes)
        {
            mascaraAtivaAntes = MascaraAtiva();
            mascara.Reinicia(quadros); // religada: o estado de quando foi desligada nao vale mais
        }
        Captura::Quadro q;
        FonteBuffer::Quadro qb;
        if (buffer)
        {
            if (!IsWindow(alvoBuffer))
            {
                Desliga();
                return false;
            }
            if (!fb.Aberta())
            {
                DWORD pid = 0;
                GetWindowThreadProcessId(alvoBuffer, &pid);
                painel.DefineMouseJogo(nullptr);
                if (!fb.Abre(gpu, pid))
                    return false;
            }
            fb.Batimento(true, cfg.modo != 1 && redeOk);
            if (!fb.Pega(qb))
            {
                if (fb.Aberta() && !fb.JogoVivo())
                    estadoBuffer = "o jogo parou de mandar quadros (FSR/XeSS desligado ou jogo pausado)";
                return false;
            }
            quadroJogo = qb.n;
            q.tex = qb.cor->r.Get();
            q.fmt = qb.cor->fmt;
            q.w = qb.cor->w;
            q.h = qb.cor->h;
            q.cercaValor = 0;
            estadoBuffer = Fmt("conectado: %ux%u, cor sem HUD, profundidade e movimento do jogo", q.w, q.h);
            if (qb.reset)
                mascara.Reinicia(quadros);
            if (qb.reset && temporal.Valido())
            {
                temporal.Reinicia(); // o jogo avisou corte de cena
                zeraHistorico = true;
                quadroCorte = quadros;
            }
        }
        else if (imagem)
        {
            q.tex = imagem.Get();
            q.fmt = imgFmt;
            q.w = imgW;
            q.h = imgH;
            q.cercaValor = 0;
        }
        else if (!cap.Pega(q))
            return false;
        const auto t0 = std::chrono::steady_clock::now();
        RECT r{};
        if (!imagem && !buffer && (IsIconic(cap.Alvo()) || !Captura::RetanguloCliente(cap.Alvo(), r)))
        {
            ov.Mostra(false);
            return false;
        }
        Garante(q.w, q.h);
        if (!imagem && !buffer)
        {
            // o overlay cobre a area cliente
            r.right = r.left + LONG(q.w);
            r.bottom = r.top + LONG(q.h);
            ov.Posiciona(r, hdr);
        }
        desc.Reinicia();

        const D3D12_RESOURCE_STATES SRV =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        // ---- lista 1: entrada ----
        Ck(al[0]->Reset(), "reset0");
        Ck(cl[0]->Reset(al[0].Get(), nullptr), "list0");
        auto* c = cl[0].Get();
        if (q.cercaValor)
            Ck(gpu.fila->Wait(cap.Cerca(), q.cercaValor), "Wait(captura)");
        Barreira(c, q.tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        inteiro.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // branco de papel: o do usuario em nits ou o branco SDR do Windows (HDR); ponte: auto = SDR direto, HDR SoftKnee
        float escK = cfg.brancoNits > 0.f ? cfg.brancoNits / 80.f : escalaSdr;
        escalaUsada = escK;
        const UINT ponte = cfg.ponte < 0 ? (hdr ? 1u : 0u) : (UINT)std::clamp(cfg.ponte, 0, 3);
        pontePassada = ponte;
        UINT k[16] = {inteiro.w, inteiro.h, fed.w, fed.h, hdr ? 1u : 0u, 0, ponte, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        memcpy(&k[11], &cfg.preLimiarSombra, 4);
        memcpy(&k[12], &cfg.preCor, 4);
        k[13] = reproducao ? 1u : 0u;
        memcpy(&k[8], &cfg.preLuz, 4);
        memcpy(&k[9], &cfg.preSombra, 4);
        memcpy(&k[10], &cfg.preLimiar, 4);
        memcpy(&k[5], &escK, 4);
        const float expK = std::max(cfg.temporal.exposicao, 1e-3f);
        memcpy(&k[7], &expK, 4);
        pInteiro.Roda(c, desc, gpu.dev.Get(), {{q.tex, q.fmt}},
                      {{inteiro.r.Get(), inteiro.fmt}}, k, (inteiro.w + 7) / 8, (inteiro.h + 7) / 8);
        inteiro.Para(c, SRV);
        orig.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        fed.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        pReduz.Roda(c, desc, gpu.dev.Get(), {{inteiro.r.Get(), inteiro.fmt}},
                    {{orig.r.Get(), orig.fmt}, {fed.r.Get(), fed.fmt}}, k, (fed.w + 7) / 8, (fed.h + 7) / 8);
        orig.Para(c, SRV);
        fed.Para(c, SRV);
        Barreira(c, q.tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        // movimento simulado (fluxo optico contra o quadro anterior): reprojeta a edicao e traz a resposta atrasada
        bool temMov;
        if (buffer)
        {
            // captura de buffer: o movimento e a profundidade do proprio jogo (exatos), no lugar do fluxo optico
            Fluxo::Camera cam;
            cam.corW = float(q.w);
            cam.corH = float(q.h);
            cam.perto = qb.perto;
            cam.longe = qb.longe;
            cam.invertida = qb.profInvertidaConhecida ? qb.profInvertida : fluxo.InvertidaMedida();
            invertidaUsada = cam.invertida;
            fb.DefineInvertida(invertidaUsada); // o jogo reprojeta a edicao com a mesma conta de distancia
            cam.infinita = qb.profInfinita;
            cam.reset = qb.reset;
            fluxo.DoJogo(c, desc, gpu.dev.Get(), *qb.mov, *qb.prof, cam);
            temMov = true;
        }
        else
            temMov = cfg.movimento && fluxo.Calcula(c, desc, gpu.dev.Get(), fed.r.Get(), fed.fmt);
        if (temMov)
            fluxo.Estatisticas(c, desc, gpu.dev.Get(), fed, fedAnt);
        // mascara de desenvolvedor: a classificacao (quando chega) e o estado seguem o movimento ate este quadro
        if (MascaraAtiva())
            mascara.Quadro(c, desc, gpu.dev.Get(), fed, temMov ? &fluxo.Movimento() : nullptr,
                           temMov ? &fluxo.Confianca() : nullptr, buffer ? fluxo.DistanciaAtual() : nullptr,
                           cfg.temporal.exposicao, quadros, cfg.dev.taxa);
        // A rede roda na fila dela e responde com atraso: este quadro vai para a tela agora, com a ultima edicao; a
        // resposta, quando chega, e do quadro t0 que a rede viu e entra pelo deslocamento acumulado desde t0.
        // sincrono (imagem, reproducao sem --atraso): a rede ve este quadro e responde nele mesmo, como antes.
        const bool sincrono = atraso == 0;
        Tex* deslocamento = nullptr;
        if (!sincrono && rede.EmVoo())
        {
            deslocamento = temporal.Acumula(c, desc, gpu.dev.Get(), temMov ? &fluxo.Movimento() : nullptr,
                                            temMov ? &fluxo.Confianca() : nullptr, deslZerado);
            deslZerado = false;
        }
        temporal.AcumulaGrade(c, desc, gpu.dev.Get(), temMov ? &fluxo.Movimento() : nullptr,
                              temMov ? &fluxo.Confianca() : nullptr);
        const bool podeRede = redeOk && rede.Pronta() && cfg.modo != 1;
        const bool vez = !temporal.Valido() || !temMov || zeraHistorico ||
                         quadros >= quadroEnvio + (UINT64)std::clamp(cfg.ritmo, 1, 4);
        bool envia = sincrono && podeRede && vez;
        if (envia)
        {
            AjustaProfundidade(temMov);
            CopiaParaRede(c);
        }
        Ck(c->Close(), "close0");
        ID3D12CommandList* l0[] = {c};
        gpu.fila->ExecuteCommandLists(1, l0);
        bool chegou = false;
        if (envia)
        {
            if (rede.Envia(gpu.cerca.Get(), gpu.Sinaliza(), ParamsDaRede()))
            {
                MarcaEnvio();
                rede.EsperaChegar();
                chegou = true;
            }
            envia = false;
        }
        else if (rede.EmVoo())
        {
            // com --atraso N (teste) a resposta entra exatamente N quadros depois do envio; ao vivo, quando ficar pronta
            chegou = atraso > 0 ? quadros >= quadroEnvio + (UINT64)atraso : rede.Chegou();
            if (chegou)
                rede.EsperaChegar();
        }
        if (chegou)
            Chegada();

        // ---- lista 2: edicao, composicao, apresentacao (e a entrada da proxima rede) ----
        Ck(al[1]->Reset(), "reset1");
        Ck(cl[1]->Reset(al[1].Get(), nullptr), "list1");
        c = cl[1].Get();
        // edicao estabilizada no tempo (e reprojetada nos quadros sem resposta)
        ParamsTemporal tp = cfg.temporal;
        tp.intensidade = cfg.ngx.intensidade;
        // resposta de um quadro anterior a um corte de cena e da cena velha: descartada
        const bool usa = chegou && cfg.modo != 1 && redeOk && quadroEnvio >= quadroCorte;
        if (usa)
        {
            temporal.Edicao(c, desc, gpu.dev.Get(), origRede, rede.Entrada(), rede.Resposta(), tp);
            temporal.Grade(c, desc, gpu.dev.Get(), origRede, semDeslocamento ? nullptr : deslocamento,
                           buffer && zRede.r ? &zRede : nullptr);
        }
        Tex* edicao = ultimaEdicao = (cfg.modo != 1 && redeOk)
                          ? temporal.Roda(c, desc, gpu.dev.Get(), orig, fed, fedAnt, usa, usa ? &rede.Entrada() : nullptr,
                                          usa && !semDeslocamento ? deslocamento : nullptr,
                                          temMov ? &fluxo.Movimento() : nullptr,
                                          (temMov && cfg.usaConfianca) ? &fluxo.Confianca() : nullptr,
                                          fluxo.BufEstat(), tp, buffer ? fluxo.DistanciaAtual() : nullptr)
                          : nullptr;
        if (usa)
        {
            // de volta ao estado em que a fila da rede pega
            rede.Entrada().Para(c, D3D12_RESOURCE_STATE_COMMON);
            rede.Resposta().Para(c, D3D12_RESOURCE_STATE_COMMON);
        }
        envia = !sincrono && podeRede && vez && !rede.EmVoo();
        if (envia)
        {
            AjustaProfundidade(temMov);
            CopiaParaRede(c);
        }
        // gradacao do Model B/C (constantes do kernel da NVIDIA), escalada pelo tom local
        const float tg = std::clamp(cfg.ngx.tomLocal, 0.f, 1.f);
        float kExp = 0, kCon = 0, kSat = 0;
        if (cfg.ngx.modelo == 1)
            kExp = -0.10f * tg, kCon = -0.25f * tg, kSat = -0.10f * tg;
        else if (cfg.ngx.modelo == 2)
            kSat = -0.15f * tg;
        const bool grada = cfg.ngx.gradacao && cfg.ngx.modelo > 0;
        saida.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        struct
        {
            UINT fw, fh, dw, dh;
            float forca;
            UINT modo;
            float divisao;
            UINT usaEdicao;
            UINT hdr;
            float escala, kExp, kCon, kSat;
            UINT grada, filmica, pad1;
            float preLuz, preSombra, preLimiar, preLimiarSombra, preCor;
            UINT pad2, pad3, pad4;
            float dlssGrupo[4];
            UINT usaMascara, pad5, pad6, pad7;
        } kc{inteiro.w,      inteiro.h,      fed.w, fed.h, edicao ? cfg.forca : 0.f, (UINT)cfg.modo, cfg.divisao,
             edicao ? 1u : 0u, hdr ? 1u : 0u, escalaUsada, kExp, kCon, kSat, grada ? 1u : 0u,
             pontePassada == 3 ? 1u : 0u, (hdr && pontePassada == 1) ? 1u : 0u, cfg.preLuz, cfg.preSombra, cfg.preLimiar, cfg.preLimiarSombra, cfg.preCor,
             0, 0, 0, {}, 0, 0, 0, 0};
        // pele e objetos separados: quanto do DLSS 5 entra em cada grupo (o ControlMask da NVIDIA)
        Tex* estMascara = MascaraAtiva() && cfg.dev.PorIntensidade() ? mascara.Estado() : nullptr;
        for (int g = 0; g < 4; ++g)
            kc.dlssGrupo[g] = std::clamp(cfg.dev.grupo[g].dlss, 0.f, 1.f);
        kc.usaMascara = estMascara ? 1u : 0u;
        if (estMascara)
            estMascara->Para(c, SRV);
        pCompoe.Roda(c, desc, gpu.dev.Get(),
                     {{inteiro.r.Get(), inteiro.fmt},
                      {fed.r.Get(), fed.fmt},
                      edicao ? std::pair<ID3D12Resource*, DXGI_FORMAT>{edicao->r.Get(), edicao->fmt}
                             : std::pair<ID3D12Resource*, DXGI_FORMAT>{fed.r.Get(), fed.fmt},
                      temMov ? std::pair<ID3D12Resource*, DXGI_FORMAT>{fluxo.Movimento().r.Get(), fluxo.Movimento().fmt}
                             : std::pair<ID3D12Resource*, DXGI_FORMAT>{fed.r.Get(), fed.fmt},
                      estMascara ? std::pair<ID3D12Resource*, DXGI_FORMAT>{estMascara->r.Get(), estMascara->fmt}
                                 : std::pair<ID3D12Resource*, DXGI_FORMAT>{fed.r.Get(), fed.fmt}},
                     {{saida.r.Get(), saida.fmt}}, &kc, (saida.w + 7) / 8, (saida.h + 7) / 8);
        if (MascaraAtiva() && cfg.dev.mostra)
            mascara.Mostra(c, desc, gpu.dev.Get(), saida, hdr, escalaUsada); // "Mostrar mascara" (Show Masks)
        // captura de buffer: a edicao (C(saida) - C(cor do jogo)) vai para o slot que o jogo soma no quadro seguinte
        Tex* edJogo = buffer ? fb.EdicaoLivre() : nullptr;
        if (edJogo)
        {
            saida.Para(c, SRV);
            qb.cor->Para(c, SRV);
            qb.prof->Para(c, SRV);
            edJogo->Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            struct
            {
                UINT w, h, profW, profH;
                float perto, longe;
                UINT invertida, infinita;
            } ke{saida.w, saida.h, qb.prof->w, qb.prof->h, qb.perto, qb.longe, invertidaUsada ? 1u : 0u,
                 qb.profInfinita ? 1u : 0u};
            pExporta.Roda(c, desc, gpu.dev.Get(),
                          {{qb.cor->r.Get(), qb.cor->fmt}, {saida.r.Get(), saida.fmt}, {qb.prof->r.Get(), qb.prof->fmt}},
                          {{edJogo->r.Get(), edJogo->fmt}}, &ke, (saida.w + 7) / 8, (saida.h + 7) / 8);
            edJogo->Para(c, D3D12_RESOURCE_STATE_COMMON);
            qb.cor->Para(c, D3D12_RESOURCE_STATE_COMMON);
            qb.prof->Para(c, D3D12_RESOURCE_STATE_COMMON);
        }
        saida.Para(c, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (!dumpPasta.empty() && !(quadrosTeste > 0 && quadros + 1 >= (UINT64)quadrosTeste))
        {
            saidaAnt.Para(c, D3D12_RESOURCE_STATE_COPY_DEST);
            c->CopyResource(saidaAnt.r.Get(), saida.r.Get());
        }
        ID3D12Resource* bb = ov.Buffer();
        const bool apresenta = !imagem && !buffer && bb && ov.Largura() == saida.w && ov.Altura() == saida.h;
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
        if (edJogo)
            fb.Publica(qb.n);
        const UINT64 fim = gpu.Sinaliza();
        if (envia && rede.Envia(gpu.cerca.Get(), fim, ParamsDaRede()))
            MarcaEnvio();
        if (apresenta)
        {
            ov.Apresenta();
            ov.Mostra(true);
        }
        gpu.EsperaValor(fim);
        if (MascaraAtiva())
            mascara.DepoisDoQuadro(); // o quadro reduzido vai para o classificador
        if (buffer)
            fb.Consumido(qb.n);
        estat = fluxo.LeEstatisticas();
        if (estat.valida && estat.semConfianca > 0.88f)
        {
            // corte de cena (o shader ja limpou este quadro): historico e grade da cena velha saem; a resposta em voo,
            // se foi enviada antes deste quadro, e descartada quando chegar; a rede roda logo na cena nova
            temporal.Reinicia();
            mascara.Reinicia(quadros);
            zeraHistorico = true;
            quadroCorte = quadros;
            if (cortes++ % 50 == 0)
                Log("tempo", Fmt("corte de cena no quadro %llu (confianca %.2f)", (unsigned long long)quadros,
                                 1.0 - estat.semConfianca));
        }
        if (gravaEm > 0 && quadros + 1 == (UINT64)gravaEm)
            IniciaGravacao(); // --grava-em N (testes): grava a partir do quadro N
        if (gravando > 0)
            GravaQuadro(usa, temMov, edicao);
        if (usa)
        {
            desdeRede = 0;
            ++quadrosComRede;
        }
        else
            ++desdeRede;
        if ((quadrosComRede == 3 || quadros % 300 == 0) && usa)
            Log("rede", rede.Status());
        ++quadros;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        somaMs += ms;
        maxMs = std::max(maxMs, ms);
        {
            const auto agora = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(agora - marcaFps).count();
            if (dt >= 0.5)
            {
                fps = double(quadros - quadrosMarca) / dt;
                marcaFps = agora;
                quadrosMarca = quadros;
            }
            msMedio = msMedio * 0.95 + ms * 0.05;
        }
        if (quadros % 60 == 0)
        {
            Log("tempo", Fmt("quadros %llu: media %.2f ms, pior %.2f ms (captura -> overlay) | resposta da rede em %.0f%% "
                             "dos quadros, %.1f quadros / %.1f ms depois do envio | confianca %.3f, luz %.4f, vel %.1f px",
                             (unsigned long long)quadros, somaMs / 60.0, maxMs,
                             100.0 * double(quadrosComRede - comRedeMarca) / 60.0,
                             chegadas ? somaAtrasoQ / chegadas : 0.0, chegadas ? somaAtrasoMs / chegadas : 0.0,
                             1.0 - estat.semConfianca, estat.mudancaLuz, estat.velocidade));
            fracaoRede = double(quadrosComRede - comRedeMarca) / 60.0;
            if (MascaraAtiva() && quadros % 300 == 0)
                Log("mascara", mascara.Status());
            comRedeMarca = quadrosComRede;
            somaMs = maxMs = 0;
            somaAtrasoQ = somaAtrasoMs = 0;
            chegadas = 0;
        }
        return true;
    }
    // ---- rede na fila propria ----
    int atraso = -1;         // -1 ao vivo (a resposta entra quando fica pronta); 0 sincrono; N > 0 exatamente N quadros
    bool semDeslocamento = false; // --sem-deslocamento (teste): a resposta atrasada entra onde estava em t0
    Tex origRede;            // o quadro que a rede esta vendo (t0), no tamanho da rede
    Tex zRede;               // captura de buffer: a distancia da camera em t0 (grade por camada)
    UINT64 quadroEnvio = 0;  // t0
    UINT64 quadroCorte = 0;  // ultimo corte de cena: respostas enviadas antes dele sao da cena velha
    UINT64 cortes = 0;
    bool deslZerado = false; // o deslocamento acumulado recomeca em t0
    std::chrono::steady_clock::time_point tEnvio;
    double somaAtrasoQ = 0, somaAtrasoMs = 0;
    int chegadas = 0;
    void CopiaParaRede(ID3D12GraphicsCommandList* c)
    {
        rede.CopiaEntrada(c, fed);
        if (MascaraAtiva() && rede.TemMascara())
            mascara.Codifica(c, desc, gpu.dev.Get(), rede.Entrada()); // grupo e peso de cada pixel no alfa
        if (origRede.w != orig.w || origRede.h != orig.h)
            origRede.Cria(gpu.dev.Get(), orig.w, orig.h, orig.fmt, L"original que a rede viu", false);
        const auto antes = orig.estado;
        orig.Para(c, D3D12_RESOURCE_STATE_COPY_SOURCE);
        origRede.Para(c, D3D12_RESOURCE_STATE_COPY_DEST);
        c->CopyResource(origRede.r.Get(), orig.r.Get());
        orig.Para(c, antes);
        if (Tex* z = buffer ? fluxo.DistanciaAtual() : nullptr)
        {
            if (zRede.w != z->w || zRede.h != z->h)
                zRede.Cria(gpu.dev.Get(), z->w, z->h, z->fmt, L"distancia que a rede viu", false);
            const auto antesZ = z->estado;
            z->Para(c, D3D12_RESOURCE_STATE_COPY_SOURCE);
            zRede.Para(c, D3D12_RESOURCE_STATE_COPY_DEST);
            c->CopyResource(zRede.r.Get(), z->r.Get());
            z->Para(c, antesZ);
        }
    }
    void MarcaEnvio()
    {
        quadroEnvio = quadros;
        deslZerado = true;
        zeraHistorico = false;
        tEnvio = std::chrono::steady_clock::now();
    }
    // a resposta chegou (GPU da rede terminou): confere e libera o trabalho no runtime (a resposta e copia nossa)
    void Chegada()
    {
        somaAtrasoQ += double(quadros - quadroEnvio);
        somaAtrasoMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tEnvio).count();
        ++chegadas;
        rede.Aposenta();
        const Tex& r = rede.Resposta();
        if (quadrosComRede == 0)
            Log("rede", Fmt("resposta %ux%u formato %d", r.w, r.h, (int)r.fmt));
        if (r.w != fed.w || r.h != fed.h)
        {
            Log("rede", "resposta fora do formato esperado: desligando a rede");
            redeOk = false;
        }
    }
    double somaMs = 0, maxMs = 0;
    UINT64 comRedeMarca = 0;
    double fracaoRede = 1;
    bool ultimoTemMov = false;
    Fluxo::Estat estat;
    UINT desdeRede = 0;
    UINT64 quadrosComRede = 0;
    Tex* ultimaEdicao = nullptr;

    void GravaSaida(const std::wstring& arq) { GravaTex(saida, arq, hdr ? 8 : 4); }

    void Painel_()
    {
        if (!comPainel)
            return;
        EstadoPainel e;
        e.placa = gpu.nome;
        e.redeOk = redeOk && rede.Pronta();
        e.fonte = cfg.fonte;
        e.estadoBuffer = estadoBuffer;
        e.estadoMascara = !cfg.dev.liga ? std::string() : !mascaraTentou ? std::string("o classificador carrega quando o DLSS ligar no jogo") : mascara.Status() +
                          (rede.TemMascara() ? "" : " | runtime sem controles por pixel (anterior ao v4)");
        if (Ligado())
        {
            wchar_t t[256] = {};
            GetWindowTextW(Alvo(), t, 256);
            e.alvo = t[0] ? t : L"(sem titulo)";
            e.janelaW = inteiro.w;
            e.janelaH = inteiro.h;
            e.redeW = fed.w;
            e.redeH = fed.h;
            e.msMedio = msMedio;
            e.fps = fps;
            e.fracaoRede = fracaoRede;
        }
        e.exe = Utf8(exeAlvo);
        painel.DicaBandeja(Ligado() && !exeAlvo.empty() ? L"DLSS 5 Overlay - " + exeAlvo : L"DLSS 5 Overlay - sem jogo");
        e.perfilExiste = PerfilExiste();
        e.perfilCarregado = perfilCarregado;
        painel.DefineMouseJogo(buffer && fb.Conectada() && fb.JogoVivo() ? fb.Cab() : nullptr);
        const AcoesPainel a = painel.Quadro(cfg, e);
        if (a.salvarPerfil && !exeAlvo.empty())
        {
            CreateDirectoryW((pasta + L"\\perfis").c_str(), nullptr);
            cfg.Salva(PerfilArq());
            perfilCarregado = true;
            Log("perfil", "salvo: " + Utf8(PerfilArq()));
        }
        if (a.apagarPerfil && PerfilExiste())
        {
            DeleteFileW(PerfilArq().c_str());
            perfilCarregado = false;
            Log("perfil", "apagado: " + Utf8(PerfilArq()));
        }
        if (a.redefinir)
        {
            AplicaConfig(Config{});
            Log("app", "ajustes redefinidos para os padroes");
        }
        if (a.ligar)
            Liga(a.ligar);
        if (a.desligar)
            Desliga();
        if (a.salvar)
        {
            cfg.Salva(pasta + L"\\config.json");
            Log("app", "configuracao salva");
        }
        if (a.zeraHistorico)
        {
            zeraHistorico = true;
            temporal.Reinicia();
        }
        if (a.ocultaMudou)
            ov.OcultaDeGravacoes(cfg.ocultaDeGravacoes);
        if (a.sair)
            PostQuitMessage(0);
        // trocou o Modo de Rede com o jogo ligado: religa na mesma janela pela outra fonte
        if (Ligado() && cfg.fonte != fonteLigada)
        {
            HWND h = Alvo();
            Desliga();
            Liga(h);
        }
    }

    // grava uma textura crua (bpp bytes por pixel) para conferencia
    // ---- gravacao (Ctrl+Shift+G): os proximos quadros em bruto, no tamanho da rede, para analise fora do jogo ----
    // Por quadro: original (RGBA16F), edicao crua da rede (RGBA16F; so vale nos quadros com rede), edicao exibida
    // (RGBA16F), movimento (RG16F, pixels ate o quadro anterior) e confianca (R16F). meta.jsonl diz quais quadros tiveram
    // rede; config.json e o config em uso. Escrita numa thread; ~6 GB no maximo por gravacao.
    int gravando = 0, gravados = 0;
    std::wstring pastaGravacao;
    FILE* metaGravacao = nullptr;
    std::thread escritor;
    std::mutex mxEscrita;
    std::condition_variable cvEscrita;
    std::deque<std::pair<std::wstring, std::vector<unsigned char>>> filaEscrita;
    bool fimEscrita = false;
    struct Leitura
    {
        ComPtr<ID3D12Resource> rb;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
    };
    Leitura leituras[7];

    void IniciaGravacao()
    {
        if (gravando > 0 || !orig.r)
        {
            Log("gravacao", gravando > 0 ? "ja gravando" : "nada para gravar (ligue o overlay no jogo)");
            return;
        }
        const double porQuadro = double(fed.w) * fed.h * (8 + 8 + 8 + 4 + 2);
        gravando = std::clamp(int(6e9 / porQuadro), 8, 240);
        gravados = 0;
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t nome[64];
        swprintf(nome, 64, L"%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        pastaGravacao = pasta + L"\\gravacoes\\" + nome;
        CreateDirectoryW((pasta + L"\\gravacoes").c_str(), nullptr);
        CreateDirectoryW(pastaGravacao.c_str(), nullptr);
        cfg.Salva(pastaGravacao + L"\\config.json");
        _wfopen_s(&metaGravacao, (pastaGravacao + L"\\meta.jsonl").c_str(), L"w");
        if (metaGravacao)
            fprintf(metaGravacao,
                    "{\"rede_w\":%u,\"rede_h\":%u,\"janela_w\":%u,\"janela_h\":%u,\"hdr\":%d,\"escala\":%.4f,"
                    "\"arquivos\":{\"orig\":\"RGBA16F\",\"crua\":\"RGBA16F\",\"edicao\":\"RGBA16F\",\"mov\":\"RG16F\","
                    "\"conf\":\"R16F\"%s},\"quadros\":%d}\n",
                    fed.w, fed.h, inteiro.w, inteiro.h, hdr ? 1 : 0, escalaUsada,
                    MascaraAtiva() ? ",\"mascara\":\"RGBA16F\",\"mascara_sabe\":\"R8\"" : "", gravando);
        if (!escritor.joinable())
            escritor = std::thread([this] {
                for (;;)
                {
                    std::pair<std::wstring, std::vector<unsigned char>> item;
                    {
                        std::unique_lock<std::mutex> lk(mxEscrita);
                        cvEscrita.wait(lk, [&] { return fimEscrita || !filaEscrita.empty(); });
                        if (filaEscrita.empty())
                            return;
                        item = std::move(filaEscrita.front());
                        filaEscrita.pop_front();
                    }
                    cvEscrita.notify_all();
                    FILE* f = nullptr;
                    _wfopen_s(&f, item.first.c_str(), L"wb");
                    if (f)
                    {
                        fwrite(item.second.data(), 1, item.second.size(), f);
                        fclose(f);
                    }
                }
            });
        MessageBeep(MB_OK);
        Log("gravacao", Fmt("gravando %d quadros (rede %ux%u) em %s", gravando, fed.w, fed.h, Utf8(pastaGravacao).c_str()));
    }

    // copia as texturas do quadro (GPU ja ociosa) e entrega os bytes a thread de escrita
    void GravaQuadro(bool comRede, bool comMov, Tex* edicaoExibida)
    {
        // com a mascara de desenvolvedor: o estado dela neste quadro (rgba = pele, cabelo, personagem, objeto)
        Tex* m = MascaraAtiva() ? mascara.Estado() : nullptr;
        Tex* ms = MascaraAtiva() ? mascara.Sabe() : nullptr;
        Tex* tex[7] = {&orig, &temporal.Crua(), edicaoExibida ? edicaoExibida : &temporal.Crua(), &fluxo.Movimento(),
                       &fluxo.Confianca(), m ? m : &orig, ms ? ms : &fluxo.Confianca()};
        const UINT bpp[7] = {8, 8, 8, 4, 2, 8, 1};
        const wchar_t* nomes[7] = {L"orig", L"crua", L"edicao", L"mov", L"conf", L"mascara", L"mascara_sabe"};
        const int n = MascaraAtiva() ? 7 : 5;
        Ck(al[0]->Reset(), "reset");
        Ck(cl[0]->Reset(al[0].Get(), nullptr), "list");
        for (int i = 0; i < n; ++i)
        {
            Leitura& L = leituras[i];
            const auto d = tex[i]->r->GetDesc();
            UINT64 total = 0;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
            gpu.dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
            if (!L.rb || L.total != total)
            {
                D3D12_HEAP_PROPERTIES hp{};
                hp.Type = D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC bd{};
                bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bd.Width = total;
                bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
                bd.SampleDesc.Count = 1;
                bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                L.rb.Reset();
                Ck(gpu.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&L.rb)),
                   "readback(gravacao)");
                L.total = total;
            }
            L.fp = fp;
            tex[i]->Para(cl[0].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{L.rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            dst.PlacedFootprint = fp;
            D3D12_TEXTURE_COPY_LOCATION src{tex[i]->r.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            cl[0]->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        cl[0]->Close();
        ID3D12CommandList* l[] = {cl[0].Get()};
        gpu.fila->ExecuteCommandLists(1, l);
        gpu.Espera();
        for (int i = 0; i < n; ++i)
        {
            Leitura& L = leituras[i];
            std::vector<unsigned char> b(size_t(tex[i]->w) * tex[i]->h * bpp[i]);
            // sem edicao exibida (antes da 1a resposta, depois de um corte de cena) a "edicao" gravada e zero; antes
            // gravava a ultima crua, e as ferramentas mostravam na analise uma edicao velha que nao estava na tela
            if ((i != 2 || edicaoExibida) && (i != 5 || m) && (i != 6 || ms))
            {
                unsigned char* p = nullptr;
                L.rb->Map(0, nullptr, (void**)&p);
                for (UINT y = 0; y < tex[i]->h; ++y)
                    memcpy(b.data() + size_t(y) * tex[i]->w * bpp[i],
                           p + L.fp.Offset + size_t(y) * L.fp.Footprint.RowPitch, size_t(tex[i]->w) * bpp[i]);
                L.rb->Unmap(0, nullptr);
            }
            wchar_t arq[96];
            swprintf(arq, 96, L"\\%04d_%s.raw", gravados, nomes[i]);
            std::unique_lock<std::mutex> lk(mxEscrita);
            cvEscrita.wait(lk, [&] { return filaEscrita.size() < 24; }); // segura o overlay se o disco nao der conta
            filaEscrita.emplace_back(pastaGravacao + arq, std::move(b));
            lk.unlock();
            cvEscrita.notify_all();
        }
        if (metaGravacao)
            fprintf(metaGravacao, "{\"q\":%d,\"jogo\":%llu,\"rede\":%d,\"mov\":%d,\"t_ms\":%.3f}\n", gravados,
                    (unsigned long long)quadroJogo, comRede ? 1 : 0, comMov ? 1 : 0,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count());
        ++gravados;
        if (--gravando == 0)
        {
            if (metaGravacao)
                fclose(metaGravacao), metaGravacao = nullptr;
            MessageBeep(MB_ICONASTERISK);
            Log("gravacao", Fmt("gravacao terminada: %d quadros em %s", gravados, Utf8(pastaGravacao).c_str()));
        }
    }

    void EncerraGravacao()
    {
        if (metaGravacao)
            fclose(metaGravacao), metaGravacao = nullptr;
        {
            std::lock_guard<std::mutex> lk(mxEscrita);
            fimEscrita = true;
        }
        cvEscrita.notify_all();
        if (escritor.joinable())
            escritor.join(); // termina de escrever o que ja estava na fila
    }

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
    std::wstring alvoTitulo, imagemArq;
    bool teclasTeste = false;
    int atrasoArg = -2; // --atraso N: a resposta da rede entra N quadros depois do envio (teste do modo assincrono)
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (!wcscmp(argv[i], L"--alvo"))
            alvoTitulo = argv[++i];
        else if (!wcscmp(argv[i], L"--teclas-teste"))
            teclasTeste = true; // testes com outro overlay aberto: atalhos Ctrl+Shift+F5..F10
        else if (!wcscmp(argv[i], L"--grava-em"))
            app.gravaEm = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--quadros"))
            app.quadrosTeste = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--grava"))
            app.gravaArquivo = argv[++i];
        else if (!wcscmp(argv[i], L"--dump"))
            app.dumpPasta = argv[++i];
        else if (!wcscmp(argv[i], L"--runtime"))
            app.pastaRuntime = argv[++i];
        else if (!wcscmp(argv[i], L"--hdr-sintetico"))
            app.hdrSintetico = float(_wtof(argv[++i]));
        else if (!wcscmp(argv[i], L"--troca-rede"))
            app.trocaRedeNoQuadro = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--alterna"))
            app.alternaAltura = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--pula"))
        {
            app.temPulaTeste = true;
            for (wchar_t* t = argv[++i]; *t;)
            {
                wchar_t* f = nullptr;
                const unsigned long a = wcstoul(t, &f, 10), b = *f == L'-' ? wcstoul(f + 1, &f, 10) : a;
                for (unsigned long k = a; k && k <= b; ++k)
                    app.pulaTeste.push_back(uint32_t(k));
                t = *f ? f + 1 : f;
            }
        }
        else if (!wcscmp(argv[i], L"--imagem"))
            imagemArq = argv[++i];
        else if (!wcscmp(argv[i], L"--reproduz"))
            app.pastaReproducao = argv[++i];
        else if (!wcscmp(argv[i], L"--config"))
            app.configArq = argv[++i];
        else if (!wcscmp(argv[i], L"--atraso"))
            atrasoArg = _wtoi(argv[++i]);
    }
    for (int i = 1; i < argc; ++i)
    {
        if (!wcscmp(argv[i], L"--sem-painel"))
            app.comPainel = false;
        if (!wcscmp(argv[i], L"--sem-deslocamento"))
            app.semDeslocamento = true;
    }
    if (!imagemArq.empty() || !app.pastaReproducao.empty())
        app.comPainel = false;
    // imagem e reproducao: sincrono (deterministico) salvo --atraso; ao vivo a resposta entra quando fica pronta
    app.atraso = atrasoArg >= -1 ? atrasoArg : (!imagemArq.empty() || !app.pastaReproducao.empty()) ? 0 : -1;
    // tela de abertura ja (a rede leva de 2 a 30 s para carregar); fora dos modos de teste
    Abertura abertura;
    if (imagemArq.empty() && app.pastaReproducao.empty() && app.quadrosTeste == 0)
    {
        abertura.Mostra(GetModuleHandleW(nullptr));
        app.abertura = &abertura;
    }
    try
    {
        app.Inicia();
        app.abertura = nullptr;
        abertura.Fecha();
    }
    catch (const std::exception& e)
    {
        app.abertura = nullptr;
        abertura.Fecha();
        Log("erro", e.what());
        // modos de teste (imagem, reproducao, --quadros): so o log e o codigo de saida, nunca uma janela modal que
        // prende o processo e aparece na tela de quem esta usando o PC
        if (imagemArq.empty() && app.pastaReproducao.empty() && app.quadrosTeste == 0)
            MessageBoxA(nullptr, e.what(), "DLSS5 Overlay", MB_ICONERROR);
        return 1;
    }
    // reproducao: os quadros gravados pelo pipeline inteiro, gravando a saida de cada um (bin\gravacoes\<data_hora>)
    if (!app.pastaReproducao.empty())
    {
        int codigo = 0;
        try
        {
            if (!app.AbreReproducao(app.pastaReproducao))
                codigo = 2;
            else
            {
                const int n = app.quadrosTeste > 0 ? std::min(app.quadrosTeste, app.quadrosReproducao) : app.quadrosReproducao;
                for (int i = 0; i < n; ++i)
                {
                    app.CarregaQuadroReproducao(i);
                    if (i == 0)
                    {
                        app.Quadro(); // cria os recursos no tamanho gravado
                        app.IniciaGravacao();
                        app.gravando = n - 1;
                        continue;
                    }
                    app.Quadro();
                }
                app.gpu.Espera();
            }
        }
        catch (const std::exception& e)
        {
            Log("erro", e.what());
            codigo = 3;
        }
        app.Encerra();
        return codigo;
    }
    // modo imagem: N quadros da mesma foto pelo pipeline inteiro (o acumulo temporal assenta) e grava a saida
    if (!imagemArq.empty())
    {
        int codigo = 0;
        try
        {
            if (!app.CarregaImagem(imagemArq))
                codigo = 2;
            else
            {
                const int n = app.quadrosTeste > 0 ? app.quadrosTeste : 8;
                double soma = 0;
                int contados = 0;
                for (int i = 0; i < n; ++i)
                {
                    if (i == app.trocaRedeNoQuadro)
                        app.cfg.redePequena = !app.cfg.redePequena; // o Quadro() recria a rede
                    const int alturaBase = app.cfg.alturaRede;
                    if (app.alternaAltura && (i / 4) % 2)
                        app.cfg.alturaRede = app.alternaAltura;
                    const auto t = std::chrono::steady_clock::now();
                    app.Quadro();
                    app.gpu.Espera(); // tempo de GPU (rede inclusa), nao so o de envio
                    if (app.alternaAltura)
                    {
                        Log("imagem", Fmt("quadro %d altura %d: %.2f ms", i, app.cfg.alturaRede,
                                          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count()));
                        app.cfg.alturaRede = alturaBase;
                    }
                    if (i >= 2)
                        soma += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(),
                            ++contados;
                }
                Log("imagem", Fmt("tempo medio por quadro: %.2f ms (%d quadros, profundidade %d)",
                                  contados ? soma / contados : 0.0, contados, app.nivelAtual));
                if (!app.gravaArquivo.empty())
                    app.GravaSaida(app.gravaArquivo);
                if (!app.dumpPasta.empty())
                {
                    app.GravaTex(app.orig, app.dumpPasta + L"\\orig.f16", 8);
                    app.GravaTex(app.fed, app.dumpPasta + L"\\proxy.f16", 8);
                    if (app.ultimaEdicao)
                        app.GravaTex(*app.ultimaEdicao, app.dumpPasta + L"\\edicao.f16", 8);
                }
                Log("imagem", app.rede.Status());
            }
        }
        catch (const std::exception& e)
        {
            Log("erro", e.what());
            codigo = 3;
        }
        app.Encerra();
        return codigo;
    }
    // --teclas-teste (testes com outro overlay aberto): Ctrl+Shift+F5..F10 no lugar de D, N, S, Q, P, G
    const UINT mods = MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT;
    const UINT teclas[6] = {'D', 'N', 'S', 'Q', 'P', 'G'};
    for (int i = 0; i < 6; ++i)
        RegisterHotKey(nullptr, i + 1, mods, teclasTeste ? UINT(VK_F5 + i) : teclas[i]);
    if (!alvoTitulo.empty())
    {
        Busca b{alvoTitulo};
        EnumWindows(PorTitulo, reinterpret_cast<LPARAM>(&b));
        if (!b.achou)
            Log("teste", "janela nao encontrada: " + Utf8(alvoTitulo));
        app.Liga(b.achou);
    }
    Log("app", "pronto: Ctrl+Shift+D liga no jogo em primeiro plano");
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
                    if (app.Ligado())
                        app.Desliga();
                    else
                    {
                        app.Liga(GetForegroundWindow());
                        if (app.Ligado())
                            app.painel.Esconde(false);
                    }
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
                case 5:
                    app.painel.Alterna();
                    break;
                case 6:
                    app.IniciaGravacao();
                    break;
                }
            }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (!rodando)
            break;
        // Game Bar: com o jogo fora de foco (Alt+Tab, minimizado, outra janela na frente) overlay e painel somem e
        // nada e processado; o "sempre no topo" so e reafirmado com o jogo em foco (antes, a cada 250 ms, o overlay
        // voltava por cima de qualquer janela e parecia que o Alt+Tab e o minimizar nao funcionavam)
        const bool foco = !app.Ligado() || app.quadrosTeste > 0 || app.JogoEmFoco();
        app.painel.DefineAutoOculto(app.Ligado() && !foco);
        {
            static auto ultimaOrdem = clk::now();
            if (app.cap.Ativa() && foco && clk::now() - ultimaOrdem >= std::chrono::milliseconds(250))
            {
                ultimaOrdem = clk::now();
                app.ov.Ordena(app.painel.Janela());
            }
            static auto ultimoPainel = clk::now();
            if (clk::now() - ultimoPainel >= std::chrono::milliseconds(16))
            {
                ultimoPainel = clk::now();
                try
                {
                    app.Painel_();
                }
                catch (const std::exception& e)
                {
                    Log("erro", std::string("painel: ") + e.what());
                }
            }
        }
        if (!app.Ligado())
        {
            app.ov.Mostra(false);
            if (app.quadrosTeste > 0 && !alvoTitulo.empty())
                break; // teste sem alvo
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
            continue;
        }
        if (!foco)
        {
            app.Pausa();
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
            continue;
        }
        app.Retoma();
        app.SegueJanela();
        HANDLE ev = app.buffer ? app.fb.Evento() : app.cap.EventoQuadro();
        MsgWaitForMultipleObjects(ev ? 1 : 0, ev ? &ev : nullptr, FALSE, 16, QS_ALLINPUT);
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
                        app.GravaTex(app.saida, app.dumpPasta + L"\\saida_atual.rgba", app.hdr ? 8 : 4);
                        app.GravaTex(app.saidaAnt, app.dumpPasta + L"\\saida_anterior.rgba", app.hdr ? 8 : 4);
                        if (app.ultimoTemMov)
                            app.GravaTex(app.fluxo.Movimento(), app.dumpPasta + L"\\movimento.f16", 4);
                        if (app.ultimoTemMov)
                            app.GravaTex(app.fluxo.Confianca(), app.dumpPasta + L"\\confianca.f16", 2);
                        if (app.ultimaEdicao)
                        {
                            app.GravaTex(*app.ultimaEdicao, app.dumpPasta + L"\\edicao_estavel.f16", 8);
                            app.GravaTex(app.temporal.Crua(), app.dumpPasta + L"\\edicao_crua.f16", 8);
                        }
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
