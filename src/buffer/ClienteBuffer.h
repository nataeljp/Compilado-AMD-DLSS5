// Lado do JOGO da captura de buffer (ver Protocolo.h). Independente do resto do overlay: compila dentro do host
// (OptiScaler) e no jogo de teste. Grava na lista do jogo, depois do upscale:
//   - exporta cor (x exposicao), movimento e profundidade para os slots compartilhados, quando o overlay quer;
//   - soma a ultima edicao publicada pelo overlay, reprojetada pelos vetores do jogo, na propria cor (no lugar).
// Nunca espera o overlay: sem overlay vivo (batimento), nao faz nada alem de manter o cabecalho.
#pragma once
#include "Protocolo.h"
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <vector>

namespace dlss5ov {

struct EntradasJogo
{
    // cor final do quadro (saida do upscaler: linear, sem HUD); somada no lugar
    ID3D12Resource* cor = nullptr;
    D3D12_RESOURCE_STATES corEstado = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // vetores de movimento do jogo: MV * escala = pixels do espaco do MV (movEspacoW x movEspacoH), do atual ao anterior
    ID3D12Resource* mov = nullptr;
    D3D12_RESOURCE_STATES movEstado = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    float movEscalaX = 1, movEscalaY = 1;
    uint32_t movEspacoW = 0, movEspacoH = 0; // 0 = o tamanho da textura de movimento
    ID3D12Resource* prof = nullptr;
    D3D12_RESOURCE_STATES profEstado = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // exposicao do jogo (1x1); e = texel * escalaExposicao / preExposicao. Sem textura: e = 1
    ID3D12Resource* exposicao = nullptr;
    D3D12_RESOURCE_STATES expEstado = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    float preExposicao = 1, escalaExposicao = 1;
    // regiao valida (resolucao dinamica: so o canto superior esquerdo da textura); 0 = a textura inteira
    uint32_t corValidoW = 0, corValidoH = 0, movValidoW = 0, movValidoH = 0, profValidoW = 0, profValidoH = 0;
    float jitterX = 0, jitterY = 0;
    float perto = 0, longe = 0, fovY = 0;
    bool reset = false, profInvertida = false, profInfinita = false, hdr = true;
    bool profInvertidaConhecida = true; // false: o modulo chegou depois de o jogo dizer (o overlay mede)
};

class ClienteBuffer
{
  public:
    // cria a memoria compartilhada (nome pelo pid deste processo) e os shaders; false = sem D3DCompile ou sem memoria
    bool Inicia(ID3D12Device* d, const char* exe);
    void Encerra();
    // grava os passes na lista `c` (que o jogo vai submeter); devolve o numero do quadro (0 = nada gravado)
    uint64_t Quadro(ID3D12GraphicsCommandList* c, const EntradasJogo& e);
    // a lista do ultimo quadro ja foi submetida nesta fila: sinaliza a cerca do jogo (chamar depois do Execute/Present)
    void Submetido(ID3D12CommandQueue* q);
    bool OverlayVivo() const;
    uint64_t Aplicado() const { return cab ? cab->quadroAplicado : 0; } // ultimo quadro com a edicao somada
    const char* UltimoErro() const { return erro.c_str(); }

  private:
    template <class T> using Com = Microsoft::WRL::ComPtr<T>;
    bool Garante(const EntradasJogo& e);
    bool CriaTex(Com<ID3D12Resource>& t, UINT w, UINT h, DXGI_FORMAT f, const wchar_t* qual, uint32_t slot);
    bool CriaCerca(Com<ID3D12Fence>& f, const wchar_t* qual);
    void Barreira(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b);
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT i) const;
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT i) const;
    void Srv(UINT i, ID3D12Resource* r, DXGI_FORMAT f);
    void Uav(UINT i, ID3D12Resource* r, DXGI_FORMAT f);

    ID3D12Device* dev = nullptr;
    HANDLE mapa = nullptr;
    Cabecalho* cab = nullptr;
    uint32_t geracao = 0;
    Com<ID3D12RootSignature> raiz;
    Com<ID3D12PipelineState> psoMov, psoProf, psoCor;
    Com<ID3D12DescriptorHeap> heap;
    UINT passoDesc = 0, proxDesc = 0;
    Com<ID3D12Resource> cor[kSlotsEntrada], mov[kSlotsEntrada], prof[kSlotsEntrada], exp[kSlotsEntrada];
    Com<ID3D12Resource> edicao[kSlotsEdicao];
    Com<ID3D12Resource> temp; // copia da cor quando ela nao aceita UAV
    Com<ID3D12Fence> cercaJogo, cercaOverlay;
    DXGI_FORMAT fmtCor = DXGI_FORMAT_UNKNOWN, fmtMov = DXGI_FORMAT_UNKNOWN, fmtProf = DXGI_FORMAT_UNKNOWN;
    UINT64 corW = 0;
    UINT corH = 0, movW = 0, movH = 0, profW = 0, profH = 0;
    uint64_t quadro = 0, pendente = 0;
    std::string erro;
    std::vector<HANDLE> handles; // nomes da geracao atual (fechados quando ela troca)
};

} // namespace dlss5ov
