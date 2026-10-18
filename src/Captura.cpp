#include "Captura.h"
#include <algorithm>
#include <dwmapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;

namespace ov {

struct Captura::Impl
{
    wdx::Direct3D11::IDirect3DDevice dispositivo{nullptr};
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession sessao{nullptr};
    winrt::Windows::Graphics::SizeInt32 tamanhoPool{};
    winrt::event_token token{};
    bool fechou = false;
};

Captura::Captura() : p(std::make_unique<Impl>()) {}
Captura::~Captura() { Para(); }

void Captura::Cria(Gpu& g)
{
    gpu = &g;
    evento = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // D3D11 na mesma placa do D3D12 (a textura e a cerca sao compartilhadas)
    ComPtr<IDXGIAdapter1> ad;
    Ck(g.fabrica->EnumAdapterByLuid(g.luid, IID_PPV_ARGS(&ad)), "EnumAdapterByLuid");
    ComPtr<ID3D11Device> d;
    ComPtr<ID3D11DeviceContext> c;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    Ck(D3D11CreateDevice(ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                         D3D11_SDK_VERSION, &d, nullptr, &c),
       "D3D11CreateDevice");
    Ck(d.As(&dev11), "ID3D11Device5");
    Ck(c.As(&ctx11), "ID3D11DeviceContext4");
    ComPtr<IDXGIDevice> dxgi;
    Ck(d.As(&dxgi), "IDXGIDevice");
    winrt::com_ptr<::IInspectable> insp;
    Ck(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), insp.put()), "CreateDirect3D11DeviceFromDXGIDevice");
    p->dispositivo = insp.as<wdx::Direct3D11::IDirect3DDevice>();
    // cerca compartilhada: o D3D11 sinaliza depois da copia, a fila D3D12 espera antes de ler
    Ck(g.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&cerca12)), "CreateFence(shared)");
    HANDLE h = nullptr;
    Ck(g.dev->CreateSharedHandle(cerca12.Get(), nullptr, GENERIC_ALL, nullptr, &h), "CreateSharedHandle(fence)");
    const HRESULT hr = dev11->OpenSharedFence(h, IID_PPV_ARGS(&cerca11));
    CloseHandle(h);
    Ck(hr, "OpenSharedFence");
}

bool Captura::RetanguloCliente(HWND h, RECT& r)
{
    RECT c{};
    if (!GetClientRect(h, &c))
        return false;
    POINT o{0, 0};
    if (!ClientToScreen(h, &o))
        return false;
    r = {o.x, o.y, o.x + (c.right - c.left), o.y + (c.bottom - c.top)};
    return r.right > r.left && r.bottom > r.top;
}

bool Captura::Inicia(HWND h, bool hdr_)
{
    Para();
    hdr = hdr_;
    comp11.Reset();
    comp12.Reset();
    cw = ch = 0;
    if (!wgc::GraphicsCaptureSession::IsSupported())
    {
        Log("captura", "Windows Graphics Capture nao suportado neste Windows");
        return false;
    }
    try
    {
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        Ck(interop->CreateForWindow(h, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                                    winrt::put_abi(p->item)),
           "CreateForWindow");
        p->tamanhoPool = p->item.Size();
        p->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            p->dispositivo,
            hdr ? wdx::DirectXPixelFormat::R16G16B16A16Float : wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
            p->tamanhoPool);
        HANDLE ev = evento;
        p->token = p->pool.FrameArrived([ev](auto&&, auto&&) { SetEvent(ev); });
        p->sessao = p->pool.CreateCaptureSession(p->item);
        try
        {
            p->sessao.IsCursorCaptureEnabled(false);
        }
        catch (...)
        {
        }
        try
        {
            p->sessao.IsBorderRequired(false); // Windows 11: sem a borda amarela
        }
        catch (...)
        {
        }
        p->fechou = false;
        p->item.Closed([this](auto&&, auto&&) { p->fechou = true; });
        p->sessao.StartCapture();
    }
    catch (const winrt::hresult_error& e)
    {
        Log("captura", "WGC: " + winrt::to_string(e.message()));
        Para();
        return false;
    }
    catch (const std::exception& e)
    {
        Log("captura", e.what());
        Para();
        return false;
    }
    alvo = h;
    wchar_t titulo[256] = {};
    GetWindowTextW(h, titulo, 256);
    Log("captura", Fmt("capturando \"%s\" (%dx%d, %s)", Utf8(titulo).c_str(), p->tamanhoPool.Width,
                       p->tamanhoPool.Height, hdr ? "HDR FP16 scRGB" : "SDR 8 bits"));
    return true;
}

