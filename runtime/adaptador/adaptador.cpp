// LmxxfNrRuntime.dll com a API do AMDNR (fork, LmxxfNrApi_amdnr.h) por cima do NOSSO runtime upstream do lmxxf
// (lmxxf_upstream.dll, ABI v1 do lmxxf). Permite usar o OptiScaler.dll do AMDNR, sem modificação, com a nossa rede.
//
// Tradução:
//   passes 1..3            -> DLSS5_MULTI_PASS (lido pelo upstream ao criar a rede; mudança recria a sessão)
//   FLAG_FULL_NETWORK      -> DLSS5_SKIP_BLOCKS vazio; sem ela, 42,43,46 (cronograma de produção do lmxxf)
//   FLAG_CONTROLS          -> escala das colunas da mistura de entrada do bloco 0 (block0-ffn.f32, mix[c*16+f]):
//                             estilo=6, tom=7, estrutura=11, pele=10, outro=14 (mapeamento medido visualmente,
//                             reports/lmxxf_port; pele/outro provisórios). Gera uma pasta de assets variante com
//                             links físicos para tudo e só o block0-ffn.f32 reescrito.
//   EnqueueHipAsync        -> EnqueueHip (espera na fila; sem atraso de quadro)
//   OutputReady            -> sempre pronto;  AbandonJob -> Drain + Retire
//   FLAG_TEMPORAL / VULKAN -> ignoradas; GetStatus acrescenta "hist=off"
#include <windows.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include "LmxxfNrApi_amdnr.h"

// ---- ABI v1 do lmxxf upstream (include/LmxxfNrApi.h), copiada aqui para não colidir com a do AMDNR ----
namespace up {
struct FrameInfo {
    uint32_t struct_size; uint64_t session_id, frame_id, list_generation; void *command_list;
    uint32_t color_width, color_height; void *color; uint32_t color_state, flags;
    float transfer_strength, color_strength; uint32_t debug_view; float model_scale;
    void *exposure; uint32_t exposure_state; float pre_exposure, exposure_scale;
};
struct Timings { uint32_t struct_size, valid; float network_ms; uint32_t reserved; uint64_t frame_id; };
struct Api {
    uint32_t struct_size, abi_version;
    int32_t (*QueryCapabilities)(LmxxfNrCapabilities *);
    int32_t (*Create)(const LmxxfNrCreateInfo *, void **);
    int32_t (*Destroy)(void *);
    int32_t (*PrepareSession)(void *);
    int32_t (*PrepareFrame)(void *, const FrameInfo *, LmxxfNrJob *);
    int32_t (*RecordInputs)(void *, void *, void *);
    int32_t (*EnqueueHip)(void *, void *, void *);
    int32_t (*RecordOutputs)(void *, void *, void *);
    int32_t (*ExecuteAfterProducer)(void *, void *, void *);
    int32_t (*CancelUnsubmitted)(void *, void *);
    int32_t (*Poll)(void *, void *, uint32_t *);
    int32_t (*Retire)(void *, void *);
    int32_t (*ResetHistory)(void *);
    int32_t (*Drain)(void *);
    int32_t (*GetStatus)(void *, char *, uint32_t);
    int32_t (*GetLastError)(char *, uint32_t);
    int32_t (*GetTimings)(void *, Timings *);
};
}  // namespace up

static up::Api U{};
static std::string g_err;
static std::mutex g_mu;

static void setErr(const std::string &e) { std::lock_guard<std::mutex> g(g_mu); g_err = e; }

struct Ctx {
    LmxxfNrCreateInfo ci{};
    int adiar = 0; // [lmxxf2] quadros que faltam até poder recriar a rede (ver A_PrepareFrame)
    std::wstring assets;          // pasta original passada pelo host
    std::wstring usedAssets;      // pasta efetivamente usada (original ou variante)
    void *inner = nullptr;
    int passes = 1; bool full = true; float ctl[5] = {1, 1, 1, 1, 1};  // tom, estrutura, pele, outro, estilo
    bool built = false;
    std::vector<float> base0; // [lmxxf2] block0-ffn.f32 original, para os controles sem recriar a rede
    std::vector<uint32_t> dynSkip; // [overlay] blocos pulados pela profundidade dinamica (pedido do host)
    bool skipSujo = true;          // o conjunto pulado (rede completa + dinamico) tem de ser reenviado ao runtime
    bool passesSujo = false;       // os passes mudaram e vao pela chamada direta (sem recriar a rede)
};

