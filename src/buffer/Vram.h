// DLSS5_TEX_RAM=1 poe as texturas da captura na RAM (custom heap L0): medido, custa o dobro do tempo por quadro, por isso nao e o padrao. E, para o registro, quanto o jogo usa da VRAM.
#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>

namespace dlss5ov {

// O que o overlay ocupa (medido: 1,6 a 1,8 GB): enquanto ele estiver ligado, o orcamento que o jogo ve nao encolhe por causa dele (Modulo.cpp, Orcamento)
constexpr UINT64 kVramDoOverlay = UINT64(1800) << 20;
// Quem chama volta para a VRAM se o driver recusar
inline bool QuerRam() { return GetEnvironmentVariableW(L"DLSS5_TEX_RAM", nullptr, 0) != 0; }
inline D3D12_HEAP_PROPERTIES HeapDaRam()
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_CUSTOM;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    return hp;
}

inline void TextoVramJogo(ID3D12Device* d, char* texto, size_t n)
{
    texto[0] = 0;
    Microsoft::WRL::ComPtr<IDXGIFactory4> f;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> a;
    DXGI_QUERY_VIDEO_MEMORY_INFO i{};
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) && SUCCEEDED(f->EnumAdapterByLuid(d->GetAdapterLuid(), IID_PPV_ARGS(&a))) &&
        SUCCEEDED(a->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &i)))
        snprintf(texto, n, "VRAM do jogo %llu de %llu MB", (unsigned long long)(i.CurrentUsage >> 20), (unsigned long long)(i.Budget >> 20));
}

} // namespace dlss5ov
