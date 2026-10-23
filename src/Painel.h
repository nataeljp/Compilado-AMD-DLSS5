// Painel de controle (Dear ImGui) no estilo do "DLSS 5 Developer Controls" da NVIDIA: encostado na direita do monitor
// do jogo, sempre acima do overlay, e (como a Game Bar) so aparece com o jogo em foco.
#pragma once
#include "Comum.h"
#include "Config.h"
#include "Gancho.h"
#include "buffer/Protocolo.h"

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
    int fonte = 0;               // Modo de Rede: 0 captura de video, 1 captura de buffer
    std::string estadoBuffer;    // estado da captura de buffer
    // a rede da sessao (o que vale ate religar) e a memoria de video do overlay
    int alturaSessao = 0;
    bool pequenaSessao = false;
    int passesSessao = 1;
    bool completaSessao = true;
    std::string memoria;         // uso e orcamento de VRAM do overlay
    std::string avisoMemoria;    // a rede foi desligada por falta de VRAM ("" = nao)
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
    // Ctrl+Shift+P: com o jogo em primeiro plano, abre SEM tirar o foco dele (o jogo nao pausa no menu e a rede
    // continua visivel): o mouse fica com o painel (Gancho) ate Esc ou Ctrl+Shift+P. Fora do jogo (bandeja, area de
    // trabalho) abre com foco, como antes. Aberto -> fecha e devolve o mouse ao jogo.
    void Alterna();
    void Esconde(bool devolveFoco = true);
    void DefineJogo(HWND j) { jogo = j; }
    void DefineAutoOculto(bool sim);  // some enquanto o jogo esta fora de foco (Alt+Tab, minimizado)
    // captura de buffer: o modulo dentro do jogo entrega o mouse ao painel (o jogo le entrada bruta e o cursor do
    // sistema nem anda); nullptr = sem modulo
    void DefineMouseJogo(dlss5ov::Cabecalho* c);
    void Ancora(HWND alvo);           // encosta na direita do monitor de `alvo` (ou do principal)
    // icone na bandeja do sistema: clique esquerdo mostra/esconde o painel; direito abre o menu (Sair fecha o app)
    void DicaBandeja(const std::wstring& dica);

  private:
    void Redimensiona(UINT w, UINT h);
    void Aplica();
    bool JogoNaFrente() const;
    Gancho gancho;
    dlss5ov::Cabecalho* mouseJogo = nullptr;
    uint32_t desceVisto[3] = {}, sobeVisto[3] = {};
    ULONGLONG ultimoBruto = 0; // ultima vez que veio movimento bruto do jogo (com ele, o do cursor e ignorado)
    // o jogo nao le o mouse por entrada bruta (GameInput?): o modulo nao ve nada; o gancho do Windows volta a segurar
    uint32_t eventosAoAbrir = 0;
    int movesSemJogo = 0;
    bool jogoSemBruta = false;
    long brutoDx = 0, brutoDy = 0;   // diagnostico: movimento recebido do jogo nesta abertura
    ULONGLONG vigiaFocoAte = 0;      // fechado sobre o jogo: devolve o foco se o Windows ativar outra janela logo depois
    bool semFoco = false; // aberto sobre o jogo sem tirar o foco dele (mouse pelo gancho)
    void CriaBandeja();
    std::wstring dicaBandeja;
    bool avisouBandeja = false;
    Gpu* gpu = nullptr;
    HWND hwnd = nullptr;
    HWND jogo = nullptr;
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
