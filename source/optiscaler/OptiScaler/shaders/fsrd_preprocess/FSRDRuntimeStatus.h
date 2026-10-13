#pragma once
// [lmxxf2] Tipos que o menu do AMDNR consulta. O FSR Ray Regeneration deste host é o do burak113, que não publica
// a tabela de debug views do AMDNR nem a desistência por sessão: DebugViews fica nulo e o menu do AMDNR esconde as
// linhas que dependem dele (regra dele: "no table yet: no row"). O debug do RR fica no cartão Ray Regeneration.
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace FSRD::RuntimeStatus
{
struct DebugView
{
    const char* Name = nullptr;
    uint64_t Value = 0;
};
struct DebugViewTable
{
    size_t Count = 0;
    const DebugView* Views = nullptr;
};
inline std::atomic<const DebugViewTable*> DebugViews { nullptr };
inline std::atomic<bool> RayRegenOffForSession { false };
} // namespace FSRD::RuntimeStatus
