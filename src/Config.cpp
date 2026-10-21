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
    r.mascaraPixel = dev.liga && dev.PorPixel();
    TabelaMascara(r.tabela);
    return r;
}

void Config::TabelaMascara(float t[40]) const
{
    auto linha = [&](int g, float tom, float est, float pele, int modelo) {
        float* l = t + g * 8;
        l[0] = std::clamp(tom, 0.f, 1.f);
        l[1] = std::clamp(est, -1.f, 4.f);
        l[2] = std::clamp(pele, -1.f, 4.f);
        l[3] = ngx.mascara ? std::clamp(est, -1.f, 4.f) : -1.f; // "outro": a estrutura de quem nao e personagem
        l[4] = float(std::clamp(modelo, 0, 2)) / 128.f;         // estilo: Model A/B/C = 0, 1, 2 (x 1/128)
        l[5] = l[6] = l[7] = 0.f;
    };
    linha(0, ngx.tomLocal, ngx.estruturaLocal, ngx.mascara ? ngx.estruturaPele : -1.f, ngx.modelo);
    // A rede mistura cada pixel com os vizinhos: numa regiao pequena (a pele) o mesmo ajuste rende ~metade do que rende na
    // tela inteira (medido na Zorah). O ajuste do grupo entra em dobro para o painel valer o que diz.
    constexpr float kRegiao = 2.f;
    // explicita: a rede ja foi treinada com o canal de personagem (pele); falta dizer ONDE e personagem. O cenario (e os
    // objetos) ficam com o canal de personagem desligado (-1), o personagem com ele ligado.
    const bool expl = ngx.mascara && dev.explicita > 0;
    float base[8];
    std::copy(t, t + 8, base);
    if (expl)
        t[2] = -1.f;
    for (int g = 1; g <= 4; ++g)
    {
        if (expl)
        {
            std::copy(base, base + 8, t + g * 8);
            float* l = t + g * 8;
            if (g == 4)
                l[2] = -1.f; // objeto: cenario
            else if (dev.explicita == 2)
                l[3] = -1.f; // personagem: o canal de cena sai
            const GrupoMascara& G = dev.grupo[g - 1];
            if (G.Neutro())
                continue;
            l[0] = std::clamp(std::clamp(ngx.tomLocal, 0.f, 1.f) + kRegiao * G.tom, -1.f, 2.f);
            l[1] = std::clamp(ngx.estruturaLocal + kRegiao * G.estrutura, -1.f, 4.f);
            if (g <= 3)
                l[2] = std::clamp(ngx.estruturaPele + kRegiao * G.estrutura, -1.f, 4.f);
            if (dev.explicita != 2 || g == 4)
                l[3] = l[1];
            if (G.modelo >= 0)
                l[4] = float(std::clamp(G.modelo, 0, 2)) / 128.f;
            continue;
        }
        const GrupoMascara& G = dev.grupo[g - 1];
        std::copy(t, t + 8, t + g * 8);
        if (G.Neutro())
            continue;
        float* l = t + g * 8;
        const float est = ngx.estruturaLocal + kRegiao * G.estrutura;
        l[0] = std::clamp(std::clamp(ngx.tomLocal, 0.f, 1.f) + kRegiao * G.tom, -1.f, 2.f);
        l[1] = std::clamp(est, -1.f, 4.f);
        if (ngx.mascara)
        {
            l[3] = l[1];
            if (g <= 3) // pele, cabelo e personagem: o canal de pele (o do personagem) leva a estrutura do grupo
                l[2] = std::clamp(ngx.estruturaPele + kRegiao * G.estrutura, -1.f, 4.f);
        }
        if (G.modelo >= 0)
            l[4] = float(std::clamp(G.modelo, 0, 2)) / 128.f;
    }
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
    B("mascaraDev", dev.liga);
    B("mostraMascara", dev.mostra);
    I("taxaMascara", dev.taxa);
    I("mascaraExplicita", dev.explicita);
    for (int g = 0; g < 4; ++g)
    {
        const std::string p = "grupo" + std::to_string(g + 1);
        F((p + "Dlss").c_str(), dev.grupo[g].dlss);
        F((p + "Estrutura").c_str(), dev.grupo[g].estrutura);
        F((p + "Tom").c_str(), dev.grupo[g].tom);
        I((p + "Modelo").c_str(), dev.grupo[g].modelo);
    }
    I("composicao", temporal.composicao);
    F("corComposicao", temporal.cor);
    F("protecao", temporal.guarda);
    F("exposicao", temporal.exposicao);
    I("passes", rede.passes);
    B("redeCompleta", rede.redeCompleta);
    I("alturaRede", alturaRede);
    I("fonte", fonte);
    B("redePequena", redePequena);
    F("forca", forca);
    I("ponte", ponte);
    F("brancoNits", brancoNits);
    F("preLuz", preLuz);
    F("preSombra", preSombra);
    F("preLimiar", preLimiar);
    F("preLimiarSombra", preLimiarSombra);
    F("preCor", preCor);
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
    f << "  \"mascaraDev\": " << b(dev.liga) << ",\n";
    f << "  \"mostraMascara\": " << b(dev.mostra) << ",\n";
    f << "  \"taxaMascara\": " << dev.taxa << ",\n";
    f << "  \"mascaraExplicita\": " << dev.explicita << ",\n";
    for (int g = 0; g < 4; ++g)
    {
        const std::string p = "  \"grupo" + std::to_string(g + 1);
        f << p << "Dlss\": " << dev.grupo[g].dlss << ",\n";
        f << p << "Estrutura\": " << dev.grupo[g].estrutura << ",\n";
        f << p << "Tom\": " << dev.grupo[g].tom << ",\n";
        f << p << "Modelo\": " << dev.grupo[g].modelo << ",\n";
    }
    f << "  \"composicao\": " << temporal.composicao << ",\n";
    f << "  \"corComposicao\": " << temporal.cor << ",\n";
    f << "  \"protecao\": " << temporal.guarda << ",\n";
    f << "  \"exposicao\": " << temporal.exposicao << ",\n";
    f << "  \"passes\": " << rede.passes << ",\n";
    f << "  \"redeCompleta\": " << b(rede.redeCompleta) << ",\n";
    f << "  \"alturaRede\": " << alturaRede << ",\n";
    f << "  \"fonte\": " << fonte << ",\n";
    f << "  \"redePequena\": " << b(redePequena) << ",\n";
    f << "  \"forca\": " << forca << ",\n";
    f << "  \"ponte\": " << ponte << ",\n";
    f << "  \"brancoNits\": " << brancoNits << ",\n";
    f << "  \"preLuz\": " << preLuz << ",\n";
    f << "  \"preSombra\": " << preSombra << ",\n";
    f << "  \"preLimiar\": " << preLimiar << ",\n";
    f << "  \"preLimiarSombra\": " << preLimiarSombra << ",\n";
    f << "  \"preCor\": " << preCor << ",\n";
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
