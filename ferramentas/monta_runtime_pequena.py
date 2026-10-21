# Pasta de runtime da rede pequena: hardlinks do bin\runtime e so os pesos dos blocos mapeados (e os nao-bloco, ViT/post70)
# trocados pelos convertidos de monta_rede_pequena.py. Cada arquivo trocado e APAGADO antes (o hardlink nunca e escrito).
# uso: python monta_runtime_pequena.py <mapa.json> <pasta dos pesos convertidos> <pasta de saida do runtime>
import json, os, re, shutil, sys
from pathlib import Path
import numpy as np

mapa = json.loads(Path(sys.argv[1]).read_text())
mapeados = {int(v) for v in mapa['mapa_pequena_para_grande'].values()}
pesos, rt = Path(sys.argv[2]), Path(sys.argv[3])
orig = Path(__file__).resolve().parents[1] / 'bin' / 'runtime'
if rt.exists():
    shutil.rmtree(rt)
for raiz, _, arqs in os.walk(orig):
    d = rt / Path(raiz).relative_to(orig)
    d.mkdir(parents=True, exist_ok=True)
    for a in arqs:
        os.link(Path(raiz) / a, d / a)
ast = rt / 'native-game-tiled-assets'
n16 = n32 = 0
for f in sorted(pesos.glob('*.f32')):
    if f.name.endswith('.audit.f32'):
        continue
    stem = f.name[:-4]
    m = re.match(r'block(\d+)', stem)
    if m and int(m.group(1)) not in mapeados:
        continue
    v = np.fromfile(f, np.float32)
    for ext in ('.f16', '.f32'):
        p = ast / (stem + ext)
        if p.exists():
            os.remove(p)
    if (ast / (stem + '.f16')).exists() is False and np.array_equal(v.astype(np.float16).astype(np.float32), v) and \
            (orig / 'native-game-tiled-assets' / (stem + '.f16')).exists():
        v.astype(np.float16).tofile(ast / (stem + '.f16')); n16 += 1
    else:
        v.tofile(ast / (stem + '.f32')); n32 += 1
print('trocados: .f16', n16, ' .f32', n32)
