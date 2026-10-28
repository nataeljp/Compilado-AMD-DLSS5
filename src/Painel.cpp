#include "Painel.h"
#include "../externo/imgui/imgui.h"
#include "../externo/imgui/imgui_impl_dx12.h"
#include "../externo/imgui/imgui_impl_win32.h"
#include <algorithm>
#include <cmath>
#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <windowsx.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace ov {

namespace {
UINT g_pendW = 0, g_pendH = 0;
bool g_fechar = false;
// bandeja do sistema: o que o usuario pediu pelo icone (lido no laco principal, mesmo com o painel escondido)
constexpr UINT kMsgBandeja = WM_APP + 7;
enum { kBandejaNada, kBandejaAlterna, kBandejaDesliga, kBandejaSair };
int g_bandeja = kBandejaNada;
bool g_capturando = false; // habilita "Desligar overlay" no menu
UINT g_msgTaskbarCreated = 0;
bool g_recriaBandeja = false;
DescriptorHeapAllocator g_srvAlloc;
ImFont* g_texto = nullptr;  // Bahnschrift (a mais proxima da fonte da NVIDIA que vem no Windows)
ImFont* g_secao = nullptr;  // Segoe UI Semibold, maiusculas espacadas
float g_tTexto = 15.f, g_tSecao = 12.5f, g_tTitulo = 26.f, g_e = 1.f;

// cores do painel da NVIDIA
const ImU32 kVerde = IM_COL32(118, 185, 0, 255);
const ImU32 kVerdeEscuro = IM_COL32(32, 44, 10, 255);
const ImU32 kTrilho = IM_COL32(58, 62, 67, 255);
const ImU32 kTexto = IM_COL32(228, 228, 228, 255);
const ImU32 kTexto2 = IM_COL32(150, 155, 162, 255);
const ImU32 kLinha = IM_COL32(44, 47, 52, 255);
const ImU32 kBotao = IM_COL32(42, 45, 50, 255);
const ImU32 kBotaoHover = IM_COL32(54, 58, 64, 255);

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
    case kMsgBandeja:
        switch (LOWORD(l))
        {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            g_bandeja = kBandejaAlterna;
            return 0;
        case WM_CONTEXTMENU:
        {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, kBandejaAlterna, L"Mostrar / esconder painel\tCtrl+Shift+P");
            AppendMenuW(m, MF_STRING | (g_capturando ? 0 : MF_GRAYED), kBandejaDesliga, L"Desligar overlay\tCtrl+Shift+D");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, kBandejaSair, L"Sair\tCtrl+Shift+Q");
            SetMenuDefaultItem(m, kBandejaAlterna, FALSE);
            SetForegroundWindow(h); // sem isso o menu nao fecha ao clicar fora
            const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN,
                                           GET_X_LPARAM(w), GET_Y_LPARAM(w), 0, h, nullptr);
            PostMessageW(h, WM_NULL, 0, 0);
            DestroyMenu(m);
            if (cmd)
                g_bandeja = cmd;
            return 0;
        }
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((w & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    }
    if (g_msgTaskbarCreated && m == g_msgTaskbarCreated)
        g_recriaBandeja = true; // o Explorer reiniciou: o icone some e precisa voltar
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

std::string Fonte(const wchar_t* nome)
{
    wchar_t d[MAX_PATH] = {};
    GetWindowsDirectoryW(d, MAX_PATH);
    const std::wstring p = std::wstring(d) + L"\\Fonts\\" + nome;
    return GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES ? std::string() : Utf8(p);
}

void EstiloNvidia(float e)
{
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(22 * e, 18 * e);
    s.FramePadding = ImVec2(10 * e, 7 * e);
    s.ItemSpacing = ImVec2(8 * e, 8 * e);
    s.WindowRounding = 0;
    s.FrameRounding = 6 * e;
    s.PopupRounding = 6 * e;
    s.GrabRounding = 10 * e;
    s.ScrollbarSize = 8 * e;
    s.ScrollbarRounding = 4 * e;
    s.WindowBorderSize = 0;
    s.FrameBorderSize = 0;
    ImVec4* c = s.Colors;
    auto rgb = [](int r, int g, int b, float a = 1.f) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); };
    c[ImGuiCol_Text] = rgb(228, 228, 228);
    c[ImGuiCol_TextDisabled] = rgb(110, 114, 120);
    c[ImGuiCol_WindowBg] = rgb(22, 23, 26);
    c[ImGuiCol_PopupBg] = rgb(30, 32, 36);
    c[ImGuiCol_Border] = rgb(44, 47, 52);
    c[ImGuiCol_FrameBg] = rgb(42, 45, 50);
    c[ImGuiCol_FrameBgHovered] = rgb(54, 58, 64);
    c[ImGuiCol_FrameBgActive] = rgb(60, 64, 70);
    c[ImGuiCol_Button] = rgb(42, 45, 50);
    c[ImGuiCol_ButtonHovered] = rgb(54, 58, 64);
    c[ImGuiCol_ButtonActive] = rgb(32, 44, 10);
    c[ImGuiCol_Header] = rgb(42, 45, 50);
    c[ImGuiCol_HeaderHovered] = rgb(54, 58, 64);
    c[ImGuiCol_HeaderActive] = rgb(32, 44, 10);
    c[ImGuiCol_CheckMark] = rgb(118, 185, 0);
    c[ImGuiCol_SliderGrab] = rgb(118, 185, 0);
    c[ImGuiCol_SliderGrabActive] = rgb(140, 210, 20);
    c[ImGuiCol_Separator] = rgb(44, 47, 52);
    c[ImGuiCol_ScrollbarBg] = rgb(22, 23, 26);
    c[ImGuiCol_ScrollbarGrab] = rgb(60, 64, 70);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(80, 85, 92);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(118, 185, 0);
}

// ---- widgets no desenho do painel da NVIDIA ----
int BytesUtf8(unsigned char c) { return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4; }

// titulo de secao: maiusculas espacadas, uma linha fina antes
void Secao(const char* t, bool linha = true)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(0, 6 * g_e));
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (linha)
    {
        dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y), kLinha, 1.f);
        p.y += 12 * g_e;
    }
    float x = p.x;
    for (const char* c = t; *c;)
    {
        const int n = BytesUtf8((unsigned char)*c);
        dl->AddText(g_secao, g_tSecao, ImVec2(x, p.y), kTexto, c, c + n);
        x += g_secao->CalcTextSizeA(g_tSecao, FLT_MAX, 0, c, c + n).x + 1.8f * g_e;
        c += n;
    }
    ImGui::Dummy(ImVec2(0, (linha ? 12 * g_e : 0) + g_tSecao + 6 * g_e));
}

