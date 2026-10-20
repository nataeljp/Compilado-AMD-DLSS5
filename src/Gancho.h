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

  private:
    std::thread t;
    std::atomic<DWORD> idThread{0};
};

} // namespace ov
