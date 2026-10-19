# Custo do atraso da rede (fila propria): compara reproducoes da mesma gravacao (--reproduz) com --atraso N contra a
# sincrona (--atraso 0, a rede respondendo no proprio quadro, o ideal). Por quadro, depois do primeiro com resposta:
#   erro_edicao = RMS(edicao exibida - edicao sincrona) / RMS(edicao sincrona)  (0 = igual; 1 = errou o tamanho todo)
#   psnr_tela   = PSNR da imagem final na tela (8 bits) contra a sincrona
#   fantasma    = fracao dos pixels em que a diferenca na tela passa de 8/255 (manchas visiveis)
# uso: python compara_atraso.py <pasta sincrona> <pasta teste> [<pasta teste> ...]
import json, sys
from pathlib import Path
import numpy as np


def abre(pasta):
    pasta = Path(pasta)
    l = [json.loads(x) for x in (pasta / 'meta.jsonl').read_text().splitlines() if x.strip()]
    cfg = json.loads((pasta / 'config.json').read_text())
    return pasta, l[0], l[1:], cfg.get('ponte', -1) == 3


def tela(lin, filmica, hdr):
    x = np.maximum(lin, 0)
    if filmica:
        x = np.clip((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0, 1)
    elif hdr:
        m = x.max(-1, keepdims=True)
        j = np.where(m > 0.75, 0.75 + 0.25 * (1 - np.exp(-(m - 0.75) / 0.25)), m)
        x = np.where(m > 0.75, x * j / np.maximum(m, 1e-9), x)
    x = np.clip(x, 0, 1)
    return np.round(np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(x, 1 / 2.4) - 0.055) * 255)


ref = abre(sys.argv[1])
pr, cab, qr, film = ref
W, H = cab['rede_w'], cab['rede_h']
ld = lambda p, i, n: np.fromfile(p / f'{i:04d}_{n}.raw', np.float16).reshape(H, W, 4)[..., :3].astype(np.float32)
for pt in sys.argv[2:]:
    p, c2, qt, _ = abre(pt)
    n = min(len(qr), len(qt))
    ini = next((i for i in range(n) if qt[i]['rede']), n)
    ee, ps, fa = [], [], []
    for i in range(ini, n):
        o = ld(pr, i, 'orig')
        er, et = ld(pr, i, 'edicao'), ld(p, i, 'edicao')
        ee.append(np.sqrt(np.mean((et - er) ** 2)) / max(np.sqrt(np.mean(er ** 2)), 1e-9))
        tr, tt = tela(o + er, film, cab.get('hdr')), tela(o + et, film, cab.get('hdr'))
        d = np.abs(tt - tr)
        ps.append(10 * np.log10(255 ** 2 / max(np.mean(d ** 2), 1e-9)))
        fa.append(float((d.max(-1) > 8).mean()))
    print(json.dumps({'teste': Path(pt).name, 'quadros': len(ee), 'erro_edicao': round(float(np.mean(ee)), 3),
                      'psnr_tela': round(float(np.mean(ps)), 2), 'psnr_pior': round(float(np.min(ps)), 2),
                      'fantasma': round(float(np.mean(fa)), 4)}, ensure_ascii=False), flush=True)
