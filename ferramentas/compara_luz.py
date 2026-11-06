# O que a NVIDIA muda na luz e quanto disso a nossa saida reproduz (contra a saida REAL da nvngx_dlssnr, referencia_nvidia).
# PSNR nao serve: a saida e quase toda a entrada, entao o dB mede pixel parado e nao a luz que a rede mexe.
# Aqui tudo e medido na EDICAO (saida - entrada) em CIELAB (L* = luz percebida, a*/b* = cor), separada por escala:
#   textura  < 2 px     | detalhe 2-8 px | contraste local 8-32 px | luz local 32-128 px | luz global > 128 px
# Por banda:  captura = 1 - |nossa - nvidia|^2 / |nvidia|^2  (100% = mesma mudanca de luz; 0% = nao mexer; <0 = mexeu errado)
#             corr    = mesma forma da mudanca, independente da forca
# Tom: a mudanca media de L* e de croma por faixa de brilho da entrada (sombra -> alta luz), NVIDIA x nossa.
# Percepcao: ꟻLIP (NVIDIA, LDR, 67 ppd) e dE2000 da nossa contra a NVIDIA; a entrada sem rede entra como ancora.
# Grade: erro periodico em 8/16/32/64 px (z ~ 1 = sem grade; z >> 1 = a nossa rede tem um padrao de bloco que a NVIDIA nao tem)
# uso: python compara_luz.py <entrada.png> <nvidia.png> <nossa.rgba|png> [...]
import os, sys
import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter

SIGMAS = [2, 8, 32, 128]
BANDAS = ['textura', 'detalhe', 'contr.local', 'luz local', 'luz global']
TONS = [(0, 15), (15, 35), (35, 60), (60, 80), (80, 101)]


def carrega(p, w=0, h=0):
    if p.lower().endswith('.rgba'):
        return np.fromfile(p, np.uint8).reshape(h, w, 4)[..., :3].astype(np.float64) / 255
    return np.asarray(Image.open(p).convert('RGB')).astype(np.float64) / 255


def lab(s):
    l = np.where(s <= 0.04045, s / 12.92, ((s + 0.055) / 1.055) ** 2.4)
    xyz = l @ np.array([[0.4124564, 0.2126729, 0.0193339], [0.3575761, 0.7151522, 0.1191920], [0.1804375, 0.0721750, 0.9503041]])
    xyz /= np.array([0.95047, 1.0, 1.08883])
    d = 6 / 29
    f = np.where(xyz > d ** 3, np.cbrt(xyz), xyz / (3 * d * d) + 4 / 29)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], -1)


def de2000(a, b):
    L1, a1, b1 = a[..., 0], a[..., 1], a[..., 2]
    L2, a2, b2 = b[..., 0], b[..., 1], b[..., 2]
    C1, C2 = np.hypot(a1, b1), np.hypot(a2, b2)
    Cm = (C1 + C2) / 2
    G = 0.5 * (1 - np.sqrt(Cm ** 7 / (Cm ** 7 + 25.0 ** 7)))
    a1p, a2p = a1 * (1 + G), a2 * (1 + G)
    C1p, C2p = np.hypot(a1p, b1), np.hypot(a2p, b2)
    h1p, h2p = np.degrees(np.arctan2(b1, a1p)) % 360, np.degrees(np.arctan2(b2, a2p)) % 360
    dLp, dCp = L2 - L1, C2p - C1p
    dh = h2p - h1p
    dh = np.where(C1p * C2p == 0, 0, np.where(dh > 180, dh - 360, np.where(dh < -180, dh + 360, dh)))
    dHp = 2 * np.sqrt(C1p * C2p) * np.sin(np.radians(dh / 2))
    Lpm, Cpm = (L1 + L2) / 2, (C1p + C2p) / 2
    hs = h1p + h2p
    hpm = np.where(C1p * C2p == 0, hs, np.where(np.abs(h1p - h2p) <= 180, hs / 2, np.where(hs < 360, (hs + 360) / 2, (hs - 360) / 2)))
    T = 1 - 0.17 * np.cos(np.radians(hpm - 30)) + 0.24 * np.cos(np.radians(2 * hpm)) + 0.32 * np.cos(np.radians(3 * hpm + 6)) - 0.20 * np.cos(np.radians(4 * hpm - 63))
    dth = 30 * np.exp(-(((hpm - 275) / 25) ** 2))
    Rc = 2 * np.sqrt(Cpm ** 7 / (Cpm ** 7 + 25.0 ** 7))
    Sl = 1 + 0.015 * (Lpm - 50) ** 2 / np.sqrt(20 + (Lpm - 50) ** 2)
    Sc, Sh = 1 + 0.045 * Cpm, 1 + 0.015 * Cpm * T
    Rt = -np.sin(np.radians(2 * dth)) * Rc
    return np.sqrt((dLp / Sl) ** 2 + (dCp / Sc) ** 2 + (dHp / Sh) ** 2 + Rt * (dCp / Sc) * (dHp / Sh))


