# Pele contra a NVIDIA (Zorah, fotos alinhadas): rosto e maos, pixel a pixel.
#   dL      = mudanca media de luz (L*) contra a foto off; a NVIDIA e a referencia
#   C       = croma medio (cor) / o da NVIDIA
#   fino    = detalhe < 2 px (poros, textura) / o da NVIDIA
#   forma   = definicao 2..12 px (volume, sombra do rosto) / o da NVIDIA
#   dE      = erro de cor por pixel (CIELAB) contra a NVIDIA (menor = mais perto); "off" mostra o erro sem rede
#   nota    = dE medio de rosto e maos + penalidade das razoes (1 = igual)
# uso: python pele_nvidia.py <off> <on> <nossa.rgba> [...]
import os, sys
import numpy as np
from scipy import ndimage
sys.path.insert(0, os.path.dirname(__file__))
from tracos_nvidia import carrega, lab

PELE = {'rosto': (1820, 380, 2080, 700), 'maos': (1760, 1440, 2160, 1600)}


def medidas(Lab, Loff, caixa):
    x0, y0, x1, y1 = caixa
    sl = (slice(y0, y1), slice(x0, x1))
    l = Lab[..., 0]
    g2, g12 = ndimage.gaussian_filter(l, 2.0), ndimage.gaussian_filter(l, 12.0)
    return {'dL': float((l - Loff[..., 0])[sl].mean()), 'C': float(np.hypot(Lab[..., 1], Lab[..., 2])[sl].mean()),
            'fino': float((l - g2)[sl].std()), 'forma': float((g2 - g12)[sl].std())}


def main():
    off_p, on_p, nossas = sys.argv[1], sys.argv[2], sys.argv[3:]
    off = carrega(off_p)
    H, W = off.shape[:2]
    Loff, Lon = lab(off), lab(carrega(on_p, W, H))
    ref = {n: medidas(Lon, Loff, c) for n, c in PELE.items()}
    print(f"{'imagem':<28}" + ''.join(f"{n + ': dL   C    fino forma  dE':>36}" for n in PELE) + '   nota')
    print(f"{'NVIDIA':<28}" + ''.join(f"{ref[n]['dL']:+7.2f} {1:5.2f} {1:5.2f} {1:5.2f} {0:5.2f}       " for n in PELE))
    linhas = []
    for p in ['off'] + nossas:
        L = Loff if p == 'off' else lab(carrega(p, W, H))
        s, nota = '', 0.0
        for n, c in PELE.items():
            m = medidas(L, Loff, c)
            x0, y0, x1, y1 = c
            dE = float(np.linalg.norm(L[y0:y1, x0:x1] - Lon[y0:y1, x0:x1], axis=-1).mean())
            rC, rf, rd = m['C'] / ref[n]['C'], m['fino'] / ref[n]['fino'], m['forma'] / ref[n]['forma']
            nota += dE + 5 * (abs(np.log(rC)) + abs(np.log(rf)) + abs(np.log(rd)))
            s += f"{m['dL']:+7.2f} {rC:5.2f} {rf:5.2f} {rd:5.2f} {dE:5.2f}       "
        linhas.append((nota / len(PELE), os.path.basename(p), s))
    for nota, n, s in sorted(linhas):
        print(f'{n[:28]:<28}{s}{nota:6.2f}')


if __name__ == '__main__':
    main()
