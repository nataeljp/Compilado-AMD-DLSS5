# Pacote da rede pequena (310.9.16) para o runtime: so os arquivos que mudam em relacao a rede grande + network-layout.txt
# (blocos ausentes e deslocamentos de janela medidos) + a cabeca de historico da propria pequena. O overlay monta a pasta
# completa (hardlinks da grande + estes) na primeira vez que a rede pequena e escolhida.
# uso: python monta_pacote_pequena.py <pasta de trabalho> <bin\runtime>
import json, re, shutil, subprocess, sys
from pathlib import Path
import numpy as np

AQUI = Path(__file__).parent
trab, runtime = Path(sys.argv[1]), Path(sys.argv[2])
# deslocamentos de janela da pequena (regra da grande para estagios de 2 blocos: codificador (0,3), decodificador (1,2);
# confirmada pela busca medida contra a NVIDIA, ferramentas/busca_deslocamentos.py)
SHIFT = '3:0,4:3,7:0,8:3,13:0,14:3,21:0,22:3,29:0,30:3,40:1,41:2,48:1,49:2,56:1,57:2,62:1,63:2,66:1,69:2'

subprocess.run([sys.executable, str(AQUI / 'monta_rede_pequena.py'), str(trab / 'mapa.json'), str(trab / 'saida')], check=True)
mapa = json.loads((trab / 'mapa.json').read_text())
mapeados = {int(v) for v in mapa['mapa_pequena_para_grande'].values()}
base = runtime / 'native-game-tiled-assets'
dst = runtime / 'pequena'
if dst.exists():
    shutil.rmtree(dst)
dst.mkdir(parents=True)
n = 0
for f in sorted((trab / 'saida' / 'pesos').glob('*.f32')):
    if f.name.endswith('.audit.f32'):
        continue
    stem = f.name[:-4]
    m = re.match(r'block(\d+)', stem)
    if m and int(m.group(1)) not in mapeados:
        continue
    v = np.fromfile(f, np.float32)
    if (base / (stem + '.f16')).exists() and np.array_equal(v.astype(np.float16).astype(np.float32), v):
        v.astype(np.float16).tofile(dst / (stem + '.f16'))
    else:
        v.tofile(dst / (stem + '.f32'))
    n += 1
# cabeca de historico (modo temporal 1) da propria pequena: o bloco 70 do arquivo hibrido e o bloco 30 dela
subprocess.run([sys.executable, str(AQUI / 'extrai_historico.py'), str(trab / 'nvngx_dlssnr_hibrido.bin'), str(dst)], check=True)
(dst / 'network-layout.txt').write_text('# rede pequena do DLSS 5 (nvngx_dlssnr 310.9.16, WEIGHTS_CC_SMALL_FP8) no layout da grande\n'
                                        f"skip={','.join(map(str, mapa['pulados']))}\nshift={SHIFT}\n", encoding='ascii')
print(f'{n} pesos + historico + network-layout.txt em {dst}')
