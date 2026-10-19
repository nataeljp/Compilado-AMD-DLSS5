# Saida HDR do modo imagem (scRGB RGBA16F cru) -> a mesma "tela" do HDR sintetico (SoftKnee + sRGB, RGBA8), para comparar
# com as fotos. uso: python hdr_para_sdr.py <entrada.rgba16> <saida.rgba> <nits do branco> [largura altura]
import sys
import numpy as np

ent, sai, nits = sys.argv[1], sys.argv[2], float(sys.argv[3])
W, H = (int(sys.argv[4]), int(sys.argv[5])) if len(sys.argv) > 5 else (3840, 2160)
a = np.fromfile(ent, np.float16).reshape(H, W, 4)[..., :3].astype(np.float32) / (nits / 80.0)
a = np.maximum(a, 0)
m = a.max(-1, keepdims=True)
j = np.where(m > 0.75, 0.75 + 0.25 * (1 - np.exp(-(m - 0.75) / 0.25)), m)
a = np.where(m > 0.75, a * j / np.maximum(m, 1e-9), a)
a = np.clip(a, 0, 1)
s = np.where(a <= 0.0031308, a * 12.92, 1.055 * np.power(a, 1 / 2.4) - 0.055)
o = np.concatenate([np.round(s * 255).astype(np.uint8), np.full((H, W, 1), 255, np.uint8)], -1)
o.tofile(sai)
print('ok', o.shape)
