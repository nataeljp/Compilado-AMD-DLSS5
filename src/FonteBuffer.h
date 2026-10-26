// Captura de buffer, lado do overlay (o protocolo esta em buffer/Protocolo.h): abre o que o modulo do jogo
// (DLSS5Buffer.dll) criou -- memoria compartilhada, cercas e texturas --, entrega cada quadro novo (cor sem HUD,
// movimento e profundidade do upscaler do jogo) e publica a edicao que o jogo soma ao quadro seguinte.
// Nunca espera a GPU do jogo: so pega um quadro depois que a cerca do jogo ja passou dele.
#pragma once
#include "Comum.h"
#include "buffer/Protocolo.h"

namespace ov {

class FonteBuffer
{
  public:
    ~FonteBuffer() { Fecha(); }
    // memoria compartilhada do modulo no jogo `pid`; false = o modulo ainda nao esta la (ou o jogo nao usa FSR/XeSS)
    bool Abre(Gpu& g, DWORD pid);
    void Fecha();
    bool Aberta() const { return cab != nullptr; }
    bool Conectada() const { return cab && geracao && geracao == cab->geracao; }
    DWORD Pid() const { return pid; }
    // a cada volta do laco: o jogo so exporta e aplica com o batimento do overlay; espera e repete = os do Cabecalho (sem
    // atraso: o jogo espera a edicao; quadros repetidos 1..4)
    void Batimento(bool querQuadros, bool aplica, uint32_t espera, uint32_t repete, uint32_t gerado, uint32_t geracao);
    // o jogo esta com o swapchain da AMD (abriu pelo overlay com a geracao de quadros)
    bool GeracaoPronta() const { return cab && cab->geracaoPronta != 0; }
    void SemEspera() { if (cab) cab->espera = 0; } // overlay pausado: o jogo nao pode ficar esperando
    void DefineInvertida(bool inv) { if (cab) cab->profInvertidaUsada = inv ? 1u : 0u; }
    bool JogoVivo() const;
    // evento que dispara quando a GPU do jogo terminar o proximo quadro (para o laco dormir ate ele)
    HANDLE Evento();

    struct Quadro
    {
        uint64_t n = 0;
        Tex* cor = nullptr;  // RGBA16F, linear x exposicao
        Tex* mov = nullptr;  // RG32F, pixels da cor, do atual ate o quadro exportado anterior
        Tex* prof = nullptr; // R32F
        bool reset = false, profInvertida = false, profInfinita = false;
        bool profInvertidaConhecida = true; // false: medir (Fluxo::InvertidaMedida)
        float perto = 0, longe = 0, fovY = 0;
    };
    // o quadro mais novo que a GPU do jogo ja terminou e o overlay ainda nao pegou
    bool Pega(Quadro& q);
    // a GPU do overlay terminou de ler o quadro n: o jogo pode reescrever o slot
    void Consumido(uint64_t n);
    // slot de edicao livre para escrever agora (nullptr = nenhum: pula a publicacao deste quadro)
    Tex* EdicaoLivre();
    // depois de submeter (na fila g.fila) a lista que escreveu a edicao do quadro n
    void Publica(uint64_t n, bool luzLog);
    std::string Exe() const { return cab ? std::string(cab->exe) : std::string(); }
    UINT CorW() const { return cab ? cab->corW : 0; }
    UINT CorH() const { return cab ? cab->corH : 0; }
    uint64_t Aplicados() const { return cab ? cab->quadroAplicado : 0; }
    dlss5ov::Cabecalho* Cab() const { return cab; }

  private:
    bool AbreGeracao();
    bool AbreRecurso(const wchar_t* qual, uint32_t slot, REFIID iid, void** p);
    Gpu* gpu = nullptr;
    DWORD pid = 0;
    HANDLE mapa = nullptr;
    dlss5ov::Cabecalho* cab = nullptr;
    uint32_t geracao = 0;
    ComPtr<ID3D12Fence> cercaJogo, cercaOverlay;
    UINT64 valorOverlay = 0;
    Tex cor[dlss5ov::kSlotsEntrada], mov[dlss5ov::kSlotsEntrada], prof[dlss5ov::kSlotsEntrada];
    Tex edicao[dlss5ov::kSlotsEdicao];
    uint64_t ultimoPego = 0;
    int slotEscrita = -1;  // slot de edicao reservado por EdicaoLivre
    int ultimoPublicado = -1;
    UINT64 cercaUltimo = 0; // valor da cerca da ultima publicacao
    HANDLE evento = nullptr;
    HANDLE evPublica = nullptr; // do jogo: disparado a cada publicacao (a espera do modo sem atraso acorda nele)
};

} // namespace ov