static std::wstring dirOf(HMODULE m) {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(m, p, MAX_PATH);
    std::wstring s(p); return s.substr(0, s.find_last_of(L"\\/"));
}

// pasta de assets variante com o block0-ffn.f32 escalado; o resto por link físico
static bool variante(Ctx *c, std::wstring *saida) {
    bool neutro = true;
    for (float v : c->ctl) neutro &= v == 1.f;
    if (neutro) { *saida = c->assets; return true; }
    wchar_t nome[128];
    swprintf(nome, 128, L"nr_ctl_%.3f_%.3f_%.3f_%.3f_%.3f", c->ctl[0], c->ctl[1], c->ctl[2], c->ctl[3], c->ctl[4]);
    // [overlay] uma subpasta por pasta de assets de origem: a rede pequena (native-game-tiled-assets-pequena) e a grande
    // tem pesos diferentes e nao podem dividir a mesma variante dos mesmos controles
    const size_t sep = c->assets.find_last_of(L"\\/");
    std::wstring base = c->assets.substr(0, sep) + L"\\nr_variantes";
    CreateDirectoryW(base.c_str(), nullptr);
    base += L"\\" + c->assets.substr(sep + 1);
    CreateDirectoryW(base.c_str(), nullptr);
    std::wstring dst = base + L"\\" + nome;
    CreateDirectoryW(dst.c_str(), nullptr);
    // links físicos de todos os arquivos da raiz (pesos, shaders), exceto block0-ffn.f32
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((c->assets + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring from = c->assets + L"\\" + n, to = dst + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // subpastas (HIP, shaders): junção
            if (GetFileAttributesW(to.c_str()) == INVALID_FILE_ATTRIBUTES) {
                std::wstring cmd = L"cmd /c mklink /J \"" + to + L"\" \"" + from + L"\" >nul";
                _wsystem(cmd.c_str());
            }
            continue;
        }
        if (n == L"block0-ffn.f32") continue;
        if (GetFileAttributesW(to.c_str()) == INVALID_FILE_ATTRIBUTES && !CreateHardLinkW(to.c_str(), from.c_str(), nullptr))
            CopyFileW(from.c_str(), to.c_str(), TRUE);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    // block0-ffn.f32: os 512 primeiros floats são a mistura 32x16
    FILE *f = _wfopen((c->assets + L"\\block0-ffn.f32").c_str(), L"rb");
    if (!f) return false;
    std::vector<float> w; float x;
    while (std::fread(&x, 4, 1, f) == 1) w.push_back(x);
    std::fclose(f);
    if (w.size() < 512) return false;
    const int col[5] = {7, 11, 10, 14, 6};
    for (int c32 = 0; c32 < 32; ++c32)
        for (int k = 0; k < 5; ++k) w[c32 * 16 + col[k]] *= c->ctl[k];
    f = _wfopen((dst + L"\\block0-ffn.f32").c_str(), L"wb");
    if (!f) return false;
    std::fwrite(w.data(), 4, w.size(), f); std::fclose(f);
    *saida = dst;
    return true;
}

static int32_t constroi(Ctx *c) {
    if (c->inner) { U.Drain(c->inner); U.Destroy(c->inner); c->inner = nullptr; }
    char v[8]; std::snprintf(v, sizeof v, "%d", c->passes);
    SetEnvironmentVariableA("DLSS5_MULTI_PASS", v);
    _putenv_s("DLSS5_MULTI_PASS", v);
    _putenv_s("DLSS5_MULTI_PASS_PREDICT", "0");
    _putenv_s("DLSS5_SKIP_BLOCKS", c->full ? "" : "42,43,46");
    SetEnvironmentVariableA("DLSS5_SKIP_BLOCKS", c->full ? "" : "42,43,46");
    if (!variante(c, &c->usedAssets)) { setErr("adaptador: falha ao montar a pasta variante dos controles"); return LMXXF_NR_FAILED; }
    LmxxfNrCreateInfo ci = c->ci; ci.struct_size = sizeof ci; ci.assets_directory = c->usedAssets.c_str(); ci.flags = 0;
    int32_t r = U.Create(&ci, &c->inner);
    if (r) return r;
    r = U.PrepareSession(c->inner);
    c->built = r == LMXXF_NR_OK;
    return r;
}

static unsigned long long g_n[8];
extern "C" { static void loga(const char *fmt, ...); }
static int32_t (*g_override)(void *, const char *, const float *, uint64_t); // [lmxxf2] LmxxfNrOverrideWeight do runtime
// [overlay] runtime v3: blocos pulados e passes por quadro, sem recriar a rede (o ambiente o runtime so le ao carregar)
static int32_t (*g_setSkip)(void *, const uint32_t *, uint32_t);
static int32_t (*g_setPasses)(void *, uint32_t);
// envia ao runtime o conjunto pulado: rede completa (sem 42,43,46) + profundidade dinamica
static void aplicaSkip(Ctx *c) {
    if (!g_setSkip || !c->inner || !c->skipSujo) return;
    std::vector<uint32_t> v = c->dynSkip;
    if (!c->full) for (uint32_t b : {42u, 43u, 46u}) if (std::find(v.begin(), v.end(), b) == v.end()) v.push_back(b);
    if (g_setSkip(c->inner, v.empty() ? nullptr : v.data(), (uint32_t)v.size()) == LMXXF_NR_OK) c->skipSujo = false;
}
// [lmxxf2] controles por escala das colunas da mistura do bloco 0 aplicados no lugar (sem Destroy/Create)
static bool controlesNoLugar(Ctx *c) {
    if (!g_override || !c->inner) return false;
    if (c->base0.empty()) {
        FILE *f = _wfopen((c->assets + L"\\block0-ffn.f32").c_str(), L"rb");
        if (!f) return false;
        float x; while (std::fread(&x, 4, 1, f) == 1) c->base0.push_back(x);
        std::fclose(f);
        if (c->base0.size() < 512) { c->base0.clear(); return false; }
    }
    std::vector<float> w = c->base0;
    const int col[5] = {7, 11, 10, 14, 6};
    for (int c32 = 0; c32 < 32; ++c32)
        for (int k = 0; k < 5; ++k) w[c32 * 16 + col[k]] *= c->ctl[k];
    const bool ok = g_override(c->inner, "block0-ffn.f32", w.data(), w.size()) == LMXXF_NR_OK;
    loga("controles no lugar %s: tom %.2f estrutura %.2f pele %.2f outro %.2f estilo %.2f\n", ok ? "ok" : "FALHOU", c->ctl[0], c->ctl[1], c->ctl[2], c->ctl[3], c->ctl[4]);
    return ok;
}
extern "C" {
static int32_t confere(int32_t r, const char *fn);
static int32_t A_Query(LmxxfNrCapabilities *o) { return U.QueryCapabilities(o); }
static int32_t A_Create(const LmxxfNrCreateInfo *ci, void **ctx) {
    if (!ci || !ctx) return LMXXF_NR_INVALID_ARGUMENT;
    auto *c = new Ctx; c->ci = *ci; c->assets = ci->assets_directory ? ci->assets_directory : L"";
    // O host aponta o LmxxfNrRuntime.pak oficial quando ele existe ao lado; a nossa rede lê só a pasta solta.
    DWORD at = GetFileAttributesW(c->assets.c_str());
    if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) {
        HMODULE self; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&dirOf, &self);
        c->assets = dirOf(self) + L"\\DLSS5-AMD\\native-game-tiled-assets";
    }
    int32_t r = constroi(c);
    if (r) { delete c; return r; }
    *ctx = c; return LMXXF_NR_OK;
}
static int32_t A_Destroy(void *ctx) { auto *c = (Ctx *)ctx; if (c && c->inner) U.Destroy(c->inner); delete c; return LMXXF_NR_OK; }
static int32_t A_PrepareSession(void *ctx) { return LMXXF_NR_OK; }
static int32_t A_PrepareFrame(void *ctx, const LmxxfNrFrameInfo *fi, LmxxfNrJob *job) {
    auto *c = (Ctx *)ctx;
    if (!c || !fi) return LMXXF_NR_INVALID_ARGUMENT;
    // campos do fork só existem se o host mandou a struct grande
    const bool temPasses = fi->struct_size >= offsetof(LmxxfNrFrameInfo, passes) + sizeof(uint32_t);
    const bool temCtl = fi->struct_size >= sizeof(LmxxfNrFrameInfo);
    int passes = temPasses && fi->passes ? (int)fi->passes : 1;
    if (passes < 1) passes = 1; if (passes > 3) passes = 3;
    bool full = (fi->flags & LMXXF_NR_FRAME_FLAG_FULL_NETWORK) != 0;
    float ctl[5] = {1, 1, 1, 1, 1};
    if (temCtl && (fi->flags & LMXXF_NR_FRAME_FLAG_CONTROLS)) {
        ctl[0] = fi->control_tone; ctl[1] = fi->control_structure; ctl[2] = fi->control_skin;
        ctl[3] = fi->control_other; ctl[4] = fi->control_style;
    }
    // [overlay] com o runtime v3, passes e rede completa mudam sem recriar a sessao
    if (g_setSkip && g_setPasses && c->built) {
        if (passes != c->passes) { c->passes = passes; c->passesSujo = true; }
        if (full != c->full) { c->full = full; c->skipSujo = true; }
    }
    const bool mudaRede = passes != c->passes || full != c->full;
    bool mudaCtl = false;
    for (int k = 0; k < 5; ++k) mudaCtl |= ctl[k] != c->ctl[k];
    if (mudaRede || mudaCtl) {
        c->passes = passes; c->full = full; std::memcpy(c->ctl, ctl, sizeof ctl);
        // [lmxxf2] só os controles mudaram: troca o peso do bloco 0 no lugar (sem travar recriando a rede).
        // Passes / rede completa precisam recriar a sessão, e NUNCA aqui: neste ponto a lista do jogo já tem gravado o
        // leitor da saída do job anterior (RecordOutputs vem antes do PrepareFrame) e ainda não foi submetida; destruir
        // a sessão agora deixa esse leitor apontando para memória liberada (GPU travada, DXGI_ERROR_DEVICE_HUNG).
        // Então: 2 quadros sem job (nenhum leitor novo; vale também com 2 slots) e a sessão é recriada no 3o.
        if (mudaRede || !controlesNoLugar(c)) c->adiar = 3;
    }
    if (c->adiar > 0) {
        if (--c->adiar > 0) {
            setErr("adaptador: recriando a rede - aguardando o leitor da saida anterior ser submetido");
            return LMXXF_NR_UNAVAILABLE;
        }
        int32_t r = constroi(c);
        loga("rede recriada sem leitor pendente: passes %d, rede completa %d (status %d)\n", c->passes, c->full ? 1 : 0, r);
        if (r) return r;
    }
    up::FrameInfo u{};
    u.struct_size = sizeof u; u.session_id = fi->session_id; u.frame_id = fi->frame_id; u.list_generation = fi->list_generation;
    u.command_list = fi->command_list; u.color_width = fi->color_width; u.color_height = fi->color_height;
    u.color = fi->color; u.color_state = fi->color_state;
    u.flags = fi->flags & (LMXXF_NR_FRAME_FLAG_STRENGTH | LMXXF_NR_FRAME_FLAG_DEBUG_VIEW | LMXXF_NR_FRAME_FLAG_CODEC_PASSTHROUGH);
    u.transfer_strength = fi->transfer_strength; u.color_strength = fi->color_strength;
    u.debug_view = fi->debug_view; u.model_scale = fi->model_scale > 0 ? fi->model_scale : 1.f;
    u.pre_exposure = 1.f; u.exposure_scale = 1.f;
    ++g_n[0];
    // [overlay] antes do PrepareFrame: vale para este quadro (a rede so roda no EnqueueHip). Sem a rede criada ainda
    // (primeiro quadro), o runtime recusa e fica sujo para o proximo.
    aplicaSkip(c);
    if (c->passesSujo && g_setPasses && g_setPasses(c->inner, (uint32_t)c->passes) == LMXXF_NR_OK) c->passesSujo = false;
    int32_t r = U.PrepareFrame(c->inner, &u, job);
    // [lmxxf2] trabalho anterior largado sem AbandonJob (lista descartada pelo jogo): o runtime fica "not Ready" para
    // sempre e a rede para de agir. Destrava (Drain + CancelUnsubmitted) e tenta de novo neste mesmo quadro.
    if (r == LMXXF_NR_UNAVAILABLE) {
        U.Drain(c->inner); U.CancelUnsubmitted(c->inner, nullptr); ++g_n[6];
        r = U.PrepareFrame(c->inner, &u, job);
    }
    return confere(r, "PrepareFrame");
}
// ---- log do adaptador (lmxxf_adaptador.log ao lado da DLL): contagem por chamada, erros e tempo do EnqueueHip ----
static FILE *g_log; static double g_enq_ms; static unsigned g_erros;
static void loga(const char *fmt, ...) {
    if (!g_log) { HMODULE self; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&loga, &self);
                  g_log = _wfopen((dirOf(self) + L"\\lmxxf_adaptador.log").c_str(), L"w"); if (!g_log) return; }
    va_list a; va_start(a, fmt); std::vfprintf(g_log, fmt, a); va_end(a); std::fflush(g_log);
}
static int32_t confere(int32_t r, const char *fn) {
    if (r && g_erros++ < 50) { char b[512] = {}; U.GetLastError(b, sizeof b); loga("ERRO %s status=%d: %s\n", fn, r, b); }
    return r;
}
static int32_t A_RecordInputs(void *ctx, void *job, void *cl) { ++g_n[1]; return confere(U.RecordInputs(((Ctx *)ctx)->inner, job, cl), "RecordInputs"); }
static int32_t A_EnqueueHip(void *ctx, void *job, void *q) {
    ++g_n[2]; LARGE_INTEGER t0, t1, f; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    int32_t r = confere(U.EnqueueHip(((Ctx *)ctx)->inner, job, q), "EnqueueHip");
    QueryPerformanceCounter(&t1); g_enq_ms += 1000.0 * double(t1.QuadPart - t0.QuadPart) / double(f.QuadPart);
    if (g_n[2] % 300 == 0) { loga("quadros: prepare=%llu inputs=%llu enqueue=%llu outputs=%llu retire=%llu abandon=%llu | enqueue medio %.2f ms | destravados %llu\n",
                                 g_n[0], g_n[1], g_n[2], g_n[3], g_n[4], g_n[5], g_enq_ms / 300.0, g_n[6]); g_enq_ms = 0; }
    return r;
}
static int32_t A_RecordOutputs(void *ctx, void *job, void *cl) { ++g_n[3]; return confere(U.RecordOutputs(((Ctx *)ctx)->inner, job, cl), "RecordOutputs"); }
static int32_t A_ExecuteAfterProducer(void *ctx, void *job, void *q) { return U.ExecuteAfterProducer(((Ctx *)ctx)->inner, job, q); }
static int32_t A_Cancel(void *ctx, void *job) { return U.CancelUnsubmitted(((Ctx *)ctx)->inner, job); }
static int32_t A_Poll(void *ctx, void *job, uint32_t *s) { return U.Poll(((Ctx *)ctx)->inner, job, s); }
static int32_t A_Retire(void *ctx, void *job) { ++g_n[4]; return confere(U.Retire(((Ctx *)ctx)->inner, job), "Retire"); }
static int32_t A_ResetHistory(void *ctx) { return U.ResetHistory(((Ctx *)ctx)->inner); }
static int32_t A_Drain(void *ctx) { return U.Drain(((Ctx *)ctx)->inner); }
static int32_t A_GetStatus(void *ctx, char *buf, uint32_t n) {
    int32_t r = U.GetStatus(((Ctx *)ctx)->inner, buf, n);
    if (buf && n) { size_t l = std::strlen(buf); std::snprintf(buf + l, n > l ? n - l : 0, " hist=off adaptador=1"); }
    return r;
}
static int32_t A_GetLastError(char *buf, uint32_t n) {
    { std::lock_guard<std::mutex> g(g_mu); if (!g_err.empty() && buf && n) { std::snprintf(buf, n, "%s", g_err.c_str()); g_err.clear(); return LMXXF_NR_OK; } }
    return U.GetLastError(buf, n);
}
static int32_t A_EnqueueHipAsync(void *ctx, void *job, void *q) { return A_EnqueueHip(ctx, job, q); }
// [lmxxf2] abandonado = a lista deste trabalho nunca vai rodar: CancelUnsubmitted (Retire deixava o runtime preso em "not Ready")
static int32_t A_AbandonJob(void *ctx, void *job) { ++g_n[5]; auto *c = (Ctx *)ctx; U.Drain(c->inner); U.CancelUnsubmitted(c->inner, job); return LMXXF_NR_OK; }
static int32_t A_OutputReady(void *, void *, uint32_t *ready) { if (ready) *ready = 1; return LMXXF_NR_OK; }

