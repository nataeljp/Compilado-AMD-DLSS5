// Captura da API no Vulkan: camada implicita do carregador (HKCU\SOFTWARE\Khronos\Vulkan\ImplicitLayers), registrada so enquanto o
// overlay esta aberto com a opcao ligada. Nada vai para a pasta do jogo. Usa a mesma memoria compartilhada e as mesmas texturas
// nomeadas da captura do Present (buffer/ProtocoloApi.h): do lado do overlay, nada muda.
// Sem o overlay aberto ou com anti-cheat ativo a camada recusa a negociacao, e o carregador segue sem ela.
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>
#include <vulkan/vk_layer.h>
#include "../buffer/LimiteFps.h"
#include "../buffer/ProtocoloApi.h"
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#undef CreateSemaphore

#pragma comment(lib, "psapi.lib")
#pragma comment(linker, "/EXPORT:vkNegotiateLoaderLayerInterfaceVersion")

namespace {
using Microsoft::WRL::ComPtr;
using dlss5ov::CabApi;
constexpr uint32_t kSlots = dlss5ov::kSlotsApi;
constexpr VkExternalMemoryHandleTypeFlagBits kTipo = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;

#define FUNCOES_INST(X)                                                                                                                  \
    X(DestroyInstance) X(EnumerateDeviceExtensionProperties) X(GetPhysicalDeviceProperties2) X(GetPhysicalDeviceImageFormatProperties2)  \
    X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceSurfaceCapabilitiesKHR)            \
    X(CreateWin32SurfaceKHR) X(DestroySurfaceKHR)
#define FUNCOES_DISP(X)                                                                                                                  \
    X(DestroyDevice) X(GetDeviceQueue) X(GetDeviceQueue2) X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR)          \
    X(QueuePresentKHR) X(QueueSubmit) X(WaitForFences) X(ResetFences) X(CreateFence) X(DestroyFence) X(CreateSemaphore)                  \
    X(DestroySemaphore) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(FreeCommandBuffers)                       \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyImage) X(CmdBlitImage) X(CreateImage) \
    X(DestroyImage) X(AllocateMemory) X(FreeMemory) X(BindImageMemory) X(GetImageMemoryRequirements) X(GetMemoryWin32HandlePropertiesKHR)

struct Inst
{
    VkInstance inst = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
#define X(n) PFN_vk##n n = nullptr;
    FUNCOES_INST(X)
#undef X
};

// o que se usa para copiar uma imagem do swapchain: uma por imagem, porque o semaforo so fica livre quando ela e adquirida de novo
struct Quadro
{
    VkCommandBuffer cb = VK_NULL_HANDLE;
    uint32_t fam = 0;
    VkFence f = VK_NULL_HANDLE;
    VkSemaphore s = VK_NULL_HANDLE;
    std::atomic<bool> ocupado{false};
};

struct Troca
{
    std::vector<VkImage> imgs;
    std::vector<Quadro*> q;
    VkExtent2D ext{};
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR espaco = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    HWND raiz = nullptr;
    bool copiavel = false;
};

struct Dev
{
    VkDevice dev = VK_NULL_HANDLE;
    VkPhysicalDevice fis = VK_NULL_HANDLE;
    Inst* inst = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkSetDeviceLoaderData setData = nullptr;
    bool extOk = false;
    std::vector<VkQueueFamilyProperties> fams;
    std::unordered_map<VkQueue, uint32_t> filas;
    std::unordered_map<VkSwapchainKHR, Troca*> trocas;
    std::unordered_map<uint32_t, VkCommandPool> pools;
#define X(n) PFN_vk##n n = nullptr;
    FUNCOES_DISP(X)
#undef X
};

struct Pend
{
    Dev* d = nullptr;
    Quadro* q = nullptr;
    ComPtr<ID3D12Fence> cerca;
    uint64_t n = 0;
};

// nunca destruido: na saida do processo o destrutor soltaria objetos D3D12 num driver que ja saiu
struct Estado
{
    std::mutex mx; // os mapas
    std::unordered_map<void*, Inst*> insts;
    std::unordered_map<void*, Dev*> devs;
    std::unordered_map<VkSurfaceKHR, HWND> superficies;
    std::mutex mxCap; // a captura: texturas, quadros e o que o overlay le
    CabApi* cab = nullptr;
    ULONGLONG vivoEm = 0, tentaEm = 0, alvoVistoEm = 0;
    uint32_t geracao = 0;
    uint64_t n = 0;
    Dev* dono = nullptr;
    ComPtr<ID3D12Device> d3d;
    ComPtr<ID3D12Resource> tex[kSlots];
    ComPtr<ID3D12Fence> cerca;
    HANDLE handles[kSlots + 1] = {};
    VkImage img[kSlots] = {};
    VkDeviceMemory mem[kSlots] = {};
    uint32_t w = 0, h = 0;
    VkFormat vk = VK_FORMAT_UNDEFINED;
    std::mutex mxF;
    std::condition_variable cvF;
    std::deque<Pend> pend;
    bool ajudante = false;
    char erroDito[160] = {};
};
Estado& g = *new Estado;

