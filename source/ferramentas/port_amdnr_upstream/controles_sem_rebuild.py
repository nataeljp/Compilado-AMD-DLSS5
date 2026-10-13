"""Controles da rede (tom/estrutura/pele/outro/estilo) sem recriar a rede.

Runtime (lmxxf): ReadWeights consulta um mapa de pesos substitutos; LmxxfNrOverrideWeight grava o substituto e tira do
cache só as entradas desse peso (o próximo quadro reempacota ~35 KB em vez de recarregar módulos e 150 MB de pesos).
Adaptador: guarda o block0-ffn.f32 original, escala as colunas e chama a função nova; runtime antigo = caminho antigo.
Idempotente."""
from pathlib import Path

L = Path(r'C:\dlss5\lmxxf')
A = Path(r'C:\dlss5\tools\lmxxf_port\adaptador\adaptador.cpp')


def troca(p, velho, novo, marca=None):
    t = p.read_text(encoding='utf-8-sig', errors='replace')
    if (marca or novo) in t:
        return
    assert t.count(velho) == 1, f'{p.name}: {t.count(velho)}x {velho[:70]!r}'
    p.write_text(t.replace(velho, novo), encoding='utf-8')
    print('ok', p.name, (marca or novo)[:50].replace('\n', ' '))


H = L / 'Development/HIP/hip_reference_network.h'
troca(H, 'inline std::vector<float> ReadWeights(const std::string&path){',
      '/* [dlssnr_rebuild] pesos substitutos por nome de arquivo (LmxxfNrOverrideWeight): controles da rede sem recriá-la */\n'
      'inline std::map<std::string,std::vector<float>>&WeightOverrides(){static std::map<std::string,std::vector<float>>m;return m;}\n'
      'inline std::mutex&WeightOverridesMutex(){static std::mutex m;return m;}\n'
      'inline std::vector<float> ReadWeights(const std::string&path){'
      '{std::lock_guard<std::mutex>g(WeightOverridesMutex());const auto base=path.substr(path.find_last_of("/\\\\")+1);'
      'auto it=WeightOverrides().find(base);if(it!=WeightOverrides().end())return it->second;}', 'WeightOverrides()')
troca(H, 'Tensor UploadWeight(const std::vector<float>&v,const std::string&key){',
      '    /* [dlssnr_rebuild] tira do cache um peso e todos os seus empacotamentos (nome@layout); o próximo uso relê (com o\n'
      '       substituto, se houver) e reempacota. Espera a fila para nenhum kernel em voo ler a memória antiga. */\n'
      '    void DropWeight(const std::string&name){api.hipStreamSynchronize(stream);\n'
      '     for(auto it=weights.begin();it!=weights.end();){if(it->first==name||it->first.rfind(name+"@",0)==0)it=weights.erase(it);else ++it;}}\n'
      'Tensor UploadWeight(const std::vector<float>&v,const std::string&key){', 'void DropWeight(')
B = L / 'Development/HIP/hip_d3d12_bridge.h'
troca(B, ' Network& DiagnosticNetwork(){return *network;}',
      ' Network& DiagnosticNetwork(){return *network;}\n'
      ' bool DropWeight(const std::string&name){if(!network)return false;network->DropWeight(name);return true;} /* [dlssnr_rebuild] */',
      'bool DropWeight(')
R = L / 'src/LmxxfNrRuntime.cpp'
troca(R, 'extern "C" int32_t LmxxfNrGetApi(uint32_t abi_version, LmxxfNrApi *out)\n',
      '/* [dlssnr_rebuild] Troca a origem de um peso (nome do arquivo, ex. "block0-ffn.f32") sem recriar a rede: o próximo\n'
      '   quadro reempacota só esse peso. data/count nulos removem o substituto. Fora da tabela da API (ABI inalterada). */\n'
      'extern "C" LMXXF_NR_EXPORT int32_t LmxxfNrOverrideWeight(void *context, const char *name, const float *data, uint64_t count)\n{\n'
      '    return Guard([&] {\n'
      '        if (!context || !name || !*name)\n            return Fail(LMXXF_NR_INVALID_ARGUMENT, "OverrideWeight: null argument");\n'
      '        {\n            std::lock_guard<std::mutex> g(WeightOverridesMutex());\n'
      '            if (data && count)\n                WeightOverrides()[name].assign(data, data + count);\n'
      '            else\n                WeightOverrides().erase(name);\n        }\n'
      '        auto *s = static_cast<Session *>(context);\n        if (s->bridge)\n            s->bridge->DropWeight(name);\n'
      '        return LMXXF_NR_OK;\n    });\n}\n\n'
      'extern "C" int32_t LmxxfNrGetApi(uint32_t abi_version, LmxxfNrApi *out)\n', 'LmxxfNrOverrideWeight(void *context')

