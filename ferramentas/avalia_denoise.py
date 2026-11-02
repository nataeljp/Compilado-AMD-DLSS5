# Saida do jogo de teste contra a referencia (a mesma cena sem ruido nem filtro), depois de assentar:
#   erro     = RMS(C(saida) - C(ref)), C(x) = x / (1 + x)
#   maximo   = o maior |C(saida) - C(ref)| de cada quadro (media dos quadros), e o percentil 99,9
#   ssim     = SSIM da luminancia de C(x) (janela gaussiana 11, sigma 1,5)
#   cintil   = RMS(C(saida_i) - C(saida_{i-1}) reprojetada pelo movimento exato): o que pisca de um quadro para o outro
#   bordas   = energia do gradiente da saida nas bordas fortes da ref / a da ref (1 = detalhe intacto)
#   fantasma = erro na faixa que acabou de ser descoberta (ate 4 quadros): rastro
# Referencia: uma sequencia de gera_sintetico_buffer.py (meta.jsonl, NNNN_orig.raw) ou um cenario do JogoTeste --recorte
# (recorte.json, NNNN_saida.raw: o mesmo trecho da mesma cena limpa). Movimento e profundidade vem dela.
# uso: python avalia_denoise.py <referencia> <cenario> [<cenario>...]
import json, sys
from pathlib import Path
import cv2
import numpy as np

ref = Path(sys.argv[1])
if (ref / 'recorte.json').exists():
    m = json.loads((ref / 'recorte.json').read_text())
    W, H, MW, MH = m['w'], m['h'], m['mw'], m['mh']
    uv = (m['W'], m['H'])  # o movimento do jogo esta em uv da imagem inteira
    nomeRef = 'saida'
else:
    m = json.loads((ref / 'meta.jsonl').read_text().splitlines()[0])
    W, H = m['rede_w'], m['rede_h']
    MW, MH, uv = W, H, (1, 1)
    nomeRef = 'orig'
gy, gx = np.mgrid[0:H, 0:W].astype(np.float32)
C = lambda x: np.maximum(x, 0) / (1 + np.maximum(x, 0))
ld = lambda p: np.fromfile(p, np.float16).reshape(H, W, 4)[..., :3].astype(np.float32)


def mv(i):
    d = np.fromfile(ref / f'{i:04d}_mv.raw', np.float32).reshape(MH, MW, 2) * np.float32(uv)
    return cv2.resize(d, (W, H), interpolation=cv2.INTER_NEAREST) if (MW, MH) != (W, H) else d


def prof(i):
    d = np.fromfile(ref / f'{i:04d}_prof.raw', np.float32).reshape(MH, MW)
    return cv2.resize(d, (W, H), interpolation=cv2.INTER_NEAREST) if (MW, MH) != (W, H) else d


def ssim(a, b):
    k1, k2 = 0.01 ** 2, 0.03 ** 2
    g = lambda x: cv2.GaussianBlur(x, (11, 11), 1.5)
    ma, mb = g(a), g(b)
    va, vb, ab = g(a * a) - ma * ma, g(b * b) - mb * mb, g(a * b) - ma * mb
    return np.mean((2 * ma * mb + k1) * (2 * ab + k2) / ((ma * ma + mb * mb + k1) * (va + vb + k2)))


def mede(p):
    p = Path(p)
    ids = sorted(int(a.name[:4]) for a in p.glob('*_saida.raw'))
    ids = [i for i in ids if i >= 8]
    er, mx, p9, ss, ci, bo, fa = [], [], [], [], [], [], []
    ant = None
    for i in ids:
        s = C(ld(p / f'{i:04d}_saida.raw'))
        l = C(ld(ref / f'{i:04d}_{nomeRef}.raw'))
        d = prof(i)
        v = mv(i)
        dif = np.abs(s - l)
        er.append(np.sqrt(np.mean((s - l) ** 2)))
        mx.append(dif.max())
        p9.append(np.percentile(dif, 99.9))
        ss.append(ssim(s.mean(-1), l.mean(-1)))
        gl = np.abs(np.diff(l.mean(-1), axis=1))
        forte = gl > np.percentile(gl, 98)
        bo.append(np.abs(np.diff(s.mean(-1), axis=1))[forte].mean() / gl[forte].mean())
        if ant is not None and ant[0] == i - 1:
            ax, ay = gx + v[..., 0], gy + v[..., 1]
            r = cv2.remap(ant[1], ax, ay, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
            ok = (ax >= 2) & (ax <= W - 3) & (ay >= 2) & (ay <= H - 3)
            da = cv2.remap(prof(i - 1), ax, ay, cv2.INTER_NEAREST)
            ok &= np.abs(da - d) <= 0.15 * d  # Z invertido: maior = mais perto
            ci.append(np.sqrt(np.mean((s - r)[ok] ** 2)))
            # descoberto agora: algo mais perto cobria este ponto ate 4 quadros atras
            faixa = da > d * 1.15
            for k in range(2, 5):
                if i - k >= ids[0]:
                    faixa |= cv2.remap(prof(i - k), gx + v[..., 0] * k, gy + v[..., 1] * k, cv2.INTER_NEAREST) > d * 1.15
            if faixa.sum() > 50:
                fa.append(np.sqrt(np.mean((s - l)[faixa] ** 2)))
        ant = (i, s)
    return np.mean(er), np.mean(mx), np.mean(p9), np.mean(ss), np.mean(ci), np.mean(bo), np.mean(fa) if fa else float('nan'), len(ids)


for nome in sys.argv[2:]:
    e, x, q, ssi, c, b, f, n = mede(nome)
    print('%-22s erro %.4f  max %.3f  p99,9 %.4f  ssim %.4f  cintil %.4f  bordas %.3f  fantasma %.4f  (%d quadros)'
          % (Path(nome).name, e, x, q, ssi, c, b, f, n))
