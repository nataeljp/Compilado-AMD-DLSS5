#pragma once
// [lmxxf2] Modo seguro que aprende com queda. Cada sessão grava lmxxf2_sessao.json ao lado do OptiScaler: "rodando" na
// carga, "estavel" depois de alguns minutos de jogo e "limpa" na saída normal (DLL_PROCESS_DETACH). Se a sessão
// anterior ficou em "rodando" (o jogo caiu ou travou a GPU antes de ficar estável: carregamento, compilação de shaders),
// esta desliga UM recurso que estava ligado na sessão que caiu: a rede DLSS5 se a queda veio até 30 s depois de ela
// ligar (batimento a cada 5 s dá a hora da queda); senão o primeiro ainda ligado de spoof NVIDIA (em auto), FG
// automático, rede. Spoof e FG fixados à mão no ini não são tocados; o ini nunca é regravado. O que foi desligado
// fica (sessão estável sem ele = ele era a causa) até a UI pedir "Voltar ao normal". [DlssNr] ModoSeguro=false desliga.
#include <string>

namespace Lmxxf2Seguro
{
// dllmain, antes do FG automático: lê a sessão anterior, decide o nível, grava "rodando" e agenda o "estavel"
void Inicia();
// DLL_PROCESS_DETACH: saída normal
void Saida();
// o que esta sessão ligou (o que pode ser culpado se ela cair)
void NotaSpoof(bool ligado);
void NotaFgAuto(bool aplicado);
void NotaRede();
// a janela do jogo está fechando (WM_CLOSE / WM_DESTROY): saída pedida, antes da limpeza do jogo, que pode cair sozinha
// (ex.: XeFG soltando o swapchain); uma queda depois disso não conta
void Saindo();
// quantos recursos estão desligados pelo modo seguro (0 = normal)
int Nivel();
std::string Motivo();
bool SemSpoof();
bool SemFgAuto();
bool SemRede();
// UI: nível 0 na próxima vez que o jogo abrir
void Normal();
} // namespace Lmxxf2Seguro
