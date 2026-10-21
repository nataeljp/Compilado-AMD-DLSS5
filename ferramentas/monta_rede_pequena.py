# Rede pequena do DLSS 5 (nvngx_dlssnr 310.9.16, recurso WEIGHTS_CC_SMALL_FP8, 31 blocos) no layout da rede grande
# (WEIGHTS_HT, 71 blocos): cada bloco da pequena tem o formato exato de um bloco da grande; os pesos dela entram nas
# posicoes equivalentes (codificador: os 2 ultimos de cada estagio, o ultimo reduz; decodificador: os 2 primeiros, o
# primeiro amplia; ViT, entrada e saida iguais) e o runtime pula as posicoes que sobram. O arquivo hibrido passa pelo
# conversor MIT do guentra (externo/guentra) sem mudanca. So leitura das DLLs (nada e executado).
#
# uso: python monta_rede_pequena.py <saida.json do mapa> <pasta de saida dos pesos convertidos>
#                                   <nvngx_dlssnr.dll 310.9.16> <nvngx_dlssnr.dll 310.8> [--identidade]
import hashlib, importlib.util, json, sys
from pathlib import Path
import pefile

RAIZ = Path(__file__).resolve().parents[1]
ARGS = [a for a in sys.argv[1:] if not a.startswith('--')]
DLL_NOVA, DLL_310_8 = Path(ARGS[2]), Path(ARGS[3])

# bloco da rede pequena -> posicao na grande
MAPA = {0: 0, 1: 3, 2: 4, 3: 7, 4: 8, 5: 13, 6: 14, 7: 21, 8: 22, 9: 29, 10: 30,
        **{11 + i: 31 + i for i in range(8)}, 19: 39, 20: 40, 21: 41, 22: 48, 23: 49, 24: 56, 25: 57,
        26: 62, 27: 63, 28: 66, 29: 69, 30: 70}
PULADOS = sorted(set(range(1, 70)) - set(MAPA.values()) - {39})


def recurso(dll, nome):
    pe = pefile.PE(str(dll), fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_RESOURCE']])
    dados = Path(dll).read_bytes()
    for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        for n in t.directory.entries:
            if str(n.name) == nome:
                l = n.directory.entries[0].data.struct
                off = pe.get_offset_from_rva(l.OffsetToData)
                return dados[off:off + l.Size]
    raise KeyError(nome)


def registros(blob, conv):
    # mesmo enquadramento do conversor, sem exigir o conjunto de 153 nomes
    out, cur = {}, 8
    while cur < len(blob):
        nl = conv._u64(blob, cur)
        nome = blob[cur + 8:cur + 8 + nl].decode('ascii')
        span = conv._u64(blob, cur + 8 + nl)
        corpo = cur + 16 + nl
        tam = conv._u64(blob, corpo + 8)
        out[nome] = (corpo + 20, tam)
        cur = corpo + span
    return out


def main():
    spec = importlib.util.spec_from_file_location('conv', RAIZ / 'externo/guentra/convert_dll.py')
    conv = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(conv)
    ht = bytearray(recurso(DLL_NOVA, 'WEIGHTS_HT'))
    small = recurso(DLL_NOVA, 'WEIGHTS_CC_SMALL_FP8')
    rh, rs = registros(bytes(ht), conv), registros(small, conv)
    trocados = []
    identidade = '--identidade' in sys.argv   # controle: a grande pelo mesmo caminho, sem trocar nada
    for nome, (off_s, tam_s) in ({} if identidade else rs).items():
        partes = nome.split('.')
        b = int(partes[0][5:])
        alvo = '.'.join([f'block{MAPA[b]}'] + partes[1:])
        off_h, tam_h = rh[alvo]
        if tam_h != tam_s:
            raise SystemExit(f'tamanho diferente: {nome} ({tam_s}) -> {alvo} ({tam_h})')
        ht[off_h:off_h + tam_h] = small[off_s:off_s + tam_s]
        trocados.append([nome, alvo])
    # DLL 310.8 com o arquivo de pesos trocado pelo hibrido (mesmo tamanho), so para o conversor ler
    base = bytearray(DLL_310_8.read_bytes())
    ao = conv.ARCHIVE_OFFSET
    if conv._u64(base, ao) != len(ht):
        raise SystemExit('o arquivo de pesos da 310.8 nao tem o tamanho do hibrido')
    base[ao:ao + len(ht)] = ht
    saida = Path(ARGS[1])
    saida.mkdir(parents=True, exist_ok=True)
    dll_h = saida.parent / 'nvngx_dlssnr_hibrido.bin'
    dll_h.write_bytes(base)
    conv.KNOWN_NVIDIA_SHA = hashlib.sha256(base).hexdigest()
    cache = conv.convert_nvidia_dll(dll_h, saida / 'pesos', layout_mode='amd-consumer-derived')
    Path(ARGS[0]).write_text(json.dumps({'mapa_pequena_para_grande': MAPA, 'pulados': PULADOS,
                                             'registros_trocados': len(trocados), 'pesos': str(cache)}, indent=1))
    print('registros trocados', len(trocados), 'de', len(rs), '| pulados:', ','.join(map(str, PULADOS)))
    print('pesos:', cache)


if __name__ == '__main__':
    main()

