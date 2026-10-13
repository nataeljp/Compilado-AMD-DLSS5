"""Controles que precisam agir de verdade no fork lmxxf2:
1) "Ligar a rede" ([DlssNr] Enabled) passa a decidir se o Evaluate chama a rede (o núcleo publicado só checava arquivos);
   religar zera o histórico (sem aplicar edição velha).
2) Atalho [DlssNr] DlssNrToggleKey (padrão Home) pelo mesmo caminho dos atalhos do OptiScaler (funciona com o menu
   fechado) + linha em Menu > Keybinds.
3) Estilo da rede lmxxf: [DlssNr] AmdRuntimeStyle -1 (auto) = 0 (visual NVIDIA, como o AMDNR 0.3.5.3), 0/1/2 = estilo;
   vai como control_style (escala do canal de estilo, 1/128 embutido).
4) Menu: status real (rodando / iniciando / desligada), NoteToggle no clique. Idempotente."""
from pathlib import Path

U = Path(r'C:\dlss5\terceiros\optiscaler\OptiScaler')


def troca(rel, velho, novo, marca=None):
    p = U / rel
    t = p.read_text(encoding='utf-8-sig', errors='replace')
    if (marca or novo) in t:
        return
    n = t.count(velho)
    assert n == 1, f'{rel}: {n}x {velho[:80]!r}'
    p.write_text(t.replace(velho, novo), encoding='utf-8')
    print('ok', rel, (marca or novo)[:40].replace('\n', ' '))


# 1) Evaluate
troca('upscalers/IFeature_Dx12.cpp', '    DlssNr::AmdBridge::Before(InCommandList, InParameters, NrQueue);',
      '    // [lmxxf2] só com "Ligar a rede" marcado; ao religar, o histórico zera (sem aplicar edição velha)\n'
      '    {\n        static bool nrAntes = false;\n        const bool nrAgora = Config::Instance()->DlssNrEnabled.value_or_default();\n'
      '        if (nrAgora && !nrAntes)\n            DlssNr::AmdBridge::InvalidateHistory();\n        nrAntes = nrAgora;\n'
      '        if (nrAgora)\n            DlssNr::AmdBridge::Before(InCommandList, InParameters, NrQueue);\n    }',
      'static bool nrAntes')

# 2) atalho
M = 'menu/menu_common.cpp'
troca(M, 'static bool inputFG = false;\n', 'static bool inputFG = false;\nstatic bool inputNr = false; // [lmxxf2] atalho liga/desliga a rede\n', 'static bool inputNr')
troca(M, '        CheckShortcut(config->FGShortcutKey.value_or_default(), inputFG, "Menu key pressed, will be switching FG mode");\n',
      '        CheckShortcut(config->FGShortcutKey.value_or_default(), inputFG, "Menu key pressed, will be switching FG mode");\n'
      '        CheckShortcut(config->DlssNrToggleKey.value_or_default(), inputNr, "Menu key pressed, will be switching DLSS5"); // [lmxxf2]\n',
      'inputNr, "Menu key pressed')
troca(M, '    inputFG = vKey == Config::Instance()->FGShortcutKey.value_or_default();\n',
      '    inputFG = vKey == Config::Instance()->FGShortcutKey.value_or_default();\n'
      '    inputNr = vKey == Config::Instance()->DlssNrToggleKey.value_or_default(); // [lmxxf2]\n', 'inputNr = vKey ==')
troca(M, '    // Handle Inputs\n    {\n        if (inputFG)\n',
      '    // Handle Inputs\n    {\n'
      '        // [lmxxf2] atalho da rede (padrão Home): inverte [DlssNr] Enabled, com o menu aberto ou fechado\n'
      '        if (inputNr)\n        {\n            inputNr = false;\n            const bool on = !config->DlssNrEnabled.value_or_default();\n'
      '            config->DlssNrEnabled = on;\n            DlssNr::AmdBridge::NoteToggle(on, "key");\n        }\n\n'
      '        if (inputFG)\n', 'atalho da rede (padrão Home)')
