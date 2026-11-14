// NoiseReducer — transparência, redução de ruído, aprendizado, troca de perfil, offline, estimativa.
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

#include "NoiseReducer.h"
#include "sinais.h"
#include "teste.h"

using pestana::NoiseReducer;

namespace {
constexpr double kTaxa = 48000.0;
constexpr int kBloco = 256;

/// Processa `x` (mono duplicado em 2 canais) em blocos; devolve o canal esquerdo. `aCada` roda
/// depois de cada bloco (com o índice da próxima amostra).
template <typename F>
std::vector<float> processar(NoiseReducer& nr, const std::vector<float>& x, F aCada) {
  std::vector<float> l = x, r = x;
  for (size_t off = 0; off < x.size(); off += kBloco) {
    const int n = static_cast<int>(std::min<size_t>(kBloco, x.size() - off));
    float* ch[2] = {l.data() + off, r.data() + off};
    nr.process(ch, 2, n);
    aCada(static_cast<int64_t>(off) + n);
  }
  return l;
}
std::vector<float> processar(NoiseReducer& nr, const std::vector<float>& x) {
  return processar(nr, x, [](int64_t) {});
}
} // namespace

TESTE(sem_perfil_e_transparente_com_atraso_de_latency) {
  NoiseReducer nr(512);
  nr.prepare(kTaxa);
  CONFERE_IGUAL(nr.latency(), 512);
  std::vector<float> x = sinais::arCondicionado(48000, kTaxa);
  const auto n = sinais::nota(48000, kTaxa);
  for (size_t i = 0; i < x.size(); i++) x[i] += n[i];
  const auto y = processar(nr, x);
  double erro = 0;
  for (size_t i = 0; i < 512; i++) erro = std::max(erro, static_cast<double>(std::fabs(y[i])));
  for (size_t i = 512; i < x.size(); i++) erro = std::max(erro, static_cast<double>(std::fabs(y[i] - x[i - 512])));
  teste::info("erro máximo (saída − entrada atrasada) = %.3g", erro);
  CONFERE(erro < 1e-5);
  // Intensidade 0 com perfil também é transparente.
  NoiseReducer nr2(512);
  nr2.prepare(kTaxa);
  std::vector<float> prof(static_cast<size_t>(nr2.bins()), 1.0f);
  nr2.setProfile(prof.data(), nr2.bins());
  nr2.setAmount(0.0f);
  const auto y2 = processar(nr2, x);
  erro = 0;
  for (size_t i = 512; i < x.size(); i++) erro = std::max(erro, static_cast<double>(std::fabs(y2[i] - x[i - 512])));
  CONFERE(erro < 1e-5);
}

