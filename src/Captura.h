// Captura de uma janela por Windows Graphics Capture: nada e injetado no jogo (serve para qualquer jogo, inclusive
// online). O quadro sai numa textura compartilhada D3D11 -> D3D12, ja recortada na area cliente da janela.
#pragma once
#include "Comum.h"
#include <d3d11_4.h>
#include <memory>

namespace ov {

class Captura
{
  public:
    Captura();
    ~Captura();
    // dispositivo D3D11 na mesma placa (LUID) do D3D12; cerca compartilhada com a fila D3D12
    void Cria(Gpu& g);
    bool Inicia(HWND alvo);
    void Para();
    bool Ativa() const;
    HWND Alvo() const { return alvo; }
    HANDLE EventoQuadro() const { return evento; }

    struct Quadro
    {
        ID3D12Resource* tex = nullptr; // B8G8R8A8, ja recortada (origem 0,0), w x h
        UINT w = 0, h = 0;
        UINT64 cercaValor = 0; // a fila D3D12 espera este valor na cerca compartilhada antes de ler
        bool novoTamanho = false;
    };
    // pega o quadro mais novo (se houver) e copia para a textura compartilhada
    bool Pega(Quadro& q);
    ID3D12Fence* Cerca() const { return cerca12.Get(); }
    // retangulo da area cliente do alvo em pixels de tela
    static bool RetanguloCliente(HWND h, RECT& r);

  private:
    struct Impl;
    std::unique_ptr<Impl> p;
    HWND alvo = nullptr;
    HANDLE evento = nullptr;
    Gpu* gpu = nullptr;
    ComPtr<ID3D11Device5> dev11;
    ComPtr<ID3D11DeviceContext4> ctx11;
    ComPtr<ID3D12Fence> cerca12;
    ComPtr<ID3D11Fence> cerca11;
    UINT64 valor = 0;
    // textura compartilhada (area cliente)
    ComPtr<ID3D11Texture2D> comp11;
    ComPtr<ID3D12Resource> comp12;
    UINT cw = 0, ch = 0;
    bool RecriaCompartilhada(UINT w, UINT h);
};

} // namespace ov
