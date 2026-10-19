# Compara a saida do overlay com a saida REAL da NVIDIA para a mesma entrada (pares publicados por mochizuki0323/DLSSNR-AMD,
# docs/ngx-verification: RTX 5090 + nvngx_dlssnr 310.8, quadro unico, Style 0). Mesma metrica do relatorio: RGB 8 bits.
#   psnr     = contra a NVIDIA (o dele: 45,56 / 47,99 / 49,06; sem rede: 26,44 / 27,72 / 28,69)
#   corr     = correlacao da edicao (nossa - entrada) com a da NVIDIA (nvidia - entrada)
#   ganho    = forca da nossa edicao em relacao a da NVIDIA (1 = mesma)
#   media    = desvio medio por canal (1/255)
# uso: python compara_ref.py <entrada.png> <nvidia.png> <nossa.rgba|png> [...]
import os, sys
import numpy as np
from PIL import Image


def carrega(p, w=0, h=0):
    if p.lower().endswith('.rgba'):
        return np.fromfile(p, np.uint8).reshape(h, w, 4)[..., :3].astype(np.float64)
    return np.asarray(Image.open(p).convert('RGB')).astype(np.float64)


def main():
    ent, nv = carrega(sys.argv[1]), carrega(sys.argv[2])
    H, W = ent.shape[:2]
    en = (nv - ent).ravel()
    print(f"{'imagem':<34}{'psnr':>7}{'corr':>7}{'ganho':>7}   media R/G/B (1/255)")
    psnr = lambda a: 10 * np.log10(255 ** 2 / np.mean((a - nv) ** 2))
    print(f"{'(entrada, sem rede)':<34}{psnr(ent):7.2f}")
    for p in sys.argv[3:]:
        a = carrega(p, W, H)
        eo = (a - ent).ravel()
        c = np.corrcoef(eo, en)[0, 1]
        g = np.dot(eo, en) / np.dot(en, en)
        m = (a - nv).reshape(-1, 3).mean(0)
        print(f"{os.path.basename(p)[:34]:<34}{psnr(a):7.2f}{c:7.3f}{g:7.3f}   {m[0]:+.2f} {m[1]:+.2f} {m[2]:+.2f}")


if __name__ == '__main__':
    main()
