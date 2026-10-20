#include "Gancho.h"
#include <algorithm>
#include <mutex>

namespace ov {

namespace {
std::mutex g_mx;
std::vector<EventoMouse> g_eventos;
POINT g_cursor{};
RECT g_area{};
bool g_esc = false;
std::atomic<ULONGLONG> g_batimento{0};
ULONGLONG g_ultimoSolta = 0;

bool Vivo() { return GetTickCount64() - g_batimento.load() < 1000; }

void Prende(POINT& p)
{
    p.x = std::clamp(p.x, g_area.left, std::max(g_area.left, g_area.right - 1));
    p.y = std::clamp(p.y, g_area.top, std::max(g_area.top, g_area.bottom - 1));
}

// O passo de cada evento e "ponto novo - cursor atual", e o ponto novo ja vem cortado pela area em que o jogo prende o
// cursor (ClipCursor). O cursor de verdade fica parado (os eventos sao engolidos) no centro dessa area, longe das
// bordas: assim nenhum passo e cortado. Jogo que prende num ponto (area minuscula): solta, no maximo 4x por segundo.
void Estaciona()
{
    RECT c{};
    GetClipCursor(&c);
    if (c.right - c.left < 64 || c.bottom - c.top < 64)
    {
        const ULONGLONG agora = GetTickCount64();
        if (agora - g_ultimoSolta < 250)
            return;
        g_ultimoSolta = agora;
        ClipCursor(nullptr);
        GetClipCursor(&c);
    }
    POINT p{};
    GetCursorPos(&p);
    const LONG mx = std::min<LONG>(48, (c.right - c.left) / 4), my = std::min<LONG>(48, (c.bottom - c.top) / 4);
    if (p.x < c.left + mx || p.x >= c.right - mx || p.y < c.top + my || p.y >= c.bottom - my)
        SetCursorPos((c.left + c.right) / 2, (c.top + c.bottom) / 2);
}

LRESULT CALLBACK ProcMouse(int code, WPARAM w, LPARAM l)
{
    if (code != HC_ACTION || !Vivo())
        return CallNextHookEx(nullptr, code, w, l);
    const auto* s = reinterpret_cast<const MSLLHOOKSTRUCT*>(l);
    std::lock_guard<std::mutex> lk(g_mx);
    EventoMouse e;
    switch (w)
    {
    case WM_MOUSEMOVE:
    {
        // o evento ainda nao moveu o cursor de verdade (e nao vai: e engolido): o passo e o ponto novo - o atual
        POINT atual{};
        GetCursorPos(&atual);
        g_cursor.x += s->pt.x - atual.x;
        g_cursor.y += s->pt.y - atual.y;
        Prende(g_cursor);
        e.tipo = EventoMouse::Move;
        e.x = g_cursor.x;
        e.y = g_cursor.y;
        break;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        e.tipo = EventoMouse::Botao, e.botao = 0, e.desce = w == WM_LBUTTONDOWN;
        break;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        e.tipo = EventoMouse::Botao, e.botao = 1, e.desce = w == WM_RBUTTONDOWN;
        break;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        e.tipo = EventoMouse::Botao, e.botao = 2, e.desce = w == WM_MBUTTONDOWN;
        break;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        e.tipo = EventoMouse::Botao, e.botao = HIWORD(s->mouseData) == XBUTTON1 ? 3 : 4, e.desce = w == WM_XBUTTONDOWN;
        break;
    case WM_MOUSEWHEEL:
        e.tipo = EventoMouse::Roda, e.roda = short(HIWORD(s->mouseData)) / float(WHEEL_DELTA);
        break;
    case WM_MOUSEHWHEEL:
        e.tipo = EventoMouse::Roda, e.rodaH = short(HIWORD(s->mouseData)) / float(WHEEL_DELTA);
        break;
    default:
        return 1;
    }
    if (g_eventos.size() < 4096)
        g_eventos.push_back(e);
    return 1; // engole: o jogo nao ve
}

LRESULT CALLBACK ProcTeclado(int code, WPARAM w, LPARAM l)
{
    const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(l);
    if (code == HC_ACTION && k->vkCode == VK_ESCAPE && Vivo())
    {
        // Esc fecha o painel; o jogo nao recebe (abriria o menu dele)
        if (w == WM_KEYDOWN || w == WM_SYSKEYDOWN)
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_esc = true;
        }
        return 1;
    }
    return CallNextHookEx(nullptr, code, w, l);
}
} // namespace

void Gancho::Liga(const RECT& area)
{
    if (Ligado())
        return;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_area = area;
        g_cursor = {(area.left + area.right) / 2, (area.top + area.bottom) / 2};
        g_eventos.clear();
        g_eventos.push_back({EventoMouse::Move, g_cursor.x, g_cursor.y});
        g_esc = false;
    }
    g_batimento = GetTickCount64();
    Estaciona(); // o cursor de verdade sai de perto das bordas da area em que o jogo o prende
    HANDLE pronto = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    t = std::thread([this, pronto] {
        MSG m;
        PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE); // cria a fila antes de avisar (Desliga posta WM_QUIT)
        idThread = GetCurrentThreadId();
        HHOOK hm = SetWindowsHookExW(WH_MOUSE_LL, ProcMouse, GetModuleHandleW(nullptr), 0);
        HHOOK ht = SetWindowsHookExW(WH_KEYBOARD_LL, ProcTeclado, GetModuleHandleW(nullptr), 0);
        SetEvent(pronto);
        while (GetMessageW(&m, nullptr, 0, 0) > 0)
            DispatchMessageW(&m);
        if (hm)
            UnhookWindowsHookEx(hm);
        if (ht)
            UnhookWindowsHookEx(ht);
    });
    WaitForSingleObject(pronto, 2000);
    CloseHandle(pronto);
}

void Gancho::Desliga()
{
    if (!Ligado())
        return;
    PostThreadMessageW(idThread.load(), WM_QUIT, 0, 0);
    t.join();
    idThread = 0;
    std::lock_guard<std::mutex> lk(g_mx);
    g_eventos.clear();
}

void Gancho::Area(const RECT& area)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (EqualRect(&area, &g_area))
        return;
    g_area = area;
    Prende(g_cursor);
}

std::vector<EventoMouse> Gancho::Coleta(POINT& cursor, bool& esc)
{
    g_batimento = GetTickCount64();
    Estaciona();
    std::vector<EventoMouse> v;
    std::lock_guard<std::mutex> lk(g_mx);
    v.swap(g_eventos);
    cursor = g_cursor;
    esc = g_esc;
    g_esc = false;
    return v;
}

} // namespace ov
