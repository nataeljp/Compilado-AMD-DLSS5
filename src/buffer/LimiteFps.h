// Teto de FPS do jogo (ProtocoloApi.h), no Present da DXGI e no vkQueuePresentKHR: depois do present, antes do jogo comecar o quadro
// seguinte. O tempo de GPU que o jogo deixa de usar fica para a rede; esperar ali faz o jogo ler a entrada mais tarde, sem somar
// atraso. Sem batimento do overlay por 2 s nao limita (overlay fechado ou travado nao prende o jogo).
#pragma once
#include "ProtocoloApi.h"
#include <windows.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace dlss5ov {

class LimiteFps
{
  public:
    void Espera(const CabApi* cab)
    {
        const uint32_t fps = cab ? cab->limiteFps : 0;
        if (!fps || GetTickCount64() - cab->batimentoLimite >= 2000)
        {
            proximo = 0;
            return;
        }
        LARGE_INTEGER f, t;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t);
        const LONGLONG periodo = f.QuadPart / fps;
        // bem atrasado (carregamento, jogo abaixo do teto): nao corre depois para compensar
        if (!proximo || t.QuadPart - proximo > periodo)
            proximo = t.QuadPart;
        if (t.QuadPart < proximo)
        {
            if (!timer)
                timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            LARGE_INTEGER d;
            d.QuadPart = -((proximo - t.QuadPart) * 10000000 / f.QuadPart);
            if (timer && SetWaitableTimer(timer, &d, 0, nullptr, nullptr, FALSE))
                WaitForSingleObject(timer, 1000);
            else
                Sleep(DWORD((proximo - t.QuadPart) * 1000 / f.QuadPart));
        }
        proximo += periodo;
    }

  private:
    LONGLONG proximo = 0;
    HANDLE timer = nullptr;
};

} // namespace dlss5ov
