#include "Rede.h"
#include <algorithm>
#include <filesystem>
#include <set>

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

// Pasta de assets da rede pequena: hardlinks dos assets da grande (copia se o hardlink falhar), menos os pesos que a
// pequena troca (runtime\pequena, com o network-layout.txt: blocos ausentes e deslocamentos). Refeita so quando o
// network-layout.txt da pequena muda.
static bool MontaAssetsPequena(const std::wstring& pasta, std::wstring& saida)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path base = fs::path(pasta) / L"native-game-tiled-assets", peq = fs::path(pasta) / L"pequena",
                   dst = fs::path(pasta) / L"native-game-tiled-assets-pequena";
    if (!fs::exists(peq / L"network-layout.txt") || !fs::exists(base))
        return false;
    saida = dst.wstring();
    const auto marca = dst / L"network-layout.txt";
    if (fs::exists(marca) && fs::file_size(marca, ec) == fs::file_size(peq / L"network-layout.txt", ec) &&
        fs::last_write_time(marca, ec) == fs::last_write_time(peq / L"network-layout.txt", ec))
        return true;
    fs::remove_all(dst, ec);
    fs::create_directories(dst, ec);
    std::set<std::wstring> proprios; // nomes (sem .f16/.f32) que a pequena traz: o da grande nao entra em nenhuma extensao
    for (auto& e : fs::directory_iterator(peq, ec))
        if (e.is_regular_file())
            proprios.insert(e.path().stem().wstring());
    for (auto it = fs::recursive_directory_iterator(base, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        const auto rel = fs::relative(it->path(), base, ec);
        const auto alvo = dst / rel;
        if (it->is_directory())
        {
            fs::create_directories(alvo, ec);
            continue;
        }
        const auto ext = it->path().extension().wstring();
        if (rel.parent_path().empty() && (ext == L".f16" || ext == L".f32") && proprios.count(it->path().stem().wstring()))
            continue;
        fs::create_hard_link(it->path(), alvo, ec);
        if (ec)
        {
            ec.clear();
            fs::copy_file(it->path(), alvo, fs::copy_options::overwrite_existing, ec);
        }
    }
    // remove antes de copiar: se o destino for um hardlink da grande, sobrescrever alteraria a grande tambem
    for (auto& e : fs::directory_iterator(peq, ec))
        if (e.is_regular_file())
        {
            fs::remove(dst / e.path().filename(), ec);
            fs::copy_file(e.path(), dst / e.path().filename(), fs::copy_options::overwrite_existing, ec);
        }
    fs::last_write_time(marca, fs::last_write_time(peq / L"network-layout.txt", ec), ec);
    Log("rede", "assets da rede pequena montados: " + Utf8(saida));
    return fs::exists(marca);
}

bool Rede::Inicia(Gpu& g, const std::wstring& pasta, bool pequena)
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
    setGuias = reinterpret_cast<int32_t (*)(void*, void*, void*, uint32_t)>(GetProcAddress(mod, "LmxxfNrSetHistoryGuides"));
    setForcaHist = reinterpret_cast<int32_t (*)(void*, float)>(GetProcAddress(mod, "LmxxfNrSetHistoryStrength"));
    Log("rede", setGuias ? "guias do historico da rede disponiveis (LmxxfNrSetHistoryGuides)"
                         : "runtime sem LmxxfNrSetHistoryGuides: o historico da rede, se ligado, fica estatico");
    assets = pasta + L"\\native-game-tiled-assets";
    if (pequena && !MontaAssetsPequena(pasta, assets))
    {
        Log("rede", "rede pequena indisponivel (falta runtime\\pequena): usando a grande");
        assets = pasta + L"\\native-game-tiled-assets";
    }
    Log("rede", pequena ? "rede: pequena 310.9.16 (31 blocos)" : "rede: grande (71 blocos)");
    dev = g.dev.Get();
    if (!fila)
    {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Ck(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&fila)), "CreateCommandQueue(rede)");
        fila->SetName(L"fila da rede");
        Ck(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&cerca)), "CreateFence(rede)");
        evento = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        for (int i = 0; i < 2; ++i)
        {
            Ck(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al[i])), "allocator(rede)");
            Ck(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al[i].Get(), nullptr, IID_PPV_ARGS(&cl[i])),
               "list(rede)");
            cl[i]->Close();
        }
    }
    LmxxfNrCreateInfo ci{};
    ci.struct_size = sizeof ci;
    ci.device = g.dev.Get();
    ci.queue = fila.Get();
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

