// config.json ao lado do exe: objeto plano de numeros/booleanos (lido na inicializacao e regravado ao sair)
#pragma once
#include "Rede.h"
#include <map>

namespace ov {

struct Config
{
    ParamsRede rede;
    int alturaRede = 1080; // altura em que a rede roda (720..2160); a largura segue a proporcao da janela
    float forca = 1.f;     // quanto da edicao da rede entra no quadro (0..2)
    int modo = 0;          // 0 rede, 1 original, 2 comparacao, 3 so a edicao, 4 movimento
    bool movimento = true; // movimento simulado por fluxo optico (historico temporal da rede)
    float divisao = 0.5f;  // posicao da divisao no modo comparacao
    int fpsMax = 60;       // teto de quadros processados por segundo (0 = sem teto)

    void Carrega(const std::wstring& arquivo);
    void Salva(const std::wstring& arquivo) const;
};

} // namespace ov
