"""Gera OptiScaler/dlssnr/lmxxf2_teste.cpp: tabela "Secao.Chave" -> atribuição no Config (operator=, como o menu faz),
lida do próprio Config.cpp (linhas `Membro.set_from_config(readBool|Int|UInt|Float("Secao", "Chave"))`).
Usado só pelo teste ao vivo (variável LMXXF2_TESTE): Lmxxf2Teste::Aplica(arquivo) lê linhas Secao.Chave=valor
("auto" = volta ao padrão) e atribui. Rodar de novo quando o Config mudar."""
import re
from pathlib import Path

U = Path(__file__).resolve().parents[2] / 'optiscaler' / 'OptiScaler'
src = (U / 'Config.cpp').read_text(encoding='utf-8', errors='replace')
hdr = (U / 'Config.h').read_text(encoding='utf-8', errors='replace')

conv = {'Bool': 'Bool(v)', 'Int': 'std::stoi(v)', 'UInt': 'static_cast<uint32_t>(std::stoul(v))', 'Float': 'std::stof(v)'}
vistos = {}
for m in re.finditer(r'(\w+)\.set_from_config\(\s*read(Bool|Int|UInt|Float)\(\s*"(\w+)"\s*,\s*"(\w+)"\s*\)\s*\)', src):
    membro, tipo, sec, key = m.groups()
    # tipo declarado no Config.h (o read* pode diferir: ex. int lido como UInt)
    d = re.search(r'CustomOptional<\s*([\w:]+)\s*(?:,\s*\w+)?\s*>\s+' + membro + r'\b', hdr)
    if not d:
        continue
    decl = d.group(1)
    if decl not in ('bool', 'int', 'uint32_t', 'float', 'double', 'UINT', 'unsigned int', 'int32_t'):
        continue
    vistos.setdefault(f'{sec}.{key}', (membro, tipo, decl))

linhas = []
for nome, (membro, tipo, decl) in sorted(vistos.items()):
    val = conv[tipo]
    linhas.append(f'        {{ "{nome}", [](const std::string& v) {{ auto c = Config::Instance(); '
                  f'if (v == "auto") c->{membro}.reset(); else c->{membro} = static_cast<{decl}>({val}); }} }},')

cpp = '''// [lmxxf2] GERADO por tools/optiscaler_fork/gera_teste_ao_vivo.py - não editar à mão.
// Teste ao vivo (só com LMXXF2_TESTE no ambiente): aplica "Secao.Chave=valor" com operator=, como o menu.
#include "pch.h"
#include "Config.h"
#include "dlssnr/lmxxf2_reset.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_map>

void KeyUp(UINT vKey); // menu_common.cpp: a tecla do menu

namespace Lmxxf2Teste
{
static bool Bool(const std::string& v) { return v == "true" || v == "1" || v == "on"; }

void Aplica(const std::filesystem::path& arquivo)
{
    static const std::unordered_map<std::string, std::function<void(const std::string&)>> setters = {
''' + '\n'.join(linhas) + '''
    };
    std::ifstream in(arquivo);
    std::string linha;
    while (std::getline(in, linha))
    {
        while (!linha.empty() && (linha.back() == '\\r' || linha.back() == ' '))
            linha.pop_back();
        // comandos: @menu abre/fecha o menu (como a tecla), @reset=<cartao> é o botão "R" daquele cartão
        if (linha == "@menu")
        {
            KeyUp(Config::Instance()->ShortcutKey.value_or_default());
            LOG_INFO("[lmxxf2 teste] menu");
            continue;
        }
        if (linha.rfind("@reset=", 0) == 0)
        {
            const bool ok = Lmxxf2Reset::Restaura(linha.substr(7).c_str());
            LOG_INFO("[lmxxf2 teste] reset {}: {}", linha.substr(7), ok ? "ok" : "cartao sem reset");
            continue;
        }
        const auto eq = linha.find('=');
        if (linha.empty() || linha[0] == '#' || eq == std::string::npos)
            continue;
        const std::string chave = linha.substr(0, eq), valor = linha.substr(eq + 1);
        const auto it = setters.find(chave);
        if (it == setters.end())
        {
            LOG_WARN("[lmxxf2 teste] chave desconhecida: {}", chave);
            continue;
        }
        try
        {
            it->second(valor);
            LOG_INFO("[lmxxf2 teste] {} = {}", chave, valor);
        }
        catch (...)
        {
            LOG_WARN("[lmxxf2 teste] valor invalido: {} = {}", chave, valor);
        }
    }
}
} // namespace Lmxxf2Teste
'''
(U / 'dlssnr' / 'lmxxf2_teste.cpp').write_text(cpp, encoding='utf-8')
print(len(linhas), 'chaves')
