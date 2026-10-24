# Simulacao da grade afim (as mesmas contas de src/Temporal.cpp: kAfim e kAfimAplica) contra o transporte por pixel,
# numa cena sintetica: fundo texturizado parado e um disco texturizado que anda 14 px entre o quadro que a rede viu (t0)
# e o atual (t1). A "rede" e uma edicao construida aqui (luz suave pela posicao, puxada de cor nos claros e realce de
# detalhe), nao a do DLSS 5: mostra o mecanismo (a grade e recalculada da cor atual, o transporte leva a do quadro velho)
# e a sensibilidade a erro no vetor de movimento; a fidelidade com a rede de verdade se mede no jogo.
# Mede o RMS (espaco comprimido) contra a edicao que a rede daria em t1, em volta do objeto e na area descoberta/coberta.
# O transporte aqui nao tem o preenchimento de oclusao do overlay: na area descoberta ele leva a edicao do objeto.
# uso: python simula_grade_afim.py
import numpy as np
rng = np.random.default_rng(1)
H, W, CEL, F = 192, 320, 16, 8
LUMA = np.array([0.2126, 0.7152, 0.0722])
def lum(x): return x @ LUMA
def comp(x): return x / (1.0 + np.maximum(lum(x), 0.0))[..., None]
def guia(o):
    l = np.maximum(lum(np.maximum(o, 0)), 0); return (l / (1 + l)) ** (1 / 2.2)
def suave(n, s):
    from numpy.fft import fft2, ifft2, fftfreq
    fy, fx = np.meshgrid(fftfreq(H), fftfreq(W), indexing='ij')
    g = np.exp(-(fx**2 + fy**2) * (2 * np.pi * s)**2 / 2)
    return np.real(ifft2(fft2(n) * g))
# cena: fundo (textura media + fina) e um objeto (disco texturizado, outra cor)
yy, xx = np.mgrid[0:H, 0:W].astype(float)
fundo = np.stack([0.35 + 0.25 * suave(rng.standard_normal((H, W)), 6) * 8 for _ in range(3)], -1)
fundo += 0.08 * rng.standard_normal((H, W, 1))  # textura fina
fundo = np.clip(fundo * np.array([0.9, 1.0, 0.8]), 0.02, 2.0)
tex_obj = np.clip(np.array([0.7, 0.35, 0.2]) + 0.12 * rng.standard_normal((H, W, 1)), 0.02, 2)
def quadro(cx):
    m = ((xx - cx) ** 2 + (yy - H / 2) ** 2) < 40 ** 2
    obj = np.roll(tex_obj, int(cx), axis=1)  # a textura anda com o objeto
    return np.where(m[..., None], obj, fundo), m
# "rede": relighting suave na posicao + mudanca de cor por brilho + detalhe (realce de alta frequencia)
luz = 1.0 + 0.6 * np.exp(-((xx - 200) ** 2 + (yy - 60) ** 2) / (2 * 70 ** 2))
def rede(o):
    x = comp(o)
    y = x * luz[..., None] ** 0.5
    y = y + 0.05 * np.array([-1, 0.4, 0.8]) * guia(o)[..., None]      # puxa para o ciano nos claros
    lx = lum(x); det = lx - suave(lx, 1.5)
    y = y + 0.6 * det[..., None]                                         # detalhe (estrutura local)
    return y - x                                                         # a edicao, comprimida
