"""Gera OptiScaler/dlssnr/lmxxf2_reset.cpp: para cada cartão do menu (ids de MenuCommon::GetMenuBoxes), a lista dos
membros do Config que o cartão mexe, para o botão "R" (canto do cartão) voltar todos ao padrão.

Como acha os membros: o corpo de cada função de desenho (lambda do cartão ou bloco ScopedCard::Shows("id") da função
que desenha vários) e das funções que ele chama (até 3 níveis), procurando `config->X`, `c->X`, `cfg->X`,
`Config::Instance()->X` com X declarado como CustomOptional no Config.h (strings e caminhos ficam de fora).
EXTRA e FORA ajustam à mão. Rodar de novo quando o menu mudar. Imprime a contagem por cartão para revisão."""
import re
import sys
from pathlib import Path

U = Path(__file__).resolve().parents[2] / 'optiscaler' / 'OptiScaler'
FONTES = [U / 'menu' / 'menu_common.cpp', U / 'menu' / 'lmxxf2_menu.inl'] + sorted((U / 'dlssnr' / 'menu').glob('*.cpp'))

# cartões sem botão (só leitura, log, layout do próprio menu)
SEM_BOTAO = {'lmxxf2_log', 'lmxxf2_desempenho', 'shader_times', 'custom_tab', 'status_system', 'status_detected',
             'status_hooks', 'status_init', 'keybinds'}
# membros que o reset nunca toca
# liga/desliga de cada efeito (FG, rede, DLSS, GI, filtro, luz, ...): o reset volta os ajustes, não desliga o efeito
FORA = re.compile(r'^(FGEnabled|DlssNrEnabled|DLSSEnabled|AmdRtgiEnabled|AmdLookEnabled|Lmxxf2LightEnabled|'
                  r'FGReprojectionEnabled|MagnifierEnabled|MainDllPath|CustomTabCards|ConfigPath|.*Path|.*Library|LogFileName|.*Keybind.*|.*Key|'
                  r'MenuInitDelay|UseHQFont)$')
# membros que o código acessa por outra rota (tabela, função de outro módulo)
EXTRA = {
    'lmxxf2_rrjogo': ['RrJogo*'],
    'lmxxf2_gi': ['AmdRtgi*'],
    'menu_scale': ['MenuScale', 'MenuWidth', 'MenuHeight', 'FontSize'],
}
# membros que aparecem em vários cartões (ajudantes comuns) ficam só no cartão dono
SO_EM = {
    'FGInput': {'fg_selection', 'lmxxf2_fg_auto'},
    'FGOutput': {'fg_selection', 'lmxxf2_fg_auto'},
    'FGNvngxReplacement': {'fg_selection', 'lmxxf2_fg_auto'},
    'DxgiSpoofing': {'spoofing', 'lmxxf2_fg_auto'},
    'LightTheme': {'theme'},
    'MenuAccentColor*': {'theme'},
    'MenuBGColor*': {'theme'},
    'OverlayMenu': {'theme'},
    'ExtendedLimits': {'shaders_limits'},
    'OutputScalingEnabled': {'output_scaling'},
    'AmdRtgi*': {'lmxxf2_gi'},
    'FfxDenoiser*': {'upscaler'},
    'AmdNrScale': {'lmxxf2_desempenho_amdnr'},
    'AmdDynamicRes': {'lmxxf2_desempenho_amdnr'},
    'AmdInterleave*': {'lmxxf2_desempenho_amdnr', 'lmxxf2_ferramentas_amdnr'},
    'DlssNrPasses': {'lmxxf2_desempenho_amdnr'},
    'LmxxfFullNetwork': {'lmxxf2_desempenho_amdnr'},
    'AmdLmxxfFastMode': {'lmxxf2_desempenho_amdnr'},
    'AmdLmxxfTierCap': {'lmxxf2_desempenho_amdnr'},
    'AmdColourStrength': {'lmxxf2_rede'},
    'AmdDetailStrength': {'lmxxf2_rede'},
    'AmdResidualIntensity': {'lmxxf2_rede'},
    'AmdResidualLimit': {'lmxxf2_rede'},
    'AmdSharpness': {'lmxxf2_qualidade_amdnr'},
    'AmdTemporalStability': {'lmxxf2_qualidade_amdnr'},
}


