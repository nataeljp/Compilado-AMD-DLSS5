#include "pch.h"
#include "lmxxf2_autofg.h"
#include "lmxxf2_seguro.h"

#include <Config.h>
#include <State.h>
#include <Util.h>

#include <magic_enum.hpp>

#include <filesystem>
#include <queue>
#include <set>

namespace Lmxxf2AutoFg
{
namespace
{
Achados g;
bool g_aplicado = false;

std::wstring Minusculo(std::wstring s)
{
    for (auto& ch : s)
        ch = static_cast<wchar_t>(towlower(ch));
    return s;
}

// O exe importa essa DLL (tabela de importação do módulo já mapeado)
bool Importa(HMODULE m, const char* dll)
{
    const auto* base = reinterpret_cast<const BYTE*>(m);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (base == nullptr || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0)
        return false;
    for (auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name != 0; ++imp)
    {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) == 0)
            return true;
    }
    return false;
}

// Pastas que não são do jogo: a do OptiScaler (e o streamline dela), backups e os assets da rede
bool Pula(const std::filesystem::path& dir, const std::filesystem::path& opti)
{
    if (!opti.empty() && Util::IsSubpath(dir, opti))
        return true;
    const std::wstring nome = Minusculo(dir.filename().wstring());
    return nome == L"optiscaler" || nome.starts_with(L"backup") || nome == L"dlss5-amd" || nome == L"nr_variantes" ||
           nome == L"licenses";
}

void Varre()
{
    if (g.varrido)
        return;
    g.varrido = true;

    const ULONGLONG t0 = GetTickCount64();
    const std::filesystem::path exeDir = Util::ExePath().parent_path().lexically_normal();
    std::filesystem::path opti;
    if (Config::Instance()->MainDllPath.has_value())
        opti = std::filesystem::path(Config::Instance()->MainDllPath.value()).lexically_normal();

    // Unreal: o exe fica em <jogo>\<Projeto>\Binaries\Win64 e as DLLs do Streamline/FFX nos plugins do Engine
    std::vector<std::filesystem::path> raizes { exeDir };
    for (auto p = exeDir; p.has_parent_path() && p != p.parent_path(); p = p.parent_path())
    {
        if (Minusculo(p.filename().wstring()) == L"binaries")
        {
            raizes.push_back(p.parent_path().parent_path());
            break;
        }
    }

    struct Alvo
    {
        const wchar_t* nome;
        std::wstring* destino;
    };
    const Alvo alvos[] = {
        { L"sl.dlss_g.dll", &g.dlssg },
        { L"nvngx_dlssg.dll", &g.dlssg },
        { L"nvngx_dlssd.dll", &g.dlssd },
        { L"sl.dlss_d.dll", &g.dlssd },
        { L"amd_fidelityfx_framegeneration_dx12.dll", &g.ffxFg },
        { L"amd_fidelityfx_dx12.dll", &g.ffxFg }, // FSR 3.1 tudo-em-um (upscaler e FG)
        { L"ffx_frameinterpolation_x64.dll", &g.ffxFg },
        { L"libxess_fg.dll", &g.xefg },
        { L"nvngx_dlss.dll", &g.dlss },
        { L"amd_fidelityfx_upscaler_dx12.dll", &g.fsr },
        { L"amd_fidelityfx_dx12.dll", &g.fsr },
        { L"ffx_fsr2_api_x64.dll", &g.fsr },
        { L"ffx_fsr2_api_dx12_x64.dll", &g.fsr },
        { L"libxess.dll", &g.xess },
    };

    // Busca em largura, com teto de pastas e de tempo (pasta de jogo grande não pode segurar a carga)
    std::queue<std::filesystem::path> fila;
    std::set<std::wstring> vistas;
    for (const auto& r : raizes)
        fila.push(r);
    while (!fila.empty() && g.pastas < 20000 && GetTickCount64() - t0 < 2000)
    {
        const std::filesystem::path dir = fila.front().lexically_normal();
        fila.pop();
        if (!vistas.insert(Minusculo(dir.wstring())).second || Pula(dir, opti))
            continue;
        ++g.pastas;

        std::error_code ec;
        std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec);
        for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        {
            std::error_code ec2;
            if (it->is_directory(ec2))
            {
                if (!it->is_symlink(ec2))
                    fila.push(it->path());
                continue;
            }
            const std::wstring nome = Minusculo(it->path().filename().wstring());
            for (const auto& a : alvos)
            {
                if (a.destino->empty() && nome == a.nome)
                    *a.destino = it->path().wstring();
            }
        }
    }
    g.ms = static_cast<unsigned>(GetTickCount64() - t0);

    HMODULE exe = GetModuleHandleW(nullptr);
    g.vulkan = Importa(exe, "vulkan-1.dll") && !Importa(exe, "d3d12.dll");
}

