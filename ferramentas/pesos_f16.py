# Monta uma pasta de runtime com os pesos em meia precisao onde isso NAO perde nada: cada .f32 cujos valores sao todos
# exatos em FP16 vira .f16 (o carregador do runtime le path.f16 quando o .f32 nao existe e expande para f32 -- mesmo
# valor, bit a bit). Os demais .f32 e o block0-ffn.f32 (reescrito pelos controles do adaptador) ficam como estao.
#
# uso: python ferramentas/pesos_f16.py <runtime_origem> <runtime_destino>
import json, os, shutil, sys
import numpy as np

orig, dest = sys.argv[1], sys.argv[2]
rel_assets = 'native-game-tiled-assets'
total_antes = total_depois = 0
convertidos = mantidos = 0
for raiz, pastas, arqs in os.walk(orig):
    rel = os.path.relpath(raiz, orig)
    if rel == '.':
        # so o runtime: as DLLs da raiz e a pasta dos assets (o resto do pacote antigo fica de fora)
        pastas[:] = [p for p in pastas if p == rel_assets]
        arqs = [a for a in arqs if a.lower().endswith('.dll')]
    os.makedirs(os.path.join(dest, rel), exist_ok=True)
    for a in arqs:
        s = os.path.join(raiz, a)
        d = os.path.join(dest, rel, a)
        tam = os.path.getsize(s)
        total_antes += tam
        f16ok = False
        if a.endswith('.f32') and a != 'block0-ffn.f32' and os.path.basename(raiz) == rel_assets:
            v = np.fromfile(s, np.float32)
            h = v.astype(np.float16)
            if np.all(h.astype(np.float32) == v) and np.all(np.isfinite(v)):
                h.tofile(d[:-4] + '.f16')
                total_depois += h.nbytes
                convertidos += 1
                f16ok = True
        if not f16ok:
            if not os.path.exists(d):
                try:
                    os.link(s, d)
                except OSError:
                    shutil.copy2(s, d)
            total_depois += tam
            if a.endswith('.f32'):
                mantidos += 1
print(json.dumps({'convertidos_f16': convertidos, 'mantidos_f32': mantidos, 'MB_antes': round(total_antes / 1e6, 1),
                  'MB_depois': round(total_depois / 1e6, 1)}))