def bandas(x):
    g = [x] + [gaussian_filter(x, s, mode='reflect') for s in SIGMAS]
    return [g[i] - g[i + 1] for i in range(len(SIGMAS))] + [g[-1]]


def grade(e):
    # media do erro dobrada no periodo p; sem padrao de bloco a variancia dela e var(e)*p^2/N (z ~ 1)
    H, W = e.shape
    r = []
    for p in (8, 16, 32, 64):
        h, w = H // p * p, W // p * p
        f = e[:h, :w].reshape(h // p, p, w // p, p).mean((0, 2))
        r.append(f.var() / (e[:h, :w].var() * p * p / (h * w)))
    return r


def main():
    ent_s, nv_s = carrega(sys.argv[1]), carrega(sys.argv[2])
    H, W = ent_s.shape[:2]
    ent, nv = lab(ent_s), lab(nv_s)
    dn = nv - ent
    bn = [bandas(dn[..., k]) for k in range(3)]
    try:
        import flip_evaluator as flip
    except ImportError:
        flip = None
    fl = lambda s: flip.evaluate(nv_s.astype(np.float32), s.astype(np.float32), 'LDR', applyMagma=False)[1] if flip else float('nan')
    tom_in = ent[..., 0]
    mt = [(tom_in >= a) & (tom_in < b) for a, b in TONS]
    print(f'{W}x{H}  NVIDIA mexe: |dL*| medio {np.abs(dn[..., 0]).mean():.2f}, |da*b*| medio {np.hypot(dn[..., 1], dn[..., 2]).mean():.2f}')
    print('  energia da edicao de L* da NVIDIA por banda: ' + '  '.join(f'{n} {100 * (b ** 2).sum() / (dn[..., 0] ** 2).sum():.0f}%' for n, b in zip(BANDAS, bn[0])))
    print('  NVIDIA por tom de entrada (L*):  ' + '  '.join(f'{a}-{b}: dL {dn[..., 0][m].mean():+.2f} dC {(np.hypot(*nv[..., 1:].transpose(2, 0, 1)) - np.hypot(*ent[..., 1:].transpose(2, 0, 1)))[m].mean():+.2f}' for (a, b), m in zip(TONS, mt)))
    print(f'  entrada sem rede: FLIP {fl(ent_s):.4f}  dE2000 medio {de2000(nv, ent).mean():.3f}')
    # teto do 8 bits: a propria saida da NVIDIA arredondada de novo (duas quantizacoes independentes de 1/255 diferem assim)
    rng = np.random.default_rng(0)
    teto = np.clip(nv_s + (rng.random(nv_s.shape) - rng.random(nv_s.shape)) / 255, 0, 1)
    for p in ['(teto 8 bits)'] + sys.argv[3:]:
        us_s = teto if p.startswith('(') else carrega(p, W, H)
        us = lab(us_s)
        du = us - ent
        nome = os.path.basename(p)
        e00 = de2000(nv, us)
        print(f'== {nome}')
        print(f'  FLIP {fl(us_s):.4f}   dE2000 medio {e00.mean():.3f}  p99 {np.percentile(e00, 99):.2f}  >1: {100 * (e00 > 1).mean():.2f}%  >2: {100 * (e00 > 2).mean():.2f}%')
        for k, canal in enumerate(('L*', 'a*', 'b*')):
            bu = bandas(du[..., k])
            cols = []
            for n, x, y in zip(BANDAS, bu, bn[k]):
                en = (y ** 2).sum()
                cols.append(f'{n} {100 * (1 - ((x - y) ** 2).sum() / en):6.1f}% c{np.corrcoef(x.ravel(), y.ravel())[0, 1]:.3f}')
            print(f'  {canal} captura: ' + ' | '.join(cols))
        dc_us = np.hypot(*us[..., 1:].transpose(2, 0, 1)) - np.hypot(*ent[..., 1:].transpose(2, 0, 1))
        print('  nossa por tom:   ' + '  '.join(f'{a}-{b}: dL {du[..., 0][m].mean():+.2f} dC {dc_us[m].mean():+.2f}' for (a, b), m in zip(TONS, mt)))
        e = us[..., 0] - nv[..., 0]
        b32 = 32
        borda = np.concatenate([e[:b32].ravel(), e[-b32:].ravel(), e[:, :b32].ravel(), e[:, -b32:].ravel()])
        print(f'  erro L*: borda rms {np.sqrt((borda ** 2).mean()):.3f} x centro {np.sqrt((e[b32:-b32, b32:-b32] ** 2).mean()):.3f}   grade z (8/16/32/64 px): '
              + ' '.join(f'{z:.1f}' for z in grade(e)))


if __name__ == '__main__':
    main()
