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
#include "Painel.h"
#include "Rede.h"
#include "Shaders.h"
#include <shellapi.h>
#include <wincodec.h>
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
    Painel painel;
    bool comPainel = true;
    double fps = 0, msMedio = 0;
    std::chrono::steady_clock::time_point marcaFps = std::chrono::steady_clock::now();
    UINT64 quadrosMarca = 0;
    Config cfg;
    Passe pInteiro, pReduz, pCompoe;
    Tex inteiro, orig, fed, saida, fedAnt, saidaAnt; // orig: quadro no tamanho da rede; fed: o proxy dele
    std::wstring dumpPasta;
    std::wstring pastaRuntime; // --runtime: outra pasta do runtime (testes)
    std::vector<uint32_t> pulaTeste; // --pula 1,2,3: blocos pulados fixos (medicao de custo por bloco)
    bool temPulaTeste = false;
    int alternaAltura = 0; // --alterna 720: modo imagem troca a altura da rede a cada 4 quadros e registra cada quadro
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

    void Inicia()
    {
        pasta = PastaDoExe();
        LogAbre(pasta);
        cfg.Carrega(configArq.empty() ? pasta + L"\\config.json" : configArq);
        gpu.Cria();
        desc.Cria(gpu.dev.Get(), 512);
        for (int i = 0; i < 2; ++i)
        {
            Ck(gpu.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al[i])), "allocator");
            Ck(gpu.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al[i].Get(), nullptr, IID_PPV_ARGS(&cl[i])),
               "list");
            cl[i]->Close();
        }
        const std::string prepara = std::string(hlsl::Comum) + hlsl::Prepara;
        const std::string compoe = std::string(hlsl::Comum) + hlsl::Compoe;
        pInteiro.Cria(gpu.dev.Get(), prepara.c_str(), "CSInteiro", 1, 1, 12, "inteiro");
        pReduz.Cria(gpu.dev.Get(), prepara.c_str(), "CSReduz", 1, 2, 12, "reduz");
        pCompoe.Cria(gpu.dev.Get(), compoe.c_str(), "CSCompoe", 4, 1, 16, "compoe");
        fluxo.Cria(gpu.dev.Get());
        temporal.Cria(gpu.dev.Get());
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
        redeOk = rede.Inicia(gpu, pastaRuntime.empty() ? pasta + L"\\runtime" : pastaRuntime);
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
        rede.Encerra();
        cap.Para();
        painel.Destroi();
        ov.Destroi();
        if (configArq.empty())
            cfg.Salva(pasta + L"\\config.json");
    }

    void Liga(HWND h)
    {
        if (!h || h == ov.Janela() || h == painel.Janela())
            return;
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
        const DXGI_FORMAT fmtSaida = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
        if (inteiro.w == fw && inteiro.h == fh && fed.w == dw && fed.h == dh && saida.fmt == fmtSaida)
            return;
        gpu.Espera();
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
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = imgW;
        rd.Height = imgH;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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
            memcpy(m + fp.Offset + size_t(y) * fp.Footprint.RowPitch, px.data() + size_t(y) * imgW * 4, size_t(imgW) * 4);
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
        hdr = false;
        escalaSdr = 1.f;
        Log("imagem", Fmt("imagem %ux%u: %s", imgW, imgH, Utf8(arq).c_str()));
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
        if (!rede.TemProfundidade())
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
        Captura::Quadro q;
        if (imagem)
        {
            q.tex = imagem.Get();
            q.fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
            q.w = imgW;
            q.h = imgH;
            q.cercaValor = 0;
        }
        else if (!cap.Pega(q))
            return false;
        const auto t0 = std::chrono::steady_clock::now();
        RECT r{};
        if (!imagem && (IsIconic(cap.Alvo()) || !Captura::RetanguloCliente(cap.Alvo(), r)))
        {
            ov.Mostra(false);
            return false;
        }
        Garante(q.w, q.h);
        if (!imagem)
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
        UINT ponte = cfg.ponte < 0 ? (hdr ? 1u : 0u) : (UINT)std::clamp(cfg.ponte, 0, 3);
        if (ponte == 3 && hdr)
            ponte = 1; // a curva do jogo (sanduiche) so no SDR por enquanto
        pontePassada = ponte;
        UINT k[12] = {inteiro.w, inteiro.h, fed.w, fed.h, hdr ? 1u : 0u, 0, ponte, 0, 0, 0, 0, 0};
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
        // movimento simulado (fluxo optico contra o quadro anterior): alimenta o historico temporal da rede
        const bool temMov = cfg.movimento && fluxo.Calcula(c, desc, gpu.dev.Get(), fed.r.Get(), fed.fmt);
        if (temMov)
            fluxo.Estatisticas(c, desc, gpu.dev.Get(), fed, fedAnt);
        bool vezDaRede = !temporal.Valido() || !temMov || zeraHistorico;
        if (!vezDaRede)
        {
            if (true) // o dono: a rede nao desliga em movimento; o ritmo automatico (pular quadros) saiu
                vezDaRede = (quadros % (UINT64)std::clamp(cfg.ritmo, 1, 4)) == 0;
            else
                // automatico: a rede roda quando a previsao pelo movimento deixa de valer (oclusao/conteudo novo,
                // mudanca de luz, movimento muito rapido) ou depois de ritmoMax quadros
                vezDaRede = desdeRede + 1 >= (UINT)std::clamp(cfg.ritmoMax, 1, 8) || !estat.valida ||
                            estat.semConfianca > 0.06f || estat.mudancaLuz > 0.015f || estat.velocidade > 48.f;
        }
        const bool usaRede = redeOk && rede.Pronta() && cfg.modo != 1 && vezDaRede;
        if (usaRede)
            AjustaProfundidade(temMov);
        bool preparou = false;
        if (usaRede)
        {
            const ParamsRede pr = cfg.Rede(); // controles da rede a partir dos parametros NGX
            preparou = rede.Prepara(c, fed.r.Get(), fed.estado, fed.w, fed.h,
                                    temMov ? fluxo.Movimento().r.Get() : nullptr, fluxo.Movimento().estado,
                                    zeraHistorico || !temMov, pr);
        }
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
        ParamsTemporal tp = cfg.temporal;
        tp.intensidade = cfg.ngx.intensidade;
        Tex* edicao = ultimaEdicao = (cfg.modo != 1 && redeOk)
                          ? temporal.Roda(c, desc, gpu.dev.Get(), orig, fed, fedAnt, resp, fmtResp,
                                          temMov ? &fluxo.Movimento() : nullptr,
                                          (temMov && cfg.usaConfianca) ? &fluxo.Confianca() : nullptr, tp)
                          : nullptr;
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
        } kc{inteiro.w,      inteiro.h,      fed.w, fed.h, edicao ? cfg.forca : 0.f, (UINT)cfg.modo, cfg.divisao,
             edicao ? 1u : 0u, hdr ? 1u : 0u, escalaUsada, kExp, kCon, kSat, grada ? 1u : 0u,
             pontePassada == 3 ? 1u : 0u, 0};
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
        const bool apresenta = !imagem && bb && ov.Largura() == saida.w && ov.Altura() == saida.h;
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
        estat = fluxo.LeEstatisticas();
        if (preparou)
        {
            rede.Aposenta();
            zeraHistorico = false;
            desdeRede = 0;
            ++quadrosComRede;
        }
        else
            ++desdeRede;
        if ((quadros == 3 || quadros % 300 == 0) && preparou)
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
            Log("tempo", Fmt("quadros %llu: media %.2f ms, pior %.2f ms (captura -> rede -> overlay), rede em %.0f%% dos "
                             "quadros | confianca %.3f, luz %.4f, vel %.1f px",
                             (unsigned long long)quadros, somaMs / 60.0, maxMs,
                             100.0 * double(quadrosComRede - comRedeMarca) / 60.0, 1.0 - estat.semConfianca,
                             estat.mudancaLuz, estat.velocidade));
            fracaoRede = double(quadrosComRede - comRedeMarca) / 60.0;
            comRedeMarca = quadrosComRede;
            somaMs = maxMs = 0;
        }
        return true;
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
        if (cap.Ativa())
        {
            wchar_t t[256] = {};
            GetWindowTextW(cap.Alvo(), t, 256);
            e.alvo = t[0] ? t : L"(sem titulo)";
            e.janelaW = inteiro.w;
            e.janelaH = inteiro.h;
            e.redeW = fed.w;
            e.redeH = fed.h;
            e.msMedio = msMedio;
            e.fps = fps;
            e.fracaoRede = fracaoRede;
        }
        const AcoesPainel a = painel.Quadro(cfg, e);
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
    }

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
    std::wstring alvoTitulo, imagemArq;
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
        else if (!wcscmp(argv[i], L"--runtime"))
            app.pastaRuntime = argv[++i];
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
        else if (!wcscmp(argv[i], L"--config"))
            app.configArq = argv[++i];
    }
    for (int i = 1; i < argc; ++i)
    {
        if (!wcscmp(argv[i], L"--sem-painel"))
            app.comPainel = false;
    }
    if (!imagemArq.empty())
        app.comPainel = false;
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
    RegisterHotKey(nullptr, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'D');
    RegisterHotKey(nullptr, 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'N');
    RegisterHotKey(nullptr, 3, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'S');
    RegisterHotKey(nullptr, 4, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q');
    RegisterHotKey(nullptr, 5, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'P');
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
                case 5:
                    app.painel.Alterna();
                    break;
                }
            }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (!rodando)
            break;
        {
            static auto ultimaOrdem = clk::now();
            if (app.cap.Ativa() && clk::now() - ultimaOrdem >= std::chrono::milliseconds(250))
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
        if (!app.cap.Ativa())
        {
            app.ov.Mostra(false);
            if (app.quadrosTeste > 0 && !alvoTitulo.empty())
                break; // teste sem alvo
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
            continue;
        }
        HANDLE ev = app.cap.EventoQuadro();
        MsgWaitForMultipleObjects(1, &ev, FALSE, 16, QS_ALLINPUT);
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
