// RTGI: original de Caliel666/neural-amd-opti (GPL-3.0), sem mudanca.
#pragma once
#include "AmdPreSr.h"
#include <memory>
namespace AmdPreSr
{
// [lmxxf2] a pasta dos shaders da iluminacao: DLSS5-AMD\experimental_lighting (o pacote organizado), senao a antiga ao
// lado do OptiScaler.dll
inline std::filesystem::path LightingDir(const std::filesystem::path& dir)
{
    std::error_code ec;
    const auto novo = dir / L"DLSS5-AMD" / L"experimental_lighting";
    return std::filesystem::is_directory(novo, ec) ? novo : dir / L"experimental_lighting";
}
// Called only at the owning backend's quiescent frame boundary. The caller
// retains this object and its resources until its GPU completion fence retires.
class RtgiNative
{
    struct Impl;
    std::unique_ptr<Impl> p;

  public:
    RtgiNative(ID3D12Device*, const std::filesystem::path&);
    ~RtgiNative();
    ID3D12Resource* Record(ID3D12GraphicsCommandList*, const Frame&, const RtgiSettings&);
    void ResetHistory();
};
} // namespace AmdPreSr
