"""(2) Iluminação RenoDX no fork lmxxf2: chaves [Lighting], LightSettings, preenchimento no AmdBridge, construtor do
Pass a partir de DXIL pronto e a etapa logo depois do Appearance filter no LmxxfBackend. Idempotente."""
from pathlib import Path

U = Path(r'C:\dlss5\terceiros\optiscaler\OptiScaler')

# (nome do campo, tipo, padrão, chave no ini, leitor, gravador)
CAMPOS = [
    ('Lmxxf2LightEnabled', 'bool', 'false', 'Enabled', 'readBool', 'GetBoolValue'),
    ('Lmxxf2LightToneMapper', 'int', '0', 'ToneMapper', 'readInt', 'GetIntValue'),
    ('Lmxxf2LightExposure', 'float', '1.0f', 'Exposure', 'readFloat', 'GetFloatValue'),
    ('Lmxxf2LightHighlights', 'float', '1.0f', 'Highlights', 'readFloat', 'GetFloatValue'),
    ('Lmxxf2LightShadows', 'float', '1.0f', 'Shadows', 'readFloat', 'GetFloatValue'),
    ('Lmxxf2LightContrast', 'float', '1.0f', 'Contrast', 'readFloat', 'GetFloatValue'),
    ('Lmxxf2LightSaturation', 'float', '1.0f', 'Saturation', 'readFloat', 'GetFloatValue'),
    ('Lmxxf2LightDechroma', 'float', '0.5f', 'Dechroma', 'readFloat', 'GetFloatValue'),
    ('Lmxxf2LightFlare', 'float', '0.0f', 'Flare', 'readFloat', 'GetFloatValue'),
    ('Lmxxf2LightBlowout', 'float', '0.0f', 'Blowout', 'readFloat', 'GetFloatValue'),
    ('Lmxxf2LightHueStrength', 'float', '1.0f', 'HueStrength', 'readFloat', 'GetFloatValue'),
    ('Lmxxf2LightPeakNits', 'float', '1000.0f', 'PeakNits', 'readFloat', 'GetFloatValue'),
    ('Lmxxf2LightGameNits', 'float', '203.0f', 'GameNits', 'readFloat', 'GetFloatValue'),
    ('Lmxxf2LightMidGrayNits', 'float', '18.0f', 'MidGrayNits', 'readFloat', 'GetFloatValue'),
    ('Lmxxf2LightGamma', 'float', '0.0f', 'GammaCorrection', 'readFloat', 'GetFloatValue'),
    ('Lmxxf2LightMix', 'float', '1.0f', 'Mix', 'readFloat', 'GetFloatValue'),
]


def ler(rel):
    return (U / rel).read_text(encoding='utf-8-sig', errors='replace')


def grava(rel, t):
    (U / rel).write_text(t, encoding='utf-8')


def insere(rel, ancora, texto, marca):
    t = ler(rel)
    if marca in t:
        return
    assert t.count(ancora) == 1, f'{rel}: ancora {t.count(ancora)}x: {ancora[:60]!r}'
    i = t.find(ancora) + len(ancora)
    grava(rel, t[:i] + texto + t[i:])
    print('ok', rel, marca)


# Config
insere('Config.h', 'CustomOptional<int> AmdLookStrength { 1 }; // [lmxxf2] força do filtro: 1x..4x',
       '\n    // [lmxxf2-luz] iluminação RenoDX ([Lighting])\n' +
       '\n'.join(f'    CustomOptional<{t}> {n} {{ {d} }};' for n, t, d, *_ in CAMPOS), '[lmxxf2-luz]')
insere('Config.cpp', 'AmdLookStrength.set_from_config(readInt("AmdLook", "Strength")); // [lmxxf2]',
       '\n            // [lmxxf2-luz]\n' + '\n'.join(f'            {n}.set_from_config({r}("Lighting", "{k}"));' for n, _, _, k, r, _ in CAMPOS),
       '// [lmxxf2-luz]\n            Lmxxf2LightEnabled.set_from_config')
insere('Config.cpp', 'ini.SetValue("AmdLook", "Strength", GetIntValue(Instance()->AmdLookStrength.value_for_config()).c_str()); // [lmxxf2]',
       '\n        // [lmxxf2-luz-grava]\n' + '\n'.join(f'        ini.SetValue("Lighting", "{k}", {g}(Instance()->{n}.value_for_config()).c_str());' for n, _, _, k, _, g in CAMPOS),
       '[lmxxf2-luz-grava]')

# Settings
insere('dlssnr/amd/AmdPreSr.h', 'struct RtgiSettings {',
       '', '[lmxxf2-luz]')  # só valida a âncora