def permitido(nome, cid):
    for p, donos in SO_EM.items():
        if (nome.startswith(p[:-1]) if p.endswith('*') else nome == p):
            return cid in donos
    return True


# o que fazer depois de zerar (o automático volta a valer)
DEPOIS = {
    'lmxxf2_fg_auto': 'Lmxxf2AutoFg::Reaplica();',
    'fg_selection': 'Lmxxf2AutoFg::Reaplica();',
    'spoofing': 'Lmxxf2AutoFg::AplicaSpoof();',
}

hdr = (U / 'Config.h').read_text(encoding='utf-8', errors='replace')
membros = {}
for m in re.finditer(r'CustomOptional<\s*([\w:]+)\s*(?:,\s*(\w+))?\s*>\s+(\w+)\s*(?:\{[^}]*\})?\s*;', hdr):
    tipo, _, nome = m.groups()
    if 'string' in tipo or FORA.match(nome):
        continue
    membros[nome] = tipo


def corpo(texto, abre):
    """Texto entre a chave em `abre` e a que fecha."""
    nivel = 0
    i = abre
    while i < len(texto):
        ch = texto[i]
        if ch == '"':  # pula strings
            i += 1
            while i < len(texto) and texto[i] != '"':
                i += 2 if texto[i] == '\\' else 1
        elif ch == '/' and texto[i + 1:i + 2] == '/':
            i = texto.find('\n', i)
            if i < 0:
                return texto[abre:]
        elif ch == '{':
            nivel += 1
        elif ch == '}':
            nivel -= 1
            if nivel == 0:
                return texto[abre:i + 1]
        i += 1
    return texto[abre:]


funcoes = {}
DEF = re.compile(r'^[ \t]*(?:static\s+|inline\s+)*(?:void|bool|int|float|auto|const\s+[\w:]+&?)\s+'
                 r'((?:\w+::)*(\w+))\s*\(([^;{}]*?)\)\s*(?:const\s*)?\{', re.M)
fontes = {}
for f in FONTES:
    t = f.read_text(encoding='utf-8', errors='replace')
    fontes[f] = t
    for m in DEF.finditer(t):
        nome = m.group(2)
        if nome in ('if', 'for', 'while', 'switch'):
            continue
        funcoes.setdefault(nome, []).append(corpo(t, m.end() - 1))

ACESSO = re.compile(r'(?:\bconfig|\bc|\bcfg|\bconf|Config::Instance\(\))\s*->\s*(\w+)')
CHAMADA = re.compile(r'\b(\w+)\s*\(')
IGNORA_CHAMADA = {'RenderMenuBox', 'GetMenuBoxes', 'RenderCustomTab', 'SaveIni', 'Save', 'Reload', 'RenderMenu',
                  'RenderMainMenu', 'RenderMainMenuFooter'}


def membros_de(texto, nivel=0, vistos=None):
    vistos = vistos if vistos is not None else set()
    achados = {m.group(1) for m in ACESSO.finditer(texto) if m.group(1) in membros}
    if nivel >= 3:
        return achados
    # no lambda do cartão a função vai como ponteiro (RenderCard(c, RenderVsyncSettings)): qualquer nome conhecido
    chamadas = re.finditer(r'(\w+)', texto) if nivel == 0 else CHAMADA.finditer(texto)
    for m in chamadas:
        n = m.group(1)
        if n in vistos or n in IGNORA_CHAMADA or n not in funcoes:
            continue
        vistos.add(n)
        for b in funcoes[n]:
            achados |= membros_de(b, nivel + 1, vistos)
    return achados