# adaptador
troca(A, '    bool built = false;\n};', '    bool built = false;\n    std::vector<float> base0; // [lmxxf2] block0-ffn.f32 original, para os controles sem recriar a rede\n};', 'std::vector<float> base0;')
troca(A, 'static unsigned long long g_n[8];',
      'static unsigned long long g_n[8];\n'
      'static int32_t (*g_override)(void *, const char *, const float *, uint64_t); // [lmxxf2] LmxxfNrOverrideWeight do runtime\n'
      '// [lmxxf2] controles por escala das colunas da mistura do bloco 0 aplicados no lugar (sem Destroy/Create)\n'
      'static bool controlesNoLugar(Ctx *c) {\n'
      '    if (!g_override || !c->inner) return false;\n'
      '    if (c->base0.empty()) {\n'
      '        FILE *f = _wfopen((c->assets + L"\\\\block0-ffn.f32").c_str(), L"rb");\n'
      '        if (!f) return false;\n'
      '        float x; while (std::fread(&x, 4, 1, f) == 1) c->base0.push_back(x);\n'
      '        std::fclose(f);\n'
      '        if (c->base0.size() < 512) { c->base0.clear(); return false; }\n'
      '    }\n'
      '    std::vector<float> w = c->base0;\n'
      '    const int col[5] = {7, 11, 10, 14, 6};\n'
      '    for (int c32 = 0; c32 < 32; ++c32)\n        for (int k = 0; k < 5; ++k) w[c32 * 16 + col[k]] *= c->ctl[k];\n'
      '    return g_override(c->inner, "block0-ffn.f32", w.data(), w.size()) == LMXXF_NR_OK;\n}', 'static bool controlesNoLugar(')
troca(A, '    bool muda = passes != c->passes || full != c->full;\n    for (int k = 0; k < 5; ++k) muda |= ctl[k] != c->ctl[k];\n    if (muda) {\n'
         '        c->passes = passes; c->full = full; std::memcpy(c->ctl, ctl, sizeof ctl);\n        int32_t r = constroi(c);\n        if (r) return r;\n    }',
      '    const bool mudaRede = passes != c->passes || full != c->full;\n    bool mudaCtl = false;\n'
      '    for (int k = 0; k < 5; ++k) mudaCtl |= ctl[k] != c->ctl[k];\n'
      '    if (mudaRede || mudaCtl) {\n'
      '        c->passes = passes; c->full = full; std::memcpy(c->ctl, ctl, sizeof ctl);\n'
      '        // [lmxxf2] só os controles mudaram: troca o peso do bloco 0 no lugar (sem travar recriando a rede)\n'
      '        if (mudaRede || !controlesNoLugar(c)) {\n            int32_t r = constroi(c);\n            if (r) return r;\n        }\n    }',
      'if (mudaRede || !controlesNoLugar(c))')
troca(A, '        if (!get || get(1, &U) != LMXXF_NR_OK) { U.struct_size = 0; return LMXXF_NR_UNAVAILABLE; }',
      '        if (!get || get(1, &U) != LMXXF_NR_OK) { U.struct_size = 0; return LMXXF_NR_UNAVAILABLE; }\n'
      '        g_override = (int32_t (*)(void *, const char *, const float *, uint64_t))GetProcAddress(inner, "LmxxfNrOverrideWeight");',
      'GetProcAddress(inner, "LmxxfNrOverrideWeight")')
