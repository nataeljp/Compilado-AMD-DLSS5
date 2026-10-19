// Painel de controle (Dear ImGui) no estilo do "DLSS 5 Developer Controls" da NVIDIA: encostado na direita do monitor
// do jogo, sempre acima do overlay, e (como a Game Bar) so aparece com o jogo em foco.
#pragma once
#include "Comum.h"
#include "Config.h"

struct ImFont;

namespace ov {

struct EstadoPainel
{
    std::wstring placa;
    std::wstring alvo;           // titulo da janela capturada ("" = nenhuma)
    std::string exe;             // processo do jogo (nome do perfil); "" sem jogo
    bool perfilExiste = false;   // bin\perfis\<exe>.json
    bool perfilCarregado = false;
    bool redeOk = false;
    UINT janelaW = 0, janelaH = 0, redeW = 0, redeH = 0;
    double msMedio = 0, fps = 0;
    double fracaoRede = 1; // fracao dos quadros com resposta nova da rede
};

struct AcoesPainel
{
    HWND ligar = nullptr; // captura esta janela
    bool desligar = false;
    bool sair = false;
    bool salvar = false;       // config.json (o padrao de quem nao tem perfil)
    bool salvarPerfil = false; // perfis\<exe>.json
    bool apagarPerfil = false;
    bool redefinir = false;    // padroes do programa
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
    void Alterna();                   // mostra/esconde (Ctrl+Alt+P)
    void DefineAutoOculto(bool sim);  // some enquanto o jogo esta fora de foco (Alt+Tab, minimizado)
    void Ancora(HWND alvo);           // encosta na direita do monitor de `alvo` (ou do principal)

  private:
    void Redimensiona(UINT w, UINT h);
    void Aplica();
    Gpu* gpu = nullptr;
    HWND hwnd = nullptr;
    ComPtr<IDXGISwapChain3> sc;
    ComPtr<ID3D12DescriptorHeap> rtvHeap, srvHeap;
    ComPtr<ID3D12Resource> bb[2];
    ComPtr<ID3D12CommandAllocator> al;
    ComPtr<ID3D12GraphicsCommandList> cl;
    UINT rtvPasso = 0, w = 0, h = 0;
    float esc = 1.f;
    bool usuarioQuer = true, autoOculto = false;
    bool avancado = false;
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
