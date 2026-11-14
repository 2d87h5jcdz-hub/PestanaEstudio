// Sinais simulados para os testes de DSP: ar-condicionado e nota de violão/guitarra.
#pragma once
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace sinais {

constexpr double kPi = 3.14159265358979323846;

/// Ar-condicionado: chiado grave (ruído branco em passa-baixas de ~800 Hz) + ronco (ruído em
/// passa-baixas de ~90 Hz) + zumbido de 120 Hz. Nível total ≈ −40 dBFS.
inline std::vector<float> arCondicionado(int64_t n, double taxa, uint32_t semente = 7) {
  std::mt19937 rng(semente);
  std::normal_distribution<float> g(0.0f, 1.0f);
  std::vector<float> x(static_cast<size_t>(n));
  const float a1 = static_cast<float>(std::exp(-2 * kPi * 800.0 / taxa));
  const float a2 = static_cast<float>(std::exp(-2 * kPi * 90.0 / taxa));
  float lp1 = 0, lp2 = 0, lp2b = 0;
  for (int64_t i = 0; i < n; i++) {
    lp1 = a1 * lp1 + (1 - a1) * g(rng);
    lp2 = a2 * lp2 + (1 - a2) * g(rng);
    lp2b = a2 * lp2b + (1 - a2) * lp2;
    const double t = static_cast<double>(i) / taxa;
    x[static_cast<size_t>(i)] = 0.05f * lp1 + 0.25f * lp2b + 0.004f * static_cast<float>(std::sin(2 * kPi * 120.0 * t));
  }
  return x;
}

/// Nota com harmônicas (amplitude 1/k) decaindo com τ; ataque de 2 ms.
inline std::vector<float> nota(int64_t n, double taxa, double f0 = 196.0, double tau = 1.2, float amp = 0.3f,
                               int harmonicas = 8) {
  std::vector<float> x(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; i++) {
    const double t = static_cast<double>(i) / taxa;
    double s = 0;
    for (int k = 1; k <= harmonicas; k++) s += std::sin(2 * kPi * f0 * k * t + 0.3 * k) / k;
    const double ataque = std::min(1.0, t / 0.002);
    x[static_cast<size_t>(i)] = static_cast<float>(amp * ataque * std::exp(-t / tau) * s / 2.0);
  }
  return x;
}

inline double rms(const std::vector<float>& x, int64_t de, int64_t ate) {
  double e = 0;
  int64_t c = 0;
  for (int64_t i = std::max<int64_t>(0, de); i < ate && i < static_cast<int64_t>(x.size()); i++) {
    e += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
    c++;
  }
  return c > 0 ? std::sqrt(e / static_cast<double>(c)) : 0.0;
}

inline double db(double r) { return 20.0 * std::log10(std::max(r, 1e-20)); }

} // namespace sinais
