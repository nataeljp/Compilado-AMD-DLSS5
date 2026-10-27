// Lado do JOGO da captura de buffer (ver Protocolo.h). Independente do resto do overlay: compila dentro da
// DLSS5Buffer.dll (src\jogo\Modulo.cpp) e no jogo de teste. Grava na lista do jogo, depois do upscale:
//   - exporta cor (x exposicao), movimento e profundidade para os slots compartilhados, quando o overlay quer;
//   - soma a ultima edicao publicada pelo overlay, reprojetada pelos vetores do jogo, na propria cor (no lugar).
// So espera o overlay no modo sem atraso (EsperaEdicao, com prazo); sem overlay vivo (batimento), nao faz nada alem de
// manter o cabecalho.
#pragma once
#include "Protocolo.h"
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <mutex>
#include <string>
#include <vector>

namespace dlss5ov {

struct EntradasJogo
{
    // cor final do quadro (saida do upscaler: linear, sem HUD); somada no lugar
    ID3D12Resource* cor = nullptr;
    D3D12_RESOURCE_STATES corEstado = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // vetores de movimento do jogo: MV * escala = pixels do espaco do MV (movEspacoW x movEspacoH), do atual ao anterior
    ID3D12Resource* mov = nullptr;
    D3D12_RESOURCE_STATES movEstado = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    float movEscalaX = 1, movEscalaY = 1;
    uint32_t movEspacoW = 0, movEspacoH = 0; // 0 = o tamanho da textura de movimento
    ID3D12Resource* prof = nullptr;
    D3D12_RESOURCE_STATES profEstado = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // exposicao do jogo (1x1); e = texel * escalaExposicao / preExposicao. Sem textura: e = 1
    ID3D12Resource* exposicao = nullptr;
    D3D12_RESOURCE_STATES expEstado = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    float preExposicao = 1, escalaExposicao = 1;
    // regiao valida (resolucao dinamica: so o canto superior esquerdo da textura); 0 = a textura inteira
    uint32_t corValidoW = 0, corValidoH = 0, movValidoW = 0, movValidoH = 0, profValidoW = 0, profValidoH = 0;
    // o maior tamanho de render que o jogo declarou ao FSR ao criar o contexto (0 = desconhecido: o modulo chegou depois);
    // movimento em resolucao de render e profundidade cabem nele, entao a resolucao dinamica nao recria as texturas
    uint32_t maxRenderW = 0, maxRenderH = 0;
    float jitterX = 0, jitterY = 0;
    float perto = 0, longe = 0, fovY = 0;
    bool reset = false, profInvertida = false, profInfinita = false, hdr = true;
    bool profInvertidaConhecida = true; // false: o modulo chegou depois de o jogo dizer (o overlay mede)
};

class ClienteBuffer
{
  public:
    // cria a memoria compartilhada (nome pelo pid deste processo) e os shaders; false = sem D3DCompile ou sem memoria
    bool Inicia(ID3D12Device* d, const char* exe);
    // compila os shaders do modulo em segundo plano (chamada no carregamento da DLL; Inicia tambem chama). Enquanto
    // compila, Inicia devolve false e Compilando() e true: tentar de novo no proximo quadro
    static void PreCompila();
    static bool Compilando();
    void Encerra();
    // grava os passes na lista `c` (que o jogo vai submeter); devolve o numero do quadro (0 = nada gravado)
    uint64_t Quadro(ID3D12GraphicsCommandList* c, const EntradasJogo& e);
    // sem atraso (o overlay liga em cab->espera): chamar ANTES de Quadro, fora do mutex `mx` que protege as outras
    // chamadas. Se o quadro anterior exportou, espera no CPU (ate kPrazoEspera ms) a edicao dele ficar pronta.
    void EsperaEdicao(std::mutex& mx);
    static constexpr DWORD kPrazoEspera = 100;
    // contadores para o log do modulo (diagnostico do jogo de verdade): quadros, exportados, com edicao aplicada e a idade
    // da edicao aplicada (quadros do jogo desde o quadro em que a rede a fez); esperas (sem atraso), tempo e estouros
    struct Diag
    {
        uint64_t quadros = 0, exportados = 0, aplicados = 0, idadeSoma = 0, idadeMax = 0;
        uint64_t esperas = 0, estouros = 0;
        double esperaMs = 0, esperaMsMax = 0;
        // por que a espera falhou: a edicao nao saiu no prazo, o overlay sem batimento (> 1 s), a cerca da GPU nao chegou
        // depois do aviso; e os quadros que nao esperaram (espera suspensa depois de 2 falhas, ou overlay sem batimento)
        uint64_t falhaPrazo = 0, falhaBatimento = 0, falhaCerca = 0, suspensos = 0, semBatimento = 0;
    } diag;
    // a lista do ultimo quadro ja foi submetida nesta fila: sinaliza a cerca do jogo (chamar depois do Execute/Present)
    void Submetido(ID3D12CommandQueue* q);
    bool OverlayVivo() const;
    uint64_t Aplicado() const { return cab ? cab->quadroAplicado : 0; } // ultimo quadro com a edicao somada
    Cabecalho* Cab() const { return cab; }
    const char* UltimoErro() const { return erro.c_str(); }
    // a gravacao do overlay: no proximo quadro com edicao, a cor do jogo antes e depois dela e a edicao recebida (na
    // resolucao do jogo) vao para `pasta` -- o que a gravacao da rede (no tamanho da rede) nao mostra
    void DespejaEm(const char* pasta);

