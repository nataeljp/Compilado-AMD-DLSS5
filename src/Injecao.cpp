#include "Injecao.h"
#include "buffer/Protocolo.h"
#include <psapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cwctype>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace ov {

namespace {
std::wstring Minusculo(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    return s;
}

std::vector<std::wstring> Modulos(HANDLE p)
{
    std::vector<std::wstring> r;
    HMODULE m[1024];
    DWORD n = 0;
    if (!EnumProcessModulesEx(p, m, sizeof m, &n, LIST_MODULES_64BIT))
        return r;
    for (DWORD i = 0; i < n / sizeof(HMODULE); ++i)
    {
        wchar_t b[MAX_PATH];
        if (GetModuleBaseNameW(p, m[i], b, MAX_PATH))
            r.push_back(Minusculo(b));
    }
    return r;
}

bool Existe(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// anti-cheat na pasta do jogo (ou ate 3 niveis acima: em Unreal o exe fica em <jogo>\<Projeto>\Binaries\Win64)
std::wstring AntiCheatNaPasta(std::wstring pasta)
{
    for (int i = 0; i < 4 && !pasta.empty(); ++i)
    {
        for (const wchar_t* a : {L"EasyAntiCheat", L"BattlEye", L"EAAntiCheat.GameServiceLauncher.exe", L"EAAntiCheat.cfg",
                                 L"start_protected_game.exe", L"GameGuard", L"XIGNCODE", L"FACEIT"})
            if (Existe(pasta + L"\\" + a))
                return a;
        const auto k = pasta.find_last_of(L"\\/");
        if (k == std::wstring::npos)
            break;
        pasta.resize(k);
    }
    return {};
}
// servico/processo de anti-cheat rodando agora
std::wstring AntiCheatRodando()
{
    std::wstring achou;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE)
        return achou;
    PROCESSENTRY32W e{sizeof e};
    for (BOOL ok = Process32FirstW(s, &e); ok && achou.empty(); ok = Process32NextW(s, &e))
    {
        const std::wstring n = Minusculo(e.szExeFile);
        for (const wchar_t* a : {L"easyanticheat", L"beservice", L"bedaisy", L"eaanticheat", L"vgc.exe", L"faceit",
                                 L"gamemon", L"xxd-0.xem", L"ricochet", L"mhyprot"})
            if (n.find(a) != std::wstring::npos)
            {
                achou = e.szExeFile;
                break;
            }
    }
    CloseHandle(s);
    return achou;
}
} // namespace

