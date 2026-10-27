// Geracao de quadros da AMD dentro do jogo (captura de buffer): FidelityFX Frame Interpolation + Optical Flow + o
// swapchain de geracao do FidelityFX SDK 1.1.4, compilados do codigo fonte (MIT), ligados como o provedor de referencia
// da AMD (ffx-api/src/ffx_provider_framegeneration.cpp). O swapchain do jogo e trocado pelo da AMD NA CRIACAO (por isso
// o modulo entra antes do D3D12, instalado na pasta do jogo como dxgi.dll); a cada quadro o jogo entrega a profundidade,
// o movimento e a camera que ja manda ao FSR. Ate 4x: o SDK vai com externo\ffx_multiquadro.py (t = k/N nos shaders e
// ate 3 gerados por quadro no swapchain; a AMD so faz 2x). A interpolacao parte do backbuffer final (a saida do upscaler nao serve de imagem sem
// HUD: e a cena antes do tonemap e dos pos-efeitos, outro espaco de cor) -- os quadros gerados levam a edicao da rede,
// que ja esta no quadro do jogo.
#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>
#include <string>

namespace dlss5ov {

// a lista de comandos de verdade por tras de uma do NVIDIA Streamline (sl.interposer embrulha dispositivo, filas e
// listas; o jogo entrega a nativa ao FSR, como a NVIDIA recomenda, e envia a embrulhada a GPU, ou o contrario): as
// duas pontas viram a nativa para casar (Streamline sl.api/internal.h: StreamlineRetrieveBaseInterface). Sem Streamline,
// a propria lista
ID3D12CommandList* ListaNativa(ID3D12CommandList* l);

// o que o jogo entrega ao FSR neste quadro (estados D3D12 em que cada recurso esta naquele ponto)
struct QuadroGeracao
{
    ID3D12Resource* prof = nullptr;
    D3D12_RESOURCE_STATES profEstado = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* mov = nullptr;
    D3D12_RESOURCE_STATES movEstado = D3D12_RESOURCE_STATE_COMMON;
    UINT renderW = 0, renderH = 0;       // a resolucao de render deste quadro (de prof e mov)
    UINT maxRenderW = 0, maxRenderH = 0; // a maior (0 = a deste quadro)
    bool movNaTela = false;              // movimento na resolucao da tela
    bool movComJitter = false;           // o jitter da camera esta no movimento
    float jitterX = 0, jitterY = 0, movEscX = 1, movEscY = 1;
    float perto = 0, longe = 0, fovY = 0, msQuadro = 16.6f;
    bool invertida = true, infinita = true, hdr = false, reset = false;
};

// A UI que o jogo entrega a geracao FSR 3 dele (ffxConfigure): HUD e legendas fora da imagem -- numa textura que o
// swapchain compoe, ou num callback que a desenha em cada quadro -- ou a imagem sem HUD. Com a nossa gerando no lugar da
// do jogo, ela vem para o nosso swapchain: a interpolacao parte da imagem sem UI e a UI entra parada em cada quadro, real
// e gerado (o swapchain do jogo, por fora do nosso, compunha a UI antes: ela era interpolada com a cena)
struct UiDoJogo
{
    ID3D12Resource* textura = nullptr; // FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12
    uint32_t texturaEstado = 0;        // FFX_API_RESOURCE_STATE_* (os mesmos valores do SDK)
    uint32_t texturaFlags = 0;         // FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_*
    void* callback = nullptr;          // ffxConfigureDescFrameGeneration.presentCallback (FfxApiPresentCallbackFunc)
    void* callbackCtx = nullptr;
    ID3D12Resource* semHud = nullptr;  // ffxConfigureDescFrameGeneration.HUDLessColor
    uint32_t semHudEstado = 0;
    uint64_t frameId = 0;              // o frameID do jogo (o callback dele pode escolher os recursos por ele)
    bool Tem() const { return textura || callback || semHud; }
};

class Geracao
{
  public:
    Geracao();
    ~Geracao();
    // o swapchain do jogo vira o da AMD: devolve em `saida` o swapchain que o jogo usa. Falha = o jogo segue com o dele
    HRESULT CriaSwapchain(IDXGIFactory* fabrica, ID3D12CommandQueue* fila, HWND janela, const DXGI_SWAP_CHAIN_DESC1* d,
                          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* tela, IDXGISwapChain1** saida);
    HRESULT CriaSwapchain(IDXGIFactory* fabrica, ID3D12CommandQueue* fila, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** saida);
    // o swapchain da AMD e so do jogo (o modulo nao o segura): quando o jogo o solta (troca de modo de tela, HDR, saida),
    // a AMD avisa antes de destruir e o contexto sai junto -- preso, ele ocupava a janela (a criacao do novo falhava) e
    // na saida do processo as threads da AMD morriam com ele vivo (o driver caia em amdxc64.dll)
    bool TemSwapchain() const;
    // a cada quadro, na lista do jogo (antes do Present): prepara a interpolacao com a profundidade e o movimento deste
    // quadro e configura o swapchain. n = quadros apresentados por quadro do jogo (2..4: n - 1 gerados entre ele e o
    // anterior, em t = 1/n, 2/n...); 0/1 = desligada, o swapchain so apresenta os quadros do jogo. Muda a qualquer quadro.
    // A tela nunca recebe mais quadros por segundo que a taxa do monitor (acima dela cada atualizacao mostra pedacos de
    // varios quadros: rasgo + fantasma): o N em uso (EmUso) cai ate caber
    bool Quadro(ID3D12GraphicsCommandList* c, const QuadroGeracao& q, uint32_t n);
    // a cada configuracao da geracao FSR 3 do jogo (depois do despacho do FSR do quadro): a UI dele. nossa = a nossa gera
    // no lugar da dele (o modulo tira a UI do swapchain do jogo); sem isso a UI volta a ser do jogo
    void UiJogo(const UiDoJogo& u, bool nossa);
    bool UiDoJogoEmUso() const; // a UI vem do jogo: a procura da imagem sem HUD fica parada
    // a fila do jogo (a do swapchain de geracao dele) e o Present dele: a imagem sem HUD que ele entrega e copiada ali,
    // na fila do jogo, depois do quadro inteiro e antes do seguinte (a interpolacao le a copia: a do mesmo quadro)
    void FilaDoJogo(ID3D12CommandQueue* q);
    void AntesDoPresent();
    bool RelatoUi(char* b, size_t n); // o log de 5 s da UI do jogo (false sem ela)
    void CopiasSemHud(uint64_t& feitas, uint64_t& usadas) const; // desde o inicio (o log de 5 s mostra a diferenca)
    // um quadro gerado em `pasta` (os gerados, o real, a imagem sem HUD e os quadros como foram para a tela): a gravacao
    // do overlay pede, para ver o HUD dos gerados como sai no jogo
    void DespejaEm(const char* pasta);
    // ExecuteCommandLists do jogo (gancho do modulo): a lista com o preparo de um quadro foi para a GPU. A apresentacao
    // usa o preparo mais novo ja enviado -- o motor pode gravar o quadro seguinte antes do Present deste, mas so o envia
    // depois. Sem esta informacao (jogo de teste direto), a ordem de gravacao
    void Submetido(ID3D12CommandList* const* l, UINT n);
    // a imagem sem HUD do quadro preparado por ultimo: o modulo numera os passes do jogo que escrevem uma textura do
    // tamanho e formato da tela depois do upscaler (jogo\SemHud.h) e a geracao copia o escolhido aqui, na lista do jogo.
    // Cada copia e conferida na apresentacao contra o quadro final (a certa so difere dele no HUD): a escolha corre os
    // passes e fica com o que confere, e so a conferida vai para a AMD como currentBackBuffer_HUDLess (e para o fluxo
    // optico, como no FSR 3 da AMD) -- o HUD fica parado nos gerados em vez de andar com a cena
    bool SemHud(ID3D12GraphicsCommandList* c, ID3D12Resource* fonte, D3D12_RESOURCE_STATES estado, uint32_t passe);
    // a escolha: o passe em uso ou em teste, quantos o jogo tem, a fracao da tela igual ao quadro final e a da mascara
    // do que difere que trocou com a cena andando (nas ultimas conferencias) e a fase (0 procurando, 1 em uso conferida
    // com a cena andando, 2 nenhum passe confere: espera e procura de novo, 3 em uso so pela igualdade: a cena ainda nao
    // andou)
    void SemHudEstado(uint32_t& passe, uint32_t& passes, double& igual, double& troca, int& fase) const;
    const char* UltimoErro() const;
    uint64_t Gerados() const; // quadros gerados ate aqui (cada chamada da AMD gera um)
    uint64_t Encontros() const; // vezes em que o preparo (render) e a geracao (Present) quiseram o backend da AMD juntos
    // o contexto da AMD (compilado em segundo plano a partir do primeiro quadro): 0 nada, 1 compilando, 2 pronto,
    // 3 falhou; e quanto a ultima compilacao levou
    int EstadoContexto() const;
    double MsCompilacao() const;
    // o ritmo das apresentacoes do swapchain da AMD desde a ultima leitura: intervalo medio, desvio, maior e o que so 1%
    // das apresentacoes passa (p99: 1000 / p99Ms = o "1% low") em ms
    uint32_t Ritmo(double& mediaMs, double& desvioMs, double& maxMs, double& p99Ms) const;
    // a taxa do monitor, medida FORA da thread de render (consultar o monitor disputa trava com o DirectX: engasgo e ate
    // bloqueio). Sem ela, sem limite
    void DefineTaxa(uint32_t hz, uint32_t hzMax);
    uint32_t EmUso() const;    // quadros apresentados por quadro do jogo de fato (1 = sem geracao)
    uint32_t TrocasN() const;  // quantas vezes o N em uso mudou desde a ultima leitura (zera)
    // desde a ultima leitura: apresentacoes com geracao, quantos quadros o jogo ja tinha preparado a frente de cada uma
    // (media e maior) e quantas ficaram sem o preparo do proprio quadro
    uint32_t Fila(double& mediaAFrente, uint64_t& maxAFrente, uint64_t& semPreparo) const;
    // diagnostico: as 120 primeiras apresentacoes com geracao (de cada swapchain), uma linha cada -- quando foi, quantos
    // despachos do FSR desde a anterior, a fila (quadro:ordem de envio) e qual quadro ela usou. Devolve as linhas novas
    // desde a ultima leitura (vazio quando nao ha)
    std::string Rastro();
    uint32_t Hz() const;       // a taxa atual do monitor da janela do jogo (0 = desconhecida)
    uint32_t HzMaximo() const; // a maior que ele aceita nesta resolucao
    double FpsJogo() const;    // quadros do jogo por segundo (entre as chamadas de Quadro)

  private:
    struct Impl;
    Impl* p;
};

} // namespace dlss5ov
