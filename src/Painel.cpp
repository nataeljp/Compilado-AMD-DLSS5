#include "Painel.h"
#include "../externo/imgui/imgui.h"
#include "../externo/imgui/imgui_impl_dx12.h"
#include "../externo/imgui/imgui_impl_win32.h"
#include <algorithm>
#include <psapi.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace ov {

namespace {
UINT g_pendW = 0, g_pendH = 0;
bool g_fechar = false;
DescriptorHeapAllocator g_srvAlloc;

LRESULT CALLBACK ProcPainel(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l))
        return 1;
    switch (m)
    {
    case WM_SIZE:
        if (w != SIZE_MINIMIZED)
        {
            g_pendW = LOWORD(l);
            g_pendH = HIWORD(l);
        }
        return 0;
    case WM_CLOSE:
        g_fechar = true;
        return 0;
    case WM_SYSCOMMAND:
        if ((w & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

std::string NomeProcesso(HWND h)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    std::string r;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
    {
        wchar_t b[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, b, &n))
        {
            std::wstring s(b, n);
            r = Utf8(s.substr(s.find_last_of(L"\\/") + 1));
        }
        CloseHandle(p);
    }
    return r;
}
} // namespace

bool Painel::Cria(Gpu& g, HINSTANCE inst, const std::wstring& pastaIni)
{
    gpu = &g;
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = ProcPainel;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DLSS5OverlayPainel";
    RegisterClassExW(&wc);
    const UINT dpi = GetDpiForSystem();
    const int pw = MulDiv(460, dpi, 96), ph = MulDiv(760, dpi, 96);
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"DLSS5 Overlay", WS_OVERLAPPEDWINDOW, 80, 80, pw, ph, nullptr,
                           nullptr, inst, nullptr);
    if (!hwnd)
        return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 2;
    Ck(g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)), "rtvHeap");
    rtvPasso = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 64;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Ck(g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap)), "srvHeap(painel)");
    g_srvAlloc.Create(g.dev.Get(), srvHeap.Get());
    Ck(g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al)), "al(painel)");
    Ck(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(&cl)), "cl(painel)");
    cl->Close();
    RECT r{};
    GetClientRect(hwnd, &r);
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = UINT(std::max<LONG>(1, r.right));
    d.Height = UINT(std::max<LONG>(1, r.bottom));
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> s1;
    Ck(g.fabrica->CreateSwapChainForHwnd(g.fila.Get(), hwnd, &d, nullptr, nullptr, &s1), "swapchain(painel)");
    Ck(s1.As(&sc), "IDXGISwapChain3(painel)");
    g.fabrica->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    Redimensiona(d.Width, d.Height);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    iniUtf8 = Utf8(pastaIni + L"\\painel.ini");
    io.IniFilename = iniUtf8.c_str();
    ImGui::StyleColorsDark();
    const float esc = dpi / 96.f;
    ImGui::GetStyle().ScaleAllSizes(esc);
    io.FontGlobalScale = esc;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX12_InitInfo ii{};
    ii.Device = g.dev.Get();
    ii.CommandQueue = g.fila.Get();
    ii.NumFramesInFlight = 2;
    ii.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    ii.SrvDescriptorHeap = srvHeap.Get();
    ii.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* c,
                                 D3D12_GPU_DESCRIPTOR_HANDLE* gp) { g_srvAlloc.Alloc(c, gp); };
    ii.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE c,
                                D3D12_GPU_DESCRIPTOR_HANDLE gp) { g_srvAlloc.Free(c, gp); };
    ImGui_ImplDX12_Init(&ii);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    AtualizaJanelas();
    return true;
}

void Painel::Destroi()
{
    if (!hwnd)
        return;
    gpu->Espera();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    for (auto& b : bb)
        b.Reset();
    sc.Reset();
    DestroyWindow(hwnd);
    hwnd = nullptr;
}

void Painel::Redimensiona(UINT nw, UINT nh)
{
    gpu->Espera();
    for (auto& b : bb)
        b.Reset();
    if (w != 0)
        Ck(sc->ResizeBuffers(2, nw, nh, DXGI_FORMAT_R8G8B8A8_UNORM, 0), "ResizeBuffers(painel)");
    for (UINT i = 0; i < 2; ++i)
    {
        Ck(sc->GetBuffer(i, IID_PPV_ARGS(&bb[i])), "GetBuffer(painel)");
        auto hnd = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        hnd.ptr += SIZE_T(i) * rtvPasso;
        gpu->dev->CreateRenderTargetView(bb[i].Get(), nullptr, hnd);
    }
    w = nw;
    h = nh;
}

