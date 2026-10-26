; A DLSS5Buffer.dll instalada na pasta do jogo como dxgi.dll (o jogo carrega a DLL sozinho em toda abertura): as
; funcoes da dxgi.dll do Windows que o modulo nao trata vao direto para ela. Cada uma salta pela tabela g_dxgiFn,
; preenchida por ResolveDxgiSistema (Modulo.cpp) na primeira chamada -- os registradores de argumento (inteiros e
; ponto flutuante) sao guardados em volta dela. Sem a funcao no Windows desta maquina: E_NOTIMPL.
; CreateDXGIFactory, CreateDXGIFactory1 e CreateDXGIFactory2 ficam em C (o modulo engancha a fabrica criada).

EXTERN ResolveDxgiSistema:PROC

.data
PUBLIC g_dxgiFn
g_dxgiFn QWORD 17 DUP(0)

.code

DxgiFalha PROC
    mov eax, 80004001h ; E_NOTIMPL
    ret
DxgiFalha ENDP

REPASSA MACRO nome, idx
nome PROC
    mov rax, qword ptr [g_dxgiFn + idx * 8]
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
    call ResolveDxgiSistema
    mov rcx, [rsp + 20h]
    mov rdx, [rsp + 28h]
    mov r8, [rsp + 30h]
    mov r9, [rsp + 38h]
    movdqu xmm0, xmmword ptr [rsp + 40h]
    movdqu xmm1, xmmword ptr [rsp + 50h]
    movdqu xmm2, xmmword ptr [rsp + 60h]
    movdqu xmm3, xmmword ptr [rsp + 70h]
    add rsp, 88h
    mov rax, qword ptr [g_dxgiFn + idx * 8]
    test rax, rax
    jz DxgiFalha
    jmp rax
nome ENDP
ENDM

; a ordem e a de kDxgiNomes em Modulo.cpp
REPASSA PxApplyCompatResolutionQuirking, 0
REPASSA PxCompatString, 1
REPASSA PxCompatValue, 2
REPASSA PxDXGID3D10CreateDevice, 3
REPASSA PxDXGID3D10CreateLayeredDevice, 4
REPASSA PxDXGID3D10GetLayeredDeviceSize, 5
REPASSA PxDXGID3D10RegisterLayers, 6
REPASSA PxDXGIDeclareAdapterRemovalSupport, 7
REPASSA PxDXGIDisableVBlankVirtualization, 8
REPASSA PxDXGIDumpJournal, 9
REPASSA PxDXGIGetDebugInterface1, 10
REPASSA PxDXGIReportAdapterConfiguration, 11
REPASSA PxPIXBeginCapture, 12
REPASSA PxPIXEndCapture, 13
REPASSA PxPIXGetCaptureState, 14
REPASSA PxSetAppCompatStringPointer, 15
REPASSA PxUpdateHMDEmulationStatus, 16

END
