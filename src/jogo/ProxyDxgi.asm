; A DLSS5Buffer.dll instalada na pasta do jogo como version.dll ou dxgi.dll (o jogo carrega a DLL sozinho em toda
; abertura, antes do DirectX): as funcoes da DLL do Windows de mesmo nome vao direto para ela. Cada uma salta pela sua
; tabela (g_verFn / g_dxgiFn), preenchida pelo resolvedor (ResolveVersionSistema / ResolveDxgiSistema, Modulo.cpp) na
; primeira chamada -- os registradores de argumento (inteiros e ponto flutuante) sao guardados em volta dele. Sem a funcao
; no Windows desta maquina: E_NOTIMPL. CreateDXGIFactory, CreateDXGIFactory1 e CreateDXGIFactory2 ficam em C (o modulo
; engancha a fabrica criada).

EXTERN ResolveDxgiSistema:PROC
EXTERN ResolveVersionSistema:PROC

.data
PUBLIC g_dxgiFn
PUBLIC g_verFn
g_dxgiFn QWORD 17 DUP(0)
g_verFn QWORD 15 DUP(0)

.code

ProxyFalha PROC
    mov eax, 80004001h ; E_NOTIMPL
    ret
ProxyFalha ENDP

REPASSA MACRO nome, tabela, idx, resolve
nome PROC
    mov rax, qword ptr [tabela + idx * 8]
    test rax, rax
    jz @F
    jmp rax
@@:
    sub rsp, 88h
    mov [rsp + 20h], rcx
    mov [rsp + 28h], rdx
    mov [rsp + 30h], r8
    mov [rsp + 38h], r9
    movdqu xmmword ptr [rsp + 40h], xmm0
    movdqu xmmword ptr [rsp + 50h], xmm1
    movdqu xmmword ptr [rsp + 60h], xmm2
    movdqu xmmword ptr [rsp + 70h], xmm3
    call resolve
    mov rcx, [rsp + 20h]
    mov rdx, [rsp + 28h]
    mov r8, [rsp + 30h]
    mov r9, [rsp + 38h]
    movdqu xmm0, xmmword ptr [rsp + 40h]
    movdqu xmm1, xmmword ptr [rsp + 50h]
    movdqu xmm2, xmmword ptr [rsp + 60h]
    movdqu xmm3, xmmword ptr [rsp + 70h]
    add rsp, 88h
    mov rax, qword ptr [tabela + idx * 8]
    test rax, rax
    jz ProxyFalha
    jmp rax
nome ENDP
ENDM

; a ordem e a de kDxgiNomes em Modulo.cpp
REPASSA PxApplyCompatResolutionQuirking, g_dxgiFn, 0, ResolveDxgiSistema
REPASSA PxCompatString, g_dxgiFn, 1, ResolveDxgiSistema
REPASSA PxCompatValue, g_dxgiFn, 2, ResolveDxgiSistema
REPASSA PxDXGID3D10CreateDevice, g_dxgiFn, 3, ResolveDxgiSistema
REPASSA PxDXGID3D10CreateLayeredDevice, g_dxgiFn, 4, ResolveDxgiSistema
REPASSA PxDXGID3D10GetLayeredDeviceSize, g_dxgiFn, 5, ResolveDxgiSistema
REPASSA PxDXGID3D10RegisterLayers, g_dxgiFn, 6, ResolveDxgiSistema
REPASSA PxDXGIDeclareAdapterRemovalSupport, g_dxgiFn, 7, ResolveDxgiSistema
REPASSA PxDXGIDisableVBlankVirtualization, g_dxgiFn, 8, ResolveDxgiSistema
REPASSA PxDXGIDumpJournal, g_dxgiFn, 9, ResolveDxgiSistema
REPASSA PxDXGIGetDebugInterface1, g_dxgiFn, 10, ResolveDxgiSistema
REPASSA PxDXGIReportAdapterConfiguration, g_dxgiFn, 11, ResolveDxgiSistema
REPASSA PxPIXBeginCapture, g_dxgiFn, 12, ResolveDxgiSistema
REPASSA PxPIXEndCapture, g_dxgiFn, 13, ResolveDxgiSistema
REPASSA PxPIXGetCaptureState, g_dxgiFn, 14, ResolveDxgiSistema
REPASSA PxSetAppCompatStringPointer, g_dxgiFn, 15, ResolveDxgiSistema
REPASSA PxUpdateHMDEmulationStatus, g_dxgiFn, 16, ResolveDxgiSistema

; a ordem e a de kVerNomes em Modulo.cpp
REPASSA PvGetFileVersionInfoA, g_verFn, 0, ResolveVersionSistema
REPASSA PvGetFileVersionInfoByHandle, g_verFn, 1, ResolveVersionSistema
REPASSA PvGetFileVersionInfoExA, g_verFn, 2, ResolveVersionSistema
REPASSA PvGetFileVersionInfoExW, g_verFn, 3, ResolveVersionSistema
REPASSA PvGetFileVersionInfoSizeA, g_verFn, 4, ResolveVersionSistema
REPASSA PvGetFileVersionInfoSizeExA, g_verFn, 5, ResolveVersionSistema
REPASSA PvGetFileVersionInfoSizeExW, g_verFn, 6, ResolveVersionSistema
REPASSA PvGetFileVersionInfoSizeW, g_verFn, 7, ResolveVersionSistema
REPASSA PvGetFileVersionInfoW, g_verFn, 8, ResolveVersionSistema
REPASSA PvVerFindFileA, g_verFn, 9, ResolveVersionSistema
REPASSA PvVerFindFileW, g_verFn, 10, ResolveVersionSistema
REPASSA PvVerInstallFileA, g_verFn, 11, ResolveVersionSistema
REPASSA PvVerInstallFileW, g_verFn, 12, ResolveVersionSistema
REPASSA PvVerQueryValueA, g_verFn, 13, ResolveVersionSistema
REPASSA PvVerQueryValueW, g_verFn, 14, ResolveVersionSistema

END