std::string NomeArq(const std::wstring& caminho)
{
    return wstring_to_string(std::filesystem::path(caminho).filename().wstring());
}

// Carimbo PE (TimeDateStamp, SizeOfImage) do arquivo, sem carregar a DLL
bool Carimbo(const std::filesystem::path& arq, uint32_t& stamp, uint32_t& tamanho)
{
    HANDLE h = CreateFileW(arq.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    BYTE buf[1024] {};
    DWORD lido = 0;
    const bool ok = ReadFile(h, buf, sizeof(buf), &lido, nullptr) && lido >= 0x40;
    CloseHandle(h);
    if (!ok)
        return false;
    const LONG pe = *reinterpret_cast<const LONG*>(buf + 0x3C);
    if (pe <= 0 || static_cast<DWORD>(pe) + 24 + 60 > lido || *reinterpret_cast<const DWORD*>(buf + pe) != IMAGE_NT_SIGNATURE)
        return false;
    stamp = *reinterpret_cast<const DWORD*>(buf + pe + 8);
    tamanho = *reinterpret_cast<const DWORD*>(buf + pe + 24 + 56);
    return true;
}

// O multi-frame do XeFG destrava (proxies/XeFGUnlock.h + XeLLUnLock.h) com as builds do pacote
bool XeFgMfgDisponivel(const std::filesystem::path& opti)
{
    if (!Config::Instance()->FGXeFGUnlockEnabled.value_or_default() || opti.empty())
        return false;
    uint32_t s = 0, t = 0;
    if (!Carimbo(opti / L"libxess_fg.dll", s, t) || s != 0x69CB0F4D || t != 0x015ED000)
        return false;
    if (!Carimbo(opti / L"libxell.dll", s, t))
        return false;
    return (s == 0x6A561284 && t == 0x0006A000) || (s == 0x69A6C659 && t == 0x00069000);
}

// Política "AMD primeiro": o spoof NVIDIA só é preciso quando o jogo não traz FSR (o upscaler só entra pelo DLSS).
// DLSS Ray Reconstruction não conta: na AMD o caminho é o Ray Regeneration do jogo, e o spoof em jogo Unreal trava a GPU
bool SpoofNecessario() { return g.fsr.empty() && !g.dlss.empty(); }

// FSR 3 FG de verdade (a DLL de FG ou a do FSR 3.0), não só o amd_fidelityfx_dx12.dll do upscaler
bool FsrFgDoJogo()
{
    if (g.ffxFg.empty())
        return false;
    return _wcsicmp(std::filesystem::path(g.ffxFg).filename().c_str(), L"amd_fidelityfx_dx12.dll") != 0;
}
} // namespace

const Achados& Detectados()
{
    Varre();
    return g;
}

