// Tela de abertura: aparece no instante em que o exe abre (o runtime da rede leva de 2 a 30 s para carregar e, sem
// ela, parecia que o programa nao tinha aberto). Logo "NR", a etapa atual e a barra do boot do Windows XP (tres blocos
// correndo), numa thread propria para animar enquanto a thread principal esta presa carregando a rede.
#pragma once
#include <windows.h>
#include <atomic>
#include <string>
#include <thread>

namespace ov {

class Abertura
{
  public:
    void Mostra(HINSTANCE inst);
    void Etapa(const wchar_t* texto); // "Carregando a rede DLSS 5..."
    void Fecha();
    ~Abertura() { Fecha(); }

  private:
    std::thread t;
    std::atomic<HWND> hwnd{nullptr};
};

} // namespace ov
