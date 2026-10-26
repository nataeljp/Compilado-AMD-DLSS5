// Geracao de quadros da AMD dentro do jogo (captura de buffer): FidelityFX Frame Interpolation + Optical Flow + o
// swapchain de geracao do FidelityFX SDK 1.1.4, compilados do codigo fonte (MIT), ligados como o provedor de referencia
// da AMD (ffx-api/src/ffx_provider_framegeneration.cpp). O swapchain do jogo e trocado pelo da AMD NA CRIACAO (por isso
// o jogo abre pelo overlay, com o modulo dentro antes do D3D12); a cada quadro o jogo entrega a profundidade, o movimento
// e a camera que ja manda ao FSR. A interpolacao parte do backbuffer final (a saida do upscaler nao serve de imagem sem
// HUD: e a cena antes do tonemap e dos pos-efeitos, outro espaco de cor) -- os quadros gerados levam a edicao da rede,
// que ja esta no quadro do jogo.
#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>

namespace dlss5ov {

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

class Geracao
{
  public:
    Geracao();
    ~Geracao();
    // o swapchain do jogo vira o da AMD: devolve em `saida` o swapchain que o jogo usa. Falha = o jogo segue com o dele
    HRESULT CriaSwapchain(IDXGIFactory* fabrica, ID3D12CommandQueue* fila, HWND janela, const DXGI_SWAP_CHAIN_DESC1* d,
                          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* tela, IDXGISwapChain1** saida);
    HRESULT CriaSwapchain(IDXGIFactory* fabrica, ID3D12CommandQueue* fila, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** saida);
    bool TemSwapchain() const;
    // a cada quadro, na lista do jogo (antes do Present): prepara a interpolacao com a profundidade e o movimento deste
    // quadro e configura o swapchain. ligada = false: o swapchain so apresenta os quadros do jogo
    bool Quadro(ID3D12GraphicsCommandList* c, const QuadroGeracao& q, bool ligada);
    const char* UltimoErro() const;
    uint64_t Gerados() const; // quadros gerados ate aqui (cada chamada da AMD gera um)
    // o contexto da AMD (compilado em segundo plano a partir do primeiro quadro): 0 nada, 1 compilando, 2 pronto,
    // 3 falhou; e quanto a ultima compilacao levou
    int EstadoContexto() const;
    double MsCompilacao() const;

  private:
    struct Impl;
    Impl* p;
};

} // namespace dlss5ov
