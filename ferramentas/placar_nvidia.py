# Placar de semelhanca com a foto da NVIDIA (Zorah), independente de alinhamento:
#  curva  = media |desloc. nosso - desloc. NVIDIA| dos percentis de L* (2..99) em relacao a foto "off"
#  croma  = media |C nosso - C NVIDIA| nas faixas escuro/medio/claro
#  detalhe = 10 * media |ln(razao)| do detalhe fino e da definicao nas faixas escuro/claro
#  total = curva + croma + detalhe (menor = mais parecido)
# uso: python placar_nvidia.py <off> <on> <nossa.rgba> [...]
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from tracos_nvidia import carrega, lab, tracos, SELOS
from scipy import ndimage

Q = [2, 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 95, 97, 98, 99]


def main():
    off_p, on_p, nossas = sys.argv[1], sys.argv[2], sys.argv[3:]
    off = carrega(off_p)
    H, W = off.shape[:2]
    valida = np.ones((H, W), bool)
    for x0, y0, x1, y1 in SELOS:
        valida[y0:y1, x0:x1] = False
    lo = lab(off)[..., 0]
    faixas = {'escuro': valida & (lo < 22), 'medio': valida & (lo >= 22) & (lo < 60), 'claro': valida & (lo >= 60)}
    fontes = valida & (lo >= np.percentile(lo[valida], 97))
    anel = valida & ndimage.binary_dilation(fontes, iterations=24) & ~ndimage.binary_dilation(fontes, iterations=4)
    on = carrega(on_p, W, H)
    curva = lambda img: np.percentile(lab(img)[..., 0][valida], Q)
    co, cn = curva(off), curva(on)
    tn = tracos(on, faixas, valida, fontes, anel)
    print(f"{'imagem':<26}{'curva':>7}{'croma':>7}{'detalhe':>8}{'total':>7}   claros(p90/p99)  preto(p1)  C_claro")
    linhas = []
    for p in nossas:
        img = carrega(p, W, H)
        c = curva(img)
        t = tracos(img, faixas, valida, fontes, anel)
        dc = float(np.mean(np.abs((c - co) - (cn - co))))
        dcr = float(np.mean([abs(t[f]['C'] - tn[f]['C']) for f in faixas]))
        ddt = 10 * float(np.mean([abs(np.log(t[f][k] / tn[f][k])) for f in ('escuro', 'claro') for k in ('detalhe', 'definicao')]))
        linhas.append((dc + dcr + ddt, p, dc, dcr, ddt, t))
    for tot, p, dc, dcr, ddt, t in sorted(linhas, key=lambda x: x[0]):
        pl = t['percentis_L']
        print(f"{os.path.basename(p)[:26]:<26}{dc:7.2f}{dcr:7.2f}{ddt:8.2f}{tot:7.2f}   {pl['90']:6.1f}/{pl['99']:5.1f}    {pl['1']:5.2f}    {t['claro']['C']:6.2f}")
    pl = tn['percentis_L']
    print(f"{'(nvidia)':<26}{'':>29}   {pl['90']:6.1f}/{pl['99']:5.1f}    {pl['1']:5.2f}    {tn['claro']['C']:6.2f}")


if __name__ == '__main__':
    main()
