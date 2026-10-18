// Janela overlay: sem moldura, sempre no topo, transparente a cliques e sem foco (o jogo continua recebendo
// mouse e teclado), apresentada por DirectComposition e fora da captura de tela.
#pragma once
#include "Comum.h"
#include <dcomp.h>

namespace ov {

class Overlay
{
  public:
    bool Cria(Gpu& g, HINSTANCE inst);
    void Destroi();
    // cobre o retangulo (pixels de tela); recria a swapchain quando o tamanho muda
    void Posiciona(const RECT& r);
    void Mostra(bool sim);
    bool Visivel() const { return visivel; }
    HWND Janela() const { return hwnd; }

    // buffer atual da swapchain (B8G8R8A8, estado PRESENT) e apresentacao
    ID3D12Resource* Buffer();
    UINT Largura() const { return w; }
    UINT Altura() const { return h; }
    void Apresenta();

  private:
    void CriaSwapchain(UINT w_, UINT h_);
    Gpu* gpu = nullptr;
    HWND hwnd = nullptr;
    ComPtr<IDXGISwapChain3> sc;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> alvo;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<ID3D12Resource> buffers[2];
    UINT w = 0, h = 0;
    RECT ultimo{};
    bool visivel = false;
};

} // namespace ov
