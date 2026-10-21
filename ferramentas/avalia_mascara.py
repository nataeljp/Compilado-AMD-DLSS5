# Mede a mascara de desenvolvedor em movimento: a sequencia de gera_sintetico_buffer.py --pessoa (verdade em NNNN_gt.raw)
# contra o estado da mascara gravado pelo overlay (NNNN_mascara.raw, RGBA16F: pele, cabelo, personagem, objeto).
#   cobertura: personagem medio DENTRO da pessoa (borda de 6 px fora): deve ficar perto de 1
#   rastro:    personagem medio onde a pessoa ESTAVA nos 3 quadros anteriores e nao esta mais (a mascara atrasada)
#   IoU:       personagem > 0,5 contra a pessoa > 0,5
# uso: python avalia_mascara.py <sequencia> <gravacao> <mapa.jsonl> [saida.png]
import json, sys
from pathlib import Path
import numpy as np
from scipy import ndimage

seq, rep, mapa = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
out = sys.argv[4] if len(sys.argv) > 4 else None
meta = json.loads((rep / 'meta.jsonl').read_text().splitlines()[0])
W, H = meta['rede_w'], meta['rede_h']
jogo2seq = {}
for l in mapa.read_text().splitlines():
    try:
        d = json.loads(l)
        jogo2seq[d['jogo']] = d['seq']
    except Exception:
        pass
linhas = []
for l in (rep / 'meta.jsonl').read_text().splitlines()[1:]:
    try:
        linhas.append(json.loads(l))
    except Exception:
        pass  # linha cortada (gravacao interrompida)
gt = lambda i: np.fromfile(seq / f'{i:04d}_gt.raw', np.float32).reshape(H, W)
res = []
vis = []
for d in linhas:
    q, s = d['q'], jogo2seq.get(d.get('jogo'), None)
    f = rep / f'{q:04d}_mascara.raw'
    if s is None or not f.exists():
        continue
    m = np.fromfile(f, np.float16).reshape(H, W, 4).astype(np.float32)
    ch = m[..., 0] + m[..., 1] + m[..., 2]
    if ch.max() == 0:
        continue
    g = gt(s) > 0.5
    dentro = ndimage.binary_erosion(g, iterations=6)
    perto = ndimage.binary_dilation(g, iterations=12)
    antes = np.zeros_like(g)
    for k in (1, 2, 3):
        if s - k >= 0:
            antes |= gt(s - k) > 0.5
    rastro = antes & ~perto
    iou = ((ch > 0.5) & g).sum() / max(((ch > 0.5) | g).sum(), 1)
    res.append((s, ch[dentro].mean() if dentro.any() else np.nan, ch[rastro].mean() if rastro.any() else np.nan, iou))
    if out and len(vis) < 6 and q % 6 == 0:
        o = np.fromfile(rep / f'{q:04d}_orig.raw', np.float16).reshape(H, W, 4)[..., :3].astype(np.float32)
        o = np.clip(o, 0, 1) ** (1 / 2.2)
        cor = o * 0.5
        cor[..., 2] += 0.5 * np.clip(ch, 0, 1)
        cor[..., 0] += 0.5 * np.clip(m[..., 0], 0, 1)
        borda = g ^ ndimage.binary_erosion(g, iterations=2)
        cor[borda] = [1, 1, 0]
        vis.append((np.clip(cor, 0, 1)[::2, ::2] * 255).astype(np.uint8))
r = np.array(res, np.float64)
print('quadros %d | cobertura %.3f (pior %.3f) | rastro %.3f (pior %.3f) | IoU %.3f (pior %.3f)' % (
    len(r), np.nanmean(r[:, 1]), np.nanmin(r[:, 1]), np.nanmean(r[:, 2]), np.nanmax(r[:, 2]), r[:, 3].mean(), r[:, 3].min()))
if out and vis:
    from PIL import Image
    lin = [np.concatenate(vis[i:i + 3], 1) for i in range(0, len(vis) - len(vis) % 3 or 3, 3)]
    Image.fromarray(np.concatenate(lin, 0)).save(out)