ResultadoInjecao InjetaBuffer(DWORD pid, const std::wstring& dll, bool qualquerDx)
{
    ResultadoInjecao r;
    if (!Existe(dll))
    {
        r.motivo = "DLSS5Buffer.dll nao encontrada (" + Utf8(dll) + ")";
        return r;
    }
    // ja esta no jogo: nao precisa de acesso nenhum ao processo (o overlay fala pela memoria compartilhada)
    if (ModuloPresente(pid))
    {
        r.ok = true;
        return r;
    }
    HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                               PROCESS_CREATE_THREAD,
                           FALSE, pid);
    if (!p)
    {
        const DWORD e = GetLastError();
        r.motivo = e == ERROR_ACCESS_DENIED
                       ? "sem acesso ao jogo (ele roda como administrador? abra o DLSS5 Overlay como administrador)"
                       : Fmt("nao consegui abrir o processo do jogo (erro %lu)", e);
        return r;
    }
    BOOL wow = FALSE;
    IsWow64Process(p, &wow);
    std::wstring exe;
    {
        wchar_t b[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, b, &n))
            exe.assign(b, n);
    }
    const std::vector<std::wstring> mods = Modulos(p);
    auto tem = [&](const wchar_t* parte) {
        return std::any_of(mods.begin(), mods.end(), [&](const std::wstring& m) { return m.find(parte) != std::wstring::npos; });
    };
    // 1) anti-cheat: nunca (risco de banimento)
    std::wstring ac;
    for (const wchar_t* a : {L"easyanticheat", L"beclient", L"battleye", L"eaanticheat", L"eaac", L"javelin", L"gameguard",
                             L"xigncode", L"faceit", L"anticheat", L"vgk", L"nprotect"})
        if (tem(a))
        {
            ac = a;
            break;
        }
    // anti-cheat rodando (o servico/processo dele sobe junto com o jogo protegido): o risco de banimento e esse
    if (ac.empty())
        ac = AntiCheatRodando();
    if (!ac.empty())
    {
        r.motivo = "o anti-cheat esta ativo (" + Utf8(ac) + "): colocar codigo no jogo pode dar banimento. Use a captura "
                   "de video";
        CloseHandle(p);
        return r;
    }
    // instalado na pasta, mas nada dele rodando (o jogo foi aberto sem ele, modo offline): conecta, com aviso
    std::wstring acPasta;
    if (!exe.empty())
        acPasta = AntiCheatNaPasta(exe.substr(0, exe.find_last_of(L"\\/")));
    if (wow)
    {
        r.motivo = "o jogo e 32 bits (so 64 bits)";
        CloseHandle(p);
        return r;
    }
    if (!tem(L"d3d12.dll") && !(qualquerDx && tem(L"d3d11.dll")))
    {
        r.vulkan = qualquerDx && tem(L"vulkan-1.dll");
        r.motivo = !qualquerDx ? "o jogo nao usa DirectX 12"
                   : r.vulkan  ? "o jogo usa Vulkan e abriu sem a camada: marque Vulkan aqui embaixo e abra o jogo de novo"
                               : "o jogo nao usa DirectX 11, 12 nem Vulkan. Use a captura de video";
        CloseHandle(p);
        return r;
    }
    for (const wchar_t* u : {L"amd_fidelityfx_upscaler_dx12.dll", L"amd_fidelityfx_dx12.dll", L"amd_fidelityfx_loader_dx12.dll",
                             L"libxess.dll"})
        if (tem(u))
        {
            r.upscaler = Utf8(u);
            break;
        }
    // OptiScaler instalado na pasta do jogo assume o upscaler dele: avisa (a captura pode nao ver o FSR do jogo)
    if (!exe.empty())
    {
        const std::wstring pasta = exe.substr(0, exe.find_last_of(L"\\/"));
        if (Existe(pasta + L"\\OptiScaler.ini"))
            r.motivo = "aviso: o OptiScaler esta instalado na pasta do jogo e assume o upscaler; se nao conectar, tire o "
                       "dxgi.dll dele de la";
    }
    if (!acPasta.empty())
        r.motivo += std::string(r.motivo.empty() ? "" : " | ") + "aviso: o jogo tem anti-cheat instalado (" + Utf8(acPasta) +
                    "), inativo agora: use a captura de buffer so offline";
    if (tem(L"dlss5buffer.dll") || ModuloPresente(pid))
    {
        r.ok = true; // ja esta no jogo (ligou de novo, ou instalado na pasta dele como dxgi.dll)
        CloseHandle(p);
        return r;
    }
    // 2) LoadLibraryW numa thread do jogo (kernel32 tem o mesmo endereco em todos os processos desta sessao)
    const size_t tam = (dll.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(p, nullptr, tam, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem || !WriteProcessMemory(p, mem, dll.c_str(), tam, nullptr))
    {
        r.motivo = "nao consegui escrever no processo do jogo";
        if (mem)
            VirtualFreeEx(p, mem, 0, MEM_RELEASE);
        CloseHandle(p);
        return r;
    }
    auto* carrega = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE t = CreateRemoteThread(p, nullptr, 0, carrega, mem, 0, nullptr);
    if (!t)
    {
        r.motivo = "o jogo recusou a thread de carga (CreateRemoteThread)";
        VirtualFreeEx(p, mem, 0, MEM_RELEASE);
        CloseHandle(p);
        return r;
    }
    const DWORD espera = WaitForSingleObject(t, 10000);
    DWORD saida = 0;
    GetExitCodeThread(t, &saida);
    CloseHandle(t);
    if (espera == WAIT_OBJECT_0)
        VirtualFreeEx(p, mem, 0, MEM_RELEASE);
    CloseHandle(p);
    if (espera != WAIT_OBJECT_0 || !saida)
    {
        r.motivo = espera != WAIT_OBJECT_0 ? "a carga da DLL no jogo nao terminou em 10 s" : "o jogo nao carregou a DLL";
        return r;
    }
    r.ok = true;
    Log("buffer", Fmt("DLSS5Buffer.dll carregada no jogo (pid %lu)%s", (unsigned long)pid,
                      r.upscaler.empty() ? "; o jogo ainda nao carregou FSR/XeSS" : (", upscaler: " + r.upscaler).c_str()));
    return r;
}

bool ModuloPresente(DWORD pid)
{
    wchar_t nome[96];
    dlss5ov::NomeModulo(nome, 96, pid);
    HANDLE e = OpenEventW(SYNCHRONIZE, FALSE, nome);
    if (e)
        CloseHandle(e);
    return e != nullptr;
}

