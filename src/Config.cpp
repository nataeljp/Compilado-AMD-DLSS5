#include "Config.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace ov {

// nomes das chaves de cor por matiz no config.json (matizVermelhoSat, matizVermelhoLuz, ...)
static const char* const kNomesMatiz[6] = {"Vermelho", "Amarelo", "Verde", "Ciano", "Azul", "Magenta"};

// Tom e Estrutura sao entradas da propria rede, nas faixas da NVIDIA. Medido (mede_controles, imagem da NVIDIA): fora
// delas a rede inverte -- a estrutura acima de 2 (1,5 na rede pequena) e o tom acima de 1 clareiam a imagem em vez de
// reforcar o efeito -- entao nada passa delas.
ParamsRede Config::Rede() const
{
    ParamsRede r = rede;
    r.estilo = float(std::clamp(ngx.modelo, 0, 2)); // o controle de estilo da rede: Model A/B/C = 0, 1, 2 (x 1/128)
    r.tom = std::clamp(ngx.tomLocal, 0.f, 1.f);
    r.estrutura = std::clamp(ngx.estruturaLocal, -1.f, 2.f);
    // pele: so o canal de pele recebe a estrutura da pele; o "outro" fica no 1 da NVIDIA. Antes, desligar a pele
    // separada levava os dois a -1 e o efeito da rede caia pela metade na imagem inteira (medido), pele ou nao.
    // Medido canal por canal: nenhum canal da rede separa a pele (todos mudam pele e resto quase igual); a separacao de
    // pele da NVIDIA usa um modelo de mascara proprio, que nao existe neste runtime.
    r.pele = ngx.mascara ? std::clamp(ngx.estruturaPele, -1.f, 2.f) : 1.f;
    r.outro = 1.f;
    // a decodificacao da NVIDIA no runtime: a resposta inteira (a forca e a cor ficam nos controles globais)
    r.detalhe = 1.f;
    r.cor = 1.f;
    r.forcaHistorico = std::clamp(forcaHistorico, 0.f, 2.f);
    r.recorteHistorico = std::clamp(recorteHistorico, 0.f, 4.f);
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
    I("luz", temporal.luz);
    I("passes", rede.passes);
    B("redeCompleta", rede.redeCompleta);
    I("alturaRede", alturaRede);
    I("fonte", fonte);
    B("redePequena", redePequena);
    F("forca", forca);
    F("bilateral", bilateral);
    I("raioBilateral", raioBilateral);
    raioBilateral = raioBilateral < 1 ? 1 : raioBilateral > 8 ? 8 : raioBilateral;
    F("denoiseTemporal", denoiseTemporal);
    F("rcasForca", rcas);
    F("nitidezCas", nitidez);
    I("ponte", ponte);
    F("brancoNits", brancoNits);
    F("preLuz", preLuz);
    F("preSombra", preSombra);
    F("preLimiar", preLimiar);
    F("preLimiarSombra", preLimiarSombra);
    F("preCor", preCor);
    for (int i = 0; i < 6; ++i)
    {
        F((std::string("matiz") + kNomesMatiz[i] + "Sat").c_str(), matiz.sat[i]);
        F((std::string("matiz") + kNomesMatiz[i] + "Luz").c_str(), matiz.luz[i]);
    }
    I("modo", modo);
    I("quadroGerado", gerado);
    I("geracaoQuadros", geracao);
    if (geracao > 4)
        geracao = 4;
    B("movimento", movimento);
    I("ritmo", ritmo);
    B("reusoVit", reusoVit);
    I("temporalRede", temporalRede);
    I("profundidade", profundidade);
    B("profundidadeAuto", profundidadeAuto);
    I("profundidadeMax", profundidadeMax);
    F("realceTemporal", realceTemporal);
    F("forcaHistoricoRede", forcaHistorico);
    F("recorteHistoricoRede", recorteHistorico);
    F("divisao", divisao);
    I("fpsMax", fpsMax);
    B("ocultaDeGravacoes", ocultaDeGravacoes);
}

std::string Config::Texto() const
{
    std::ostringstream f;
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
    f << "  \"luz\": " << temporal.luz << ",\n";
    f << "  \"passes\": " << rede.passes << ",\n";
    f << "  \"redeCompleta\": " << b(rede.redeCompleta) << ",\n";
    f << "  \"alturaRede\": " << alturaRede << ",\n";
    f << "  \"fonte\": " << fonte << ",\n";
    f << "  \"redePequena\": " << b(redePequena) << ",\n";
    f << "  \"forca\": " << forca << ",\n";
    f << "  \"bilateral\": " << bilateral << ",\n";
    f << "  \"raioBilateral\": " << raioBilateral << ",\n";
    f << "  \"denoiseTemporal\": " << denoiseTemporal << ",\n";
    f << "  \"rcasForca\": " << rcas << ",\n";
    f << "  \"nitidezCas\": " << nitidez << ",\n";
    f << "  \"ponte\": " << ponte << ",\n";
    f << "  \"brancoNits\": " << brancoNits << ",\n";
    f << "  \"preLuz\": " << preLuz << ",\n";
    f << "  \"preSombra\": " << preSombra << ",\n";
    f << "  \"preLimiar\": " << preLimiar << ",\n";
    f << "  \"preLimiarSombra\": " << preLimiarSombra << ",\n";
    f << "  \"preCor\": " << preCor << ",\n";
    for (int i = 0; i < 6; ++i)
    {
        f << "  \"matiz" << kNomesMatiz[i] << "Sat\": " << matiz.sat[i] << ",\n";
        f << "  \"matiz" << kNomesMatiz[i] << "Luz\": " << matiz.luz[i] << ",\n";
    }
    f << "  \"modo\": " << modo << ",\n";
    f << "  \"quadroGerado\": " << gerado << ",\n";
    f << "  \"geracaoQuadros\": " << geracao << ",\n";
    f << "  \"movimento\": " << b(movimento) << ",\n";
    f << "  \"ritmo\": " << ritmo << ",\n";
    f << "  \"reusoVit\": " << b(reusoVit) << ",\n";
    f << "  \"temporalRede\": " << temporalRede << ",\n";
    f << "  \"profundidade\": " << profundidade << ",\n";
    f << "  \"profundidadeAuto\": " << b(profundidadeAuto) << ",\n";
    f << "  \"profundidadeMax\": " << profundidadeMax << ",\n";
    f << "  \"realceTemporal\": " << realceTemporal << ",\n";
    f << "  \"forcaHistoricoRede\": " << forcaHistorico << ",\n";
    f << "  \"recorteHistoricoRede\": " << recorteHistorico << ",\n";
    f << "  \"divisao\": " << divisao << ",\n";
    f << "  \"fpsMax\": " << fpsMax << ",\n";
    f << "  \"ocultaDeGravacoes\": " << b(ocultaDeGravacoes) << "\n";
    f << "}\n";
    return f.str();
}

void Config::Salva(const std::wstring& arquivo) const
{
    std::ofstream f(arquivo);
    f << Texto();
}

} // namespace ov