template <class T> void* Chave(T h) { return *reinterpret_cast<void**>(h); }

void Log(const char* f, ...)
{
    static FILE* a = nullptr;
    if (!a)
    {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&Log), &m);
        wchar_t c[MAX_PATH];
        GetModuleFileNameW(m, c, MAX_PATH);
        std::wstring p = c;
        p.resize(p.find_last_of(L"\\/") + 1);
        a = _wfsopen((p + L"DLSS5Camada.log").c_str(), L"a", 0x40 /* _SH_DENYNO */);
        if (!a)
            return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(a, "[%02u:%02u:%02u.%03u pid %lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId());
    va_list l;
    va_start(l, f);
    vfprintf(a, f, l);
    va_end(l);
    fputc('\n', a);
    fflush(a);
}

void Erro(const char* oque, long codigo)
{
    snprintf(g.cab->erro, sizeof g.cab->erro, "%s: 0x%08lx", oque, static_cast<unsigned long>(codigo));
    g.tentaEm = GetTickCount64() + 2000; // nao tenta de novo a cada quadro
    if (strcmp(g.erroDito, g.cab->erro))
    {
        strcpy_s(g.erroDito, g.cab->erro);
        Log("%s", g.cab->erro);
    }
}

bool Contem(const std::wstring& nome, const wchar_t* parte) { return nome.find(parte) != std::wstring::npos; }
std::wstring Minusculo(std::wstring s)
{
    for (auto& c : s)
        c = wchar_t(towlower(c));
    return s;
}

// a mesma regra da injecao (Injecao.cpp): colocar codigo num jogo com anti-cheat pode dar banimento
bool AntiCheatAtivo()
{
    HMODULE m[1024];
    DWORD n = 0;
    if (EnumProcessModulesEx(GetCurrentProcess(), m, sizeof m, &n, LIST_MODULES_64BIT))
        for (DWORD i = 0; i < n / sizeof(HMODULE); ++i)
        {
            wchar_t b[MAX_PATH];
            if (!GetModuleBaseNameW(GetCurrentProcess(), m[i], b, MAX_PATH))
                continue;
            const std::wstring nome = Minusculo(b);
            for (const wchar_t* a : {L"easyanticheat", L"beclient", L"battleye", L"eaanticheat", L"eaac", L"javelin", L"gameguard", L"xigncode",
                                     L"faceit", L"anticheat", L"vgk", L"nprotect"})
                if (Contem(nome, a))
                    return true;
        }
    bool achou = false;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W e{sizeof e};
    for (BOOL ok = Process32FirstW(s, &e); ok && !achou; ok = Process32NextW(s, &e))
    {
        const std::wstring nome = Minusculo(e.szExeFile);
        for (const wchar_t* a : {L"easyanticheat", L"beservice", L"bedaisy", L"eaanticheat", L"vgc.exe", L"faceit", L"gamemon", L"xxd-0.xem",
                                 L"ricochet", L"mhyprot"})
            if (Contem(nome, a))
                achou = true;
    }
    CloseHandle(s);
    return achou;
}

bool OverlayAberto()
{
    HANDLE e = OpenEventW(SYNCHRONIZE, FALSE, dlss5ov::kEventoCamada);
    if (e)
        CloseHandle(e);
    return e != nullptr;
}

Inst* AchaInst(void* chave)
{
    std::lock_guard<std::mutex> lk(g.mx);
    const auto it = g.insts.find(chave);
    if (it != g.insts.end())
        return it->second;
    return g.insts.size() == 1 ? g.insts.begin()->second : nullptr;
}
Dev* AchaDev(void* chave)
{
    std::lock_guard<std::mutex> lk(g.mx);
    const auto it = g.devs.find(chave);
    return it != g.devs.end() ? it->second : nullptr;
}

// a memoria compartilhada sai no primeiro swapchain, nao na instancia: jogo DirectX que so sonda o Vulkan segue pela injecao
void AbreMapa()
{
    if (g.cab)
        return;
    wchar_t nome[96];
    dlss5ov::NomeMapaApi(nome, 96, GetCurrentProcessId());
    HANDLE mapa = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(CabApi), nome);
    if (!mapa)
        return;
    auto* c = static_cast<CabApi*>(MapViewOfFile(mapa, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CabApi)));
    if (!c)
        return;
    c->versao = dlss5ov::kVersaoApi;
    c->tamanho = sizeof(CabApi);
    g.cab = c;
    // o carregador solta a DLL ao destruir a ultima instancia: com a memoria aberta e a thread de ajuda, ela fica ate o fim do processo
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&AbreMapa), &m);
    Log("camada ativa");
}