menu = fontes[U / 'menu' / 'menu_common.cpp']
ini = menu.index('const MenuCommon::MenuBox* MenuCommon::GetMenuBoxes')
fim = menu.index('// clang-format on', ini)
tabela = menu[ini:fim]
cartoes = []
for m in re.finditer(r'\{\s*"(\w+)",\s*"[^"]*",\s*"[^"]*",\s*(.*?)\},?\s*\n(?=\s*(?:\{|//|#|\}))', tabela, re.S):
    cartoes.append((m.group(1), m.group(2)))

# blocos das funções que desenham vários cartões: do Shows("id") até o próximo Shows( ou o fim do corpo
blocos = {}
for nome, corpos in funcoes.items():
    for b in corpos:
        pos = [(mm.start(), mm.group(1)) for mm in re.finditer(r'ScopedCard::Shows\("(\w+)"\)', b)]
        for k, (p, cid) in enumerate(pos):
            q = pos[k + 1][0] if k + 1 < len(pos) else len(b)
            blocos.setdefault(cid, []).append(b[p:q])

saida = {}
for cid, resto in cartoes:
    if cid in SEM_BOTAO:
        continue
    achados = set()
    if cid in blocos:
        for b in blocos[cid]:
            achados |= membros_de(b)
    else:
        achados |= membros_de(resto)
    lista = sorted(achados)
    padroes = EXTRA.get(cid, [])
    for p in padroes:
        pre = p[:-1] if p.endswith('*') else None
        lista += [n for n in membros if (n.startswith(pre) if pre else n == p) and n not in lista]
    saida[cid] = sorted(n for n in set(lista) if permitido(n, cid))

linhas = []
for cid, lista in saida.items():
    if not lista and cid not in DEPOIS:
        continue
    zeros = ''.join(f'\n            Zera(c->{n});' for n in lista)
    depois = f'\n            {DEPOIS[cid]}' if cid in DEPOIS else ''
    linhas.append(f'        {{ "{cid}", {len(lista)}, [](Config* c) {{{zeros}{depois}\n        }} }},')

cpp = '''// [lmxxf2] GERADO por tools/optiscaler_fork/gera_reset_cartoes.py - não editar à mão.
// Botão "R" de cada cartão do menu: volta ao padrão os membros do Config que o cartão mexe.
#include "pch.h"
#include "Config.h"
#include "dlssnr/lmxxf2_autofg.h"
#include "dlssnr/lmxxf2_reset.h"

#include <cstring>

namespace Lmxxf2Reset
{
namespace
{
// operator= com optional vazio: sai do modo volátil e fica sem valor (o padrão / auto), como o menu faz
template <class T, HasDefaultValue D> void Zera(CustomOptional<T, D>& o) { o = std::optional<T> {}; }

struct Cartao
{
    const char* id;
    int chaves;
    void (*zera)(Config*);
};

const Cartao kCartoes[] = {
''' + '\n'.join(linhas) + '''
};

const Cartao* Acha(const char* id)
{
    if (id == nullptr)
        return nullptr;
    for (const auto& c : kCartoes)
        if (std::strcmp(c.id, id) == 0)
            return &c;
    return nullptr;
}
} // namespace

int Chaves(const char* cartao)
{
    const auto* c = Acha(cartao);
    return c == nullptr ? -1 : c->chaves;
}

bool Restaura(const char* cartao)
{
    const auto* c = Acha(cartao);
    if (c == nullptr)
        return false;
    c->zera(Config::Instance());
    LOG_INFO("[lmxxf2] reset do cartao {} ({} chaves)", cartao, c->chaves);
    return true;
}
} // namespace Lmxxf2Reset
'''
(U / 'dlssnr' / 'lmxxf2_reset.cpp').write_text(cpp, encoding='utf-8')
for cid, lista in saida.items():
    print(f'{cid:28} {len(lista):3}  {", ".join(lista[:14])}{" ..." if len(lista) > 14 else ""}')
print('cartoes no menu:', len(cartoes), file=sys.stderr)
