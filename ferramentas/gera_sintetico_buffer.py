# Sequencia sintetica com os BUFFERS de um jogo (teste da captura de buffer sem jogo): a mesma camera de
# gera_sintetico.py (panoramica subpixel, aceleracao, zoom leve) sobre a foto, mais um objeto em primeiro plano (um
# recorte da propria foto) andando com movimento proprio -- oclusao e desoclusao de verdade, com profundidade.
# Grava por quadro: NNNN_orig.raw (RGBA16F, cor linear), NNNN_mv.raw (RG32F, pixels do atual ao anterior),
# NNNN_prof.raw (R32F, Z invertido infinito, perto = 0,1: d = 0,1 / z), e transf.json/objeto.json (verdade).
# uso: python gera_sintetico_buffer.py <foto> <pasta> <quadros> [--rapido] [--pessoa recorte.png]
#   --rapido: movimento extremo -- a camera da "flicks" de ate ~300 px por quadro com reversoes bruscas e zoom, e o
#   objeto cruza a tela a ate ~270 px por quadro (o teste de fantasma e de liga/desliga)
#   --pessoa: o objeto e um personagem recortado (PNG RGBA, alfa = a pessoa) no lugar do quadrado da foto; grava tambem
#   NNNN_gt.raw (R32F, a pessoa neste quadro) -- a verdade para medir a mascara de desenvolvedor em movimento
#   --laco: o quadro 0 traz o movimento real desde o ultimo (o jogo de teste repete a sequencia; sem isso a volta e um
#   quadro com movimento zero e a camera pulando)
#   foto .npy: luz linear em float (HxWx3), sem passar por sRGB -- um quadro HDR de jogo (ex.: o orig de uma gravacao do
#   overlay), com os realces acima de 1 que o PNG cortaria
import json, sys, math
from pathlib import Path
import cv2
import numpy as np
foto, pasta, N = sys.argv[1], Path(sys.argv[2]), int(sys.argv[3])
pasta.mkdir(parents=True, exist_ok=True)
if foto.endswith('.npy'):
    lin = np.ascontiguousarray(np.load(foto)[..., :3].astype(np.float32))
else:
    s = cv2.imread(foto, cv2.IMREAD_COLOR)[..., ::-1].astype(np.float32) / 255
    lin = np.where(s <= 0.04045, s / 12.92, ((s + 0.055) / 1.055) ** 2.4).astype(np.float32)
W, H = 1920, 1080
PERTO, Z_FUNDO, Z_OBJ = 0.1, 20.0, 2.0
TAM = 360  # lado do objeto (pixels do quadro)
obj = cv2.resize(cv2.GaussianBlur(lin[200:200 + 720, 2400:2400 + 720], (0, 0), 1.0), (TAM, TAM), interpolation=cv2.INTER_AREA)
alfaObj = np.ones((TAM, TAM), np.float32)
PESSOA = sys.argv[sys.argv.index('--pessoa') + 1] if '--pessoa' in sys.argv else None
if PESSOA:
    r = cv2.imread(PESSOA, cv2.IMREAD_UNCHANGED)
    c = r[..., 2::-1].astype(np.float32) / 255
    obj = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4).astype(np.float32)
    alfaObj = r[..., 3].astype(np.float32) / 255

RAPIDO = '--rapido' in sys.argv
LACO = '--laco' in sys.argv
# --giro: a camera GIRA em 3D (guinada e arfagem, como mirar no jogo), FOV horizontal de 90 graus. O fundo e uma esfera
#   em volta dela (a foto em azimute/elevacao): a distancia radial de cada ponto nao muda, mas o Z da camera (o que o
#   jogo entrega na profundidade) muda fora do centro a cada giro -- o caso do Gears girando rapido, que a panoramica 2D
#   (Z constante) nao reproduz. Guinada ate ~300 px/quadro no centro.
GIRO = '--giro' in sys.argv
# --anda: a camera tambem anda (como em volta do personagem no Gears): a distancia do objeto perto dela varia ate ~12% por
#   quadro e a do fundo ~2% -- a mesma superficie com outra distancia, que a confianca nao pode tomar por oclusao
ANDA = '--anda' in sys.argv


