// Mouse do painel sem tirar o foco do jogo. Muitos jogos (Gears of War: E-Day e outros em Unreal) pausam no menu
// quando perdem o foco, e entao nao da para ver a rede mudando enquanto se mexe no painel. Com o painel aberto sobre o
// jogo em primeiro plano, um gancho de mouse de baixo nivel (WH_MOUSE_LL, numa thread propria) fica com o mouse: o
// jogo nao recebe movimento nem clique (a mira nao anda), o foco continua nele e o painel desenha o proprio cursor,
// preso na area do painel. Esc (engolida tambem) ou Ctrl+Shift+P devolvem o mouse ao jogo.
// Seguranca: sem batimento do laco principal por 1 s (laco travado), o gancho deixa tudo passar.
#pragma once
#include <windows.h>
#include <atomic>
#include <thread>
#include <vector>

namespace ov {

// tecla segurada pelo gancho enquanto o painel recebe texto (CapturaTeclado): o jogo continua em foco e nao a ve
struct EventoTecla
{
    UINT vk = 0;
    bool desce = false;
    wchar_t ch[4] = {}; // o caractere que a tecla produz no layout do jogo (vazio: tecla de controle)
    int nch = 0;
};

struct EventoMouse
{
    enum Tipo { Move, Botao, Roda } tipo = Move;
    LONG x = 0, y = 0;    // cursor virtual (tela), em Move
    int botao = 0;        // 0 esquerdo, 1 direito, 2 meio, 3/4 laterais
    bool desce = false;
    float roda = 0, rodaH = 0; // em entalhes
};

class Gancho
{
  public:
    ~Gancho() { Desliga(); }
    // `area`: retangulo da tela onde o cursor virtual anda (o painel); ele comeca no centro
    void Liga(const RECT& area);
    void Desliga();
    bool Ligado() const { return t.joinable(); }
    void Area(const RECT& area);
    // eventos desde a ultima chamada (vale como batimento); `esc` = Esc apertada desde a ultima chamada
    std::vector<EventoMouse> Coleta(POINT& cursor, bool& esc);
    // false: o MOVIMENTO passa para o jogo (o modulo dentro dele pega o movimento bruto e zera o que o jogo ve);
    // cliques e roda continuam presos aqui
    void EngoleMouse(bool sim);
    // movimento bruto (do modulo do jogo): anda o cursor virtual e devolve a posicao
    POINT MoveBruto(int dx, int dy);
    // com o painel digitando (um campo de texto ativo): todas as teclas ficam com o painel, inclusive o Esc (cancela o
    // campo em vez de fechar o painel); o jogo nao recebe nenhuma
    void CapturaTeclado(bool sim);
    std::vector<EventoTecla> ColetaTeclas();

  private:
    std::thread t;
    std::atomic<DWORD> idThread{0};
};

} // namespace ov