void Rede::GaranteGuias(UINT w, UINT h)
{
    if (guiaMov.w == w && guiaMov.h == h)
        return;
    guiaMov.Cria(dev, w, h, DXGI_FORMAT_R32G32_FLOAT, L"guia de movimento do historico da rede");
    guiaProf.Cria(dev, w, h, DXGI_FORMAT_R32_FLOAT, L"guia de profundidade do historico da rede");
}

void Rede::ZeraHistorico()
{
    if (ctx && api.ResetHistory)
        Erro(api.ResetHistory(ctx), "ResetHistory");
}

bool Rede::DefineProfundidade(const std::vector<uint32_t>& blocos)
{
    if (!setSkip || !ctx)
        return false;
    return setSkip(ctx, blocos.empty() ? nullptr : blocos.data(), (uint32_t)blocos.size()) == LMXXF_NR_OK;
}

void Rede::Encerra()
{
    Descarta();
    if (ctx)
    {
        api.Drain(ctx);
        api.Destroy(ctx);
        ctx = nullptr;
    }
    emVoo = false;
}

void Rede::EsperaChegar()
{
    if (!emVoo || cerca->GetCompletedValue() >= valorJob)
        return;
    Ck(cerca->SetEventOnCompletion(valorJob, evento), "SetEventOnCompletion(rede)");
    WaitForSingleObject(evento, INFINITE);
}

void Rede::Descarta()
{
    if (!emVoo)
        return;
    EsperaChegar();
    Aposenta();
}

void Rede::CopiaEntrada(ID3D12GraphicsCommandList* c, Tex& fed)
{
    if (emVoo)
        return;
    if (entrada.w != fed.w || entrada.h != fed.h || entrada.fmt != fed.fmt)
        entrada.Cria(dev, fed.w, fed.h, fed.fmt, L"entrada da rede", true, true);
    const auto antes = fed.estado;
    fed.Para(c, D3D12_RESOURCE_STATE_COPY_SOURCE);
    entrada.Para(c, D3D12_RESOURCE_STATE_COPY_DEST);
    c->CopyResource(entrada.r.Get(), fed.r.Get());
    entrada.Para(c, D3D12_RESOURCE_STATE_COMMON); // a fila da rede pega daqui
    fed.Para(c, antes);
}

