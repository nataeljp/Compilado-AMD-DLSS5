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
    bool vulkan = false;  // recusado porque o jogo so usa Vulkan: quem le esse jogo e a camada, nao a injecao
};

// qualquerDx: a captura da API, que tambem le D3D11 (a de buffer exige D3D12)
ResultadoInjecao InjetaBuffer(DWORD pid, const std::wstring& dll, bool qualquerDx = false);

// o modulo esta no processo (injetado)
bool ModuloPresente(DWORD pid);
// um processo (menos `exceto`) com o modulo dentro e a memoria compartilhada criada (o FSR/XeSS ja rodou); 0 = nenhum
DWORD ProcessoComModulo(DWORD exceto);
// caminho completo do executavel do processo ("" sem acesso)
std::wstring ExeDoProcesso(DWORD pid);
// um processo (menos `exceto`) rodando este executavel; 0 = nenhum
DWORD ProcessoDoExe(const std::wstring& exe, DWORD exceto);
// A geracao de quadros instalava a DLL na pasta do jogo (version.dll ou dxgi.dll, DLSS5Overlay.alvo, copias .antiga.*):
// tira o que for nosso (nunca a DLL de outro programa). Para a Lixeira; a DLL em uso pelo jogo aberto e renomeada para nao
// carregar na proxima abertura e vai para a Lixeira na proxima conexao. Devolve o que fez ("" = nao havia nada)
std::string LimpaPastaDoJogo(const std::wstring& exe);

} // namespace ov
