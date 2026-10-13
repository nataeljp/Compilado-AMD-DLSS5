#pragma once
// [lmxxf2] substituto mínimo do misc/ProcessNameMatch.h do AMDNR (não publicado):
// reconhece o instalador do runtime do danielblnc pelo nome (dlssnr_on_amd_setup*.exe).
#include <string>
#include <algorithm>
#include <cwctype>

namespace ProcessNameMatch
{
inline bool IsAmdSetupExe(std::wstring n)
{
    std::transform(n.begin(), n.end(), n.begin(), [](wchar_t c) { return (wchar_t) std::towlower(c); });
    return n.rfind(L"dlssnr_on_amd_setup", 0) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, L".exe") == 0;
}
inline bool IsAmdSetupExe(const std::string& n) { return IsAmdSetupExe(std::wstring(n.begin(), n.end())); }
}
