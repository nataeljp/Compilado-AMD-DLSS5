// Painel de controle (janela normal, Dear ImGui): escolhe a janela do jogo e ajusta os parametros ao vivo.
#pragma once
#include "Comum.h"
#include "Config.h"

namespace ov {

struct EstadoPainel
{
    std::wstring placa;
    std::wstring alvo; // titulo da janela capturada ("" = nenhuma)
    bool redeOk = false;
    UINT janelaW = 0, janelaH = 0, redeW = 0, redeH = 0;
    double msMedio = 0, fps = 0;
    double fracaoRede = 1; // fracao dos quadros em que a rede rodou
};

struct AcoesPainel
{
    HWND ligar = nullptr; // captura esta janela
    bool desligar = false;
    bool sair = false;
    bool salvar = false;
    bool zeraHistorico = false;
    bool ocultaMudou = false;
};

class Painel
{
  public:
    bool Cria(Gpu& g, HINSTANCE inst, const std::wstring& pastaIni);
    void Destroi();
    // desenha e apresenta; devolve o que o usuario pediu
    AcoesPainel Quadro(Config& cfg, const EstadoPainel& e);
    HWND Janela() const { return hwnd; }
    void Alterna(); // mostra/esconde (Ctrl+Alt+P)

  private:
    void Redimensiona(UINT w, UINT h);
    Gpu* gpu = nullptr;
    HWND hwnd = nullptr;
    ComPtr<IDXGISwapChain3> sc;
    ComPtr<ID3D12DescriptorHeap> rtvHeap, srvHeap;
    ComPtr<ID3D12Resource> bb[2];
    ComPtr<ID3D12CommandAllocator> al;
    ComPtr<ID3D12GraphicsCommandList> cl;
    UINT rtvPasso = 0, w = 0, h = 0;
    std::string iniUtf8;
    struct JanelaLista
    {
        HWND h;
        std::string nome;
    };
    std::vector<JanelaLista> janelas;
    int escolhida = -1;
    void AtualizaJanelas();
};

} // namespace ov
