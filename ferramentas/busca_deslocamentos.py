# Busca medida dos modos de deslocamento de janela da rede pequena (310.9.16) no layout da grande: estagio a estagio,
# os 4 pares consecutivos do ciclo da rede grande, escolhendo o que mais aproxima a saida da referencia real da NVIDIA.
# uso: python busca_deslocamentos.py <pasta do runtime pequeno> <mapa.json> <saida.json>
import json, os, subprocess, sys
from pathlib import Path
import numpy as np
from PIL import Image

RAIZ = Path(__file__).resolve().parents[1]
ENT = RAIZ / 'referencia_nvidia/single-frame-inputs__1920x1080.png'
NV = np.asarray(Image.open(RAIZ / 'referencia_nvidia/single-frame-outputs__1920x1080_nvidia.png').convert('RGB')).astype(float)
CFG = RAIZ / 'comparacao/busca.config.json'
CFG.write_text('{ "alturaRede": 1080, "reusoVit": false, "modelo": 0, "tomLocal": 1, "protecao": 16, "estabilidade": 1 }')
ESTAGIOS = [(3, 4), (7, 8), (13, 14), (21, 22), (29, 30), (40, 41), (48, 49), (56, 57), (62, 63), (66, 69)]
PARES = [(0, 3), (3, 1), (1, 2), (2, 0)]


def roda(rt, pula, mapa):
    env = dict(os.environ, DLSS5_SHIFT_MAP=','.join(f'{b}:{m}' for b, m in mapa.items()))
    saida = RAIZ / 'comparacao/busca.rgba'
    subprocess.run([str(RAIZ / 'bin/DLSS5Overlay.exe'), '--imagem', str(ENT), '--config', str(CFG), '--quadros', '3',
                    '--grava', str(saida), '--runtime', rt, '--pula', pula], env=env, timeout=240, check=True)
    a = np.fromfile(saida, np.uint8).reshape(1080, 1920, 4)[..., :3].astype(float)
    return 10 * np.log10(255 ** 2 / np.mean((a - NV) ** 2))


def main():
    rt, mapa_json, sai = sys.argv[1], sys.argv[2], sys.argv[3]
    pula = ','.join(map(str, json.loads(Path(mapa_json).read_text())['pulados']))
    atual = {3: 0, 4: 3, 7: 0, 8: 3, 13: 0, 14: 3, 21: 0, 22: 3, 29: 0, 30: 3,
             40: 1, 41: 2, 48: 1, 49: 2, 56: 1, 57: 2, 62: 1, 63: 2, 66: 1, 69: 2}
    melhor = roda(rt, pula, atual)
    print(f'inicio (regra A): {melhor:.2f} dB', flush=True)
    for rodada in range(2):
        mudou = False
        for a, b in ESTAGIOS:
            for pa, pb in PARES:
                if (atual[a], atual[b]) == (pa, pb):
                    continue
                teste = {**atual, a: pa, b: pb}
                v = roda(rt, pula, teste)
                if v > melhor + 0.02:
                    melhor, atual, mudou = v, teste, True
                    print(f'  estagio {a},{b} -> ({pa},{pb}): {v:.2f} dB', flush=True)
        print(f'rodada {rodada + 1}: {melhor:.2f} dB', flush=True)
        if not mudou:
            break
    Path(sai).write_text(json.dumps({'psnr_nvidia': melhor, 'mapa': atual}, indent=1))
    print('mapa final:', ','.join(f'{b}:{m}' for b, m in atual.items()))


if __name__ == '__main__':
    main()
