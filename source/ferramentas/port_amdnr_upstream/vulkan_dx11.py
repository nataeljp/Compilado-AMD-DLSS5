"""Rede também em jogos Vulkan e DX11: as pontes "com DX12" do OptiScaler (IFeature_VkwDx12 / IFeature_Dx11wDx12) rodam o
upscaler numa lista e fila DX12 próprias. O núcleo precisa dessa fila (sem ela, procura a fila do jogo, que em Vulkan/DX11
não existe, e espera para sempre). Como o AMDNR: a ponte publica a fila em IFeature_Dx12::NrQueue em volta do Evaluate e
avisa o núcleo (ListDiscarded) quando fecha uma lista sem executar. Idempotente."""
from pathlib import Path

U = Path(r'C:\dlss5\terceiros\optiscaler\OptiScaler\upscalers')


def troca(rel, velho, novo):
    p = U / rel
    t = p.read_text(encoding='utf-8-sig', errors='replace')
    if novo in t:
        return
    assert t.count(velho) == 1, f'{rel}: {t.count(velho)}x {velho[:70]!r}'
    p.write_text(t.replace(velho, novo), encoding='utf-8')
    print('ok', rel)


troca('IFeature_Dx12.h', '    static inline std::unique_ptr<Menu_Dx12> Imgui = nullptr;',
      '    static inline std::unique_ptr<Menu_Dx12> Imgui = nullptr;\n'
      '    // [lmxxf2] fila DX12 das pontes Vulkan/DX11 para a rede; nula = jogo DX12 (o núcleo usa a fila do jogo)\n'
      '    static inline ID3D12CommandQueue* NrQueue = nullptr;')
troca('IFeature_Dx12.cpp', 'DlssNr::AmdBridge::Before(InCommandList, InParameters, nullptr);',
      'DlssNr::AmdBridge::Before(InCommandList, InParameters, NrQueue);')

troca('IFeature_VkwDx12.cpp', '        dx12EvalResult = dx12Feature->Evaluate(cmdList, InParameters);',
      '        IFeature_Dx12::NrQueue = Dx12CommandQueue; // [lmxxf2] a rede roda na fila DX12 da ponte\n'
      '        dx12EvalResult = dx12Feature->Evaluate(cmdList, InParameters);\n'
      '        IFeature_Dx12::NrQueue = nullptr;')
troca('IFeature_VkwDx12.cpp', '    if (Dx12CommandList[InFrame] != nullptr)\n        Dx12CommandList[InFrame]->Close();',
      '    if (Dx12CommandList[InFrame] != nullptr)\n    {\n        Dx12CommandList[InFrame]->Close();\n'
      '        DlssNr::AmdBridge::ListDiscarded(Dx12CommandList[InFrame]); // [lmxxf2] fechada sem executar\n    }')
troca('IFeature_Dx11wDx12.cpp', '    dx12EvalResult = dx12Feature->Evaluate(cmdList, InParameters);',
      '    IFeature_Dx12::NrQueue = Dx12CommandQueue; // [lmxxf2] a rede roda na fila DX12 da ponte\n'
      '    dx12EvalResult = dx12Feature->Evaluate(cmdList, InParameters);\n'
      '    IFeature_Dx12::NrQueue = nullptr;')
troca('IFeature_Dx11wDx12.cpp', '    if (dx12EvalResult)\n    {\n        ID3D12CommandList* ppCommandLists[] = { cmdList };',
      '    // [lmxxf2] fechada e não executada: o que a rede gravou nela nunca roda; avisa o núcleo para não esperar\n'
      '    if (commandListRecording && !dx12EvalResult)\n        DlssNr::AmdBridge::ListDiscarded(cmdList);\n\n'
      '    if (dx12EvalResult)\n    {\n        ID3D12CommandList* ppCommandLists[] = { cmdList };')
for rel in ['IFeature_VkwDx12.cpp', 'IFeature_Dx11wDx12.cpp']:
    p = U / rel
    t = p.read_text(encoding='utf-8-sig', errors='replace')
    if 'dlssnr/amd/AmdBridge.h' not in t:
        i = t.find('#include', t.find('#include') + 1)
        t = t[:i] + '#include "dlssnr/amd/AmdBridge.h" // [lmxxf2]\n' + t[i:]
        p.write_text(t, encoding='utf-8')
        print('include', rel)