TESTE(perfil_aprendido_reduz_10dB_e_preserva_o_ataque) {
  NoiseReducer nr(512);
  nr.prepare(kTaxa);
  nr.setAmount(0.5f);
  const int64_t total = static_cast<int64_t>(6 * kTaxa);
  const int64_t inicioNota = static_cast<int64_t>(4 * kTaxa);
  auto ruido = sinais::arCondicionado(total, kTaxa, 11);
  auto limpa = std::vector<float>(static_cast<size_t>(total), 0.0f);
  const auto nt = sinais::nota(total - inicioNota, kTaxa);
  for (int64_t i = inicioNota; i < total; i++) limpa[static_cast<size_t>(i)] = nt[static_cast<size_t>(i - inicioNota)];
  std::vector<float> x(static_cast<size_t>(total));
  for (int64_t i = 0; i < total; i++) x[static_cast<size_t>(i)] = ruido[static_cast<size_t>(i)] + limpa[static_cast<size_t>(i)];

  nr.startLearning(1.0);
  CONFERE(nr.isLearning());
  int64_t fimAprendizado = -1;
  bool perfilJunto = true;
  std::vector<float> prof(static_cast<size_t>(nr.bins()));
  const auto y = processar(nr, x, [&](int64_t pos) {
    if (fimAprendizado < 0 && !nr.isLearning()) {
      fimAprendizado = pos;
      perfilJunto = nr.copyProfile(prof.data(), nr.bins()) == nr.bins() && nr.hasProfile();
    }
  });
  CONFERE(fimAprendizado > 0);
  CONFERE(perfilJunto);
  teste::info("aprendizado terminou em %.3f s", static_cast<double>(fimAprendizado) / kTaxa);
  CONFERE(fimAprendizado <= static_cast<int64_t>(1.1 * kTaxa));

  const int lat = nr.latency();
  // Silêncio (só ruído) entre 2 e 4 s.
  const double rIn = sinais::rms(x, static_cast<int64_t>(2 * kTaxa), inicioNota - 4096);
  const double rOut = sinais::rms(y, static_cast<int64_t>(2 * kTaxa) + lat, inicioNota - 4096 + lat);
  const double reducao = sinais::db(rIn) - sinais::db(rOut);
  teste::info("ruído: entrada %.1f dBFS, saída %.1f dBFS, redução %.1f dB", sinais::db(rIn), sinais::db(rOut), reducao);
  CONFERE(reducao >= 10.0);

  // Ataque da nota (primeiros 50 ms) comparado com a nota limpa.
  const int64_t a = inicioNota, b = inicioNota + static_cast<int64_t>(0.05 * kTaxa);
  const double rNota = sinais::rms(limpa, a, b);
  const double rAtaque = sinais::rms(y, a + lat, b + lat);
  const double dif = sinais::db(rAtaque) - sinais::db(rNota);
  teste::info("ataque: nota limpa %.2f dBFS, saída %.2f dBFS, diferença %.2f dB", sinais::db(rNota), sinais::db(rAtaque), dif);
  CONFERE(std::fabs(dif) <= 1.0);
  // Corpo da nota (0,5 s depois) também não pode sumir.
  const int64_t c = inicioNota + static_cast<int64_t>(0.5 * kTaxa), d = c + static_cast<int64_t>(0.2 * kTaxa);
  const double difCorpo = sinais::db(sinais::rms(y, c + lat, d + lat)) - sinais::db(sinais::rms(limpa, c, d));
  teste::info("corpo da nota: diferença %.2f dB", difCorpo);
  CONFERE(std::fabs(difCorpo) <= 1.5);
}

TESTE(is_learning_false_implica_copy_profile_com_perfil) {
  // Vários tamanhos de aprendizado e de bloco: a cada bloco, se isLearning() é false, copyProfile
  // tem de devolver o perfil aprendido (não zero, não o antigo).
  for (double seg : {0.05, 0.3, 1.0}) {
    NoiseReducer nr(512);
    nr.prepare(kTaxa);
    std::vector<float> antigo(static_cast<size_t>(nr.bins()), 123.0f);
    nr.setProfile(antigo.data(), nr.bins());
    const auto x = sinais::arCondicionado(static_cast<int64_t>((seg + 0.5) * kTaxa), kTaxa);
    nr.startLearning(seg);
    bool ok = true, viu = false;
    std::vector<float> p(static_cast<size_t>(nr.bins()));
    processar(nr, x, [&](int64_t) {
      if (viu || nr.isLearning()) return;
      viu = true;
      const int k = nr.copyProfile(p.data(), nr.bins());
      double soma = 0;
      for (int i = 0; i < k; i++) soma += p[static_cast<size_t>(i)];
      ok = k == nr.bins() && soma > 0 && p[10] != 123.0f;
    });
    CONFERE_MSG(viu && ok, "aprendizado de %.2f s: terminou=%d perfil certo=%d", seg, viu ? 1 : 0, ok ? 1 : 0);
  }
}

