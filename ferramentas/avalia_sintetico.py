# Avalia reproducoes da sequencia sintetica (movimento exato em transf.json).
# Referencia sem ruido do quadro f = media das edicoes CRUAS da rede (rodada sincrona, rede todo quadro) dos quadros
# f-J..f+J levadas ao quadro f pelo movimento exato. Por variante (quadros >= aquecimento):
#   erro      = RMS(edicao exibida - referencia) / RMS(referencia)   [cintilacao + atraso + fantasma, menor = melhor]
#   cintil    = RMS(edicao_f - edicao_{f-1} levada a f) / RMS(referencia)
#   forca     = RMS(edicao exibida) / RMS(referencia)                [1 = mesma intensidade]
# Tudo no espaco comprimido x/(1+luma). A rede crua (sem acumulo) aparece como linha "rede crua".
# uso: python avalia_sintetico.py <sequencia> <reproducao sincrona (referencia)> <reproducao> [...]
import json, sys
from pathlib import Path
import cv2
import numpy as np
L = np.array([0.2126, 0.7152, 0.0722], np.float32)
comp = lambda x: x / (1 + np.maximum(x @ L, 0))[..., None]
seq, refp = Path(sys.argv[1]), Path(sys.argv[2])
T = [np.array(a) for a in json.load(open(seq / 'transf.json'))]
W, H, J, AQ = 1920, 1080, 5, 6
ld = lambda p, k, n: np.fromfile(p / f'{k:04d}_{n}.raw', np.float16).reshape(H, W, 4)[..., :3].astype(np.float32)
nq = len([l for l in (refp / 'meta.jsonl').read_text().splitlines() if l.strip()]) - 1
fr = lambda k: k + 1  # quadro gravado k = quadro k+1 da sequencia (a reproducao comeca a gravar no 2o)
gy, gx = np.mgrid[0:H, 0:W].astype(np.float32)

def leva(img, de, para):
    """imagem do quadro `de` levada ao quadro `para` (pelo movimento exato) + mascara de validade"""
    A = np.vstack([T[para], [0, 0, 1]]); B = np.linalg.inv(np.vstack([T[de], [0, 0, 1]]))
    M = (B @ A)[:2]
    mx = (M[0, 0] * gx + M[0, 1] * gy + M[0, 2]).astype(np.float32); my = (M[1, 0] * gx + M[1, 1] * gy + M[1, 2]).astype(np.float32)
    ok = (mx >= 0) & (mx <= W - 1) & (my >= 0) & (my <= H - 1)
    return cv2.remap(img, mx, my, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE), ok

crua = {k: ld(refp, k, 'crua') for k in range(nq)}
ref, val = {}, {}
for k in range(AQ, nq):
    s, n = np.zeros((H, W, 3), np.float32), np.zeros((H, W), np.float32)
    for j in range(max(0, k - J), min(nq, k + J + 1)):
        im, ok = (crua[j], np.ones((H, W), bool)) if j == k else leva(crua[j], fr(j), fr(k))
        s += im * ok[..., None]; n += ok
    ref[k], val[k] = s / np.maximum(n, 1)[..., None], n >= J + 1

def mede(nome, edk):
    e_, c_, f_ = [], [], []
    ant = None
    for k in range(AQ, nq):
        e = edk(k); v = val[k]; r = ref[k]
        rr = np.sqrt(np.mean(r[v] ** 2))
        e_.append(np.sqrt(np.mean((e - r)[v] ** 2)) / rr); f_.append(np.sqrt(np.mean(e[v] ** 2)) / rr)
        if ant is not None:
            pa, ok = leva(ant, fr(k - 1), fr(k)); m = v & ok
            c_.append(np.sqrt(np.mean((e - pa)[m] ** 2)) / rr)
        ant = e
    print(json.dumps({'variante': nome, 'erro': round(float(np.mean(e_)), 3), 'cintil': round(float(np.mean(c_)), 3),
                      'forca': round(float(np.mean(f_)), 3)}), flush=True)

mede('rede crua (sem acumulo)', lambda k: crua[k])
for p in [Path(a) for a in sys.argv[2:]]:
    mede(p.name, lambda k, p=p: comp(ld(p, k, 'orig') + ld(p, k, 'edicao')) - comp(ld(p, k, 'orig')))
