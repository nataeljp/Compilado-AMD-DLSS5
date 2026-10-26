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

// Geracao de quadros da AMD: o swapchain do jogo e trocado na criacao, entao o modulo tem de estar no jogo antes do
// D3D12. AbreJogo cria o processo SUSPENSO, publica o lancamento (Protocolo.h Lancamento: o pedido de geracao), carrega
// a DLL e so solta o jogo quando o modulo avisa que enganchou a criacao do swapchain (evento pronto, ate 5 s).
// EntraNoInicio: o mesmo num processo que acabou de nascer sem o overlay (a Steam reabre o jogo pelo cliente dela).
ResultadoInjecao AbreJogo(const std::wstring& exe, const std::wstring& dll, uint32_t geracao, DWORD& pid,
                          const std::wstring& args = L"");
ResultadoInjecao EntraNoInicio(DWORD pid, const std::wstring& dll, uint32_t geracao);
// pid de um processo com esse nome de executavel (sem diferenciar maiusculas), menos `exceto`; 0 = nenhum
DWORD ProcuraProcesso(const std::wstring& nomeExe, DWORD exceto);

} // namespace ov
