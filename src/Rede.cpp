#include "Rede.h"
#include <algorithm>

namespace ov {

bool Rede::Erro(int32_t s, const char* onde)
{
    if (s == LMXXF_NR_OK)
        return false;
    Log("rede", Fmt("%s: status %d: %s", onde, s, UltimoErro().c_str()));
    return true;
}

std::string Rede::UltimoErro()
{
    char b[2048] = {};
    if (api.GetLastError)
        api.GetLastError(b, sizeof b);
    return b;
}

std::string Rede::Status()
{
    char b[2048] = {};
    if (ctx && api.GetStatus)
        api.GetStatus(ctx, b, sizeof b);
    return b;
}

bool Rede::Inicia(Gpu& g, const std::wstring& pasta)
{
    // tamanho livre da rede (ate 3840x2176 de superficie): o tamanho que o overlay entrega e o que a rede roda
    SetEnvironmentVariableW(L"DLSS5_NETWORK_FREE_RES", L"1");
    SetDllDirectoryW(pasta.c_str());
    mod = LoadLibraryW((pasta + L"\\LmxxfNrRuntime.dll").c_str());
    if (!mod)
    {
        Log("rede", Fmt("LoadLibrary(LmxxfNrRuntime.dll) falhou: %lu", GetLastError()));
        return false;
    }
    auto get = reinterpret_cast<int32_t (*)(uint32_t, LmxxfNrApi*)>(GetProcAddress(mod, "LmxxfNrGetApi"));
    api.struct_size = sizeof(LmxxfNrApi);
    if (!get || get(LMXXF_NR_ABI_VERSION, &api) != LMXXF_NR_OK)
    {
        Log("rede", "LmxxfNrGetApi falhou");
        return false;
    }
    LmxxfNrCapabilities cap{};
    cap.struct_size = sizeof cap;
    if (api.QueryCapabilities && api.QueryCapabilities(&cap) == LMXXF_NR_OK)
        Log("rede", Fmt("runtime: entrada maxima %ux%u, hip_ready %u, gfx1201 %u", cap.max_input_width,
                        cap.max_input_height, cap.hip_ready, cap.gfx1201_target));
    setSkip = reinterpret_cast<int32_t (*)(void*, const uint32_t*, uint32_t)>(GetProcAddress(mod, "LmxxfNrSetDynamicSkip"));
    Log("rede", setSkip ? "profundidade dinamica disponivel (runtime v3)" : "runtime sem profundidade dinamica");
    assets = pasta + L"\\native-game-tiled-assets";
    LmxxfNrCreateInfo ci{};
    ci.struct_size = sizeof ci;
    ci.device = g.dev.Get();
    ci.queue = g.fila.Get();
    ci.assets_directory = assets.c_str();
    if (Erro(api.Create(&ci, &ctx), "Create"))
    {
        ctx = nullptr;
        return false;
    }
    if (Erro(api.PrepareSession(ctx), "PrepareSession"))
        return false;
    Log("rede", "runtime pronto: " + Utf8(assets));
    return true;
}

bool Rede::DefineProfundidade(const std::vector<uint32_t>& blocos)
{
    if (!setSkip || !ctx)
        return false;
    return setSkip(ctx, blocos.empty() ? nullptr : blocos.data(), (uint32_t)blocos.size()) == LMXXF_NR_OK;
}

void Rede::Encerra()
{
    if (ctx)
    {
        if (emVoo && api.AbandonJob)
            api.AbandonJob(ctx, job.handle);
        api.Drain(ctx);
        api.Destroy(ctx);
        ctx = nullptr;
    }
    emVoo = false;
}

bool Rede::Prepara(ID3D12GraphicsCommandList* c, ID3D12Resource* cor, D3D12_RESOURCE_STATES estadoCor, UINT w, UINT h,
                   ID3D12Resource* movimento, D3D12_RESOURCE_STATES estadoMov, bool zeraHistorico, const ParamsRede& p)
{
    if (!Pronta() || emVoo)
        return false;
    ++quadro;
    LmxxfNrFrameInfo fi{};
    fi.struct_size = sizeof fi;
    fi.session_id = 1;
    fi.frame_id = quadro;
    fi.list_generation = quadro;
    fi.command_list = c;
    fi.color_width = w;
    fi.color_height = h;
    fi.color = cor;
    fi.color_state = (uint32_t)estadoCor;
    fi.model_scale = 1.f;
    fi.transfer_strength = std::clamp(p.detalhe, 0.f, 1.f);
    fi.color_strength = std::clamp(p.cor, 0.f, 1.f);
    if (fi.transfer_strength != 1.f || fi.color_strength != 1.f)
        fi.flags |= LMXXF_NR_FRAME_FLAG_STRENGTH;
    fi.passes = (uint32_t)std::clamp(p.passes, 1, 3);
    if (p.redeCompleta)
        fi.flags |= LMXXF_NR_FRAME_FLAG_FULL_NETWORK;
    fi.control_tone = std::clamp(p.tom, -1.f, 4.f);
    fi.control_structure = std::clamp(p.estrutura, -1.f, 4.f);
    fi.control_skin = std::clamp(p.pele, -1.f, 4.f);
    fi.control_other = std::clamp(p.outro, -1.f, 4.f);
    fi.control_style = std::clamp(p.estilo, -1.f, 4.f);
    if (fi.control_tone != 1.f || fi.control_structure != 1.f || fi.control_skin != 1.f || fi.control_other != 1.f ||
        fi.control_style != 1.f)
        fi.flags |= LMXXF_NR_FRAME_FLAG_CONTROLS;
    if (movimento)
    {
        // historico temporal da rede: a resposta anterior reprojetada pelo movimento simulado (fluxo optico)
        fi.flags |= LMXXF_NR_FRAME_FLAG_TEMPORAL;
        fi.motion = movimento;
        fi.motion_state = (uint32_t)estadoMov;
        fi.motion_width = w;
        fi.motion_height = h;
        fi.motion_scale_x = 1.f;
        fi.motion_scale_y = 1.f;
        fi.history_reset = zeraHistorico ? 1u : 0u;
        fi.output_smooth = std::clamp(p.suaviza, 0.f, 0.8f);
    }
    else
        fi.history_reset = 1;
    job = {};
    job.struct_size = sizeof job;
    if (Erro(api.PrepareFrame(ctx, &fi, &job), "PrepareFrame"))
        return false;
    if (Erro(api.RecordInputs(ctx, job.handle, c), "RecordInputs"))
    {
        api.CancelUnsubmitted(ctx, job.handle);
        return false;
    }
    emVoo = true;
    return true;
}

bool Rede::Enfileira(ID3D12CommandQueue* fila)
{
    if (!emVoo)
        return false;
    if (Erro(api.EnqueueHip(ctx, job.handle, fila), "EnqueueHip"))
    {
        api.AbandonJob(ctx, job.handle);
        emVoo = false;
        return false;
    }
    return true;
}

ID3D12Resource* Rede::Saida(ID3D12GraphicsCommandList* c)
{
    if (!emVoo)
        return nullptr;
    if (Erro(api.RecordOutputs(ctx, job.handle, c), "RecordOutputs"))
    {
        api.AbandonJob(ctx, job.handle);
        emVoo = false;
        return nullptr;
    }
    return static_cast<ID3D12Resource*>(job.private_output);
}

void Rede::Aposenta()
{
    if (!emVoo)
        return;
    api.Retire(ctx, job.handle);
    emVoo = false;
}

} // namespace ov