void Painel::AtualizaJanelas()
{
    janelas.clear();
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto* v = reinterpret_cast<std::vector<JanelaLista>*>(l);
            if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) || (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
                return TRUE;
            wchar_t t[256] = {};
            if (GetWindowTextW(h, t, 256) <= 0)
                return TRUE;
            RECT r{};
            GetClientRect(h, &r);
            if (r.right < 320 || r.bottom < 200)
                return TRUE;
            v->push_back({h, Utf8(t) + "  [" + NomeProcesso(h) + Fmt(", %dx%d]", r.right, r.bottom)});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&janelas));
    janelas.erase(std::remove_if(janelas.begin(), janelas.end(), [this](const JanelaLista& j) { return j.h == hwnd; }),
                  janelas.end());
    escolhida = janelas.empty() ? -1 : std::clamp(escolhida, 0, int(janelas.size()) - 1);
}

static bool Deslizador(const char* rot, float* v, float a, float b, const char* fmt, const char* dica)
{
    const bool m = ImGui::SliderFloat(rot, v, a, b, fmt);
    if (dica && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", dica);
    return m;
}

AcoesPainel Painel::Quadro(Config& cfg, const EstadoPainel& e)
{
    AcoesPainel a;
    if (g_fechar)
    {
        a.sair = true;
        return a;
    }
    if (IsIconic(hwnd))
        return a;
    if (g_pendW && g_pendH && (g_pendW != w || g_pendH != h))
        Redimensiona(g_pendW, g_pendH);
    g_pendW = g_pendH = 0;

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("DLSS5 Overlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    // ---- estado ----
    ImGui::TextDisabled("%s", Utf8(e.placa).c_str());
    if (e.alvo.empty())
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Nenhuma janela capturada (Ctrl+Alt+D no jogo)");
    else
    {
        ImGui::TextColored(ImVec4(0.4f, 1, 0.5f, 1), "Capturando: %s", Utf8(e.alvo).c_str());
        ImGui::Text("Janela %ux%u  |  rede %ux%u  |  %.1f ms  |  %.0f quadros/s", e.janelaW, e.janelaH, e.redeW,
                    e.redeH, e.msMedio, e.fps);
    }
    if (!e.redeOk)
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Rede indisponivel: so o quadro original (ver overlay.jsonl)");

    // ---- janela ----
    if (ImGui::CollapsingHeader("Janela do jogo", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const char* atual = escolhida >= 0 ? janelas[escolhida].nome.c_str() : "(nenhuma)";
        if (ImGui::BeginCombo("##janela", atual))
        {
            for (int i = 0; i < int(janelas.size()); ++i)
                if (ImGui::Selectable(janelas[i].nome.c_str(), i == escolhida))
                    escolhida = i;
            ImGui::EndCombo();
        }
        if (ImGui::IsItemClicked())
            AtualizaJanelas();
        if (ImGui::Button("Ligar") && escolhida >= 0)
            a.ligar = janelas[escolhida].h;
        ImGui::SameLine();
        if (ImGui::Button("Desligar"))
            a.desligar = true;
        ImGui::SameLine();
        if (ImGui::Button("Atualizar lista"))
            AtualizaJanelas();
        ImGui::TextDisabled("Jogo em janela ou tela cheia sem borda. Ctrl+Alt+D liga no jogo em foco.");
    }

    // ---- exibicao ----
    if (ImGui::CollapsingHeader("Exibicao", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const char* modos[] = {"Rede", "Original", "Comparacao (lado a lado)", "So a edicao da rede", "Movimento simulado"};
        ImGui::Combo("Modo (Ctrl+Alt+N)", &cfg.modo, modos, 5);
        if (cfg.modo == 2)
            Deslizador("Divisao", &cfg.divisao, 0.f, 1.f, "%.2f", nullptr);
        Deslizador("Forca da edicao", &cfg.forca, 0.f, 2.f, "%.2f",
                   "Quanto da mudanca da rede entra no quadro (1 = a da rede).");
        ImGui::SliderInt("FPS maximo", &cfg.fpsMax, 0, 240, cfg.fpsMax ? "%d" : "sem teto");
        if (ImGui::Checkbox("Ocultar o overlay de prints e gravacoes", &cfg.ocultaDeGravacoes))
            a.ocultaMudou = true;
    }

    // ---- rede ----
    if (ImGui::CollapsingHeader("Rede DLSS 5", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const int alturas[] = {720, 900, 1080, 1440, 2160};
        const char* nomes[] = {"720p", "900p", "1080p", "1440p", "2160p"};
        int ia = 2;
        for (int i = 0; i < 5; ++i)
            if (alturas[i] == cfg.alturaRede)
                ia = i;
        if (ImGui::Combo("Tamanho da rede", &ia, nomes, 5))
            cfg.alturaRede = alturas[ia];
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Altura em que a rede roda (limitada pela janela). Menor = mais rapido; a edicao e\n"
                              "ampliada sobre o quadro nativo, que mantem o proprio detalhe.");
        ImGui::Checkbox("Rede completa (71 blocos)", &cfg.rede.redeCompleta);
        ImGui::SliderInt("Passes", &cfg.rede.passes, 1, 3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Cada passe roda a rede de novo sobre a propria resposta: estilo mais forte, custo linear.");
        if (Deslizador("Estilo", &cfg.rede.estilo, 0.f, 4.f, "%.2f",
                       "0 = padrao da NVIDIA, 1 = Stellar Blade, 2 = mais forte."))
            a.zeraHistorico = true;
        ImGui::SeparatorText("Controles do primeiro bloco");
        bool mc = false;
        mc |= Deslizador("Tom", &cfg.rede.tom, -1.f, 4.f, "%.2f", "Reiluminacao (0 = luz mais proxima da NVIDIA).");
        mc |= Deslizador("Estrutura", &cfg.rede.estrutura, -1.f, 4.f, "%.2f", nullptr);
        mc |= Deslizador("Pele", &cfg.rede.pele, -1.f, 4.f, "%.2f", nullptr);
        mc |= Deslizador("Outro", &cfg.rede.outro, -1.f, 4.f, "%.2f", nullptr);
        if (ImGui::Button("Controles padrao"))
        {
            cfg.rede.tom = cfg.rede.estrutura = cfg.rede.pele = cfg.rede.outro = 1.f;
            mc = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Tom 0 + estilo 2"))
        {
            cfg.rede.tom = 0.f;
            cfg.rede.estilo = 2.f;
            mc = true;
        }
        if (mc)
            a.zeraHistorico = true;
        ImGui::SeparatorText("Decodificacao");
        Deslizador("Detalhe", &cfg.rede.detalhe, 0.f, 1.f, "%.2f", "Forca de detalhe da decodificacao do runtime.");
        Deslizador("Cor", &cfg.rede.cor, 0.f, 1.f, "%.2f", "Forca de cor da decodificacao do runtime.");
    }

    // ---- entradas simuladas / tempo ----
    if (ImGui::CollapsingHeader("Movimento e estabilidade", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::Checkbox("Movimento simulado (fluxo optico)", &cfg.movimento))
            a.zeraHistorico = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Vetores de movimento estimados entre os quadros capturados, sem buffer do jogo.");
        ImGui::BeginDisabled(!cfg.movimento);
        ImGui::SliderInt("Estabilidade (quadros)", &cfg.temporal.maxQuadros, 1, 16);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Quantos quadros a edicao da rede acumula (menos cintilacao; 1 = sem acumulo).");
        Deslizador("Rejeicao", &cfg.temporal.rejeicao, 2.f, 40.f, "%.1f",
                   "Quao rapido uma mudanca de cor derruba o acumulo (mais alto = menos fantasma).");
        ImGui::SliderInt("Ritmo da rede", &cfg.ritmo, 1, 4, cfg.ritmo == 1 ? "todo quadro" : "1 a cada %d");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A rede roda a cada N quadros; nos outros a ultima edicao segue o movimento.\n"
                              "Libera a GPU para o jogo.");
        ImGui::EndDisabled();
    }
    ImGui::Separator();
    if (ImGui::Button("Salvar configuracao"))
        a.salvar = true;
    ImGui::SameLine();
    if (ImGui::Button("Sair (Ctrl+Alt+Q)"))
        a.sair = true;
    ImGui::End();
    ImGui::Render();

    const UINT i = sc->GetCurrentBackBufferIndex();
    Ck(al->Reset(), "al(painel) reset");
    Ck(cl->Reset(al.Get(), nullptr), "cl(painel) reset");
    Barreira(cl.Get(), bb[i].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(i) * rtvPasso;
    const float fundo[4] = {0.08f, 0.08f, 0.09f, 1.f};
    cl->ClearRenderTargetView(rtv, fundo, 0, nullptr);
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* hs[] = {srvHeap.Get()};
    cl->SetDescriptorHeaps(1, hs);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl.Get());
    Barreira(cl.Get(), bb[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    Ck(cl->Close(), "cl(painel) close");
    ID3D12CommandList* l[] = {cl.Get()};
    gpu->fila->ExecuteCommandLists(1, l);
    sc->Present(0, 0);
    gpu->Espera();
    return a;
}

} // namespace ov
