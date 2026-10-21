# Extrai do nvngx_dlssnr.dll 310.8 os pesos da cabeca de historico da rede (post70-history-head.f16, 32 coeficientes) e
# o escalar de mistura (post70-blend.f16), que o modo temporal 1 do runtime lmxxf exige. Mesma recuperacao do
# lmxxf (Development/HIP/experiments/post-history-gate/extract_gate.py, MIT): a linha 6 da cabeca 16x32 do registro do
# bloco 70 (21808 bytes, f16 a partir de 0x5130), sem ajuste nenhum. O arquivo de pesos e lido pelo parser do guentra
# (MIT, externo/guentra). Precisa do lmxxf clonado (runtime/lmxxf/compilar.ps1, ou LMXXF_FONTE=<pasta do lmxxf>).
#
# uso: python extrai_historico.py <nvngx_dlssnr.dll 310.8> <pasta native-game-tiled-assets>
import hashlib, importlib.util, json, os, sys
from pathlib import Path
import numpy as np

RAIZ = Path(__file__).resolve().parents[1]
LMXXF = Path(os.environ.get('LMXXF_FONTE', RAIZ / 'runtime/lmxxf/fonte'))
dll, saida = Path(sys.argv[1]), Path(sys.argv[2])

spec = importlib.util.spec_from_file_location('conv', RAIZ / 'externo/guentra/convert_dll.py')
conv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(conv)
sys.path.insert(0, str(LMXXF / 'Development'))
from native_split_reference import bits  # noqa: E402

dados = dll.read_bytes()
# a 310.8 pelo hash; ou o arquivo hibrido da rede pequena (monta_rede_pequena.py), cujo bloco 70 e o bloco 30 dela
if hashlib.sha256(dados).hexdigest() != conv.KNOWN_NVIDIA_SHA and not dll.name.startswith('nvngx_dlssnr_hibrido'):
    raise SystemExit('DLL nao e a 310.8 esperada')
tam = int.from_bytes(dados[conv.ARCHIVE_OFFSET:conv.ARCHIVE_OFFSET + 8], 'little')
arq = dados[conv.ARCHIVE_OFFSET:conv.ARCHIVE_OFFSET + tam]
regs = {r['name']: arq[r['payload_offset']:r['payload_offset'] + r['payload_size']] for r in conv.parse_archive(arq)}
corpo = [n for n, b in regs.items() if len(b) == 21808]
mistura = [n for n, b in regs.items() if len(b) == 2 and int.from_bytes(b, 'little') == 0x39eb] or     [n for n in regs if n == 'block70.layer0.blend_scale']
print('registro de 21808 bytes:', corpo, '| escalar 0x39eb:', mistura)
if len(corpo) != 1 or not mistura:
    raise SystemExit('registros nao encontrados de forma unica')
raw = regs[corpo[0]]
cabeca = np.zeros((16, 32), np.float32)
cabeca[bits(512, [2, 5, 6, 7]), bits(512, [0, 1, 3, 4, 8])] = np.frombuffer(raw[0x5130:], '<f2')
assert all(np.count_nonzero(cabeca[i]) == 32 for i in [0, 2, 4, 6])
saida.mkdir(parents=True, exist_ok=True)
cabeca[6].astype('<f2').tofile(saida / 'post70-history-head.f16')
(saida / 'post70-blend.f16').write_bytes(regs[mistura[0]])
print(json.dumps({'registro': corpo[0], 'escalar': mistura[0], 'blend': float(np.frombuffer(regs[mistura[0]], '<f2')[0]),
                  'cabeca_historico': [round(float(x), 5) for x in cabeca[6][:8]]}, ensure_ascii=False))
