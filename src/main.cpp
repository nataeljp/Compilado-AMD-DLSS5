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
#include <psapi.h>
#include "Abertura.h"
#include "Captura.h"
#include "Config.h"
#include "FonteApi.h"
#include "RayRegen.h"
#include "FonteBuffer.h"
#include "Fluxo.h"
#include "Injecao.h"
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
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <mutex>
#include <thread>

using namespace ov;

namespace {

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
// espera curta com precisao de fracao de milissegundo. O Sleep (e o sleep_for) acorda no passo do timer do processo,
// 15,6 ms por padrao desde o Windows 10 2004 (o timeBeginPeriod de outro programa nao vale mais aqui): o teto de fps
// chegava a segurar o quadro ate ~15 ms a mais que o pedido.
void EsperaPrecisa(std::chrono::steady_clock::duration d)
{
    if (d <= std::chrono::steady_clock::duration::zero())
        return;
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER prazo{};
    prazo.QuadPart = -std::max<LONGLONG>(1, LONGLONG(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count() / 100));
    if (timer && SetWaitableTimer(timer, &prazo, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(timer, INFINITE);
    else
        std::this_thread::sleep_for(d); // sem o timer de alta resolucao (anterior ao Windows 10 1803)
}

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
    ParamsRede ParamsDaRede() const
    {
        ParamsRede p = cfg.Rede();
        p.passes = sessao.passes;
        p.redeCompleta = sessao.redeCompleta;
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
    Passe pInteiro, pReduz, pCompoe, pExporta, pGuias;
    // ---- captura de buffer (Modo de Rede): a DLSS5Buffer.dll no jogo aberto entrega cor sem HUD, movimento e
    // profundidade do upscaler dele; a edicao volta para o jogo, que a soma no quadro seguinte ----
    FonteBuffer fb;
    bool buffer = false;        // ligado pela captura de buffer (sem WGC e sem a janela do overlay)
    bool redeLenta = false;     // a rede passou do prazo do jogo: ele so espera se o painel pedir
    HWND alvoBuffer = nullptr;  // o jogo, nas duas capturas com a DLL
    FonteApi fa;
    bool captApi = false;       // ligado pela captura da API (DLL no Present; o resto como a captura de video)
    uint64_t quadroApi = 0;     // o quadro em uso, ate a GPU terminar de le-lo
    ULONGLONG apiDesde = 0;
    std::string estadoBuffer;   // linha de estado no painel
    std::string avisoPastaJogo; // o que saiu da pasta do jogo (a DLL antiga da geracao de quadros)
    DWORD pidBuffer = 0;                       // processo do jogo conectado pela captura de buffer
    DWORD pidRecusado = 0;                     // desligado pelo usuario: nao reconecta sozinho nesse processo
    std::wstring exeJogo;                      // caminho completo do executavel do jogo conectado
    std::wstring Dll() const { return pasta + L"\\captura\\DLSS5Buffer.dll"; }
    int fonteLigada = 0;        // a fonte com que ligou (trocar no painel religa)
    uint64_t quadroJogo = 0;    // numero do quadro do jogo em processamento (gravacao: alinha com o jogo)
    int gravaEm = 0;            // --grava-em N: comeca a gravar no quadro N (testes)
    bool invertidaUsada = true; // Z invertido em uso (do jogo, ou medido quando ele nao diz)
    bool Ligado() const { return cap.Ativa() || buffer || captApi; }
    HWND Alvo() const { return buffer || captApi ? alvoBuffer : cap.Alvo(); }
    bool fedAntValido = false; // fedAnt tem o quadro anterior (sem copia, nao tem)
    Tex inteiro, orig, fed, saida, fedAnt, saidaAnt; // orig: quadro no tamanho da rede; fed: o proxy dele
    // ---- denoise: CAS na imagem final e visao de depuracao, para ajustar o filtro vendo o que ele faz ----
    Passe pCas, pDnVista;
    RayRegen rayRegen;
    float fovYJogo = 0; // o que o jogo passa ao FSR (captura de buffer)
    Tex saidaCas;
    Tex* dnBruta = nullptr; // a edicao deste quadro antes do denoise
    void CasNaSaida(ID3D12GraphicsCommandList* c)
    {
        const D3D12_RESOURCE_STATES SRV =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if (saidaCas.w != saida.w || saidaCas.h != saida.h || saidaCas.fmt != saida.fmt)
            saidaCas.Cria(gpu.dev.Get(), saida.w, saida.h, saida.fmt, L"CAS do denoise");
        saida.Para(c, SRV);
        saidaCas.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        struct
        {
            UINT w, h, hdr;
            float nitidez;
        } k{saida.w, saida.h, hdr ? 1u : 0u, std::clamp(cfg.dnCas, 0.f, 1.f)};
        pCas.Roda(c, desc, gpu.dev.Get(), {{saida.r.Get(), saida.fmt}}, {{saidaCas.r.Get(), saidaCas.fmt}}, &k, (saida.w + 7) / 8, (saida.h + 7) / 8);
        // o resultado vira a saida: as duas tem o mesmo tamanho, formato e flags (Cria), e quem le a saida pega o recurso a cada quadro
        std::swap(saida.r, saidaCas.r);
        std::swap(saida.estado, saidaCas.estado);
    }
    // a visao de depuracao do SDK no lugar da imagem
    void VistaDenoise(ID3D12GraphicsCommandList* c)
    {
        const D3D12_RESOURCE_STATES SRV =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        Tex& d = rayRegen.depura;
        if (!d.r || d.w != saida.w || d.h != saida.h)
            return;
        if (saidaCas.w != saida.w || saidaCas.h != saida.h || saidaCas.fmt != saida.fmt)
            saidaCas.Cria(gpu.dev.Get(), saida.w, saida.h, saida.fmt, L"CAS do denoise");
        d.Para(c, SRV);
        saidaCas.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        struct
        {
            UINT w, h, hdr;
            float escala;
        } k{saida.w, saida.h, hdr ? 1u : 0u, escalaUsada};
        pDnVista.Roda(c, desc, gpu.dev.Get(), {{d.r.Get(), d.fmt}}, {{saidaCas.r.Get(), saidaCas.fmt}}, &k, (saida.w + 7) / 8, (saida.h + 7) / 8);
        // o resultado vira a saida: as duas tem o mesmo tamanho, formato e flags (Cria), e quem le a saida pega o recurso a cada quadro
        std::swap(saida.r, saidaCas.r);
        std::swap(saida.estado, saidaCas.estado);
    }
    // ---- a GPU para a rede: o teto de FPS do jogo. A prioridade nao adianta: D3DKMTSetProcessSchedulingPriorityClass (alta e tempo
    // real, aceitas) e hipStreamCreateWithPriority deram os mesmos 10,3 fps de rede com a GPU cheia; o teto em 30 deu 17,4 ----
    LimiteJogo limiteJogo;
    void AplicaTetoDoJogo()
    {
        DWORD pid = 0;
        if (buffer)
            pid = pidBuffer;
        else if (captApi)
            GetWindowThreadProcessId(alvoBuffer, &pid);
        limiteJogo.Define(pid, UINT(cfg.limiteFpsJogo));
    }
    std::wstring dumpPasta;
    std::wstring pastaRuntime; // --runtime: outra pasta do runtime (testes)
    std::vector<uint32_t> pulaTeste; // --pula 1,2,3: blocos pulados fixos (medicao de custo por bloco)
    bool temPulaTeste = false;
    int alternaAltura = 0; // --alterna 720: modo imagem troca a altura da rede a cada 4 quadros e registra cada quadro
    int trocaRedeNoQuadro = -1; // --troca-rede N: modo imagem troca grande <-> pequena no quadro N (teste da troca ao vivo)
    bool redePequenaAtiva = false;
    bool vitAdaptativo = false; // reuso adaptativo do ViT ligado no ambiente do runtime (so muda ao reabrir o programa)
    int temporalSessao = 0;     // DLSS5_TEMPORAL_MODE passado ao runtime ao abrir (so muda ao reabrir o programa)
    // historico da propria rede com as guias (modo 1 e um runtime com LmxxfNrSetHistoryGuides)
    bool HistoricoRede() const { return temporalSessao == 1 && rede.TemGuias(); }
    // quem usa o movimento: o historico da propria rede, a exibicao "Movimento", a gravacao, o dump e a profundidade
    // automatica
    bool PrecisaMovimento() const
    {
        return HistoricoRede() || cfg.modo == 4 || gravando > 0 || gravaEm > 0 || !dumpPasta.empty() ||
               cfg.profundidadeAuto || cfg.dnAtivo;
    }
    std::wstring PastaRede() const { return pastaRuntime.empty() ? pasta + L"\\runtime" : pastaRuntime; }
    // A rede da sessao: tamanho, rede grande ou pequena, passes e rede completa valem do Liga ate o proximo Liga; mudar
    // no painel vale ao religar. Durante o jogo nada disso muda: cada tamanho novo de rede prende buffers que o driver
    // HIP da AMD nunca devolve (hip_d3d12_bridge.h do lmxxf, 44 bytes por pixel da rede, ate o overlay fechar), e
    // trocar de rede recarrega os pesos com a GPU parada (travada de segundos).
    struct Sessao
    {
        int alturaRede = 1080;
        bool redePequena = false;
        int passes = 1;
        bool redeCompleta = true;
    } sessao;
    // com o historico da propria rede os passes tambem valem: no caminho do runtime em uso (LmxxfNrSetPasses -> a rede HIP)
    // so o ultimo passe recebe o historico e da a decisao dele (hip_reference_network.h, MultiPassRest); o limite de 1
    // passe e do consumidor experimental (native_fast_history_policy.h), que este runtime nao usa
    Sessao SessaoDoConfig() const
    {
        return {cfg.alturaRede, cfg.redePequena, cfg.rede.passes, cfg.rede.redeCompleta};
    }
    // ao ligar: os valores do painel (e do perfil do jogo) passam a valer; a rede e recriada so se a escolha grande /
    // pequena mudou ou se ela tinha sido desligada (emergencia de memoria)
    void IniciaSessao()
    {
        sessao = SessaoDoConfig();
        if (!redeOk || sessao.redePequena != redePequenaAtiva)
            TrocaRede();
        emergenciaMemoria.clear();
        Log("rede", Fmt("sessao: rede %s, altura %d, passes %d, rede completa %d", sessao.redePequena ? "pequena" : "grande",
                        sessao.alturaRede, sessao.passes, sessao.redeCompleta ? 1 : 0));
    }
    // troca grande <-> pequena (so ao ligar): a rede e recriada (o historico recomeca)
    void TrocaRede()
    {
        gpu.Espera();
        rede.Encerra();
        redeOk = rede.Inicia(gpu, PastaRede(), sessao.redePequena);
        redeSolta = false;
        redePequenaAtiva = sessao.redePequena;
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
        MigraPerfisAntigos(); // perfis\<exe>\<nome>.json (formato anterior) -> perfis\<nome>.json
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
        pInteiro.Cria(gpu.dev.Get(), prepara.c_str(), "CSInteiro", 1, 2, 16, "inteiro");
        pReduz.Cria(gpu.dev.Get(), prepara.c_str(), "CSReduz", 1, 2, 16, "reduz");
        pCompoe.Cria(gpu.dev.Get(), compoe.c_str(), "CSCompoe", 7, 1, 40, "compoe");
        pExporta.Cria(gpu.dev.Get(), (std::string(hlsl::Comum) + hlsl::Exporta).c_str(), "CSExporta", 3, 2, 12,
                      "edicao para o jogo");
        pGuias.Cria(gpu.dev.Get(), hlsl::Guias, "CSGuias", 2, 2, 4, "guias do historico da rede");
        rayRegen.Cria(gpu.dev.Get());
        // AMD FidelityFX CAS (MIT): nitidez que recua onde o contraste ja e alto. Em HDR, no espaco comprimido: a conta supoe 0..1
        pCas.Cria(gpu.dev.Get(), R"(
cbuffer C : register(b0) { uint w; uint h; uint hdr; float nitidez; };
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
float3 L(int2 p)
{
    const float3 c = max(src.Load(int3(clamp(p, int2(0, 0), int2(w, h) - 1), 0)).rgb, 0.0);
    return hdr ? c / (1.0 + c) : saturate(c);
}
[numthreads(8, 8, 1)] void CSCas(int2 p : SV_DispatchThreadID)
{
    if (p.x >= int(w) || p.y >= int(h)) return;
    const float3 a = L(p + int2(-1, -1)), b = L(p + int2(0, -1)), c = L(p + int2(1, -1));
    const float3 d = L(p + int2(-1, 0)), e = L(p), f = L(p + int2(1, 0));
    const float3 g = L(p + int2(-1, 1)), h2 = L(p + int2(0, 1)), i = L(p + int2(1, 1));
    float3 mn = min(min(min(d, e), min(f, b)), h2);
    const float3 mn2 = min(mn, min(min(a, c), min(g, i)));
    mn += mn2;
    float3 mx = max(max(max(d, e), max(f, b)), h2);
    const float3 mx2 = max(mx, max(max(a, c), max(g, i)));
    mx += mx2;
    const float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-5)));
    const float3 w8 = amp * (-1.0 / lerp(8.0, 5.0, nitidez));
    float3 r = saturate((b * w8 + d * w8 + f * w8 + h2 * w8 + e) / (1.0 + 4.0 * w8));
    if (hdr)
        r = r / max(1.0 - r, 1e-4);
    dst[p] = float4(r, src.Load(int3(p, 0)).a);
}
)",
                  "CSCas", 1, 1, 4, "CAS do denoise");
        pDnVista.Cria(gpu.dev.Get(), R"(
cbuffer C : register(b0) { uint w; uint h; uint hdr; float escala; };
Texture2D<float4> depura : register(t0);
RWTexture2D<float4> saida : register(u0);
[numthreads(8, 8, 1)] void CSVista(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    const float4 v = depura.Load(int3(p, 0));
    const float3 c = v.a > 0.0 ? saturate(v.rgb) : float3(0.0, 0.0, 0.0);
    saida[p] = float4(hdr ? pow(c, 2.2) * escala : c, 1.0);
}
)",
                      "CSVista", 1, 1, 4, "visao de depuracao do denoise");
        fluxo.Cria(gpu.dev.Get());
        temporal.Cria(gpu.dev.Get());
        EsperaPasses();
        if (cfg.temporalRede > 0)
        {
            // historico temporal da propria rede (o runtime le o ambiente que copiou ao carregar: tem de vir antes)
            temporalSessao = std::clamp(cfg.temporalRede, 0, 3);
            SetEnvironmentVariableW(L"DLSS5_TEMPORAL_MODE", std::to_wstring(temporalSessao).c_str());
            SetEnvironmentVariableW(L"DLSS5_TEMPORAL_ENHANCE_STRENGTH",
                                    std::to_wstring(std::clamp(cfg.realceTemporal, 0.f, 2.f)).c_str());
        }
        // o reuso adaptativo do ViT nao combina com o historico da rede: fica de fora nesta sessao sem mexer no ajuste
        // (antes o cfg.reusoVit era zerado aqui e ia assim para o config.json ao sair)
        if (cfg.reusoVit && temporalSessao == 0)
        {
            // reuso adaptativo do ViT (lmxxf): o runtime le uma vez, ao criar a rede
            SetEnvironmentVariableW(L"DLSS5_VIT_ADAPTIVE", L"1");
            vitAdaptativo = true;
            SetEnvironmentVariableW(L"DLSS5_VIT_REUSE_PERIOD", L"4");
            SetEnvironmentVariableW(L"DLSS5_HIP_GRAPH", L"0");
            SetEnvironmentVariableW(L"DLSS5_HIP_VIT_BYTE_STREAM", L"0");
        }
        textoSalvo = cfg.Texto(); // os ajustes de config.json: o ponto de partida do "alterado"
        marca("shaders prontos");
        Etapa(cfg.redePequena ? L"Carregando a rede DLSS 5 (pequena)..." : L"Carregando a rede DLSS 5...");
        redeOk = rede.Inicia(gpu, PastaRede(), cfg.redePequena);
        marca(redeOk ? "rede carregada" : "rede falhou");
        Etapa(L"Preparando a captura e o painel...");
        redePequenaAtiva = cfg.redePequena;
        sessao = SessaoDoConfig();
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
        fa.Fecha();
        AplicaCamada(false);
        painel.Destroi();
        ov.Destroi();
        GravaDestino();
    }

    // ---- perfis (como o ReShade): bin\perfis\<nome>.json, um arquivo por perfil, todos na mesma pasta. Cada jogo tem o
    // perfil dele -- o nome do exe, ou o nome que o usuario der (bin\perfis\_jogos.ini, exe=nome) --, carregado ao ligar
    // e para onde vao os ajustes ao salvar, ao sair do programa e ao trocar de jogo. Qualquer perfil pode ser carregado em
    // qualquer jogo (os ajustes do TLOU2 no Gears, por exemplo): carregar so traz os ajustes; o destino continua sendo o
    // perfil do jogo ligado. Sem jogo, o destino e config.json, o padrao do programa (de onde parte um jogo sem perfil). ----
    std::wstring exeAlvo;          // processo do jogo capturado ("" = nenhum)
    std::wstring exePerfis;        // o exe dos ajustes em uso (o ultimo para o qual o perfil foi aplicado)
    std::wstring perfilJogo;       // o perfil do jogo ligado: o destino dos ajustes ("" = config.json)
    std::wstring perfilBase;       // de onde vieram os ajustes em uso ("" = config.json)
    std::string textoSalvo;        // o que esta gravado no destino (cfg.Texto() dele): diferente = alterado
    std::vector<std::wstring> perfisCache; // a lista, lida do disco so depois de uma mudanca (o painel a pede todo quadro)
    bool perfisSujos = true;
    std::map<std::wstring, std::wstring> vinculos; // exe (minusculas) -> nome do perfil dele
    bool vinculosLidos = false;
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
    static std::wstring SemExtensao(std::wstring n)
    {
        const auto pt = n.find_last_of(L'.');
        if (pt != std::wstring::npos)
            n.resize(pt);
        return n;
    }
    static std::wstring Minusculas(std::wstring s)
    {
        for (auto& c : s)
            c = towlower(c);
        return s;
    }
    std::wstring PastaPerfis() const { return pasta + L"\\perfis"; }
    std::wstring PerfilArq(const std::wstring& nome) const { return PastaPerfis() + L"\\" + nome + L".json"; }
    std::wstring ConfigArq() const { return pasta + L"\\config.json"; }
    std::wstring ArqDestino() const { return perfilJogo.empty() ? ConfigArq() : PerfilArq(perfilJogo); }
    static bool Existe(const std::wstring& arq) { return GetFileAttributesW(arq.c_str()) != INVALID_FILE_ATTRIBUTES; }
    // nome de arquivo valido: sem \ / : * ? " < > |, sem espaco ou ponto nas pontas (o Windows os corta), ate 60
    // caracteres, e sem os nomes de dispositivo do Windows (CON, NUL, COM1...: o arquivo nao seria criado, mas o
    // dispositivo "existe"); "" se nao sobrar nada. Nome comecado por "_" e dos arquivos do programa (_jogos.ini).
    static std::wstring NomeValido(const std::wstring& n)
    {
        std::wstring r;
        for (wchar_t c : n)
            r += (c < 32 || wcschr(L"\\/:*?\"<>|", c)) ? L'_' : c;
        while (!r.empty() && (r.back() == L' ' || r.back() == L'.'))
            r.pop_back();
        while (!r.empty() && (r.front() == L' ' || r.front() == L'.' || r.front() == L'_'))
            r.erase(r.begin());
        if (r.size() > 60)
            r.resize(60);
        std::wstring m = r;
        for (auto& c : m)
            c = towupper(c);
        static const wchar_t* const reservados[] = {L"CON", L"PRN", L"AUX", L"NUL"};
        bool reservado = false;
        for (const wchar_t* x : reservados)
            reservado |= m == x;
        if (m.size() == 4 && (m.compare(0, 3, L"COM") == 0 || m.compare(0, 3, L"LPT") == 0) && m[3] >= L'0' && m[3] <= L'9')
            reservado = true;
        return reservado ? r + L"_" : r;
    }
    // o texto de um arquivo (UTF-8, sem o BOM do Bloco de Notas); "" sem o arquivo
    static std::string LeTexto(const std::wstring& arq)
    {
        std::string s;
        if (FILE* f = _wfopen(arq.c_str(), L"rb"))
        {
            char b[4096];
            size_t n;
            while ((n = fread(b, 1, sizeof b, f)) > 0)
                s.append(b, n);
            fclose(f);
        }
        if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
            s.erase(0, 3);
        return s;
    }
    static std::string Apara(std::string s)
    {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
            s.pop_back();
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            ++i;
        return s.substr(i);
    }
    // bin\perfis\_jogos.ini: uma linha exe=perfil por jogo (o arquivo pode ser editado a mao, como o ReShade.ini)
    void LeVinculos()
    {
        if (vinculosLidos)
            return;
        vinculosLidos = true;
        vinculos.clear();
        const std::string t = LeTexto(PastaPerfis() + L"\\_jogos.ini");
        size_t i = 0;
        while (i < t.size())
        {
            size_t f = t.find('\n', i);
            if (f == std::string::npos)
                f = t.size();
            const std::string l = Apara(t.substr(i, f - i));
            i = f + 1;
            const size_t ig = l.find('=');
            if (l.empty() || l[0] == ';' || l[0] == '#' || l[0] == '[' || ig == std::string::npos)
                continue;
            const std::wstring exe = Minusculas(Largo(Apara(l.substr(0, ig)))), nome = NomeValido(Largo(Apara(l.substr(ig + 1))));
            if (!exe.empty() && !nome.empty())
                vinculos[exe] = nome;
        }
    }
    void GravaVinculos()
    {
        CreateDirectoryW(PastaPerfis().c_str(), nullptr);
        std::string t = "; DLSS5 Overlay: o perfil de cada jogo (exe=nome), em perfis\\<nome>.json\n[jogos]\n";
        for (const auto& v : vinculos)
            t += Utf8(v.first) + "=" + Utf8(v.second) + "\n";
        if (FILE* f = _wfopen((PastaPerfis() + L"\\_jogos.ini").c_str(), L"wb"))
        {
            fwrite(t.data(), 1, t.size(), f);
            fclose(f);
        }
    }
    // o perfil de um exe: o nome dado a ele em _jogos.ini, ou o nome do exe
    std::wstring NomePerfilDoJogo(const std::wstring& exe)
    {
        LeVinculos();
        const auto it = vinculos.find(Minusculas(exe));
        return it != vinculos.end() ? it->second : NomeValido(SemExtensao(exe));
    }
    const std::vector<std::wstring>& Perfis()
    {
        if (!perfisSujos)
            return perfisCache;
        perfisSujos = false;
        perfisCache.clear();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((PastaPerfis() + L"\\*.json").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'_')
                    perfisCache.push_back(SemExtensao(fd.cFileName));
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(perfisCache.begin(), perfisCache.end(),
                  [](const std::wstring& x, const std::wstring& y) { return Minusculas(x) < Minusculas(y); });
        return perfisCache;
    }
    // o formato anterior (perfis\<exe>\<nome>.json, com _ao_ligar.txt e _nome.txt): o perfil "ao ligar" (ou o unico)
    // vira o perfil do jogo, com o nome que o jogo tinha; os outros ficam como "<exe> - <nome>". A pasta velha (com os
    // .txt) vai para perfis\_antigos. Roda ao abrir o programa.
    void MigraPerfisAntigos()
    {
        if (!configArq.empty())
            return;
        std::vector<std::wstring> pastas;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((PastaPerfis() + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
            return;
        do
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' && fd.cFileName[0] != L'_')
                pastas.push_back(fd.cFileName);
        while (FindNextFileW(h, &fd));
        FindClose(h);
        if (pastas.empty())
            return;
        LeVinculos();
        auto linha = [](const std::string& t) { return Apara(t.substr(0, t.find_first_of("\r\n"))); };
        for (const std::wstring& p : pastas)
        {
            const std::wstring dir = PastaPerfis() + L"\\" + p;
            const std::wstring exeChave = Minusculas(p + L".exe");
            const std::wstring nomeJogo = NomeValido(Largo(linha(LeTexto(dir + L"\\_nome.txt"))));
            const std::wstring aoLigar = NomeValido(Largo(linha(LeTexto(dir + L"\\_ao_ligar.txt"))));
            std::vector<std::wstring> jsons;
            HANDLE hj = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
            if (hj != INVALID_HANDLE_VALUE)
            {
                do
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                        jsons.push_back(SemExtensao(fd.cFileName));
                while (FindNextFileW(hj, &fd));
                FindClose(hj);
            }
            std::wstring principal = std::find(jsons.begin(), jsons.end(), aoLigar) != jsons.end() ? aoLigar : std::wstring();
            if (principal.empty() && jsons.size() == 1)
                principal = jsons[0];
            for (const std::wstring& j : jsons)
            {
                // o principal fica com o nome que o usuario deu a ele; so os nomes automaticos de antes ("Principal",
                // "Perfil N") viram o nome do jogo (ou o do exe)
                const bool automatico = j == L"Principal" || (j.rfind(L"Perfil ", 0) == 0 && j.size() <= 10);
                const std::wstring base =
                    j == principal ? (automatico ? (nomeJogo.empty() ? p : nomeJogo) : j) : p + L" - " + j;
                std::wstring alvo = NomeValido(base);
                if (alvo.empty())
                    alvo = NomeValido(p);
                for (int k = 2; Existe(PerfilArq(alvo)); ++k)
                    alvo = NomeValido(base + L" (" + std::to_wstring(k) + L")");
                if (!MoveFileW((dir + L"\\" + j + L".json").c_str(), PerfilArq(alvo).c_str()))
                {
                    Log("perfil", Fmt("migracao: nao moveu (%lu) ", GetLastError()) + Utf8(dir + L"\\" + j + L".json"));
                    continue;
                }
                if (j == principal && !vinculos.count(exeChave))
                    vinculos[exeChave] = alvo;
                Log("perfil", "migrado: " + Utf8(p + L"\\" + j) + " -> " + Utf8(alvo) + ".json");
            }
            // so o nome do jogo (sem perfil salvo): o primeiro salvar ja usa esse nome
            if (principal.empty() && jsons.empty() && !nomeJogo.empty() && !vinculos.count(exeChave))
                vinculos[exeChave] = nomeJogo;
            CreateDirectoryW((PastaPerfis() + L"\\_antigos").c_str(), nullptr);
            std::wstring destino = PastaPerfis() + L"\\_antigos\\" + p;
            for (int k = 2; Existe(destino); ++k)
                destino = PastaPerfis() + L"\\_antigos\\" + p + L" (" + std::to_wstring(k) + L")";
            if (!MoveFileW(dir.c_str(), destino.c_str()))
                Log("perfil", Fmt("migracao: pasta antiga nao saiu (%lu) ", GetLastError()) + Utf8(dir));
        }
        GravaVinculos();
        perfisSujos = true;
    }
    void AplicaConfig(const Config& c)
    {
        cfg = c;
        zeraHistorico = true;
        temporal.Reinicia();
    }
    // o cfg.Texto() do que esta gravado num arquivo ("" sem o arquivo: tudo conta como alterado ate salvar)
    static std::string TextoDe(const std::wstring& arq)
    {
        if (!Existe(arq))
            return std::string();
        Config c;
        c.Carrega(arq);
        return c.Texto();
    }
    // traz os ajustes de um perfil (de qualquer jogo; "" = config.json). O destino nao muda: salvar continua indo para o
    // perfil do jogo ligado
    bool CarregaDe(const std::wstring& nome)
    {
        const std::wstring arq = nome.empty() ? ConfigArq() : PerfilArq(nome);
        if (!nome.empty() && !Existe(arq))
            return false;
        Config c;
        c.Carrega(arq);
        AplicaConfig(c);
        perfilBase = nome;
        textoSalvo = TextoDe(ArqDestino());
        Log("perfil", "carregado: " + Utf8(arq) + (arq == ArqDestino() ? "" : " (salvar vai para " + Utf8(ArqDestino()) + ")"));
        return true;
    }
    void SalvaPerfil()
    {
        if (!configArq.empty()) // --config (testes): nao e regravado
            return;
        CreateDirectoryW(PastaPerfis().c_str(), nullptr);
        cfg.Salva(ArqDestino());
        perfilBase = perfilJogo;
        textoSalvo = cfg.Texto();
        perfisSujos = true;
        Log("perfil", "salvo: " + Utf8(ArqDestino()));
    }
    // o perfil do jogo ligado ganha outro nome (o arquivo e o vinculo do exe)
    void RenomeiaPerfilJogo(const std::wstring& novo)
    {
        if (exeAlvo.empty() || novo.empty() || novo == perfilJogo)
            return;
        if (Existe(PerfilArq(novo)))
        {
            Log("perfil", "renomear: ja existe " + Utf8(PerfilArq(novo)));
            return;
        }
        if (Existe(PerfilArq(perfilJogo)) && !MoveFileW(PerfilArq(perfilJogo).c_str(), PerfilArq(novo).c_str()))
        {
            Log("perfil", Fmt("renomear falhou (%lu): ", GetLastError()) + Utf8(PerfilArq(perfilJogo)));
            return;
        }
        LeVinculos();
        for (auto& v : vinculos) // outro jogo que usava o mesmo arquivo segue com ele
            if (v.second == perfilJogo)
                v.second = novo;
        vinculos[Minusculas(exeAlvo)] = novo;
        GravaVinculos();
        if (perfilBase == perfilJogo)
            perfilBase = novo;
        Log("perfil", "renomeado: " + Utf8(perfilJogo) + " -> " + Utf8(novo));
        perfilJogo = novo;
        perfisSujos = true;
    }
    // vai para a Lixeira (da para recuperar). Os ajustes em uso ficam na tela; se era o perfil do jogo, o proximo salvar
    // (ou a saida) o cria de novo
    void ApagaPerfil(const std::wstring& nome)
    {
        if (nome.empty() || !Existe(PerfilArq(nome)))
            return;
        std::wstring dupla = PerfilArq(nome);
        dupla.push_back(L'\0'); // SHFileOperation quer a lista terminada em dois nulos
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = dupla.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        if (SHFileOperationW(&op) != 0 || Existe(PerfilArq(nome)))
        {
            Log("perfil", "apagar falhou: " + Utf8(PerfilArq(nome)));
            return;
        }
        if (nome == perfilJogo)
            textoSalvo.clear();
        if (nome == perfilBase)
            perfilBase = perfilJogo == nome ? std::wstring() : perfilBase;
        perfisSujos = true;
        Log("perfil", "apagado (Lixeira): " + Utf8(PerfilArq(nome)));
    }
    // ao sair do programa ou trocar de jogo: os ajustes vao para o perfil do jogo (ou para config.json sem jogo)
    void GravaDestino()
    {
        if (!configArq.empty()) // --config (testes): nao e regravado
            return;
        CreateDirectoryW(PastaPerfis().c_str(), nullptr);
        cfg.Salva(ArqDestino());
        textoSalvo = cfg.Texto();
        perfisSujos = true;
    }
    // ao ligar: num exe diferente do anterior, os ajustes do anterior vao para o perfil dele e o perfil deste jogo e
    // carregado; jogo sem perfil ainda parte do padrao do programa (config.json) e o perfil dele nasce no primeiro salvar.
    // Religar o mesmo jogo mantem os ajustes da tela (inclusive os que so valem ao religar).
    void CarregaPerfil(HWND h)
    {
        const std::wstring exe = NomeExe(h);
        if (!configArq.empty()) // testes com --config: o config do teste manda
        {
            exeAlvo = exe;
            perfisSujos = true;
            return;
        }
        if (exe != exePerfis)
            GravaDestino(); // os ajustes de antes (do jogo anterior, ou o padrao sem jogo) ficam no destino deles
        exeAlvo = exe;
        perfisSujos = true;
        if (exeAlvo == exePerfis)
            return;
        exePerfis = exeAlvo;
        perfilJogo = NomePerfilDoJogo(exeAlvo);
        // a captura escolhida no painel vale: a do perfil trocava "buffer" por "API" ao ligar, sem avisar
        const int fonteEscolhida = cfg.fonte;
        const bool temPerfil = CarregaDe(perfilJogo);
        if (!temPerfil)
            CarregaDe(L"");
        cfg.fonte = fonteEscolhida;
        if (temPerfil)
            return;
        Log("perfil", Utf8(exeAlvo) + " ainda sem perfil: padrao do programa, salvo em " + Utf8(PerfilArq(perfilJogo)));
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
        fb.SemEspera(); // sem quadros processados o jogo nao espera a edicao
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
        if (imagem || buffer || !inteiro.w || !Captura::RetanguloCliente(Alvo(), r))
            return;
        if (captApi)
        {
            ov.Posiciona(r, hdr, inteiro.w, inteiro.h); // o back buffer nao precisa ter o tamanho da janela
            return;
        }
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
        // janela que ja fechou: GetWindowThreadProcessId deixa o pid como estava (0) e OpenProcess(0) e o erro 87
        if (!IsWindow(h))
        {
            estadoBuffer = "nao conectou: a janela escolhida nao existe mais (o jogo fechou). Escolha de novo ou use Ctrl+Shift+D no jogo";
            Log("buffer", estadoBuffer);
            return;
        }
        fonteLigada = cfg.fonte;
        if (cfg.fonte == 1)
        {
            LigaBuffer(h);
            return;
        }
        if (cfg.fonte == 2)
        {
            LigaApi(h);
            return;
        }
        CarregaPerfil(h);
        IniciaSessao();
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
        // o que o usuario desligou fica desligado: nem o modulo dentro dele nem o exe escolhido religam sozinhos
        if (HWND h = Alvo())
        {
            DWORD p = 0;
            GetWindowThreadProcessId(h, &p);
            if (p)
                pidRecusado = p;
        }
        cap.Para();
        if (buffer)
        {
            pidRecusado = pidBuffer; // ProcuraJogo nao religa sozinho o que foi desligado
            gpu.Espera();
            FechaFonte(); // sem batimento o modulo do jogo para de exportar e de somar
            buffer = false;
            alvoBuffer = nullptr;
            fluxo.Reinicia();
            temporal.Reinicia();
            zeraHistorico = true;
        }
        if (captApi)
        {
            gpu.Espera();
            fa.Fecha(); // sem batimento o modulo para de copiar e, em 5 s, solta a VRAM
            captApi = false;
            quadroApi = 0;
            alvoBuffer = nullptr;
            estadoBuffer.clear();
            fluxo.Reinicia();
            temporal.Reinicia();
            zeraHistorico = true;
        }
        ov.Mostra(false);
        // desligado devolve a VRAM ao jogo (2026-11-06, Gears: 50 fps ligado, 25 depois de ligar/desligar algumas vezes com o
        // overlay desligado, 77 so ao fechar o processo): a sessao da rede (o HIP segura 2-3 GB ate o Destroy), os quadros no
        // tamanho da rede e o contexto do Ray Regeneration ficavam alocados. Ligar recria tudo: IniciaSessao sobe a rede (redeOk
        // falso), Garante refaz os quadros e o Ray Regeneration reabre no primeiro filtro.
        gpu.Espera();
        UINT64 antes = 0, depois = 0, orc = 0, ram = 0;
        gpu.Memoria(antes, orc, ram);
        rede.Encerra();
        redeOk = false;
        redeSolta = true;
        rayRegen.Fecha();
        for (Tex* t : {&inteiro, &inteiroA, &orig, &fed, &fedAnt, &origCrua})
            t->Solta();
        origCruaValida = false;
        fedAntValido = false;
        gpu.Memoria(depois, orc, ram);
        Log("memoria", Fmt("desligado: rede, quadros e Ray Regeneration soltos, VRAM do processo %llu -> %llu MB",
                           (unsigned long long)(antes >> 20), (unsigned long long)(depois >> 20)));
    }
    // jogo com o modulo dentro (injetado antes) e o FSR rodando: com o Modo de Rede em
    // buffer o overlay conecta sozinho, sem Ctrl+Shift+D (o que o usuario desligou fica desligado)
    void ProcuraJogo()
    {
        static auto ultima = std::chrono::steady_clock::now();
        const auto agora = std::chrono::steady_clock::now();
        if (Ligado() || agora - ultima < std::chrono::seconds(1))
            return;
        ultima = agora;
        // o executavel escolhido no painel: liga sozinho quando ele abrir
        if (!exeEscolhido.empty())
            if (const DWORD pid = ProcessoDoExe(exeEscolhido, pidRecusado))
                if (HWND h = JanelaDoProcesso(pid))
                {
                    Log("app", Fmt("%s aberto (pid %lu): ligando", Utf8(exeEscolhido).c_str(), (unsigned long)pid));
                    Liga(h);
                    if (!Ligado())
                        pidRecusado = pid;
                    return;
                }
        if (cfg.fonte != 1)
            return;
        if (const DWORD pid = ProcessoComModulo(pidRecusado))
            if (HWND h = JanelaDoProcesso(pid))
            {
                Log("buffer", Fmt("jogo com o modulo dentro (pid %lu): conectando", (unsigned long)pid));
                Liga(h);
                if (!Ligado())
                    pidRecusado = pid; // nao conectou: nao tenta de novo a cada segundo
            }
    }
    std::wstring exeEscolhido; // bin\jogo_escolhido.txt: fora do config (que muda com o perfil do jogo)
    void LeExeEscolhido() { exeEscolhido = Largo(LeTexto(pasta + L"\\jogo_escolhido.txt")); }
    void DefineExeEscolhido(const std::wstring& exe)
    {
        exeEscolhido = exe;
        pidRecusado = 0;
        if (FILE* f = _wfopen((pasta + L"\\jogo_escolhido.txt").c_str(), L"wb"))
        {
            const std::string t = Utf8(exe);
            fwrite(t.data(), 1, t.size(), f);
            fclose(f);
        }
        Log("app", exe.empty() ? std::string("exe esquecido") : "exe escolhido: " + Utf8(exe));
    }
    static HWND JanelaDoProcesso(DWORD pid)
    {
        struct Busca
        {
            DWORD pid;
            HWND achou;
        } b{pid, nullptr};
        EnumWindows(
            [](HWND h, LPARAM l) -> BOOL {
                auto* b = reinterpret_cast<Busca*>(l);
                DWORD p = 0;
                GetWindowThreadProcessId(h, &p);
                RECT r{};
                if (p == b->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER) && GetClientRect(h, &r) &&
                    r.right - r.left >= 320 && r.bottom - r.top >= 200)
                {
                    b->achou = h;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&b));
        return b.achou;
    }
    // captura de buffer: coloca a DLSS5Buffer.dll no jogo aberto (sem gravar nada na pasta dele) e espera o modulo
    // aparecer (ele cria a memoria compartilhada no primeiro quadro do FSR/XeSS do jogo)
    void LigaBuffer(HWND h)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        pidBuffer = pid;
        exeJogo = ExeDoProcesso(pid);
        // a DLL que as versoes com a geracao de quadros deixaram na pasta do jogo: sem isso o jogo seguiria abrindo com ela
        avisoPastaJogo = LimpaPastaDoJogo(exeJogo);
        if (!avisoPastaJogo.empty())
            Log("buffer", avisoPastaJogo);
        CarregaPerfil(h);
        painel.Ancora(h);
        painel.DefineJogo(h);
        ResultadoInjecao r;
        painel.DefineMouseJogo(nullptr); // Abre fecha a memoria compartilhada de antes
        if (fb.Abre(gpu, pid))
            r.ok = true; // o modulo ja esta no jogo (religou, ou e o jogo de teste)
        else if (fb.VersaoRecusada())
            // o modulo ja esta no jogo, de um protocolo que este overlay nao fala: injetar outro por cima nao resolve
            // (e o jogo nega o acesso; a mensagem era "rode como administrador")
            r.motivo = Fmt("o modulo dentro do jogo e de outra versao (protocolo %u; este overlay fala %u a %u)",
                           fb.VersaoRecusada(), dlss5ov::kVersaoMinima, dlss5ov::kVersao);
        else
            r = InjetaBuffer(pid, Dll());
        if (!r.ok)
        {
            estadoBuffer = "nao conectou: " + r.motivo;
            if (fb.VersaoRecusada())
                estadoBuffer += " | feche e abra o jogo";
            Log("buffer", estadoBuffer);
            return;
        }
        if (!r.motivo.empty())
            Log("buffer", r.motivo);
        IniciaSessao();
        buffer = true;
        alvoBuffer = h;
        hdr = true; // a cor do jogo antes do tonemap e linear
        escalaSdr = 1.f;
        estadoBuffer = r.upscaler.empty() ? "esperando o jogo usar FSR 3.1 ou mais novo (escolha no menu grafico do jogo)"
                                          : "esperando o primeiro quadro do " + r.upscaler;
        if (!r.motivo.empty())
            estadoBuffer += " | " + r.motivo;
        fluxo.Reinicia();
        temporal.Reinicia();
        zeraHistorico = true;
        ov.Mostra(false);
        // o que a captura de buffer nao usa sai da VRAM: o jogo fica no limite do orcamento (quem precisar recria)
        gpu.Espera();
        ov.SoltaSwapchain();
        fedAnt.Solta();
        fluxo.Libera();
        Log("buffer", "ligado em " + Utf8(exeAlvo));
    }

    // captura da API: a mesma DLL no jogo, mas a copia sai do Present (D3D11 e D3D12, com ou sem upscaler). Daqui em diante e o
    // caminho da captura de video: janela do overlay, fluxo optico e o quadro ja com o HUD
    void LigaApi(HWND h)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        exeJogo = ExeDoProcesso(pid);
        avisoPastaJogo = LimpaPastaDoJogo(exeJogo);
        if (!avisoPastaJogo.empty())
            Log("api", avisoPastaJogo);
        CarregaPerfil(h);
        painel.Ancora(h);
        painel.DefineJogo(h);
        // Vulkan com a camada, ou o modulo que ja estava no jogo: a memoria ja existe e nao ha o que injetar
        ResultadoInjecao r;
        r.ok = fa.Abre(gpu, pid, h);
        if (!r.ok)
        {
            r = InjetaBuffer(pid, Dll(), true);
            if (r.ok && !fa.Abre(gpu, pid, h))
            {
                r.ok = false;
                r.motivo = "o modulo dentro do jogo e de uma versao antiga: feche e abra o jogo";
            }
        }
        // jogo Vulkan com a camada ligada: ela abre a memoria no primeiro swapchain, e Quadro tenta de novo ate la
        const bool esperaCamada = !r.ok && r.vulkan && camadaVulkan;
        if (!r.ok && !esperaCamada)
        {
            estadoBuffer = "nao conectou: " + r.motivo;
            Log("api", estadoBuffer);
            return;
        }
        if (esperaCamada)
            r.motivo.clear();
        if (!r.motivo.empty())
            Log("api", r.motivo);
        IniciaSessao();
        float esc = 1.f;
        hdr = MonitorHdr(gpu, h, esc);
        escalaSdr = esc;
        Log("app", hdr ? Fmt("monitor em HDR: branco SDR = %.0f nits", esc * 80.f) : std::string("monitor em SDR"));
        captApi = true;
        alvoBuffer = h;
        quadroApi = 0;
        apiDesde = GetTickCount64();
        estadoBuffer = "esperando o primeiro quadro do jogo";
        if (!r.motivo.empty())
            estadoBuffer += " | " + r.motivo;
        fluxo.Reinicia();
        temporal.Reinicia();
        zeraHistorico = true;
        Log("api", "ligado em " + Utf8(exeAlvo));
    }
    // A camada Vulkan: registrada so enquanto o overlay esta aberto com a opcao ligada (camada_vulkan.txt: fora do config, que muda
    // com o perfil do jogo). Sem o evento ela recusa carregar, entao um registro que sobrou de uma queda nao faz nada; a proxima
    // abertura do overlay o tira.
    bool camadaVulkan = false;
    HANDLE camadaEvento = nullptr;
    void LeCamada() { camadaVulkan = LeTexto(pasta + L"\\camada_vulkan.txt").rfind("1", 0) == 0; }
    void AplicaCamada(bool liga)
    {
        static const wchar_t* const kChave = L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers";
        const std::wstring manifesto = pasta + L"\\captura\\DLSS5Camada.json";
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kChave, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k) == ERROR_SUCCESS)
        {
            // os nossos, inclusive de outra pasta (o overlay foi movido): as camadas de outros programas ficam
            std::vector<std::wstring> nossos;
            wchar_t nome[1024];
            for (DWORD i = 0;; ++i)
            {
                DWORD n = 1024;
                if (RegEnumValueW(k, i, nome, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                    break;
                const std::wstring v = nome;
                static const std::wstring kFim = L"\\DLSS5Camada.json";
                if (v.size() >= kFim.size() && !_wcsicmp(v.c_str() + v.size() - kFim.size(), kFim.c_str()))
                    nossos.push_back(v);
            }
            for (const auto& v : nossos)
                if (!(liga && !_wcsicmp(v.c_str(), manifesto.c_str())))
                    RegDeleteValueW(k, v.c_str());
            RegCloseKey(k);
        }
        if (!liga)
        {
            if (camadaEvento)
                CloseHandle(camadaEvento);
            camadaEvento = nullptr;
            return;
        }
        if (GetFileAttributesW(manifesto.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            Log("api", "camada Vulkan: DLSS5Camada.json nao encontrado em " + Utf8(manifesto));
            return;
        }
        if (!camadaEvento)
            camadaEvento = CreateEventW(nullptr, TRUE, FALSE, dlss5ov::kEventoCamada);
        const DWORD zero = 0; // 0 = o carregador abre o manifesto
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kChave, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS)
        {
            RegSetValueExW(k, manifesto.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero), sizeof zero);
            RegCloseKey(k);
            Log("api", "camada Vulkan registrada: vale para o que abrir daqui em diante");
        }
    }
    void DefineCamada(bool liga)
    {
        camadaVulkan = liga;
        if (FILE* f = _wfopen((pasta + L"\\camada_vulkan.txt").c_str(), L"wb"))
        {
            fputs(liga ? "1" : "0", f);
            fclose(f);
        }
        if (liga)
            DeleteFileW((pasta + L"\\captura\\DLSS5Camada.log").c_str());
        AplicaCamada(liga);
    }
    // o back buffer vem como o jogo o escreveu: como ler (o passa do CSInteiro)
    UINT EntradaApi(DXGI_FORMAT f) const
    {
        constexpr UINT kPq = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 + 1;
        const UINT e = fa.EspacoCor();
        if (f == DXGI_FORMAT_R16G16B16A16_FLOAT)
            return hdr ? 0u : 3u;
        // 10 bits sem o jogo ter dito (ele escolheu antes do gancho): HDR10 se o monitor esta em HDR
        if (f == DXGI_FORMAT_R10G10B10A2_UNORM && (e == kPq || (!e && hdr)))
            return 2u;
        return hdr ? 1u : 0u;
    }

    // tamanho em que a rede roda: altura configurada (ou a da janela, se menor), largura pela proporcao, pares
    void TamanhoRede(UINT fw, UINT fh, UINT& dw, UINT& dh) const
    {
        const UINT alvo = (UINT)std::clamp(sessao.alturaRede, 360, 2160);
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
        const FormatosCor ff = FormatosAgora();
        if (inteiro.w == fw && inteiro.h == fh && fed.w == dw && fed.h == dh && saida.fmt == fmtSaida && inteiro.fmt == ff.inteiro &&
            (inteiroA.r != nullptr) == ff.alfaFora)
            return;
        formatos = ff;
        gpu.Espera();
        rede.Descarta(); // a resposta em voo e do tamanho antigo
        // a sessao HIP fica com os buffers de todo tamanho que ja rodou (2160p -> 720p seguia com 2,3 GB): so o Destroy devolve
        if (redeOk && fed.w && (fed.w != dw || fed.h != dh))
            RecriaSessaoRede();
        inteiro.Cria(gpu.dev.Get(), fw, fh, ff.inteiro, L"inteiro"); // a GPU parada: a textura velha sai antes de a nova entrar
        if (ff.alfaFora)
            inteiroA.Cria(gpu.dev.Get(), fw, fh, DXGI_FORMAT_R8_UNORM, L"inteiro: folga");
        else
            inteiroA.Solta();
        orig.Cria(gpu.dev.Get(), dw, dh, DXGI_FORMAT_R16G16B16A16_FLOAT, L"original no tamanho da rede");
        fed.Cria(gpu.dev.Get(), dw, dh, DXGI_FORMAT_R16G16B16A16_FLOAT, L"proxy (alimentado)", true, true);
        saida.Cria(gpu.dev.Get(), fw, fh, fmtSaida, L"saida");
        // recriadas no tamanho novo quando o denoise estiver ligado (a GPU ja esta parada aqui)
        if (!dumpPasta.empty())
            saidaAnt.Cria(gpu.dev.Get(), fw, fh, fmtSaida, L"saida anterior", false);
        // o movimento e alocado no quadro que precisar dele (PrecisaMovimento), ja no tamanho novo
        fedAnt.Solta();
        fluxo.Libera();
        temporal.Garante(gpu.dev.Get(), dw, dh);
        temporal.Reinicia();
        zeraHistorico = true;
        Log("app", Fmt("janela %ux%u, rede %ux%u", fw, fh, dw, dh));
    }
    // Os formatos escolhidos no painel. A cor (quadro inteiro e o que o jogo exporta) e a edicao que vai para o jogo.
    // Quadro com realce (HDR ou buffer): os UNORM cortariam acima de 1, a cor fica em float. A edicao e relativa, vale nos dois
    struct FormatosCor
    {
        DXGI_FORMAT inteiro, inteiroUav, inteiroSrv, corJogo, edicaoJogo;
        bool alfaFora, srgb;
    };
    FormatosCor formatos{DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                         DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, false, false};
    Tex inteiroA; // a folga do quadro inteiro quando ele nao tem alfa (R11G11B10_FLOAT)
    int ciclaFormato = 0; // --cicla-formato Q: troca o formato a cada Q quadros (o teste de que a troca nao acumula VRAM)
    FormatosCor FormatosAgora() const
    {
        const DXGI_FORMAT f16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
        const int op = std::clamp(cfg.formatoCor, 0, 4);
        FormatosCor f{f16, f16, f16, f16, f16, false, false};
        f.edicaoJogo = op == 1 || op == 2 ? DXGI_FORMAT_R10G10B10A2_UNORM : op == 4 ? DXGI_FORMAT_R8G8B8A8_SNORM : f16;
        // (na ponte 3 e na reproducao o quadro inteiro passa de 1 mesmo em SDR)
        const bool soFloat = buffer || hdr || cfg.ponte == 3 || !pastaReproducao.empty();
        if (op == 1)
        {
            f.inteiro = f.inteiroUav = f.inteiroSrv = f.corJogo = DXGI_FORMAT_R11G11B10_FLOAT;
            f.alfaFora = true;
        }
        else if (!soFloat && op == 2)
            f.inteiro = f.inteiroUav = f.inteiroSrv = DXGI_FORMAT_R10G10B10A2_UNORM;
        else if (!soFloat && op == 3)
            f.inteiro = f.inteiroUav = f.inteiroSrv = DXGI_FORMAT_R16G16B16A16_UNORM;
        else if (!soFloat && op == 4)
        {
            // gravado em sRGB, a mesma coisa que a captura de 8 bits; a vista _SRGB devolve linear
            f.inteiro = DXGI_FORMAT_R8G8B8A8_TYPELESS, f.inteiroUav = DXGI_FORMAT_R8G8B8A8_UNORM, f.inteiroSrv = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            f.srgb = true;
        }
        return f;
    }
    static const char* NomeFormato(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
        case DXGI_FORMAT_R8G8B8A8_SNORM: return "R8G8B8A8_SNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM";
        default: return "R16G16B16A16_FLOAT";
        }
    }
    std::string TextoFormatos() const
    {
        std::string t = std::string("Em uso: cor ") + NomeFormato(formatos.inteiroSrv) + (formatos.alfaFora ? " + R8_UNORM" : "");
        if (buffer)
            t += std::string(", edição ") + NomeFormato(formatos.edicaoJogo) +
                 (formatos.edicaoJogo == DXGI_FORMAT_R10G10B10A2_UNORM ? " + R8_UNORM" : "");
        return t;
    }
    // O denoise do grao da rede: filtra a edicao dela (o sinal ruidoso, com sinal, log2(saida / cor)) no tamanho da rede, onde o
    // grao tem 1 pixel, com a imagem do jogo so como guia, antes de ela ser ampliada e composta. Como o NRD pede ("de-modulated
    // before denoising"): onde a rede nao mexeu a edicao e zero e nada muda, e o detalhe do jogo nao passa pelo filtro.
    // reconstrucao -> acumulacao -> filtro de bordas. Devolve a edicao filtrada, em SRV
    uint64_t quadroDnEdicao = 0;
    // jogo: o sinal e o quadro pronto (jogo + rede), na resolucao da tela, no lugar da edicao no tamanho da rede
    Tex* DenoiseEdicao(ID3D12GraphicsCommandList* c, Tex* ed, bool temMov)
    {
        quadroDnEdicao = quadros;
        if (!temMov)
            return ed;
        // no painel: 0 imagem, 1 visao geral, 2 em diante os paineis do manual sem o 7 ("Unused")
        const int vistaSdk = cfg.dnVista >= 9 ? cfg.dnVista + 1 : cfg.dnVista;
        Tex* filtrada = rayRegen.Filtra(c, desc, gpu, pasta, *ed, orig, LuzNaTela(cfg.temporal.luz), cfg.dnRegenForca, cfg.dnRegenParam, vistaSdk, saida.w ? saida.w : ed->w, saida.h ? saida.h : ed->h);
        return filtrada ? filtrada : ed;
    }
    // o que falta, dito no painel: sem isto o denoise parece funcionar com a profundidade fixa
    std::string EstadoDenoise()
    {
        if (!cfg.dnAtivo)
            return "";
        std::string s;
        if (captApi && !fa.Prof() && cfg.dnRegenProf)
        {
            if (fa.Api() == dlss5ov::kApiVulkan)
                s = ", sem profundidade (Vulkan)";
            else if (fa.ProfRecusada() == dlss5ov::kProfSemVram)
                s = ", sem profundidade: nao coube na VRAM";
            else if (fa.ProfRecusada())
                s = Fmt(", sem profundidade: o buffer do jogo esta no formato %u, que nao da para copiar", fa.ProfRecusada());
            else
                s = ", sem profundidade: nenhum buffer achado nesta tela";
        }
        return rayRegen.Estado().empty() ? s : s + ", " + rayRegen.Estado();
    }
    void RecriaSessaoRede()
    {
        const auto t0 = std::chrono::steady_clock::now();
        rede.Encerra();
        redeOk = rede.Inicia(gpu, PastaRede(), redePequenaAtiva);
        nivelAtual = -1;
        Log("rede", redeOk ? Fmt("sessao recriada para o tamanho novo em %.2f s",
                                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count())
                           : std::string("a rede nao subiu depois de mudar de tamanho"));
    }

    // ---- modo imagem (comparacao com as fotos da NVIDIA): um arquivo no lugar da captura, sem overlay ----
    ComPtr<ID3D12Resource> imagem;
    UINT imgW = 0, imgH = 0;
    DXGI_FORMAT imgFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    // ---- reproducao (--reproduz pasta): os quadros gravados (Ctrl+Shift+G) entram como entrada, um por quadro ----
    // Gravacao da captura de buffer (tem "dist"): o movimento e a distancia gravados entram no lugar dos buffers do jogo
    // (DoJogo, oclusao reconstruida, acumulo exato) -- o modo buffer inteiro fora do jogo, deterministico.
    bool reproducao = false, repJogo = false;
    Tex repMov, repProf; // movimento (pixels da rede) e "profundidade" 1 / distancia (Z invertido infinito, perto = 1)
    std::wstring pastaReproducao;
    int quadrosReproducao = 0;
    ComPtr<ID3D12Resource> uploadReproducao;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fpReproducao{};
    bool Exata() const { return buffer || repJogo; } // movimento e distancia do jogo (ao vivo ou gravados)
    // sobe bytes (linhas de w * bpp) para a textura t (GPU ociosa; so na reproducao)
    void SobeTex(Tex& t, const std::vector<unsigned char>& b, UINT bpp)
    {
        const auto rd = t.r->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        gpu.dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> up;
        Ck(gpu.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&up)),
           "upload(reproducao jogo)");
        unsigned char* m = nullptr;
        up->Map(0, nullptr, (void**)&m);
        for (UINT y = 0; y < t.h; ++y)
            memcpy(m + fp.Offset + size_t(y) * fp.Footprint.RowPitch, b.data() + size_t(y) * t.w * bpp, size_t(t.w) * bpp);
        up->Unmap(0, nullptr);
        Ck(al[0]->Reset(), "reset");
        Ck(cl[0]->Reset(al[0].Get(), nullptr), "list");
        t.Para(cl[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{t.r.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION src{up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint = fp;
        cl[0]->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        t.Para(cl[0].Get(), D3D12_RESOURCE_STATE_COMMON);
        cl[0]->Close();
        ID3D12CommandList* l[] = {cl[0].Get()};
        gpu.fila->ExecuteCommandLists(1, l);
        gpu.Espera();
    }
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
        repJogo = cab.find("\"dist\":") != std::string::npos;
        if (repJogo)
        {
            repMov.Cria(gpu.dev.Get(), imgW, imgH, DXGI_FORMAT_R16G16_FLOAT, L"reproducao movimento", false);
            repProf.Cria(gpu.dev.Get(), imgW, imgH, DXGI_FORMAT_R32_FLOAT, L"reproducao profundidade", false);
            Log("reproducao", "gravacao da captura de buffer: movimento e distancia gravados no lugar dos buffers do jogo");
        }
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
        if (repJogo)
        {
            auto le = [&](const wchar_t* nome, size_t bytes) {
                std::vector<unsigned char> b(bytes);
                wchar_t a[64];
                swprintf(a, 64, L"\\%04d_%s.raw", i, nome);
                FILE* g = nullptr;
                _wfopen_s(&g, (pastaReproducao + a).c_str(), L"rb");
                if (!g || fread(b.data(), 1, bytes, g) != bytes)
                    throw std::runtime_error("reproducao: falta o movimento ou a distancia gravada");
                fclose(g);
                return b;
            };
            SobeTex(repMov, le(L"mov", size_t(imgW) * imgH * 4), 4);
            auto z = le(L"dist", size_t(imgW) * imgH * 4);
            float* zf = reinterpret_cast<float*>(z.data());
            for (size_t k = 0; k < size_t(imgW) * imgH; ++k)
                zf[k] = 1.f / std::max(zf[k], 1e-6f); // Dist(d) = perto / d com perto = 1 devolve a distancia gravada
            SobeTex(repProf, z, 4);
        }
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
    // 0 = 71 blocos, 1 = 68, 2 = 63, 3 = 55, 4 = 47. O runtime (SetDynamicSkip) recusa 31..38 com o reuso adaptativo
    // do ViT e exige um bloco vivo em cada cadeia C32 (1..4, 66..69); o adaptador aceita a lista e so a reenvia, entao
    // um nivel recusado nao avisa: o teto fica aqui.
    int NivelMaximo() const { return vitAdaptativo ? 3 : 4; }
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
        return v;
    }
    int nivelAtual = -1, nivelAlvo = 0, quadrosNoNivel = 0;
    bool redeSolta = false; // a rede foi solta pelo desligar (o painel nao avisa falha; Liga sobe de novo)
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
        int alvo = std::clamp(cfg.profundidade, 0, NivelMaximo());
        if (cfg.profundidadeAuto && temMov && estat.valida)
        {
            // velocidade em pixels de uma rede de 1080 linhas
            const float v = estat.velocidade * 1080.f / float(std::max(1u, fed.h));
            const int pelaVelocidade = v < 3.f ? 0 : v < 8.f ? 1 : v < 16.f ? 2 : v < 32.f ? 3 : v < 64.f ? 4 : 5;
            alvo = std::min(std::max(alvo, pelaVelocidade), std::clamp(cfg.profundidadeMax, 0, NivelMaximo()));
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
            // sem atraso: o jogo espera a edicao do quadro exportado antes (cab->espera 2) e mostra o quadro processado
            // repetido em 1..4 quadros (o Ritmo vira os quadros repetidos)
            // Rede mais lenta que o prazo do jogo (em 2160): preso a ela, o quadro leva o tempo da rede e o input lag fica grotesco. Sem o
            // pedido no painel o jogo roda solto e a edicao chega atrasada (sem os 2 estouros de 100 ms a cada suspensao, que engasgavam)
            if (msMedio > 95.0)
                redeLenta = true;
            else if (msMedio < 70.0)
                redeLenta = false;
            const bool espera = atraso < 0 && (!redeLenta || cfg.sincronizaRedeLenta);
            fb.Batimento(true, cfg.modo != 1 && redeOk, espera ? 2u : 0u, espera ? 1u : 0u, 0u);
            // preso a rede lenta: o jogo espera o tempo que ela esta levando, com folga; o teto fica abaixo do 1 s sem batimento
            fb.DefinePrazo(cfg.sincronizaRedeLenta ? uint32_t(std::clamp(msMedio * 3.0, 100.0, 900.0)) : 100u);
            if (ciclaFormato && quadros && quadros % ciclaFormato == 0)
            {
                cfg.formatoCor = (cfg.formatoCor + 1) % 5;
                Log("memoria", Fmt("teste: formato %d", cfg.formatoCor));
            }
            {
                const FormatosCor ff = FormatosAgora();
                fb.DefineFormatos(ff.corJogo, ff.edicaoJogo);
            }
            if (!fb.Pega(qb))
            {
                if (fb.Aberta() && !fb.JogoVivo())
                    estadoBuffer = "o jogo parou de mandar quadros (FSR desligado ou jogo pausado)";
                else if (const std::string e = fb.Erro(); !e.empty())
                    estadoBuffer = "nao conectou: texturas da captura nao criadas " + e;
                return false;
            }
            quadroJogo = qb.n;
            q.tex = qb.cor->r.Get();
            q.fmt = qb.cor->fmt;
            q.w = qb.cor->w;
            q.h = qb.cor->h;
            q.cercaValor = 0;
            estadoBuffer = Fmt("conectado: %ux%u, cor sem HUD, profundidade e movimento do jogo", q.w, q.h) + EstadoDenoise();
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
        else if (captApi)
        {
            if (!IsWindow(alvoBuffer))
            {
                Desliga();
                estadoBuffer = "o jogo fechou";
                return false;
            }
            if (!fa.Aberta())
            {
                // jogo Vulkan esperando a camada abrir a memoria (no primeiro swapchain)
                static ULONGLONG tentaEm = 0;
                const ULONGLONG agora = GetTickCount64();
                if (agora >= tentaEm)
                {
                    tentaEm = agora + 500;
                    DWORD pid = 0;
                    GetWindowThreadProcessId(alvoBuffer, &pid);
                    fa.Abre(gpu, pid, alvoBuffer);
                }
                if (!fa.Aberta())
                {
                    if (agora - apiDesde > 8000)
                        estadoBuffer = "nao conectou: o jogo abriu antes da camada Vulkan. Feche e abra o jogo com o overlay aberto";
                    return false;
                }
            }
            fa.Batimento();
            fa.QuerProf(cfg.dnAtivo && cfg.dnRegenProf);
            // saiu antes de usar o quadro passado (Ritmo, janela minimizada): o slot volta ao jogo
            if (quadroApi)
            {
                fa.Consumido(quadroApi);
                quadroApi = 0;
            }
            if (HWND j = fa.JanelaSwap(); j && j != alvoBuffer && IsWindow(j))
            {
                Log("api", "o jogo apresenta em outra janela: seguindo ela");
                alvoBuffer = j;
                fa.DefineAlvo(j);
            }
            Tex* t = nullptr;
            if (!fa.Pega(t, quadroApi))
            {
                if (*fa.Erro())
                    estadoBuffer = std::string("nao conectou: ") + fa.Erro();
                else if (!fa.JogoVivo() && GetTickCount64() - apiDesde > 3000)
                    estadoBuffer = "o jogo nao esta apresentando quadros (pausado, ou a tela dele nao e DirectX 11, 12 nem Vulkan)";
                return false;
            }
            q.tex = t->r.Get();
            q.fmt = t->fmt;
            q.w = t->w;
            q.h = t->h;
            q.cercaValor = 0; // so pega o que a GPU do jogo ja terminou
            estadoBuffer = (fa.Api() == dlss5ov::kApiVulkan ? std::string("conectado: Vulkan") : Fmt("conectado: DirectX %u", fa.Api())) +
                           Fmt(", %ux%u, com HUD, movimento por fluxo optico%s", q.w, q.h, fa.Prof() ? (fa.ProfExata() ? ", profundidade do FSR" : ", profundidade do jogo") : "") + EstadoDenoise();
        }
        else if (!cap.Pega(q))
            return false;
        const auto t0 = std::chrono::steady_clock::now();
        RECT r{};
        if (!imagem && !buffer && (IsIconic(Alvo()) || !Captura::RetanguloCliente(Alvo(), r)))
        {
            ov.Mostra(false);
            return false;
        }
        Garante(q.w, q.h);
        if (captApi)
            ov.Posiciona(r, hdr, q.w, q.h);
        else if (!imagem && !buffer)
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
        // a luz direta (4) e para luz de cena (captura de buffer): na captura de video o quadro ja passou pelo tonemap do
        // jogo e, em HDR, a neve a varias vezes o branco satura o codificador (no TLOU2 em HDR a imagem ficou quase
        // preta e borrada com ela). Ali ela vira a automatica.
        const int ponteCfg = (cfg.ponte == 4 && !Exata()) ? -1 : cfg.ponte;
        const UINT ponte = ponteCfg < 0 ? (hdr ? 1u : 0u) : (UINT)std::clamp(ponteCfg, 0, 4);
        pontePassada = ponte;
        UINT k[16] = {inteiro.w, inteiro.h, fed.w, fed.h, hdr ? 1u : 0u, 0, ponte, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        memcpy(&k[11], &cfg.preLimiarSombra, 4);
        memcpy(&k[12], &cfg.preCor, 4);
        k[13] = (reproducao ? 1u : 0u) | ((captApi ? EntradaApi(q.fmt) : 0u) << 1);
        memcpy(&k[8], &cfg.preLuz, 4);
        memcpy(&k[9], &cfg.preSombra, 4);
        memcpy(&k[10], &cfg.preLimiar, 4);
        memcpy(&k[5], &escK, 4);
        const float expK = std::max(cfg.temporal.exposicao, 1e-3f);
        memcpy(&k[7], &expK, 4);
        k[14] = formatos.alfaFora ? 1u : 0u;
        k[15] = formatos.srgb ? 1u : 0u;
        if (formatos.alfaFora)
            inteiroA.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // (sem alfa fora o u1 nao e escrito)
        pInteiro.Roda(c, desc, gpu.dev.Get(), {{q.tex, q.fmt}},
                      {{inteiro.r.Get(), formatos.inteiroUav},
                       formatos.alfaFora ? std::pair<ID3D12Resource*, DXGI_FORMAT>{inteiroA.r.Get(), inteiroA.fmt}
                                         : std::pair<ID3D12Resource*, DXGI_FORMAT>{inteiro.r.Get(), formatos.inteiroUav}},
                      k, (inteiro.w + 7) / 8, (inteiro.h + 7) / 8);
        if (formatos.alfaFora)
            inteiroA.Para(c, SRV);
        inteiro.Para(c, SRV);
        orig.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        fed.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        pReduz.Roda(c, desc, gpu.dev.Get(), {{inteiro.r.Get(), formatos.inteiroSrv}},
                    {{orig.r.Get(), orig.fmt}, {fed.r.Get(), fed.fmt}}, k, (fed.w + 7) / 8, (fed.h + 7) / 8);
        orig.Para(c, SRV);
        fed.Para(c, SRV);
        Barreira(c, q.tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (buffer)
        {
            // Z invertido: o que o jogo diz ao FSR, senao o medido pelo DoJogo; o jogo reprojeta a edicao com a mesma
            // conta de distancia
            invertidaUsada = qb.profInvertidaConhecida ? qb.profInvertida : fluxo.InvertidaMedida();
            fb.DefineInvertida(invertidaUsada);
        }
        // o movimento (fluxo optico na captura de video, o do jogo na de buffer) so serve ao historico da propria rede, a
        // exibicao "Movimento", a gravacao e a profundidade automatica -- e, na captura de buffer, a medida do Z invertido
        // quando o jogo nao diz. Sem eles nao e calculado nem alocado (~430 MB em 4K com a rede em 2160).
        const bool comMov = PrecisaMovimento() || (buffer && !qb.profInvertidaConhecida);
        if (comMov)
        {
            fluxo.Garante(gpu.dev.Get(), fed.w, fed.h, !buffer && !repJogo);
            if (fedAnt.w != fed.w || fedAnt.h != fed.h)
            {
                fedAnt.Cria(gpu.dev.Get(), fed.w, fed.h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"alimentado anterior", false);
                fedAntValido = false;
            }
        }
        bool temMov = false, movDenoise = false;
        if (comMov && buffer)
        {
            // captura de buffer: o movimento e a profundidade do proprio jogo (exatos), no lugar do fluxo optico
            Fluxo::Camera cam;
            cam.corW = float(q.w);
            cam.corH = float(q.h);
            cam.perto = qb.perto;
            cam.longe = qb.longe;
            cam.fovY = fovYJogo = qb.fovY;
            cam.invertida = invertidaUsada;
            cam.infinita = qb.profInfinita;
            cam.reset = qb.reset;
            cam.jitterX = qb.jitterX;
            cam.jitterY = qb.jitterY;
            fluxo.DoJogo(c, desc, gpu.dev.Get(), *qb.mov, *qb.prof, cam);
            temMov = true;
        }
        else if (comMov && repJogo)
        {
            // reproducao de uma gravacao da captura de buffer: o movimento (pixels da rede) e a distancia radial gravados
            Fluxo::Camera cam;
            cam.corW = float(repMov.w);
            cam.corH = float(repMov.h);
            cam.perto = 1.f; // repProf = 1 / distancia: Dist devolve a distancia gravada (ja radial: sem FOV)
            cam.invertida = cam.infinita = true;
            fluxo.DoJogo(c, desc, gpu.dev.Get(), repMov, repProf, cam);
            temMov = true;
        }
        else if (comMov)
        {
            // o denoiser precisa do movimento mesmo com o "movimento simulado" desligado para a rede
            const bool calculou = (cfg.movimento || cfg.dnAtivo) && fluxo.Calcula(c, desc, gpu.dev.Get(), fed.r.Get(), fed.fmt);
            temMov = calculou && cfg.movimento;
            movDenoise = calculou;
        }
        movDenoise = movDenoise || temMov;
        if (temMov && fedAntValido)
            fluxo.Estatisticas(c, desc, gpu.dev.Get(), fed, fedAnt);
        // A rede ve este quadro e responde nele mesmo, como a NVIDIA roda o DLSS 5: a rede dentro do quadro, a edicao do
        // proprio quadro, nada trazido de quadros velhos (todo rastro vinha de trazer a edicao de quadros atras, por mais
        // que o movimento fosse exato). Na captura de video o quadro vai para a tela depois da rede; na de buffer o jogo
        // espera a edicao.
        const bool podeRede = redeOk && rede.Pronta() && cfg.modo != 1;
        // (com a rede ainda presa num quadro anterior -- VRAM cheia -- nada e enviado; a resposta dela, de um quadro velho,
        // e descartada quando chegar)
        bool envia = podeRede && !rede.EmVoo();
        if (envia)
        {
            AjustaProfundidade(temMov);
            CopiaParaRede(c, temMov);
            if (movDenoise && cfg.dnAtivo && (cfg.modo == 0 || cfg.modo == 2 || cfg.modo == 5))
            {
                EntradaRayRegen e;
                e.mov = &fluxo.Movimento();
                e.salto = quadroDnEdicao + 1 != quadros || (buffer && qb.reset); // quadro sem resposta da rede, ou corte de cena
                if (buffer)
                {
                    // o original do jogo, nao o da rede (pixel mais proximo, para o historico dela)
                    e.tanY = dlss5ov::TanMeioFov(fovYJogo);
                    e.perto = qb.perto;
                    e.longe = qb.longe;
                    e.infinita = qb.profInfinita;
                    e.invertidaConhecida = true;
                    e.invertida = invertidaUsada;
                    if (cfg.dnRegenProf)
                    {
                        e.dist = qb.prof;
                        e.distCrua = true;
                        e.mov = qb.mov;
                        e.jitterX = qb.jitterX;
                        e.jitterY = qb.jitterY;
                        e.escMovX = float(orig.w) / float(q.w);
                        e.escMovY = float(orig.h) / float(q.h);
                    }
                }
                else if (cfg.dnRegenProf)
                {
                    e.dist = captApi ? fa.Prof() : fluxo.DistanciaAtual();
                    e.distCrua = captApi;
                }
                rayRegen.Prepara(c, desc, gpu, orig, e);
                float cobertura;
                if (rayRegen.PegaCobertura(cobertura) && captApi)
                    fa.AvaliaProf(cobertura);
            }
        }
        Ck(c->Close(), "close0");
        ID3D12CommandList* l0[] = {c};
        gpu.fila->ExecuteCommandLists(1, l0);
        bool chegou = false, velha = false;
        if (envia)
        {
            if (rede.Envia(gpu.cerca.Get(), gpu.Sinaliza(), ParamsDaRede()))
            {
                MarcaEnvio();
                // ate 1 s: com a VRAM cheia (o jogo usando quase tudo) a rede pode levar segundos por quadro; a tela nao
                // congela esperando, o quadro sai sem edicao
                chegou = rede.EsperaChegar(1000);
            }
        }
        else if (rede.EmVoo() && rede.Chegou())
        {
            // a resposta que passou de 1 s e de um quadro velho: libera a rede, mas nao entra (seria o fantasma)
            rede.EsperaChegar();
            chegou = velha = true;
        }
        if (chegou)
            Chegada();

        // ---- lista 2: edicao, composicao, apresentacao ----
        Ck(al[1]->Reset(), "reset1");
        Ck(cl[1]->Reset(al[1].Get(), nullptr), "list1");
        c = cl[1].Get();
        ParamsTemporal tp = cfg.temporal;
        tp.intensidade = cfg.ngx.intensidade;
        // resposta de um quadro anterior a um corte de cena e da cena velha: descartada
        const bool usa = chegou && !velha && cfg.modo != 1 && redeOk && quadroEnvio >= quadroCorte;
        // a edicao no espaco comprimido (Crua) so para quem a le: a exibicao "saida crua", a gravacao e o dump
        const bool comCrua = cfg.modo == 5 || gravando > 0 || gravaEm > 0 || !dumpPasta.empty();
        if (comCrua)
            temporal.GaranteCrua(gpu.dev.Get());
        Tex* edicao = ultimaEdicao =
            usa ? temporal.Edicao(c, desc, gpu.dev.Get(), orig, rede.Entrada(), rede.Resposta(), tp, comCrua) : nullptr;
        dnBruta = edicao; // a edicao deste quadro antes do denoise
        if (usa)
        {
            if (cfg.modo == 5)
            {
                // Exibicao "saida crua": o quadro desta resposta, ao lado da edicao crua dela (fica nos quadros sem
                // resposta)
                if (origCrua.w != orig.w || origCrua.h != orig.h)
                    origCrua.Cria(gpu.dev.Get(), orig.w, orig.h, orig.fmt, L"quadro da saida crua", false);
                const auto antes = orig.estado;
                orig.Para(c, D3D12_RESOURCE_STATE_COPY_SOURCE);
                origCrua.Para(c, D3D12_RESOURCE_STATE_COPY_DEST);
                c->CopyResource(origCrua.r.Get(), orig.r.Get());
                orig.Para(c, antes);
                origCrua.Para(c, SRV);
                origCruaValida = true;
            }
            // de volta ao estado em que a fila da rede pega
            rede.Entrada().Para(c, D3D12_RESOURCE_STATE_COMMON);
            rede.Resposta().Para(c, D3D12_RESOURCE_STATE_COMMON);
            if (cfg.dnAtivo && (cfg.modo == 0 || cfg.modo == 2 || cfg.modo == 5))
                edicao = ultimaEdicao = DenoiseEdicao(c, edicao, movDenoise);
        }
        // gradacao do Model B/C (constantes do kernel da NVIDIA), escalada pelo tom local
        const float tg = std::clamp(cfg.ngx.tomLocal, 0.f, 2.f);
        float kExp = 0, kCon = 0, kSat = 0;
        if (cfg.ngx.modelo == 1)
            kExp = -0.10f * tg, kCon = -0.25f * tg, kSat = -0.10f * tg;
        else if (cfg.ngx.modelo == 2)
            kSat = -0.15f * tg;
        const bool grada = cfg.ngx.gradacao && cfg.ngx.modelo > 0;
        // Exibicao "saida crua": a ultima resposta e o quadro dela, no tamanho atual da rede
        const bool crua = cfg.modo == 5 && origCruaValida && origCrua.w == fed.w && origCrua.h == fed.h &&
                          temporal.Crua().w == fed.w && temporal.Crua().h == fed.h;
        if (crua)
            temporal.Crua().Para(c, SRV);
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
            float satMatiz[6], luzMatiz[6];
            UINT usaMatiz, temCrua, luz, padA1;
        } kc{inteiro.w,      inteiro.h,      fed.w, fed.h, edicao ? std::clamp(cfg.forca, 0.f, 2.f) : 0.f, (UINT)cfg.modo, cfg.divisao,
             edicao ? 1u : 0u, hdr ? 1u : 0u, escalaUsada, kExp, kCon, kSat, grada ? 1u : 0u,
             pontePassada == 3 ? 1u : 0u, (hdr && pontePassada == 1) ? 1u : 0u, cfg.preLuz, cfg.preSombra, cfg.preLimiar, cfg.preLimiarSombra, cfg.preCor,
             0, 0, 0, {}, {}, cfg.matiz.Ativa() ? 1u : 0u, (cfg.modo == 5 && crua) ? 1u : 0u,
             LuzNaTela(cfg.temporal.luz), formatos.alfaFora ? 1u : 0u};
        static_assert(sizeof(kc) == 40 * 4, "constantes: o mesmo layout do cbuffer do CSCompoe");
        for (int i = 0; i < 6; ++i)
        {
            kc.satMatiz[i] = std::clamp(cfg.matiz.sat[i], -1.f, 1.f);
            kc.luzMatiz[i] = std::clamp(cfg.matiz.luz[i], -1.f, 1.f);
        }
        using Par = std::pair<ID3D12Resource*, DXGI_FORMAT>;
        const Par vazio{fed.r.Get(), fed.fmt};
        pCompoe.Roda(c, desc, gpu.dev.Get(),
                     {{inteiro.r.Get(), formatos.inteiroSrv},
                      vazio,
                      edicao ? Par{edicao->r.Get(), edicao->fmt} : vazio,
                      temMov ? Par{fluxo.Movimento().r.Get(), fluxo.Movimento().fmt} : vazio,
                      crua ? Par{origCrua.r.Get(), origCrua.fmt} : vazio,
                      crua ? Par{temporal.Crua().r.Get(), temporal.Crua().fmt} : vazio,
                      formatos.alfaFora ? Par{inteiroA.r.Get(), inteiroA.fmt} : vazio},
                     {{saida.r.Get(), saida.fmt}}, &kc, (saida.w + 7) / 8, (saida.h + 7) / 8);
        if (cfg.dnAtivo && edicao && dnBruta)
        {
            if (cfg.dnVista > 0)
                VistaDenoise(c);
            else if (cfg.dnCas > 0.f)
                CasNaSaida(c);
        }
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
                float tanY, aspecto;
                UINT luz, cod;
            } ke{saida.w, saida.h, qb.prof->w, qb.prof->h, qb.perto, qb.longe, invertidaUsada ? 1u : 0u,
                 qb.profInfinita ? 1u : 0u, dlss5ov::TanMeioFov(qb.fovY), float(saida.w) / float(saida.h),
                 (UINT)std::clamp(cfg.temporal.luz, 0, 2),
                 edJogo->fmt == DXGI_FORMAT_R10G10B10A2_UNORM ? 1u : edJogo->fmt == DXGI_FORMAT_R8G8B8A8_SNORM ? 2u : 0u};
            Tex* edAlfa = fb.EdicaoAlfaLivre();
            if (edAlfa)
                edAlfa->Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            pExporta.Roda(c, desc, gpu.dev.Get(),
                          {{qb.cor->r.Get(), qb.cor->fmt}, {saida.r.Get(), saida.fmt}, {qb.prof->r.Get(), qb.prof->fmt}},
                          {{edJogo->r.Get(), edJogo->fmt},
                           edAlfa ? std::pair<ID3D12Resource*, DXGI_FORMAT>{edAlfa->r.Get(), edAlfa->fmt}
                                  : std::pair<ID3D12Resource*, DXGI_FORMAT>{edJogo->r.Get(), edJogo->fmt}},
                          &ke, (saida.w + 7) / 8, (saida.h + 7) / 8);
            edJogo->Para(c, D3D12_RESOURCE_STATE_COMMON);
            if (edAlfa)
                edAlfa->Para(c, D3D12_RESOURCE_STATE_COMMON);
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
            saida.Para(c, D3D12_RESOURCE_STATE_COPY_SOURCE);
            c->CopyResource(bb, saida.r.Get());
            Barreira(c, bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        }
        if (!dumpPasta.empty() && quadrosTeste > 0 && quadros + 1 >= (UINT64)quadrosTeste)
        {
            ultimoTemMov = temMov; // o ultimo quadro do dump: fedAnt guarda o anterior
            fedAntValido = false;
        }
        else if (fedAnt.r && (temMov || cfg.movimento || !dumpPasta.empty()))
        {
            // so quem le: as estatisticas do movimento no proximo quadro e o dump (antes copiava com o movimento desligado, sem leitor)
            fed.Para(c, D3D12_RESOURCE_STATE_COPY_SOURCE);
            fedAnt.Para(c, D3D12_RESOURCE_STATE_COPY_DEST);
            c->CopyResource(fedAnt.r.Get(), fed.r.Get());
            fed.Para(c, SRV);
            fedAntValido = true;
        }
        else
            fedAntValido = false;
        Ck(c->Close(), "close1");
        ID3D12CommandList* l1[] = {c};
        gpu.fila->ExecuteCommandLists(1, l1);
        if (edJogo)
            fb.Publica(qb.n, cfg.temporal.luz);
        const UINT64 fim = gpu.Sinaliza();
        if (apresenta)
        {
            ov.Apresenta();
            ov.Mostra(true);
        }
        gpu.EsperaValor(fim);
        VigiaMemoria();
        if (buffer)
            fb.Consumido(qb.n);
        if (captApi)
        {
            fa.Consumido(quadroApi);
            quadroApi = 0;
        }
        estat = temMov ? fluxo.LeEstatisticas() : Fluxo::Estat{};
        if (!comCrua && temporal.Crua().r)
            temporal.SoltaCrua();
        if (!comMov && fedAnt.r)
        {
            // o movimento deixou de ser usado (historico desligado, saiu da exibicao "Movimento", fim da gravacao): a
            // GPU ja terminou este quadro, a VRAM volta
            fedAnt.Solta();
            fluxo.Libera();
        }
        if (estat.valida && estat.semConfianca > 0.88f)
        {
            // corte de cena (o shader ja limpou este quadro): historico e grade da cena velha saem; a resposta em voo,
            // se foi enviada antes deste quadro, e descartada quando chegar; a rede roda logo na cena nova
            temporal.Reinicia();
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
                             "dos quadros, %.1f quadros / %.1f ms depois do envio | confianca %.3f, luz %.4f, vel %.1f px | "
                             "%.1f fps",
                             (unsigned long long)quadros, somaMs / 60.0, maxMs,
                             100.0 * double(quadrosComRede - comRedeMarca) / 60.0,
                             chegadas ? somaAtrasoQ / chegadas : 0.0, chegadas ? somaAtrasoMs / chegadas : 0.0,
                             1.0 - estat.semConfianca, estat.mudancaLuz, estat.velocidade, fps));
            fracaoRede = double(quadrosComRede - comRedeMarca) / 60.0;
            comRedeMarca = quadrosComRede;
            somaMs = maxMs = 0;
            somaAtrasoQ = somaAtrasoMs = 0;
            chegadas = 0;
        }
        return true;
    }
    // ---- rede na fila propria ----
    int atraso = -1;         // -1 ao vivo; 0 imagem e reproducao (testes: o jogo nao espera a edicao, nada e pulado)
    Tex origCrua;            // Exibicao "saida crua": o quadro da ultima resposta
    bool origCruaValida = false;
    UINT64 quadroEnvio = 0;  // o quadro do ultimo envio
    bool jaEnviou = false;   // quadroEnvio ja e de um envio (o primeiro pode ser o quadro 0)
    UINT64 quadroCorte = 0;  // ultimo corte de cena: respostas enviadas antes dele sao da cena velha
    UINT64 cortes = 0;
    std::chrono::steady_clock::time_point tEnvio;
    double somaAtrasoQ = 0, somaAtrasoMs = 0;
    int chegadas = 0;
    void CopiaParaRede(ID3D12GraphicsCommandList* c, bool temMov)
    {
        rede.CopiaEntrada(c, fed);
        if (HistoricoRede())
            GuiasDaRede(c, temMov);
    }
    // guias do historico da propria rede para o envio deste quadro (Rede::Envia as repassa ao runtime): a rede viu o
    // quadro anterior se ele foi enviado, e o movimento do quadro e o caminho ate ele
    void GuiasDaRede(ID3D12GraphicsCommandList* c, bool temMov)
    {
        const D3D12_RESOURCE_STATES SRV =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if (zeraHistorico)
            rede.ZeraHistorico();
        rede.GaranteGuias(fed.w, fed.h);
        UINT modo = 2; // sem historico
        Tex* fonte = nullptr;
        if (jaEnviou && quadros == quadroEnvio + 1)
        {
            if (temMov)
                modo = 1, fonte = &fluxo.Movimento();
            else
                modo = 0; // sem movimento (modo imagem): parado
        }
        Tex* z = Exata() ? fluxo.DistanciaAtual() : nullptr;
        Tex& gm = rede.GuiaMov();
        Tex& gp = rede.GuiaProf();
        const auto antesF = fonte ? fonte->estado : D3D12_RESOURCE_STATE_COMMON;
        const auto antesZ = z ? z->estado : D3D12_RESOURCE_STATE_COMMON;
        const auto antesFed = fed.estado;
        if (fonte)
            fonte->Para(c, SRV);
        if (z)
            z->Para(c, SRV);
        fed.Para(c, SRV); // no lugar das fontes que faltam (o shader nao as le)
        gm.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gp.Para(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const UINT k[4] = {gm.w, gm.h, modo, z ? 1u : 0u};
        pGuias.Roda(c, desc, gpu.dev.Get(), {{fonte ? fonte->r.Get() : fed.r.Get(), fonte ? fonte->fmt : fed.fmt},
                                             {z ? z->r.Get() : fed.r.Get(), z ? z->fmt : fed.fmt}},
                    {{gm.r.Get(), gm.fmt}, {gp.r.Get(), gp.fmt}}, k, (gm.w + 7) / 8, (gm.h + 7) / 8);
        gm.Para(c, D3D12_RESOURCE_STATE_COMMON); // a fila da rede pega daqui
        gp.Para(c, D3D12_RESOURCE_STATE_COMMON);
        if (fonte)
            fonte->Para(c, antesF);
        if (z)
            z->Para(c, antesZ);
        fed.Para(c, antesFed);
        // o movimento vai sempre, tambem no modo 2 (NaN em tudo: o runtime descarta o historico em vez de usa-lo parado)
        rede.UsaGuias(true, z != nullptr);
    }
    void MarcaEnvio()
    {
        quadroEnvio = quadros;
        jaEnviou = true;
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

    // ---- memoria de video: o uso deste processo contra o orcamento que o Windows da a ele ----
    // So mostra e registra. Acima do orcamento (pico do jogo, jogo pesado) o Windows move para a RAM o que estiver menos
    // em uso; a rede segue rodando (antes ela era desligada de vez depois de 2 s acima do orcamento, e desligava em todo
    // pico). A parte do overlay (texturas dele) e a do resto (rede, runtime HIP, captura) aparecem separadas.
    double texListadas = -1;       // o total da ultima lista de texturas no registro
    std::string memoriaTexto;      // linha do painel
    std::string emergenciaMemoria; // aviso de VRAM acima do orcamento ("" = dentro)
    std::chrono::steady_clock::time_point marcaMemoria{}, registroMemoria{};
    void VigiaMemoria()
    {
        const auto agora = std::chrono::steady_clock::now();
        if (agora - marcaMemoria < std::chrono::milliseconds(250))
            return;
        marcaMemoria = agora;
        UINT64 uso = 0, orcamento = 0, naRam = 0;
        if (!gpu.Memoria(uso, orcamento, naRam))
            return;
        const double mb = 1024.0 * 1024.0, gb = mb * 1024.0;
        const double tex = double(Tex::BytesVivos());
        memoriaTexto = Fmt("VRAM do overlay %.2f de %.2f GB (orçamento): rede e runtime %.2f, texturas %.2f  ·  na RAM %.2f GB",
                           uso / gb, orcamento / gb, std::max(0.0, double(uso) - tex) / gb, tex / gb, naRam / gb);
        if (agora - registroMemoria >= std::chrono::seconds(5))
        {
            registroMemoria = agora;
            Log("memoria", Fmt("VRAM do processo %.0f MB (texturas do overlay %.0f MB), orcamento %.0f MB, memoria de video "
                               "na RAM %.0f MB",
                               uso / mb, tex / mb, orcamento / mb, naRam / mb));
            if (tex != texListadas)
            {
                texListadas = tex;
                Log("memoria", "texturas do overlay: " + Tex::ListaVivas());
            }
        }
        emergenciaMemoria = orcamento && uso > orcamento
                                ? Fmt("VRAM acima do orçamento (%.2f de %.2f GB): o Windows move o excedente para a RAM. "
                                      "A rede continua; se o fps cair, use a rede pequena ou um tamanho menor.",
                                      uso / gb, orcamento / gb)
                                : std::string();
    }
    UINT64 quadrosComRede = 0;
    Tex* ultimaEdicao = nullptr;

    void GravaSaida(const std::wstring& arq) { GravaTex(saida, arq, hdr ? 8 : 4); }

    void Painel_()
    {
        if (!comPainel)
            return;
        EstadoPainel e;
        e.placa = gpu.nome;
        e.redeOk = redeSolta || (redeOk && rede.Pronta()); // solta pelo desligar nao e falha: volta ao ligar
        e.fonte = cfg.fonte;
        e.estadoBuffer = estadoBuffer;
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
            // na captura de buffer os do jogo (o modulo conta); na de video os processados pela rede e os que vao a tela
            // com os quadros gerados (Ritmo 2-4)
            e.fps = buffer && fb.FpsJogo() > 0 ? fb.FpsJogo() : fps;
            e.fracaoRede = fracaoRede;
            e.alturaSessao = sessao.alturaRede;
            e.pequenaSessao = sessao.redePequena;
            e.passesSessao = sessao.passes;
            e.completaSessao = sessao.redeCompleta;
        }
        e.memoria = memoriaTexto;
        e.historicoRede.clear();
        if (temporalSessao == 1)
            e.historicoRede = rede.TemGuias() ? (Exata() ? "Em uso: com o movimento e a profundidade do jogo."
                                                         : "Em uso: com o movimento do fluxo óptico.")
                                              : "Ligado, mas o runtime não tem LmxxfNrSetHistoryGuides: o histórico fica "
                                                "parado (sem movimento). Recompile o runtime deste branch.";
        if ((cfg.temporalRede == 1) != (temporalSessao == 1))
            e.historicoRede += (e.historicoRede.empty() ? "" : " ") + std::string("A mudança vale ao reabrir o programa.");
        e.avisoMemoria = emergenciaMemoria;
        if (const double ms = rede.MsEmVoo(); ms > 2000.0)
            e.avisoMemoria = Fmt("A rede está há %.0f s sem terminar um quadro: falta VRAM (o jogo e o overlay passam da placa e a "
                                 "rede, que usa toda a memória dela em cada quadro, fica parada na RAM). Use a rede em 1080p ou "
                                 "menos, ou a rede pequena (vale ao religar).",
                                 ms / 1000.0) +
                             (emergenciaMemoria.empty() ? std::string() : " " + emergenciaMemoria);
        e.exe = Utf8(exeAlvo);
        painel.DicaBandeja(Ligado() && !exeAlvo.empty() ? L"DLSS 5 Overlay - " + exeAlvo : L"DLSS 5 Overlay - sem jogo");
        e.perfis.clear();
        for (const auto& n : Perfis()) // relida do disco so depois de uma mudanca
            e.perfis.push_back(Utf8(n));
        e.perfilJogo = Utf8(perfilJogo);
        e.perfilBase = Utf8(perfilBase);
        e.perfilAlterado = configArq.empty() && cfg.Texto() != textoSalvo;
        e.perfilExiste = !perfilJogo.empty() && Existe(PerfilArq(perfilJogo));
        e.historicoAtivo = temporalSessao == 1;
        e.avisoPastaJogo = avisoPastaJogo;
        e.formatoAgora = Ligado() ? TextoFormatos() : std::string();
        e.exeEscolhido = exeEscolhido.empty() ? std::string() : Utf8(exeEscolhido.substr(exeEscolhido.find_last_of(L"\\/") + 1));
        e.camadaVulkan = camadaVulkan;
        painel.DefineMouseJogo(buffer && fb.Conectada() && fb.JogoVivo() ? fb.Cab() : nullptr);
        const AcoesPainel a = painel.Quadro(cfg, e);
        // perfis (na ordem: salvar antes de carregar, para o "salvar e carregar")
        if (a.perfilSalvar)
            SalvaPerfil();
        if (a.carregarPadrao)
            CarregaDe(L"");
        if (!a.perfilCarregar.empty())
            CarregaDe(NomeValido(Largo(a.perfilCarregar)));
        if (!a.perfilApagar.empty())
            ApagaPerfil(NomeValido(Largo(a.perfilApagar)));
        if (!a.perfilRenomear.empty())
            RenomeiaPerfilJogo(NomeValido(Largo(a.perfilRenomear)));

        if (a.redefinir)
        {
            AplicaConfig(Config{});
            Log("app", "ajustes redefinidos para os padroes");
        }
        if (!a.exeNovo.empty())
            DefineExeEscolhido(a.exeNovo);
        if (a.exeEsquecer)
            DefineExeEscolhido(L"");
        if (a.camadaMudou)
            DefineCamada(a.camadaValor);
        if (a.ligar)
            Liga(a.ligar);
        if (a.desligar)
            Desliga();
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
    Leitura leituras[6];

    void IniciaGravacao()
    {
        if (gravando > 0 || !orig.r)
        {
            Log("gravacao", gravando > 0 ? "ja gravando" : "nada para gravar (ligue o overlay no jogo)");
            return;
        }
        const double porQuadro = double(fed.w) * fed.h * (8 + 8 + 8 + 4 + 2 + (Exata() ? 4 : 0));
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
                    Exata() ? ",\"dist\":\"R32F\"" : "", gravando);
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
        // captura de buffer: o modulo do jogo grava a cor antes e depois da edicao na mesma pasta -- o pedido e um arquivo
        // em %TEMP% com o pid do jogo
        if (buffer && pidBuffer)
        {
            wchar_t tmp[MAX_PATH], arq[MAX_PATH + 40];
            GetTempPathW(MAX_PATH, tmp);
            swprintf(arq, MAX_PATH + 40, L"%sDLSS5Ov.despejo.%lu", tmp, (unsigned long)pidBuffer);
            FILE* f = nullptr;
            _wfopen_s(&f, arq, L"wb");
            if (f)
            {
                const std::string u = Utf8(pastaGravacao);
                fwrite(u.data(), 1, u.size(), f);
                fclose(f);
            }
        }
        MessageBeep(MB_OK);
        Log("gravacao", Fmt("gravando %d quadros (rede %ux%u) em %s", gravando, fed.w, fed.h, Utf8(pastaGravacao).c_str()));
    }

    // copia as texturas do quadro (GPU ja ociosa) e entrega os bytes a thread de escrita
    void GravaQuadro(bool comRede, bool comMov, Tex* edicaoExibida)
    {
        // captura de buffer: a distancia radial do jogo (R32F) para medir oclusao e rastro fora do jogo
        Tex* zt = Exata() ? fluxo.DistanciaAtual() : nullptr;
        Tex* tex[6] = {&orig, &temporal.Crua(), edicaoExibida ? edicaoExibida : &temporal.Crua(), &fluxo.Movimento(),
                       &fluxo.Confianca(), zt ? zt : &fluxo.Confianca()};
        const UINT bpp[6] = {8, 8, 8, 4, 2, 4};
        const wchar_t* nomes[6] = {L"orig", L"crua", L"edicao", L"mov", L"conf", L"dist"};
        std::vector<int> quais = {0, 1, 2, 3, 4};
        if (zt)
            quais.push_back(5);
        Ck(al[0]->Reset(), "reset");
        Ck(cl[0]->Reset(al[0].Get(), nullptr), "list");
        for (const int i : quais)
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
        for (const int i : quais)
        {
            Leitura& L = leituras[i];
            std::vector<unsigned char> b(size_t(tex[i]->w) * tex[i]->h * bpp[i]);
            // sem edicao exibida (antes da 1a resposta, depois de um corte de cena) a "edicao" gravada e zero; antes
            // gravava a ultima crua, e as ferramentas mostravam na analise uma edicao velha que nao estava na tela
            if (i != 2 || edicaoExibida)
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
    bool teclasTeste = false, ignoraFoco = false;
    int cicloSeg = 0, cicloVezes = 0; // --cicla <s> <n>: desliga/liga o alvo n vezes, a cada s segundos, e registra a memoria
    HWND alvoCiclo = nullptr;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (!wcscmp(argv[i], L"--alvo"))
            alvoTitulo = argv[++i];
        else if (!wcscmp(argv[i], L"--teclas-teste"))
            teclasTeste = true; // testes com outro overlay aberto: atalhos Ctrl+Shift+F5..F10
        else if (!wcscmp(argv[i], L"--ignora-foco"))
            ignoraFoco = true; // testes com o PC em uso: roda sem a janela do jogo em primeiro plano
        else if (!wcscmp(argv[i], L"--cicla") && i + 2 < argc)
        {
            cicloSeg = _wtoi(argv[++i]);
            cicloVezes = _wtoi(argv[++i]);
        }
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
        else if (!wcscmp(argv[i], L"--cicla-formato"))
            app.ciclaFormato = _wtoi(argv[++i]);
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
    }
    for (int i = 1; i < argc; ++i)
        if (!wcscmp(argv[i], L"--sem-painel"))
            app.comPainel = false;
    if (!imagemArq.empty() || !app.pastaReproducao.empty())
        app.comPainel = false;
    // imagem e reproducao: testes deterministicos (o jogo nao espera a edicao, nenhum quadro e pulado)
    app.atraso = (!imagemArq.empty() || !app.pastaReproducao.empty()) ? 0 : -1;
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
                    {
                        app.cfg.redePequena = !app.cfg.redePequena;
                        app.IniciaSessao(); // como religar: recria a rede
                    }
                    const int alturaBase = app.cfg.alturaRede;
                    if (app.alternaAltura && (i / 4) % 2)
                        app.cfg.alturaRede = app.alternaAltura;
                    app.sessao.alturaRede = app.cfg.alturaRede; // o teste troca a altura sem religar
                    const auto t = std::chrono::steady_clock::now();
                    app.Quadro();
                    app.gpu.Espera(); // tempo de GPU (rede inclusa), nao so o de envio
                    if (app.alternaAltura)
                    {
                        UINT64 uso = 0, orcamento = 0, naRam = 0;
                        app.gpu.Memoria(uso, orcamento, naRam);
                        Log("imagem", Fmt("quadro %d altura %d: %.2f ms, VRAM %llu MB", i, app.cfg.alturaRede,
                                          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(),
                                          (unsigned long long)(uso >> 20)));
                        app.cfg.alturaRede = alturaBase;
                        app.sessao.alturaRede = alturaBase;
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
    UINT atalhos[6];
    for (int i = 0; i < 6; ++i)
    {
        atalhos[i] = teclasTeste ? UINT(VK_F5 + i) : teclas[i];
        RegisterHotKey(nullptr, i + 1, mods, atalhos[i]);
    }
    app.painel.DefineAtalhos(atalhos); // com o painel aberto as teclas nao chegam ao RegisterHotKey
    // excecao sem tratamento: a queda sai no registro, nao so como 0xc0000409 no Windows
    std::set_terminate([] {
        try
        {
            if (const auto e = std::current_exception())
                std::rethrow_exception(e);
            Log("erro", "encerrado sem excecao ativa (std::terminate)");
        }
        catch (const std::exception& x)
        {
            Log("erro", std::string("excecao sem tratamento: ") + x.what());
        }
        catch (...)
        {
            Log("erro", "excecao sem tratamento (tipo desconhecido)");
        }
        abort();
    });
    app.LeExeEscolhido();
    app.LeCamada();
    app.AplicaCamada(app.camadaVulkan); // tambem tira o registro que sobrou de uma queda
    if (!alvoTitulo.empty())
    {
        Busca b{alvoTitulo};
        EnumWindows(PorTitulo, reinterpret_cast<LPARAM>(&b));
        if (!b.achou)
            Log("teste", "janela nao encontrada: " + Utf8(alvoTitulo));
        app.Liga(b.achou);
        alvoCiclo = b.achou;
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
                    app.cfg.modo = (app.cfg.modo + 1) % 6;
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
        const bool foco = !app.Ligado() || app.quadrosTeste > 0 || ignoraFoco || app.JogoEmFoco();
        app.painel.DefineAutoOculto(app.Ligado() && !foco);
        app.AplicaTetoDoJogo();
        {
            static auto ultimaOrdem = clk::now();
            if ((app.cap.Ativa() || app.captApi) && foco && clk::now() - ultimaOrdem >= std::chrono::milliseconds(250))
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
                    if (!app.imagem && app.pastaReproducao.empty())
                        app.ProcuraJogo(); // o modulo ja no jogo: conecta sozinho
                    app.Painel_();
                }
                catch (const std::exception& e)
                {
                    Log("erro", std::string("painel: ") + e.what());
                }
            }
        }
        // --cicla: o liga/desliga do Ctrl+Shift+D no alvo, com a memoria do processo a cada troca (VRAM local, video na RAM, RAM
        // privada, texturas do overlay)
        if (cicloSeg > 0 && alvoCiclo)
        {
            static auto ultimoCiclo = clk::now();
            static int feitos = 0;
            if (clk::now() - ultimoCiclo >= std::chrono::seconds(cicloSeg))
            {
                ultimoCiclo = clk::now();
                if (feitos >= cicloVezes * 2 || !IsWindow(alvoCiclo))
                    break;
                if (app.Ligado())
                    app.Desliga();
                else
                    app.Liga(alvoCiclo);
                ++feitos;
                UINT64 uso = 0, orc = 0, naRam = 0;
                app.gpu.Memoria(uso, orc, naRam);
                PROCESS_MEMORY_COUNTERS_EX pm{};
                pm.cb = sizeof pm;
                GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm);
                Log("ciclo", Fmt("%d %s: VRAM %llu MB, video na RAM %llu MB, RAM privada %llu MB, texturas %llu MB", feitos,
                                 app.Ligado() ? "ligado" : "desligado", (unsigned long long)(uso >> 20), (unsigned long long)(naRam >> 20),
                                 (unsigned long long)(pm.PrivateUsage >> 20), (unsigned long long)(Tex::BytesVivos() >> 20)));
            }
        }
        if (!app.Ligado())        {
            app.ov.Mostra(false);
            if (app.quadrosTeste > 0 && !alvoTitulo.empty())
                break; // teste sem alvo
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
            continue;
        }
        // fora deste try, uma falha de GPU aqui (recriar a swapchain com o jogo caindo) derrubava o overlay sem registro
        try
        {
            if (!foco)
            {
                app.Pausa();
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
                continue;
            }
            app.Retoma();
            app.SegueJanela();
        }
        catch (const std::exception& e)
        {
            Log("erro", std::string("janela: ") + e.what());
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
            continue;
        }
        HANDLE ev = app.buffer ? app.fb.Evento() : app.captApi ? app.fa.Evento() : app.cap.EventoQuadro();
        MsgWaitForMultipleObjects(ev ? 1 : 0, ev ? &ev : nullptr, FALSE, 16, QS_ALLINPUT);
        if (app.cfg.fpsMax > 0)
        {
            const auto minimo = std::chrono::microseconds(1000000 / app.cfg.fpsMax);
            const auto agora = clk::now();
            if (agora - ultimo < minimo)
                EsperaPrecisa(minimo - (agora - ultimo));
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
                        if (app.rayRegen.saida.r)
                        {
                            app.GravaTex(app.rayRegen.sinal, app.dumpPasta + L"\\rr_sinal.f16", 8);
                            app.GravaTex(app.rayRegen.saida, app.dumpPasta + L"\\rr_saida.f16", 8);
                            app.GravaTex(app.rayRegen.movZ, app.dumpPasta + L"\\rr_mov.f16", 8);
                            if (app.rayRegen.depura.r)
                                app.GravaTex(app.rayRegen.depura, app.dumpPasta + L"\\rr_depura.f16", 8);
                            app.GravaTex(app.rayRegen.albedo, app.dumpPasta + L"\\rr_albedo.rgba", 4);
                            app.GravaTex(app.rayRegen.prof, app.dumpPasta + L"\\rr_prof.r32", 4);
                            app.GravaTex(app.rayRegen.normais, app.dumpPasta + L"\\rr_normais.rgb10", 4);
                        }
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
