# Sequencia sintetica com movimento EXATO conhecido, no formato de gravacao do overlay (--reproduz):
# a foto 4K da NVIDIA vista por uma camera 1920x1080 que faz panoramica (subpixel), acelera e da zoom leve.
# Grava tambem transf.json: para cada quadro, a afim que leva pixel do quadro -> pixel da foto 4K.
# uso: python gera_sintetico.py <foto> <pasta> <quadros>
import json, sys, math
from pathlib import Path
import cv2
import numpy as np
foto, pasta, N = sys.argv[1], Path(sys.argv[2]), int(sys.argv[3])
pasta.mkdir(parents=True, exist_ok=True)
s = cv2.imread(foto, cv2.IMREAD_COLOR)[..., ::-1].astype(np.float32) / 255
lin = np.where(s <= 0.04045, s / 12.92, ((s + 0.055) / 1.055) ** 2.4).astype(np.float32)
W, H = 1920, 1080
transf = []
cache = {}
with open(pasta / 'meta.jsonl', 'w') as m:
    m.write(json.dumps({'rede_w': W, 'rede_h': H, 'janela_w': W, 'janela_h': H, 'hdr': 0, 'escala': 1.0,
                        'arquivos': {'orig': 'RGBA16F'}, 'quadros': N}) + '\n')
    for i in range(N):
        t = i / (N - 1)
        # zoom 1.9..1.7 (a foto 4K reduzida perto de 1080), pan com velocidade variando de ~3 a ~18 px/quadro
        z = 1.9 - 0.2 * t
        cx = 1920 + 700 * math.sin(2.2 * t * math.pi) * t
        cy = 1080 + 250 * math.sin(1.3 * t * math.pi)
        # pixel do quadro (x,y) -> foto: X = cx + (x - W/2 + 0.5) * z - 0.5
        A = np.array([[z, 0, cx - (W / 2 - 0.5) * z - 0.5], [0, z, cy - (H / 2 - 0.5) * z - 0.5]], np.float64)
        # pre-filtro (anti-serrilhado, como o TAA/DLSS do jogo): gaussiana na escala da reducao antes de amostrar
        if z not in cache: cache.clear(); cache[z] = cv2.GaussianBlur(lin, (0, 0), 0.5 * z)
        q = cv2.warpAffine(cache[z], A, (W, H), flags=cv2.INTER_CUBIC | cv2.WARP_INVERSE_MAP, borderMode=cv2.BORDER_REFLECT)
        rgba = np.concatenate([np.maximum(q, 0), np.ones((H, W, 1), np.float32)], -1).astype(np.float16)
        rgba.tofile(pasta / f'{i:04d}_orig.raw')
        transf.append(A.tolist())
        m.write(json.dumps({'q': i, 'rede': 1, 'mov': 1, 't_ms': i * 16.7}) + '\n')
json.dump(transf, open(pasta / 'transf.json', 'w'))
v = [math.hypot(transf[i][0][2] - transf[i - 1][0][2], transf[i][1][2] - transf[i - 1][1][2]) / transf[i][0][0] for i in range(1, N)]
print('quadros', N, 'velocidade px/quadro: min %.1f mediana %.1f max %.1f' % (min(v), float(np.median(v)), max(v)))