def z_obj(i):
    return Z_OBJ * (1 + 0.15 * math.sin(2 * math.pi * i / 8)) if ANDA else Z_OBJ


def z_fundo(i):
    return Z_FUNDO * (1 + 0.04 * math.sin(2 * math.pi * i / 12)) if ANDA else Z_FUNDO
TAN_X = 1.0
TAN_Y = TAN_X * H / W
RAD_PX = 2 * TAN_X / W  # radianos por pixel no centro da tela (a foto tem a mesma resolucao angular)


def giro(i):
    g = math.radians(45) * math.sin(2 * math.pi * i / 16)
    a = math.radians(8) * math.sin(2 * math.pi * i / 24)
    Ry = np.array([[math.cos(g), 0, math.sin(g)], [0, 1, 0], [-math.sin(g), 0, math.cos(g)]])
    Rx = np.array([[1, 0, 0], [0, math.cos(a), -math.sin(a)], [0, math.sin(a), math.cos(a)]])
    return Ry @ Rx

def camera(i):
    if RAPIDO:
        z = 1.0 + 0.15 * math.sin(2 * math.pi * i / 30)
        cx = 1920 + 900 * math.sin(2 * math.pi * i / 18)
        cy = 1080 + 400 * math.sin(2 * math.pi * i / 23)
        return np.array([[z, 0, cx - (W / 2 - 0.5) * z - 0.5], [0, z, cy - (H / 2 - 0.5) * z - 0.5]], np.float64)
    t = i / (N - 1)
    z = 1.9 - 0.2 * t
    cx = 1920 + 700 * math.sin(2.2 * t * math.pi) * t
    cy = 1080 + 250 * math.sin(1.3 * t * math.pi)
    return np.array([[z, 0, cx - (W / 2 - 0.5) * z - 0.5], [0, z, cy - (H / 2 - 0.5) * z - 0.5]], np.float64)

def objeto(i):
    if RAPIDO:
        return np.array([780 + 700 * math.sin(2 * math.pi * i / 16), 360 + 300 * math.sin(2 * math.pi * i / 11)])
    # canto superior esquerdo do objeto: atravessa a tela com velocidade variando (6..20 px/quadro) e sobe e desce
    t = i / (N - 1)
    return np.array([200 + 1300 * (t + 0.15 * math.sin(4 * math.pi * t)), 300 + 250 * math.sin(3 * math.pi * t)])

ys, xs = np.mgrid[0:H, 0:W].astype(np.float64)
cache = {}
transf, objs = [], []
if GIRO:
    # direcao de cada pixel no espaco da camera (raio unitario) e a foto com o desfoque de amostragem
    raio = np.stack([((xs + 0.5) * 2 / W - 1) * TAN_X, (1 - (ys + 0.5) * 2 / H) * TAN_Y, np.ones_like(xs)], -1)
    raio /= np.linalg.norm(raio, axis=-1, keepdims=True)
    fotoG = cv2.GaussianBlur(lin, (0, 0), 0.5)
    Hp, Wp = lin.shape[:2]
