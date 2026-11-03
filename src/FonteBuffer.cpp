#include "FonteBuffer.h"
#include <algorithm>
#include <cstring>

namespace ov {

bool FonteBuffer::Abre(Gpu& g, DWORD pid_)
{
    if (cab && pid == pid_)
        return true;
    Fecha();
    versaoRecusada = 0;
    gpu = &g;
    wchar_t nome[96];
    dlss5ov::NomeMapa(nome, 96, pid_);
    mapa = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, nome);
    if (!mapa)
        return false;
    cab = static_cast<dlss5ov::Cabecalho*>(MapViewOfFile(mapa, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(dlss5ov::Cabecalho)));
    const uint32_t v = cab ? cab->versao : 0;
    if (!cab || v < dlss5ov::kVersaoMinima || v > dlss5ov::kVersao || cab->tamanho != sizeof(dlss5ov::Cabecalho))
    {
        Log("buffer", cab ? Fmt("modulo do jogo com outro protocolo (versao %u; este overlay fala %u a %u)", v,
                                dlss5ov::kVersaoMinima, dlss5ov::kVersao)
                          : "MapViewOfFile falhou");
        Fecha();
        versaoRecusada = v;
        return false;
    }
    pid = pid_;
    dlss5ov::NomeEventoPublica(nome, 96, pid_);
    evPublica = OpenEventW(EVENT_MODIFY_STATE, FALSE, nome);
    Log("buffer", Fmt("conectado ao modulo do jogo %s (pid %lu, protocolo %u)", cab->exe, (unsigned long)pid, v));
    if (!evPublica) // Batimento nao pede a espera sem ele: o jogo mostra a edicao com atraso em vez de travar
        Log("buffer", Fmt("sem o evento de publicacao do jogo (erro %lu): o modo sem atraso fica desligado", GetLastError()));
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
    erroAbre.clear();
    pid = 0;
    geracao = 0; // quem fecha ja parou a GPU (e na saida do programa ela pode nem existir mais)
    SoltaTexturas();
    if (evento)
        CloseHandle(evento);
    evento = nullptr;
    if (evPublica)
        CloseHandle(evPublica);
    evPublica = nullptr;
}

void FonteBuffer::DefineDenoise(float reconstrucao, float acumulacao, float recorte, float bordas, float sigma,
                                uint32_t passes, float divisao, uint32_t grao, bool economia)
{
    if (!cab)
        return;
    cab->dnReconstrucao = reconstrucao;
    cab->dnAcumulacao = acumulacao;
    cab->dnRecorte = recorte;
    cab->dnBordas = bordas;
    cab->dnBordasSigma = sigma;
    cab->dnBordasPasses = passes;
    cab->dnDivisao = divisao;
    cab->dnGrao = grao;
    cab->dnEconomia = economia ? 1u : 0u;
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
    HRESULT hr = gpu->dev->OpenSharedHandleByName(nome, GENERIC_ALL, &h);
    if (SUCCEEDED(hr) && h)
    {
        hr = gpu->dev->OpenSharedHandle(h, iid, p);
        CloseHandle(h);
    }
    else if (SUCCEEDED(hr))
        hr = E_HANDLE;
    if (FAILED(hr))
        erroAbre = Utf8(qual) + Fmt(": 0x%08lx", (unsigned long)hr);
    return SUCCEEDED(hr);
}

std::string FonteBuffer::Erro() const
{
    if (cab && cab->erroJogo[0])
        return std::string("no jogo: ") + std::string(cab->erroJogo, strnlen(cab->erroJogo, sizeof cab->erroJogo));
    return erroAbre.empty() ? std::string() : "no overlay: " + erroAbre;
}

// o que este processo segura da geracao: enquanto houver referencia aqui, a VRAM dela nao volta, mesmo depois de o jogo soltar
void FonteBuffer::SoltaTexturas()
{
    if (gpu && geracao)
        gpu->Espera();
    geracao = 0;
    cercaJogo.Reset();
    cercaOverlay.Reset();
    for (Tex* v : {cor, mov, prof})
        for (uint32_t s = 0; s < dlss5ov::kSlotsEntrada; ++s)
            v[s].r.Reset();
    for (Tex* v : {edicao, edicaoA})
        for (uint32_t s = 0; s < dlss5ov::kSlotsEdicao; ++s)
            v[s].r.Reset();
}

bool FonteBuffer::AbreGeracao()
{
    const uint32_t g = cab->geracao;
    if (!g)
    {
        if (geracao)
            SoltaTexturas(); // o jogo soltou (formato ou tamanho novo, ou sem overlay): aqui tambem
        return false;
    }
    if (g == geracao)
        return true;
    SoltaTexturas(); // nunca a velha e a nova juntas neste processo
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
        ok = tex(cor[s], L"cor", s, cab->corW, cab->corH, DXGI_FORMAT(cab->fmtCorSlot)) &&
             tex(mov[s], L"mov", s, cab->movW, cab->movH, DXGI_FORMAT_R32G32_FLOAT) &&
             tex(prof[s], L"prof", s, cab->profW, cab->profH, DXGI_FORMAT_R32_FLOAT);
    for (uint32_t s = 0; ok && s < dlss5ov::kSlotsEdicao; ++s)
        ok = tex(edicao[s], L"edicao", s, cab->corW, cab->corH, DXGI_FORMAT(cab->fmtEdicaoSlot)) &&
             (cab->fmtEdicaoSlot != DXGI_FORMAT_R10G10B10A2_UNORM ||
              tex(edicaoA[s], L"edicaoA", s, cab->corW, cab->corH, DXGI_FORMAT_R8_UNORM));
    if (!ok)
    {
        if (geracaoRecusada != g) // tenta de novo a cada quadro: o registro diz uma vez
            Log("buffer", Fmt("geracao %u: nao abriu as texturas do jogo (%s)", g, erroAbre.c_str()));
        geracaoRecusada = g;
        geracao = 0;
        return false;
    }
    erroAbre.clear();
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
    q.jitterX = e.jitterX, q.jitterY = e.jitterY;
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

void FonteBuffer::Publica(uint64_t n, int luz)
{
    if (slotEscrita < 0 || !Conectada())
        return;
    const UINT64 v = ++valorOverlay;
    if (FAILED(gpu->fila->Signal(cercaOverlay.Get(), v)))
        return;
    cab->edicao[slotEscrita].cerca = v;
    cab->edicao[slotEscrita].luz = uint32_t(std::clamp(luz, 0, 2));
    MemoryBarrier();
    cab->edicao[slotEscrita].quadro = n;
    ultimoPublicado = slotEscrita;
    cercaUltimo = v;
    slotEscrita = -1;
    if (evPublica)
        SetEvent(evPublica);
}

} // namespace ov
