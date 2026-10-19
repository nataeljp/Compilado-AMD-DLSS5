# Cintilacao da edicao exibida numa gravacao (Ctrl+Alt+G ou --reproduz), descontado o movimento:
#   mudanca  = |E_i - E_{i-1} deslocada pelo fluxo do quadro i|  (RMS), o que "pisca" de um quadro para o outro
#   granulado = a parte pixel a pixel dessa mudanca (tira a media 3x3): o "fog com poucas amostras"
#   forca    = RMS da propria edicao (uma variante que so apaga o efeito tambem "pisca" pouco)
# Mede so nos quadros com movimento, separando os que tiveram rede dos reaproveitados.
# uso: python cintilacao.py <pasta> [<pasta> ...]
import json, sys
from pathlib import Path
import cv2
import numpy as np


def mede(pasta):
    pasta = Path(pasta)
    l = [json.loads(x) for x in (pasta / 'meta.jsonl').read_text().splitlines() if x.strip()]
    cab, q = l[0], l[1:]
    W, H = cab['rede_w'], cab['rede_h']
    s = max(1, W // 1920)   # mede em ate 1920 de largura (o granulado continua visivel)
    ld = lambda i, n, c: np.fromfile(pasta / f'{i:04d}_{n}.raw', np.float16).reshape(H, W, c).astype(np.float32)
    gx, gy = np.meshgrid(np.arange(W, dtype=np.float32), np.arange(H, dtype=np.float32))
    res = {'rede': [], 'reaproveitado': []}
    ant = None
    for i in range(len(q)):
        e = ld(i, 'edicao', 4)[..., :3]
        if ant is not None and q[i]['mov']:
            v = ld(i, 'mov', 2)
            rep = cv2.remap(ant, gx + v[..., 0], gy + v[..., 1], cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
            d = (e - rep)[::s, ::s]
            viz = cv2.blur(d, (3, 3))
            k = 'rede' if q[i]['rede'] else 'reaproveitado'
            res[k].append((float(np.sqrt(np.mean(d ** 2))), float(np.sqrt(np.mean((d - viz) ** 2))),
                           float(np.sqrt(np.mean(e[::s, ::s] ** 2)))))
        ant = e
    return {k: (None if not v else {'mudanca': round(float(np.mean([x[0] for x in v])), 4),
                                     'granulado': round(float(np.mean([x[1] for x in v])), 4),
                                     'forca': round(float(np.mean([x[2] for x in v])), 4), 'quadros': len(v)})
            for k, v in res.items()}


for p in sys.argv[1:]:
    print(Path(p).name, json.dumps(mede(p), ensure_ascii=False), flush=True)
