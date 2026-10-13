"""Gera a pasta de pesos do runtime do lmxxf a partir da NOSSA nvngx_dlssnr.dll.

Fonte dos bytes: raw/weights/<idx>_<nome>.bin (tools/extrai_pesos.py), contêiner [20 B][payload][20 B]. Os 153
registros têm o mesmo payload_size do weights-index.json do autor (conferido 153/153 em 2026-10-09).

Os decodificadores são os do lmxxf (MIT, lmxxf/Development), chamados sem modificação: eles leem caminhos
'release/...' relativos ao diretório atual, então montamos essa árvore em <saida>/work e rodamos com cwd lá.
A montagem de cada arquivo final repete a dos scripts prepare_* do autor (citados em cada família).

Lacunas tratadas explicitamente (registradas no manifesto, nunca preenchidas em silêncio):
  - skip-channels.npy de C64/128/256: medido pelo autor numa GPU NVIDIA. Aqui usamos a MESMA ordem do skip da
    FFN (native_c64_reference.py:29) como HIPÓTESE, até conferir contra o runtime AMD.
  - layouts C32 do preblock (blocos 0-4, 66-69, post70, block4-ds): sem fórmula; ficam fora até serem
    recuperados contra o runtime AMD.

uso: rtk python tools/lmxxf_port/gera_assets.py [saida]
"""
import hashlib
import json
import os
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[2]
DEV = REPO / "lmxxf" / "Development"
RAW = REPO / "raw" / "weights"
SAIDA = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "lmxxf_port"
WORK = SAIDA / "work"
PESOS = SAIDA / "assets" / "weights"

manifesto = {"gerados": {}, "lacunas": {}, "hipoteses": {}}


def payload(nome):
    """Payload cru do registro <nome> (ex.: 'block23.layer2.layer'); block70.layer0 tem dois registros."""
    achados = sorted(RAW.glob(f"*_{nome}.bin"), key=lambda p: int(p.name.split("_")[0]))
    if not achados:
        raise SystemExit("registro ausente: " + nome)
    saida = []
    for f in achados:
        b = f.read_bytes()
        n = int.from_bytes(b[8:16], "little")
        assert len(b) == n + 40, f
        saida.append(b[20:20 + n])
    return saida


def grava_registro(destino, nome, qual=0):
    destino.parent.mkdir(parents=True, exist_ok=True)
    destino.write_bytes(payload(nome)[qual])


def salva(nome, arr, origem):
    arr = np.asarray(arr, dtype="<f4")
    arr.tofile(PESOS / nome)
    manifesto["gerados"][nome] = {"bytes": arr.nbytes, "origem": origem}


def canal(C):
    return "native-c%d" % C


