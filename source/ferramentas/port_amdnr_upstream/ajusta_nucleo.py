"""Ajustes para o núcleo neural do AMDNR compilar no OptiScaler upstream com o VS2022 (ramo lmxxf2). Idempotente."""
import re
from pathlib import Path

UP = Path(r'C:\dlss5\terceiros\optiscaler\OptiScaler')


def ler(p):
    return p.read_text(encoding='utf-8-sig', errors='replace')


def grava(p, t):
    p.write_text(t, encoding='utf-8')


# 1) texto do shader de estabilidade temporal: 142 mil caracteres não cabem num literal do MSVC 14.44
p = UP / 'dlssnr/amd/TemporalStability.h'
t = ler(p)
m = re.search(r'inline constexpr char TemporalStabilityShader\[\] = R"\((.*?)\)";', t, re.S)
if m:
    corpo, pedacos, atual = m.group(1), [], ''
    for linha in corpo.splitlines(keepends=True):
        if len(atual) + len(linha) > 15000:
            pedacos.append(atual); atual = ''
        atual += linha
    pedacos.append(atual)
    fn = ('// [lmxxf2] o literal original passa do limite do MSVC 14.44; mesmo texto, montado em pedaços\n'
          'inline const std::string& TemporalStabilityShaderText()\n{\n    static const std::string s = std::string(R"('
          + ')")\n        + R"('.join(pedacos) + ')");\n    return s;\n}')
    t = t[:m.start()] + fn + t[m.end():]
    t = t.replace('TemporalStabilityShader, sizeof(TemporalStabilityShader)',
                  'TemporalStabilityShaderText().c_str(), TemporalStabilityShaderText().size() + 1')
    if '#include <string>' not in t:
        t = t.replace('#pragma once', '#pragma once\n#include <string>', 1)
    grava(p, t)
    print('TemporalStability: ', len(pedacos), 'pedacos')

# 2) pch nos .cpp que não o incluem
for rel in ['dlssnr/amd/AmdPreSr.cpp', 'dlssnr/amd/RtgiNative.cpp']:
    p = UP / rel
    t = ler(p)
    if not re.search(r'#include\s+[<"]pch\.h', t):
        grava(p, '#include "pch.h" // [lmxxf2]\n' + t)
        print('pch:', rel)

# 3) State: o upstream chama de hdrOutputActive
for p in (UP / 'dlssnr').rglob('*.*'):
    if p.suffix in ('.h', '.cpp'):
        t = ler(p)
        if 'isHdrActive' in t:
            grava(p, t.replace('isHdrActive', 'hdrOutputActive'))
            print('hdr:', p.name)

# 4) FrameLimit: relógio e sono em ns que o pacer do AMDNR usa
h = UP / 'misc/FrameLimit.h'
t = ler(h)
if 'now_ns' not in t:
    t = t.replace('    static void sleep(bool fgActive);',
                  '    static void sleep(bool fgActive);\n    // [lmxxf2] usados pelo pacer do núcleo neural (InterleavePacing.h)\n'
                  '    static uint64_t now_ns();\n    static void sleep_ns(int64_t ns);')
    grava(h, t)
    c = UP / 'misc/FrameLimit.cpp'
    grava(c, ler(c) + '\n// [lmxxf2]\nuint64_t FrameLimit::now_ns()\n{\n    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();\n'
          '    LARGE_INTEGER c; QueryPerformanceCounter(&c);\n    return uint64_t(c.QuadPart / f.QuadPart) * 1000000000ull + uint64_t(c.QuadPart % f.QuadPart) * 1000000000ull / uint64_t(f.QuadPart);\n}\n\n'
          'void FrameLimit::sleep_ns(int64_t ns)\n{\n    if (ns > 0)\n        combined_sleep(ns);\n}\n')
    print('FrameLimit: now_ns/sleep_ns')

# 5) ProcessNameMatch (não publicado pelo AMDNR): só a regra que o AmdBridge usa
p = UP / 'misc/ProcessNameMatch.h'
if not p.exists():
    grava(p, '#pragma once\n// [lmxxf2] substituto mínimo do misc/ProcessNameMatch.h do AMDNR (não publicado):\n'
          '// reconhece o instalador do runtime do danielblnc pelo nome (dlssnr_on_amd_setup*.exe).\n#include <string>\n#include <algorithm>\n#include <cwctype>\n\n'
          'namespace ProcessNameMatch\n{\ninline bool IsAmdSetupExe(std::wstring n)\n{\n'
          '    std::transform(n.begin(), n.end(), n.begin(), [](wchar_t c) { return (wchar_t) std::towlower(c); });\n'
          '    return n.rfind(L"dlssnr_on_amd_setup", 0) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, L".exe") == 0;\n}\n'
          'inline bool IsAmdSetupExe(const std::string& n) { return IsAmdSetupExe(std::wstring(n.begin(), n.end())); }\n}\n')
    print('ProcessNameMatch criado')