void Dica(const char* d)
{
    if (d && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", d);
}

// valor sendo digitado (clique no numero de um deslizador): qual, o texto e se o campo ainda precisa do foco
ImGuiID g_editando = 0;
char g_textoEdicao[32] = {};
bool g_focarEdicao = false;

// seta de voltar (circulo aberto com a ponta no alto, apontando para a esquerda): o "volta ao padrao" de cada ajuste
void SetaVolta(ImDrawList* dl, ImVec2 c, float r, ImU32 cor, float esp)
{
    const float pi = 3.14159265f, topo = -pi * 0.5f;
    dl->PathArcTo(c, r, topo + 0.35f, topo + pi * 1.55f, 22);
    dl->PathStroke(cor, 0, esp);
    const ImVec2 p(c.x, c.y - r), dir(-1.f, 0.f), rad(0.f, -1.f);
    const float s = r * 0.75f;
    dl->AddTriangleFilled(ImVec2(p.x + dir.x * s, p.y + dir.y * s), ImVec2(p.x + rad.x * s * 0.7f, p.y + rad.y * s * 0.7f),
                          ImVec2(p.x - rad.x * s * 0.7f, p.y - rad.y * s * 0.7f), cor);
}

// rotulo | trilho verde com botao redondo | valor | seta de volta ao padrao. cor: trilho e botao nessa cor (0 = verde).
// Clique no valor: digita o numero (Enter aplica, Esc cancela; fora da faixa vai para o limite). A seta (ou duplo clique
// no trilho) volta ao padrao; apagada quando o valor ja e o padrao, ausente sem padrao.
// Ajuste fino: Shift + arrastar anda 1/10 do normal; a roda do mouse sobre o trilho soma um passo (a penultima casa do
// formato, ex. 0,01 em "%.3f") e, com Shift, a ultima (0,001)
bool Deslizador(const char* rot, float* v, float a, float b, const char* fmt, const char* dica, float padrao = NAN,
                ImU32 cor = 0)
{
    ImGui::PushID(rot);
    const float larg = ImGui::GetContentRegionAvail().x;
    const float colIco = 22 * g_e, colRot = larg * 0.38f, colVal = 58 * g_e, raio = 9 * g_e, alt = 26 * g_e;
    const float trilho = larg - colRot - colVal - colIco - 10 * g_e;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float a1 = ImGui::GetStyle().Alpha;
    auto alfa = [&](ImU32 c) { return (c & 0x00FFFFFF) | (ImU32(((c >> 24) & 0xFF) * a1) << 24); };
    // rotulo: o que nao cabe antes do trilho vira "..." (o nome inteiro aparece na dica, sobre o rotulo ou o trilho)
    const float maxRot = colRot - 6 * g_e;
    const char* fimRot = rot + strlen(rot);
    const bool cortado = g_texto->CalcTextSizeA(g_tTexto, FLT_MAX, 0, rot).x > maxRot;
    if (cortado)
    {
        const float lp = g_texto->CalcTextSizeA(g_tTexto, FLT_MAX, 0, "...").x;
        fimRot = rot;
        for (const char* c = rot; *c;)
        {
            const char* prox = c + BytesUtf8((unsigned char)*c);
            if (g_texto->CalcTextSizeA(g_tTexto, FLT_MAX, 0, rot, prox).x + lp > maxRot)
                break;
            fimRot = c = prox;
        }
    }
    const ImVec2 pRot(p0.x, p0.y + (alt - g_tTexto) * 0.5f);
    dl->AddText(g_texto, g_tTexto, pRot, alfa(kTexto2), rot, fimRot);
    if (cortado)
        dl->AddText(g_texto, g_tTexto, ImVec2(pRot.x + g_texto->CalcTextSizeA(g_tTexto, FLT_MAX, 0, rot, fimRot).x, pRot.y),
                    alfa(kTexto2), "...");
    const std::string dicaRot = cortado ? std::string(rot) + (dica ? std::string("\n\n") + dica : std::string()) : std::string();
    if (cortado && ImGui::IsMouseHoveringRect(p0, ImVec2(p0.x + colRot, p0.y + alt)) && ImGui::IsWindowHovered())
        ImGui::SetTooltip("%s", dicaRot.c_str());
    ImGui::SetCursorScreenPos(ImVec2(p0.x + colRot, p0.y));
    ImGui::InvisibleButton("##t", ImVec2(trilho, alt));
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY); // a roda sobre o trilho ajusta o valor, nao rola o painel
    const float x0 = p0.x + colRot + raio, x1 = p0.x + colRot + trilho - raio;
    const ImGuiIO& io = ImGui::GetIO();
    // casas decimais do formato ("%.3f" -> 3, "%+.2f" -> 2, "%.0f" -> 0)
    int casas = 0;
    if (const char* ponto = strchr(fmt, '.'))
        casas = std::max(0, atoi(ponto + 1));
    bool mudou = false;
    if (ImGui::IsItemActive())
    {
        float nv;
        if (io.KeyShift)
            nv = std::clamp(*v + io.MouseDelta.x / std::max(x1 - x0, 1.f) * (b - a) * 0.1f, a, b);
        else
            nv = a + std::clamp((io.MousePos.x - x0) / std::max(x1 - x0, 1.f), 0.f, 1.f) * (b - a);
        if (nv != *v)
            *v = nv, mudou = true;
    }
    else if (ImGui::IsItemHovered() && io.MouseWheel != 0.f)
    {
        const float passo = casas > 0 ? std::pow(10.f, -float(io.KeyShift ? casas : casas - 1)) : 1.f;
        const float q = std::pow(10.f, float(casas));
        const float nv = std::clamp(std::round((*v + io.MouseWheel * passo) * q) / q, a, b);
        if (nv != *v)
            *v = nv, mudou = true;
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && !std::isnan(padrao))
        *v = padrao, mudou = true;
    Dica(cortado ? dicaRot.c_str() : dica);
    const bool foco = ImGui::IsItemHovered() || ImGui::IsItemActive();
    const float t = std::clamp((*v - a) / (b - a), 0.f, 1.f), xk = x0 + t * (x1 - x0), y = p0.y + alt * 0.5f;
    dl->AddRectFilled(ImVec2(x0 - raio * 0.5f, y - 2.5f * g_e), ImVec2(x1 + raio * 0.5f, y + 2.5f * g_e), alfa(kTrilho),
                      3 * g_e);
    const ImU32 corTrilho = cor ? cor : kVerde;
    dl->AddRectFilled(ImVec2(x0 - raio * 0.5f, y - 2.5f * g_e), ImVec2(xk, y + 2.5f * g_e), alfa(corTrilho), 3 * g_e);
    dl->AddCircleFilled(ImVec2(xk, y), raio + (foco ? 1.5f * g_e : 0.f), alfa(corTrilho), 24);
    dl->AddCircle(ImVec2(xk, y), raio * 0.55f, alfa(IM_COL32(20, 30, 4, 255)), 24, 2.f * g_e);

    // valor: clique para digitar
    const ImVec2 pv(p0.x + colRot + trilho + 6 * g_e, p0.y);
    const ImGuiID idValor = ImGui::GetID("##valor");
    const bool numerico = strchr(fmt, '%') != nullptr; // "Windows" (Branco em 0) e afins: texto fixo, nao digita
    if (g_editando == idValor)
    {
        ImGui::SetCursorScreenPos(pv);
        ImGui::SetNextItemWidth(colVal);
        if (g_focarEdicao)
        {
            ImGui::SetKeyboardFocusHere();
            g_focarEdicao = false;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4 * g_e, (alt - g_tTexto) * 0.5f));
        const bool enter = ImGui::InputText("##valor", g_textoEdicao, sizeof g_textoEdicao,
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll |
                                                ImGuiInputTextFlags_CharsScientific);
        ImGui::PopStyleVar();
        const bool saiu = ImGui::IsItemDeactivated();
        if (enter || (saiu && !ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        {
            // virgula ou ponto como separador decimal
            for (char* c = g_textoEdicao; *c; ++c)
                if (*c == ',')
                    *c = '.';
            char* fim = nullptr;
            const float lido = strtof(g_textoEdicao, &fim);
            if (fim != g_textoEdicao && std::isfinite(lido))
            {
                const float nv = std::clamp(lido, a, b);
                if (nv != *v)
                    *v = nv, mudou = true;
            }
            g_editando = 0;
        }
        else if (saiu)
            g_editando = 0; // Esc: fica o valor de antes
    }
    else
    {
        char buf[32];
        snprintf(buf, sizeof buf, fmt, *v);
        ImGui::SetCursorScreenPos(pv);
        ImGui::InvisibleButton("##valorBotao", ImVec2(colVal, alt));
        const bool hovV = ImGui::IsItemHovered() && numerico;
        if (numerico && ImGui::IsItemClicked())
        {
            snprintf(g_textoEdicao, sizeof g_textoEdicao, "%.*f", casas, *v);
            g_editando = idValor;
            g_focarEdicao = true;
        }
        if (hovV)
            ImGui::SetTooltip("Clique para digitar o valor (%g a %g)", a, b);
        const float tw = g_texto->CalcTextSizeA(g_tTexto, FLT_MAX, 0, buf).x;
        const ImVec2 pt(pv.x + colVal - tw, p0.y + (alt - g_tTexto) * 0.5f);
        dl->AddText(g_texto, g_tTexto, pt, alfa(hovV ? kVerde : kTexto), buf);
        if (hovV)
            dl->AddLine(ImVec2(pt.x, pt.y + g_tTexto + 1 * g_e), ImVec2(pt.x + tw, pt.y + g_tTexto + 1 * g_e),
                        alfa(kVerde), 1.f * g_e);
    }

    // volta ao padrao
    if (!std::isnan(padrao))
    {
        ImGui::SetCursorScreenPos(ImVec2(p0.x + larg - colIco, p0.y));
        ImGui::InvisibleButton("##padrao", ImVec2(colIco, alt));
        const float meio = casas > 0 ? 0.5f * std::pow(10.f, -float(casas)) : 0.5f;
        const bool noPadrao = std::fabs(*v - padrao) < meio;
        const bool hovP = ImGui::IsItemHovered() && !noPadrao;
        if (hovP && ImGui::IsItemClicked())
            *v = padrao, mudou = true;
        if (hovP)
        {
            char bp[32];
            snprintf(bp, sizeof bp, numerico ? fmt : "%g", padrao);
            ImGui::SetTooltip("Volta ao padrão (%s)", bp);
        }
        SetaVolta(dl, ImVec2(p0.x + larg - colIco * 0.5f, p0.y + alt * 0.5f), 6.f * g_e,
                  alfa(noPadrao ? kTrilho : (hovP ? kVerde : kTexto2)), 1.6f * g_e);
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + alt + 4 * g_e));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopID();
    return mudou;
}

bool DeslizadorInt(const char* rot, int* v, int a, int b, const char* fmt, const char* dica, float padrao = NAN)
{
    float f = float(*v);
    if (!Deslizador(rot, &f, float(a), float(b), "%.0f", dica, padrao))
        return false;
    const int n = std::clamp(int(std::lround(f)), a, b);
    const bool mudou = n != *v;
    *v = n;
    (void)fmt;
    return mudou;
}

