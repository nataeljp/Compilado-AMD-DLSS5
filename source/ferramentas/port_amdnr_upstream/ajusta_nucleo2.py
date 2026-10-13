"""Segunda leva de ajustes do ramo lmxxf2 (idempotente)."""
import re
from pathlib import Path

UP = Path(r'C:\dlss5\terceiros\optiscaler\OptiScaler')


def ler(p):
    return p.read_text(encoding='utf-8-sig', errors='replace')


def grava(p, t):
    p.write_text(t, encoding='utf-8')


# 1) junção dos pedaços do shader de estabilidade: um ")" a mais em cada emenda
p = UP / 'dlssnr/amd/TemporalStability.h'
t = ler(p)
if 'std::string(R"(' in t:
    t = t.replace('static const std::string s = std::string(R"(', 'static const std::string s = std::string() + R"(')
    t = t.replace(')")\n        + R"(', ')"\n        + R"(')
    i = t.find('static const std::string s = std::string() + R"(')
    j = t.find(')");', i)
    t = t[:j] + ')";' + t[j + 4:]
    grava(p, t)
    print('shader: emendas corrigidas')

# 2) leituras do AMDNR que dependiam de variáveis locais: recursos fora do escopo (proxy NR, denoiser, RR)
p = UP / 'Config.cpp'
t = ler(p)
for l in ['DlssNrScalingDownscaler.set_from_config(*v);', 'FfxDenoiserDebugMode.set_from_config(mode);',
          'RrDefaultSharpness.set_from_config(std::clamp(setting.value(), 0.0f, 1.0f));']:
    t = re.sub(r'^[ \t]*' + re.escape(l) + r'[ \t]*\r?\n', '', t, flags=re.M)
grava(p, t)

# 3) CustomOptional::value_for_config_ignore_default (gravação do AMDNR)
p = UP / 'Config.h'
t = ler(p)
if 'value_for_config_ignore_default' not in t.split('class Config')[0]:
    i = t.find('    constexpr std::optional<T> value_for_config()')
    assert i > 0
    t = t[:i] + ('    // [lmxxf2] do AMDNR: o valor que o ini deve guardar, mesmo quando igual ao padrão\n'
                 '    constexpr std::optional<T> value_for_config_ignore_default()\n    {\n'
                 '        if (_volatile)\n            return _configIni;\n        if (this->has_value())\n            return this->value();\n'
                 '        return std::nullopt;\n    }\n\n') + t[i:]
    grava(p, t)
    print('CustomOptional: value_for_config_ignore_default')

# 4) LogRotationNames (não publicado pelo AMDNR): só o SettleLogGate que o AmdBridge usa
p = UP / 'misc/LogRotationNames.h'
if not p.exists():
    grava(p, '#pragma once\n// [lmxxf2] substituto mínimo do misc/LogRotationNames.h do AMDNR (não publicado):\n'
          '// SettleLogGate deixa passar um texto de status só quando ele muda; "" zera.\n#include <string>\n\n'
          'namespace LogRotationNames\n{\nstruct SettleLogGate\n{\n    std::string last;\n'
          '    bool ShouldLog(const std::string& s)\n    {\n        if (s.empty())\n        {\n            last.clear();\n            return false;\n        }\n'
          '        if (s == last)\n            return false;\n        last = s;\n        return true;\n    }\n};\n}\n')
    print('LogRotationNames criado')