with open(pasta / 'meta.jsonl', 'w') as m:
    m.write(json.dumps({'rede_w': W, 'rede_h': H, 'janela_w': W, 'janela_h': H, 'hdr': 0, 'escala': 1.0,
                        'arquivos': {'orig': 'RGBA16F', 'mv': 'RG32F', 'prof': 'R32F'}, 'quadros': N,
                        'perto': PERTO, 'invertida': 1, 'infinita': 1,
                        **({'fovY': 2 * math.atan(TAN_Y)} if GIRO else {})}) + '\n')
    for i in range(N):
        ia = (i - 1) % N if LACO else max(i - 1, 0)
        if GIRO:
            mundo = raio @ giro(i).T
            az = np.arctan2(mundo[..., 0], mundo[..., 2])
            el = np.arcsin(np.clip(mundo[..., 1], -1, 1))
            q = cv2.remap(fotoG, (Wp / 2 + az / RAD_PX - 0.5).astype(np.float32), (Hp / 2 - el / RAD_PX - 0.5).astype(np.float32),
                          cv2.INTER_CUBIC, borderMode=cv2.BORDER_REFLECT)
            # movimento: onde a mesma direcao do mundo caia na tela do quadro anterior
            c = mundo @ giro(ia)
            mvx = ((c[..., 0] / c[..., 2]) / TAN_X + 1) * W / 2 - 0.5 - xs
            mvy = (1 - (c[..., 1] / c[..., 2]) / TAN_Y) * H / 2 - 0.5 - ys
            # esfera de raio Z_FUNDO: o Z da camera e o raio vezes o cosseno do angulo fora do eixo
            prof = (PERTO / (z_fundo(i) * raio[..., 2])).astype(np.float32)
            A = giro(i)
        else:
            A = camera(i)
            z = A[0, 0]
            if z not in cache: cache.clear(); cache[z] = cv2.GaussianBlur(lin, (0, 0), 0.5 * z)
            q = cv2.warpAffine(cache[z], A, (W, H), flags=cv2.INTER_CUBIC | cv2.WARP_INVERSE_MAP, borderMode=cv2.BORDER_REFLECT)
            # movimento do fundo: o ponto da foto que este pixel ve, no quadro anterior
            Ap = camera(ia)
            X = A[0, 0] * xs + A[0, 2]
            Y = A[1, 1] * ys + A[1, 2]
            mvx = (X - Ap[0, 2]) / Ap[0, 0] - xs
            mvy = (Y - Ap[1, 2]) / Ap[1, 1] - ys
            prof = np.full((H, W), PERTO / Z_FUNDO, np.float32)
        # objeto em primeiro plano (posicao subpixel por interpolacao)
        o = objeto(i)
        op = objeto(ia)
        M = np.array([[1, 0, o[0]], [0, 1, o[1]]], np.float64)
        camada = cv2.warpAffine(obj, M, (W, H), flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT, borderValue=0)
        mascara = cv2.warpAffine(alfaObj, M, (W, H), flags=cv2.INTER_LINEAR,
                                 borderMode=cv2.BORDER_CONSTANT, borderValue=0)[..., None]
        q = q * (1 - mascara) + camada * mascara
        dentro = mascara[..., 0] > 0.5
        mvx[dentro] = op[0] - o[0]
        mvy[dentro] = op[1] - o[1]
        prof[dentro] = PERTO / z_obj(i)
        if i == 0 and not LACO:
            mvx[:] = 0
            mvy[:] = 0
        rgba = np.concatenate([np.maximum(q, 0), np.ones((H, W, 1), np.float32)], -1).astype(np.float16)
        rgba.tofile(pasta / f'{i:04d}_orig.raw')
        np.stack([mvx, mvy], -1).astype(np.float32).tofile(pasta / f'{i:04d}_mv.raw')
        prof.tofile(pasta / f'{i:04d}_prof.raw')
        if PESSOA:
            mascara[..., 0].astype(np.float32).tofile(pasta / f'{i:04d}_gt.raw')
        transf.append(A.tolist())
        objs.append(o.tolist())
        m.write(json.dumps({'q': i, 'rede': 1, 'mov': 1, 't_ms': i * 16.7}) + '\n')
json.dump(transf, open(pasta / 'transf.json', 'w'))
json.dump({'tam': list(alfaObj.shape) if PESSOA else TAM, 'pos': objs}, open(pasta / 'objeto.json', 'w'))
if GIRO:  # o centro da tela: angulo entre os eixos oticos de dois quadros, em pixels do centro
    vc = [math.acos(min(1.0, float(giro(i)[:, 2] @ giro(i - 1)[:, 2]))) / RAD_PX for i in range(1, N)]
else:
    vc = [math.hypot(*(np.array(camera(i))[:, 2] - np.array(camera(i - 1))[:, 2])) for i in range(1, N)]
vo = [float(np.hypot(*(objeto(i) - objeto(i - 1)))) for i in range(1, N)]
print('quadros', N, W, 'x', H, '| camera ate %.0f px/quadro (mediana %.0f), objeto ate %.0f px/quadro' % (max(vc), float(np.median(vc)), max(vo)))
