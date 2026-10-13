"""Copia para o Config do OptiScaler upstream (ramo lmxxf2) as chaves do AMDNR que o núcleo neural usa.

Campos: linhas `CustomOptional<...> Nome ...;` do Config.h do AMDNR cujo nome não existe no upstream.
Leitura/gravação: linhas de uma só instrução `Nome.set_from_config(...)` e `ini.SetValue(..., Instance()->Nome...)`.
Tudo entra num bloco marcado [lmxxf2], para dar para achar e refazer.
"""
import re
from pathlib import Path

AM = Path(r'C:\dlss5\terceiros\amdnr_src\main\AMDNR-source\optiscaler')
UP = Path(r'C:\dlss5\terceiros\optiscaler\OptiScaler')
MARCA = '[lmxxf2]'


def ler(p):
    return p.read_text(encoding='utf-8-sig', errors='replace')


ah, uh = ler(AM / 'Config.h'), ler(UP / 'Config.h')
ac, uc = ler(AM / 'Config.cpp'), ler(UP / 'Config.cpp')
if MARCA in uh:
    raise SystemExit('ja mesclado')

campo = re.compile(r'^\s*(?:static\s+inline\s+)?CustomOptional<.+?>\s+(\w+)\b.*;\s*(//.*)?$')
up_nomes = {m.group(1) for m in map(campo.match, uh.splitlines()) if m}
novos, linhas = [], []
for l in ah.splitlines():
    m = campo.match(l)
    if m and m.group(1) not in up_nomes and m.group(1) not in novos:
        novos.append(m.group(1)); linhas.append(l.rstrip())
print('campos novos:', len(novos))

# Config.h: antes do fechamento da classe Config (última linha "};" antes de "Config* Instance" ou do fim)
i = uh.find('CustomOptional<bool> UseFfxInputs')
assert i > 0
ini_linha = uh.rfind('\n', 0, i) + 1
bloco_h = '    // ---- ' + MARCA + ' chaves do núcleo neural do AMDNR (GPL-3.0, 3zwr1) ----\n' + '\n'.join(linhas) + '\n    // ---- fim ' + MARCA + ' ----\n'
uh = uh[:ini_linha] + bloco_h + uh[ini_linha:]

nomes = set(novos)
le = [l.rstrip() for l in ac.splitlines() if (m := re.match(r'^\s*(\w+)\.set_from_config\(.*\);\s*$', l)) and m.group(1) in nomes]
grava = [l.rstrip() for l in ac.splitlines() if re.match(r'^\s*ini\.SetValue\(.*\);\s*$', l) and (m := re.search(r'Instance\(\)->(\w+)', l)) and m.group(1) in nomes]
print('leituras:', len(le), 'gravacoes:', len(grava))
a = uc.find('EnableFfxInputs.set_from_config(')
a = uc.find('\n', a) + 1
uc = uc[:a] + '            // ' + MARCA + '\n' + '\n'.join(le) + '\n' + uc[a:]
b = uc.find('ini.SetValue("Inputs", "EnableFfxInputs"')
b = uc.find('\n', b) + 1
uc = uc[:b] + '        // ' + MARCA + '\n' + '\n'.join(grava) + '\n' + uc[b:]
(UP / 'Config.h').write_text(uh, encoding='utf-8')
(UP / 'Config.cpp').write_text(uc, encoding='utf-8')
lidos = {re.match(r'^\s*(\w+)\.', l).group(1) for l in le}
print('campos sem leitura simples:', sorted(nomes - lidos)[:40])