// a GPU do jogo terminou a copia: avisa o overlay pela cerca do D3D12. Sem semaforo de linha do tempo importado, o dispositivo do jogo
// nao precisa de nenhum recurso a mais alem da memoria externa
DWORD WINAPI Ajudante(void*)
{
    for (;;)
    {
        Pend p;
        {
            std::unique_lock<std::mutex> lk(g.mxF);
            g.cvF.wait(lk, [] { return !g.pend.empty(); });
            p = std::move(g.pend.front());
            g.pend.pop_front();
        }
        VkResult r;
        do
            r = p.d->WaitForFences(p.d->dev, 1, &p.q->f, VK_TRUE, 200000000ULL);
        while (r == VK_TIMEOUT);
        if (r == VK_SUCCESS)
            p.cerca->Signal(p.n);
        p.q->ocupado = false;
    }
}

// ate a ajudante largar o quadro: ela larga depois da cerca ou da perda do dispositivo (WaitForFences devolve nos dois). Desistir em
// 2 s e destruir era uso depois de liberar: a ajudante ainda esperava nessa cerca e escrevia no quadro
void EsperaQuadros(Troca& t)
{
    for (Quadro* q : t.q)
        while (q->ocupado)
            Sleep(1);
}

void Solta()
{
    if (!g.dono)
        return;
    Dev& d = *g.dono;
    g.cab->geracao = 0;
    for (auto& par : d.trocas)
        EsperaQuadros(*par.second);
    for (uint32_t s = 0; s < kSlots; ++s)
    {
        if (g.img[s])
            d.DestroyImage(d.dev, g.img[s], nullptr);
        if (g.mem[s])
            d.FreeMemory(d.dev, g.mem[s], nullptr);
        g.img[s] = VK_NULL_HANDLE;
        g.mem[s] = VK_NULL_HANDLE;
        g.tex[s].Reset();
    }
    for (HANDLE& h : g.handles)
    {
        if (h)
            CloseHandle(h);
        h = nullptr;
    }
    g.cerca.Reset();
    g.w = g.h = 0;
    g.vk = VK_FORMAT_UNDEFINED;
    g.dono = nullptr;
}

struct MapaFmt
{
    DXGI_FORMAT dx;
    VkFormat vk; // a imagem importada
    bool blit;   // canais em outra ordem: a copia crua trocaria vermelho e azul
};
bool Formato(VkFormat f, MapaFmt& m)
{
    switch (f)
    {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB: m = {DXGI_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, false}; return true;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: m = {DXGI_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, false}; return true;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: m = {DXGI_FORMAT_R10G10B10A2_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32, false}; return true;
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: m = {DXGI_FORMAT_R10G10B10A2_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32, true}; return true;
    case VK_FORMAT_R16G16B16A16_SFLOAT: m = {DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, false}; return true;
    default: return false;
    }
}

// o D3D12 so entra quando o overlay conecta: num programa Vulkan qualquer, nada disto carrega
bool CriaD3d(Dev& d)
{
    if (g.d3d)
        return true;
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    d.inst->GetPhysicalDeviceProperties2(d.fis, &p);
    if (!id.deviceLUIDValid)
        return Erro("a placa nao informou o LUID", VK_ERROR_FEATURE_NOT_PRESENT), false;
    HMODULE mdx = LoadLibraryW(L"dxgi.dll"), md3 = LoadLibraryW(L"d3d12.dll");
    auto criaF = mdx ? reinterpret_cast<decltype(&CreateDXGIFactory1)>(GetProcAddress(mdx, "CreateDXGIFactory1")) : nullptr;
    auto criaD = md3 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(md3, "D3D12CreateDevice")) : nullptr;
    if (!criaF || !criaD)
        return Erro("D3D12 indisponivel", GetLastError()), false;
    ComPtr<IDXGIFactory4> f;
    ComPtr<IDXGIAdapter1> ad;
    LUID luid;
    memcpy(&luid, id.deviceLUID, sizeof luid);
    HRESULT hr = criaF(IID_PPV_ARGS(&f));
    if (SUCCEEDED(hr))
        hr = f->EnumAdapterByLuid(luid, IID_PPV_ARGS(&ad));
    if (SUCCEEDED(hr))
        hr = criaD(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.d3d));
    if (FAILED(hr))
        return Erro("dispositivo D3D12", hr), false;
    return true;
}

// jogo no limite da VRAM: "the creation APIs will return failure" acima do orcamento; sem residencia a criacao passa
HRESULT CriaTex(const D3D12_RESOURCE_DESC& rd, ComPtr<ID3D12Resource>& t)
{
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    HRESULT hr = g.d3d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t));
    if (hr != E_OUTOFMEMORY)
        return hr;
    hr = g.d3d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t));
    if (FAILED(hr))
        return hr;
    ID3D12Pageable* p[] = {t.Get()};
    return g.d3d->MakeResident(1, p);
}

