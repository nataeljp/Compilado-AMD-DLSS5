#include "Config.h"
#include <fstream>
#include <sstream>
#include <cctype>

namespace ov {

// le "chave": valor de um objeto JSON plano (numeros e booleanos); o resto e ignorado
static std::map<std::string, double> LePlano(const std::string& s)
{
    std::map<std::string, double> m;
    size_t i = 0;
    while ((i = s.find('"', i)) != std::string::npos)
    {
        const size_t f = s.find('"', i + 1);
        if (f == std::string::npos)
            break;
        const std::string chave = s.substr(i + 1, f - i - 1);
        size_t j = s.find_first_not_of(" \t\r\n", f + 1);
        if (j == std::string::npos || s[j] != ':')
        {
            i = f + 1;
            continue;
        }
        j = s.find_first_not_of(" \t\r\n", j + 1);
        if (j == std::string::npos)
            break;
        if (s.compare(j, 4, "true") == 0)
            m[chave] = 1;
        else if (s.compare(j, 5, "false") == 0)
            m[chave] = 0;
        else if (std::isdigit((unsigned char)s[j]) || s[j] == '-' || s[j] == '.')
            m[chave] = std::atof(s.c_str() + j);
        i = j;
    }
    return m;
}

void Config::Carrega(const std::wstring& arquivo)
{
    std::ifstream f(arquivo);
    if (!f)
        return;
    std::stringstream ss;
    ss << f.rdbuf();
    const auto m = LePlano(ss.str());
    auto F = [&](const char* k, float& v) { if (auto it = m.find(k); it != m.end()) v = float(it->second); };
    auto I = [&](const char* k, int& v) { if (auto it = m.find(k); it != m.end()) v = int(it->second); };
    auto B = [&](const char* k, bool& v) { if (auto it = m.find(k); it != m.end()) v = it->second != 0; };
    F("estilo", rede.estilo);
    F("tom", rede.tom);
    F("estrutura", rede.estrutura);
    F("pele", rede.pele);
    F("outro", rede.outro);
    F("detalhe", rede.detalhe);
    F("cor", rede.cor);
    I("passes", rede.passes);
    B("redeCompleta", rede.redeCompleta);
    F("suaviza", rede.suaviza);
    I("alturaRede", alturaRede);
    F("forca", forca);
    I("modo", modo);
    F("divisao", divisao);
    I("fpsMax", fpsMax);
    B("movimento", movimento);
}

void Config::Salva(const std::wstring& arquivo) const
{
    std::ofstream f(arquivo);
    f << "{\n";
    f << "  \"estilo\": " << rede.estilo << ",\n";
    f << "  \"tom\": " << rede.tom << ",\n";
    f << "  \"estrutura\": " << rede.estrutura << ",\n";
    f << "  \"pele\": " << rede.pele << ",\n";
    f << "  \"outro\": " << rede.outro << ",\n";
    f << "  \"detalhe\": " << rede.detalhe << ",\n";
    f << "  \"cor\": " << rede.cor << ",\n";
    f << "  \"passes\": " << rede.passes << ",\n";
    f << "  \"redeCompleta\": " << (rede.redeCompleta ? "true" : "false") << ",\n";
    f << "  \"suaviza\": " << rede.suaviza << ",\n";
    f << "  \"alturaRede\": " << alturaRede << ",\n";
    f << "  \"forca\": " << forca << ",\n";
    f << "  \"modo\": " << modo << ",\n";
    f << "  \"divisao\": " << divisao << ",\n";
    f << "  \"fpsMax\": " << fpsMax << ",\n";
    f << "  \"movimento\": " << (movimento ? "true" : "false") << "\n";
    f << "}\n";
}

} // namespace ov
