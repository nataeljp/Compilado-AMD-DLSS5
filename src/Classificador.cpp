#include "Classificador.h"
#include <onnxruntime_c_api.h>
#include <dml_provider_factory.h>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace ov {

namespace {
constexpr int kMaxPessoas = 3;          // recortes de partes por quadro (os maiores; cada um custa ~1 ms de GPU)
constexpr int kAreaMin = 80;            // pixels (640x360) para um personagem ganhar recorte
constexpr float kLimiarPessoa = 0.4f;
constexpr int kLado = 256;              // entrada do modelo de partes
constexpr float kNotaDet = 0.15f;       // nota minima do detector (objeto x pessoa)
} // namespace

bool Classificador::Falha(OrtStatus* s, const char* onde)
{
    if (!s)
        return false;
    {
        std::lock_guard<std::mutex> g(muErro);
        erro = Fmt("%s: %s", onde, ort->GetErrorMessage(s));
    }
    ort->ReleaseStatus(s);
    return true;
}

bool Classificador::Sessao(const std::wstring& arquivo, OrtSession** s)
{
    OrtSessionOptions* so = nullptr;
    if (Falha(ort->CreateSessionOptions(&so), "CreateSessionOptions"))
        return false;
    const OrtDmlApi* dmlApi = nullptr;
    bool bom = !Falha(ort->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL), "otimizacao") &&
               !Falha(ort->DisableMemPattern(so), "DisableMemPattern") &&
               !Falha(ort->SetSessionExecutionMode(so, ORT_SEQUENTIAL), "execucao sequencial") &&
               !Falha(ort->GetExecutionProviderApi("DML", ORT_API_VERSION, reinterpret_cast<const void**>(&dmlApi)),
                      "API do DirectML") &&
               !Falha(dmlApi->SessionOptionsAppendExecutionProvider_DML1(so, static_cast<IDMLDevice*>(dml.Get()),
                                                                          fila.Get()),
                      "DirectML na sessao") &&
               !Falha(ort->CreateSession(env, arquivo.c_str(), so, s), "CreateSession");
    ort->ReleaseSessionOptions(so);
    return bom;
}

bool Classificador::Inicia(const std::wstring& pasta, ID3D12Device* dev)
{
    Encerra();
    erro.clear();
    for (const wchar_t* f : {L"\\onnxruntime.dll", L"\\DirectML.dll", L"\\cena.onnx", L"\\partes.onnx"})
        if (GetFileAttributesW((pasta + f).c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            erro = "falta " + Utf8(pasta + f);
            return false;
        }
    // o DirectML.dll da pasta primeiro: o onnxruntime.dll importa pelo nome e pega o que ja esta carregado (o do sistema
    // e antigo demais para o DirectML do ONNX Runtime)
    if (!modDml)
        modDml = LoadLibraryExW((pasta + L"\\DirectML.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!modOrt)
        modOrt = LoadLibraryExW((pasta + L"\\onnxruntime.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!modDml || !modOrt)
    {
        erro = Fmt("DLL do classificador nao carregou (erro %lu)", GetLastError());
        return false;
    }
    using FnBase = const OrtApiBase*(ORT_API_CALL*)();
    using FnDml = HRESULT(WINAPI*)(ID3D12Device*, DML_CREATE_DEVICE_FLAGS, REFIID, void**);
    auto base = reinterpret_cast<FnBase>(GetProcAddress(modOrt, "OrtGetApiBase"));
    auto criaDml = reinterpret_cast<FnDml>(GetProcAddress(modDml, "DMLCreateDevice"));
    if (!base || !criaDml || !(ort = base()->GetApi(ORT_API_VERSION)))
    {
        erro = "onnxruntime.dll / DirectML.dll sem a versao esperada";
        return false;
    }
    ComPtr<IDMLDevice> d;
    if (FAILED(criaDml(dev, DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(&d))))
    {
        erro = "DMLCreateDevice falhou";
        return false;
    }
    dml = d;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&fila))))
    {
        erro = "fila do classificador";
        return false;
    }
    fila->SetName(L"classificador da mascara");
    if (Falha(ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "dlss5-mascara", &env), "CreateEnv") ||
        Falha(ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memCpu), "memoria") ||
        !Sessao(pasta + L"\\cena.onnx", &cena) || !Sessao(pasta + L"\\partes.onnx", &partes) ||
        (GetFileAttributesW((pasta + L"\\deteccao.onnx").c_str()) != INVALID_FILE_ATTRIBUTES &&
         !Sessao(pasta + L"\\deteccao.onnx", &det)))
    {
        Encerra();
        return false;
    }
    OrtAllocator* al = nullptr;
    ort->GetAllocatorWithDefaultOptions(&al);
    auto nome = [&](OrtSession* s, bool ent) {
        char* n = nullptr;
        if (ent ? Falha(ort->SessionGetInputName(s, 0, al, &n), "nome") : Falha(ort->SessionGetOutputName(s, 0, al, &n), "nome"))
            return std::string();
        std::string r = n;
        al->Free(al, n);
        return r;
    };
    nomeEntCena = nome(cena, true);
    nomeSaiCena = nome(cena, false);
    nomeEntPartes = nome(partes, true);
    nomeSaiPartes = nome(partes, false);
    if (det)
    {
        nomeEntDet = nome(det, true);
        nomeSaiDet = nome(det, false);
        OrtTypeInfo* ti = nullptr;
        const OrtTensorTypeAndShapeInfo* tsi = nullptr;
        int64_t dims[4] = {};
        if (!Falha(ort->SessionGetInputTypeInfo(det, 0, &ti), "detector") &&
            !Falha(ort->CastTypeInfoToTensorInfo(ti, &tsi), "detector") && !Falha(ort->GetDimensions(tsi, dims, 4), "detector"))
            ladoDet = int(dims[2]);
        if (ti)
            ort->ReleaseTypeInfo(ti);
    }
    entrada.resize(size_t(kEntW) * kEntH * 4);
    saida.resize(size_t(kSaiW) * kSaiH * 4);
    sair = temEntrada = temSaida = false;
    ocupado = false;
    ok = true;
    thr = std::thread([this] { Laco(); });
    return true;
}

