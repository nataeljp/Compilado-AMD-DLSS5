"""Tom da rede ligado de verdade (o núcleo do AMDNR fixava o canal 7 em 1; medido: é o canal mais forte e o mais
concentrado no rosto), faixas 0..4 para Tom/Estrutura/Pele (o núcleo aceita [0,4]; 0..2 parecia sem efeito) e Frame Gen
visível no menu. Idempotente."""
from pathlib import Path

U = Path(r'C:\dlss5\terceiros\optiscaler\OptiScaler')


def troca(rel, velho, novo, marca=None):
    p = U / rel
    t = p.read_text(encoding='utf-8-sig', errors='replace')
    if (marca or novo) in t:
        return
    assert t.count(velho) == 1, f'{rel}: {t.count(velho)}x {velho[:70]!r}'
    p.write_text(t.replace(velho, novo), encoding='utf-8')
    print('ok', rel)


N = 'dlssnr/amd/NrControls.h'
troca(N, 'inline NrControls ResolveNrControls(bool autoMask, float structure, float skin)',
      'inline NrControls ResolveNrControls(bool autoMask, float structure, float skin, float tone = 1.f) // [lmxxf2] tone')
p = U / N
t = p.read_text(encoding='utf-8-sig', errors='replace')
if 'c.tone = NrControlStrength(tone)' not in t:
    i = t.find('inline NrControls ResolveNrControls(')
    j = t.find('{', i)
    j = t.find('\n', j) + 1
    # declara c no início? procura a primeira linha com "NrControls c" depois da assinatura
    k = t.find('NrControls c', i)
    k = t.find('\n', k) + 1
    t = t[:k] + '    c.tone = NrControlStrength(tone); // [lmxxf2] canal 7 (tom) ligado ao [DlssNr] LocalTone\n' + t[k:]
    p.write_text(t, encoding='utf-8')
    print('ok NrControls tone')

troca('dlssnr/amd/AmdPreSr.h', '    float tone = 0, structure = 1, skin = 1;',
      '    float tone = 0, structure = 1, skin = 1;\n    float netTone = 1; // [lmxxf2] canal de tom da rede lmxxf ([DlssNr] LocalTone)')
troca('dlssnr/amd/AmdBridge.cpp', '    s.skin = cfg.DlssNrSkinStructure.value_or_default();',
      '    s.skin = cfg.DlssNrSkinStructure.value_or_default();\n    s.netTone = cfg.DlssNrLocalTone.value_or_default(); // [lmxxf2]')
troca('dlssnr/lmxxf/LmxxfBackend.cpp', 'AmdPreSr::ResolveNrControls(cfg.autoMask, cfg.structure, cfg.skin);',
      'AmdPreSr::ResolveNrControls(cfg.autoMask, cfg.structure, cfg.skin, cfg.netTone); // [lmxxf2] tom')

I = 'menu/lmxxf2_menu.inl'
troca(I, 'SliderF("Tom", c->DlssNrLocalTone, 0.f, 2.f, "Força do ajuste de tom local da rede.");',
      'SliderF("Tom", c->DlssNrLocalTone, 0.f, 4.f, "Canal de tom da rede (o mais forte; age mais em rostos). 1 = original.");')
troca(I, 'SliderF("Estrutura", c->DlssNrLocalStructure, 0.f, 2.f, "Força do ajuste de estrutura (forma, relevo) da rede.");',
      'SliderF("Estrutura", c->DlssNrLocalStructure, 0.f, 4.f, "Estrutura do cenário (com a máscara ligada) ou da imagem toda (desligada). 1 = original.");')
troca(I, 'if (!peleAuto && ImGui::SliderFloat("Estrutura da pele", &pele, 0.f, 2.f, "%.2f"))',
      'if (!peleAuto && ImGui::SliderFloat("Estrutura da pele", &pele, 0.f, 4.f, "%.2f"))')

M = 'menu/menu_common.cpp'
troca(M, '        { "MAIN", "Frame Gen", "Frame generation source, output and pacing.", nullptr, false,\n'
         '          [](RenderMenuContext& c) { return c.config->Lmxxf2MenuShowAll.value_or_default(); } }, // [lmxxf2] oculta',
      '        { "MAIN", "Frame Gen", "Frame generation source, output and pacing." }, // [lmxxf2] visível')
troca(I, '"Desligado esconde as abas que este produto não usa (Frame Gen, Textures).");',
      '"Desligado esconde a aba Textures (Frame Gen fica sempre visível).");')