// a textura do D3D12 vista pelo Vulkan. O handle continua nosso ("does not transfer ownership of the handle"): fecha em Solta
bool Importa(Dev& d, uint32_t s, VkFormat fmt, uint32_t w, uint32_t h)
{
    VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.handleTypes = kTipo;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {w, h, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = d.CreateImage(d.dev, &ii, nullptr, &g.img[s]);
    if (r != VK_SUCCESS)
        return Erro("imagem importada", r), false;
    VkMemoryRequirements mr{};
    d.GetImageMemoryRequirements(d.dev, g.img[s], &mr);
    uint32_t bits = mr.memoryTypeBits;
    VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
    if (d.GetMemoryWin32HandlePropertiesKHR(d.dev, kTipo, g.handles[s], &hp) == VK_SUCCESS && (bits & hp.memoryTypeBits))
        bits &= hp.memoryTypeBits;
    VkPhysicalDeviceMemoryProperties mp{};
    d.inst->GetPhysicalDeviceMemoryProperties(d.fis, &mp);
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = g.img[s];
    VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &ded};
    imp.handleType = kTipo;
    imp.handle = g.handles[s];
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp};
    ai.allocationSize = mr.size;
    r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    // primeiro a memoria da placa, como o OBS: o primeiro tipo que aceitar
    for (int volta = 0; volta < 2 && r != VK_SUCCESS; ++volta)
        for (uint32_t i = 0; i < mp.memoryTypeCount && r != VK_SUCCESS; ++i)
        {
            const bool local = (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
            if (!(bits >> i & 1) || local != (volta == 0))
                continue;
            ai.memoryTypeIndex = i;
            r = d.AllocateMemory(d.dev, &ai, nullptr, &g.mem[s]);
        }
    if (r != VK_SUCCESS)
        return Erro("memoria importada", r), false;
    r = d.BindImageMemory(d.dev, g.img[s], g.mem[s], 0);
    if (r != VK_SUCCESS)
        return Erro("ligar a memoria importada", r), false;
    return true;
}