Proposta Calcula()
{
    Varre();
    Proposta p;

    // Geradores que o pacote põe na pasta OptiScaler
    std::error_code ec;
    std::filesystem::path opti;
    if (Config::Instance()->MainDllPath.has_value())
        opti = Config::Instance()->MainDllPath.value();
    const bool enabler = !opti.empty() && std::filesystem::exists(opti / L"dlss-enabler-headless.dll", ec);
    const bool nukems = !opti.empty() && std::filesystem::exists(opti / L"dlssg_to_fsr3_amd_is_better.dll", ec);
    const bool ffxFg = !g.vulkan && ((!opti.empty() && std::filesystem::exists(
                                                           opti / L"amd_fidelityfx_framegeneration_dx12.dll", ec)) ||
                                     !g.ffxFg.empty());

    const bool xefgMfg = !g.vulkan && XeFgMfgDisponivel(opti);
    const bool spoof = SpoofNecessario();
    int maxXe = Config::Instance()->FGXeFGMaxInterpolatedFrames.value_or_default();
    if (maxXe < 1 || maxXe > Config::XeFGMaxInterpolations)
        maxXe = Config::XeFGMaxInterpolations;

    // sem spoof o jogo mostra o caminho AMD: o FSR 3 FG dele vira a entrada do XeFG destravado (multi-frame sem
    // fingir NVIDIA). Com spoof (jogo só DLSS) o DLSS-FG aparece e o Combo serve.
    if (!spoof && FsrFgDoJogo() && xefgMfg)
    {
        p.in = FGInput::FSRFG;
        p.out = FGOutput::XeFG;
        p.subst = FGNvngxReplacement::None;
        p.motivo = std::format("o jogo tem FSR 3 Frame Generation ({}): entrada FSR-FG, saida XeFG multi-frame (ate "
                               "{}x), sem spoof - no jogo ligue o FSR Frame Generation",
                               NomeArq(g.ffxFg), maxXe + 1);
    }
    else if (!g.dlssg.empty() && (enabler || ffxFg || nukems))
    {
        p.in = FGInput::NvngxFG;
        p.out = FGOutput::NoFG; // com entrada Nvngx a saída é o gerador substituto
        p.subst = enabler && ffxFg ? FGNvngxReplacement::Combo
                  : enabler        ? FGNvngxReplacement::Arturs
                  : ffxFg          ? FGNvngxReplacement::FFX
                                   : FGNvngxReplacement::Nukems;
        p.motivo = std::format("o jogo tem DLSS Frame Generation ({}): entrada DLSS-FG, gerador {}{}",
                               NomeArq(g.dlssg), magic_enum::enum_name(p.subst),
                               p.subst == FGNvngxReplacement::Combo || p.subst == FGNvngxReplacement::Arturs
                                   ? " (ate 6x)"
                                   : " (2x)");
    }
    else if (!g.dlssg.empty())
        p.motivo = "o jogo tem DLSS Frame Generation, mas falta gerador na pasta OptiScaler "
                   "(dlss-enabler-headless.dll ou amd_fidelityfx_framegeneration_dx12.dll)";
    else if (!g.ffxFg.empty())
        p.motivo = std::format("o jogo tem FSR 3 Frame Generation ({}): passa direto", NomeArq(g.ffxFg));
    else if (!g.xefg.empty())
        p.motivo = "o jogo tem XeFG: passa direto";
    else
        p.motivo = "o jogo nao traz FG; FG do OptiScaler so no modo manual (usa o upscaler)";
    return p;
}

void Aplica()
{
    auto* cfg = Config::Instance();

    // ajustes de HUD do Enabler (só agem no Enabler / Combo): o conjunto que o dono validou no Crimson Desert
    if (!cfg->NvngxFGDispatchFlags.value_for_config_ignore_default().has_value())
        cfg->NvngxFGDispatchFlags.set_volatile_value(kEnablerHudFlags);

    // ignore_default: "Desligado" grava FGInput=NoFG, que é o padrão; value_for_config o confundiria com auto
    if (cfg->FGInput.value_for_config_ignore_default().has_value() ||
        cfg->FGOutput.value_for_config_ignore_default().has_value())
    {
        spdlog::info("[lmxxf2] FG automatico: desligado (FGInput/FGOutput definidos no ini)");
        return;
    }

    if (Lmxxf2Seguro::SemFgAuto())
    {
        spdlog::info("[lmxxf2] FG automatico: desligado pelo modo seguro ({})", Lmxxf2Seguro::Motivo());
        return;
    }

    const Proposta p = Calcula();
    if (p.in != FGInput::NoFG)
    {
        cfg->FGInput.set_volatile_value(p.in);
        cfg->FGOutput.set_volatile_value(p.out);
        if (p.in == FGInput::NvngxFG && !cfg->FGNvngxReplacement.value_for_config_ignore_default().has_value())
            cfg->FGNvngxReplacement.set_volatile_value(p.subst);
        // XeFG destravado: quadros gerados no máximo configurado ([XeFG] MaxInterpolatedFrames, padrão 5 = 6x) quando o
        // ini deixou a contagem em auto; o cartão da aba Frame Gen ajusta na hora
        if (p.out == FGOutput::XeFG && !cfg->FGXeFGInterpolationCount.value_for_config_ignore_default().has_value())
        {
            int maxXe = cfg->FGXeFGMaxInterpolatedFrames.value_or_default();
            if (maxXe < 1 || maxXe > Config::XeFGMaxInterpolations)
                maxXe = Config::XeFGMaxInterpolations;
            cfg->FGXeFGInterpolationCount.set_volatile_value(maxXe);
        }
        g_aplicado = true;
        Lmxxf2Seguro::NotaFgAuto(true);
    }
    spdlog::info("[lmxxf2] FG automatico: {} | jogo traz: {} | {} pastas em {} ms", p.motivo, Resumo(), g.pastas, g.ms);
}

