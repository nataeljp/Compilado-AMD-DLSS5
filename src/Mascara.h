// Mascara de desenvolvedor (o "Developer Masking" + "Show Masks" do painel da NVIDIA): pele, cabelo, o resto do
// personagem e objetos separados do cenario, cada grupo com os proprios controles DENTRO da rede (o alfa da entrada leva
// grupo e peso; o kernel de entrada do runtime troca tom, estrutura, pele, outro e estilo por pixel). Nada e misturado
// depois da rede.
//  - o Classificador (thread propria, ONNX Runtime + DirectML) diz o que e cada coisa num quadro reduzido;
//  - a resposta dele chega alguns quadros depois e e trazida ate o quadro atual pelo deslocamento acumulado (o mesmo do
//    Temporal: movimento do jogo na captura de buffer, fluxo optico no video); a borda e ajustada pela cor e pela
//    profundidade do quadro atual (bilateral conjunto) e o estado segue o movimento entre uma classificacao e outra.
#pragma once
#include "Classificador.h"
#include <chrono>

namespace ov {

class Mascara
{
  public:
    static constexpr int kGrupos = 4; // 1 pele, 2 cabelo, 3 personagem (roupa, armadura, acessorios, animais), 4 objetos
    void Cria(ID3D12Device* d);
    // carrega o classificador (bin\ml) na primeira vez que a mascara e ligada; false = sem ele (erro em Erro())
    bool Inicia(const std::wstring& pastaMl, ID3D12Device* d);
    void Encerra() { cls.Encerra(); }
    bool Pronta() const { return cls.Pronto(); }
    // corte de cena no quadro `corte` (o primeiro da cena nova): o estado sai e a classificacao em voo, se e de um
    // quadro anterior ao corte (cena velha), e descartada; a de um quadro da cena nova fica
    void Reinicia(UINT64 corte)
    {
        valido = false;
        descarta = emVoo && idEmVoo < corte;
        pendente = false; // quadro reduzido de antes (a mascara estava desligada): nao vai mais
    }
    // a cada quadro, na lista 0 depois do movimento: rastreia a mascara ate este quadro (com a classificacao nova, se
    // chegou) e, se o classificador esta livre e passaram `taxa` quadros, reduz este quadro para ele.
    // mov/conf: movimento ate o quadro anterior (nullptr = parado); z: distancia da camera (captura de buffer)
    void Quadro(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex& fed, Tex* mov, Tex* conf, Tex* z,
                float exposicao, UINT64 quadro, int taxa);
    // depois que a GPU terminou o quadro: entrega o quadro reduzido ao classificador
    void DepoisDoQuadro();
    // grava grupo e peso de cada pixel no alfa da entrada da rede (estado COMMON antes e depois)
    void Codifica(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex& entrada);
    // pinta os grupos sobre a saida (Mostrar mascara)
    void Mostra(ID3D12GraphicsCommandList* c, Descritores& desc, ID3D12Device* d, Tex& saida, bool hdr, float escala);
    bool Valida() const { return valido; }
    Tex* Estado() { return valido ? &estado[qualEstado] : nullptr; } // rgba = pele, cabelo, personagem, objeto
    Tex* Sabe() { return valido ? &sabe[qualEstado] : nullptr; }     // quanto o estado sabe de cada pixel (R8)
    std::string Status() const;
    std::string Erro() const { return cls.Erro(); }

  private:
    Classificador cls;
    Passe pReduz, pAcumula, pLimpa, pEstado, pPreenche, pCodifica, pMostra;
    Tex reduzida, guia, obs, obsLimpa, desl[2], estado[2], sabe[2], bruto, conhecido;
    bool guiaComZ = false; // o guia de t0 tem a distancia da camera (captura de buffer)
    float ganhoReduz = 1.f; // clareia o quadro do classificador (cena escura), pela mediana que ele mediu
    UINT qualDesl = 0, qualEstado = 0;
    ComPtr<ID3D12Resource> leitura, envio;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT pegada{};
    bool pendente = false; // quadro reduzido na leitura, esperando a GPU
    bool emVoo = false;    // o classificador esta com um quadro (o deslocamento acumula desde ele)
    bool zeraDesl = false;
    bool valido = false;
    bool descarta = false;
    UINT64 idPendente = 0, idEmVoo = 0, ultimoEnvio = 0, quadroAtual = 0;
    std::chrono::steady_clock::time_point tUltimoEnvio{};
    std::vector<uint8_t> resultado;
    UINT w = 0, h = 0;
    void Garante(ID3D12Device* d, UINT w_, UINT h_);
};

} // namespace ov