// caixa verde (marcada) ou contorno cinza, com o rotulo ao lado
bool Marca(const char* rot, bool* v, const char* dica = nullptr, bool maiusculo = false)
{
    ImGui::PushID(rot);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float lado = 18 * g_e, alt = 24 * g_e;
    ImFont* f = maiusculo ? g_secao : g_texto;
    const float tam = maiusculo ? g_tSecao + 1 : g_tTexto;
    const float tw = f->CalcTextSizeA(tam, FLT_MAX, 0, rot).x;
    ImGui::InvisibleButton("##m", ImVec2(lado + 10 * g_e + tw, alt));
    bool mudou = false;
    if (ImGui::IsItemClicked())
        *v = !*v, mudou = true;
    Dica(dica);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float a1 = ImGui::GetStyle().Alpha;
    auto alfa = [&](ImU32 c) { return (c & 0x00FFFFFF) | (ImU32(((c >> 24) & 0xFF) * a1) << 24); };
    const ImVec2 b0(p.x, p.y + (alt - lado) * 0.5f), b1(b0.x + lado, b0.y + lado);
    if (*v)
    {
        dl->AddRectFilled(b0, b1, alfa(kVerde), 3 * g_e);
        const ImVec2 pts[3] = {ImVec2(b0.x + lado * 0.24f, b0.y + lado * 0.52f), ImVec2(b0.x + lado * 0.43f, b0.y + lado * 0.70f),
                               ImVec2(b0.x + lado * 0.78f, b0.y + lado * 0.30f)};
        dl->AddPolyline(pts, 3, alfa(IM_COL32(16, 22, 4, 255)), 0, 2.4f * g_e);
    }
    else
        dl->AddRect(b0, b1, alfa(ImGui::IsItemHovered() ? kTexto : kTexto2), 3 * g_e, 0, 1.6f * g_e);
    if (maiusculo)
    {
        float x = b1.x + 10 * g_e;
        for (const char* c = rot; *c;)
        {
            const int n = BytesUtf8((unsigned char)*c);
            dl->AddText(f, tam, ImVec2(x, p.y + (alt - tam) * 0.5f), alfa(kTexto), c, c + n);
            x += f->CalcTextSizeA(tam, FLT_MAX, 0, c, c + n).x + 1.8f * g_e;
            c += n;
        }
    }
    else
        dl->AddText(f, tam, ImVec2(b1.x + 10 * g_e, p.y + (alt - tam) * 0.5f), alfa(kTexto), rot);
    ImGui::PopID();
    return mudou;
}

// botoes lado a lado de largura igual; o escolhido com borda e texto verdes (como Model A / B / C)
bool Segmentos(const char* id, const char* const* nomes, int n, int* sel, float alt = 0.f)
{
    ImGui::PushID(id);
    const float gap = 8 * g_e, larg = ImGui::GetContentRegionAvail().x;
    const float bw = (larg - gap * (n - 1)) / n, bh = alt > 0 ? alt : 36 * g_e;
    bool mudou = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float a1 = ImGui::GetStyle().Alpha;
    auto alfa = [&](ImU32 c) { return (c & 0x00FFFFFF) | (ImU32(((c >> 24) & 0xFF) * a1) << 24); };
    for (int i = 0; i < n; ++i)
    {
        if (i)
            ImGui::SameLine(0, gap);
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##s", ImVec2(bw, bh));
        if (ImGui::IsItemClicked() && *sel != i)
            *sel = i, mudou = true;
        const bool on = *sel == i, hov = ImGui::IsItemHovered();
        const ImVec2 q(p.x + bw, p.y + bh);
        dl->AddRectFilled(p, q, alfa(on ? kVerdeEscuro : (hov ? kBotaoHover : kBotao)), 6 * g_e);
        if (on)
            dl->AddRect(p, q, alfa(kVerde), 6 * g_e, 0, 1.6f * g_e);
        const ImVec2 ts = g_texto->CalcTextSizeA(g_tTexto, FLT_MAX, 0, nomes[i]);
        dl->AddText(g_texto, g_tTexto, ImVec2(p.x + (bw - ts.x) * 0.5f, p.y + (bh - ts.y) * 0.5f),
                    alfa(on ? kVerde : kTexto2), nomes[i]);
        ImGui::PopID();
    }
    ImGui::PopID();
    return mudou;
}

// tecla virtual -> tecla do ImGui (so as que um campo de texto usa)
ImGuiKey TeclaImGui(UINT vk)
{
    switch (vk)
    {
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_DELETE: return ImGuiKey_Delete;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_TAB: return ImGuiKey_Tab;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_SHIFT: case VK_LSHIFT: return ImGuiKey_LeftShift;
    case VK_RSHIFT: return ImGuiKey_RightShift;
    case VK_CONTROL: case VK_LCONTROL: return ImGuiKey_LeftCtrl;
    case VK_RCONTROL: return ImGuiKey_RightCtrl;
    case 'A': return ImGuiKey_A;
    case 'C': return ImGuiKey_C;
    case 'V': return ImGuiKey_V;
    case 'X': return ImGuiKey_X;
    case 'Z': return ImGuiKey_Z;
    default: return ImGuiKey_None;
    }
}

bool Botao(const char* rot, float largura = 0.f, bool destaque = false)
{
    if (destaque)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(kVerdeEscuro));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kVerde));
    }
    const bool r = ImGui::Button(rot, ImVec2(largura, 32 * g_e));
    if (destaque)
        ImGui::PopStyleColor(2);
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
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); // recursos\icone.ico ("NR" em fundo vermelho)
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"DLSS5OverlayPainel";
    RegisterClassExW(&wc);
    const UINT dpi = GetDpiForSystem();
    esc = dpi / 96.f;
    g_e = esc;
    // janela sem moldura encostada na direita (como a Game Bar), acima do overlay; fora da barra de tarefas
    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"DLSS 5", WS_POPUP, 0, 0,
                           MulDiv(420, dpi, 96), MulDiv(900, dpi, 96), nullptr, nullptr, inst, nullptr);
    if (!hwnd)
        return false;
    const DWM_WINDOW_CORNER_PREFERENCE canto = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &canto, sizeof canto);
    const BOOL escuro = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &escuro, sizeof escuro);
    Ancora(nullptr);
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
    g_tTexto = 15.f * esc;
    g_tSecao = 12.5f * esc;
    g_tTitulo = 28.f * esc;
    const std::string bahn = Fonte(L"bahnschrift.ttf"), segoe = Fonte(L"segoeui.ttf"), semi = Fonte(L"seguisb.ttf");
    const std::string& base = !bahn.empty() ? bahn : segoe;
    if (!base.empty())
        g_texto = io.Fonts->AddFontFromFileTTF(base.c_str(), g_tTexto);
    if (!g_texto)
        g_texto = io.Fonts->AddFontDefault();
    g_secao = !semi.empty() ? io.Fonts->AddFontFromFileTTF(semi.c_str(), g_tSecao) : g_texto;
    if (!g_secao)
        g_secao = g_texto;
    io.FontDefault = g_texto;
    EstiloNvidia(esc);
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
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    CriaBandeja();
    Aplica();
    AtualizaJanelas();
    return true;
}

void Painel::CriaBandeja()
{
    NOTIFYICONDATAW n{sizeof n};
    n.hWnd = hwnd;
    n.uID = 1;
    n.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    n.uCallbackMessage = kMsgBandeja;
    n.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wcsncpy_s(n.szTip, dicaBandeja.empty() ? L"DLSS 5 Overlay" : dicaBandeja.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_DELETE, &n);
    if (!Shell_NotifyIconW(NIM_ADD, &n))
        Log("painel", Fmt("icone da bandeja nao foi criado (%lu)", GetLastError()));
    else
        Log("painel", "icone na bandeja");
    n.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &n);
}

void Painel::DicaBandeja(const std::wstring& dica)
{
    if (!hwnd || dica == dicaBandeja)
        return;
    dicaBandeja = dica;
    NOTIFYICONDATAW n{sizeof n};
    n.hWnd = hwnd;
    n.uID = 1;
    n.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcsncpy_s(n.szTip, dica.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void Painel::Destroi()
{
    if (!hwnd)
        return;
    NOTIFYICONDATAW n{sizeof n};
    n.hWnd = hwnd;
    n.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &n);
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

void Painel::Ancora(HWND alvo)
{
    if (!hwnd)
        return;
    HMONITOR m = alvo ? MonitorFromWindow(alvo, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof mi};
    if (!GetMonitorInfoW(m, &mi))
        return;
    const RECT t = mi.rcWork;
    const int margem = int(16 * esc), larg = int(420 * esc);
    const int alt = std::min<int>(t.bottom - t.top - 2 * margem, int(1040 * esc));
    SetWindowPos(hwnd, HWND_TOPMOST, t.right - larg - margem, t.top + margem, larg, alt, SWP_NOACTIVATE);
}

void Painel::Aplica()
{
    if (!hwnd)
        return;
    const bool v = usuarioQuer && !autoOculto;
    if (v != (IsWindowVisible(hwnd) != FALSE))
    {
        if (v)
        {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        else // sem ativar outra janela (com ShowWindow o Windows ativava a proxima da fila: area de trabalho, barra)
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                         SWP_HIDEWINDOW | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    }
    // o mouse so fica com o painel enquanto ele esta na tela sobre o jogo (Alt+Tab o esconde e solta o mouse)
    const bool querGancho = v && semFoco;
    if (mouseJogo)
    {
        mouseJogo->painelAberto = querGancho ? 1u : 0u;
        if (querGancho)
        {
            // o que o jogo acumulou antes de abrir nao conta
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&mouseJogo->mouseDx), 0);
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&mouseJogo->mouseDy), 0);
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&mouseJogo->mouseRoda), 0);
            for (int i = 0; i < 3; ++i)
                desceVisto[i] = mouseJogo->botaoDesce[i], sobeVisto[i] = mouseJogo->botaoSobe[i];
            eventosAoAbrir = mouseJogo->mouseEventos;
            movesSemJogo = 0;
            brutoDx = brutoDy = 0;
        }
        else
        {
            Log("painel", Fmt("mouse do jogo nesta abertura: %u eventos, dx %ld dy %ld",
                              mouseJogo->mouseEventos - eventosAoAbrir, brutoDx, brutoDy));
        }
    }
    if (querGancho && !gancho.Ligado())
    {
        RECT r{};
        GetWindowRect(hwnd, &r);
        gancho.Liga(r);
    }
    else if (!querGancho && gancho.Ligado())
        gancho.Desliga();
}