void Classificador::Encerra()
{
    if (thr.joinable())
    {
        {
            std::lock_guard<std::mutex> g(mu);
            sair = true;
        }
        cv.notify_all();
        thr.join();
    }
    ok = false;
    if (ort)
    {
        if (cena)
            ort->ReleaseSession(cena);
        if (partes)
            ort->ReleaseSession(partes);
        if (det)
            ort->ReleaseSession(det);
        if (memCpu)
            ort->ReleaseMemoryInfo(memCpu);
        if (env)
            ort->ReleaseEnv(env);
    }
    cena = partes = det = nullptr;
    memCpu = nullptr;
    env = nullptr;
    dml.Reset();
    fila.Reset();
    // as DLLs ficam carregadas (descarregar o ONNX Runtime com threads do DirectML vivas derruba o processo)
}

bool Classificador::Envia(const uint8_t* rgba, size_t pitch, uint64_t id)
{
    if (!ok || ocupado)
        return false;
    {
        std::lock_guard<std::mutex> g(mu);
        for (UINT y = 0; y < kEntH; ++y)
            memcpy(entrada.data() + size_t(y) * kEntW * 4, rgba + y * pitch, size_t(kEntW) * 4);
        idEntrada = id;
        temEntrada = true;
        ocupado = true;
    }
    cv.notify_all();
    return true;
}

bool Classificador::Pega(std::vector<uint8_t>& s, uint64_t& id)
{
    std::lock_guard<std::mutex> g(mu);
    if (!temSaida)
        return false;
    s = saida;
    id = idSaida;
    temSaida = false;
    return true;
}

void Classificador::Laco()
{
    for (;;)
    {
        {
            std::unique_lock<std::mutex> g(mu);
            cv.wait(g, [this] { return sair || temEntrada; });
            if (sair)
                return;
            temEntrada = false;
        }
        try
        {
            Classifica();
        }
        catch (const std::exception& e)
        {
            std::lock_guard<std::mutex> g(muErro);
            erro = e.what();
        }
        ocupado = false;
    }
}

