#include "pch.h"
#include "lmxxf2_seguro.h"

#include <Config.h>
#include <State.h>
#include <Util.h>

#include <atomic>
#include <cstring>
#include <thread>

namespace Lmxxf2Seguro
{
namespace
{
constexpr DWORD kEstavelMs = 5 * 60 * 1000; // queda antes disso = carregamento / shaders / primeira cena
constexpr DWORD kBatidaMs = 5000;           // batimento: a hora aproximada da queda
constexpr ULONGLONG kRedeCulpadaMs = 30000; // queda até 30 s depois de a rede ligar: a culpa é da rede

// recursos que o modo seguro desliga (bits do json: "desligados" e "ativos")
constexpr int kSpoof = 1, kFg = 2, kRede = 4;

std::atomic<bool> g_iniciado { false };
bool g_ativo = false;
int g_desligados = 0;
std::atomic<int> g_ativos { 0 };
int g_quedas = 0;
const char* g_estado = "rodando";
std::string g_motivo = "ainda nao iniciado";
std::wstring g_arq;
ULONGLONG g_inicio = 0;
std::atomic<ULONGLONG> g_redeMs { 0 }; // ms desde o início em que a rede ligou (0 = não ligou)
std::atomic<ULONGLONG> g_vivoMs { 0 }; // último batimento
// trava curta sem mutex: Grava roda também no DLL_PROCESS_DETACH, quando a thread do batimento pode ter morrido
// segurando-a; depois de ~200 ms grava assim mesmo
std::atomic<bool> g_gravando { false };
struct Trava
{
    bool ok = false;
    Trava()
    {
        for (int i = 0; i < 200 && !ok; i++)
        {
            bool livre = false;
            ok = g_gravando.compare_exchange_strong(livre, true);
            if (!ok)
                Sleep(1);
        }
    }
    ~Trava()
    {
        if (ok)
            g_gravando = false;
    }
};

struct Sessao
{
    std::string estado;
    int desligados = 0, ativos = 0, quedas = 0, nivelAntigo = 0;
    ULONGLONG redeMs = 0, vivoMs = 0;
};

// só Win32: Saida() roda no DLL_PROCESS_DETACH
bool Le(Sessao& s)
{
    HANDLE h = CreateFileW(g_arq.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    char buf[768] {};
    DWORD n = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
    CloseHandle(h);

    const std::string t(buf, n);
    const auto campo = [&t](const char* k) -> std::string
    {
        auto p = t.find(std::string("\"") + k + "\"");
        if (p == std::string::npos || (p = t.find(':', p)) == std::string::npos)
            return {};
        p++;
        while (p < t.size() && (t[p] == ' ' || t[p] == '"'))
            p++;
        const auto e = t.find_first_of("\",}\r\n", p);
        return t.substr(p, e == std::string::npos ? std::string::npos : e - p);
    };
    s.estado = campo("estado");
    s.desligados = atoi(campo("desligados").c_str());
    s.ativos = atoi(campo("ativos").c_str());
    s.quedas = atoi(campo("quedas").c_str());
    s.nivelAntigo = atoi(campo("nivel").c_str()); // formato de 13/10 madrugada (níveis cumulativos)
    s.redeMs = _strtoui64(campo("rede_ms").c_str(), nullptr, 10);
    s.vivoMs = _strtoui64(campo("vivo_ms").c_str(), nullptr, 10);
    return !s.estado.empty();
}

void Grava(const char* estado)
{
    if (g_arq.empty())
        return;
    Trava trava;
    g_estado = estado;
    char buf[384];
    const int n = snprintf(buf, sizeof(buf),
                           "{\n  \"estado\": \"%s\",\n  \"desligados\": %d,\n  \"ativos\": %d,\n  \"quedas\": %d,\n"
                           "  \"rede_ms\": %llu,\n  \"vivo_ms\": %llu\n}\n",
                           estado, g_desligados, g_ativos.load(), g_quedas, (unsigned long long) g_redeMs.load(),
                           (unsigned long long) g_vivoMs.load());
    HANDLE h = CreateFileW(g_arq.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD w = 0;
    WriteFile(h, buf, static_cast<DWORD>(n), &w, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
}

std::string Lista(int bits)
{
    std::string s;
    if (bits & kSpoof)
        s += "spoof NVIDIA";
    if (bits & kFg)
        s += std::string(s.empty() ? "" : ", ") + "FG automatico";
    if (bits & kRede)
        s += std::string(s.empty() ? "" : ", ") + "rede DLSS5";
    return s.empty() ? "nada" : s;
}

void Marca(int bit)
{
    if (!g_ativo || (g_ativos.fetch_or(bit) & bit) != 0)
        return;
    if (bit == kRede)
        g_redeMs = GetTickCount64() - g_inicio;
    if (std::strcmp(g_estado, "rodando") == 0)
        Grava("rodando");
}
} // namespace

void Inicia()
{
    if (g_iniciado.exchange(true))
        return;

    auto* cfg = Config::Instance();
    if (!cfg->Lmxxf2ModoSeguro.value_or_default())
    {
        g_motivo = "desligado no ini ([DlssNr] ModoSeguro=false)";
        spdlog::info("[lmxxf2] modo seguro: {}", g_motivo);
        return;
    }

    g_ativo = true;
    g_inicio = GetTickCount64();
    g_arq = (Util::DllPath().parent_path() / L"lmxxf2_sessao.json").wstring();

    Sessao s;
    const bool tem = Le(s);
    // formato antigo (nível cumulativo 1..3): convertido para os recursos que ele desligava
    g_desligados = (s.desligados & (kSpoof | kFg | kRede)) |
                   (s.nivelAntigo >= 1 ? kSpoof : 0) | (s.nivelAntigo >= 2 ? kFg : 0) | (s.nivelAntigo >= 3 ? kRede : 0);
    g_quedas = std::max(s.quedas, 0);

    if (tem && s.estado == "rodando")
    {
        g_quedas++;
        // culpado: a rede, se a queda veio logo depois de ela ligar; senão o primeiro recurso que estava ligado e
        // ainda não foi desligado, do mais suspeito ao menos (spoof, FG automático, rede)
        const int livres = s.ativos & ~g_desligados;
        int culpado = 0;
        if ((livres & kRede) && s.redeMs != 0 && s.vivoMs >= s.redeMs && s.vivoMs - s.redeMs <= kRedeCulpadaMs)
            culpado = kRede;
        else if (livres & kSpoof)
            culpado = kSpoof;
        else if (livres & kFg)
            culpado = kFg;
        else if (livres & kRede)
            culpado = kRede;
        g_desligados |= culpado;
        g_motivo = culpado != 0
                       ? std::format("a sessao anterior caiu antes de {} min (queda {}, ~{} s de jogo): desligado {}",
                                     kEstavelMs / 60000, g_quedas, s.vivoMs / 1000, Lista(culpado))
                       : std::format("a sessao anterior caiu antes de {} min (queda {}) com {} ativo: nada a desligar",
                                     kEstavelMs / 60000, g_quedas, Lista(s.ativos));
    }
    else if (g_desligados != 0)
    {
        g_motivo = std::format("mantido desligado: {} (a sessao anterior nao caiu)", Lista(g_desligados));
    }
    else
    {
        g_motivo = "normal";
    }

    // rede: fora só nesta sessão (volátil, o ini não muda)
    if ((g_desligados & kRede) && cfg->DlssNrEnabled.value_or_default())
        cfg->DlssNrEnabled.set_volatile_value(false);

    Grava("rodando");
    spdlog::info("[lmxxf2] modo seguro: {} | desligados: {}", g_motivo, Lista(g_desligados));

    // batimento a cada 5 s (a hora da queda, para culpar a rede ligada há pouco) até a sessão ficar estável
    std::thread(
        []
        {
            for (DWORD t = 0; t < kEstavelMs; t += kBatidaMs)
            {
                Sleep(kBatidaMs);
                if (State::Instance().isShuttingDown || std::strcmp(g_estado, "rodando") != 0)
                    return;
                g_vivoMs = GetTickCount64() - g_inicio;
                Grava("rodando");
            }
            if (State::Instance().isShuttingDown || std::strcmp(g_estado, "rodando") != 0)
                return;
            Grava("estavel");
            spdlog::info("[lmxxf2] modo seguro: sessao estavel (desligados: {})", Lista(g_desligados));
        })
        .detach();
}

void NotaSpoof(bool ligado)
{
    if (ligado)
        Marca(kSpoof);
}

void NotaFgAuto(bool aplicado)
{
    if (aplicado)
        Marca(kFg);
}

void NotaRede() { Marca(kRede); }

void Saida()
{
    if (g_ativo)
        Grava("limpa");
}

void Saindo()
{
    static std::atomic<bool> uma { false };
    if (!g_ativo || uma.exchange(true))
        return;
    if (std::strcmp(g_estado, "rodando") == 0)
        Grava("saindo");
    spdlog::info("[lmxxf2] modo seguro: o jogo esta fechando a janela (saida normal)");
}

int Nivel() { return (g_desligados & kSpoof ? 1 : 0) + (g_desligados & kFg ? 1 : 0) + (g_desligados & kRede ? 1 : 0); }
std::string Motivo() { return g_motivo; }
bool SemSpoof() { return (g_desligados & kSpoof) != 0; }
bool SemFgAuto() { return (g_desligados & kFg) != 0; }
bool SemRede() { return (g_desligados & kRede) != 0; }

void Normal()
{
    g_desligados = 0;
    g_quedas = 0;
    g_motivo = "voltou ao normal pela UI; vale na proxima vez que o jogo abrir";
    if (g_ativo)
        Grava(g_estado);
    spdlog::info("[lmxxf2] modo seguro: {}", g_motivo);
}
} // namespace Lmxxf2Seguro