def ajusta(o, e, rig=0.002):
    gw, gh = -(-W // CEL), -(-H // CEL)
    x = comp(o); y = x + e
    zb = np.clip(guia(o) * F - 0.5, 0, F - 1)
    S = np.zeros((gh, gw, F, 22))
    for b in range(F):
        wb = np.clip(1 - np.abs(zb - b), 0, 1)
        for cy in range(gh):
            for cx in range(gw):
                sl = (slice(cy * CEL, (cy + 1) * CEL), slice(cx * CEL, (cx + 1) * CEL))
                w = wb[sl].ravel(); X = x[sl].reshape(-1, 3); Y = y[sl].reshape(-1, 3)
                S[cy, cx, b] = np.concatenate([[w.sum()], (w[:, None] * X).sum(0), (w[:, None] * Y).sum(0),
                                               ((w[:, None] * X).T @ X).ravel()[[0, 1, 2, 4, 5, 8]],
                                               ((w[:, None] * X).T @ Y).ravel()])
    # 1-2-1 nas celulas vizinhas
    k = np.array([1, 2, 1]) / 4
    Sp = np.pad(S, ((1, 1), (1, 1), (0, 0), (0, 0)), mode='edge')
    S2 = sum(k[i] * k[j] * Sp[j:j + gh, i:i + gw] for i in range(3) for j in range(3))
    A = np.zeros((gh, gw, F, 3, 4)); conf = np.zeros((gh, gw, F))
    for cy in range(gh):
        for cx in range(gw):
            for b in range(F):
                s = S2[cy, cx, b]; n = s[0]
                if n < 1e-3: A[cy, cx, b] = np.c_[np.eye(3), np.zeros(3)]; continue
                sx, sy = s[1:4], s[4:7]; u = s[7:13]
                sxx = np.array([[u[0], u[1], u[2]], [u[1], u[3], u[4]], [u[2], u[4], u[5]]])
                sxy = s[13:22].reshape(3, 3)
                lam = rig * n + 1e-6
                M = np.zeros((4, 4)); M[:3, :3] = sxx + lam * np.eye(3); M[:3, 3] = sx; M[3, :3] = sx; M[3, 3] = n
                for c in range(3):
                    r = np.r_[sxy[:, c], sy[c]]; r[c] += lam
                    A[cy, cx, b, c] = np.linalg.solve(M, r)
                conf[cy, cx, b] = min(n / 16, 1)
    return A, conf
def aplica(A, conf, x, pos, z01):
    gh, gw = conf.shape[:2]
    g = pos / CEL - 0.5; g0 = np.floor(g).astype(int); f = g - g0
    zb = np.clip(z01 * F - 0.5, 0, F - 1); z0 = np.floor(zb).astype(int); fz = zb - z0
    acc = np.zeros_like(x); ws = np.zeros(x.shape[:-1])
    for j in range(2):
        for i in range(2):
            for kk in range(2):
                cx = np.clip(g0[..., 0] + i, 0, gw - 1); cy = np.clip(g0[..., 1] + j, 0, gh - 1)
                b = np.minimum(z0 + kk, F - 1)
                wt = ((f[..., 0] if i else 1 - f[..., 0]) * (f[..., 1] if j else 1 - f[..., 1]) *
                      (fz if kk else 1 - fz) * conf[cy, cx, b])
                M = A[cy, cx, b]
                y = np.einsum('...ij,...j->...i', M[..., :3], x) + M[..., 3]
                acc += wt[..., None] * y; ws += wt
    out = np.where((ws > 1e-4)[..., None], x + (acc / np.maximum(ws, 1e-9)[..., None] - x) * np.clip(ws * 4, 0, 1)[..., None], x)
    return out
def rms(a, m=None):
    d = a if m is None else a[m]
    return float(np.sqrt(np.mean(d ** 2)))
# t0 e t1 (o objeto andou 14 px); a rede respondeu em t0
o0, m0 = quadro(120); o1, m1 = quadro(134)
e0 = rede(o0); verdade = rede(o1)  # a edicao que a rede daria em t1
A, conf = ajusta(o0, e0)
x0 = comp(o0)
pos0 = np.stack([xx + 0.5, yy + 0.5], -1)
res0 = x0 + e0 - aplica(A, conf, x0, pos0, guia(o0))
print(f"ajuste em t0: edicao rms {rms(e0):.4f}, residuo rms {rms(res0):.4f} ({100*rms(res0)/rms(e0):.0f}% da edicao)")
# deslocamento de t1 ate t0: o objeto -14 px em x; o fundo parado
D = np.zeros((H, W, 2)); D[m1, 0] = -14
borda = (m0 & ~m1) | (m1 & ~m0)                     # descoberto/coberto
def dilata(m, n):
    for _ in range(n):
        m = m | np.roll(m, 1, 0) | np.roll(m, -1, 0) | np.roll(m, 1, 1) | np.roll(m, -1, 1)
    return m
viz = dilata(m1 | m0, 10)                              # em volta do objeto
x1 = comp(o1)
def transporta(e, D):
    q = np.clip(np.round(pos0 - 0.5 + D).astype(int), 0, [W - 1, H - 1])
    return e[q[..., 1], q[..., 0]]
for erro in (0.0, 1.5):
    Dn = D + (erro * rng.standard_normal((H, W, 2)) if erro else 0)
    e_transp = transporta(e0, Dn)                       # o caminho por pixel (sem preenchimento de oclusao)
    e_afim = aplica(A, conf, x1, pos0 + Dn, guia(o1)) - x1
    e_mista = e_afim + 0.25 * transporta(res0, Dn)
    print(f"erro do vetor {erro} px | rms contra a edicao verdadeira de t1 (em volta do objeto / no descoberto):")
    for nome, e in (("transporte por pixel", e_transp), ("grade afim", e_afim), ("grade afim + 0,25 residuo", e_mista)):
        print(f"   {nome:28s} {rms(e - verdade, viz):.4f} / {rms(e - verdade, borda):.4f}")
