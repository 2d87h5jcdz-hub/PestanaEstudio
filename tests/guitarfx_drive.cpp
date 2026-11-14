// GuitarFXCore — o drive sustenta a nota (não encurta) e a saída é sempre finita.
#include <cmath>
#include <random>
#include <vector>

#include "GuitarFXCore.h"
#include "sinais.h"
#include "teste.h"

using pestana::GuitarFXCore;

namespace {
constexpr double kTaxa = 48000.0;
constexpr int kBloco = 256;

std::vector<float> processar(GuitarFXCore& fx, std::vector<float> x) {
  for (size_t off = 0; off < x.size(); off += kBloco) {
    const int n = static_cast<int>(std::min<size_t>(kBloco, x.size() - off));
    float* ch[1] = {x.data() + off};
    fx.process(ch, 1, n);
  }
  return x;
}

/// Tempo (s) até o envelope (RMS em janelas de 20 ms) cair 10 dB abaixo do pico.
double tempoAteCair10dB(const std::vector<float>& y) {
  const int64_t w = static_cast<int64_t>(0.02 * kTaxa);
  std::vector<double> env;
  for (int64_t a = 0; a + w <= static_cast<int64_t>(y.size()); a += w) env.push_back(sinais::rms(y, a, a + w));
  size_t ip = 0;
  for (size_t i = 0; i < env.size(); i++) if (env[i] > env[ip]) ip = i;
  const double alvo = env[ip] * std::pow(10.0, -10.0 / 20.0);
  for (size_t i = ip; i < env.size(); i++) if (env[i] < alvo) return static_cast<double>(i) * 0.02;
  return static_cast<double>(env.size()) * 0.02;   // não caiu dentro do trecho
}
} // namespace

TESTE(distorcao_085_sustenta_ao_menos_2x_a_nota_limpa) {
  const auto nota = sinais::nota(static_cast<int64_t>(12 * kTaxa), kTaxa, 196.0, 1.2, 0.5f, 6);
  GuitarFXCore limpo;
  limpo.prepare(kTaxa, kBloco, 1);
  const double tLimpo = tempoAteCair10dB(processar(limpo, nota));

  GuitarFXCore fx;
  fx.prepare(kTaxa, kBloco, 1);
  fx.setDrive(pestana::DriveDistortion, 0.85f);
  CONFERE(fx.driveActive());
  const double tDrive = tempoAteCair10dB(processar(fx, nota));
  teste::info("até cair 10 dB: limpa %.2f s, distorção 0,85 %.2f s (razão %.2f)", tLimpo, tDrive, tDrive / tLimpo);
  CONFERE(tLimpo > 1.0 && tLimpo < 1.8);   // ≈ 1,2·ln(√10·…) para τ = 1,2 s
  CONFERE(tDrive >= 2.0 * tLimpo);
}

TESTE(saida_finita_nos_4_estilos) {
  std::mt19937 rng(42);
  std::normal_distribution<float> g(0.0f, 3.0f);
  const auto nota = sinais::nota(static_cast<int64_t>(2 * kTaxa), kTaxa, 110.0, 1.2, 0.9f, 8);
  for (int estilo = 0; estilo < 4; estilo++) {
    for (float qtd : {0.01f, 0.5f, 1.0f}) {
      GuitarFXCore fx;
      fx.prepare(kTaxa, kBloco, 2);
      fx.setDrive(estilo, qtd);
      std::vector<float> l = nota, r = nota;
      // Trecho com ruído forte (bem acima de 0 dBFS) e trecho de silêncio no fim.
      for (int i = 0; i < 48000; i++) { l.push_back(g(rng)); r.push_back(g(rng)); }
      l.resize(l.size() + 48000, 0.0f);
      r.resize(r.size() + 48000, 0.0f);
      bool finito = true;
      double pico = 0;
      for (size_t off = 0; off < l.size(); off += kBloco) {
        const int n = static_cast<int>(std::min<size_t>(kBloco, l.size() - off));
        float* ch[2] = {l.data() + off, r.data() + off};
        fx.process(ch, 2, n);
        for (int i = 0; i < n; i++) {
          for (int c = 0; c < 2; c++) {
            const float v = ch[c][i];
            if (!std::isfinite(v)) finito = false;
            pico = std::max(pico, static_cast<double>(std::fabs(v)));
          }
        }
      }
      CONFERE_MSG(finito, "estilo %d, intensidade %.2f: saída com NaN/Inf", estilo, qtd);
      CONFERE_MSG(pico < 4.0, "estilo %d, intensidade %.2f: pico %.2f", estilo, qtd, pico);
      CONFERE_MSG(pico > 1e-3, "estilo %d, intensidade %.2f: saída muda", estilo, qtd);
    }
  }
}

int main() { return rodarTestes(); }