TESTE(set_profile_concorrente_nao_quebra) {
  NoiseReducer nr(512);
  nr.prepare(kTaxa);
  nr.setAmount(0.7f);
  const auto x = sinais::arCondicionado(static_cast<int64_t>(4 * kTaxa), kTaxa);
  std::atomic<bool> fim{false};
  std::atomic<int> voltas{0};
  std::thread principal([&] {
    std::vector<float> a(static_cast<size_t>(nr.bins()), 1e-3f), b(static_cast<size_t>(nr.bins()), 10.0f);
    std::vector<float> c(static_cast<size_t>(nr.bins()));
    int k = 0;
    while (!fim.load()) {
      switch (k++ % 5) {
        case 0: nr.setProfile(a.data(), nr.bins()); break;
        case 1: nr.setProfile(b.data(), nr.bins() / 2); break;
        case 2: nr.clearProfile(); break;
        case 3: nr.copyProfile(c.data(), nr.bins()); break;
        case 4: nr.startLearning(0.05); nr.setAmount(static_cast<float>(k % 10) / 10.0f); break;
      }
      voltas++;
    }
  });
  std::vector<float> y;
  for (int rep = 0; rep < 3; rep++) y = processar(nr, x);
  fim.store(true);
  principal.join();
  bool finito = true;
  for (float v : y) if (!std::isfinite(v) || std::fabs(v) > 4.0f) finito = false;
  teste::info("%g trocas de perfil durante o processamento", voltas.load());
  CONFERE(finito);
  CONFERE(voltas.load() > 100);
}

TESTE(process_offline_sem_atraso) {
  NoiseReducer nr(2048);
  nr.prepare(kTaxa);
  const int64_t n = static_cast<int64_t>(2 * kTaxa);
  auto x = sinais::arCondicionado(n, kTaxa, 3);
  const auto nt = sinais::nota(n / 2, kTaxa, 330.0);
  for (int64_t i = 0; i < n / 2; i++) x[static_cast<size_t>(n / 2 + i)] += nt[static_cast<size_t>(i)];
  std::vector<std::vector<float>> ch = {x, x};
  const auto prof = nr.estimateProfile(ch);
  nr.setProfile(prof.data(), static_cast<int>(prof.size()));
  nr.setAmount(0.5f);
  nr.processOffline(ch);
  CONFERE_IGUAL(ch[0].size(), x.size());
  int melhor = 9999;
  double max = -1;
  for (int lag = -3000; lag <= 3000; lag += 1) {
    double s = 0;
    for (int64_t i = 4000; i < n - 4000; i += 2) s += static_cast<double>(ch[0][static_cast<size_t>(i + lag)]) * x[static_cast<size_t>(i)];
    if (s > max) { max = s; melhor = lag; }
  }
  teste::info("correlação máxima no atraso %g", melhor);
  CONFERE_IGUAL(melhor, 0);
}

TESTE(estimate_profile_ignora_silencio_digital_e_fade) {
  NoiseReducer nr(2048);
  nr.prepare(kTaxa);
  const int64_t nr_ = static_cast<int64_t>(2 * kTaxa);
  const auto ruido = sinais::arCondicionado(nr_, kTaxa, 5);
  const auto refProf = nr.estimateProfile({ruido, ruido});

  // 1 s de zeros + 0,5 s de fade quase mudo (−90 dBFS) + o mesmo ruído + nota alta.
  std::vector<float> x(static_cast<size_t>(kTaxa), 0.0f);
  auto fade = sinais::arCondicionado(static_cast<int64_t>(0.5 * kTaxa), kTaxa, 9);
  for (auto& v : fade) v *= 1e-3f;
  x.insert(x.end(), fade.begin(), fade.end());
  const size_t comecoRuido = x.size();
  x.insert(x.end(), ruido.begin(), ruido.end());
  const auto nt = sinais::nota(static_cast<int64_t>(kTaxa), kTaxa);
  for (size_t i = 0; i < nt.size(); i++) x[comecoRuido + ruido.size() / 2 + i] += nt[i];
  const auto prof = nr.estimateProfile({x, x});

  double a = 0, b = 0;
  for (size_t k = 0; k < prof.size(); k++) { a += refProf[k]; b += prof[k]; }
  const double dif = 10.0 * std::log10(std::max(b, 1e-30) / std::max(a, 1e-30));
  teste::info("potência do perfil: referência %.3g, com silêncio/fade %.3g (diferença %.2f dB)", a, b, dif);
  CONFERE(std::fabs(dif) <= 3.0);
  // Só silêncio digital: perfil zerado (não inventa ruído).
  const auto vazio = nr.estimateProfile({std::vector<float>(48000, 0.0f)});
  double z = 0;
  for (float v : vazio) z += v;
  CONFERE(z == 0.0);
}

int main() { return rodarTestes(); }
