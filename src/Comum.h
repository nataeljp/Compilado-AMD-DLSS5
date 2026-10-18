// DLSS5 Overlay: a rede DLSS 5 (lmxxf, MIT) sobre a imagem capturada de qualquer janela, num overlay por cima dela.
// Comum: D3D12 na placa AMD, log em .jsonl, passes compute compilados na inicializacao.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace ov {

// ---- log: uma linha JSON por evento em overlay.jsonl, ao lado do exe ----
void LogAbre(const std::wstring& pasta);
void Log(const char* tipo, const std::string& msg);
std::string Fmt(const char* f, ...);
std::string Utf8(const std::wstring& w);

inline void Ck(HRESULT h, const char* onde)
{
    if (FAILED(h))
        throw std::runtime_error(Fmt("%s falhou (hr=0x%08lx)", onde, (unsigned long)h));
}

// ---- GPU: dispositivo e fila DIRECT na placa AMD; a mesma placa serve a captura (D3D11) pelo LUID ----
struct Gpu
{
    ComPtr<IDXGIFactory6> fabrica;
    ComPtr<IDXGIAdapter1> adaptador;
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> fila;
    ComPtr<ID3D12Fence> cerca;
    UINT64 valor = 0;
    HANDLE evento = nullptr;
    LUID luid{};
    std::wstring nome;

    void Cria();
    // sinaliza a fila e espera na CPU
    void Espera();
    UINT64 Sinaliza();
    void EsperaValor(UINT64 v);
};

// heap de descritores CBV/SRV/UAV visivel aos shaders, alocacao linear por quadro
struct Descritores
{
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT passo = 0, total = 0, usado = 0;
    void Cria(ID3D12Device* d, UINT n);
    void Reinicia() { usado = 0; }
    // reserva n descritores contiguos; devolve o indice do primeiro
    UINT Reserva(UINT n);
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT i) const;
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu_(UINT i) const;
};

// textura 2D comum (DEFAULT) com UAV e estado rastreado
struct Tex
{
    ComPtr<ID3D12Resource> r;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    UINT w = 0, h = 0;
    D3D12_RESOURCE_STATES estado = D3D12_RESOURCE_STATE_COMMON;
    void Cria(ID3D12Device* d, UINT w_, UINT h_, DXGI_FORMAT f, const wchar_t* nome, bool uav = true, bool rt = false);
    void Para(ID3D12GraphicsCommandList* c, D3D12_RESOURCE_STATES s);
};

void Barreira(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b);

// o monitor da janela esta com HDR ligado? `escalaSdr` = branco SDR do Windows em unidades scRGB (1,0 = 80 nits)
bool MonitorHdr(Gpu& g, HWND janela, float& escalaSdr);

// passe compute: raiz = constantes (b0, ate 32 dwords) + tabela de SRVs (t0..) + tabela de UAVs (u0..) +
// amostrador linear clamp (s0)
struct Passe
{
    ComPtr<ID3D12RootSignature> raiz;
    ComPtr<ID3D12PipelineState> pso;
    UINT nSrv = 0, nUav = 0, nConst = 0;
    std::string nome;
    void Cria(ID3D12Device* d, const char* hlsl, const char* entrada, UINT srvs, UINT uavs, UINT constantes,
              const char* nome_);
    // grava o despacho; srvs/uavs ja devem estar nos estados certos
    void Roda(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d,
              const std::vector<std::pair<ID3D12Resource*, DXGI_FORMAT>>& srvs,
              const std::vector<std::pair<ID3D12Resource*, DXGI_FORMAT>>& uavs, const void* constantes,
              UINT gx, UINT gy);
};

} // namespace ov
