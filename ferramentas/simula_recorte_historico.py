# Simulacao 1D do historico da propria rede (modo 1 do lmxxf) com e sem o recorte pela vizinhanca.
# Cena: camera em 3a pessoa -- o personagem fica parado na tela e o fundo (neve) passa atras dele, para a esquerda. A rede escurece a neve
# ao lado do personagem (efeito de contexto, "halo") e cintila um pouco a cada quadro. O historico traz a saida anterior
# pelo movimento do fundo; as guardas do runtime (cor de entrada +-0,08 e profundidade +-5%) aceitam neve sobre neve.
# Mede: o rastro (diferenca da saida para a resposta sem historico longe do personagem) e a cintilacao que sobra.
# Uso: python ferramentas/simula_recorte_historico.py
import numpy as np

N, PASSO, QUADROS = 600, 26, 60          # pixels, px que o fundo anda por quadro da rede (18 fps), quadros
OBJ = (330, 400)                          # o personagem, parado na tela
HALO, LARG = 0.15, 40                     # escurecimento que a rede poe na neve ao lado dele, largura
RUIDO = 0.02                              # cintilacao da rede por quadro
PESO = 0.73974609375 / (1 + np.exp(-1.0))  # forca 1 x 0,7397 x sigmoide(logit = 1)

def fundo(x):  # neve com textura suave
    return 0.82 + 0.04 * np.sin(x * 0.07) + 0.02 * np.sin(x * 0.31)

def cena(t, comHalo=True):
    x = np.arange(N)
    entrada = fundo(x + PASSO * t)  # o fundo anda para a esquerda: sai do lado do halo para o fundo livre
    prof = np.full(N, 10.0)
    entrada[OBJ[0]:OBJ[1]] = 0.2
    prof[OBJ[0]:OBJ[1]] = 2.0
    halo = np.zeros(N)
    if comHalo:
        halo[OBJ[0] - LARG:OBJ[0]] = -HALO * np.linspace(0, 1, LARG)
    return entrada, prof, halo

def recorta(hist, atual, gama):
    if gama <= 0:
        return hist
    p = np.pad(atual, 1, mode='edge')
    viz = np.stack([p[:-2], p[1:-1], p[2:]])
    mu, sd = viz.mean(0), viz.std(0)
    ext = gama * sd + 0.001
    k = np.maximum(np.abs(hist - mu) / ext, 1.0)
    return mu + (hist - mu) / k

def roda(gama, comHalo=True, semente=1):
    """as saidas dos quadros 10.. e as respostas limpas (entrada + halo, sem cintilacao e sem historico)"""
    rng = np.random.default_rng(semente)
    saidaAnt = entAnt = profAnt = None
    saidas, limpas = [], []
    for t in range(QUADROS):
        entrada, prof, halo = cena(t, comHalo)
        crua = entrada + halo + rng.normal(0, RUIDO, N)
        saida = crua.copy()
        if saidaAnt is not None:
            x = np.arange(N)
            q = x + np.where(prof > 5, PASSO, 0)  # de onde o pixel veio no quadro anterior
            ok = (q >= 0) & (q < N)
            qc = np.clip(q, 0, N - 1)
            ok &= np.abs(entAnt[qc] - entrada) <= 0.08
            ok &= np.abs(profAnt[qc] - prof) <= 0.05 * np.maximum(profAnt[qc], prof)
            hist = recorta(saidaAnt[qc], crua, gama)
            saida = np.where(ok, crua + (hist - crua) * PESO, crua)
        if t >= 10:
            saidas.append(saida)
            limpas.append(entrada + halo)
        saidaAnt, entAnt, profAnt = saida, entrada, prof
    return np.array(saidas), np.array(limpas)

if __name__ == '__main__':
    print(f'peso do historico {PESO:.3f}, halo {HALO}, cintilacao da rede {RUIDO}, fundo anda {PASSO} px/quadro')
    longe = np.r_[0:OBJ[0] - LARG - 4]  # o fundo longe do personagem: ali a rede nao poe halo nenhum
    print(f'{"recorte":>8} {"rastro medio":>13} {"rastro max":>11} {"cintilacao":>11} {"desvio da resposta":>19}')
    for g in (0, 0.5, 1.0, 1.5, 2.0, 4.0):
        com, limpa = roda(g, True)
        sem, _ = roda(g, False)  # a mesma cintilacao, sem o halo: a diferenca e so o rastro do halo
        r = np.abs(com - sem)[:, longe]
        cint = np.std(sem[:, longe] - np.array([cena(t + 10, False)[0] for t in range(len(sem))])[:, longe], axis=0).mean()
        desvio = np.abs(com - limpa)[:, OBJ[0] - LARG:OBJ[0]].mean()
        print(f'{("sem" if g == 0 else f"{g:.2f}"):>8} {r.mean():13.4f} {r.max():11.4f} {cint:11.4f} {desvio:19.4f}')
    print(f'sem historico: rastro 0, cintilacao {RUIDO:.4f}')