bool Rede::Envia(ID3D12Fence* cercaOverlay, UINT64 valorOverlay, const ParamsRede& p)
{
    if (!Pronta() || emVoo || !entrada.r)
        return false;
    const D3D12_RESOURCE_STATES SRV =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Ck(fila->Wait(cercaOverlay, valorOverlay), "Wait(overlay)");
    // guias do historico da rede: as deste envio, ou nenhuma (o runtime volta ao historico estatico)
    const bool comMov = setGuias && usaMov && guiaMov.r, comProf = comMov && usaProf && guiaProf.r;
    if (setGuias)
    {
        const int32_t r = setGuias(ctx, comMov ? guiaMov.r.Get() : nullptr, comProf ? guiaProf.r.Get() : nullptr, 1);
        if (r == LMXXF_NR_NOT_IMPLEMENTED)
        {
            // adaptador novo com um lmxxf_upstream.dll anterior a este branch: o historico fica estatico
            Log("rede", "lmxxf_upstream.dll sem LmxxfNrSetHistoryGuides: o historico da rede fica estatico");
            setGuias = nullptr;
        }
        else if (!avisouGuias && (r != LMXXF_NR_OK || comMov))
        {
            avisouGuias = true;
            if (r != LMXXF_NR_OK)
                Erro(r, "SetHistoryGuides");
            else
                Log("rede", comProf ? "historico da rede: guias de movimento e profundidade entregues ao adaptador"
                                    : "historico da rede: guia de movimento entregue ao adaptador");
        }
    }
    // forca do historico (o adaptador so repassa quando muda)
    if (setForcaHist)
    {
        const int32_t r = setForcaHist(ctx, std::clamp(p.forcaHistorico, 0.f, 1.f));
        if (r == LMXXF_NR_NOT_IMPLEMENTED)
        {
            Log("rede", "lmxxf_upstream.dll sem LmxxfNrSetHistoryStrength: forca do historico fixa em 1");
            setForcaHist = nullptr;
        }
        else if (r != LMXXF_NR_OK)
            Erro(r, "SetHistoryStrength");
    }
    // entrada
    Ck(al[0]->Reset(), "reset(rede0)");
    Ck(cl[0]->Reset(al[0].Get(), nullptr), "list(rede0)");
    entrada.Para(cl[0].Get(), SRV);
    // o runtime le as guias em NON_PIXEL_SHADER_RESOURCE nas duas listas do trabalho
    if (comMov)
        guiaMov.Para(cl[0].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (comProf)
        guiaProf.Para(cl[0].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!Prepara(cl[0].Get(), p))
    {
        cl[0]->Close();
        entrada.estado = D3D12_RESOURCE_STATE_COMMON; // a lista nao vai rodar
        guiaMov.estado = guiaProf.estado = D3D12_RESOURCE_STATE_COMMON;
        return false;
    }
    entrada.Para(cl[0].Get(), D3D12_RESOURCE_STATE_COMMON);
    Ck(cl[0]->Close(), "close(rede0)");
    ID3D12CommandList* l0[] = {cl[0].Get()};
    fila->ExecuteCommandLists(1, l0);
    // HIP
    if (Erro(api.EnqueueHip(ctx, job.handle, fila.Get()), "EnqueueHip"))
    {
        api.AbandonJob(ctx, job.handle);
        emVoo = false;
        return false;
    }
    // saida: a resposta do runtime vai para uma textura nossa (o runtime reaproveita a dele no trabalho seguinte)
    Ck(al[1]->Reset(), "reset(rede1)");
    Ck(cl[1]->Reset(al[1].Get(), nullptr), "list(rede1)");
    if (Erro(api.RecordOutputs(ctx, job.handle, cl[1].Get()), "RecordOutputs"))
    {
        cl[1]->Close();
        api.AbandonJob(ctx, job.handle);
        emVoo = false;
        return false;
    }
    auto* out = static_cast<ID3D12Resource*>(job.private_output);
    const auto d = out->GetDesc();
    if (resposta.w != d.Width || resposta.h != d.Height || resposta.fmt != d.Format)
        resposta.Cria(dev, UINT(d.Width), d.Height, d.Format, L"resposta da rede", false);
    Barreira(cl[1].Get(), out, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    resposta.Para(cl[1].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    cl[1]->CopyResource(resposta.r.Get(), out);
    resposta.Para(cl[1].Get(), D3D12_RESOURCE_STATE_COMMON);
    Barreira(cl[1].Get(), out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (comMov)
        guiaMov.Para(cl[1].Get(), D3D12_RESOURCE_STATE_COMMON);
    if (comProf)
        guiaProf.Para(cl[1].Get(), D3D12_RESOURCE_STATE_COMMON);
    Ck(cl[1]->Close(), "close(rede1)");
    ID3D12CommandList* l1[] = {cl[1].Get()};
    fila->ExecuteCommandLists(1, l1);
    Ck(fila->Signal(cerca.Get(), ++valor), "Signal(rede)");
    valorJob = valor;
    return true;
}

bool Rede::Prepara(ID3D12GraphicsCommandList* c, const ParamsRede& p)
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
    fi.color_width = entrada.w;
    fi.color_height = entrada.h;
    fi.color = entrada.r.Get();
    fi.color_state = (uint32_t)entrada.estado;
    fi.model_scale = 1.f;
    fi.transfer_strength = std::clamp(p.detalhe, 0.f, 1.f);
    fi.color_strength = std::clamp(p.cor, 0.f, 1.f);
    if (fi.transfer_strength != 1.f || fi.color_strength != 1.f)
        fi.flags |= LMXXF_NR_FRAME_FLAG_STRENGTH;
    fi.passes = (uint32_t)std::clamp(p.passes, 1, 3);
    if (p.redeCompleta)
        fi.flags |= LMXXF_NR_FRAME_FLAG_FULL_NETWORK;
    // os controles da NVIDIA (estilo, tom, estrutura, pele, outro): o adaptador os aplica nos pesos do bloco 0
    fi.control_tone = std::clamp(p.tom, -2.f, 8.f);
    fi.control_structure = std::clamp(p.estrutura, -2.f, 8.f);
    fi.control_skin = std::clamp(p.pele, -1.f, 4.f);
    fi.control_other = std::clamp(p.outro, -1.f, 4.f);
    fi.control_style = std::clamp(p.estilo, -1.f, 4.f);
    if (fi.control_tone != 1.f || fi.control_structure != 1.f || fi.control_skin != 1.f || fi.control_other != 1.f ||
        fi.control_style != 1.f)
        fi.flags |= LMXXF_NR_FRAME_FLAG_CONTROLS;
    // o adaptador ignora FLAG_TEMPORAL/motion/history_reset (adaptador.cpp): o historico da propria rede e o
    // DLSS5_TEMPORAL_MODE do ambiente, com as guias de LmxxfNrSetHistoryGuides (Envia) e o ResetHistory (ZeraHistorico)
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

void Rede::Aposenta()
{
    if (!emVoo)
        return;
    Erro(api.Retire(ctx, job.handle), "Retire");
    emVoo = false;
}

} // namespace ov
