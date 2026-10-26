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

ResultadoInjecao InjetaBuffer(DWORD pid, const std::wstring& dll)
{
    ResultadoInjecao r;
    if (!Existe(dll))
    {
        r.motivo = "DLSS5Buffer.dll nao encontrada (" + Utf8(dll) + ")";
        return r;
    }
    HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                               PROCESS_CREATE_THREAD,
                           FALSE, pid);
    if (!p)
    {
        r.motivo = GetLastError() == ERROR_ACCESS_DENIED
                       ? "sem acesso ao jogo (ele roda como administrador? abra o DLSS5 Overlay como administrador)"
                       : "nao consegui abrir o processo do jogo";
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
    if (!tem(L"d3d12.dll"))
    {
        r.motivo = "o jogo nao usa DirectX 12";
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

namespace {
// o lancamento e o evento pronto do processo `pid`, criados antes da DLL entrar (ela le o lancamento ao carregar)
struct Lancamento
{
    HANDLE mapa = nullptr, pronto = nullptr;
    bool Cria(DWORD pid, uint32_t geracao)
    {
        wchar_t nome[96];
        dlss5ov::NomeLancamento(nome, 96, pid);
        mapa = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(dlss5ov::Lancamento), nome);
        if (!mapa)
            return false;
        if (auto* l = static_cast<dlss5ov::Lancamento*>(MapViewOfFile(mapa, FILE_MAP_WRITE, 0, 0, sizeof(dlss5ov::Lancamento))))
        {
            l->versao = dlss5ov::kVersao;
            l->geracao = geracao;
            UnmapViewOfFile(l);
        }
        dlss5ov::NomeEventoPronto(nome, 96, pid);
        pronto = CreateEventW(nullptr, TRUE, FALSE, nome);
        return pronto != nullptr;
    }
    ~Lancamento()
    {
        if (mapa)
            CloseHandle(mapa);
        if (pronto)
            CloseHandle(pronto);
    }
};

// LoadLibraryW numa thread do processo (kernel32 tem o mesmo endereco em todos os processos desta sessao)
std::string CarregaDll(HANDLE p, const std::wstring& dll)
{
    const size_t tam = (dll.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(p, nullptr, tam, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem || !WriteProcessMemory(p, mem, dll.c_str(), tam, nullptr))
    {
        if (mem)
            VirtualFreeEx(p, mem, 0, MEM_RELEASE);
        return "nao consegui escrever no processo do jogo";
    }
    auto* carrega = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE t = CreateRemoteThread(p, nullptr, 0, carrega, mem, 0, nullptr);
    if (!t)
    {
        VirtualFreeEx(p, mem, 0, MEM_RELEASE);
        return "o jogo recusou a thread de carga (CreateRemoteThread)";
    }
    const DWORD espera = WaitForSingleObject(t, 10000);
    DWORD saida = 0;
    GetExitCodeThread(t, &saida);
    CloseHandle(t);
    if (espera == WAIT_OBJECT_0)
        VirtualFreeEx(p, mem, 0, MEM_RELEASE);
    if (espera != WAIT_OBJECT_0)
        return "a carga da DLL no jogo nao terminou em 10 s";
    return saida ? std::string() : "o jogo nao carregou a DLL";
}
} // namespace

ResultadoInjecao AbreJogo(const std::wstring& exe, const std::wstring& dll, uint32_t geracao, DWORD& pid,
                          const std::wstring& args)
{
    ResultadoInjecao r;
    pid = 0;
    if (!Existe(dll))
        return r.motivo = "DLSS5Buffer.dll nao encontrada (" + Utf8(dll) + ")", r;
    if (!Existe(exe))
        return r.motivo = "o executavel nao existe (" + Utf8(exe) + ")", r;
    const std::wstring pasta = exe.substr(0, exe.find_last_of(L"\\/"));
    const std::wstring ac = AntiCheatNaPasta(pasta);
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::wstring linha = L"\"" + exe + L"\"" + (args.empty() ? L"" : L" " + args);
    if (!CreateProcessW(exe.c_str(), linha.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, pasta.c_str(), &si, &pi))
        return r.motivo = Fmt("o jogo nao abriu (erro %lu)", GetLastError()), r;
    pid = pi.dwProcessId;
    Lancamento l;
    std::string erro = l.Cria(pid, geracao) ? CarregaDll(pi.hProcess, dll) : "nao criei o lancamento";
    if (erro.empty() && WaitForSingleObject(l.pronto, 5000) != WAIT_OBJECT_0)
        erro = "o modulo nao enganchou a criacao do swapchain em 5 s";
    // solta o jogo mesmo sem o modulo (ele abre normal, sem geracao): fechar o que o usuario mandou abrir seria pior
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (!erro.empty())
        return r.motivo = erro + " (o jogo abriu sem a geracao)", r;
    r.ok = true;
    if (!ac.empty())
        r.motivo = "aviso: o jogo tem anti-cheat instalado (" + Utf8(ac) + "): use so offline";
    Log("buffer", Fmt("jogo aberto pelo overlay com o modulo (pid %lu, geracao %ux): %s", (unsigned long)pid, geracao,
                      Utf8(exe).c_str()));
    return r;
}

ResultadoInjecao EntraNoInicio(DWORD pid, const std::wstring& dll, uint32_t geracao)
{
    ResultadoInjecao r;
    HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                               PROCESS_CREATE_THREAD,
                           FALSE, pid);
    if (!p)
        return r.motivo = "sem acesso ao jogo reaberto", r;
    Lancamento l;
    std::string erro = l.Cria(pid, geracao) ? CarregaDll(p, dll) : "nao criei o lancamento";
    if (erro.empty() && WaitForSingleObject(l.pronto, 5000) != WAIT_OBJECT_0)
        erro = "o modulo nao enganchou a criacao do swapchain em 5 s";
    CloseHandle(p);
    if (!erro.empty())
        return r.motivo = erro, r;
    r.ok = true;
    Log("buffer", Fmt("modulo no jogo reaberto pela Steam logo no inicio (pid %lu, geracao %ux)", (unsigned long)pid, geracao));
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

namespace {
std::wstring DxgiDoJogo(const std::wstring& exe) { return exe.substr(0, exe.find_last_of(L"\\/")) + L"\\dxgi.dll"; }
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
    static const char marca[] = "DLSS5Ov_Modulo"; // o nome exportado pelo modulo
    return std::search(v.begin(), v.end(), marca, marca + sizeof marca - 1) != v.end();
}
} // namespace

Instalacao EstadoInstalacao(const std::wstring& exe, const std::wstring& dll)
{
    if (exe.empty())
        return Instalacao::Nenhuma;
    const std::vector<char> la = LeArquivo(DxgiDoJogo(exe));
    if (la.empty())
        return Instalacao::Nenhuma;
    if (!EhNossa(la))
        return Instalacao::Outra;
    return la == LeArquivo(dll) ? Instalacao::Nossa : Instalacao::Desatualizada;
}

std::string InstalaNoJogo(const std::wstring& exe, const std::wstring& dll)
{
    if (!Existe(dll))
        return "DLSS5Buffer.dll nao encontrada (" + Utf8(dll) + ")";
    const std::wstring dst = DxgiDoJogo(exe);
    const Instalacao e = EstadoInstalacao(exe, dll);
    if (e == Instalacao::Outra)
        return "ja existe uma dxgi.dll de outro programa na pasta do jogo (ReShade, OptiScaler?): nao substitui";
    if (e == Instalacao::Nossa)
        return "";
    if (CopyFileW(dll.c_str(), dst.c_str(), FALSE))
        return "";
    // em uso (o jogo esta aberto com a versao antiga): o arquivo carregado pode ser renomeado; a nova vale ao reabrir
    const std::wstring velha = dst + L".antiga." + std::to_wstring(GetTickCount64());
    if (e == Instalacao::Desatualizada && MoveFileExW(dst.c_str(), velha.c_str(), 0) && CopyFileW(dll.c_str(), dst.c_str(), FALSE))
        return "";
    return Fmt("nao consegui copiar para a pasta do jogo (erro %lu; pasta protegida? abra o overlay como administrador)",
               GetLastError());
}

std::string RemoveDoJogo(const std::wstring& exe)
{
    const std::wstring dst = DxgiDoJogo(exe);
    if (!EhNossa(LeArquivo(dst)))
        return "a dxgi.dll da pasta do jogo nao e a do overlay: nao mexi";
    // para a Lixeira (desfaz), nunca apagada de vez
    std::wstring de = dst + L'\0';
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = de.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted)
        return "";
    return "nao consegui tirar a dxgi.dll (o jogo esta aberto? feche e tente de novo)";
}

DWORD ProcuraProcesso(const std::wstring& nomeExe, DWORD exceto)
{
    const std::wstring alvo = Minusculo(nomeExe);
    DWORD achou = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W e{sizeof e};
    for (BOOL ok = Process32FirstW(s, &e); ok && !achou; ok = Process32NextW(s, &e))
        if (e.th32ProcessID != exceto && Minusculo(e.szExeFile) == alvo)
            achou = e.th32ProcessID;
    CloseHandle(s);
    return achou;
}

} // namespace ov
