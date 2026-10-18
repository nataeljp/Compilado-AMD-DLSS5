#include "Overlay.h"

namespace ov {

static LRESULT CALLBACK ProcOverlay(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_NCHITTEST:
        return HTTRANSPARENT; // nenhum clique fica no overlay
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(h, m, w, l);
    }
}

bool Overlay::Cria(Gpu& g, HINSTANCE inst)
{
    gpu = &g;
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = ProcOverlay;
    wc.hInstance = inst;
    wc.lpszClassName = L"DLSS5OverlayJanela";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    // WS_EX_LAYERED + WS_EX_TRANSPARENT: cliques atravessam; NOREDIRECTIONBITMAP: so o conteudo do DirectComposition
    hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                               WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                           wc.lpszClassName, L"DLSS5 Overlay", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, inst, nullptr);
    if (!hwnd)
    {
        Log("overlay", Fmt("CreateWindowEx falhou: %lu", GetLastError()));
        return false;
    }
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    Ck(DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&dcomp)), "DCompositionCreateDevice");
    Ck(dcomp->CreateTargetForHwnd(hwnd, TRUE, &alvo), "CreateTargetForHwnd");
    Ck(dcomp->CreateVisual(&visual), "CreateVisual");
    Ck(alvo->SetRoot(visual.Get()), "SetRoot");
    return true;
}

void Overlay::OcultaDeGravacoes(bool sim)
{
    if (hwnd)
        SetWindowDisplayAffinity(hwnd, sim ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
}

void Overlay::Destroi()
{
    if (gpu)
        gpu->Espera();
    for (auto& b : buffers)
        b.Reset();
    visual.Reset();
    alvo.Reset();
    dcomp.Reset();
    sc.Reset();
    if (hwnd)
        DestroyWindow(hwnd);
    hwnd = nullptr;
}

void Overlay::CriaSwapchain(UINT w_, UINT h_)
{
    gpu->Espera();
    for (auto& b : buffers)
        b.Reset();
    if (sc)
    {
        Ck(sc->ResizeBuffers(2, w_, h_, DXGI_FORMAT_R8G8B8A8_UNORM, 0), "ResizeBuffers");
    }
    else
    {
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = w_;
        d.Height = h_;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        d.Scaling = DXGI_SCALING_STRETCH;
        ComPtr<IDXGISwapChain1> s1;
        Ck(gpu->fabrica->CreateSwapChainForComposition(gpu->fila.Get(), &d, nullptr, &s1),
           "CreateSwapChainForComposition");
        Ck(s1.As(&sc), "IDXGISwapChain3");
        Ck(visual->SetContent(sc.Get()), "SetContent");
        Ck(dcomp->Commit(), "Commit");
    }
    for (UINT i = 0; i < 2; ++i)
        Ck(sc->GetBuffer(i, IID_PPV_ARGS(&buffers[i])), "GetBuffer");
    w = w_;
    h = h_;
    Log("overlay", Fmt("swapchain %ux%u", w, h));
}

void Overlay::Posiciona(const RECT& r)
{
    const UINT nw = UINT(r.right - r.left), nh = UINT(r.bottom - r.top);
    if (nw != w || nh != h || !sc)
        CriaSwapchain(nw, nh);
    if (memcmp(&r, &ultimo, sizeof r) != 0)
    {
        SetWindowPos(hwnd, HWND_TOPMOST, r.left, r.top, int(nw), int(nh), SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        ultimo = r;
    }
}

void Overlay::Mostra(bool sim)
{
    if (sim == visivel)
        return;
    ShowWindow(hwnd, sim ? SW_SHOWNOACTIVATE : SW_HIDE);
    visivel = sim;
}

ID3D12Resource* Overlay::Buffer() { return sc ? buffers[sc->GetCurrentBackBufferIndex()].Get() : nullptr; }

void Overlay::Apresenta()
{
    if (sc)
        sc->Present(0, 0);
}

} // namespace ov
