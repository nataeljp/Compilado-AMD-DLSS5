// Classificador da mascara de desenvolvedor: o que e pele, cabelo, personagem e objeto em cada quadro.
// Dois modelos ONNX (bin\ml) no ONNX Runtime com DirectML, no mesmo dispositivo D3D12 do overlay e numa fila propria,
// numa thread propria: o quadro nunca espera a classificacao (ela chega alguns quadros depois e e trazida pelo movimento).
//  - cena.onnx (LR-ASPP MobileNetV3, torchvision, BSD-3): probabilidade de pessoa, animal e objeto em 640x360
//  - deteccao.onnx (YOLOX-tiny, Apache-2.0): caixas de pessoa -- acha o personagem que a cena perde (armadura, de costas)
//  - partes.onnx (MediaPipe selfie multiclass, Apache-2.0): no recorte de cada personagem, a silhueta (1 - fundo), cabelo,
//    pele do corpo, pele do rosto, roupa e acessorios
// Entrada: o quadro em sRGB 8 bits, kEntW x kEntH. Saida: kSaiW x kSaiH RGBA8, R = pele, G = cabelo, B = o resto do
// personagem (roupa, armadura, acessorios, animais), A = objeto; o que sobra (1 - soma) e o cenario.
#pragma once
#include "Comum.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

struct OrtApi;
struct OrtEnv;
struct OrtSession;
struct OrtMemoryInfo;
struct OrtStatus;

namespace ov {

class Classificador
{
  public:
    static constexpr UINT kEntW = 1280, kEntH = 720, kSaiW = 640, kSaiH = 360;
    // pasta: onde estao onnxruntime.dll, DirectML.dll, cena.onnx e partes.onnx. false = sem eles (sem mascara)
    bool Inicia(const std::wstring& pasta, ID3D12Device* dev);
    void Encerra();
    ~Classificador() { Encerra(); }
    bool Pronto() const { return ok; }
    bool Livre() const { return ok && !ocupado; }
    // copia o quadro (kEntW x kEntH x 4, sRGB) e o classifica na thread; id = o quadro do overlay
    bool Envia(const uint8_t* rgba, size_t pitch, uint64_t id);
    // o resultado mais novo ainda nao pego (kSaiW x kSaiH x 4)
    bool Pega(std::vector<uint8_t>& saida, uint64_t& id);
    double Ms() const { return ms.load(); }
    int Pessoas() const { return pessoas.load(); }
    // mediana da luma (sRGB 0..1) do ultimo quadro recebido, antes do ganho do classificador: o quadro seguinte sai da
    // GPU ja clareado por ela (quadro escuro reduzido a 8 bits vira degraus que os modelos nao leem)
    float Mediana() const { return mediana.load(); }
    std::string Erro() const { std::lock_guard<std::mutex> g(muErro); return erro; }

  private:
    bool Falha(OrtStatus* s, const char* onde);
    void Laco();
    void Classifica();
    bool Sessao(const std::wstring& arquivo, OrtSession** s);

    HMODULE modDml = nullptr, modOrt = nullptr;
    const OrtApi* ort = nullptr;
    OrtEnv* env = nullptr;
    OrtSession* cena = nullptr;
    OrtSession* partes = nullptr;
    OrtSession* det = nullptr; // deteccao.onnx (YOLOX, opcional): caixas de pessoa
    int ladoDet = 416;
    OrtMemoryInfo* memCpu = nullptr;
    ComPtr<IUnknown> dml;
    ComPtr<ID3D12CommandQueue> fila;
    std::string nomeEntCena, nomeSaiCena, nomeEntPartes, nomeSaiPartes, nomeEntDet, nomeSaiDet;
    bool ok = false;
    std::string erro;
    mutable std::mutex muErro;

    std::thread thr;
    std::mutex mu;
    std::condition_variable cv;
    bool sair = false, temEntrada = false, temSaida = false;
    std::atomic<bool> ocupado{false};
    std::vector<uint8_t> entrada, saida;
    uint64_t idEntrada = 0, idSaida = 0;
    std::atomic<double> ms{0};
    std::atomic<int> pessoas{0};
    std::atomic<float> mediana{0.35f};
};

} // namespace ov