DWORD ProcessoComModulo(DWORD exceto)
{
    DWORD achou = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W e{sizeof e};
    for (BOOL ok = Process32FirstW(s, &e); ok && !achou; ok = Process32NextW(s, &e))
    {
        if (e.th32ProcessID == exceto || e.th32ProcessID == GetCurrentProcessId() || !ModuloPresente(e.th32ProcessID))
            continue;
        wchar_t nome[96];
        dlss5ov::NomeMapa(nome, 96, e.th32ProcessID);
        if (HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, nome))
        {
            CloseHandle(m);
            achou = e.th32ProcessID;
        }
    }
    CloseHandle(s);
    return achou;
}

std::wstring ExeDoProcesso(DWORD pid)
{
    std::wstring exe;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
    {
        wchar_t b[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, b, &n))
            exe.assign(b, n);
        CloseHandle(p);
    }
    return exe;
}

DWORD ProcessoDoExe(const std::wstring& exe, DWORD exceto)
{
    const std::wstring alvo = Minusculo(exe), nome = alvo.substr(alvo.find_last_of(L"\\/") + 1);
    DWORD achou = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W e{sizeof e};
    for (BOOL ok = Process32FirstW(s, &e); ok && !achou; ok = Process32NextW(s, &e))
        if (e.th32ProcessID != exceto && Minusculo(e.szExeFile) == nome && Minusculo(ExeDoProcesso(e.th32ProcessID)) == alvo)
            achou = e.th32ProcessID;
    CloseHandle(s);
    return achou;
}

namespace {
bool ParaLixeira(const std::wstring& arq)
{
    std::wstring de = arq + L'\0';
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = de.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}
std::vector<char> LeArquivo(const std::wstring& a)
{
    std::vector<char> v;
    HANDLE h = CreateFileW(a.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return v;
    LARGE_INTEGER t{};
    GetFileSizeEx(h, &t);
    v.resize(size_t(t.QuadPart));
    DWORD lidos = 0;
    if (!v.empty() && !ReadFile(h, v.data(), DWORD(v.size()), &lidos, nullptr))
        v.clear();
    CloseHandle(h);
    return v;
}
bool EhNossa(const std::vector<char>& v)
{
    static const char marca[] = "DLSS5Ov_Modulo"; // o nome que as versoes com a geracao exportavam
    return std::search(v.begin(), v.end(), marca, marca + sizeof marca - 1) != v.end();
}
} // namespace

std::string LimpaPastaDoJogo(const std::wstring& exe)
{
    if (exe.empty())
        return "";
    const std::wstring pasta = exe.substr(0, exe.find_last_of(L"\\/")) + L"\\";
    int lixeira = 0, renomeadas = 0, presas = 0;
    // as copias velhas (de atualizacoes com o jogo aberto, ou renomeadas aqui antes)
    for (const wchar_t* filtro : {L"version.dll.antiga.*", L"dxgi.dll.antiga.*", L"version.dll.removida.*", L"dxgi.dll.removida.*"})
    {
        WIN32_FIND_DATAW d;
        HANDLE f = FindFirstFileW((pasta + filtro).c_str(), &d);
        if (f == INVALID_HANDLE_VALUE)
            continue;
        do
            if (EhNossa(LeArquivo(pasta + d.cFileName)) && ParaLixeira(pasta + d.cFileName))
                ++lixeira; // a que nao saiu (ainda em uso pelo jogo aberto) tenta de novo na proxima conexao
        while (FindNextFileW(f, &d));
        FindClose(f);
    }
    bool tinha = false;
    for (const wchar_t* n : {L"version.dll", L"dxgi.dll"})
    {
        const std::wstring arq = pasta + n;
        if (!EhNossa(LeArquivo(arq)))
            continue; // nao existe, ou e de outro programa (Cyber Engine Tweaks, ReShade, OptiScaler): nao mexe
        tinha = true;
        if (ParaLixeira(arq))
            ++lixeira;
        else if (MoveFileExW(arq.c_str(), (arq + L".removida." + std::to_wstring(GetTickCount64())).c_str(), 0))
            ++renomeadas;
        else
            ++presas;
    }
    if (tinha && !presas && !renomeadas && Existe(pasta + L"DLSS5Overlay.alvo"))
        ParaLixeira(pasta + L"DLSS5Overlay.alvo") && ++lixeira;
    if (!lixeira && !renomeadas && !presas)
        return "";
    std::string m = Fmt("DLL antiga da geração de quadros na pasta do jogo: %d na Lixeira", lixeira);
    if (renomeadas)
        m += Fmt(", %d em uso (não carrega mais; reabra o jogo)", renomeadas);
    if (presas)
        m += Fmt(", %d não saiu (pasta protegida? abra o overlay como administrador)", presas);
    return m;
}

} // namespace ov
