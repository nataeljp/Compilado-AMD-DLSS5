#include "Config.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace ov {

ParamsRede Config::Rede() const
{
    ParamsRede r = rede;
    r.estilo = float(std::clamp(ngx.modelo, 0, 2)); // o controle de estilo da rede: Model A/B/C = 0, 1, 2 (x 1/128)
    r.tom = std::clamp(ngx.tomLocal, 0.f, 1.f);
    r.estrutura = ngx.estruturaLocal;
    r.pele = ngx.mascara ? ngx.estruturaPele : -1.f;
    r.outro = ngx.mascara ? ngx.estruturaLocal : -1.f;
    r.detalhe = r.cor = 1.f; // a resposta inteira do runtime; a composicao e feita no overlay
    return r;
}

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
    I("modelo", ngx.modelo);
    F("intensidade", ngx.intensidade);
    F("tomLocal", ngx.tomLocal);
    F("estruturaLocal", ngx.estruturaLocal);
    F("estruturaPele", ngx.estruturaPele);
    B("mascara", ngx.mascara);
    B("gradacao", ngx.gradacao);
    I("composicao", temporal.composicao);
    F("corComposicao", temporal.cor);
    F("protecao", temporal.guarda);
    F("exposicao", temporal.exposicao);
    I("passes", rede.passes);
    B("redeCompleta", rede.redeCompleta);
    I("alturaRede", alturaRede);
    F("forca", forca);
    I("ponte", ponte);
    F("brancoNits", brancoNits);
    F("preLuz", preLuz);
    F("preSombra", preSombra);
    F("preLimiar", preLimiar);
    I("modo", modo);
    B("movimento", movimento);
    B("usaConfianca", usaConfianca);
    I("ritmo", ritmo);
    I("ritmoMax", ritmoMax);
    B("reusoVit", reusoVit);
    I("temporalRede", temporalRede);
    I("profundidade", profundidade);
    B("profundidadeAuto", profundidadeAuto);
    I("profundidadeMax", profundidadeMax);
    F("realceTemporal", realceTemporal);
    I("estabilidade", temporal.maxQuadros);
    F("rejeicao", temporal.rejeicao);
    F("divisao", divisao);
    I("fpsMax", fpsMax);
    B("ocultaDeGravacoes", ocultaDeGravacoes);
}

void Config::Salva(const std::wstring& arquivo) const
{
    std::ofstream f(arquivo);
    auto b = [](bool v) { return v ? "true" : "false"; };
    f << "{\n";
    f << "  \"modelo\": " << ngx.modelo << ",\n";
    f << "  \"intensidade\": " << ngx.intensidade << ",\n";
    f << "  \"tomLocal\": " << ngx.tomLocal << ",\n";
    f << "  \"estruturaLocal\": " << ngx.estruturaLocal << ",\n";
    f << "  \"estruturaPele\": " << ngx.estruturaPele << ",\n";
    f << "  \"mascara\": " << b(ngx.mascara) << ",\n";
    f << "  \"gradacao\": " << b(ngx.gradacao) << ",\n";
    f << "  \"composicao\": " << temporal.composicao << ",\n";
    f << "  \"corComposicao\": " << temporal.cor << ",\n";
    f << "  \"protecao\": " << temporal.guarda << ",\n";
    f << "  \"exposicao\": " << temporal.exposicao << ",\n";
    f << "  \"passes\": " << rede.passes << ",\n";
    f << "  \"redeCompleta\": " << b(rede.redeCompleta) << ",\n";
    f << "  \"alturaRede\": " << alturaRede << ",\n";
    f << "  \"forca\": " << forca << ",\n";
    f << "  \"ponte\": " << ponte << ",\n";
    f << "  \"brancoNits\": " << brancoNits << ",\n";
    f << "  \"preLuz\": " << preLuz << ",\n";
    f << "  \"preSombra\": " << preSombra << ",\n";
    f << "  \"preLimiar\": " << preLimiar << ",\n";
    f << "  \"modo\": " << modo << ",\n";
    f << "  \"movimento\": " << b(movimento) << ",\n";
    f << "  \"usaConfianca\": " << b(usaConfianca) << ",\n";
    f << "  \"ritmo\": " << ritmo << ",\n";
    f << "  \"ritmoMax\": " << ritmoMax << ",\n";
    f << "  \"reusoVit\": " << b(reusoVit) << ",\n";
    f << "  \"temporalRede\": " << temporalRede << ",\n";
    f << "  \"profundidade\": " << profundidade << ",\n";
    f << "  \"profundidadeAuto\": " << b(profundidadeAuto) << ",\n";
    f << "  \"profundidadeMax\": " << profundidadeMax << ",\n";
    f << "  \"realceTemporal\": " << realceTemporal << ",\n";
    f << "  \"estabilidade\": " << temporal.maxQuadros << ",\n";
    f << "  \"rejeicao\": " << temporal.rejeicao << ",\n";
    f << "  \"divisao\": " << divisao << ",\n";
    f << "  \"fpsMax\": " << fpsMax << ",\n";
    f << "  \"ocultaDeGravacoes\": " << b(ocultaDeGravacoes) << "\n";
    f << "}\n";
}

} // namespace ov
