# Previa e medidas de uma gravacao do overlay (Ctrl+Alt+G): bin\gravacoes\<data_hora>\
# Gera previa.gif (original | resultado | edicao x4 | movimento) e recortes PNG em tamanho real, e mede a cintilacao
# da edicao entre quadros (com rede x reaproveitados). So CPU.
# uso: python ver_gravacao.py <pasta da gravacao> [x0 y0 largura altura do recorte, em pixels da rede]
import json, sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont

pasta = Path(sys.argv[1])
linhas = [json.loads(l) for l in (pasta / 'meta.jsonl').read_text().splitlines() if l.strip()]
cab, quadros = linhas[0], linhas[1:]
W, H = cab['rede_w'], cab['rede_h']
cfg = json.loads((pasta / 'config.json').read_text())
filmica = cfg.get('ponte', -1) == 3


def le(i, nome, c):
    a = np.fromfile(pasta / f'{i:04d}_{nome}.raw', np.float16).reshape(H, W, c).astype(np.float32)
    return a


def tela(lin):
    x = np.maximum(lin, 0)
    if filmica:   # luz de cena -> tela pela curva do jogo (ACES de Narkowicz), como o overlay
        x = np.clip((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0, 1)
    elif cab.get('hdr'):
        m = x.max(-1, keepdims=True)
        j = np.where(m > 0.75, 0.75 + 0.25 * (1 - np.exp(-(m - 0.75) / 0.25)), m)
        x = np.where(m > 0.75, x * j / np.maximum(m, 1e-9), x)
    x = np.clip(x, 0, 1)
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(x, 1 / 2.4) - 0.055)


def mov_cor(v, conf):
    ang = (np.arctan2(v[..., 1], v[..., 0]) / (2 * np.pi) + 0.5)
    hsv = np.stack([ang, np.clip(np.hypot(v[..., 0], v[..., 1]) / 16, 0, 1), np.clip(conf, 0, 1)], -1)
    i = (hsv[..., 0] * 6).astype(int) % 6
    f = hsv[..., 0] * 6 - np.floor(hsv[..., 0] * 6)
    v_, s = hsv[..., 2], hsv[..., 1]
    p, q, t = v_ * (1 - s), v_ * (1 - f * s), v_ * (1 - (1 - f) * s)
    r = np.choose(i, [v_, q, p, p, t, v_]); g = np.choose(i, [t, v_, v_, q, p, p]); b = np.choose(i, [p, p, t, v_, v_, q])
    return np.stack([r, g, b], -1)


esc = max(1, W // 640)
pw, ph = W // esc, H // esc
try:
    fonte = ImageFont.truetype('arialbd.ttf', 18)
except Exception:
    fonte = ImageFont.load_default()
x0, y0, cw, ch = (int(a) for a in sys.argv[2:6]) if len(sys.argv) >= 6 else (W // 2 - 400, H // 2, 800, 600)
gif, recortes, edicoes = [], [], []
for i, q in enumerate(quadros):
    o = le(i, 'orig', 4)[..., :3]
    e = le(i, 'edicao', 4)[..., :3]
    m = le(i, 'mov', 2)
    c = le(i, 'conf', 1)[..., 0]
    res = tela(o + e)
    edicoes.append(e[::esc, ::esc].copy())
    ev = np.clip(0.5 + 4 * e, 0, 1)
    pain = [tela(o), res, ev, mov_cor(m, c)]
    img = np.concatenate([np.concatenate(pain[:2], 1), np.concatenate(pain[2:], 1)], 0)[::esc, ::esc]
    pil = Image.fromarray((img * 255).astype(np.uint8))
    d = ImageDraw.Draw(pil)
    rot = 'REDE' if q['rede'] else 'reaproveitado'
    d.text((8, 6), f'quadro {i}  {rot}', fill=(255, 220, 80) if q['rede'] else (255, 90, 90), font=fonte)
    for k, t in enumerate(['original', 'resultado', 'edicao x4', 'movimento (cor=direcao, escuro=sem confianca)']):
        d.text(((k % 2) * pw * 1 + 8, (k // 2) * ph + 28), t, fill=(255, 255, 255), font=fonte)
    gif.append(pil)
    rec = np.concatenate([tela(o)[y0:y0 + ch, x0:x0 + cw], res[y0:y0 + ch, x0:x0 + cw], ev[y0:y0 + ch, x0:x0 + cw]], 1)
    recortes.append(Image.fromarray((rec * 255).astype(np.uint8)))
gif[0].save(pasta / 'previa.gif', save_all=True, append_images=gif[1:], duration=150, loop=0)
for i, r in enumerate(recortes):
    r.save(pasta / f'recorte_{i:02d}.png')
# cintilacao: diferenca da edicao exibida entre quadros consecutivos, separando quem chega num quadro com rede ou nao
dif = {'rede': [], 'reaproveitado': []}
alta = {'rede': [], 'reaproveitado': []}
for i in range(1, len(edicoes)):
    dlt = edicoes[i] - edicoes[i - 1]
    k = 'rede' if quadros[i]['rede'] else 'reaproveitado'
    dif[k].append(float(np.sqrt(np.mean(dlt ** 2))))
    # parte granulada: o que sobra tirando a media 3x3 (ruido de pixel a pixel)
    pad = np.pad(dlt, ((1, 1), (1, 1), (0, 0)), mode='edge')
    viz = sum(pad[a:a + dlt.shape[0], b:b + dlt.shape[1]] for a in range(3) for b in range(3)) / 9
    alta[k].append(float(np.sqrt(np.mean((dlt - viz) ** 2))))
print(json.dumps({k: {'mudanca_edicao_rms': round(float(np.mean(v)), 5) if v else None,
                      'granulado_rms': round(float(np.mean(alta[k])), 5) if alta[k] else None, 'quadros': len(v)}
                  for k, v in dif.items()}, ensure_ascii=False))
print('previa:', pasta / 'previa.gif')
