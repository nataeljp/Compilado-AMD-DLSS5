#include "Abertura.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <dwmapi.h>
#include <objidl.h>
namespace Gdiplus { using std::max; using std::min; } // o projeto compila com NOMINMAX
#include <gdiplus.h>

namespace ov {

namespace {
std::mutex g_mx;
std::wstring g_etapa = L"Iniciando...";
float g_e = 1.f; // escala do DPI
std::chrono::steady_clock::time_point g_t0;
ULONG_PTR g_gdip = 0;

void Arredondado(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r)
{
    const float d = r * 2;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

void Desenha(HDC dc, int W, int H)
{
    using namespace Gdiplus;
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Color(255, 22, 23, 26));
    const float e = g_e;
    // logo: "NR" em fundo vermelho (o icone do programa)
    const float ls = 64 * e, lx = 28 * e, ly = (H - ls) * 0.5f - 18 * e;
    {
        GraphicsPath p;
        Arredondado(p, lx, ly, ls, ls, 12 * e);
        LinearGradientBrush b(PointF(lx, ly), PointF(lx, ly + ls), Color(255, 214, 32, 44), Color(255, 160, 14, 26));
        g.FillPath(&b, &p);
        FontFamily ff(L"Bahnschrift");
        Font f(ff.IsAvailable() ? &ff : FontFamily::GenericSansSerif(), 30 * e, FontStyleBold, UnitPixel);
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        SolidBrush br(Color(255, 255, 255, 255));
        g.DrawString(L"NR", -1, &f, RectF(lx, ly + 1 * e, ls, ls), &sf, &br);
    }
    // titulo e etapa
    const float tx = lx + ls + 20 * e;
    {
        FontFamily ff(L"Bahnschrift");
        const FontFamily* fam = ff.IsAvailable() ? &ff : FontFamily::GenericSansSerif();
        Font ft(fam, 26 * e, FontStyleRegular, UnitPixel);
        SolidBrush branco(Color(255, 232, 232, 232));
        g.DrawString(L"DLSS 5", -1, &ft, PointF(tx, ly - 2 * e), &branco);
        Font fs(fam, 14 * e, FontStyleRegular, UnitPixel);
        SolidBrush cinza(Color(255, 150, 155, 162));
        std::wstring et;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            et = g_etapa;
        }
        g.DrawString(et.c_str(), -1, &fs, PointF(tx + 1 * e, ly + 34 * e), &cinza);
    }
    // a barra do boot do Windows XP: moldura arredondada e tres blocos correndo da esquerda para a direita
    const float bw = W - tx - 28 * e, bh = 16 * e, bx = tx, by = ly + ls + 6 * e;
    {
        GraphicsPath p;
        Arredondado(p, bx, by, bw, bh, 5 * e);
        SolidBrush fundo(Color(255, 32, 34, 38));
        g.FillPath(&fundo, &p);
        Pen borda(Color(255, 72, 76, 82), 1.2f * e);
        g.DrawPath(&borda, &p);
        Region clip(&p);
        g.SetClip(&clip);
        const float blk = 10 * e, gap = 4 * e, passo = blk + gap, curso = bw + 3 * passo;
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
        const float pos = float(std::fmod(t * 110.0 * e, double(curso)));
        for (int i = 0; i < 3; ++i)
        {
            const float x = bx + pos - i * passo - blk;
            GraphicsPath q;
            Arredondado(q, x, by + (bh - blk) * 0.5f, blk, blk, 2.5f * e);
            LinearGradientBrush v(PointF(x, by), PointF(x, by + bh), Color(255, 160, 225, 40), Color(255, 88, 140, 0));
            g.FillPath(&v, &q);
        }
        g.ResetClip();
    }
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_TIMER:
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
        HGDIOBJ ant = SelectObject(mem, bmp);
        Desenha(mem, r.right, r.bottom);
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ant);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        KillTimer(h, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
} // namespace

void Abertura::Mostra(HINSTANCE inst)
{
    if (t.joinable())
        return;
    HANDLE pronto = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    t = std::thread([this, inst, pronto] {
        Gdiplus::GdiplusStartupInput gi;
        Gdiplus::GdiplusStartup(&g_gdip, &gi, nullptr);
        g_t0 = std::chrono::steady_clock::now();
        g_e = GetDpiForSystem() / 96.f;
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = Proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_APPSTARTING);
        wc.lpszClassName = L"DLSS5OverlayAbertura";
        RegisterClassExW(&wc);
        const int W = int(420 * g_e), H = int(150 * g_e);
        POINT c{0, 0};
        GetCursorPos(&c);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY), &mi);
        const RECT a = mi.rcWork;
        HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"DLSS 5", WS_POPUP,
                                 (a.left + a.right - W) / 2, (a.top + a.bottom - H) / 2, W, H, nullptr, nullptr, inst,
                                 nullptr);
        if (h)
        {
            const DWM_WINDOW_CORNER_PREFERENCE canto = DWMWCP_ROUND;
            DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &canto, sizeof canto);
            SetTimer(h, 1, 16, nullptr);
            ShowWindow(h, SW_SHOWNOACTIVATE);
            UpdateWindow(h);
        }
        hwnd = h;
        SetEvent(pronto);
        MSG m;
        while (h && GetMessageW(&m, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        hwnd = nullptr;
        Gdiplus::GdiplusShutdown(g_gdip);
    });
    WaitForSingleObject(pronto, 2000); // a janela existe antes de a thread principal comecar o trabalho pesado
    CloseHandle(pronto);
}

void Abertura::Etapa(const wchar_t* texto)
{
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_etapa = texto;
    }
    if (HWND h = hwnd.load())
        InvalidateRect(h, nullptr, FALSE);
}

void Abertura::Fecha()
{
    if (HWND h = hwnd.load())
        PostMessageW(h, WM_CLOSE, 0, 0);
    if (t.joinable())
        t.join();
}

} // namespace ov