void Painel::DefineMouseJogo(dlss5ov::Cabecalho* c)
{
    if (c && jogoSemBruta)
        c = nullptr; // ja visto: este jogo nao passa o mouse pela entrada bruta
    if (c == mouseJogo)
        return;
    if (mouseJogo)
        mouseJogo->painelAberto = 0;
    mouseJogo = c;
    if (c)
        for (int i = 0; i < 3; ++i)
            desceVisto[i] = c->botaoDesce[i], sobeVisto[i] = c->botaoSobe[i];
    gancho.EngoleMouse(c == nullptr);
}

// a janela em primeiro plano e o jogo (ou, sem jogo ligado, um programa qualquer que nao seja a area de trabalho, a
// barra de tarefas ou o proprio overlay): abrir o painel nao pode tirar o foco dela
bool Painel::JogoNaFrente() const
{
    HWND fg = GetForegroundWindow();
    if (!fg || fg == hwnd)
        return false;
    DWORD pf = 0;
    GetWindowThreadProcessId(fg, &pf);
    if (pf == GetCurrentProcessId())
        return false;
    if (jogo && IsWindow(jogo))
    {
        DWORD pj = 0;
        GetWindowThreadProcessId(jogo, &pj);
        if (pj == pf)
            return true;
    }
    wchar_t c[96] = {};
    GetClassNameW(fg, c, 96);
    for (const wchar_t* s : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"NotifyIconOverflowWindow",
                             L"TopLevelWindowForOverflowXamlIsland", L"Windows.UI.Core.CoreWindow", L"XamlExplorerHostIslandWindow"})
        if (!wcscmp(c, s))
            return false;
    return true;
}

// Traz `h` para o primeiro plano mesmo com outro processo (o jogo) em foco: o Windows recusa SetForegroundWindow de
// quem nao recebeu a ultima entrada; unir a fila de entrada da thread em foco a nossa durante a troca e o jeito
// suportado (sem tecla simulada, que o jogo veria).
static void Foca(HWND h)
{
    if (!h || !IsWindow(h))
        return;
    HWND fg = GetForegroundWindow();
    const DWORD tFg = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, tEu = GetCurrentThreadId();
    const bool une = tFg && tFg != tEu && AttachThreadInput(tEu, tFg, TRUE);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    if (une)
        AttachThreadInput(tEu, tFg, FALSE);
}

void Painel::Alterna()
{
    if (!hwnd)
        return;
    if (IsWindowVisible(hwnd) && (GetForegroundWindow() == hwnd || semFoco))
    {
        Esconde(true);
        return;
    }
    usuarioQuer = true;
    autoOculto = false; // pedido explicito (atalho, bandeja): vale mesmo com o foco fora do jogo
    semFoco = JogoNaFrente();
    Aplica();
    if (semFoco)
    {
        Log("painel", Fmt("aberto sem tirar o foco do jogo (mouse no painel; Esc ou Ctrl+Shift+P devolve)%s",
                          mouseJogo ? "; mouse pelo modulo do jogo" : "; mouse pelo Windows"));
        return;
    }
    Foca(hwnd);
    // o jogo prende o cursor na janela dele (ClipCursor) enquanto tem o foco; sem o foco, solta: garante
    ClipCursor(nullptr);
}

void Painel::Esconde(bool devolveFoco)
{
    usuarioQuer = false;
    Aplica();
    const bool estavaSemFoco = semFoco;
    semFoco = false;
    if (!avisouBandeja && devolveFoco && !estavaSemFoco) // sobre o jogo, a notificacao puxava o foco para a barra
    {
        avisouBandeja = true;
        NOTIFYICONDATAW n{sizeof n};
        n.hWnd = hwnd;
        n.uID = 1;
        n.uFlags = NIF_INFO;
        n.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
        wcsncpy_s(n.szInfoTitle, L"DLSS 5 continua rodando", _TRUNCATE);
        wcsncpy_s(n.szInfo, L"Painel: Ctrl+Shift+P ou o icone na bandeja. Para fechar: botao direito no icone > Sair.",
                  _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &n);
    }
    if (devolveFoco && jogo && IsWindow(jogo) && !IsIconic(jogo))
    {
        // aberto com foco: devolve; aberto sobre o jogo: so se algo tirou o foco dele ao esconder
        DWORD pj = 0, pf = 0;
        GetWindowThreadProcessId(jogo, &pj);
        if (HWND fg = GetForegroundWindow())
            GetWindowThreadProcessId(fg, &pf);
        if (!estavaSemFoco || pf != pj)
            Foca(jogo);
        else
            vigiaFocoAte = GetTickCount64() + 600;
    }
}

