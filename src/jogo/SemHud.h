// A imagem sem HUD para a geracao de quadros, pelo metodo do HUDfix do OptiScaler (hudfix/Hudfix_Dx12.cpp e
// resource_tracking/ResTrack_dx12.cpp): depois do despacho do upscaler, os passes do jogo que escrevem -- alvo de desenho
// (RTV) ou UAV de compute -- uma textura do tamanho e da familia de formato do backbuffer sao os que montam a cena final
// (tonemap / pos-efeitos) e o HUD. Cada um e numerado (0, 1, ...: um por ligacao do alvo, nao por desenho) e a geracao
// copia o escolhido logo depois dele; a escolha e conferida contra o quadro final (Geracao::SemHud): a imagem certa so
// difere dele no HUD. A AMD recebe a copia (currentBackBuffer_HUDLess): onde o quadro final difere dela e HUD, e o
// gerado leva o HUD do quadro final, parado (ffx_frameinterpolation_inpainting.h). Sem ela o HUD anda com a cena.
#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstddef>
#include <cstdint>

namespace semhud {

// o passe candidato `indice` do quadro acabou de escrever `fonte` (no estado `estado`, na lista `c`): true = copiada
// como a imagem sem HUD do quadro
using FnCaptura = bool (*)(ID3D12GraphicsCommandList* c, ID3D12Resource* fonte, D3D12_RESOURCE_STATES estado,
                           uint32_t indice);

// os ganchos (uma vez: dispositivo, listas e o swapchain da geracao) e o alvo (tamanho e formato do backbuffer, que
// acompanham ResizeBuffers). Devolve o que deu errado (nullptr = ok)
const char* Instala(ID3D12Device* d, IDXGISwapChain* sc, FnCaptura captura);
void Abre();  // fim do despacho do upscaler deste quadro (e do que o modulo grava nele): os passes seguintes valem
void Fecha(); // Present do jogo (ou o proximo despacho do upscaler)

// gravacao do proprio modulo ou da AMD nesta thread: os ganchos nao olham
struct Proprio
{
    Proprio();
    ~Proprio();
};

// desde a ultima leitura: capturas e de onde veio a ultima (texto para o log; vazio sem capturas)
uint32_t Relato(char* texto, size_t n);

// diagnostico: chamada quando o runtime recusa fechar uma lista (um comando invalido foi gravado nela; enviada, ela
// derruba o dispositivo -- o TLOU2 entao para com DXGI_ERROR_INVALID_CALL). O gancho em Close vem com os das listas
void DefineRecusa(void (*recusa)(ID3D12GraphicsCommandList* l, HRESULT r));

} // namespace semhud