bool Prepara(Dev& d, Troca& t, const MapaFmt& m)
{
    if (g.dono == &d && g.img[0] && g.w == t.ext.width && g.h == t.ext.height && g.vk == m.vk)
        return true;
    Solta();
    if (!CriaD3d(d))
        return false;
    VkPhysicalDeviceExternalImageFormatInfo ei{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    ei.handleType = kTipo;
    VkPhysicalDeviceImageFormatInfo2 fi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ei};
    fi.format = m.vk;
    fi.type = VK_IMAGE_TYPE_2D;
    fi.tiling = VK_IMAGE_TILING_OPTIMAL;
    fi.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkExternalImageFormatProperties ep{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 pr{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &ep};
    const VkResult rf = d.inst->GetPhysicalDeviceImageFormatProperties2(d.fis, &fi, &pr);
    if (rf != VK_SUCCESS || !(ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
        return Erro("o driver Vulkan nao importa textura do D3D12", rf), false;

    g.dono = &d;
    ++g.geracao;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = t.ext.width;
    rd.Height = t.ext.height;
    rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.Format = m.dx;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    const DWORD pid = GetCurrentProcessId();
    wchar_t nome[128];
    for (uint32_t s = 0; s < kSlots; ++s)
    {
        HRESULT hr = CriaTex(rd, g.tex[s]);
        if (FAILED(hr))
            return Erro("textura", hr), Solta(), false;
        dlss5ov::NomeRecursoApi(nome, 128, pid, g.geracao, L"cor", s);
        hr = g.d3d->CreateSharedHandle(g.tex[s].Get(), nullptr, GENERIC_ALL, nome, &g.handles[s]);
        if (FAILED(hr))
            return Erro("nome da textura", hr), Solta(), false;
        if (!Importa(d, s, m.vk, t.ext.width, t.ext.height))
            return Solta(), false;
    }
    HRESULT hr = g.d3d->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g.cerca));
    dlss5ov::NomeRecursoApi(nome, 128, pid, g.geracao, L"cerca", 0);
    if (SUCCEEDED(hr))
        hr = g.d3d->CreateSharedHandle(g.cerca.Get(), nullptr, GENERIC_ALL, nome, &g.handles[kSlots]);
    if (FAILED(hr))
        return Erro("cerca", hr), Solta(), false;
    g.w = t.ext.width;
    g.h = t.ext.height;
    g.vk = m.vk;
    g.n = 0;
    g.cab->w = g.w, g.cab->h = g.h, g.cab->fmt = m.dx, g.cab->api = dlss5ov::kApiVulkan;
    g.cab->quadro[0] = g.cab->quadro[1] = 0;
    g.cab->consumido = 0;
    g.cab->erro[0] = 0;
    MemoryBarrier();
    g.cab->geracao = g.geracao;
    Log("Vulkan: swapchain %ux%u, formato %d (DXGI %d), espaco de cor %d", g.w, g.h, int(t.fmt), int(m.dx), int(t.espaco));
    return true;
}

bool GaranteQuadro(Dev& d, Quadro& q, uint32_t fam)
{
    // cada um por si: com a cerca criada e o semaforo recusado, os envios seguintes saiam com semaforo nulo
    if (!q.f)
    {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (d.CreateFence(d.dev, &fi, nullptr, &q.f) != VK_SUCCESS)
            return false;
    }
    if (!q.s)
    {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (d.CreateSemaphore(d.dev, &si, nullptr, &q.s) != VK_SUCCESS)
            return false;
    }
    if (q.cb && q.fam == fam)
        return true;
    auto& pool = d.pools[fam];
    if (!pool)
    {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = fam;
        if (d.CreateCommandPool(d.dev, &pi, nullptr, &pool) != VK_SUCCESS)
            return false;
    }
    if (q.cb)
        d.FreeCommandBuffers(d.dev, d.pools[q.fam], 1, &q.cb);
    q.cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (d.AllocateCommandBuffers(d.dev, &ai, &q.cb) != VK_SUCCESS)
        return false;
    // objeto despachavel criado pela camada: o carregador precisa do ponteiro de despacho nele
    if (d.setData)
        d.setData(d.dev, q.cb);
    else
        *reinterpret_cast<void**>(q.cb) = Chave(d.dev);
    q.fam = fam;
    return true;
}

// copia a imagem que o jogo vai apresentar. Espera os semaforos do present (o jogo pode ter desenhado em outra fila) e devolve o nosso
// para o present esperar; VK_NULL_HANDLE = este quadro nao vai, e o present segue como o jogo mandou
VkSemaphore Copia(Dev& d, VkQueue fila, const VkPresentInfoKHR& pi)
{
    std::unique_lock<std::mutex> lk(g.mxCap, std::try_to_lock);
    if (!lk.owns_lock() || !g.cab)
        return VK_NULL_HANDLE;
    const ULONGLONG agora = GetTickCount64();
    g.cab->batimentoJogo = agora;
    if (agora - g.cab->batimentoOverlay >= 1000)
    {
        // o overlay fechou ou desligou: a VRAM volta para o jogo
        if (agora - g.vivoEm > 5000 && g.dono)
            Solta();
        return VK_NULL_HANDLE;
    }
    if (agora - g.vivoEm >= 1000)
        g.alvoVistoEm = agora; // o overlay voltou: a janela alvo tem 1 s para aparecer
    g.vivoEm = agora;
    if (agora < g.tentaEm)
        return VK_NULL_HANDLE;
    const auto fam = d.filas.find(fila);
    if (fam == d.filas.end())
        return VK_NULL_HANDLE;
    // a janela alvo primeiro; outro swapchain (janela escolhida errada) so se ela parou
    const HWND alvo = reinterpret_cast<HWND>(uintptr_t(g.cab->janela));
    Troca* t = nullptr;
    uint32_t idx = 0;
    for (uint32_t i = 0; i < pi.swapchainCount; ++i)
    {
        const auto it = d.trocas.find(pi.pSwapchains[i]);
        if (it == d.trocas.end() || !it->second->copiavel)
            continue;
        if (it->second->raiz && it->second->raiz == alvo)
        {
            t = it->second;
            idx = pi.pImageIndices[i];
            g.alvoVistoEm = agora;
            break;
        }
        if (!t && agora - g.alvoVistoEm >= 1000 && it->second->ext.width >= 320 && it->second->ext.height >= 200)
        {
            t = it->second;
            idx = pi.pImageIndices[i];
        }
    }
    if (!t || idx >= t->q.size())
        return VK_NULL_HANDLE;
    g.cab->janelaSwap = uint64_t(reinterpret_cast<uintptr_t>(t->raiz));
    MapaFmt m{};
    if (!Formato(t->fmt, m))
        return Erro("formato do swapchain", t->fmt), VK_NULL_HANDLE;
    if (m.blit && !(d.fams[fam->second].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        return Erro("fila sem graficos para converter o formato", t->fmt), VK_NULL_HANDLE;
    if (!Prepara(d, *t, m))
        return VK_NULL_HANDLE;
    const uint32_t s = uint32_t((g.n + 1) % kSlots);
    Quadro& q = *t->q[idx];
    if (g.cab->consumido < g.cab->quadro[s] || q.ocupado)
        return VK_NULL_HANDLE; // o overlay ainda le o slot, ou a GPU ainda usa a lista: este quadro nao vai
    if (!GaranteQuadro(d, q, fam->second))
        return Erro("objetos da copia", VK_ERROR_OUT_OF_HOST_MEMORY), VK_NULL_HANDLE;

    const VkImage origem = t->imgs[idx], destino = g.img[s];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (d.ResetCommandBuffer(q.cb, 0) != VK_SUCCESS || d.BeginCommandBuffer(q.cb, &bi) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    const VkImageSubresourceRange cor{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier b[2] = {{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}, {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}};
    b[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b[0].srcQueueFamilyIndex = b[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b[0].image = origem;
    b[0].subresourceRange = cor;
    // a imagem inteira e reescrita: o conteudo anterior nao importa. Vem da outra API e volta para ela (como o OBS)
    b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    b[1].dstQueueFamilyIndex = fam->second;
    b[1].image = destino;
    b[1].subresourceRange = cor;
    d.CmdPipelineBarrier(q.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
    if (m.blit)
    {
        VkImageBlit r{};
        r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.srcOffsets[1] = r.dstOffsets[1] = {int32_t(g.w), int32_t(g.h), 1};
        d.CmdBlitImage(q.cb, origem, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destino, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, VK_FILTER_NEAREST);
    }
    else
    {
        VkImageCopy r{};
        r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.extent = {g.w, g.h, 1};
        d.CmdCopyImage(q.cb, origem, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destino, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }
    b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b[0].dstAccessMask = 0;
    b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b[1].dstAccessMask = 0;
    b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b[1].srcQueueFamilyIndex = fam->second;
    b[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    d.CmdPipelineBarrier(q.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
    if (d.EndCommandBuffer(q.cb) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    std::vector<VkPipelineStageFlags> estagios(pi.waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = pi.waitSemaphoreCount;
    si.pWaitSemaphores = pi.pWaitSemaphores;
    si.pWaitDstStageMask = estagios.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &q.cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &q.s;
    if (d.ResetFences(d.dev, 1, &q.f) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    const VkResult r = d.QueueSubmit(fila, 1, &si, q.f);
    if (r != VK_SUCCESS)
        return Erro("envio da copia", r), VK_NULL_HANDLE;
    const uint64_t n = ++g.n;
    g.cab->quadro[s] = n;
    q.ocupado = true;
    {
        std::lock_guard<std::mutex> lf(g.mxF);
        Pend p;
        p.d = &d;
        p.q = &q;
        p.cerca = g.cerca;
        p.n = n;
        g.pend.push_back(std::move(p));
        if (!g.ajudante)
        {
            g.ajudante = true;
            if (HANDLE h = CreateThread(nullptr, 0, Ajudante, nullptr, 0, nullptr))
                CloseHandle(h);
        }
    }
    g.cvF.notify_one();
    return q.s;
}

void SoltaTroca(Dev& d, Troca* t)
{
    EsperaQuadros(*t);
    for (Quadro* q : t->q)
    {
        if (q->cb)
            d.FreeCommandBuffers(d.dev, d.pools[q->fam], 1, &q->cb);
        if (q->f)
            d.DestroyFence(d.dev, q->f, nullptr);
        if (q->s)
            d.DestroySemaphore(d.dev, q->s, nullptr);
        delete q;
    }
    delete t;
}

// ---- a cadeia de chamadas ----
VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out)
{
    auto* link = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(ci->pNext));
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(link->pNext));
    if (!link)
        return VK_ERROR_INITIALIZATION_FAILED;
    const PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto cria = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!cria)
        return VK_ERROR_INITIALIZATION_FAILED;
    // a memoria externa e as consultas dela sao do nucleo a partir do 1.1 (o OBS sobe a versao do mesmo jeito)
    VkInstanceCreateInfo novo = *ci;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    if (ci->pApplicationInfo)
        app = *ci->pApplicationInfo;
    if (app.apiVersion < VK_API_VERSION_1_1)
    {
        app.apiVersion = VK_API_VERSION_1_1;
        novo.pApplicationInfo = &app;
    }
    const VkResult r = cria(&novo, a, out);
    if (r != VK_SUCCESS)
        return r;
    auto* i = new Inst;
    i->inst = *out;
    i->gipa = gipa;
#define X(n) i->n = reinterpret_cast<PFN_vk##n>(gipa(*out, "vk" #n));
    FUNCOES_INST(X)
#undef X
    std::lock_guard<std::mutex> lk(g.mx);
    g.insts[Chave(*out)] = i;
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance inst, const VkAllocationCallbacks* a)
{
    Inst* i = AchaInst(Chave(inst));
    if (!i)
        return;
    const PFN_vkDestroyInstance destroi = i->DestroyInstance;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        g.insts.erase(Chave(inst));
    }
    destroi(inst, a);
    delete i;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateWin32Surface(VkInstance inst, const VkWin32SurfaceCreateInfoKHR* ci, const VkAllocationCallbacks* a, VkSurfaceKHR* out)
{
    Inst* i = AchaInst(Chave(inst));
    if (!i)
        return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult r = i->CreateWin32SurfaceKHR(inst, ci, a, out);
    if (r == VK_SUCCESS)
    {
        std::lock_guard<std::mutex> lk(g.mx);
        g.superficies[*out] = ci->hwnd;
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroySurface(VkInstance inst, VkSurfaceKHR s, const VkAllocationCallbacks* a)
{
    Inst* i = AchaInst(Chave(inst));
    {
        std::lock_guard<std::mutex> lk(g.mx);
        g.superficies.erase(s);
    }
    if (i)
        i->DestroySurfaceKHR(inst, s, a);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice fis, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out)
{
    VkLayerDeviceCreateInfo* link = nullptr;
    PFN_vkSetDeviceLoaderData setData = nullptr;
    for (auto* p = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(ci->pNext)); p;
         p = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(p->pNext)))
    {
        if (p->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)
            continue;
        if (p->function == VK_LAYER_LINK_INFO && !link)
            link = p;
        else if (p->function == VK_LOADER_DATA_CALLBACK)
            setData = p->u.pfnSetDeviceLoaderData;
    }
    Inst* i = AchaInst(Chave(fis));
    if (!link || !i)
        return VK_ERROR_INITIALIZATION_FAILED;
    const PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto cria = reinterpret_cast<PFN_vkCreateDevice>(gipa(i->inst, "vkCreateDevice"));
    if (!cria)
        return VK_ERROR_INITIALIZATION_FAILED;
    // a extensao da memoria externa e so nossa: o jogo nao ve diferenca
    uint32_t n = 0;
    i->EnumerateDeviceExtensionProperties(fis, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> tem(n);
    if (n)
        i->EnumerateDeviceExtensionProperties(fis, nullptr, &n, tem.data());
    auto existe = [&](const char* e) {
        for (uint32_t k = 0; k < n; ++k)
            if (!strcmp(tem[k].extensionName, e))
                return true;
        return false;
    };
    std::vector<const char*> nomes(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
    auto pede = [&](const char* e) {
        for (const char* x : nomes)
            if (!strcmp(x, e))
                return;
        if (existe(e))
            nomes.push_back(e);
    };
    pede(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
    pede(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
    pede(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
    pede(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
    VkDeviceCreateInfo novo = *ci;
    novo.enabledExtensionCount = uint32_t(nomes.size());
    novo.ppEnabledExtensionNames = nomes.data();
    const VkResult r = cria(fis, &novo, a, out);
    if (r != VK_SUCCESS)
        return r;
    auto* d = new Dev;
    d->dev = *out;
    d->fis = fis;
    d->inst = i;
    d->gdpa = gdpa;
    d->setData = setData;
    d->extOk = existe(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
#define X(n) d->n = reinterpret_cast<PFN_vk##n>(gdpa(*out, "vk" #n));
    FUNCOES_DISP(X)
#undef X
    uint32_t nf = 0;
    i->GetPhysicalDeviceQueueFamilyProperties(fis, &nf, nullptr);
    d->fams.resize(nf);
    i->GetPhysicalDeviceQueueFamilyProperties(fis, &nf, d->fams.data());
    std::lock_guard<std::mutex> lk(g.mx);
    g.devs[Chave(*out)] = d;
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice dev, const VkAllocationCallbacks* a)
{
    Dev* d = AchaDev(Chave(dev));
    if (!d)
        return;
    {
        std::lock_guard<std::mutex> lc(g.mxCap);
        if (g.dono == d)
            Solta();
        for (auto& par : d->trocas)
            SoltaTroca(*d, par.second);
        d->trocas.clear();
        for (auto& par : d->pools)
            d->DestroyCommandPool(dev, par.second, nullptr);
        d->pools.clear();
    }
    const PFN_vkDestroyDevice destroi = d->DestroyDevice;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        g.devs.erase(Chave(dev));
    }
    destroi(dev, a);
    delete d;
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice dev, uint32_t fam, uint32_t idx, VkQueue* q)
{
    Dev* d = AchaDev(Chave(dev));
    d->GetDeviceQueue(dev, fam, idx, q);
    std::lock_guard<std::mutex> lc(g.mxCap);
    d->filas[*q] = fam;
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice dev, const VkDeviceQueueInfo2* qi, VkQueue* q)
{
    Dev* d = AchaDev(Chave(dev));
    d->GetDeviceQueue2(dev, qi, q);
    if (*q)
    {
        std::lock_guard<std::mutex> lc(g.mxCap);
        d->filas[*q] = qi->queueFamilyIndex;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchain(VkDevice dev, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* a, VkSwapchainKHR* out)
{
    Dev* d = AchaDev(Chave(dev));
    if (!d)
        return VK_ERROR_INITIALIZATION_FAILED;
    // a copia le a imagem do swapchain: o jogo nao costuma pedir esse uso
    VkSwapchainCreateInfoKHR novo = *ci;
    VkSurfaceCapabilitiesKHR caps{};
    if (d->extOk && d->inst->GetPhysicalDeviceSurfaceCapabilitiesKHR(d->fis, ci->surface, &caps) == VK_SUCCESS &&
        (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        novo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult r = d->CreateSwapchainKHR(dev, &novo, a, out);
    if (r != VK_SUCCESS && novo.imageUsage != ci->imageUsage)
    {
        novo = *ci;
        r = d->CreateSwapchainKHR(dev, ci, a, out);
    }
    if (r != VK_SUCCESS)
        return r;
    auto* t = new Troca;
    t->ext = ci->imageExtent;
    t->fmt = ci->imageFormat;
    t->espaco = ci->imageColorSpace;
    t->copiavel = d->extOk && (novo.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    {
        std::lock_guard<std::mutex> lk(g.mx);
        const auto it = g.superficies.find(ci->surface);
        if (it != g.superficies.end())
            t->raiz = GetAncestor(it->second, GA_ROOT);
    }
    uint32_t n = 0;
    d->GetSwapchainImagesKHR(dev, *out, &n, nullptr);
    t->imgs.resize(n);
    if (n)
        d->GetSwapchainImagesKHR(dev, *out, &n, t->imgs.data());
    for (uint32_t i = 0; i < n; ++i)
        t->q.push_back(new Quadro);
    std::lock_guard<std::mutex> lc(g.mxCap);
    d->trocas[*out] = t;
    if (t->copiavel)
    {
        AbreMapa();
        // HDR10 ou SDR em 10 bits: o Vulkan diz na criacao, em termos que o overlay ja entende (DXGI_COLOR_SPACE_TYPE + 1)
        if (g.cab)
            g.cab->espacoCor = ci->imageColorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT            ? uint32_t(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) + 1
                               : ci->imageColorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT ? uint32_t(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) + 1
                                                                                                : uint32_t(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) + 1;
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchain(VkDevice dev, VkSwapchainKHR sc, const VkAllocationCallbacks* a)
{
    Dev* d = AchaDev(Chave(dev));
    if (!d)
        return;
    if (sc)
    {
        std::lock_guard<std::mutex> lc(g.mxCap);
        const auto it = d->trocas.find(sc);
        if (it != d->trocas.end())
        {
            SoltaTroca(*d, it->second);
            d->trocas.erase(it);
        }
    }
    d->DestroySwapchainKHR(dev, sc, a);
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresent(VkQueue fila, const VkPresentInfoKHR* pi)
{
    Dev* d = AchaDev(Chave(fila));
    if (!d)
        return VK_ERROR_DEVICE_LOST;
    VkPresentInfoKHR info = *pi;
    VkSemaphore minha = VK_NULL_HANDLE;
    if (d->extOk)
        minha = Copia(*d, fila, *pi);
    if (minha)
    {
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &minha;
    }
    const VkResult r = d->QueuePresentKHR(fila, &info);
    static dlss5ov::LimiteFps limite;
    limite.Espera(g.cab);
    return r;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice dev, const char* n);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance inst, const char* n);

PFN_vkVoidFunction Funcao(const char* n)
{
#define F(nome, f) \
    if (!strcmp(n, nome)) \
        return reinterpret_cast<PFN_vkVoidFunction>(&f);
    F("vkGetDeviceProcAddr", GetDeviceProcAddr)
    F("vkDestroyDevice", DestroyDevice)
    F("vkGetDeviceQueue", GetDeviceQueue)
    F("vkGetDeviceQueue2", GetDeviceQueue2)
    F("vkCreateSwapchainKHR", CreateSwapchain)
    F("vkDestroySwapchainKHR", DestroySwapchain)
    F("vkQueuePresentKHR", QueuePresent)
#undef F
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice dev, const char* n)
{
    Dev* d = AchaDev(Chave(dev));
    if (!d)
        return nullptr;
    const PFN_vkVoidFunction prox = d->gdpa(dev, n);
    // o que nao existe no dispositivo (extensao nao ligada) continua nao existindo
    const PFN_vkVoidFunction nossa = Funcao(n);
    return nossa && prox ? nossa : prox;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance inst, const char* n)
{
    if (!strcmp(n, "vkGetInstanceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(&GetInstanceProcAddr);
    if (!strcmp(n, "vkCreateInstance"))
        return reinterpret_cast<PFN_vkVoidFunction>(&CreateInstance);
    if (!inst)
        return nullptr;
    Inst* i = AchaInst(Chave(inst));
    if (!i)
        return nullptr;
    const PFN_vkVoidFunction prox = i->gipa(inst, n);
    if (!prox)
        return nullptr;
    if (!strcmp(n, "vkDestroyInstance"))
        return reinterpret_cast<PFN_vkVoidFunction>(&DestroyInstance);
    if (!strcmp(n, "vkCreateDevice"))
        return reinterpret_cast<PFN_vkVoidFunction>(&CreateDevice);
    if (!strcmp(n, "vkCreateWin32SurfaceKHR"))
        return reinterpret_cast<PFN_vkVoidFunction>(&CreateWin32Surface);
    if (!strcmp(n, "vkDestroySurfaceKHR"))
        return reinterpret_cast<PFN_vkVoidFunction>(&DestroySurface);
    const PFN_vkVoidFunction nossa = Funcao(n);
    return nossa ? nossa : prox;
}
} // namespace

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v)
{
    if (!v || v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT || v->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    // "the loader will treat the layer as unusable and will not load it"
    if (!OverlayAberto() || AntiCheatAtivo())
        return VK_ERROR_INITIALIZATION_FAILED;
    v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = &GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = &GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
