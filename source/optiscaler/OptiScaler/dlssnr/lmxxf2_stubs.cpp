// [lmxxf2] Substitutos das partes do AMDNR que este fork não traz:
//  - registro de sessão do Logger/dllmain do AMDNR (não publicado): sem cabeçalho próprio, o AmdBridge escreve o seu;
//  (o AmdnrGi - Screen GI do AMDNR - agora é o real: dlssnr/gi/GiSeam.cpp)
#include "pch.h"
#include <string>

struct NVSDK_NGX_Parameter;
struct ID3D12Resource;

std::string SessionHeaderText() { return {}; }
void NoteSessionExitHook(long) {}
void NoteSessionExitDone(const char*, unsigned long long) {}


