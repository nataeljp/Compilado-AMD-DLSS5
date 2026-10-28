// Geracao de quadros NEURAL da AMD (FSR Frame Generation 4.x, "Redstone", RDNA 4) pela API do FidelityFX: o loader da
// pasta do jogo (amd_fidelityfx_loader_dx12.dll + amd_fidelityfx_framegeneration_dx12.dll). O provedor neural gera so 1
// quadro por real (numGeneratedFrames > 1 e aceito e ignorado: ferramentas/teste_fg_ml). 2x: o meio entre o real anterior
// e este. 4x: dois contextos -- A gera o meio M; B recebe real, M, real, M... como quadros seguidos (o movimento do real
// pela metade, a camera no meio do caminho) e gera 0,25 e 0,75 (contra a verdade na cena de teste: 22,9 / 25,4 / 26,0 dB).
// Precisa da camera do jogo (posicao e eixos, os do preparo da geracao FSR dele): sem ela a neural cai para 19 dB e fica a
// analitica.
#pragma once
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdint>

namespace dlss5ov {

// as funcoes da API da AMD (as originais: o modulo engancha as do loader -- chamar pelos ganchos mandaria a nossa geracao
// para o tratamento da do jogo)
struct ApiAmd
{
    void* cria = nullptr;      // PfnFfxCreateContext
    void* destroi = nullptr;   // PfnFfxDestroyContext
    void* configura = nullptr; // PfnFfxConfigure
    void* despacha = nullptr;  // PfnFfxDispatch
    void* consulta = nullptr;  // PfnFfxQuery
    bool Tem() const { return cria && destroi && configura && despacha && consulta; }
};

// a camera de um quadro no mundo (ffxDispatchDescFrameGenerationPrepareCameraInfo / PrepareV2)
struct CameraJogo
{
    float pos[3] = {}, cima[3] = {}, direita[3] = {}, frente[3] = {};
};

// a camera de um despacho de preparo da geracao do jogo (o antigo com a CameraInfo ligada, ou o V2); false: nao e um
bool CameraDoPreparo(const void* desc, CameraJogo& cam);

// um quadro real para a neural: profundidade e movimento (as copias do modulo, ainda validas no Present) e a camera
struct QuadroNeural
{
    ID3D12Resource* prof = nullptr; // NON_PIXEL_SHADER_RESOURCE
    ID3D12Resource* mov = nullptr;  // NON_PIXEL_SHADER_RESOURCE
    UINT renderW = 0, renderH = 0;
    float jitterX = 0, jitterY = 0, movEscX = 1, movEscY = 1, msQuadro = 16.6f, perto = 0.1f, longe = 1e5f, fovY = 1.0472f;
    CameraJogo cam;
};

class GeracaoNeural
{
  public:
    ~GeracaoNeural() { Destroi(); }
    // cria os contextos (A, e B para o 4x) no provedor 4.x do loader; false: sem a neural (Erro diz por que)
    bool Cria(const ApiAmd& api, ID3D12Device* dev, UINT telaW, UINT telaH, UINT maxRW, UINT maxRH, DXGI_FORMAT fmt,
              uint32_t flagsApi);
    void Destroi();
    bool Pronta() const { return a != nullptr; }
    const char* Provedor() const { return provedor; }
    const char* Erro() const { return erro; }
    // na apresentacao: gera n quadros (1 = 2x; 3 = 4x intercalado, em ordem de tempo) entre o real anterior e este, na
    // lista c. id sobe 1 por quadro real (outro passo reinicia a geracao). cor = o backbuffer final deste real; semHud =
    // a mesma imagem antes do HUD (ou nullptr): a AMD tira dela o HUD e os pos-efeitos (final - sem HUD) e os poe parados
    // nos gerados -- como a analitica com currentBackBuffer_HUDLess. Estados = FFX_API_RESOURCE_STATE_* (os do SDK)
    bool Gera(ID3D12GraphicsCommandList* c, uint64_t id, const QuadroNeural& q, const CameraJogo& camAnterior,
              ID3D12Resource* cor, uint32_t corEstado, ID3D12Resource* semHud, uint32_t semHudEstado,
              ID3D12Resource* const* saidas, uint32_t saidaEstado, uint32_t n, bool reset, uint32_t transferencia,
              float lumMin, float lumMax);

  private:
    ApiAmd api;
    void* a = nullptr; // ffxContext: o meio entre os reais
    void* b = nullptr; // ffxContext: o 4x intercalado (real, M, real, M...)
    UINT telaW = 0, telaH = 0;
    uint64_t ultimoA = 0, ultimoB = 0; // o ultimo frameID de cada um (o seguinte tem de ser +1, senao reinicia)
    // 4x: as tres geracoes partem da imagem sem HUD e o HUD do real entra parado depois (onde o final difere da sem HUD,
    // o pixel do final -- como a analitica da AMD). Com o HUD no meio, o contexto B o tratava como cena e o movia
    Microsoft::WRL::ComPtr<ID3D12Device> dev;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rsUi;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> psoUi;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heapUi;
    UINT incUi = 0, proximoUi = 0;
    bool CriaUi();
    void PoeUi(ID3D12GraphicsCommandList* c, ID3D12Resource* final_, ID3D12Resource* semHud, ID3D12Resource* saida);
    char provedor[64] = {};
    char erro[160] = {};
};

// a reprojecao dos gerados acima do limite de movimento (no lugar da geracao da AMD, que mistura os dois reais quando o
// movimento passa do que ela acha: fantasma e fundo duplicado no giro rapido da camera). Cada pixel dos dois reais vai
// para a posicao dele no tempo do gerado pelo movimento do jogo (que ja traz a camera e os objetos): o atual volta
// (1 - t) do movimento dele, o anterior anda t (o movimento dele ao contrario: velocidade constante); na disputa pelo
// mesmo pixel ganha o mais perto (profundidade), e no empate o real mais perto no tempo. Buracos (o que nenhum dos
// dois mostra) pegam o fundo vizinho. Sem fluxo optico e sem rede: sem blocos 8x8 e sem limite de velocidade. Ate o
// limite fica o gerado da AMD; acima entra a reprojecao aos poucos, inteira no dobro do limite. O HUD fica intacto
class Reprojecao
{
  public:
    bool Cria(ID3D12Device* dev);
    bool Pronta() const { return pso[0] != nullptr; }
    // depois da geracao, nos m gerados (UNORDERED_ACCESS, em ordem de tempo) entre o real anterior e este. q: a
    // profundidade e o movimento deste real (NON_PIXEL_SHADER_RESOURCE). final_/semHud: COMPUTE_READ (semHud pode
    // ser nullptr). limite: pixels de tela entre os reais (0 = so a reprojecao). Guarda este real para o seguinte
    void Gera(ID3D12GraphicsCommandList* c, uint64_t id, const QuadroNeural& q, bool profInvertida, ID3D12Resource* final_,
              ID3D12Resource* semHud, ID3D12Resource* const* saidas, uint32_t m, float limite, bool reset);

  private:
    Microsoft::WRL::ComPtr<ID3D12Device> dev;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rs;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso[6]; // limpa, espalha atual/anterior, cor atual/anterior, resolve
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> chave, rep, anterior; // profundidade vencedora, cor reprojetada, o real anterior
    UINT inc = 0, proximo = 0;
    uint64_t idAnterior = 0;
    bool temAnterior = false;
    QuadroNeural qAnterior{};
    bool Garante(Microsoft::WRL::ComPtr<ID3D12Resource>& r, UINT w, UINT h, DXGI_FORMAT f, bool uav);
};

} // namespace dlss5ov
