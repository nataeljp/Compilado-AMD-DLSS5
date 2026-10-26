#include "FonteBuffer.h"

namespace ov {

bool FonteBuffer::Abre(Gpu& g, DWORD pid_)
{
    if (cab && pid == pid_)
        return true;
    Fecha();
    gpu = &g;
    wchar_t nome[96];
    dlss5ov::NomeMapa(nome, 96, pid_);
    mapa = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, nome);
    if (!mapa)
        return false;
    cab = static_cast<dlss5ov::Cabecalho*>(MapViewOfFile(mapa, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(dlss5ov::Cabecalho)));
    if (!cab || cab->versao != dlss5ov::kVersao || cab->tamanho != sizeof(dlss5ov::Cabecalho))
    {
        Log("buffer", cab ? Fmt("modulo do jogo com outro protocolo (versao %u)", cab->versao) : "MapViewOfFile falhou");
        Fecha();
        return false;
    }
    pid = pid_;
    dlss5ov::NomeEventoPublica(nome, 96, pid_);
    evPublica = OpenEventW(EVENT_MODIFY_STATE, FALSE, nome);
    Log("buffer", Fmt("conectado ao modulo do jogo %s (pid %lu)", cab->exe, (unsigned long)pid));
    return true;
}

void FonteBuffer::Fecha()
{
    if (cab)
    {
        cab->ativo = 0;
        cab->espera = 0;
        cab->batimentoOverlay = 0;
        UnmapViewOfFile(cab);
    }
    cab = nullptr;
    if (mapa)
        CloseHandle(mapa);
    mapa = nullptr;
    geracao = 0;
    pid = 0;
    cercaJogo.Reset();
    cercaOverlay.Reset();
    for (auto& t : cor)
        t.r.Reset();
    for (auto& t : mov)
        t.r.Reset();
    for (auto& t : prof)
        t.r.Reset();
    for (auto& t : edicao)
        t.r.Reset();
    if (evento)
        CloseHandle(evento);
    evento = nullptr;
    if (evPublica)
        CloseHandle(evPublica);
    evPublica = nullptr;
}

void FonteBuffer::Batimento(bool querQuadros, bool aplica, uint32_t espera, uint32_t repete, uint32_t gerado)
{
    if (!cab)
        return;
    cab->ativo = querQuadros ? 1 : 0;
    cab->aplica = aplica ? 1 : 0;
    cab->espera = evPublica ? espera : 0;
    cab->repete = repete;
    cab->gerado = gerado;
    cab->batimentoOverlay = GetTickCount64();
}

bool FonteBuffer::JogoVivo() const
{
    return cab && GetTickCount64() - cab->batimentoJogo < 2000;
}

bool FonteBuffer::AbreRecurso(const wchar_t* qual, uint32_t slot, REFIID iid, void** p)
{
    wchar_t nome[128];
    dlss5ov::NomeRecurso(nome, 128, pid, geracao, qual, slot);
    HANDLE h = nullptr;
    if (FAILED(gpu->dev->OpenSharedHandleByName(nome, GENERIC_ALL, &h)) || !h)
        return false;
    const HRESULT hr = gpu->dev->OpenSharedHandle(h, iid, p);
    CloseHandle(h);
    return SUCCEEDED(hr);
}

