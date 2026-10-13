#include "pch.h"
#include "lmxxf2_rrjogo.h"

#include <proxies/FfxApi_Proxy.h>

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace Lmxxf2RrJogo
{
namespace
{
// faixas do sample da AMD (FidelityFX_Denoiser, "Dynamic Configuration"), textos do cartão do FSR-RR; chaves 1..6
const Info kInfo[kChaves] = {
    { "Bordas pela normal", "Quanto o denoiser respeita as quinas pelo angulo da superficie.\n"
                            "Mais alto: bordas mais nitidas, sem borrar entre superficies; alto demais traz ruido.",
      0.0f, 1.0f, "%.3f", false },
    { "Estabilidade temporal", "Quanto mistura dos quadros anteriores.\n"
                               "Mais alto: imagem mais lisa, mas pode deixar rastro e perder detalhe.\n"
                               "Mais baixo: responde rapido, mas pode ferver.",
      0.0f, 1.0f, "%.3f", false },
    { "Radiancia maxima", "Teto de brilho de cada amostra.\n"
                          "Mais baixo: tira mais vagalumes, mas pode apagar luzes fortes.",
      1.0f, 65504.0f, "%.0f", true },
    { "Corte de radiancia (desvios)", "Tolerancia a pontos brilhantes perto da vizinhanca.\n"
                                      "Mais baixo: tira vagalumes e ruido; mais alto: preserva reflexos e brilhos.",
      0.1f, 65504.0f, "%.1f", true },
    { "Relaxamento do kernel", "Quanto o filtro se estica para seguir a geometria.\n"
                               "Mais alto: menos ondulacao em superficies curvas; mais baixo: mais detalhe fino.",
      0.0f, 1.0f, "%.3f", false },
    { "Limite de desoclusao", "Sensibilidade a areas recem-reveladas quando algo se move.\n"
                              "Mais baixo: trata melhor objetos em movimento, mas pode piscar.\n"
                              "Mais alto: pisca menos, mas pode deixar rastro de luz.",
      0.01f, 0.05f, "%.4f", false },
};

struct Ctx
{
    bool ok[kChaves] {};
    float valor[kChaves] {};
    bool trocado[kChaves] {};
    bool padraoLido = false;
};

std::mutex g_mx;
Estado g_est;
std::unordered_map<ffxContext, Ctx> g_ctx;
std::atomic<uint64_t> g_dispatches { 0 };
std::atomic<bool> g_resetPedido { false };
bool g_logado[kChaves] {};

std::optional<float> Alvo(int i)
{
    if (!Config::Instance()->RrJogoAjustar.value_or_default())
        return std::nullopt;
    const auto& v = Valor(i);
    if (!v.has_value())
        return std::nullopt;
    return std::clamp(v.value(), kInfo[i].min, kInfo[i].max);
}

ffxReturnCode_t Envia(ffxContext* context, int i, float v)
{
    ffxConfigureDescDenoiserKeyValue desc {};
    desc.header.type = FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE;
    desc.key = static_cast<uint64_t>(i + 1);
    desc.count = 1;
    desc.data = &v;
    return FfxApiProxy::D3D12_Configure(context, &desc.header);
}

void LePadrao(ffxContext* context)
{
    for (int i = 0; i < kChaves; i++)
    {
        float v = 0.0f;
        ffxQueryDescDenoiserGetDefaultKeyValue q {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_DENOISER_GET_DEFAULT_KEYVALUE;
        q.key = static_cast<uint64_t>(i + 1);
        q.count = 1;
        q.data = &v;
        const auto r = FfxApiProxy::D3D12_Query(context, &q.header);
        std::lock_guard lock(g_mx);
        if (r == FFX_API_RETURN_OK)
        {
            g_est.padraoOk[i] = true;
            g_est.padrao[i] = v;
        }
    }
    LOG_INFO("[lmxxf2] Ray Regeneration do jogo: padroes AMD {} / {} / {} / {} / {} / {}", g_est.padrao[0],
             g_est.padrao[1], g_est.padrao[2], g_est.padrao[3], g_est.padrao[4], g_est.padrao[5]);
}
} // namespace

const Info& Chave(int i) { return kInfo[std::clamp(i, 0, kChaves - 1)]; }

CustomOptional<float, NoDefault>& Valor(int i)
{
    auto* c = Config::Instance();
    switch (i)
    {
    case 0:
        return c->RrJogoCrossBlNormStr;
    case 1:
        return c->RrJogoStabilityBias;
    case 2:
        return c->RrJogoMaxRadiance;
    case 3:
        return c->RrJogoRadianceClip;
    case 4:
        return c->RrJogoGaussKernRelax;
    default:
        return c->RrJogoDisocThreshold;
    }
}

void Restaura()
{
    for (int i = 0; i < kChaves; i++)
        Valor(i).reset();
}

ffxReturnCode_t Configure(ffxContext* context, ffxConfigureDescHeader* desc)
{
    if (desc->type != FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE || context == nullptr || *context == nullptr)
        return FfxApiProxy::D3D12_Configure(context, desc);

    auto* kv = reinterpret_cast<ffxConfigureDescDenoiserKeyValue*>(desc);
    const int i = static_cast<int>(kv->key) - 1;
    if (i < 0 || i >= kChaves || kv->count != 1 || kv->data == nullptr)
        return FfxApiProxy::D3D12_Configure(context, desc);

    const float pedido = *static_cast<const float*>(kv->data);
    const auto alvo = Alvo(i);
    float enviado = alvo.value_or(pedido);

    ffxReturnCode_t r;
    if (alvo.has_value())
    {
        ffxConfigureDescDenoiserKeyValue copia = *kv;
        copia.data = &enviado;
        r = FfxApiProxy::D3D12_Configure(context, &copia.header);
    }
    else
    {
        r = FfxApiProxy::D3D12_Configure(context, desc);
    }

    std::lock_guard lock(g_mx);
    g_est.configures++;
    g_est.jogo[i] = pedido;
    g_est.jogoDefiniu[i] = true;
    if (r == FFX_API_RETURN_OK)
    {
        auto& c = g_ctx[*context];
        c.ok[i] = true;
        c.valor[i] = enviado;
        c.trocado[i] = alvo.has_value();
        g_est.contextos = static_cast<int>(g_ctx.size());
    }
    else
    {
        g_est.erro = std::format("configure da chave {} falhou: {}", i + 1, FfxApiProxy::ReturnCodeToString(r));
    }

    if (!g_logado[i])
    {
        g_logado[i] = true;
        LOG_INFO("[lmxxf2] Ray Regeneration do jogo: {} = {} (enviado {}), resultado {}", kInfo[i].nome, pedido,
                 enviado, FfxApiProxy::ReturnCodeToString(r));
    }
    return r;
}

void Criado(const ffxContext* context, const ffxCreateContextDescHeader* desc, ffxReturnCode_t resultado)
{
    for (auto* h = reinterpret_cast<const ffxApiHeader*>(desc); h != nullptr; h = h->pNext)
    {
        if (h->type != FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER)
            continue;
        const auto* d = reinterpret_cast<const ffxCreateContextDescDenoiser*>(h);
        {
            std::lock_guard lock(g_mx);
            g_est.criado = resultado == FFX_API_RETURN_OK;
            g_est.sinais = d->signalFlags;
            g_est.sinaisXadrez = d->checkerboardSignalFlags;
            g_est.flagsCriacao = d->flags;
            g_est.versao = d->version;
            g_est.maxW = d->maxRenderSize.width;
            g_est.maxH = d->maxRenderSize.height;
        }
        LOG_INFO("[lmxxf2] Ray Regeneration do jogo: contexto {:X} criado ({}), sinais {:#x}, xadrez {:#x}, flags {:#x}, "
                 "versao {:#x}, max {}x{}",
                 context && *context ? (size_t) *context : 0, FfxApiProxy::ReturnCodeToString(resultado), d->signalFlags,
                 d->checkerboardSignalFlags, d->flags, d->version, d->maxRenderSize.width, d->maxRenderSize.height);
        return;
    }
}

void PedeReset() { g_resetPedido.store(true, std::memory_order_release); }

void AntesDoDispatch(ffxContext* context, ffxDispatchDescHeader* desc)
{
    g_dispatches.fetch_add(1, std::memory_order_relaxed);
    if (context == nullptr || *context == nullptr)
        return;

    // dispatch principal: tamanho, flags e o reset que a UI pediu
    if (desc != nullptr && desc->type == FFX_API_DISPATCH_DESC_TYPE_DENOISER)
    {
        auto* d = reinterpret_cast<ffxDispatchDescDenoiser*>(desc);
        if (g_resetPedido.exchange(false, std::memory_order_acq_rel))
        {
            d->flags |= FFX_DENOISER_DISPATCH_RESET;
            std::lock_guard lock(g_mx);
            g_est.resets++;
            LOG_INFO("[lmxxf2] Ray Regeneration do jogo: historico recomecado pela UI");
        }
        std::lock_guard lock(g_mx);
        g_est.renderW = d->renderSize.width;
        g_est.renderH = d->renderSize.height;
        g_est.flagsDispatch = d->flags;
    }

    bool lerPadrao = false;
    {
        std::lock_guard lock(g_mx);
        auto& c = g_ctx[*context];
        g_est.contextos = static_cast<int>(g_ctx.size());
        if (!c.padraoLido)
        {
            c.padraoLido = true;
            lerPadrao = true;
        }
    }
    if (lerPadrao)
    {
        LOG_INFO("[lmxxf2] Ray Regeneration do jogo: primeiro dispatch do denoiser (contexto {:X})", (size_t) *context);
        LePadrao(context);
    }

    for (int i = 0; i < kChaves; i++)
    {
        const auto alvo = Alvo(i);
        float desejado = 0.0f;
        {
            std::lock_guard lock(g_mx);
            auto& c = g_ctx[*context];
            if (alvo.has_value())
                desejado = *alvo;
            else if (c.trocado[i] && g_est.jogoDefiniu[i])
                desejado = g_est.jogo[i]; // a UI soltou a chave: volta ao que o jogo pediu
            else if (c.trocado[i] && g_est.padraoOk[i])
                desejado = g_est.padrao[i];
            else
                continue;

            if (c.ok[i] && c.valor[i] == desejado)
                continue;
        }

        const auto r = Envia(context, i, desejado);

        std::lock_guard lock(g_mx);
        auto& c = g_ctx[*context];
        if (r == FFX_API_RETURN_OK)
        {
            c.ok[i] = true;
            c.valor[i] = desejado;
            c.trocado[i] = alvo.has_value();
        }
        else
        {
            // não insiste a cada quadro: marca como aplicado e mostra o erro na UI
            c.ok[i] = true;
            c.valor[i] = desejado;
            g_est.erro = std::format("ajuste da chave {} falhou: {}", i + 1, FfxApiProxy::ReturnCodeToString(r));
            LOG_WARN("[lmxxf2] Ray Regeneration do jogo: {}", g_est.erro);
        }
    }
}

void Destruido(ffxContext context)
{
    std::lock_guard lock(g_mx);
    g_ctx.erase(context);
    g_est.contextos = static_cast<int>(g_ctx.size());
}

Estado Le()
{
    std::lock_guard lock(g_mx);
    Estado e = g_est;
    e.dispatches = g_dispatches.load(std::memory_order_relaxed);
    // dispatches por segundo, em janelas de 1 s (mostra se o denoiser do jogo parou)
    static uint64_t contAntes = 0;
    static ULONGLONG tAntes = 0;
    static float taxa = 0.f;
    const ULONGLONG agora = GetTickCount64();
    if (tAntes == 0)
    {
        tAntes = agora;
        contAntes = e.dispatches;
    }
    else if (agora - tAntes >= 1000)
    {
        taxa = float(e.dispatches - contAntes) * 1000.f / float(agora - tAntes);
        tAntes = agora;
        contAntes = e.dispatches;
    }
    e.dispatchesPorSeg = taxa;
    if (!g_ctx.empty())
    {
        const auto& c = g_ctx.begin()->second;
        for (int i = 0; i < kChaves; i++)
        {
            e.aplicadoOk[i] = c.ok[i];
            e.aplicado[i] = c.valor[i];
        }
    }
    return e;
}
} // namespace Lmxxf2RrJogo
