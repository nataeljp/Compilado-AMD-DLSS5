# Fantasma medido contra a resposta da rede para o MESMO quadro (overlay sincrono: sem reaproveitar nada, sem fantasma).
# Na faixa recem-descoberta: g = sum((e - s)·(s_obj - s)) / sum(|s_obj - s|^2), com s = edicao sincrona do fundo,
# s_obj = edicao sincrona do objeto no ponto dele que cobria o pixel. 0 = nada do objeto; 1 = a edicao dele inteira.
# uso: python fantasma_vs_sincrono.py <sequencia> <sincrono: gravacao> <mapa sincrono> <variante: gravacao|saida> <mapa>
import json, sys
from pathlib import Path
import numpy as np, cv2
seq, gs, ms, gv, mv = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]), Path(sys.argv[5])
meta = json.loads((seq / 'meta.jsonl').read_text().splitlines()[0]); W, H, N = meta['rede_w'], meta['rede_h'], meta['quadros']
POS = [np.array(p) for p in json.load(open(seq / 'objeto.json'))['pos']]
L = np.array([0.2126, 0.7152, 0.0722], np.float32)
comp = lambda x: x / (1 + np.maximum(x @ L, 0))[..., None]
ld = lambda p: np.fromfile(p, np.float16).reshape(H, W, 4)[..., :3].astype(np.float32)
obj = lambda i: np.fromfile(seq / f'{i:04d}_prof.raw', np.float32).reshape(H, W) > 0.02
gy, gx = np.mgrid[0:H, 0:W].astype(np.float32)
def mapa(grav, m):
    jogo = {json.loads(l)['jogo']: json.loads(l)['seq'] for l in open(m) if l.strip()}
    r = {}
    for l in (grav / 'meta.jsonl').read_text().splitlines()[1:]:
        try: x = json.loads(l)
        except json.JSONDecodeError: continue
        if x.get('jogo') in jogo: r[jogo[x['jogo']]] = x['q']
    return r
def edicao(grav, mp):
    if (grav / '0000_saida.raw').exists():
        return lambda i: comp(ld(grav / f'{i:04d}_saida.raw')) - comp(ld(seq / f'{i:04d}_orig.raw'))
    def f(i):
        if i not in mp: return None
        k = mp[i]; o = ld(grav / f'{k:04d}_orig.raw'); return comp(o + ld(grav / f'{k:04d}_edicao.raw')) - comp(o)
    return f
es = edicao(gs, mapa(gs, ms)); ev = edicao(gv, mapa(gv, mv) if (gv / 'meta.jsonl').exists() else {})
num = den = 0.0; quadros = 0
for f in range(8, N):
    m_ = np.fromfile(seq / f'{f:04d}_mv.raw', np.float32).reshape(H, W, 2)
    mx, my = gx + m_[..., 0], gy + m_[..., 1]
    ok = (mx >= 0) & (mx <= W - 1) & (my >= 0) & (my <= H - 1)
    era = cv2.remap(obj(f - 1).astype(np.float32), mx, my, cv2.INTER_NEAREST) > 0.5
    faixa = ok & era & ~obj(f)
    s, e = es(f), ev(f)
    if s is None or e is None or faixa.sum() < 500: continue
    d = POS[f] - POS[f - 1]
    ox, oy = (gx + d[0]).astype(np.float32), (gy + d[1]).astype(np.float32)
    so = cv2.remap(s, ox, oy, cv2.INTER_LINEAR)
    dentro = cv2.remap(obj(f).astype(np.float32), ox, oy, cv2.INTER_NEAREST) > 0.5
    m = faixa & dentro
    a = (e - s)[m]; b = (so - s)[m]
    num += float(np.sum(a * b)); den += float(np.sum(b * b)); quadros += 1
print(json.dumps({'variante': gv.name, 'fantasma_vs_sincrono': round(num / den, 3) if den else None, 'quadros': quadros}))
