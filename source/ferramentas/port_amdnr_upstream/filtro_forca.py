"""(1) Appearance filter com força 1x-4x no fork lmxxf2: chave [AmdLook] Strength (1..4), constante Strength no
shader (era 'reserved'), e a mistura final extrapola a mudança do filtro por esse fator. Idempotente."""
from pathlib import Path

U = Path(r'C:\dlss5\terceiros\optiscaler\OptiScaler')


def troca(rel, velho, novo, conta=1):
    p = U / rel
    t = p.read_text(encoding='utf-8-sig', errors='replace')
    if novo in t:
        return
    assert t.count(velho) == conta, f'{rel}: {t.count(velho)} ocorrencias de {velho[:60]!r}'
    p.write_text(t.replace(velho, novo), encoding='utf-8')
    print('ok', rel)


troca('Config.h', 'CustomOptional<uint32_t> AmdLookInspect { 0 };',
      'CustomOptional<uint32_t> AmdLookInspect { 0 };\n    CustomOptional<int> AmdLookStrength { 1 }; // [lmxxf2] força do filtro: 1x..4x')
troca('Config.cpp', 'AmdLookInspect.set_from_config(readUInt("AmdLook", "Inspect"));',
      'AmdLookInspect.set_from_config(readUInt("AmdLook", "Inspect"));\n            AmdLookStrength.set_from_config(readInt("AmdLook", "Strength")); // [lmxxf2]')
troca('Config.cpp', 'ini.SetValue("AmdLook", "Inspect", GetIntValue(Instance()->AmdLookInspect.value_for_config()).c_str());',
      'ini.SetValue("AmdLook", "Inspect", GetIntValue(Instance()->AmdLookInspect.value_for_config()).c_str());\n'
      '        ini.SetValue("AmdLook", "Strength", GetIntValue(Instance()->AmdLookStrength.value_for_config()).c_str()); // [lmxxf2]')
troca('dlssnr/amd/AmdPreSr.h', 'UINT appearance = 2, inspect = 0;', 'UINT appearance = 2, inspect = 0, strength = 1; // [lmxxf2] strength 1..4')
troca('dlssnr/amd/AmdBridge.cpp', 's.look.inspect = cfg.AmdLookInspect.value_or_default();',
      's.look.inspect = cfg.AmdLookInspect.value_or_default();\n    s.look.strength = UINT(std::clamp(cfg.AmdLookStrength.value_or_default(), 1, 4)); // [lmxxf2]')
troca('dlssnr/lmxxf/LmxxfBackend.cpp', 'look.detectSkin ? 1u : 0u, 0\n',
      'look.detectSkin ? 1u : 0u, (std::min)((std::max)(look.strength, 1u), 4u) // [lmxxf2] Strength\n')
troca('dlssnr/amd/AmdPreSr.cpp', 'f.preExposure : 1, look.detectSkin, 0\n',
      'f.preExposure : 1, look.detectSkin, (std::min)((std::max)(look.strength, 1u), 4u) // [lmxxf2] Strength\n')
troca('dlssnr/amd/AmdLookShader.h', 'uint DetectSkin; uint reserved;', 'uint DetectSkin; uint Strength;')
troca('dlssnr/amd/AmdLookShader.h', 'float3 resultColour=Mix>0?lerp(original.rgb,Decode(outc),Mix):original.rgb;',
      'float3 resultColour=Mix>0?lerp(original.rgb,Decode(saturate(lerp(s,outc,(float)clamp(Strength,1u,4u)))),Mix):original.rgb; // [lmxxf2] força 1x..4x')