void Painel::DefineAutoOculto(bool sim)
{
    if (sim == autoOculto)
        return;
    autoOculto = sim;
    Aplica();
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

// perfil do jogo (como o ReShade), logo abaixo da escolha da janela: o seletor traz os ajustes de qualquer perfil (de
// qualquer jogo); Salvar grava sempre no perfil do jogo ligado (o nome do exe, ou o nome dado em Renomear). Carregar por
// cima de alteracoes nao salvas e apagar pedem confirmacao.
void Painel::SecaoPerfil(const EstadoPainel& e, AcoesPainel& a, float lc)
{
    const ImVec4 cinza = ImGui::ColorConvertU32ToFloat4(kTexto2), amarelo(1, 0.78f, 0.3f, 1);
    const float b3 = (lc - 16 * g_e) / 3, b2 = (lc - 8 * g_e) / 2;
    const bool comJogo = !e.exe.empty();
    auto carrega = [&](const std::string& n) {
        if (e.perfilAlterado)
        {
            confirma = 2;
            confirmaNome = n;
            abreConfirma = true;
        }
        else if (n.empty())
            a.carregarPadrao = true;
        else
            a.perfilCarregar = n;
    };

    ImGui::Dummy(ImVec2(0, 2 * g_e));
    ImGui::TextColored(cinza, "Perfil");
    const std::string atual = e.perfilBase.empty() ? "(padrão do programa)" : e.perfilBase;
    ImGui::SetNextItemWidth(lc);
    if (ImGui::BeginCombo("##perfil", atual.c_str()))
    {
        if (ImGui::Selectable("(padrão do programa)##padrao", e.perfilBase.empty()))
            carrega(std::string());
        for (const auto& n : e.perfis)
        {
            const std::string rot = n == e.perfilJogo ? n + "   · deste jogo" : n;
            if (ImGui::Selectable((rot + "##" + n).c_str(), n == e.perfilBase))
                carrega(n);
        }
        ImGui::EndCombo();
    }
    Dica("Carrega o perfil de qualquer jogo ou o padrao. Salvar grava so no perfil deste jogo.");
    // para onde vai o Salvar (o "alterado" numa linha propria: ao lado de uma linha que quebra ele ficava uma letra por linha)
    ImGui::PushTextWrapPos(0.f);
    if (comJogo)
        ImGui::TextColored(cinza, "Salva em: %s.json%s", e.perfilJogo.c_str(), e.perfilExiste ? "" : "  (novo)");
    else
        ImGui::TextColored(cinza, "Sem jogo: salva no padrão do programa (config.json)");
    ImGui::PopTextWrapPos();
    if (e.perfilAlterado)
        ImGui::TextColored(amarelo, "Alterado (não salvo)");
    if (Botao("Salvar", b3, e.perfilAlterado))
        a.perfilSalvar = true;
    Dica(comJogo ? "Grava no perfil deste jogo; ao sair ou trocar de jogo, os ajustes tambem vao para la."
                 : "Grava no padrao do programa (config.json), o ponto de partida de um jogo sem perfil.");
    ImGui::SameLine(0, 8 * g_e);
    ImGui::BeginDisabled(!comJogo);
    if (Botao("Renomear", b3))
    {
        renomeando = !renomeando;
        snprintf(nomeNovo, sizeof nomeNovo, "%s", e.perfilJogo.c_str());
    }
    Dica("Outro nome para o perfil deste jogo (o exe nem sempre tem o nome); o jogo segue ligado.");
    ImGui::EndDisabled();
    ImGui::SameLine(0, 8 * g_e);
    const std::string apagavel = !e.perfilBase.empty() ? e.perfilBase : std::string();
    ImGui::BeginDisabled(apagavel.empty());
    if (Botao("Apagar", b3))
    {
        confirma = 1;
        confirmaNome = apagavel;
        abreConfirma = true;
    }
    Dica("Manda o perfil escolhido no seletor para a Lixeira. Os ajustes em uso continuam na tela.");
    ImGui::EndDisabled();
    if (renomeando && comJogo)
    {
        ImGui::SetNextItemWidth(lc);
        const bool enter = ImGui::InputText("##nomeNovo", nomeNovo, sizeof nomeNovo, ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string novo = nomeNovo;
        const bool existe = novo != e.perfilJogo && std::find(e.perfis.begin(), e.perfis.end(), novo) != e.perfis.end();
        if (existe)
            ImGui::TextColored(amarelo, "Já existe um perfil com esse nome.");
        ImGui::BeginDisabled(novo.empty() || existe || novo == e.perfilJogo);
        if (Botao("OK", b2, true) || (enter && !novo.empty() && !existe && novo != e.perfilJogo))
        {
            a.perfilRenomear = novo;
            renomeando = false;
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, 8 * g_e);
        if (Botao("Cancelar", b2))
            renomeando = false;
    }

    // confirmacoes
    if (abreConfirma)
    {
        ImGui::OpenPopup("Confirmar");
        abreConfirma = false;
    }
    ImGui::SetNextWindowSize(ImVec2(lc, 0));
    if (ImGui::BeginPopupModal("Confirmar", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
    {
        const float lb = ImGui::GetContentRegionAvail().x;
        ImGui::PushTextWrapPos(0.f);
        if (confirma == 1)
            ImGui::Text("Mandar o perfil \"%s\" para a Lixeira?", confirmaNome.c_str());
        else
            ImGui::Text("Os ajustes em uso têm alterações não salvas. Carregar %s%s%s mesmo assim?",
                        confirmaNome.empty() ? "" : "\"", confirmaNome.empty() ? "o padrão do programa" : confirmaNome.c_str(),
                        confirmaNome.empty() ? "" : "\"");
        ImGui::PopTextWrapPos();
        bool fecha = false;
        if (confirma == 1 && Botao("Apagar", lb))
            a.perfilApagar = confirmaNome, fecha = true;
        if (confirma == 2)
        {
            bool carregar = false;
            // salvar primeiro: o app trata o salvar antes do carregar
            if (Botao(comJogo ? "Salvar neste jogo e carregar" : "Salvar no padrão e carregar", lb, true))
                a.perfilSalvar = true, carregar = true;
            if (Botao("Descartar e carregar", lb))
                carregar = true;
            if (carregar)
            {
                if (confirmaNome.empty())
                    a.carregarPadrao = true;
                else
                    a.perfilCarregar = confirmaNome;
                fecha = true;
            }
        }
        if (Botao("Cancelar", lb))
            fecha = true;
        if (fecha)
        {
            confirma = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

AcoesPainel Painel::Quadro(Config& cfg, const EstadoPainel& e)
{
    AcoesPainel a;
    g_capturando = !e.alvo.empty();
    if (g_recriaBandeja)
    {
        g_recriaBandeja = false;
        CriaBandeja();
    }
    if (g_bandeja != kBandejaNada)
    {
        const int b = g_bandeja;
        g_bandeja = kBandejaNada;
        if (b == kBandejaAlterna)
            Alterna();
        a.desligar = b == kBandejaDesliga;
        a.sair = b == kBandejaSair;
        if (a.desligar || a.sair)
            return a;
    }
    if (g_fechar)
    {
        g_fechar = false;
        Esconde(true); // fechar so esconde (Ctrl+Shift+P ou o icone da bandeja volta); sair e no menu da bandeja
        return a;
    }
    if (vigiaFocoAte)
    {
        // ao esconder, o Windows as vezes ativa a area de trabalho/barra logo depois: o foco volta para o jogo
        if (GetTickCount64() > vigiaFocoAte)
            vigiaFocoAte = 0;
        else if (HWND fg = GetForegroundWindow(); jogo && IsWindow(jogo) && fg != jogo)
        {
            wchar_t c[64] = {};
            GetClassNameW(fg, c, 64);
            if (!wcscmp(c, L"Progman") || !wcscmp(c, L"WorkerW") || !wcscmp(c, L"Shell_TrayWnd") || fg == hwnd)
            {
                Foca(jogo);
                vigiaFocoAte = 0;
            }
        }
    }
    if (IsIconic(hwnd) || !IsWindowVisible(hwnd))
        return a;
    if (g_pendW && g_pendH && (g_pendW != w || g_pendH != h))
        Redimensiona(g_pendW, g_pendH);
    g_pendW = g_pendH = 0;

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    {
        // aberto sobre o jogo: o mouse vem do gancho (o jogo nao ve) e o cursor e desenhado pelo proprio painel
        ImGuiIO& io = ImGui::GetIO();
        io.MouseDrawCursor = gancho.Ligado();
        if (gancho.Ligado())
        {
            RECT r{};
            GetWindowRect(hwnd, &r);
            gancho.Area(r);
            POINT cur{};
            bool esc = false;
            // mouse de dentro do jogo (captura de buffer): movimento bruto e cliques que o modulo tirou do jogo
            bool bruto = false;
            if (mouseJogo)
            {
                const LONG dx = InterlockedExchange(reinterpret_cast<volatile LONG*>(&mouseJogo->mouseDx), 0);
                const LONG dy = InterlockedExchange(reinterpret_cast<volatile LONG*>(&mouseJogo->mouseDy), 0);
                const LONG roda = InterlockedExchange(reinterpret_cast<volatile LONG*>(&mouseJogo->mouseRoda), 0);
                if (dx || dy)
                {
                    brutoDx += dx, brutoDy += dy;
                    ultimoBruto = GetTickCount64();
                    const POINT pb = gancho.MoveBruto(dx, dy);
                    io.AddMousePosEvent(float(pb.x - r.left), float(pb.y - r.top));
                }
                // com o modulo no jogo o cursor anda so pelo movimento bruto (o do Windows somava ~1,3x); se o jogo nao
                // ler entrada bruta, a reserva abaixo desliga o modulo e o do Windows volta
                bruto = true;
                (void)roda; // cliques e roda chegam pelo gancho do Windows (que os segura)
                // o movimento passa pelo Windows (para o modulo ver a entrada bruta) e leva o cursor de verdade junto:
                // fora da janela do jogo, o Windows ativava a area de trabalho. Ele volta ao centro do jogo a cada quadro
                RECT cj{};
                if (jogo && IsWindow(jogo) && GetClientRect(jogo, &cj))
                {
                    POINT c0{(cj.left + cj.right) / 2, (cj.top + cj.bottom) / 2};
                    ClientToScreen(jogo, &c0);
                    SetCursorPos(c0.x, c0.y);
                }
            }
            std::vector<EventoMouse> evs = gancho.Coleta(cur, esc);
            if (mouseJogo && mouseJogo->mouseEventos == eventosAoAbrir)
            {
                // o Windows ve o mouse andar e o jogo nao leu nenhum evento bruto: ele usa outra entrada (GameInput?).
                // Volta ao gancho do Windows segurando o mouse (o jogo nao ve o mouse; o cursor anda pelo do Windows)
                for (const EventoMouse& m : evs)
                    movesSemJogo += m.tipo == EventoMouse::Move;
                if (movesSemJogo > 20)
                {
                    Log("painel", "o jogo nao leu o mouse por entrada bruta: o painel segura o mouse pelo Windows");
                    jogoSemBruta = true;
                    DefineMouseJogo(nullptr);
                }
            }
            for (const EventoMouse& m : evs)
            {
                if (m.tipo == EventoMouse::Move && bruto)
                    continue; // o jogo manda movimento bruto: o do cursor andaria em dobro
                if (m.tipo == EventoMouse::Move)
                    io.AddMousePosEvent(float(m.x - r.left), float(m.y - r.top));
                else if (m.tipo == EventoMouse::Botao)
                {
                    io.AddMouseButtonEvent(m.botao, m.desce);
                    if (m.desce)
                        Log("painel", Fmt("clique (botao %d) em %ld,%ld do painel", m.botao, cur.x - r.left, cur.y - r.top));
                }
                else
                    io.AddMouseWheelEvent(m.rodaH, m.roda);
            }
            // teclado do campo de texto (o jogo continua em foco): teclas de controle e caracteres
            for (const EventoTecla& t : gancho.ColetaTeclas())
            {
                const ImGuiKey k = TeclaImGui(t.vk);
                if (k != ImGuiKey_None)
                    io.AddKeyEvent(k, t.desce);
                if (t.vk == VK_SHIFT || t.vk == VK_LSHIFT || t.vk == VK_RSHIFT)
                    io.AddKeyEvent(ImGuiMod_Shift, t.desce);
                else if (t.vk == VK_CONTROL || t.vk == VK_LCONTROL || t.vk == VK_RCONTROL)
                    io.AddKeyEvent(ImGuiMod_Ctrl, t.desce);
                for (int i = 0; i < t.nch; ++i)
                    io.AddInputCharacterUTF16(t.ch[i]);
            }
            if (esc)
            {
                Esconde(true);
                return a;
            }
        }
    }
    ImGui::NewFrame();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("DLSS 5", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushFont(g_texto, g_tTexto);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float larg = ImGui::GetContentRegionAvail().x;
    bool mc = false; // mudou algo que a rede ve: o historico recomeca

    // ---- cabecalho: titulo, fechar, estado (o perfil fica na secao JOGO, como o seletor de preset do ReShade) ----
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        {
            const float tw = g_texto->CalcTextSizeA(g_tTitulo, FLT_MAX, 0, "DLSS 5").x;
            dl->AddText(g_texto, g_tTitulo, p, kTexto, "DLSS 5");
            dl->AddRectFilled(ImVec2(p.x, p.y + g_tTitulo + 4 * g_e), ImVec2(p.x + tw, p.y + g_tTitulo + 7 * g_e), kVerde,
                              1.5f * g_e);
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x + larg - 28 * g_e, p.y));
        // Esc com um campo de texto ativo cancela o campo, nao fecha o painel
        if (ImGui::InvisibleButton("##fecha", ImVec2(28 * g_e, 28 * g_e)) ||
            (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput && !g_editando))
            Esconde(true);
        const ImVec2 c(p.x + larg - 14 * g_e, p.y + 14 * g_e);
        const ImU32 cx = ImGui::IsItemHovered() ? kTexto : kTexto2;
        dl->AddLine(ImVec2(c.x - 6 * g_e, c.y - 6 * g_e), ImVec2(c.x + 6 * g_e, c.y + 6 * g_e), cx, 1.8f * g_e);
        dl->AddLine(ImVec2(c.x - 6 * g_e, c.y + 6 * g_e), ImVec2(c.x + 6 * g_e, c.y - 6 * g_e), cx, 1.8f * g_e);
        Dica("Esconde o painel e devolve o jogo (Esc; Ctrl+Shift+P mostra de novo)");
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + g_tTitulo + 14 * g_e));
        ImGui::Dummy(ImVec2(0, 0));
        if (gancho.Ligado())
        {
            // o jogo continua em foco e o mouse esta no painel: aviso numa linha propria (ao lado das abas ele cobria
            // o titulo em paineis estreitos)
            const ImVec2 q = ImGui::GetCursorScreenPos();
            dl->AddText(g_texto, g_tSecao, q, kVerde, "jogo em foco  ·  Esc devolve o mouse");
            ImGui::Dummy(ImVec2(0, g_tSecao + 4 * g_e));
        }
        if (e.alvo.empty())
            ImGui::TextColored(ImVec4(1, 0.78f, 0.3f, 1), "Nenhum jogo  ·  Ctrl+Shift+D com o jogo em foco");
        else
        {
            const ImVec2 q = ImGui::GetCursorScreenPos();
            dl->AddCircleFilled(ImVec2(q.x + 5 * g_e, q.y + g_tTexto * 0.55f), 4.5f * g_e, e.redeOk ? kVerde : IM_COL32(230, 80, 80, 255));
            ImGui::SetCursorScreenPos(ImVec2(q.x + 16 * g_e, q.y));
            const std::string nomeJogo = e.exe.empty() ? Utf8(e.alvo) : e.exe;
            if (e.vfps > 0)
                ImGui::Text("%s   %.0f fps / %.0f vfps", nomeJogo.c_str(), e.fps, e.vfps);
            else
                ImGui::Text("%s   %.0f fps", nomeJogo.c_str(), e.fps);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kTexto2), "rede %ux%u  ·  overlay %.1f ms  ·  resposta em %.0f%%",
                               e.redeW, e.redeH, e.msMedio, 100.0 * e.fracaoRede);
        }
        if (!e.memoria.empty())
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kTexto2), "%s", e.memoria.c_str());
        if (!e.redeOk)
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Rede indisponível: só o quadro original (overlay.jsonl)");
        if (!e.avisoMemoria.empty())
        {
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextColored(ImVec4(1, 0.78f, 0.3f, 1), "%s", e.avisoMemoria.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Dummy(ImVec2(0, 4 * g_e));
        bool ligado = cfg.modo != 1;
        if (Marca("DLSS LIGADO", &ligado, "Liga/desliga a rede (Ctrl+Shift+N troca o modo de exibicao)", true))
        {
            cfg.modo = ligado ? 0 : 1;
            mc = true;
        }
    }

    ImGui::BeginChild("##rolagem", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGui::PushFont(g_texto, g_tTexto);
    const float lc = ImGui::GetContentRegionAvail().x;

    {
        // ---- jogo (escolher a janela sem o atalho) e o perfil dele: a primeira secao ----
        Secao("JOGO", false);
        {
            const char* atual = escolhida >= 0 ? janelas[escolhida].nome.c_str() : "(nenhuma)";
            ImGui::SetNextItemWidth(lc);
            if (ImGui::BeginCombo("##janela", atual))
            {
                for (int i = 0; i < int(janelas.size()); ++i)
                    if (ImGui::Selectable(janelas[i].nome.c_str(), i == escolhida))
                        escolhida = i;
                ImGui::EndCombo();
            }
            if (ImGui::IsItemClicked())
                AtualizaJanelas();
            const float b3 = (lc - 16 * g_e) / 3;
            if (Botao("Ligar", b3, true) && escolhida >= 0)
                a.ligar = janelas[escolhida].h;
            ImGui::SameLine(0, 8 * g_e);
            if (Botao("Desligar", b3))
                a.desligar = true;
            ImGui::SameLine(0, 8 * g_e);
            if (Botao("Atualizar", b3))
                AtualizaJanelas();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kTexto2), "Jogo em janela ou tela cheia sem borda.");
            SecaoPerfil(e, a, lc);
        }

        // ---- predefinicoes ----
        Secao("PREDEFINIÇÕES");
        {
            const float b2 = (lc - 8 * g_e) / 2;
            if (Botao("Padrão NVIDIA", b2))
            {
                // medido contra a saida REAL da NVIDIA (RTX 5090, mesma entrada; ferramentas/compara_ref.py):
                // 45,8 / 45,3 / 47,4 dB em 1080p / 1440p / 4K, edicao com correlacao 0,99
                cfg.ngx = ParamsNgx{};
                cfg.temporal.composicao = 1;
                cfg.temporal.cor = 1.f;
                cfg.temporal.guarda = 16.f;
                cfg.temporal.exposicao = 1.f;
                cfg.forca = 1.f;
                cfg.gauss = 0.f, cfg.rcas = false; // a saida da NVIDIA nao tem filtro extra
                cfg.ponte = -1;
                cfg.preLuz = cfg.preSombra = cfg.preCor = 0.f;
                cfg.alturaRede = 2160; // a rede na resolucao da janela (abaixo da nativa e a maior perda: 1080 num 4K = 32,7 dB)
                cfg.brancoNits = 0.f;  // branco de papel do Windows: 79 nits num monitor de 240 custava 13 dB em HDR
                cfg.rede.redeCompleta = true;
                cfg.rede.passes = 1;
                cfg.profundidade = 0; // os 71 blocos
                mc = true;
            }
            Dica("Padroes da NVIDIA (Model A, sem CAS): 45,8 dB em 1080p e 47,4 dB em 4K contra a saida real.");
            ImGui::SameLine(0, 8 * g_e);
            if (Botao("Visual Zorah", b2))
            {
                // medido na Zorah (fotos on/off da NVIDIA): curva de tom, croma por faixa e detalhe (ferramentas/placar_nvidia.py)
                cfg.ngx = ParamsNgx{};
                cfg.ngx.modelo = 2;
                cfg.ngx.tomLocal = 0.f;
                cfg.ponte = 3;
                cfg.preLuz = 0.5f;
                cfg.preSombra = 0.f;
                cfg.preLimiar = 0.4f;
                cfg.preLimiarSombra = 0.2f;
                cfg.preCor = 0.f;
                cfg.forca = 1.f;
                cfg.temporal.composicao = 1;
                cfg.temporal.cor = 1.f;
                cfg.temporal.guarda = 2.f;
                cfg.temporal.exposicao = 1.f;
                mc = true;
            }
            Dica("Imita a foto Zorah do jogo (Model C, ponte Curva do jogo); fica em 24 dB contra a NVIDIA.");
            if (Botao("Redefinir tudo", lc))
                a.redefinir = true;
            Dica("Volta tudo aos padroes de fabrica; nada e gravado ate salvar, sair ou trocar de jogo.");
        }

        // ---- controles globais (os da NVIDIA) ----
        Secao("CONTROLES GLOBAIS");
        // as faixas da NVIDIA: fora delas a rede inverte (medido: estrutura acima de 2 e tom acima de 1 clareiam a imagem
        // em vez de reforcar; intensidade acima de 2 tambem)
        mc |= Deslizador("Estrutura", &cfg.ngx.estruturaLocal, -1.f, 2.f, "%.3f",
                         "DLSSNR.LocalStructureStrength: detalhe e forma locais (faixa da NVIDIA, -1 a 2).", 1.f);
        mc |= Deslizador("Tom", &cfg.ngx.tomLocal, 0.f, 1.f, "%.3f",
                         "DLSSNR.LocalToneStrength: luz e cor em escala grande e gradacao do modelo (NVIDIA, 0 a 1).", 1.f);
        Deslizador("Intensidade", &cfg.ngx.intensidade, 0.f, 2.f, "%.3f",
                   "DLSSNR.Intensity: quanto da resposta da rede entra; acima de 1 reforca a luz (0 a 2).", 1.f);
        Deslizador("Força da edição", &cfg.forca, 0.f, 2.f, "%.3f", "Quanto da mudanca da rede entra no quadro (1 = a da rede).", 1.f);
        Deslizador("Gaussian Blur (força)", &cfg.gauss, 0.f, 0.5f, "%.2f",
                   "Gaussian 3x3 fixo misturado ao quadro, so no lado da rede; 0 = desligado (padrao), 0,5 = maximo.", 0.f);
        Marca("RCAS", &cfg.rcas, "RCAS do FSR 1 depois do Gaussian, so no lado da rede.");
        if (cfg.rcas)
            Deslizador("RCAS sharpness (stops)", &cfg.rcasStops, 0.f, 1.f, "%.2f",
                       "Nitidez do RCAS em stops: 0 = o maximo, 1 = metade (padrao do FSR 1: 0,2).", 0.2f);

        {
            const char* modelos[] = {"Model A", "Model B", "Model C"};
            if (Segmentos("modelo", modelos, 3, &cfg.ngx.modelo))
                mc = true;
            Dica("A: NVIDIA. B: natural (-0,1 EV, contraste -25%, sat x0,90). C: cinematico (sat x0,85).");
            Marca("Gradação do modelo (B/C)", &cfg.ngx.gradacao, "A gradacao final do kernel da NVIDIA para os Models B e C.");
        }

        // ---- pele: a separacao que a propria rede da NVIDIA faz (UseAutoMask) e o ajuste fino dela ----
        Secao("PELE (NVIDIA)");
        {
            mc |= Marca("Pele separada pela rede", &cfg.ngx.mascara,
                        "DLSSNR.UseAutoMask: canal de pele da rede; desligado, volta ao padrao. Padrao: ligada.");
            ImGui::BeginDisabled(!cfg.ngx.mascara);
            mc |= Deslizador("Estrutura da pele", &cfg.ngx.estruturaPele, -1.f, 2.f, "%.3f",
                             "DLSSNR.SkinStructureStrength (-1 a 2): efeito pequeno, a rede nao separa a pele sozinha.", 1.f);
            ImGui::EndDisabled();
        }

        Secao("MODO DE REDE");
        {
            const char* fontes[] = {"Captura de vídeo", "Captura de buffer"};
            int f = cfg.fonte == 1 ? 1 : 0;
            if (Segmentos("fonte", fontes, 2, &f, 32 * g_e))
                cfg.fonte = f;
            Dica("Video: como o OBS, qualquer jogo. Buffer: DLL no jogo (DX12 + FSR/XeSS); anti-cheat recusa.");
            if (cfg.fonte == 1)
            {
                const std::string& s = e.estadoBuffer;
                const ImVec4 cor = s.rfind("conectado", 0) == 0     ? ImGui::ColorConvertU32ToFloat4(kVerde)
                                   : s.rfind("nao conectou", 0) == 0 ? ImVec4(1, 0.45f, 0.4f, 1)
                                                                     : ImVec4(1, 0.78f, 0.3f, 1);
                ImGui::PushTextWrapPos(0.f);
                ImGui::TextColored(cor, "%s", s.empty() ? "Ctrl+Shift+D com o jogo em foco (FSR ou XeSS ligado no jogo)" : s.c_str());
                ImGui::PopTextWrapPos();

                // geracao de quadros da AMD: a DLL na pasta do jogo troca o swapchain na criacao
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kTexto2), "Geração de quadros (AMD)");
                // so a geracao neural da AMD (FSR 4 Redstone): 2x o meio entre os reais; 3x o mesmo gerado
                // apresentado duas vezes; 4x o intercalado de dois contextos (0,25 / 0,5 / 0,75)
                const char* geracoes[] = {"Desligada", "2x", "3x", "4x"};
                int ig = cfg.geracao >= 2 ? std::min(cfg.geracao, 4) - 1 : 0;
                if (Segmentos("geracao", geracoes, 4, &ig, 30 * g_e))
                    cfg.geracao = ig ? ig + 1 : 0;
                Dica("Geracao FSR 4 Redstone no jogo (2x; 3x com o gerado repetido; 4x intercalado), muda a qualquer "
                     "momento. Precisa da geracao FSR do jogo ligada no menu dele (a camera vem dela). Na 1a vez pede reabrir o "
                     "jogo. Desligue o AFMF do driver.");
                if (e.instaladoNoJogo && Botao("Remover do jogo"))
                    a.removerDoJogo = true;
                if (!e.estadoGeracao.empty())
                {
                    ImGui::PushTextWrapPos(0.f);
                    ImGui::TextColored(e.geracaoPronta ? ImGui::ColorConvertU32ToFloat4(kVerde) : ImVec4(1, 0.78f, 0.3f, 1),
                                       "%s", e.estadoGeracao.c_str());
                    ImGui::PopTextWrapPos();
                }
            }
        }

        Secao("EXIBIÇÃO");
        {
            const char* modos[] = {"Rede", "Original", "Comparação (lado a lado)", "Só a edição da rede", "Movimento simulado",
                                   "Saída crua da rede (sem movimento)"};
            ImGui::SetNextItemWidth(lc);
            ImGui::Combo("##modo", &cfg.modo, modos, 6);
            Dica("Ctrl+Shift+N troca o modo. Saida crua: resposta da rede sem composicao; defeito aqui e dela.");
            if (cfg.modo == 2)
                Deslizador("Divisão", &cfg.divisao, 0.f, 1.f, "%.2f", nullptr, 0.5f);
        }

        // ---- avancado (o resto, recolhido) ----
        ImGui::Dummy(ImVec2(0, 6 * g_e));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
        avancado = ImGui::CollapsingHeader("AVANÇADO", avancado ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        ImGui::PopStyleColor();
        if (avancado)
        {
            Secao("COMPOSIÇÃO", false);
            {
                const char* luzes[] = {"Linear", "Logarítmica", "Mista"};
                Segmentos("luz", luzes, 3, &cfg.temporal.luz, 30 * g_e);
                Dica("Como a luz da rede chega a tela e ao jogo. Mista (padrao): na tela como a logaritmica; no jogo "
                     "clarear soma (a logaritmica vira grao na borda do cabelo).");
            }
            const char* comps[] = {"Crua (resposta - proxy)", "Referencia NVIDIA/RenoDX"};
            ImGui::SetNextItemWidth(lc);
            ImGui::Combo("##comp", &cfg.temporal.composicao, comps, 2);
            ImGui::BeginDisabled(cfg.temporal.composicao == 0);
            Deslizador("Cor", &cfg.temporal.cor, 0.f, 1.f, "%.3f", "0 = cores do jogo na luz da rede, 1 = cores da rede (padrao).", 1.f);
            Deslizador("Proteção de luz", &cfg.temporal.guarda, 1.f, 16.f, "%.1fx",
                       "Limita a mudanca de luz por ponto. 16 = desligada (padrao); 2,0 afastou 1,5-2 dB da NVIDIA.", 16.f);
            ImGui::EndDisabled();

            Secao("COR POR MATIZ (DEPOIS DA REDE)");
            {
                const char* matizes[6] = {"Vermelho", "Amarelo", "Verde", "Ciano", "Azul", "Magenta"};
                const ImU32 cores[6] = {IM_COL32(232, 72, 72, 255),  IM_COL32(236, 206, 60, 255), IM_COL32(88, 192, 80, 255),
                                        IM_COL32(60, 202, 216, 255), IM_COL32(72, 124, 238, 255), IM_COL32(212, 82, 204, 255)};
                for (int i = 0; i < 6; ++i)
                {
                    ImGui::PushID(300 + i);
                    Deslizador((std::string(matizes[i]) + ": saturação").c_str(), &cfg.matiz.sat[i], -1.f, 1.f, "%+.3f",
                               "Saturacao desta faixa de cor no resultado: -1 = cinza, 0 = igual, +1 = o dobro.", 0.f, cores[i]);
                    Deslizador((std::string(matizes[i]) + ": brilho").c_str(), &cfg.matiz.luz[i], -1.f, 1.f, "%+.3f",
                               "Brilho desta faixa: -1 apaga, +1 dobra. Cinza e branco nao mudam (pesa a saturacao).", 0.f, cores[i]);
                    ImGui::PopID();
                }
                if (Botao("Zerar cores"))
                    cfg.matiz = CorMatiz{};
            }

            Secao("PONTE HDR -> REDE");
            const char* pontes[] = {"Automatica (SDR direta, HDR SoftKnee)", "Direta", "SoftKnee", "ACES (RRT+ODT, RenoDX)",
                                    "Curva do jogo (luz de cena, SDR)", "Luz direta sem joelho (só captura de buffer)"};
            int ip = cfg.ponte + 1;
            ImGui::SetNextItemWidth(lc);
            if (ImGui::Combo("##ponte", &ip, pontes, 6))
            {
                cfg.ponte = ip - 1;
                mc = true;
            }
            Dica("Proxy SDR da rede: 'Curva do jogo' desfaz a filmica; 'Luz direta' so na captura de buffer.");
            Deslizador("Branco (nits)", &cfg.brancoNits, 0.f, 400.f, cfg.brancoNits > 0.f ? "%.0f" : "Windows",
                       "Branco de papel do jogo em HDR (0 = o branco SDR do Windows, o certo para a rede).", 0.f);

            Secao("ANTES DA REDE: LUZES E SOMBRAS");
            mc |= Deslizador("Luzes", &cfg.preLuz, 0.f, 8.f, "%.3f",
                             "Ganho das areas acima do limiar antes da rede (ideia do Shadow-Light-Scattering.fx).", 0.f);
            mc |= Deslizador("Sombras", &cfg.preSombra, -2.f, 2.f, "%.3f",
                             "Acima de 0 escurece as areas abaixo do limiar antes da rede; abaixo de 0 levanta.", 0.f);
            mc |= Deslizador("Limiar luzes", &cfg.preLimiar, 0.05f, 0.95f, "%.3f", "Luma de tela a partir da qual as luzes sobem.", 0.5f);
            mc |= Deslizador("Limiar sombras", &cfg.preLimiarSombra, 0.02f, 0.6f, "%.3f",
                             "Luma de tela abaixo da qual as sombras descem.", 0.2f);
            mc |= Deslizador("Saturação", &cfg.preCor, -1.f, 2.f, "%.3f",
                             "Saturacao antes da rede: a rede ve e devolve a cena mais (ou menos) colorida.", 0.f);

            Secao("EXECUÇÃO DA REDE");
            {
                const char* redes[] = {"Grande", "Pequena (rápida)"};
                int ir = cfg.redePequena ? 1 : 0;
                if (Segmentos("rede", redes, 2, &ir, 32 * g_e))
                    cfg.redePequena = ir == 1;
                Dica("Grande (71 blocos): fiel a NVIDIA. Pequena (31): 36 dB contra 47 em 4K. Vale ao religar.");
                const int alturas[] = {540, 600, 720, 900, 1080, 1440, 2160};
                const char* nomes[] = {"540p", "600p", "720p", "900p", "1080p", "1440p", "2160p (nativa até 4K)"};
                int ia = 4;
                for (int i = 0; i < 7; ++i)
                    if (alturas[i] == cfg.alturaRede)
                        ia = i;
                ImGui::SetNextItemWidth(lc);
                if (ImGui::Combo("##altura", &ia, nomes, 7))
                    cfg.alturaRede = alturas[ia];
                Dica("Menor = mais rapido; a da janela (ate 2160) e a mais fiel. Vale ao religar (prende VRAM).");
                ImGui::BeginDisabled(cfg.redePequena);
                Marca("Rede completa (71 blocos)", &cfg.rede.redeCompleta,
                      "Desligado pula 3 blocos (42, 43, 46): ~1 ms mais rapido. So na rede grande.");
                ImGui::EndDisabled();
                DeslizadorInt("Passes", &cfg.rede.passes, 1, 3, "%d",
                              "Passes da rede por resposta (1 = normal); mais = mais forte e mais caro. Vale ao religar.", 1.f);
                if (!e.alvo.empty() && (cfg.redePequena != e.pequenaSessao || cfg.alturaRede != e.alturaSessao ||
                                        cfg.rede.passes != e.passesSessao ||
                                        cfg.rede.redeCompleta != e.completaSessao))
                {
                    ImGui::PushTextWrapPos(0.f);
                    ImGui::TextColored(ImVec4(1, 0.78f, 0.3f, 1),
                                       "Em uso: rede %s, %dp, %d passe(s)%s. As mudanças de rede, tamanho e passes valem ao "
                                       "religar (Ctrl+Shift+D duas vezes).",
                                       e.pequenaSessao ? "pequena" : "grande", e.alturaSessao, e.passesSessao,
                                       e.completaSessao ? "" : ", sem os blocos 42/43/46");
                    ImGui::PopTextWrapPos();
                }
                // so na captura de video: na de buffer a geracao de verdade e a da AMD (MODO DE REDE)
                if (cfg.fonte != 1)
                {
                DeslizadorInt("Quadros gerados", &cfg.ritmo, 1, 4, "x%.0f",
                              "Como o AMD Fluid Motion Frames (ate 4x): a rede edita 1 de cada N; 1 = todos os quadros.", 1.f);
                ImGui::BeginDisabled(cfg.ritmo < 2);
                const char* gerados[] = {"Quadro repetido", "Quadro mesclado"};
                Segmentos("gerado", gerados, 2, &cfg.gerado, 30 * g_e);
                Dica("Repetido: repete o processado. Mesclado: mistura o atual e o proximo, sempre 1 quadro de atraso.");
                ImGui::EndDisabled();
                }
            }

            Secao("MOVIMENTO");
            if (Marca("Movimento simulado (fluxo óptico)", &cfg.movimento,
                      "Vetores de movimento estimados entre quadros capturados (sem buffer); guiam o historico."))
                mc = true;
            ImGui::BeginDisabled(e.historicoAtivo);
            Marca("Reuso adaptativo do ViT (ao reabrir)", &cfg.reusoVit,
                  "Blocos ViT reusam o calculo se o quadro muda pouco. Vale ao reabrir; nao vai com historico.");
            ImGui::EndDisabled();
            {
                bool hist = cfg.temporalRede == 1;
                if (Marca("Histórico da própria rede (ao reabrir)", &hist,
                          "Reusa a resposta anterior da rede pelo movimento. Vale ao reabrir; exige runtime compilado."))
                    cfg.temporalRede = hist ? 1 : 0;
                ImGui::BeginDisabled(!e.historicoAtivo);
                Deslizador("Força do histórico", &cfg.forcaHistorico, 0.f, 2.f, "%.3f",
                           "Peso da resposta anterior: 2 = padrao (-9% de cintilacao); 1 = o da rede; 0 = sem historico.", 2.f);
                Deslizador("Limite do histórico", &cfg.recorteHistorico, 0.f, 4.f, "%.2f",
                           "Nosso (nao NVIDIA): limita o historico a faixa da saida atual; 0 = sem limite (padrao).", 0.f);
                ImGui::EndDisabled();
                if (!e.historicoRede.empty())
                {
                    ImGui::PushTextWrapPos(0.f);
                    ImGui::TextDisabled("%s", e.historicoRede.c_str());
                    ImGui::PopTextWrapPos();
                }
            }

            Secao("OVERLAY");
            DeslizadorInt("FPS máximo", &cfg.fpsMax, 0, 240, "%d", "Teto de quadros processados por segundo (0 = sem teto).",
                          60.f);
            if (Marca("Ocultar de prints e gravações", &cfg.ocultaDeGravacoes,
                      "O overlay fica fora de prints, OBS e da propria Game Bar."))
                a.ocultaMudou = true;
        }
    }

    // ---- rodape ----
    Secao("", true);
    {
        if (Botao("Sair (Ctrl+Shift+Q)", lc))
            a.sair = true;
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kTexto2), "Ctrl+Shift+P abre/fecha (com o mouse)  ·  Esc devolve o jogo");
        ImGui::TextDisabled("%s", Utf8(e.placa).c_str());
    }
    ImGui::PopFont();
    ImGui::EndChild();
    if (mc)
        a.zeraHistorico = true;
    ImGui::PopFont();
    ImGui::End();
    ImGui::Render();
    // um campo de texto ativo com o painel sobre o jogo: as teclas passam a vir pelo gancho (o jogo nao as ve)
    gancho.CapturaTeclado(gancho.Ligado() && ImGui::GetIO().WantTextInput);

    const UINT i = sc->GetCurrentBackBufferIndex();
    Ck(al->Reset(), "al(painel) reset");
    Ck(cl->Reset(al.Get(), nullptr), "cl(painel) reset");
    Barreira(cl.Get(), bb[i].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(i) * rtvPasso;
    const float fundo[4] = {22 / 255.f, 23 / 255.f, 26 / 255.f, 1.f};
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