void Classificador::Classifica()
{
    const auto t0 = std::chrono::steady_clock::now();
    const uint8_t* src = entrada.data();
    // brilho: um ganho global pela mediana da luma (quadros escuros, como o Gears, somem para os modelos)
    std::vector<uint8_t> am;
    am.reserve((kEntW / 8) * (kEntH / 8));
    for (UINT y = 4; y < kEntH; y += 8)
        for (UINT x = 4; x < kEntW; x += 8)
        {
            const uint8_t* p = src + (size_t(y) * kEntW + x) * 4;
            am.push_back(uint8_t((54 * p[0] + 183 * p[1] + 19 * p[2]) >> 8));
        }
    std::nth_element(am.begin(), am.begin() + am.size() / 2, am.end());
    mediana = am[am.size() / 2] / 255.f;
    const float ganho = std::clamp(0.35f * 255.f / std::max<float>(am[am.size() / 2], 1.f), 1.f, 6.f);
    float lut[256];
    for (int i = 0; i < 256; ++i)
        lut[i] = std::pow(std::min(1.f, i / 255.f * ganho), 0.8f);

    // cena: 640x360, CHW, 0..1 (media 2x2)
    const size_t nS = size_t(kSaiW) * kSaiH;
    std::vector<float> xc(3 * nS);
    for (UINT y = 0; y < kSaiH; ++y)
        for (UINT x = 0; x < kSaiW; ++x)
        {
            const uint8_t* a = src + (size_t(2 * y) * kEntW + 2 * x) * 4;
            const uint8_t* b = a + size_t(kEntW) * 4;
            for (int k = 0; k < 3; ++k)
                xc[k * nS + size_t(y) * kSaiW + x] = 0.25f * (lut[a[k]] + lut[a[4 + k]] + lut[b[k]] + lut[b[4 + k]]);
        }
    auto roda = [&](OrtSession* s, const std::string& nEnt, const std::string& nSai, float* dados, size_t n,
                    const int64_t* forma, float* destino, size_t nDestino) {
        OrtValue* in = nullptr;
        OrtValue* out = nullptr;
        if (Falha(ort->CreateTensorWithDataAsOrtValue(memCpu, dados, n * sizeof(float), forma, 4,
                                                      ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in),
                  "tensor"))
            throw std::runtime_error(erro);
        const char* ne[] = {nEnt.c_str()};
        const char* ns[] = {nSai.c_str()};
        OrtStatus* st = ort->Run(s, nullptr, ne, &in, 1, ns, 1, &out);
        ort->ReleaseValue(in);
        if (Falha(st, "Run"))
            throw std::runtime_error(erro);
        float* p = nullptr;
        ort->GetTensorMutableData(out, reinterpret_cast<void**>(&p));
        memcpy(destino, p, nDestino * sizeof(float));
        ort->ReleaseValue(out);
    };
    std::vector<float> pc(3 * nS);
    const int64_t formaCena[4] = {1, 3, kSaiH, kSaiW};
    roda(cena, nomeEntCena, nomeSaiCena, xc.data(), xc.size(), formaCena, pc.data(), pc.size());
    const float* P = pc.data();

    auto amostra = [&](float fx, float fy, int k) {
        fx = std::clamp(fx, 0.f, float(kEntW - 1));
        fy = std::clamp(fy, 0.f, float(kEntH - 1));
        const int x0 = int(fx), y0 = int(fy), x1 = std::min(x0 + 1, int(kEntW) - 1), y1 = std::min(y0 + 1, int(kEntH) - 1);
        const float ax = fx - x0, ay = fy - y0;
        auto v = [&](int x, int y) { return lut[src[(size_t(y) * kEntW + x) * 4 + k]]; };
        return (v(x0, y0) * (1 - ax) + v(x1, y0) * ax) * (1 - ay) + (v(x0, y1) * (1 - ax) + v(x1, y1) * ax) * ay;
    };
    // recortes de personagem, em pixels da entrada (1280x720): caixa e peso
    struct Caixa
    {
        float x0, y0, x1, y1, peso;
    };
    std::vector<Caixa> caixas;
    auto iou = [](const Caixa& a, const Caixa& b) {
        const float ix = std::max(0.f, std::min(a.x1, b.x1) - std::max(a.x0, b.x0));
        const float iy = std::max(0.f, std::min(a.y1, b.y1) - std::max(a.y0, b.y0));
        const float in = ix * iy, ua = (a.x1 - a.x0) * (a.y1 - a.y0) + (b.x1 - b.x0) * (b.y1 - b.y0) - in;
        return ua > 0 ? in / ua : 0.f;
    };

    // 1. detector de pessoas (YOLOX, Apache-2.0): acha o personagem que a segmentacao de cena perde (armadura, de costas)
    if (det)
    {
        const int S = ladoDet;
        const float r = float(S) / float(kEntW);
        const int nh = int(std::lround(kEntH * r));
        std::vector<float> xd(size_t(3) * S * S, 114.f);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < S; ++x)
                for (int k = 0; k < 3; ++k) // BGR 0..255, a imagem no alto e o resto 114 (como no treino)
                    xd[size_t(2 - k) * S * S + size_t(y) * S + x] = 255.f * amostra((x + 0.5f) / r - 0.5f, (y + 0.5f) / r - 0.5f, k);
        int n = 0;
        for (int st : {8, 16, 32})
            n += (S / st) * (S / st);
        std::vector<float> od(size_t(n) * 85);
        const int64_t formaDet[4] = {1, 3, S, S};
        roda(det, nomeEntDet, nomeSaiDet, xd.data(), xd.size(), formaDet, od.data(), od.size());
        std::vector<Caixa> cand;
        int i = 0;
        for (int st : {8, 16, 32})
            for (int gy = 0; gy < S / st; ++gy)
                for (int gx = 0; gx < S / st; ++gx, ++i)
                {
                    const float* d = &od[size_t(i) * 85];
                    const float nota = d[4] * d[5]; // objeto x classe pessoa
                    if (nota < kNotaDet)
                        continue;
                    const float cx = (d[0] + gx) * st, cy = (d[1] + gy) * st;
                    const float bw = std::exp(d[2]) * st, bh = std::exp(d[3]) * st;
                    cand.push_back({(cx - bw / 2) / r, (cy - bh / 2) / r, (cx + bw / 2) / r, (cy + bh / 2) / r, nota});
                }
        std::sort(cand.begin(), cand.end(), [](const Caixa& a, const Caixa& b) { return a.peso > b.peso; });
        for (const Caixa& c : cand)
        {
            bool fica = true;
            for (const Caixa& k : caixas)
                fica &= iou(c, k) < 0.45f;
            if (fica && caixas.size() < size_t(kMaxPessoas))
                caixas.push_back(c);
        }
        // nota -> peso: 0,15 vale pouco, 0,5 ou mais vale tudo (a mascara ainda acumula no tempo)
        for (Caixa& c : caixas)
            c.peso = std::clamp((c.peso - 0.1f) / 0.4f, 0.f, 1.f);
    }
    const size_t deteccoes = caixas.size();

    // 2. componentes conexos da pessoa da cena: os que o detector nao achou tambem ganham recorte
    {
        std::vector<uint8_t> visto(nS, 0);
        std::vector<int> pilha;
        std::vector<std::pair<int, Caixa>> comps;
        for (UINT i = 0; i < nS; ++i)
        {
            if (visto[i] || P[i] <= kLimiarPessoa)
                continue;
            int x0 = int(i % kSaiW), y0 = int(i / kSaiW), x1 = x0 + 1, y1 = y0 + 1, area = 0;
            pilha.assign(1, int(i));
            visto[i] = 1;
            while (!pilha.empty())
            {
                const int j = pilha.back();
                pilha.pop_back();
                const int x = j % kSaiW, y = j / kSaiW;
                ++area;
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
                const int viz[4] = {x > 0 ? j - 1 : -1, x + 1 < int(kSaiW) ? j + 1 : -1, y > 0 ? j - int(kSaiW) : -1,
                                    y + 1 < int(kSaiH) ? j + int(kSaiW) : -1};
                for (int v : viz)
                    if (v >= 0 && !visto[v] && P[v] > kLimiarPessoa)
                        visto[v] = 1, pilha.push_back(v);
            }
            if (area >= kAreaMin)
                comps.push_back({area, Caixa{2.f * x0, 2.f * y0, 2.f * x1, 2.f * y1, 1.f}});
        }
        std::sort(comps.begin(), comps.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& c : comps)
        {
            bool novo = true;
            for (size_t k = 0; k < deteccoes; ++k)
                novo &= iou(c.second, caixas[k]) < 0.3f;
            if (novo && caixas.size() < size_t(kMaxPessoas) + 1)
                caixas.push_back(c.second);
        }
    }

    // 3. partes no recorte de cada personagem: a silhueta (1 - fundo) e a pele, o cabelo e a roupa dele
    std::vector<float> pele(nS, 0.f), cabelo(nS, 0.f), pessoaRec(nS, 0.f);
    std::vector<float> xp(size_t(kLado) * kLado * 3), pp(size_t(kLado) * kLado * 6);
    const int64_t formaPartes[4] = {1, kLado, kLado, 3};
    for (const Caixa& c : caixas)
    {
        const float lado = std::max(48.f, 1.25f * std::max(c.x1 - c.x0, c.y1 - c.y0)); // quadrado com folga
        const float cx = 0.5f * (c.x0 + c.x1), cy = 0.5f * (c.y0 + c.y1);
        const float ox = cx - lado * 0.5f, oy = cy - lado * 0.5f, passo = lado / kLado;
        for (int v = 0; v < kLado; ++v)
            for (int u = 0; u < kLado; ++u)
                for (int k = 0; k < 3; ++k)
                    xp[(size_t(v) * kLado + u) * 3 + k] =
                        amostra(ox + (u + 0.5f) * passo - 0.5f, oy + (v + 0.5f) * passo - 0.5f, k) * 2.f - 1.f;
        roda(partes, nomeEntPartes, nomeSaiPartes, xp.data(), xp.size(), formaPartes, pp.data(), pp.size());
        for (size_t i = 0; i < size_t(kLado) * kLado; ++i)
        {
            float* z = &pp[i * 6];
            const float m = *std::max_element(z, z + 6);
            float s = 0;
            for (int k = 0; k < 6; ++k)
                s += (z[k] = std::exp(z[k] - m));
            for (int k = 0; k < 6; ++k)
                z[k] /= s;
        }
        // cola so dentro da caixa (8% de folga): fora dela o modelo de partes ve "pessoa" no que nao e
        const float fx = 0.08f * (c.x1 - c.x0), fy = 0.08f * (c.y1 - c.y0);
        const int X0 = std::max(0, int(std::floor((c.x0 - fx) * 0.5f))), X1 = std::min(int(kSaiW), int(std::ceil((c.x1 + fx) * 0.5f)));
        const int Y0 = std::max(0, int(std::floor((c.y0 - fy) * 0.5f))), Y1 = std::min(int(kSaiH), int(std::ceil((c.y1 + fy) * 0.5f)));
        for (int y = Y0; y < Y1; ++y)
            for (int x = X0; x < X1; ++x)
            {
                const float u = (2.f * (x + 0.5f) - ox) / passo - 0.5f, v = (2.f * (y + 0.5f) - oy) / passo - 0.5f;
                if (u < -0.5f || v < -0.5f || u > kLado - 0.5f || v > kLado - 0.5f)
                    continue;
                const float uc = std::clamp(u, 0.f, float(kLado - 1)), vc = std::clamp(v, 0.f, float(kLado - 1));
                const int u0 = int(uc), v0 = int(vc), u1 = std::min(u0 + 1, kLado - 1), v1 = std::min(v0 + 1, kLado - 1);
                const float au = uc - u0, av = vc - v0;
                auto q = [&](int k) {
                    auto z = [&](int uu, int vv) { return pp[(size_t(vv) * kLado + uu) * 6 + k]; };
                    return (z(u0, v0) * (1 - au) + z(u1, v0) * au) * (1 - av) + (z(u0, v1) * (1 - au) + z(u1, v1) * au) * av;
                };
                const size_t i = size_t(y) * kSaiW + x;
                pessoaRec[i] = std::max(pessoaRec[i], c.peso * (1.f - q(0)));
                pele[i] = std::max(pele[i], c.peso * (q(2) + q(3)));
                cabelo[i] = std::max(cabelo[i], c.peso * q(1));
            }
    }

    uint8_t* o = saida.data();
    std::vector<uint8_t> res(nS * 4);
    for (size_t i = 0; i < nS; ++i)
    {
        // personagem tem prioridade: onde a cena da alguma chance de pessoa, o "objeto" concorrente (armadura escura que
        // o modelo confunde com moto/cadeira) nao conta
        const float pes = std::max(P[i], pessoaRec[i]), animal = pc[nS + i];
        const float obj = pc[2 * nS + i] * (1.f - std::clamp((pes - 0.1f) / 0.2f, 0.f, 1.f));
        const float sk = std::min(pele[i], pes);
        const float hr = std::min(cabelo[i], pes - sk);
        float resto = std::max(pes - sk - hr, 0.f) + animal, ob = obj;
        const float soma = sk + hr + resto + ob, f = soma > 1.f ? 1.f / soma : 1.f;
        res[i * 4 + 0] = uint8_t(std::lround(255.f * sk * f));
        res[i * 4 + 1] = uint8_t(std::lround(255.f * hr * f));
        res[i * 4 + 2] = uint8_t(std::lround(255.f * resto * f));
        res[i * 4 + 3] = uint8_t(std::lround(255.f * ob * f));
    }
    {
        std::lock_guard<std::mutex> g(mu);
        memcpy(o, res.data(), res.size());
        idSaida = idEntrada;
        temSaida = true;
    }
    pessoas = int(caixas.size());
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace ov
