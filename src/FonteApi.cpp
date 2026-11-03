#include "FonteApi.h"

namespace ov {

void FonteApi::DefineAlvo(HWND janela)
{
    if (cab)
        cab->janela = uint64_t(reinterpret_cast<uintptr_t>(GetAncestor(janela, GA_ROOT)));
}

bool FonteApi::Abre(Gpu& g, DWORD pid_, HWND janela)
{
    Fecha();
    gpu = &g;
    wchar_t nome[96];
    dlss5ov::NomeMapaApi(nome, 96, pid_);
    mapa = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, nome);
    if (!mapa)
        return false;
    cab = static_cast<dlss5ov::CabApi*>(MapViewOfFile(mapa, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(dlss5ov::CabApi)));
    if (!cab || cab->versao != dlss5ov::kVersaoApi || cab->tamanho != sizeof(dlss5ov::CabApi))
    {
        Fecha();
        return false;
    }
    pid = pid_;
    pronto = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    DefineAlvo(janela);
    Batimento();
    Log("api", Fmt("conectado ao modulo do jogo (pid %lu)", (unsigned long)pid));
    return true;
}

void FonteApi::SoltaTexturas()
{
    if (gpu && geracao)
        gpu->Espera();
    geracao = 0;
    cerca.Reset();
    for (auto& t : cor)
        t.r.Reset();
    for (auto& t : prof)
        t.r.Reset();
    profGeracao = 0;
    profPega = nullptr;
}

void FonteApi::Fecha()
{
    geracao = 0; // quem fecha ja parou a GPU (e na saida do programa nao ha mais GPU para esperar)
    SoltaTexturas();
    if (cab)
    {
        cab->batimentoOverlay = 0;
        UnmapViewOfFile(cab);
    }
    cab = nullptr;
    if (mapa)
        CloseHandle(mapa);
    mapa = nullptr;
    if (pronto)
        CloseHandle(pronto);
    pronto = nullptr;
    pid = 0;
    ultimoPego = 0;
}

void FonteApi::Batimento()
{
    if (cab)
        cab->batimentoOverlay = GetTickCount64();
}

bool FonteApi::AbreGeracao()
{
    const uint32_t g = cab->geracao;
    if (!g)
    {
        if (geracao)
            SoltaTexturas(); // o jogo soltou: aqui tambem, senao a VRAM nao volta
        return false;
    }
    if (g == geracao)
        return true;
    SoltaTexturas();
    wchar_t nome[128];
    auto abre = [&](const wchar_t* qual, uint32_t s, REFIID iid, void** p) {
        dlss5ov::NomeRecursoApi(nome, 128, pid, g, qual, s);
        HANDLE h = nullptr;
        HRESULT hr = gpu->dev->OpenSharedHandleByName(nome, GENERIC_ALL, &h);
        if (SUCCEEDED(hr))
        {
            hr = gpu->dev->OpenSharedHandle(h, iid, p);
            CloseHandle(h);
        }
        if (FAILED(hr))
            Log("api", Fmt("geracao %u: nao abriu %s %u: 0x%08lx", g, Utf8(qual).c_str(), s, (unsigned long)hr));
        return SUCCEEDED(hr);
    };
    bool ok = abre(L"cerca", 0, IID_PPV_ARGS(&cerca));
    for (uint32_t s = 0; ok && s < dlss5ov::kSlotsApi; ++s)
    {
        ok = abre(L"cor", s, IID_PPV_ARGS(&cor[s].r));
        cor[s].w = cab->w, cor[s].h = cab->h, cor[s].fmt = DXGI_FORMAT(cab->fmt), cor[s].estado = D3D12_RESOURCE_STATE_COMMON;
    }
    if (!ok)
    {
        SoltaTexturas();
        return false;
    }
    geracao = g;
    ultimoPego = 0;
    cerca->SetEventOnCompletion(1, pronto);
    Log("api", (cab->api == dlss5ov::kApiVulkan ? std::string("Vulkan") : Fmt("D3D%u", cab->api)) +
                   Fmt(": geracao %u, back buffer %ux%u, formato %u", g, cab->w, cab->h, cab->fmt));
    return true;
}