bool Aplicado() { return g_aplicado; }

namespace
{
std::string g_spoof = "ainda nao decidido";
}

void AplicaSpoof()
{
    auto* cfg = Config::Instance();
    if (cfg->DxgiSpoofing.value_for_config().has_value())
    {
        g_spoof = cfg->DxgiSpoofing.value_or_default() ? "ligado no ini" : "desligado no ini";
    }
    else
    {
        // "AMD primeiro": fingir NVIDIA só quando o jogo precisa. Em jogo Unreal o spoof faz o motor ligar rotas da
        // NVIDIA que travam a GPU AMD (Gears of War: E-Day, Lumen no carregamento); com FSR no jogo ele não é preciso.
        Varre();
        if (Lmxxf2Seguro::SemSpoof())
        {
            cfg->DxgiSpoofing.set_volatile_value(false);
            g_spoof = std::format("desligado pelo modo seguro ({})", Lmxxf2Seguro::Motivo());
        }
        else if (SpoofNecessario())
        {
            cfg->DxgiSpoofing.set_volatile_value(true);
            g_spoof = "ligado pelo automatico: o jogo so tem DLSS (sem FSR), o upscaler entra por ele";
        }
        else
        {
            cfg->DxgiSpoofing.set_volatile_value(false);
            g_spoof = "desligado pelo automatico: o jogo tem FSR (caminho AMD); spoof so quando o jogo precisa";
        }
    }
    spdlog::info("[lmxxf2] spoof automatico: {}", g_spoof);
    Lmxxf2Seguro::NotaSpoof(cfg->DxgiSpoofing.value_or_default());
}

std::string Spoof() { return g_spoof; }

void Reaplica()
{
    auto* cfg = Config::Instance();
    const Proposta p = Calcula();
    // voláteis por cima do ini vazio: o ini continua "auto"
    cfg->FGInput = std::optional<FGInput> {};
    cfg->FGOutput = std::optional<FGOutput> {};
    cfg->FGNvngxReplacement = std::optional<FGNvngxReplacement> {};
    cfg->FGInput.set_volatile_value(p.in);
    cfg->FGOutput.set_volatile_value(p.out);
    if (p.in == FGInput::NvngxFG)
        cfg->FGNvngxReplacement.set_volatile_value(p.subst);
    if (p.out == FGOutput::XeFG && !cfg->FGXeFGInterpolationCount.value_for_config_ignore_default().has_value())
    {
        int maxXe = cfg->FGXeFGMaxInterpolatedFrames.value_or_default();
        if (maxXe < 1 || maxXe > Config::XeFGMaxInterpolations)
            maxXe = Config::XeFGMaxInterpolations;
        cfg->FGXeFGInterpolationCount.set_volatile_value(maxXe);
    }
    cfg->NvngxFGDispatchFlags = std::optional<uint32_t> {};
    cfg->NvngxFGDispatchFlags.set_volatile_value(kEnablerHudFlags);
    cfg->DxgiSpoofing = std::optional<bool> {};
    AplicaSpoof();
}

std::string Resumo()
{
    Varre();
    std::string s;
    auto junta = [&](bool tem, const char* nome)
    {
        if (!tem)
            return;
        if (!s.empty())
            s += ", ";
        s += nome;
    };
    junta(!g.dlssg.empty(), "DLSS-FG");
    junta(!g.dlssd.empty(), "DLSS-RR");
    junta(!g.ffxFg.empty(), "FSR 3 FG");
    junta(!g.xefg.empty(), "XeFG");
    junta(!g.dlss.empty(), "DLSS");
    junta(!g.fsr.empty(), "FSR");
    junta(!g.xess.empty(), "XeSS");
    return s.empty() ? std::string("nada (DLL propria de FG ou upscaler nao achada)") : s;
}

std::string Upscalers()
{
    Varre();
    std::string s;
    for (auto [tem, nome] : { std::pair { !g.dlss.empty(), "DLSS" }, std::pair { !g.fsr.empty(), "FSR" },
                              std::pair { !g.xess.empty(), "XeSS" } })
    {
        if (!tem)
            continue;
        if (!s.empty())
            s += ", ";
        s += nome;
    }
    return s;
}
} // namespace Lmxxf2AutoFg
