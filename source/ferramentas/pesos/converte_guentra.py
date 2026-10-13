"""Roda o conversor MIT do guentra (linux/dlssnr/convert_dll.py) sobre a NOSSA nvngx_dlssnr.dll, modo
'amd-consumer-derived' (layout C32 e skips derivados dos kernels AMD), e compara cada arquivo com os nossos.

uso: python converte_guentra.py <nvngx_dlssnr.dll> <convert_dll.py do guentra> <saida> [pesos_para_comparar]
   (convert_dll.py: https://github.com/guentra/dlss5-amd-hip-linux, linux/dlssnr/convert_dll.py, MIT)
"""
import hashlib
import importlib.util
import json
import sys
from pathlib import Path

DLL = Path(sys.argv[1])
CONVERSOR = Path(sys.argv[2])
SAIDA = Path(sys.argv[3])
NOSSOS = Path(sys.argv[4]) if len(sys.argv) > 4 else SAIDA

spec = importlib.util.spec_from_file_location("convert_dll", CONVERSOR)
conv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(conv)

cache = conv.convert_nvidia_dll(DLL, SAIDA, layout_mode="amd-consumer-derived", progress=lambda *a: print(*a, flush=True))
print("cache:", cache)

h = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
comparacao = {}
for f in sorted(Path(cache).rglob("*.f32")):
    nosso = NOSSOS / f.name
    comparacao[f.name] = "igual" if nosso.exists() and h(nosso) == h(f) else ("diferente" if nosso.exists() else "so_guentra")
for f in NOSSOS.glob("*.f32"):
    comparacao.setdefault(f.name, "so_nosso")
resumo = {k: sum(1 for v in comparacao.values() if v == k) for k in ("igual", "diferente", "so_guentra", "so_nosso")}
(SAIDA / "comparacao_guentra.json").write_text(
    json.dumps({"resumo": resumo, "arquivos": comparacao}, ensure_ascii=False, indent=1), encoding="utf-8")
print(json.dumps(resumo))
print("diferentes:", [k for k, v in comparacao.items() if v != "igual"])