bool FonteApi::Pega(Tex*& t, uint64_t& n)
{
    if (!cab || !AbreGeracao())
        return false;
    const UINT64 feito = cerca->GetCompletedValue();
    int s = -1;
    uint64_t maior = 0;
    for (int i = 0; i < int(dlss5ov::kSlotsApi); ++i)
    {
        const uint64_t q = cab->quadro[i];
        if (q > ultimoPego && q <= feito && q > maior)
        {
            s = i;
            maior = q;
        }
    }
    if (s < 0)
        return false;
    n = ultimoPego = maior;
    geracaoPega = geracao;
    t = &cor[s];
    profPega = nullptr;
    const uint32_t gp = cab->profGeracao;
    if (gp && gp != profGeracao)
    {
        bool ok = true;
        wchar_t nome[128];
        if (profGeracao)
            gpu->Espera(); // as listas ainda podem ler as velhas
        for (auto& x : prof)
            x.r.Reset();
        for (uint32_t i = 0; ok && i < cab->profSlots && i < dlss5ov::kSlotsProf; ++i)
        {
            dlss5ov::NomeRecursoApi(nome, 128, pid, gp, L"prof", i);
            HANDLE h = nullptr;
            ok = SUCCEEDED(gpu->dev->OpenSharedHandleByName(nome, GENERIC_ALL, &h));
            if (ok)
            {
                ok = SUCCEEDED(gpu->dev->OpenSharedHandle(h, IID_PPV_ARGS(&prof[i].r)));
                CloseHandle(h);
            }
            prof[i].w = cab->profW, prof[i].h = cab->profH, prof[i].fmt = DXGI_FORMAT_R32_FLOAT;
        }
        if (ok)
            profGeracao = gp, Log("api", Fmt("profundidade do jogo: %ux%u", cab->profW, cab->profH));
    }
    // a profundidade deste quadro, em qualquer slot; a GPU do jogo terminou a copia antes da cerca dele
    for (uint32_t p = 0; gp && gp == profGeracao && p < dlss5ov::kSlotsProf && !profPega; ++p)
        if (cab->quadroProf[p] == maior && prof[p].r)
        {
            prof[p].estado = D3D12_RESOURCE_STATE_COMMON;
            profPega = &prof[p];
        }
    cerca->SetEventOnCompletion(maior + 1, pronto);
    return true;
}

void FonteApi::AvaliaProf(float cobertura)
{
    if (!cab || cab->profExata || cab->profTroca != cab->profTrocaFeita)
    {
        profVazia = 0;
        return;
    }
    // deu a volta em todos sem achar a cena (menu, ceu): trocar de buffer custa; espera antes de tentar de novo
    if (cab->profCiclos != profCiclosVistos)
    {
        profCiclosVistos = cab->profCiclos;
        profEspera = 300;
    }
    if (profEspera > 0)
    {
        --profEspera;
        profVazia = 0;
        return;
    }
    if (cobertura >= 0.5f)
    {
        profVazia = 0;
        return;
    }
    if (++profVazia < 20)
        return;
    profVazia = 0;
    Log("api", Fmt("buffer de profundidade quase vazio (%.0f%% da tela): pedindo outro ao modulo", cobertura * 100.f));
    cab->profTroca = cab->profTroca + 1;
}

void FonteApi::Consumido(uint64_t n)
{
    // o numero de outra geracao liberaria um slot que ainda vai ser lido
    if (cab && geracaoPega == cab->geracao && n > cab->consumido)
        cab->consumido = n;
}

void LimiteJogo::Fecha()
{
    if (cab)
    {
        cab->limiteFps = 0;
        UnmapViewOfFile(cab);
    }
    cab = nullptr;
    if (mapa)
        CloseHandle(mapa);
    mapa = nullptr;
    aberto = 0;
}

void LimiteJogo::Define(DWORD pid, UINT fps)
{
    if (!pid || !fps)
    {
        Fecha();
        return;
    }
    if (pid != aberto)
    {
        Fecha();
        const ULONGLONG agora = GetTickCount64();
        if (agora < tentaEm)
            return;
        tentaEm = agora + 1000;
        wchar_t nome[96];
        dlss5ov::NomeMapaApi(nome, 96, pid);
        mapa = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, nome);
        if (mapa)
            cab = static_cast<dlss5ov::CabApi*>(MapViewOfFile(mapa, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(dlss5ov::CabApi)));
        if (!cab || cab->versao != dlss5ov::kVersaoApi || cab->tamanho != sizeof(dlss5ov::CabApi))
        {
            Fecha();
            return;
        }
        aberto = pid;
        Log("api", Fmt("teto de FPS do jogo: %u (pid %lu)", fps, (unsigned long)pid));
    }
    cab->limiteFps = fps;
    cab->batimentoLimite = GetTickCount64();
}

} // namespace ov
