# A mudanca da NVIDIA pixel a pixel (as fotos Zorah on/off estao alinhadas: 0,00 px medidos por correlacao de fase):
# para cada imagem nossa, o quanto a NOSSA mudanca (nossa - off) segue a da NVIDIA (on - off), em L*, por escala:
#   luz      = baixa frequencia (gauss 16 px): luz e sombra em grande escala
#   definicao = banda 2..12 px: formas e volumes (contraste local)
#   fino     = abaixo de 2 px: textura e ruido
# corr = correlacao (1 = mesma mudanca); ganho = inclinacao (1 = mesma forca); resto = RMS do que sobra (menor = melhor)
# Mais cor: razao de croma por faixa de luz (faixas da foto off).
# uso: python delta_nvidia.py <off> <on> <nossa.rgba> [...] [--ordem resto|corr]
import os, sys
import numpy as np
from scipy import ndimage
sys.path.insert(0, os.path.dirname(__file__))
from tracos_nvidia import carrega, lab, SELOS

REGIOES = {'rosto': (1820, 380, 2080, 700), 'roupa': (1320, 1680, 2600, 2080), 'flores': (640, 160, 1600, 440),
           'dourado': (3280, 600, 3520, 1360), 'madeira': (0, 1520, 960, 1760)}


def bandas(l):
    g2, g12, g16 = (ndimage.gaussian_filter(l, s) for s in (2.0, 12.0, 16.0))
    return {'luz': g16, 'definicao': g2 - g12, 'fino': l - g2}


def main():
    args = [a for i, a in enumerate(sys.argv[1:], 1) if not a.startswith('--') and sys.argv[i - 1] != '--ordem']
    off_p, on_p, nossas = args[0], args[1], args[2:]
    off = carrega(off_p)
    H, W = off.shape[:2]
    on = carrega(on_p, W, H)
    v = np.ones((H, W), bool)
    for x0, y0, x1, y1 in SELOS:
        v[y0:y1, x0:x1] = False
    v = ndimage.binary_erosion(v, iterations=40)  # longe dos selos (o filtro de 16 px nao vaza)
    Loff, Lon = lab(off), lab(on)
    boff, bon = bandas(Loff[..., 0]), bandas(Lon[..., 0])
    dn = {k: (bon[k] - boff[k])[v] for k in boff}
    cortes = [0, 8, 22, 40, 60, 101]
    lo = Loff[..., 0]
    mf = [v & (lo >= a) & (lo < b) for a, b in zip(cortes[:-1], cortes[1:])]
    Cn = np.hypot(Lon[..., 1], Lon[..., 2])
    linhas = []
    for p in nossas:
        L = lab(carrega(p, W, H))
        b = bandas(L[..., 0])
        r = []
        for k in ('luz', 'definicao', 'fino'):
            d = (b[k] - boff[k])[v]
            corr = float(np.corrcoef(d, dn[k])[0, 1])
            ganho = float(np.dot(d, dn[k]) / max(np.dot(dn[k], dn[k]), 1e-9))
            resto = float(np.sqrt(np.mean((d - dn[k]) ** 2)))
            r.append((corr, ganho, resto))
        C = np.hypot(L[..., 1], L[..., 2])
        cr = [float(C[m].mean() / Cn[m].mean()) for m in mf]
        dE = float(np.linalg.norm(L[v] - Lon[v], axis=-1).mean())
        # mudanca media de luz (grande escala) por regiao, nossa e da NVIDIA
        reg = {n: (float((b['luz'] - boff['luz'])[y0:y1, x0:x1].mean()), float((bon['luz'] - boff['luz'])[y0:y1, x0:x1].mean()))
               for n, (x0, y0, x1, y1) in REGIOES.items()}
        linhas.append((sum(x[2] for x in r), os.path.basename(p), r, cr, dE, reg))
    ordem = 'corr' if '--ordem' in sys.argv and sys.argv[sys.argv.index('--ordem') + 1] == 'corr' else 'resto'
    linhas.sort(key=(lambda x: -(x[2][0][0] + x[2][1][0])) if ordem == 'corr' else (lambda x: x[0]))
    print(f"{'imagem':<30}{'luz corr/ganho/resto':>22}{'definicao c/g/r':>20}{'fino c/g/r':>19}{'dE':>6}   cor por faixa (preto..claro, 1 = NVIDIA)")
    for tot, n, r, cr, dE, reg in linhas:
        s = ''.join(f'{c:6.2f}{g:6.2f}{e:6.2f} ' for c, g, e in r)
        print(f'{n[:30]:<30}  {s}{dE:5.2f}   ' + ' '.join(f'{x:.2f}' for x in cr))
    if linhas:
        print('\nmudanca de luz por regiao (L*, grande escala)')
        print(f"{'':<30}" + ''.join(f'{k:>16}' for k in REGIOES))
        print(f"{'NVIDIA':<30}" + ''.join(f'{linhas[0][5][k][1]:16.2f}' for k in REGIOES))
        for tot, n, r, cr, dE, reg in linhas:
            print(f'{n[:30]:<30}' + ''.join(f'{reg[k][0]:16.2f}' for k in REGIOES))


if __name__ == '__main__':
    main()
