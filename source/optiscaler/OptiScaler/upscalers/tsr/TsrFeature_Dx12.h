#pragma once
// [lmxxf2] TSR proprio (upscaler temporal no estilo do TSR da UE5, escrito do zero; ver TsrShaders.h). Entra como mais
// uma opcao de upscaler do OptiScaler em D3D12 ("tsr" no ini, "TSR (lmxxf2)" no menu). A rede DLSS5 e o resto do
// pos-processamento do OptiScaler funcionam com ele como com os outros upscalers.
#include <upscalers/IFeature_Dx12.h>

#include <wrl/client.h>

class TsrFeatureDx12 : public IFeature_Dx12
{
  public:
    TsrFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);
    ~TsrFeatureDx12();

    feature_version Version() override { return { 1, 0, 0 }; }
    Upscaler GetUpscalerType() const final { return Upscaler::TSR; }
    API Api() const override { return IFeature_Dx12::Api(); }
    bool IsWithDx12() override { return false; }
    bool CallsUpscalerEndByItself() override { return IFeature_Dx12::CallsUpscalerEndByItself(); }
    std::optional<double> ReadUpscalerTime(void* commandQueue) override
    {
        return IFeature_Dx12::ReadUpscalerTime(commandQueue);
    }
    void ReadDetailedGpuTimes(void* commandQueue, std::vector<DetailedGpuTime>& detailedGpuTimes) override
    {
        return IFeature_Dx12::ReadDetailedGpuTimes(commandQueue, detailedGpuTimes);
    }

  protected:
    bool InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;
    bool EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;

  private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    struct Textura
    {
        ComPtr<ID3D12Resource> res;
        D3D12_RESOURCE_STATES estado = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    };

    static constexpr UINT kAneis = 3;   // quadros em voo
    static constexpr UINT kPasses = 3;  // paralaxe, anti-aliasing, historico
    static constexpr UINT kSrv = 8;     // t0..t7
    static constexpr UINT kUav = 4;     // u0..u3
    static constexpr UINT kBloco = kSrv + kUav;

    ComPtr<ID3D12RootSignature> _raiz;
    ComPtr<ID3D12PipelineState> _psoParalaxe, _psoAntiAlias, _psoHistorico;
    ComPtr<ID3D12DescriptorHeap> _heap;
    UINT _incremento = 0;
    UINT _anel = 0;

    Textura _hist[2], _meta[2], _lin[2], _vel, _aa;
    UINT _histW = 0, _histH = 0, _renW = 0, _renH = 0;
    UINT _atual = 0;
    UINT _ultimoRenderW = 0, _ultimoRenderH = 0;
    float _jitterAnterior[2] {};
    LONGLONG _ultimoQpc = 0;
    bool _primeiro = true;

    bool CriaPipelines();
    bool GaranteTexturas(UINT renderMaxW, UINT renderMaxH, UINT saidaW, UINT saidaH);
    ComPtr<ID3D12Resource> CriaTextura(UINT w, UINT h, DXGI_FORMAT formato, const wchar_t* nome);
    void Transiciona(ID3D12GraphicsCommandList* cmd, Textura& t, D3D12_RESOURCE_STATES novo);
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT bloco, UINT slot) const;
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT bloco) const;
    void Srv(UINT bloco, UINT slot, ID3D12Resource* r, DXGI_FORMAT forcado = DXGI_FORMAT_UNKNOWN);
    void Uav(UINT bloco, UINT slot, ID3D12Resource* r, DXGI_FORMAT forcado = DXGI_FORMAT_UNKNOWN);
    void Nulos(UINT bloco);
};
