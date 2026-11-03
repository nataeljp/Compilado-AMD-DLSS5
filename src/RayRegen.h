// AMD FSR Ray Regeneration (FidelityFX SDK 2.4.0, denoiser de machine learning) sobre a edicao da rede, no tamanho dela. Entrada "imagem inteira": a imagem pronta como luz. Entrada "brilho da rede": o ganho
// que a rede aplica, sem a curva ganho x brilho do quadro (volta pixel a pixel depois), com a media de cada bloco conservada e as luzes como outro material; o jogo vai como albedo, ou branco. Roda no overlay.
// So com RX 9000 ou mais nova, DirectX 12, Windows 11; sem isso Filtra devolve nullptr e a edicao segue sem filtro.
// O jogo nao entrega rugosidade, material, normais nem camera: rugosidade e material fixos, camera identidade (estimar a camera do movimento foi testado: nao muda nada com o movimento do
// jogo e derruba o historico com o fluxo optico), normais calculadas da profundidade. Na captura de buffer a profundidade, o FOV e o movimento originais do jogo; na da API, a profundidade do
// jogo (D3D11 e D3D12, escala relativa) e fluxo optico; na de video e no Vulkan nao ha profundidade: fixa e normais de frente.
#pragma once
#include "Comum.h"
#include <string>

namespace ov {

// na ordem das chaves do SDK (FfxApiConfigureDenoiserKey, 1 a 6): normal, estabilidade, radiancia maxima, recorte, gaussiana, oclusao
constexpr int kParamsRayRegen = 6;

// o que a captura entrega ao denoiser neste quadro
struct EntradaRayRegen
{
    Tex* mov = nullptr;          // pixels, do atual ao anterior
    float escMovX = 1.f, escMovY = 1.f; // de pixels de mov para pixels da edicao
    Tex* dist = nullptr;
    bool distCrua = false;       // true: a profundidade como o jogo escreveu; false: distancia radial (gravacao)
    float perto = 0.f, longe = 0.f; // 0 = o jogo nao disse: escala relativa
    bool infinita = true;
    bool invertidaConhecida = false, invertida = true;
    float tanY = 0.f;            // 0 = o jogo nao disse: 60 graus
    float jitterX = 0.f, jitterY = 0.f; // pixels de render: a profundidade e o movimento do FSR vem assim, a cor nao. Medido: sem isto o Z treme 38x mais com a camera parada
    bool salto = false;
};

class RayRegen
{
  public:
    ~RayRegen();
    void Cria(ID3D12Device* d);
    // na lista da entrada, antes da rede: a profundidade e o movimento deste quadro
    void Prepara(ID3D12GraphicsCommandList* c, Descritores& desc, Gpu& gpu, Tex& orig, const EntradaRayRegen& e);
    // a edicao filtrada (RGBA16F, estado SRV), ou nullptr se o denoiser nao estiver disponivel ou o Prepara nao rodou neste quadro. luz: Temporal.h LuzNaTela
    Tex* Filtra(ID3D12GraphicsCommandList* c, Descritores& desc, Gpu& gpu, const std::wstring& pasta, Tex& edicao, Tex& orig, UINT luz, float forca, int modo, const float* params, int vista, UINT vistaW, UINT vistaH, float branco);
    // para o painel: de onde vem a profundidade
    const std::string& Estado() const { return estado; }
    // a fracao da tela com alguma coisa na profundidade do jogo, medida neste quadro (captura da API); false = nao mediu
    bool PegaCobertura(float& c)
    {
        c = cobertura;
        const bool n = coberturaNova;
        coberturaNova = false;
        return n;
    }

    // as entradas e a saida do ultimo despacho (--dump)
    Tex sinal, albedo, prof, normais, saida, resultado, profAnterior, movZ;
    Tex depura; // a visao de depuracao do SDK (RGB a cor, A = pixel escrito), no tamanho da tela

  private:
    bool Abre(Gpu& gpu, const std::wstring& pasta, UINT w, UINT h, bool comDepuracao);
    void Fecha();
    void Recursos(ID3D12Device* d, UINT w, UINT h);
    void MedePontos(bool invertidaConhecida);
    Passe pCurvaZera, pCurvaSoma, pCurvaFecha, pEntra, pGeo, pBrutos, pNormais, pBaixa, pSai;
    Tex baixa; // RG32F, 1/16: a media do sinal e do filtrado por bloco
    Tex caixas, curva; // a curva ganho x brilho do quadro: soma por faixa de brilho (32 x 2) e o log2 do ganho (32 x 1)
    static constexpr UINT kBrutosW = 32, kBrutosH = 18;
    Tex brutos; // RG32F, poucos pontos: a profundidade como o jogo escreveu (Z invertido? so a captura da API precisa) e o Z (a escala da visao de depuracao)
    float zTipico = 0.f, limiteVisao = 0.f, cobertura = 1.f;
    bool coberturaNova = false;
    bool comVista = false;
    ComPtr<ID3D12Resource> brutosLeitura;
    bool brutosGravados = false;
    bool invertida = true;           // medido, e so muda depois de muitos quadros seguidos dizendo o contrario
    int contraInvertida = 0;
    bool prepPronto = false, prepReset = true;
    int fonte = 0, semProf = 0;      // 0 fixa, 1 distancia gravada, 2 profundidade do jogo, 3 a ultima (faltou neste quadro)
    bool fonteReal = false;
    float prepTan = 0.577f;
    UINT quadrosFiltro = 0, faltas = 0, resets = 0;
    std::string estado;
    HMODULE dll = nullptr;
    void* ctx = nullptr;
    void* fnCria = nullptr;
    void* fnDestroi = nullptr;
    void* fnDespacha = nullptr;
    void* fnConfigura = nullptr;
    void* fnConsulta = nullptr;
    float padrao[kParamsRayRegen] = {}, aplicado[kParamsRayRegen] = {};
    UINT ctxW = 0, ctxH = 0, indice = 0;
    bool falhou = false, ctxDepura = false, temAnterior = false;
};

} // namespace ov