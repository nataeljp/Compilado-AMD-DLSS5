#pragma once
// [lmxxf2] Botão "R" no canto de cada cartão do menu: volta ao padrão tudo o que o cartão mexe. A tabela cartão ->
// membros do Config é gerada (tools/optiscaler_fork/gera_reset_cartoes.py) a partir do próprio código do menu.

namespace Lmxxf2Reset
{
// quantos membros o cartão volta ao padrão; -1 = cartão sem botão
int Chaves(const char* cartao);
// volta o cartão ao padrão; false = cartão sem botão
bool Restaura(const char* cartao);
} // namespace Lmxxf2Reset