bool FonteBuffer::AbreGeracao()
{
    const uint32_t g = cab->geracao;
    if (!g)
        return false;
    if (g == geracao)
        return true;
    geracao = g;
    auto tex = [&](Tex& t, const wchar_t* qual, uint32_t s, UINT w, UINT h, DXGI_FORMAT f) {
        t.r.Reset();
        if (!AbreRecurso(qual, s, IID_PPV_ARGS(&t.r)))
            return false;
        t.w = w, t.h = h, t.fmt = f, t.estado = D3D12_RESOURCE_STATE_COMMON;
        return true;
    };
    bool ok = AbreRecurso(L"cercaJogo", 0, IID_PPV_ARGS(&cercaJogo)) &&
              AbreRecurso(L"cercaOverlay", 0, IID_PPV_ARGS(&cercaOverlay));
    for (uint32_t s = 0; ok && s < dlss5ov::kSlotsEntrada; ++s)
        ok = tex(cor[s], L"cor", s, cab->corW, cab->corH, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
             tex(mov[s], L"mov", s, cab->movW, cab->movH, DXGI_FORMAT_R32G32_FLOAT) &&
             tex(prof[s], L"prof", s, cab->profW, cab->profH, DXGI_FORMAT_R32_FLOAT);
    for (uint32_t s = 0; ok && s < dlss5ov::kSlotsEdicao; ++s)
        ok = tex(edicao[s], L"edicao", s, cab->corW, cab->corH, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (!ok)
    {
        Log("buffer", Fmt("geracao %u: nao abriu as texturas do jogo", g));
        geracao = 0;
        return false;
    }
    valorOverlay = cercaOverlay->GetCompletedValue();
    ultimoPego = 0;
    slotEscrita = ultimoPublicado = -1;
    cercaUltimo = 0;
    Log("buffer", Fmt("geracao %u: cor %ux%u, movimento %ux%u, profundidade %ux%u", g, cab->corW, cab->corH, cab->movW,
                      cab->movH, cab->profW, cab->profH));
    return true;
}

HANDLE FonteBuffer::Evento()
{
    if (!cab || !Conectada())
        return nullptr;
    if (!evento)
        evento = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    cercaJogo->SetEventOnCompletion(ultimoPego + 1, evento);
    return evento;
}

bool FonteBuffer::Pega(Quadro& q)
{
    if (!cab || !AbreGeracao())
        return false;
    const UINT64 pronto = cercaJogo->GetCompletedValue();
    // o slot mais novo que ja saiu da GPU do jogo
    int s = -1;
    for (int i = 0; i < int(dlss5ov::kSlotsEntrada); ++i)
    {
        const uint64_t n = cab->entrada[i].quadro;
        if (n && n > ultimoPego && n <= pronto && (s < 0 || n > cab->entrada[s].quadro))
            s = i;
    }
    if (s < 0)
        return false;
    const dlss5ov::SlotEntrada& e = cab->entrada[s];
    q.n = e.quadro;
    q.cor = &cor[s];
    q.mov = &mov[s];
    q.prof = &prof[s];
    q.reset = e.reset != 0;
    q.perto = e.perto, q.longe = e.longe, q.fovY = e.fovY;
    q.profInvertida = cab->profInvertida == 1;
    q.profInvertidaConhecida = cab->profInvertida != 2;
    q.profInfinita = cab->profInfinita != 0;
    // quadros que o overlay pulou (o jogo foi mais rapido) tambem liberam os slots deles
    ultimoPego = q.n;
    return true;
}

void FonteBuffer::Consumido(uint64_t n)
{
    if (cab && n > cab->consumido)
        cab->consumido = n;
}

Tex* FonteBuffer::EdicaoLivre()
{
    slotEscrita = -1;
    if (!Conectada())
        return nullptr;
    // a escrita anterior tem de ter terminado (o jogo ja ve a ultima publicada) ...
    if (cercaUltimo && cercaOverlay->GetCompletedValue() < cercaUltimo)
        return nullptr;
    const int k = (ultimoPublicado + 1) % int(dlss5ov::kSlotsEdicao);
    // ... e todo quadro do jogo que leu este slot ja saiu da GPU dele
    if (cercaJogo->GetCompletedValue() < cab->lido[k])
        return nullptr;
    cab->edicao[k].quadro = 0; // invalido enquanto escreve
    cab->edicao[k].cerca = 0;
    MemoryBarrier();
    slotEscrita = k;
    return &edicao[k];
}

void FonteBuffer::Publica(uint64_t n, bool luzLog)
{
    if (slotEscrita < 0 || !Conectada())
        return;
    const UINT64 v = ++valorOverlay;
    if (FAILED(gpu->fila->Signal(cercaOverlay.Get(), v)))
        return;
    cab->edicao[slotEscrita].cerca = v;
    cab->edicao[slotEscrita].luz = luzLog ? 1u : 0u;
    MemoryBarrier();
    cab->edicao[slotEscrita].quadro = n;
    ultimoPublicado = slotEscrita;
    cercaUltimo = v;
    slotEscrita = -1;
    if (evPublica)
        SetEvent(evPublica);
}

} // namespace ov
