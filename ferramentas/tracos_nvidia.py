# Os tracos que se veem na Zorah com o DLSS 5 (fotos da NVIDIA), medidos sem depender de alinhamento pixel a pixel:
#  - curva de tom: percentis de L* (o claro sobe sem estourar, o escuro desce) e fracao estourada;
#  - detalhe e contraste local por faixa de luz (escuro / medio / claro), em duas escalas;
#  - espalhamento de luz: energia de baixa frequencia em volta das fontes claras (o "brilho que vaza");
#  - croma por faixa de luz.
# As mascaras de faixa saem da foto "off" e valem para as tres imagens (as faixas sao areas grandes; o desalinhamento
# entre os dois renders da NVIDIA pouco muda as estatisticas).
#
# uso: python tracos_nvidia.py <off.jpg> <on.jpg> <nossa.rgba|png> [mais...] [--json saida.json]
import json, os, sys
import numpy as np
from PIL import Image
from scipy import ndimage

SELOS = [(60, 1780, 800, 1960), (3150, 1930, 3920, 2110)]
PCT = (1, 5, 10, 25, 50, 75, 90, 95, 99, 99.7)


def carrega(p, w=0, h=0):
    if p.lower().endswith('.rgba'):
        return np.fromfile(p, np.uint8).reshape(h, w, 4)[..., :3].astype(np.float32) / 255.0
    return np.asarray(Image.open(p).convert('RGB')).astype(np.float32) / 255.0


def lab(srgb):
    lin = np.where(srgb <= 0.04045, srgb / 12.92, ((srgb + 0.055) / 1.055) ** 2.4)
    M = np.array([[0.4124, 0.3576, 0.1805], [0.2126, 0.7152, 0.0722], [0.0193, 0.1192, 0.9505]], np.float32)
    xyz = lin @ M.T / np.array([0.9505, 1.0, 1.089], np.float32)
    f = np.where(xyz > 0.008856, np.cbrt(xyz), 7.787 * xyz + 16 / 116)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], -1)


def tracos(img, faixas, valida, fontes, anel):
    L = lab(img)
    l = L[..., 0]
    c = np.hypot(L[..., 1], L[..., 2])
    y = l / 100.0
    fino = y - ndimage.gaussian_filter(y, 2.0)                                   # detalhe (textura, poros)
    medio = ndimage.gaussian_filter(y, 2.0) - ndimage.gaussian_filter(y, 12.0)   # definicao (formas, volumes)
    baixo = ndimage.gaussian_filter(y, 32.0)                                    # luz de grande escala
    r = {'percentis_L': {str(p): round(float(np.percentile(l[valida], p)), 2) for p in PCT},
         'estourado_%': round(float((img.max(-1)[valida] >= 0.995).mean() * 100), 3),
         'preto_%': round(float((img.max(-1)[valida] <= 0.01).mean() * 100), 3)}
    for nome, m in faixas.items():
        r[nome] = {'L': round(float(l[m].mean()), 2), 'C': round(float(c[m].mean()), 2),
                   'detalhe': round(float(fino[m].std() * 100), 3), 'definicao': round(float(medio[m].std() * 100), 3)}
    # espalhamento: luz de baixa frequencia no anel em volta das fontes claras, relativa a luz das proprias fontes
    r['espalhamento'] = round(float(baixo[anel].mean() / max(baixo[fontes].mean(), 1e-6)), 4)
    r['luz_anel'] = round(float(l[anel].mean()), 2)
    return r


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    js = sys.argv[sys.argv.index('--json') + 1] if '--json' in sys.argv else None
    if js in args:
        args.remove(js)
    off_p, on_p, nossas = args[0], args[1], args[2:]
    off = carrega(off_p)
    H, W = off.shape[:2]
    valida = np.ones((H, W), bool)
    for x0, y0, x1, y1 in SELOS:
        valida[y0:y1, x0:x1] = False
    lo = lab(off)[..., 0]
    faixas = {'escuro': valida & (lo < 22), 'medio': valida & (lo >= 22) & (lo < 60), 'claro': valida & (lo >= 60)}
    fontes = valida & (lo >= np.percentile(lo[valida], 97))
    anel = valida & ndimage.binary_dilation(fontes, iterations=24) & ~ndimage.binary_dilation(fontes, iterations=4)
    res = {'off': tracos(off, faixas, valida, fontes, anel), 'nvidia': tracos(carrega(on_p), faixas, valida, fontes, anel)}
    for p in nossas:
        res[os.path.splitext(os.path.basename(p))[0]] = tracos(carrega(p, W, H), faixas, valida, fontes, anel)
    if js:
        open(js, 'w', encoding='utf-8').write(json.dumps(res, ensure_ascii=False, indent=1))
    # tabela curta: cada linha e uma imagem; as colunas sao os tracos que importam
    cols = ['p1', 'p10', 'p50', 'p90', 'p99', 'p99.7', 'estour', 'det_esc', 'def_esc', 'det_cla', 'def_cla', 'C_esc',
            'C_med', 'C_cla', 'espalh']
    print(f"{'imagem':<22}" + ''.join(f'{c:>8}' for c in cols))
    for k, r in res.items():
        pl = r['percentis_L']
        v = [pl['1'], pl['10'], pl['50'], pl['90'], pl['99'], pl['99.7'], r['estourado_%'], r['escuro']['detalhe'],
             r['escuro']['definicao'], r['claro']['detalhe'], r['claro']['definicao'], r['escuro']['C'], r['medio']['C'],
             r['claro']['C'], r['espalhamento']]
        print(f'{k[:22]:<22}' + ''.join(f'{x:>8.2f}' if abs(x) >= 0.1 or x == 0 else f'{x:>8.3f}' for x in v))


if __name__ == '__main__':
    main()
