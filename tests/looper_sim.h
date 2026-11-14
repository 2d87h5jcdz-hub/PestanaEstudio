// Simulação da thread de áudio para os testes do LooperCore.
// O áudio é simulado em blocos (process) com collectGarbage entre eles, como a principal faz.
// Convenção: `n` é o índice global da amostra (desde o início da simulação). A entrada é montada de
// impulsos marcados em índices globais; a saída (canal esquerdo) fica toda guardada em `saida`.
#pragma once
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "LooperCore.h"

struct LooperSim {
  pestana::LooperCore core;
  int bloco = 256;
  int64_t n = 0;                          // próxima amostra global a processar
  std::map<int64_t, float> impulsos;      // índice global → amplitude (entrada nos dois canais)
  std::function<float(int64_t)> fonte;    // entrada contínua opcional (somada aos impulsos)
  std::vector<float> saida;               // saída esquerda de tudo que já rodou
  bool coletar = true;                    // chama collectGarbage entre blocos

  LooperSim(double taxa = 48000.0, double maxBaseSeg = 4.0, int latencia = 1000) {
    core.prepare(taxa, maxBaseSeg);
    core.setLatencySamples(latencia);
    core.setMonitor(false);   // a saída é só o loop (e o clique)
  }

  void impulso(int64_t global, float amp) { impulsos[global] += amp; }

  void blocos(int64_t k) {
    std::vector<float> inL(static_cast<size_t>(bloco)), inR(static_cast<size_t>(bloco));
    std::vector<float> outL(static_cast<size_t>(bloco)), outR(static_cast<size_t>(bloco));
    for (int64_t b = 0; b < k; b++) {
      for (int i = 0; i < bloco; i++) {
        float x = fonte ? fonte(n + i) : 0.0f;
        auto it = impulsos.find(n + i);
        if (it != impulsos.end()) x += it->second;
        inL[static_cast<size_t>(i)] = x;
        inR[static_cast<size_t>(i)] = x;
      }
      const float* in[2] = {inL.data(), inR.data()};
      float* out[2] = {outL.data(), outR.data()};
      core.process(in, out, bloco);
      saida.insert(saida.end(), outL.begin(), outL.end());
      n += bloco;
      if (coletar) core.collectGarbage();
    }
  }
  /// Roda até a amostra global `alvo` (arredondado para cima no bloco).
  void ate(int64_t alvo) {
    if (alvo <= n) return;
    blocos((alvo - n + bloco - 1) / bloco);
  }

  /// Índices globais em [de, ate) com |saída| > limiar.
  std::vector<int64_t> picos(int64_t de, int64_t ate_, float limiar = 1e-4f) const {
    std::vector<int64_t> r;
    for (int64_t i = de; i < ate_ && i < static_cast<int64_t>(saida.size()); i++) {
      if (std::fabs(saida[static_cast<size_t>(i)]) > limiar) r.push_back(i);
    }
    return r;
  }
  float s(int64_t global) const {
    return global >= 0 && global < static_cast<int64_t>(saida.size()) ? saida[static_cast<size_t>(global)] : 0.0f;
  }
  /// Copia a saída global [de, de + len).
  std::vector<float> trecho(int64_t de, int64_t len) const {
    std::vector<float> r(static_cast<size_t>(len), 0.0f);
    for (int64_t i = 0; i < len; i++) r[static_cast<size_t>(i)] = s(de + i);
    return r;
  }
};

/// Picos esperados (relativos a `origem`) e encontrados, para imprimir quando falhar.
inline bool mesmosPicos(const std::vector<int64_t>& achados, const std::vector<int64_t>& esperados) {
  return achados == esperados;
}