void Captura::Para()
{
    if (p->sessao)
    {
        try
        {
            p->sessao.Close();
        }
        catch (...)
        {
        }
    }
    if (p->pool)
    {
        try
        {
            p->pool.FrameArrived(p->token);
            p->pool.Close();
        }
        catch (...)
        {
        }
    }
    p->sessao = nullptr;
    p->pool = nullptr;
    p->item = nullptr;
    alvo = nullptr;
}

bool Captura::Ativa() const { return p->sessao != nullptr && !p->fechou && alvo && IsWindow(alvo); }

bool Captura::RecriaCompartilhada(UINT w, UINT h)
{
    comp11.Reset();
    comp12.Reset();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = FormatoDx();
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    Ck(dev11->CreateTexture2D(&d, nullptr, &comp11), "CreateTexture2D(compartilhada)");
    ComPtr<IDXGIResource1> r;
    Ck(comp11.As(&r), "IDXGIResource1");
    HANDLE h_ = nullptr;
    Ck(r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h_),
       "CreateSharedHandle(tex)");
    const HRESULT hr = gpu->dev->OpenSharedHandle(h_, IID_PPV_ARGS(&comp12));
    CloseHandle(h_);
    Ck(hr, "OpenSharedHandle(tex)");
    cw = w;
    ch = h;
    return true;
}

bool Captura::Pega(Quadro& q)
{
    if (!Ativa())
        return false;
    wgc::Direct3D11CaptureFrame quadro{nullptr};
    try
    {
        // o mais novo: os anteriores so atrasariam
        for (auto f = p->pool.TryGetNextFrame(); f; f = p->pool.TryGetNextFrame())
            quadro = f;
        if (!quadro)
            return false;
        const auto cont = quadro.ContentSize();
        if (cont.Width != p->tamanhoPool.Width || cont.Height != p->tamanhoPool.Height)
        {
            p->tamanhoPool = cont;
            p->pool.Recreate(p->dispositivo,
                             hdr ? wdx::DirectXPixelFormat::R16G16B16A16Float
                                 : wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                             2, cont);
        }
        // a imagem da janela comeca nos limites visuais dela (DWMWA_EXTENDED_FRAME_BOUNDS); recorta a area cliente
        RECT vis{}, cli{};
        if (FAILED(DwmGetWindowAttribute(alvo, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof vis)) ||
            !RetanguloCliente(alvo, cli))
            return false;
        LONG x0 = cli.left - vis.left, y0 = cli.top - vis.top;
        LONG w = cli.right - cli.left, h = cli.bottom - cli.top;
        // o quadro ainda nao cobre a area cliente (minimizando, restaurando, redimensionando): espera o proximo
        if (IsIconic(alvo) || cont.Width < x0 + w - 2 || cont.Height < y0 + h - 2)
            return false;
        x0 = std::max<LONG>(0, std::min<LONG>(x0, cont.Width - 1));
        y0 = std::max<LONG>(0, std::min<LONG>(y0, cont.Height - 1));
        w = std::min<LONG>(w, cont.Width - x0);
        h = std::min<LONG>(h, cont.Height - y0);
        if (w < 16 || h < 16)
            return false;
        q.novoTamanho = false;
        if ((UINT)w != cw || (UINT)h != ch || !comp11)
        {
            RecriaCompartilhada((UINT)w, (UINT)h);
            q.novoTamanho = true;
        }
        auto acesso = quadro.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> src;
        Ck(acesso->GetInterface(IID_PPV_ARGS(&src)), "IDirect3DDxgiInterfaceAccess");
        D3D11_BOX caixa{(UINT)x0, (UINT)y0, 0, (UINT)(x0 + w), (UINT)(y0 + h), 1};
        ctx11->CopySubresourceRegion(comp11.Get(), 0, 0, 0, 0, src.Get(), 0, &caixa);
        Ck(ctx11->Signal(cerca11.Get(), ++valor), "Signal(D3D11)");
        ctx11->Flush();
        q.tex = comp12.Get();
        q.fmt = FormatoDx();
        q.w = cw;
        q.h = ch;
        q.cercaValor = valor;
        return true;
    }
    catch (const winrt::hresult_error& e)
    {
        Log("captura", "quadro: " + winrt::to_string(e.message()));
        return false;
    }
}

} // namespace ov