troca(M, '        fgEnable.Render(config->FGShortcutKey);',
      '        fgEnable.Render(config->FGShortcutKey);\n\n'
      '        static auto nrEnable = Keybind("DLSS5 (rede) liga/desliga", 41); // [lmxxf2]\n'
      '        nrEnable.Render(config->DlssNrToggleKey);', 'nrEnable.Render(')

# 3) estilo -> control_style
troca('dlssnr/amd/NrControls.h', 'inline NrControls ResolveNrControls(bool autoMask, float structure, float skin, float tone = 1.f) // [lmxxf2] tone',
      'inline NrControls ResolveNrControls(bool autoMask, float structure, float skin, float tone = 1.f, float style = 1.f) // [lmxxf2] tone, style')
troca('dlssnr/amd/NrControls.h', '    c.tone = NrControlStrength(tone); // [lmxxf2] canal 7 (tom) ligado ao [DlssNr] LocalTone\n',
      '    c.tone = NrControlStrength(tone); // [lmxxf2] canal 7 (tom) ligado ao [DlssNr] LocalTone\n'
      '    c.style = std::isfinite(style) ? std::clamp(style, 0.f, 4.f) : 1.f; // [lmxxf2] canal 6: estilo (0 = visual NVIDIA)\n', 'c.style = std::isfinite(style)')
troca('dlssnr/lmxxf/LmxxfBackend.cpp', 'AmdPreSr::ResolveNrControls(cfg.autoMask, cfg.structure, cfg.skin, cfg.netTone); // [lmxxf2] tom',
      'AmdPreSr::ResolveNrControls(cfg.autoMask, cfg.structure, cfg.skin, cfg.netTone,\n'
      '                                                  float(cfg.runtimeStyle < 0 ? 0 : cfg.runtimeStyle)); // [lmxxf2] tom, estilo (auto = 0)')

# 4) menu
I = 'menu/lmxxf2_menu.inl'
troca(I, '    Check("Ligar a rede", c->DlssNrEnabled, "Liga/desliga o DLSS5 neural rendering antes do upscaler.");\n'
         '    ImGui::SameLine();\n'
         '    ImGui::TextDisabled(DlssNr::AmdBridge::BackendActive() ? "(rodando)" : "(parada)");',
      '    bool on = c->DlssNrEnabled.value_or_default();\n'
      '    if (ImGui::Checkbox("Ligar a rede", &on))\n    {\n        c->DlssNrEnabled = on;\n        DlssNr::AmdBridge::NoteToggle(on, "menu");\n    }\n'
      '    Dica("Liga/desliga o DLSS5 antes do upscaler. Atalho: Home (troca em Menu > Keybinds).");\n'
      '    ImGui::SameLine();\n'
      '    ImGui::TextDisabled(!on ? "(desligada)" : DlssNr::AmdBridge::BackendActive() ? "(rodando)" : "(iniciando)");',
      'NoteToggle(on, "menu")')
troca(I, '    if (ImGui::Combo("Estilo", &estilo, "Automatico (NVIDIA)\\0Padrao\\0Natural\\0Cinematico\\0\\0"))',
      '    if (ImGui::Combo("Estilo", &estilo, "Automatico (padrao NVIDIA)\\0Padrao NVIDIA (0)\\0Natural (1)\\0Cinematico (2)\\0\\0"))')
troca(I, '    ImGui::Text("Rede: %s", DlssNr::AmdBridge::BackendActive() ? "rodando" : "parada");',
      '    const bool ligada = Config::Instance()->DlssNrEnabled.value_or_default();\n'
      '    ImGui::Text("Rede: %s", !ligada ? "desligada" : DlssNr::AmdBridge::BackendActive() ? "rodando" : "iniciando");')
