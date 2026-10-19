# Qualidade do fluxo optico nos quadros gravados (Ctrl+Alt+G), sem GPU:
#   erro  = |luma atual - luma anterior deslocada pelo fluxo| medio (perceptual, 0..1), nos pixels validos
#   conf  = fracao de pixels em que ida e volta batem (|v_ida + v_volta(p+v_ida)| < 0,5 + 0,05|v|)
# Compara o fluxo gravado (o do overlay) com o DIS do OpenCV em escalas diferentes.
# uso: python avalia_fluxo.py <pasta da gravacao> [quadro_inicial]
import json, sys, time
from pathlib import Path
import cv2
import numpy as np

pasta = Path(sys.argv[1])
ini = int(sys.argv[2]) if len(sys.argv) > 2 else 1
l = [json.loads(x) for x in (pasta / 'meta.jsonl').read_text().splitlines() if x.strip()]
cab, q = l[0], l[1:]
W, H = cab['rede_w'], cab['rede_h']


def luma(i):
    o = np.fromfile(pasta / f'{i:04d}_orig.raw', np.float16).reshape(H, W, 4)[..., :3].astype(np.float32)
    y = o @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    y = np.clip(y, 0, None)
    return np.power(y / (1 + y), 1 / 2.2)   # compressao suave (luz de cena) -> ~perceptual 0..1


def desloca(img, v):
    gx, gy = np.meshgrid(np.arange(img.shape[1], dtype=np.float32), np.arange(img.shape[0], dtype=np.float32))
    return cv2.remap(img, gx + v[..., 0], gy + v[..., 1], cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)


def mede(ya, yb, ida, volta):
    """ida: vetor atual->anterior (no atual); volta: anterior->atual (no anterior)."""
    rep = desloca(yb, ida)
    vv = desloca(volta, ida)
    soma = np.hypot(ida[..., 0] + vv[..., 0], ida[..., 1] + vv[..., 1])
    ok = soma < 0.5 + 0.05 * np.hypot(ida[..., 0], ida[..., 1])
    gx, gy = np.meshgrid(np.arange(W), np.arange(H))
    dentro = (gx + ida[..., 0] >= 0) & (gx + ida[..., 0] < W) & (gy + ida[..., 1] >= 0) & (gy + ida[..., 1] < H)
    err = np.abs(ya - rep)[dentro]
    return float(err.mean()), float(ok[dentro].mean())


def dis(ya, yb, escala, preset):
    f = cv2.DISOpticalFlow_create(preset)
    a = cv2.resize((ya * 255).astype(np.uint8), (W // escala, H // escala), interpolation=cv2.INTER_AREA)
    b = cv2.resize((yb * 255).astype(np.uint8), (W // escala, H // escala), interpolation=cv2.INTER_AREA)
    ida = f.calc(a, b, None)
    volta = f.calc(b, a, None)
    up = lambda v: cv2.resize(v, (W, H), interpolation=cv2.INTER_LINEAR) * escala
    return up(ida), up(volta)


res = {}
for i in range(ini, len(q)):
    if not q[i]['mov']:
        continue
    ya, yb = luma(i), luma(i - 1)
    gravado = np.fromfile(pasta / f'{i:04d}_mov.raw', np.float16).reshape(H, W, 2).astype(np.float32)
    # o overlay so grava a ida; a volta para a medida de consistencia sai do proprio DIS na mesma escala abaixo
    linha = {'q': i, 'vel': round(float(np.hypot(gravado[..., 0], gravado[..., 1]).mean()), 1)}
    e0 = float(np.abs(ya - desloca(yb, gravado)).mean())
    linha['overlay_erro'] = round(e0, 4)
    linha['sem_fluxo_erro'] = round(float(np.abs(ya - yb).mean()), 4)
    for esc, preset, nome in [(4, cv2.DISOPTICAL_FLOW_PRESET_MEDIUM, 'dis_1/4_medio'), (8, cv2.DISOPTICAL_FLOW_PRESET_MEDIUM, 'dis_1/8_medio'),
                              (4, cv2.DISOPTICAL_FLOW_PRESET_FAST, 'dis_1/4_rapido')]:
        t = time.perf_counter()
        ida, volta = dis(ya, yb, esc, preset)
        dt = (time.perf_counter() - t) * 1000
        e, c = mede(ya, yb, ida, volta)
        linha[nome] = f'erro {e:.4f} conf {c:.2f} ({dt:.0f} ms cpu)'
    print(json.dumps(linha, ensure_ascii=False), flush=True)
