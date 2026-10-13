#pragma once
// [lmxxf2] substituto mínimo do misc/LogRotationNames.h do AMDNR (não publicado):
// SettleLogGate deixa passar um texto de status só quando ele muda; "" zera.
#include <string>

namespace LogRotationNames
{
struct SettleLogGate
{
    std::string last;
    bool ShouldLog(const std::string& s)
    {
        if (s.empty())
        {
            last.clear();
            return false;
        }
        if (s == last)
            return false;
        last = s;
        return true;
    }
};
}
