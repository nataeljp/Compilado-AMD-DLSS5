"""(3) Menu: volta a janela/abas originais do OptiScaler e acrescenta a aba "DLSS5" (primeira, aberta por padrão) com os
nossos cartões; Frame Gen e Textures ficam ocultas a menos que [Menu] Lmxxf2ShowAll=true. Idempotente."""
from pathlib import Path

U = Path(r'C:\dlss5\terceiros\optiscaler\OptiScaler')
M = U / 'menu/menu_common.cpp'
t = M.read_text(encoding='utf-8-sig', errors='replace')


def troca(velho, novo, obrig=True):
    global t
    if novo in t:
        return
    n = t.count(velho)
    if n != 1:
        if obrig:
            raise SystemExit(f'ancora {n}x: {velho[:70]!r}')
        return
    t = t.replace(velho, novo)
    print('ok', novo.strip()[:60])


# 1) desfaz a janela própria: cabeçalho, abas e rodapé originais, título original
i = t.find('        // [lmxxf2] só a nossa janela')
if i >= 0:
    j = t.find('});', i) + 3
    t = t[:i] + ('        // Update and config notices shown above the tabs.\n        RenderMainMenuHeaderMessages(ctx);\n\n'
                 '        // Tabs on the left, tab content on the right.\n        RenderMainMenuTabs(ctx);\n\n'
                 '        // Graphs and buttons, always visible regardless of the selected tab.\n        RenderMainMenuFooter(ctx);') + t[j:]
    print('ok janela original restaurada')
troca('windowTitle = StrFmt("%s - %s %s", "lmxxf 2 (DLSS5)", state.gameExe.c_str(),',
      'windowTitle = StrFmt("%s - %s %s", "lmxxf 2 (DLSS5) / OptiScaler", state.gameExe.c_str(),')

# 2) aba DLSS5 primeiro e aberta por padrão; Frame Gen e Textures ocultas sem Lmxxf2ShowAll
troca('        { "MAIN", "Custom", "Boxes picked in Menu > Custom Tab.", RenderCustomTab },',
      '        { "MAIN", "Custom", "Boxes picked in Menu > Custom Tab.", RenderCustomTab },\n'
      '        { "MAIN", "DLSS5", "Rede lmxxf 2 (DLSS5), Appearance filter e iluminacao RenoDX." }, // [lmxxf2]')
troca('        { "MAIN", "Frame Gen", "Frame generation source, output and pacing." },',
      '        { "MAIN", "Frame Gen", "Frame generation source, output and pacing.", nullptr, false,\n'
      '          [](RenderMenuContext& c) { return c.config->Lmxxf2MenuShowAll.value_or_default(); } }, // [lmxxf2] oculta')
troca('        { "SYSTEM", "Textures", "Mipmap bias and anisotropic filtering." },',
      '        { "SYSTEM", "Textures", "Mipmap bias and anisotropic filtering.", nullptr, false,\n'
      '          [](RenderMenuContext& c) { return c.config->Lmxxf2MenuShowAll.value_or_default(); } }, // [lmxxf2] oculta')
troca('static int selectedMenuTab = 0;', 'static int selectedMenuTab = 1; // [lmxxf2] abre na aba DLSS5')

# 3) cartões da aba DLSS5
troca('    static const MenuBox boxes[] = {\n',
      '    static const MenuBox boxes[] = {\n'
      '        // [lmxxf2] aba DLSS5\n'
      '        { "lmxxf2_rede", "DLSS5", "Rede DLSS5 (lmxxf 2)",\n'
      '          [](RenderMenuContext& c) { RenderCard(c, [](RenderMenuContext& cc) { SectionTitle("Rede DLSS5 (lmxxf 2)"); Lmxxf2Menu::Rede(cc.config); }); } },\n'
      '        { "lmxxf2_filtro", "DLSS5", "Appearance filter",\n'
      '          [](RenderMenuContext& c) { RenderCard(c, [](RenderMenuContext& cc) { SectionTitle("Appearance filter"); Lmxxf2Menu::Filtro(cc.config); }); } },\n'
      '        { "lmxxf2_luz", "DLSS5", "Iluminacao (RenoDX)",\n'
      '          [](RenderMenuContext& c) { RenderCard(c, [](RenderMenuContext& cc) { SectionTitle("Iluminacao (RenoDX)"); Lmxxf2Menu::Luz(cc.config); }); } },\n'
      '        { "lmxxf2_desempenho", "DLSS5", "Desempenho",\n'
      '          [](RenderMenuContext& c) { RenderCard(c, [](RenderMenuContext&) { SectionTitle("Desempenho"); Lmxxf2Menu::Desempenho(); }); } },\n\n')
M.write_text(t, encoding='utf-8')

# 4) chave [Menu] Lmxxf2ShowAll
C = U / 'Config.h'
h = C.read_text(encoding='utf-8-sig', errors='replace')
if 'Lmxxf2MenuShowAll' not in h:
    h = h.replace('    CustomOptional<int> AmdLookStrength { 1 }; // [lmxxf2] força do filtro: 1x..4x',
                  '    CustomOptional<int> AmdLookStrength { 1 }; // [lmxxf2] força do filtro: 1x..4x\n'
                  '    CustomOptional<bool> Lmxxf2MenuShowAll { false }; // [lmxxf2] mostra as abas ocultas do OptiScaler', 1)
    C.write_text(h, encoding='utf-8')
    cpp = U / 'Config.cpp'
    c = cpp.read_text(encoding='utf-8-sig', errors='replace')
    c = c.replace('            AmdLookStrength.set_from_config(readInt("AmdLook", "Strength")); // [lmxxf2]',
                  '            AmdLookStrength.set_from_config(readInt("AmdLook", "Strength")); // [lmxxf2]\n'
                  '            Lmxxf2MenuShowAll.set_from_config(readBool("Menu", "Lmxxf2ShowAll")); // [lmxxf2]', 1)
    c = c.replace('        ini.SetValue("AmdLook", "Strength", GetIntValue(Instance()->AmdLookStrength.value_for_config()).c_str()); // [lmxxf2]',
                  '        ini.SetValue("AmdLook", "Strength", GetIntValue(Instance()->AmdLookStrength.value_for_config()).c_str()); // [lmxxf2]\n'
                  '        ini.SetValue("Menu", "Lmxxf2ShowAll", GetBoolValue(Instance()->Lmxxf2MenuShowAll.value_for_config()).c_str()); // [lmxxf2]', 1)
    cpp.write_text(c, encoding='utf-8')
    print('ok Lmxxf2ShowAll')