def ordem_skip(C, tipo):
    """Candidatos de ordem de canal do skip: 'ffn' = skip da FFN C64+ (native_c64_reference.py:29),
    'c32' = padrão literal do skip C32 repetido por grupo de 16, 'id' = identidade."""
    s = np.arange(C)
    if tipo == "ffn":
        return (s // 16) * 16 + (s % 8) * 2 + (s % 16 // 8)
    if tipo == "c32":
        t = s % 16
        return (s // 16) * 16 + np.where(t < 8, (t // 2) * 4 + t % 2, ((t - 8) // 2) * 4 + 2 + t % 2)
    if tipo == "id":
        return s
    raise SystemExit("ordem desconhecida: " + tipo)


def layouts_por_formula(r):
    """Reimplementa as fábricas derive_native_* sem o bloco de módulo que exige os mapas medidos do autor."""
    def bits(n, positions):
        index = np.arange(n, dtype=np.int32)
        out = np.zeros(n, np.int32)
        for target, source in enumerate(positions):
            out |= ((index >> source) & 1) << target
        return out

    for C, base in ((64, 0x70a0), (128, 0x18120), (256, 0x58220)):
        d = C.bit_length() - 1
        group = list(range(12, d + 7))
        ffn = {
            "w1_input": bits(4 * C * C, [1, 0, 4, 5, 2] + group),
            "w1_hidden": bits(4 * C * C, [3, 6, 7, 8, 9, 10, 11] + list(range(d + 7, 2 * d + 2))),
            "w2_hidden": bits(128 * C, [1, 0, 4, 5, 2, 10, 11] + group),
            "w2_output": bits(128 * C, [3, 6, 7, 8, 9] + group),
            "w3_input": bits(C * C, [1, 0, 4, 5, 2] + list(range(d + 5, 2 * d))),
            "w3_output": bits(C * C, [3, 6, 7, 8, 9] + list(range(10, d + 5))),
        }
        n = 2 * C * C
        ds = {"input": bits(n, [1, 0, 4, 5, 2] + list(range(d + 6, 2 * d + 1))),
              "output": bits(n, [3, 6, 7, 8, 9] + list(range(10, d + 6)))}
        N = C * C
        i = np.arange(N)
        inputs = bits(N, [1, 0, 4, 5, 2] + list(range(d + 5, 2 * d)))
        outputs = bits(N, [3, 6, 7, 8, 9] + list(range(10, d + 5)))
        matrix = dict(v_offsets=base + (i // 1024) * 3072 + 2048 + i % 1024, v_input=inputs, v_output=outputs,
                      p_input=inputs, p_output=outputs)
        count = (C // 32) * 4096
        bias = dict(head=bits(count, list(range(12, 12 + d - 5))), query=bits(count, [5, 6, 10, 7, 1, 11]),
                    key=bits(count, [0, 3, 8, 4, 2, 9]))
        root = r / canal(C)
        for sub, data in (("ffn-layout/layout.npz", ffn), ("ds-layout/layout.npz", ds),
                          ("attention-layout/matrix-layout.npz", matrix), ("attention-layout/bias-layout.npz", bias)):
            (root / sub).parent.mkdir(parents=True, exist_ok=True)
            np.savez(root / sub, **data)
        np.save(root / "attention-layout/skip-channels.npy", ordem_skip(C, os.environ.get("SKIP_C64", "ffn")))
        manifesto["hipoteses"][f"native-c{C}/attention-layout/skip-channels.npy"] = (
            "ordem do skip da FFN (native_c64_reference.py:29); o autor mediu numa GPU NVIDIA - conferir contra o runtime AMD")


def gera():
    from native_c64_reference import unpack as unpack64
    from native_split_weights import unpack as unpack_split
    from native_split_reference import bits
    from decode_tinlayout_global import e4m3fn
    from native_vit_linear_reference import unpack_expand, unpack_residual
    from native_vit_qkv_reference import unpack as unpack_qkv
    from native_decoder_entry_reference import unpack as unpack_dec
    from native_upsample48_reference import unpack as unpack_up

    # encoder C64..C256, blocos 5-22 (prepare_native_front_chain.py:22-26)
    for b in range(5, 23):
        C = 256 if b >= 15 else 128 if b >= 9 else 64
        fw, qkv, pw, bias, scales, skip = unpack64(f"release/native-c{C}/block{b}.weights")
        salva(f"block{b}-ffn.f32", np.concatenate([fw[k].ravel() for k in ("W1", "W2", "W3", "skip")]), "prepare_native_front_chain.py:25")
        salva(f"block{b}-attention.f32", np.concatenate([m.ravel() for m in (*qkv, pw, bias, scales, skip)]), "prepare_native_front_chain.py:26")
    # descidas 8/14/22 (prepare_native_front_chain.py:27-41)
    l64 = np.load("release/native-c64/ffn-layout/layout.npz")
    raw8 = np.fromfile("release/native-c64/block8.weights", np.uint8)
    assert raw8.size == 69936
    ds8 = np.empty((128, 64), np.float32)
    ds8[l64["w1_hidden"][:8192], l64["w1_input"][:8192]] = e4m3fn(raw8[0xf130:])
    salva("block8-ds.f32", ds8, "prepare_native_front_chain.py:29")
    for b, C, off, size in ((14, 128, 0x30230, 229936), (22, 256, 0xa8440, 820288)):
        lay = np.load(f"release/native-c{C}/ds-layout/layout.npz")
        raw = np.fromfile(f"release/native-c{C}/block{b}.weights", np.uint8)
        assert raw.size == size
        m = np.empty((2 * C, C), np.float32)
        m[lay["output"], lay["input"]] = e4m3fn(raw[off:])
        salva(f"block{b}-ds.f32", m, "prepare_native_front_chain.py:33/37")
    # C512 split 23-30 e 40-47 (prepare_native_rgb128_gpu.py:12-15)
    for b in [*range(23, 31), *range(40, 48)]:
        fw, fp, qkv, bias, scales, projection = unpack_split("release/native-c512", b)
        salva(f"block{b}-ffwd.f32", np.concatenate([fw[k].ravel() for k in ("pre", "expand", "contract")]), "prepare_native_rgb128_gpu.py:13")
        salva(f"block{b}-ffwd-projection.f32", np.concatenate([fp["matrix"].ravel(), fp["skip"]]), "prepare_native_rgb128_gpu.py:14")
        salva(f"block{b}-attention.f32", np.concatenate([*[m.ravel() for m in qkv], projection["matrix"].ravel(), bias.ravel(), scales, projection["skip"]]), "prepare_native_rgb128_gpu.py:15")
    # head-matrix (validate_native_valid1080_pool_head.py: bits do block30-4)
    w = np.fromfile("release/native-c512/block30-4.weights", np.uint8)
    hm = np.empty((1024, 512), np.float32)
    hm[bits(524288, [3, 6, 7, 8, 9, 10, 11, 12, 13, 14]), bits(524288, [1, 0, 4, 5, 2, 15, 16, 17, 18])] = e4m3fn(w[:524288])
    salva("head-matrix.f32", hm, "validate_native_valid1080_pool_head.py")
    # ViT 31-38 (prepare_native_vit_block_gpu.py:21-26)
    for b in range(31, 39):
        salva(f"block{b}-expand.f32", unpack_expand(f"release/native-vit/block{b}-expand.weights"), "prepare_native_vit_block_gpu.py:23")
        for kind, inputs in (("contract", 4096), ("projection", 1024)):
            m, skip = unpack_residual(f"release/native-vit/block{b}-{kind}.weights", inputs)
            salva(f"block{b}-{kind}.f32", np.concatenate([m.ravel(), skip]), "prepare_native_vit_block_gpu.py:25")
        mats, sc = unpack_qkv(f"release/native-vit/block{b}-qkv.weights")
        salva(f"block{b}-qkv.f32", np.concatenate([*[m.ravel() for m in mats], sc]), "prepare_native_vit_block_gpu.py:26")
    # entrada do decoder 39 (prepare_native_rgb512_decoder.py:50)
    p = unpack_dec("release/decoder39.weights")
    salva("decoder39-weights.f32", np.concatenate([p[0].ravel(), p[1]]), "prepare_native_rgb512_decoder.py:50")
    # decoder C256..C64: corpo 49-55, 57-61, 63-65 e subidas 48/56/62 (prepare_native_valid1080_full_gpu.py / tail69)
    for b in [*range(49, 56), *range(57, 62), *range(63, 66)]:
        C = 256 if b < 56 else 128 if b < 62 else 64
        fw, qkv, pw, bias, scales, skip = unpack64(f"release/native-c{C}/block{b}.weights")
        salva(f"block{b}-ffn.f32", np.concatenate([fw[k].ravel() for k in ("W1", "W2", "W3", "skip")]), "prepare_native_rgb512_tail69_gpu.py")
        salva(f"block{b}-attention.f32", np.concatenate([m.ravel() for m in (*qkv, pw, bias, scales, skip)]), "prepare_native_rgb512_tail69_gpu.py")
    for b in (48, 56, 62):
        matrix, scale, body = unpack_up(f"release/upsample{b}/weights.bin")
        salva(f"block{b}-weights.f32", np.concatenate([matrix.ravel(), scale]), "prepare_native_valid1080_full_gpu.py:33")
        ffn, qkv, pw, bias, scales, skip = body
        salva(f"block{b}-ffn.f32", np.concatenate([ffn[k].ravel() for k in ("W1", "W2", "W3", "skip")]), "prepare_native_valid1080_full_gpu.py:42")
        salva(f"block{b}-attention.f32", np.concatenate([*[m.ravel() for m in qkv], pw.ravel(), bias.ravel(), scales, skip]), "prepare_native_valid1080_full_gpu.py:43")

    gera_c32()


def layouts_c32_hipotese():
    """Tabelas C32 por HIPÓTESE: as fórmulas de bits de C64..C256 com d=5 (as dimensões fecham: W1 128x32,
    W2 32x128, V/P 32x32, viés 64x64). O skip da atenção usa a mesma ordem literal do skip da FFN C32
    (native_c32_reference.py:27). O autor mediu estas tabelas numa GPU NVIDIA; conferir contra o runtime AMD."""
    def bits(n, positions):
        index = np.arange(n, dtype=np.int32)
        out = np.zeros(n, np.int32)
        for target, source in enumerate(positions):
            out |= ((index >> source) & 1) << target
        return out
    r = Path("release")
    (r / "preblock-ffn-byte-layout").mkdir(parents=True, exist_ok=True)
    (r / "preblock-attention-layout").mkdir(parents=True, exist_ok=True)
    np.savez(r / "preblock-ffn-byte-layout/layout.npz",
             w1_input=bits(4096, [1, 0, 4, 5, 2]), w1_hidden=bits(4096, [3, 6, 7, 8, 9, 10, 11]),
             w2_hidden=bits(4096, [1, 0, 4, 5, 2, 10, 11]), w2_output=bits(4096, [3, 6, 7, 8, 9]))
    io_in, io_out = bits(1024, [1, 0, 4, 5, 2]), bits(1024, [3, 6, 7, 8, 9])
    skip_map = ordem_skip(32, os.environ.get("SKIP_C32", "c32"))
    np.savez(r / "preblock-attention-layout/matrix-layout.npz", v_input=io_in, v_output=io_out,
             projection_input=io_in, projection_output=io_out, skip_channel=skip_map)
    np.savez(r / "preblock-attention-layout/bias-layout.npz",
             query=bits(4096, [5, 6, 10, 7, 1, 11]), key=bits(4096, [0, 3, 8, 4, 2, 9]))
    manifesto["hipoteses"]["preblock-*-layout (C32)"] = (
        "fórmulas de bits de C64..C256 com d=5; skip da atenção = ordem do skip da FFN C32; conferir contra o runtime AMD")


def gera_c32():
    layouts_c32_hipotese()
    from native_c32_reference import unpack_bytes
    from native_upsample66_reference import unpack as unpack66
    from native_post70_reference import unpack as unpack_post
    from preblock_mix_reference import unpack_mix
    from decode_tinlayout_global import e4m3fn

    def ffn(w1, w2, fs, mix=None):
        return np.concatenate([np.zeros(512) if mix is None else mix.ravel(), w1.ravel(), w2.ravel(), fs])

    def att(q, k, v, p, bias, scale, ats):
        return np.concatenate([q.ravel(), k.ravel(), v.ravel(), p.ravel(), bias.ravel(), [scale], ats])

    # bloco 0: C32 comum com 1024 bytes de mistura de entrada inseridos no byte 8208 (halves 4104:4616)
    raw0 = np.frombuffer(payload("block0.layer0.layer")[0], np.uint8)
    assert raw0.size == 21696
    w1, w2, q, k, v, p, bias, scale, fs, ats = unpack_bytes(np.concatenate([raw0[:8208], raw0[9232:]]))
    salva("block0-ffn.f32", ffn(w1, w2, fs, unpack_mix(raw0.view("<f2").astype(np.float32))), "C32 + mistura (preblock_mix_reference.unpack_mix)")
    salva("block0-attention.f32", att(q, k, v, p, bias, scale, ats), "C32 deslocado 1024 B")
    # blocos 1-4 e 67-69 (prepare_native_front_chain.py:10-13)
    for b in (1, 2, 3, 4, 67, 68, 69):
        raw = np.frombuffer(payload(f"block{b}.layer0.layer")[0], np.uint8)
        w1, w2, q, k, v, p, bias, scale, fs, ats = unpack_bytes(raw)
        salva(f"block{b}-ffn.f32", ffn(w1, w2, fs), "prepare_native_front_chain.py:12")
        salva(f"block{b}-attention.f32", att(q, k, v, p, bias, scale, ats), "prepare_native_front_chain.py:13")
        if b == 4:  # prepare_native_front_chain.py:15-18
            lay = np.load("release/preblock-ffn-byte-layout/layout.npz")
            m = np.empty((64, 32), np.float32)
            m[lay["w1_hidden"][:2048], lay["w1_input"][:2048]] = e4m3fn(raw[20656:22704])
            salva("block4-ds.f32", m, "prepare_native_front_chain.py:17")
    # subida 66 (prepare_native_rgb512_tail69_gpu.py:13-18)
    matrix, scale66, body = unpack66("release/upsample66/weights.bin")
    salva("block66-weights.f32", np.concatenate([matrix.ravel(), scale66]), "prepare_native_rgb512_tail69_gpu.py:14")
    w1, w2, q, k, v, p, bias, scale, fs, ats = body
    salva("block66-ffn.f32", ffn(w1, w2, fs), "prepare_native_rgb512_tail69_gpu.py:17")
    salva("block66-attention.f32", att(q, k, v, p, bias, scale, ats), "prepare_native_rgb512_tail69_gpu.py:17")
    # bloco 70 (prepare_native_post70_gpu.py:17-21), renomeado para post70-*
    body, sm, ss, head = unpack_post("release/post70/weights.bin")
    salva("post70-scales.f32", np.concatenate([sm, ss]), "prepare_native_post70_gpu.py:17")
    salva("post70-head.f32", head, "prepare_native_post70_gpu.py:17")
    w1, w2, q, k, v, p, bias, scale, fs, ats = body
    salva("post70-ffn.f32", ffn(w1, w2, fs), "prepare_native_post70_gpu.py:19")
    salva("post70-attention.f32", att(q, k, v, p, bias, scale, ats), "prepare_native_post70_gpu.py:20")


def main():
    PESOS.mkdir(parents=True, exist_ok=True)
    r = WORK / "release"
    # registros crus
    sys.path.insert(0, str(DEV))
    rel = r
    for p in [rel]:
        p.mkdir(parents=True, exist_ok=True)
    monta_release_sem_import(r)
    layouts_por_formula(r)
    os.chdir(WORK)
    gera()
    for nome in manifesto["gerados"]:
        manifesto["gerados"][nome]["sha256"] = hashlib.sha256((PESOS / nome).read_bytes()).hexdigest()
    (SAIDA / "manifesto_assets.json").write_text(json.dumps(manifesto, ensure_ascii=False, indent=1), encoding="utf-8")
    print(json.dumps({"gerados": len(manifesto["gerados"]), "lacunas": list(manifesto["lacunas"]),
                      "hipoteses": list(manifesto["hipoteses"])}, ensure_ascii=False))


def monta_release_sem_import(r):
    for b in [*range(5, 23), *range(49, 56), *range(57, 62), *range(63, 66)]:
        C = 256 if (15 <= b <= 22 or 49 <= b <= 55) else 128 if (9 <= b <= 14 or 57 <= b <= 61) else 64
        grava_registro(r / canal(C) / f"block{b}.weights", f"block{b}.layer0.layer")
    for b in [*range(23, 31), *range(40, 48)]:
        for i in range(4):
            grava_registro(r / "native-c512" / f"block{b}-{i}.weights", f"block{b}.layer{i}.layer")
    grava_registro(r / "native-c512" / "block30-4.weights", "block30.layer4.layer")
    for b in range(31, 39):
        for kind, layer in (("expand", 0), ("contract", 1), ("qkv", 2), ("projection", 4)):
            grava_registro(r / "native-vit" / f"block{b}-{kind}.weights", f"block{b}.layer{layer}.layer")
    grava_registro(r / "decoder39.weights", "block39.layer0.layer")
    for b in (48, 56, 62, 66):
        grava_registro(r / f"upsample{b}" / "weights.bin", f"block{b}.layer0.layer")
    (r / "post70").mkdir(parents=True, exist_ok=True)
    (r / "post70" / "weights.bin").write_bytes(next(p for p in payload("block70.layer0.layer") if len(p) == 21808))


if __name__ == "__main__":
    main()