// [overlay] profundidade dinamica: blocos residuais pulados a partir do proximo quadro (nulos = os 71). 0 = aceito;
// LMXXF_NR_NOT_IMPLEMENTED = runtime sem a funcao (anterior ao v3).
__declspec(dllexport) int32_t LmxxfNrSetDynamicSkip(void *ctx, const uint32_t *blocks, uint32_t count) {
    auto *c = (Ctx *)ctx;
    if (!c) return LMXXF_NR_INVALID_ARGUMENT;
    if (!g_setSkip) return LMXXF_NR_NOT_IMPLEMENTED;
    std::vector<uint32_t> v(blocks, blocks + (blocks ? count : 0));
    if (v != c->dynSkip) { c->dynSkip = v; c->skipSujo = true; }
    return LMXXF_NR_OK;
}
// [overlay 2026-10-20] mascara por pixel na entrada da rede: liga e tabela de 40 floats (grupo g em table[g*8 + k], k =
// tom, estrutura, pele, outro, estilo/128); o alfa da cor leva grupo e peso. O runtime aplica antes do proximo quadro.
// LMXXF_NR_NOT_IMPLEMENTED = runtime sem a funcao (anterior ao v4).
static int32_t (*g_setMask)(void *, uint32_t, const float *, uint32_t);
__declspec(dllexport) int32_t LmxxfNrSetMask(void *ctx, uint32_t on, const float *table, uint32_t count) {
    auto *c = (Ctx *)ctx;
    if (!c) return LMXXF_NR_INVALID_ARGUMENT;
    if (!g_setMask) return LMXXF_NR_NOT_IMPLEMENTED;
    return g_setMask(c->inner ? c->inner : (void *)c, on, table, count); // o estado do runtime e global ao processo
}
__declspec(dllexport) int32_t LmxxfNrGetApi(uint32_t abi, LmxxfNrApi *out) {
    if (!out || abi != LMXXF_NR_ABI_VERSION) return LMXXF_NR_UNSUPPORTED_ABI;
    if (!U.struct_size) {
        HMODULE self; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&LmxxfNrGetApi, &self);
        HMODULE inner = LoadLibraryW((dirOf(self) + L"\\lmxxf_upstream.dll").c_str());
        if (!inner) return LMXXF_NR_UNAVAILABLE;
        auto get = (int32_t (*)(uint32_t, up::Api *))GetProcAddress(inner, "LmxxfNrGetApi");
        U.struct_size = sizeof U;
        if (!get || get(1, &U) != LMXXF_NR_OK) { U.struct_size = 0; return LMXXF_NR_UNAVAILABLE; }
        g_override = (int32_t (*)(void *, const char *, const float *, uint64_t))GetProcAddress(inner, "LmxxfNrOverrideWeight");
        g_setSkip = (int32_t (*)(void *, const uint32_t *, uint32_t))GetProcAddress(inner, "LmxxfNrSetDynamicSkip");
        g_setPasses = (int32_t (*)(void *, uint32_t))GetProcAddress(inner, "LmxxfNrSetPasses");
        g_setMask = (int32_t (*)(void *, uint32_t, const float *, uint32_t))GetProcAddress(inner, "LmxxfNrSetMask");
    }
    LmxxfNrApi a{};
    a.struct_size = sizeof a; a.abi_version = LMXXF_NR_ABI_VERSION;
    a.QueryCapabilities = A_Query; a.Create = A_Create; a.Destroy = A_Destroy; a.PrepareSession = A_PrepareSession;
    a.PrepareFrame = A_PrepareFrame; a.RecordInputs = A_RecordInputs; a.EnqueueHip = A_EnqueueHip;
    a.RecordOutputs = A_RecordOutputs; a.ExecuteAfterProducer = A_ExecuteAfterProducer; a.CancelUnsubmitted = A_Cancel;
    a.Poll = A_Poll; a.Retire = A_Retire; a.ResetHistory = A_ResetHistory; a.Drain = A_Drain; a.GetStatus = A_GetStatus;
    a.GetLastError = A_GetLastError; a.EnqueueHipAsync = A_EnqueueHipAsync; a.AbandonJob = A_AbandonJob; a.OutputReady = A_OutputReady;
    const uint32_t n = out->struct_size && out->struct_size < sizeof a ? out->struct_size : (uint32_t)sizeof a;
    std::memcpy(out, &a, n); out->struct_size = n;
    return LMXXF_NR_OK;
}
}