  private:
    template <class T> using Com = Microsoft::WRL::ComPtr<T>;
    // despejo (DespejaEm): pedido, leituras em voo e o quadro delas na cerca local
    std::string despejoPedido, despejoPasta;
    struct LeituraDespejo
    {
        Com<ID3D12Resource> rb;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT w = 0, h = 0;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    } despejo[3];
    uint64_t despejoValor = 0;
    bool CopiaDespejo(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES estado, int i);
    void GravaDespejo();
    bool Garante(const EntradasJogo& e);
    bool CriaTex(Com<ID3D12Resource>& t, UINT w, UINT h, DXGI_FORMAT f, const wchar_t* qual, uint32_t slot);
    bool CriaCerca(Com<ID3D12Fence>& f, const wchar_t* qual);
    void Barreira(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b);
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT i) const;
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT i) const;
    void Srv(UINT i, ID3D12Resource* r, DXGI_FORMAT f);
    void Uav(UINT i, ID3D12Resource* r, DXGI_FORMAT f);

    ID3D12Device* dev = nullptr;
    HANDLE mapa = nullptr;
    Cabecalho* cab = nullptr;
    uint32_t geracao = 0;
    Com<ID3D12RootSignature> raiz;
    Com<ID3D12PipelineState> psoMov, psoProf, psoCor, psoLimpaRecon, psoRecon;
    static constexpr UINT kDescQuadro = 22; // por conjunto de descritores: SRV t0..t11 e UAV u0..u9
    Com<ID3D12DescriptorHeap> heap;
    // profundidade reconstruida na ancora da edicao (R32_UINT, resolucao da profundidade; local, nao compartilhada)
    Com<ID3D12Resource> recon;
    D3D12_RESOURCE_STATES estadoRecon = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    UINT passoDesc = 0, proxDesc = 0;
    Com<ID3D12Resource> cor[kSlotsEntrada], mov[kSlotsEntrada], prof[kSlotsEntrada], exp[kSlotsEntrada];
    Com<ID3D12Resource> edicao[kSlotsEdicao];
    Com<ID3D12Resource> temp; // copia da cor quando ela nao aceita UAV
    Com<ID3D12Fence> cercaJogo, cercaOverlay;
    DXGI_FORMAT fmtCor = DXGI_FORMAT_UNKNOWN, fmtMov = DXGI_FORMAT_UNKNOWN, fmtProf = DXGI_FORMAT_UNKNOWN;
    UINT64 corW = 0;
    UINT corH = 0, movW = 0, movH = 0, profW = 0, profH = 0; // tamanho das texturas (o maximo da geracao)
    UINT movValW = 0, movValH = 0, profValW = 0, profValH = 0; // a regiao valida do jogo neste quadro
    // Fila de descarte: uma geracao velha pode estar em uso por quadros ja gravados nas listas do jogo. Os recursos dela
    // so saem quando a cerca local (sinalizada depois de cada lista com os passes daqui) passar do ultimo quadro gravado.
    struct Velho
    {
        std::vector<Com<ID3D12Pageable>> itens;
        uint64_t valor = 0;
    };
    std::vector<Velho> lixeira;
    Com<ID3D12Fence> cercaLocal;
    uint64_t valorLocal = 0, pendenteLocal = 0;
    uint64_t quadro = 0, pendente = 0;
    std::string erro;
    std::vector<HANDLE> handles; // nomes da geracao atual (fechados quando ela troca)
    // movimento acumulado ate cada ancora (quadro exportado): 3 acumuladores em vai-e-volta, na resolucao do MV
    void Esquece();
    Com<ID3D12Resource> acc[3][2];
    D3D12_RESOURCE_STATES estadoAcc[3][2] = {};
    uint64_t ancora[3] = {};
    int jUlt = -1;
    UINT pp = 0;
    uint64_t ultimaExport = 0;
    bool estavaVivo = false;
    // sem atraso: a exportacao cuja edicao a ultima espera aguardou (e se chegou); dois estouros seguidos suspendem a
    // espera por 300 quadros (overlay travado ou fora de foco nao pode segurar o jogo)
    HANDLE evPublica = nullptr, evCerca = nullptr;
    uint64_t esperouPor = 0, esperaSuspensaAte = 0;
    bool esperaOk = false;
    uint32_t estourosSeguidos = 0;
    // quadros repetidos: a cor (x exposicao) dos 2 ultimos quadros exportados, e qual quadro cada copia guarda (0 = nada)
    Com<ID3D12Resource> guardada[2];
    D3D12_RESOURCE_STATES estadoGuardada[2] = {};
    uint64_t quadroGuardado[2] = {};
    // mesclado: o processado mais novo com edicao e o quadro do jogo em que ela chegou (os gerados contam a partir dele)
    uint64_t novoMostrado = 0, chegouEm = 0;
};

} // namespace dlss5ov
