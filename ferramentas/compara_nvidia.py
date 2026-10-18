# Compara a saida do overlay (modo imagem) com a foto da NVIDIA (DLSS 5 ligado), regiao por regiao.
# A comparacao pixel a pixel engana (as fotos da NVIDIA sao dois renders do jogo, nao o mesmo quadro com e sem a rede);
# o que vale e o "carater" de cada regiao: luz (L*), sombras/realces (p10/p90 de L*), croma (C*), matiz e detalhe.
#
# uso: python compara_nvidia.py <off.jpg> <on.jpg> <nossa.rgba|nossa.png> [<rotulo>] [--json saida.json]
import json, sys
import numpy as np
from PIL import Image
from scipy import ndimage

# regioes da foto Zorah 002 (3840x2160): x0, y0, x1, y1
REGIOES = {
    'rosto': (1820, 380, 2080, 700),
    'maos': (1760, 1440, 2160, 1600),
    'turbante': (1720, 180, 2160, 400),
    'flores': (640, 160, 1600, 440),
    'frutas': (2280, 780, 2600, 1000),
    'toalha': (0, 1080, 1280, 1360),
    'roupa': (1320, 1680, 2600, 2080),
    'dourado': (3280, 600, 3520, 1360),
    'madeira': (0, 1520, 960, 1760),
}
SELOS = [(60, 1780, 800, 1960), (3150, 1930, 3920, 2110)]  # "DLSS 5 Off" / "DLSS 5 On"


def carrega(p, w, h):
    if p.lower().endswith('.rgba'):
        a = np.fromfile(p, np.uint8)
        a = a.reshape(h, w, 4)[..., :3]
    else:
        a = np.asarray(Image.open(p).convert('RGB'))
    return a.astype(np.float32) / 255.0


def lab(srgb):
    lin = np.where(srgb <= 0.04045, srgb / 12.92, ((srgb + 0.055) / 1.055) ** 2.4)
    M = np.array([[0.4124, 0.3576, 0.1805], [0.2126, 0.7152, 0.0722], [0.0193, 0.1192, 0.9505]], np.float32)
    xyz = lin @ M.T / np.array([0.9505, 1.0, 1.089], np.float32)
    f = np.where(xyz > 0.008856, np.cbrt(xyz), 7.787 * xyz + 16 / 116)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], -1)


def stats(img, L):
    l = L[..., 0]
    c = np.hypot(L[..., 1], L[..., 2])
    h = np.degrees(np.arctan2(L[..., 2].mean(), L[..., 1].mean())) % 360
    y = l / 100.0
    det = float(np.std(y - ndimage.gaussian_filter(y, 2.0)) * 100)
    return {'L': round(float(l.mean()), 2), 'p10': round(float(np.percentile(l, 10)), 2),
            'p90': round(float(np.percentile(l, 90)), 2), 'C': round(float(c.mean()), 2), 'matiz': round(float(h), 1),
            'detalhe': round(det, 3)}


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    js = sys.argv[sys.argv.index('--json') + 1] if '--json' in sys.argv else None
    if js in args:
        args.remove(js)
    off_p, on_p, nossa_p = args[:3]
    rotulo = args[3] if len(args) > 3 else 'nossa'
    off = carrega(off_p, 0, 0)
    H, W = off.shape[:2]
    on = carrega(on_p, W, H)
    nos = carrega(nossa_p, W, H)
    mascara = np.ones((H, W), bool)
    for x0, y0, x1, y1 in SELOS:
        mascara[y0:y1, x0:x1] = False
    Loff, Lon, Lnos = lab(off), lab(on), lab(nos)
    psnr = lambda a, b: float(10 * np.log10(1.0 / np.mean((a[mascara] - b[mascara]) ** 2)))
    de = lambda A, B: float(np.linalg.norm(A[mascara] - B[mascara], axis=-1).mean())
    res = {'rotulo': rotulo,
           'geral': {'psnr_original_vs_nvidia': round(psnr(off, on), 2), 'psnr_nossa_vs_nvidia': round(psnr(nos, on), 2),
                     'dE_original_vs_nvidia': round(de(Loff, Lon), 2), 'dE_nossa_vs_nvidia': round(de(Lnos, Lon), 2)},
           'regioes': {}}
    # quanto da mudanca da NVIDIA (on - off) a nossa reproduz, por regiao: 1 = mesma mudanca, 0 = nenhuma
    soma_erro = 0.0
    for nome, (x0, y0, x1, y1) in REGIOES.items():
        sl = (slice(y0, y1), slice(x0, x1))
        so, sn, sv = stats(off[sl], Loff[sl]), stats(on[sl], Lon[sl]), stats(nos[sl], Lnos[sl])
        dif = {k: round(sv[k] - sn[k], 2) for k in ('L', 'p10', 'p90', 'C', 'detalhe')}
        dif['matiz'] = round(((sv['matiz'] - sn['matiz'] + 180) % 360) - 180, 1)
        dif['detalhe_razao'] = round(sv['detalhe'] / max(sn['detalhe'], 1e-6), 3)
        res['regioes'][nome] = {'original': so, 'nvidia': sn, 'nossa': sv, 'nossa_menos_nvidia': dif,
                                'dE': round(float(np.linalg.norm(Lnos[sl] - Lon[sl], axis=-1).mean()), 2)}
        soma_erro += abs(dif['L']) + abs(dif['C']) + 0.5 * abs(dif['p90']) + 0.5 * abs(dif['p10']) + \
            20 * abs(np.log(max(dif['detalhe_razao'], 1e-3))) + 0.3 * abs(dif['matiz'])
    res['erro_regional'] = round(soma_erro / len(REGIOES), 3)
    txt = json.dumps(res, ensure_ascii=False, indent=1)
    if js:
        open(js, 'w', encoding='utf-8').write(txt)
    # resumo curto no terminal
    print(json.dumps({'rotulo': rotulo, **res['geral'], 'erro_regional': res['erro_regional'],
                      'rosto': res['regioes']['rosto']['nossa_menos_nvidia'],
                      'maos': res['regioes']['maos']['nossa_menos_nvidia']}, ensure_ascii=False))


if __name__ == '__main__':
    main()
