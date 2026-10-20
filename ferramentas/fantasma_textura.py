# Fantasma como TEXTURA: na faixa recem-descoberta, a correlacao normalizada entre o detalhe fino da edicao (sem o
# desfoque de 6 px) e o detalhe fino da edicao do objeto no ponto dele que cobria o pixel. Preenchimento suave ou o
# detalhe do proprio fundo dao ~0; a edicao do objeto copiada da ~1. Sem referencia nenhuma (nao depende da rede).
# uso: python fantasma_textura.py <sequencia> <variante: gravacao|saida> [mapa]   (ou importar e chamar mede())
import json, sys
from pathlib import Path
import numpy as np, cv2

def prepara(seq):
    seq = Path(seq)
    meta = json.loads((seq / 'meta.jsonl').read_text().splitlines()[0])
    return seq, meta['rede_w'], meta['rede_h'], meta['quadros'], [np.array(p) for p in json.load(open(seq / 'objeto.json'))['pos']]

def mede(seq, ed, nome):
    seq, W, H, N, POS = prepara(seq)
    obj = lambda i: np.fromfile(seq / f'{i:04d}_prof.raw', np.float32).reshape(H, W) > 0.02
    gy, gx = np.mgrid[0:H, 0:W].astype(np.float32)
    hp = lambda x: x - cv2.GaussianBlur(x, (0, 0), 6)
    sab = sa = sb = 0.0; q = 0
    for f in range(8, N):
        m_ = np.fromfile(seq / f'{f:04d}_mv.raw', np.float32).reshape(H, W, 2)
        mx, my = (gx + m_[..., 0]).astype(np.float32), (gy + m_[..., 1]).astype(np.float32)
        ok = (mx >= 0) & (mx <= W - 1) & (my >= 0) & (my <= H - 1)
        faixa = ok & (cv2.remap(obj(f - 1).astype(np.float32), mx, my, cv2.INTER_NEAREST) > 0.5) & ~obj(f)
        e = ed(f)
        if e is None or faixa.sum() < 500: continue
        d = POS[f] - POS[f - 1]
        ox, oy = (gx + d[0]).astype(np.float32), (gy + d[1]).astype(np.float32)
        dentro = cv2.remap(obj(f).astype(np.float32), ox, oy, cv2.INTER_NEAREST) > 0.5
        m = cv2.erode((faixa & dentro).astype(np.uint8), np.ones((9, 9), np.uint8)) > 0  # longe da borda do objeto
        if m.sum() < 500: continue
        a = hp(e)[m]; b = hp(cv2.remap(e, ox, oy, cv2.INTER_LINEAR))[m]
        sab += float(np.sum(a * b)); sa += float(np.sum(a * a)); sb += float(np.sum(b * b)); q += 1
    r = sab / np.sqrt(sa * sb) if sa > 0 and sb > 0 else None
    print(json.dumps({'variante': nome, 'fantasma_textura': round(r, 3) if r is not None else None, 'quadros': q}), flush=True)

if __name__ == '__main__':
    import importlib.util
    seq, var = sys.argv[1], sys.argv[2]
    mapa = sys.argv[3] if len(sys.argv) > 3 else None
    spec = importlib.util.spec_from_file_location('av', str(Path(__file__).with_name('avalia_buffer.py')))
    sys.argv = ['avalia_buffer.py', seq]
    av = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(av)
    av.MAPA = mapa
    mede(seq, av.variante(var), Path(var).name)
