// Captura de buffer: coloca a DLSS5Buffer.dll no jogo ja aberto (LoadLibraryW numa thread remota, como os injetores de
// camera do tipo UUU/IGCS). Nada e gravado na pasta do jogo. Antes, recusa jogo com anti-cheat (injetar codigo nele pode
// dar banimento) e jogo que nao e DirectX 12.
#pragma once
#include "Comum.h"

namespace ov {

struct ResultadoInjecao
{
    bool ok = false;
    std::string motivo;   // por que nao (ou um aviso, com ok = true)
    std::string upscaler; // o upscaler que o jogo ja carregou ("" = nenhum ainda)
};

ResultadoInjecao InjetaBuffer(DWORD pid, const std::wstring& dll);

} // namespace ov
