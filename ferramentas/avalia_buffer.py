# Avalia a sequencia de gera_sintetico_buffer.py (movimento e profundidade EXATOS, objeto em primeiro plano):
#   cintil   = RMS(e_f - e_{f-1} reprojetada pelo movimento exato) / RMS(e_f), onde o ponto e a mesma superficie
#   cintObj  = o mesmo so no objeto
#   rastro   = na faixa que o objeto acabou de descobrir (ate 4 quadros), RMS(e_f - edicao do mesmo fundo 6 quadros
#              depois, ja assentada) / RMS(dela)                         [o fantasma do objeto no fundo]
#   estavel  = a mesma medida no fundo longe do objeto (o piso: o quanto a edicao do fundo muda em 6 quadros)
#   pulso    = media de |log(RMS(e_f) / RMS(e_{f-1} reprojetada))| na mesma superficie: o "liga/desliga" quadro a quadro
#              (0 = a edicao tem a mesma forca em todo quadro)
#   fantasma = na faixa descoberta, quanto da edicao do OBJETO ficou no fundo: projecao de (e - fundo) em
#              (objeto - fundo), 0 = nada, 1 = a edicao do objeto inteira
# e = edicao no espaco comprimido x/(1+luma). Variantes:
#   gravacao do overlay (Ctrl+Shift+G ou --reproduz): NNNN_orig.raw + NNNN_edicao.raw; meta.jsonl com "jogo" (buffer)
#     ou sem ele (reproducao: gravado k = quadro k+1 da sequencia)
#   saida do jogo de teste: NNNN_saida.raw (a cor final com a edicao somada pelo jogo)
# uso: python avalia_buffer.py <sequencia> <variante> [...]
import json, sys
from functools import lru_cache
from pathlib import Path
import cv2
import numpy as np
L = np.array([0.2126, 0.7152, 0.0722], np.float32)
comp = lambda x: x / (1 + np.maximum(x @ L, 0))[..., None]
seq = Path(sys.argv[1])
meta = json.loads((seq / 'meta.jsonl').read_text().splitlines()[0])
W, H, N = meta['rede_w'], meta['rede_h'], meta['quadros']
T = [np.array(a) for a in json.load(open(seq / 'transf.json'))]
OBJ = json.load(open(seq / 'objeto.json'))
POS = [np.array(p) for p in OBJ['pos']]
AQ, ADIANTE, FAIXA = 8, 6, 4
gy, gx = np.mgrid[0:H, 0:W].astype(np.float32)

@lru_cache(maxsize=16)
def orig(i): return np.fromfile(seq / f'{i:04d}_orig.raw', np.float16).reshape(H, W, 4)[..., :3].astype(np.float32)
@lru_cache(maxsize=16)
def mv(i): return np.fromfile(seq / f'{i:04d}_mv.raw', np.float32).reshape(H, W, 2)
@lru_cache(maxsize=16)
def obj(i): return np.fromfile(seq / f'{i:04d}_prof.raw', np.float32).reshape(H, W) > 0.02  # objeto: d = 0,05

def amostra(img, mx, my, interp=cv2.INTER_LINEAR):
    return cv2.remap(img, mx.astype(np.float32), my.astype(np.float32), interp, borderMode=cv2.BORDER_REPLICATE)

def anterior(f):
    """posicao no quadro f-1 de cada pixel de f, e se e a mesma superficie (fundo->fundo, objeto->objeto)"""
    m = mv(f)
    mx, my = gx + m[..., 0], gy + m[..., 1]
    dentro = (mx >= 0) & (mx <= W - 1) & (my >= 0) & (my <= H - 1)
    oa = amostra(obj(f - 1).astype(np.float32), mx, my, cv2.INTER_NEAREST) > 0.5
    return mx, my, dentro, oa == obj(f)

def fundo_para(de, para):
    """fundo do quadro `de` levado ao quadro `para` pela camera (afim exata)"""
    A = np.vstack([T[para], [0, 0, 1]]); B = np.linalg.inv(np.vstack([T[de], [0, 0, 1]]))
    M = (B @ A)[:2]
    mx = M[0, 0] * gx + M[0, 1] * gy + M[0, 2]; my = M[1, 0] * gx + M[1, 1] * gy + M[1, 2]
    return mx, my, (mx >= 0) & (mx <= W - 1) & (my >= 0) & (my <= H - 1)