t = ler('dlssnr/amd/AmdPreSr.h')
if 'LightSettings' not in t:
    t = t.replace('struct RtgiSettings {',
                  '// [lmxxf2-luz] iluminação RenoDX depois do Appearance filter\nstruct LightSettings\n{\n    bool enabled = false;\n    UINT toneMapper = 0;\n'
                  '    float exposure = 1, highlights = 1, shadows = 1, contrast = 1, saturation = 1;\n'
                  '    float dechroma = .5f, flare = 0, blowout = 0, hueStrength = 1;\n'
                  '    float peakNits = 1000, gameNits = 203, midGrayNits = 18, gamma = 0, mix = 1;\n};\nstruct RtgiSettings {', 1)
    t = t.replace('    LookSettings look;', '    LookSettings look;\n    LightSettings light; // [lmxxf2-luz]', 1)
    grava('dlssnr/amd/AmdPreSr.h', t)
    print('ok AmdPreSr.h LightSettings')

insere('dlssnr/amd/AmdBridge.cpp', 's.look.highlightCompression = cfg.AmdLookHighlightCompression.value_or_default();',
       '\n    // [lmxxf2-luz]\n'
       '    s.light.enabled = cfg.Lmxxf2LightEnabled.value_or_default();\n'
       '    s.light.toneMapper = UINT(std::clamp(cfg.Lmxxf2LightToneMapper.value_or_default(), 0, 2));\n'
       '    s.light.exposure = cfg.Lmxxf2LightExposure.value_or_default(); s.light.highlights = cfg.Lmxxf2LightHighlights.value_or_default();\n'
       '    s.light.shadows = cfg.Lmxxf2LightShadows.value_or_default(); s.light.contrast = cfg.Lmxxf2LightContrast.value_or_default();\n'
       '    s.light.saturation = cfg.Lmxxf2LightSaturation.value_or_default(); s.light.dechroma = cfg.Lmxxf2LightDechroma.value_or_default();\n'
       '    s.light.flare = cfg.Lmxxf2LightFlare.value_or_default(); s.light.blowout = cfg.Lmxxf2LightBlowout.value_or_default();\n'
       '    s.light.hueStrength = cfg.Lmxxf2LightHueStrength.value_or_default(); s.light.peakNits = cfg.Lmxxf2LightPeakNits.value_or_default();\n'
       '    s.light.gameNits = cfg.Lmxxf2LightGameNits.value_or_default(); s.light.midGrayNits = cfg.Lmxxf2LightMidGrayNits.value_or_default();\n'
       '    s.light.gamma = cfg.Lmxxf2LightGamma.value_or_default(); s.light.mix = cfg.Lmxxf2LightMix.value_or_default();',
       '[lmxxf2-luz]')

# LmxxfBackend: include do DXIL, construtor por bytecode, membros, criação e a etapa
B = 'dlssnr/lmxxf/LmxxfBackend.cpp'
insere(B, '#include "../amd/AmdLookShader.h"', '\n#include "../lmxxf2/LightingDxil.h" // [lmxxf2-luz] RenoDX compilado com DXC', '[lmxxf2-luz] RenoDX')
insere(B, '    D3D12_DESCRIPTOR_HEAP_DESC hd {};\n        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = kRegions * perRegion;\n        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;\n        Check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "descriptor heap");\n        stride = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);\n    }',
       '\n    // [lmxxf2-luz] o mesmo passe a partir de um shader já compilado (DXIL do DXC), sem o compilador do sistema\n'
       '    struct Bytecode { const void* data; size_t size; };\n'
       '    Pass(ID3D12Device* d, Bytecode cs, UINT srvs, UINT constants)\n        : device(d), srvCount(srvs), perRegion(srvs + 1), constantCount(constants)\n    {\n'
       '        D3D12_DESCRIPTOR_RANGE ranges[2] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, srvs, 0, 0, 0 },\n'
       '                                             { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, srvs } };\n'
       '        D3D12_ROOT_PARAMETER params[2] {};\n        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;\n        params[0].DescriptorTable = { 2, ranges };\n'
       '        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;\n        params[1].Constants = { 0, 0, constants };\n'
       '        D3D12_ROOT_SIGNATURE_DESC rd {};\n        rd.NumParameters = 2; rd.pParameters = params;\n        ComPtr<ID3DBlob> b, e;\n'
       '        Check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &b, &e), "root signature");\n'
       '        Check(d->CreateRootSignature(0, b->GetBufferPointer(), b->GetBufferSize(), IID_PPV_ARGS(&root)), "root signature");\n'
       '        D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};\n        pd.pRootSignature = root.Get(); pd.CS = { cs.data, cs.size };\n'
       '        Check(d->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)), "pipeline");\n'
       '        D3D12_DESCRIPTOR_HEAP_DESC hd {};\n        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = kRegions * perRegion;\n'
       '        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;\n        Check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "descriptor heap");\n'
       '        stride = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);\n    }',
       'Bytecode cs, UINT srvs')
insere(B, 'std::unique_ptr<Pass> exposurePass, blitColour, blitMotion, motionChain, residual, lookPass, blitDepth;',
       '\n    std::unique_ptr<Pass> lightPass; // [lmxxf2-luz]', 'std::unique_ptr<Pass> lightPass;')
insere(B, 'ComPtr<ID3D12Resource> lookOut; // the appearance filter\'s output, render size',
       '\n    ComPtr<ID3D12Resource> lightOut; // [lmxxf2-luz] a saída da iluminação, tamanho de render', 'ComPtr<ID3D12Resource> lightOut;')
insere(B, '        lookPass = std::make_unique<Pass>(device.Get(), AmdLookShader, sizeof(AmdLookShader), "lmxxf appearance", 1u, nullptr, 24u);',
       '\n        lightPass = std::make_unique<Pass>(device.Get(), Pass::Bytecode { kLmxxf2LightingDxil, sizeof(kLmxxf2LightingDxil) }, 1u, 20u); // [lmxxf2-luz]',
       'lightPass = std::make_unique<Pass>')
insere(B, '        Bury(lookOut);', ' Bury(lightOut); // [lmxxf2-luz]', 'Bury(lightOut)')
insere(B, '        lookOut = Texture(device.Get(), w, h, kFp16);', '\n        lightOut = Texture(device.Get(), w, h, kFp16); // [lmxxf2-luz]', 'lightOut = Texture(')
insere(B, '        lookPass->RunConstants(cmd, srv, fmts, lookOut.Get(), kFp16, w, h, &c, 24u);\n        Barrier(cmd, lookOut.Get(), kUav, kSrv);\n        out = lookOut.Get();\n    }',
       '\n    // 7b. [lmxxf2-luz] Iluminação RenoDX sobre o resultado (depois do Appearance filter), na mesma escala linear.\n'
       '    const auto& light = cfg.light;\n'
       '    if (out && light.enabled && lightPass && cfg.interleaveDebug != 5u && !rawNetwork && light.mix > 0.f)\n    {\n'
       '        auto fin = [](float v, float lo, float hi, float fb) { return std::isfinite(v) ? std::clamp(v, lo, hi) : fb; };\n'
       '        struct LightConstants\n        {\n            UINT w, h, toneMapper, hueMethod;\n'
       '            float preExposure, exposure, highlights, shadows, contrast, saturation, dechroma, flare;\n'
       '            float blowout, hueStrength, peakNits, gameNits, midGrayNits, gamma, mix, pad;\n        } lc {\n'
       '            w, h, (std::min)(light.toneMapper, 2u), 0u,\n'
       '            std::isfinite(f.preExposure) && f.preExposure > 0 ? f.preExposure : 1,\n'
       '            fin(light.exposure, 0, 8, 1), fin(light.highlights, 0, 4, 1), fin(light.shadows, 0, 4, 1), fin(light.contrast, 0, 4, 1),\n'
       '            fin(light.saturation, 0, 4, 1), fin(light.dechroma, 0, 1, .5f), fin(light.flare, 0, 1, 0),\n'
       '            fin(light.blowout, 0, 1, 0), fin(light.hueStrength, 0, 1, 1), fin(light.peakNits, 80, 10000, 1000),\n'
       '            fin(light.gameNits, 80, 1000, 203), fin(light.midGrayNits, 1, 100, 18), fin(light.gamma, 0, 2, 0), fin(light.mix, 0, 1, 1), 0.f\n        };\n'
       '        static_assert(sizeof(LightConstants) == 20 * sizeof(UINT));\n'
       '        ID3D12Resource* lsrv[1] = { out };\n        const DXGI_FORMAT lfmts[1] = { kFp16 };\n'
       '        Barrier(cmd, lightOut.Get(), kSrv, kUav);\n        lightPass->RunConstants(cmd, lsrv, lfmts, lightOut.Get(), kFp16, w, h, &lc, 20u);\n'
       '        Barrier(cmd, lightOut.Get(), kUav, kSrv);\n        out = lightOut.Get();\n    }',
       '// 7b. [lmxxf2-luz]')