def variante(p):
    p = Path(p)
    if (p / '0000_saida.raw').exists() or any(p.glob('*_saida.raw')):
        def ed(i):
            f = p / f'{i:04d}_saida.raw'
            if not f.exists(): return None
            s = np.fromfile(f, np.float16).reshape(H, W, 4)[..., :3].astype(np.float32)
            return comp(s) - comp(orig(i))
        return ed
    linhas = []
    for l in (p / 'meta.jsonl').read_text().splitlines()[1:]:
        try:
            linhas.append(json.loads(l))
        except json.JSONDecodeError:
            pass  # linha cortada (o overlay fechou no meio da escrita)
    jogo = {}
    if MAPA:  # o jogo de teste anotou que quadro da sequencia cada quadro do jogo era
        jogo = {m['jogo']: m['seq'] for m in (json.loads(l) for l in Path(MAPA).read_text().splitlines() if l.strip())}
    mapa = {}
    for l in linhas:
        if l.get('jogo'):
            if l['jogo'] not in jogo: continue
            i = jogo[l['jogo']]
        else:
            i = l['q'] + 1
        mapa[i] = l['q']  # o ultimo gravado de cada quadro da sequencia (ultima volta)
    def ed(i):
        if i not in mapa: return None
        k = mapa[i]
        o = np.fromfile(p / f'{k:04d}_orig.raw', np.float16).reshape(H, W, 4)[..., :3].astype(np.float32)
        e = np.fromfile(p / f'{k:04d}_edicao.raw', np.float16).reshape(H, W, 4)[..., :3].astype(np.float32)
        return comp(o + e) - comp(o)
    return ed

def mede(nome, ed):
    ed = lru_cache(maxsize=12)(ed)
    rms = lambda x: float(np.sqrt(np.mean(x ** 2))) if x.size else float('nan')
    ci, co, ra, es, pu = [], [], [], [], []
    fn, fd = 0.0, 0.0
    vazios = 0
    idade = np.full((H, W), 1e9, np.float32) # quadros desde que o fundo foi descoberto
    for f in range(1, N):
        mx, my, dentro, mesma = anterior(f)
        # faixa descoberta: fundo agora que era objeto no quadro anterior; a idade anda com o fundo
        nova = ~obj(f) & dentro & ~mesma
        idade = np.where(nova, 0.0, np.where(~obj(f), amostra(idade, mx, my, cv2.INTER_NEAREST) + 1, 1e9)).astype(np.float32)
        faixa = idade < FAIXA
        if f < AQ: continue
        e, ea = ed(f), ed(f - 1)
        if e is None or ea is None: continue
        if rms(e) < 1e-4 or rms(ea) < 1e-4:
            vazios += 1  # quadro sem edicao (aquecimento): nao mede
            continue
        pa = amostra(ea, mx, my)
        v = dentro & mesma
        ci.append(rms((e - pa)[v]) / max(rms(e[v]), 1e-9))
        pu.append(abs(np.log(max(rms(e[v]), 1e-9) / max(rms(pa[v]), 1e-9))))
        vo = v & obj(f)
        if vo.any(): co.append(rms((e - pa)[vo]) / max(rms(e[vo]), 1e-9))
        if f + ADIANTE < N and (eb := ed(f + ADIANTE)) is not None:
            bx, by, bd = fundo_para(f + ADIANTE, f)
            ok = bd & ~(amostra(obj(f + ADIANTE).astype(np.float32), bx, by, cv2.INTER_NEAREST) > 0.5) & ~obj(f)
            ref = amostra(eb, bx, by)
            longe = ok & (cv2.dilate(obj(f).astype(np.uint8), np.ones((61, 61), np.uint8)) == 0)
            fx = ok & faixa
            if fx.sum() > 200:
                ra.append(rms((e - ref)[fx]) / max(rms(ref[fx]), 1e-9))
                # fantasma: a edicao do objeto no ponto dele que cobria este pixel (o objeto so translada)
                d = POS[f] - POS[f - 1]
                ox, oy = gx + d[0], gy + d[1]
                dentroObj = amostra(obj(f).astype(np.float32), ox, oy, cv2.INTER_NEAREST) > 0.5
                eo = amostra(e, ox, oy)
                m = fx & dentroObj
                if m.sum() > 200:
                    a_ = (e - ref)[m]; b_ = (eo - ref)[m]
                    fn += float(np.sum(a_ * b_)); fd += float(np.sum(b_ * b_))
            if longe.any(): es.append(rms((e - ref)[longe]) / max(rms(ref[longe]), 1e-9))
    r = lambda a: round(float(np.mean(a)), 3) if a else None
    print(json.dumps({'variante': nome, 'cintil': r(ci), 'cintObj': r(co), 'rastro': r(ra), 'estavel': r(es),
                      'pulso': r(pu), 'fantasma': round(fn / fd, 3) if fd > 0 else None,
                      'quadros': len(ci), 'sem_edicao': vazios}), flush=True)

MAPA = None
args = sys.argv[2:]
if '--mapa' in args:
    i = args.index('--mapa'); MAPA = args[i + 1]; del args[i:i + 2]
for a in args:
    mede(Path(a).name, variante(a))
