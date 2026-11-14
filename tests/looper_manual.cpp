// LooperCore — modo manual: base, sobregravação com latência, ×2, mudo, parar/tocar, limpar, toque duplo.
#include <string>

#include "looper_sim.h"
#include "teste.h"

using pestana::LooperCore;

namespace {
constexpr int L = 1000;              // latência de ida e volta simulada (amostras)
constexpr int64_t B = 256 * 200;     // base = 51200 amostras (múltiplo do bloco: posições exatas)

std::string lista(const std::vector<int64_t>& v, int64_t origem) {
  std::string s;
  for (auto x : v) s += std::to_string(x - origem) + " ";
  return s.empty() ? "(nenhum)" : s;
}

/// Grava a base na faixa 0 com um impulso na posição 1000 do ciclo. Devolve o índice global de G = 0.
int64_t gravarBase(LooperSim& sim) {
  CONFERE(sim.core.press(0));
  const int64_t g0 = sim.n;                 // o comando entra no começo do próximo bloco: G = 0 ali
  sim.impulso(g0 + 1000 + L, 0.5f);         // tocado ouvindo G = 1000, chega L depois
  sim.ate(g0 + B);
  CONFERE(sim.core.press(0));               // fecha o ciclo em G = B
  sim.ate(g0 + B + L + 512);                // cauda
  return g0;
}
} // namespace

TESTE(base_fecha_no_tamanho_certo) {
  LooperSim sim;
  const int64_t g0 = gravarBase(sim);
  CONFERE_IGUAL(sim.core.baseLength(), B);
  CONFERE_IGUAL(sim.core.trackLength(0), B);
  CONFERE_IGUAL(sim.core.trackState(0), LooperCore::Playing);
  CONFERE(sim.core.playing());
  sim.ate(g0 + 3 * B);
  const auto p = sim.picos(g0 + 2 * B, g0 + 3 * B);
  CONFERE_MSG(mesmosPicos(p, {g0 + 2 * B + 1000}), "picos no ciclo 2: %s", lista(p, g0 + 2 * B).c_str());
  CONFERE_PERTO(sim.s(g0 + 2 * B + 1000), 0.5, 1e-6);
}

TESTE(sobregravacao_cai_na_posicao_certa_com_latencia) {
  LooperSim sim;
  const int64_t g0 = gravarBase(sim);
  sim.ate(g0 + 2 * B);
  CONFERE(sim.core.press(0));                       // sobregrava a partir do ciclo 2
  CONFERE_IGUAL(sim.core.trackState(0), LooperCore::Overdubbing);
  sim.impulso(g0 + 2 * B + 20000 + L, 0.3f);        // tocado na posição 20000
  sim.impulso(g0 + 3 * B - 200 + L, 0.2f);          // tocado no fim do ciclo: chega depois do botão (cauda)
  sim.ate(g0 + 3 * B);
  CONFERE(sim.core.press(0));                       // volta a tocar
  sim.ate(g0 + 5 * B);
  CONFERE_IGUAL(sim.core.trackState(0), LooperCore::Playing);
  const auto p = sim.picos(g0 + 4 * B, g0 + 5 * B);
  const int64_t c = g0 + 4 * B;
  CONFERE_MSG(mesmosPicos(p, {c + 1000, c + 20000, c + B - 200}), "picos: %s", lista(p, c).c_str());
  CONFERE_PERTO(sim.s(c + 20000), 0.3, 1e-6);
  CONFERE_PERTO(sim.s(c + B - 200), 0.2, 1e-6);
  CONFERE(sim.core.trackCanUndo(0));
}

TESTE(faixa_x2_ancorada_no_ciclo) {
  LooperSim sim;
  const int64_t g0 = gravarBase(sim);
  sim.core.setTrackMultiplier(1, 2);
  CONFERE_IGUAL(sim.core.trackMultiplier(1), 2);
  sim.ate(g0 + 4 * B + 512);                        // aperta no meio do começo do ciclo 4
  CONFERE(sim.core.press(1));
  sim.impulso(g0 + 5 * B + 500 + L, 0.25f);         // 2º ciclo da faixa ×2, posição 500
  sim.ate(g0 + 6 * B);
  CONFERE(sim.core.press(1));
  sim.ate(g0 + 6 * B + L + 512);
  CONFERE_IGUAL(sim.core.trackLength(1), 2 * B);
  CONFERE_IGUAL(sim.core.trackState(1), LooperCore::Playing);
  sim.ate(g0 + 10 * B);
  // A faixa ×2 está ancorada em G = 4B: o impulso dela aparece só nos ciclos ímpares (5, 7, 9).
  const auto p = sim.picos(g0 + 8 * B, g0 + 10 * B);
  const int64_t c = g0 + 8 * B;
  CONFERE_MSG(mesmosPicos(p, {c + 1000, c + B + 500, c + B + 1000}), "picos: %s", lista(p, c).c_str());
}

TESTE(mudo_parar_tocar_e_limpar) {
  LooperSim sim;
  const int64_t g0 = gravarBase(sim);
  sim.core.setTrackMultiplier(1, 2);
  sim.ate(g0 + 4 * B);
  CONFERE(sim.core.press(1));
  sim.impulso(g0 + 5 * B + 500 + L, 0.25f);
  sim.ate(g0 + 6 * B);
  CONFERE(sim.core.press(1));
  sim.ate(g0 + 8 * B);

  // Mudo na faixa 1: só a base toca.
  sim.core.toggleMute(1);
  CONFERE(sim.core.trackMuted(1));
  sim.ate(g0 + 10 * B);
  auto p = sim.picos(g0 + 9 * B, g0 + 10 * B);
  CONFERE_MSG(mesmosPicos(p, {g0 + 9 * B + 1000}), "mudo: %s", lista(p, g0 + 9 * B).c_str());
  sim.core.toggleMute(1);
  CONFERE(!sim.core.trackMuted(1));
  sim.ate(g0 + 12 * B);
  p = sim.picos(g0 + 11 * B, g0 + 12 * B);
  CONFERE_MSG(mesmosPicos(p, {g0 + 11 * B + 500, g0 + 11 * B + 1000}), "sem mudo: %s", lista(p, g0 + 11 * B).c_str());

  // Parar tudo: silêncio.
  sim.core.stopAll();
  sim.blocos(2);
  CONFERE(!sim.core.playing());
  const int64_t parado = sim.n;
  sim.ate(parado + 2 * B);
  CONFERE(sim.picos(parado, parado + 2 * B).empty());
  CONFERE_IGUAL(sim.core.trackState(0), LooperCore::Playing);   // as faixas continuam com conteúdo

  // Tocar tudo: recomeça do início do ciclo (G = 0).
  sim.core.playAll();
  const int64_t g1 = sim.n;
  sim.ate(g1 + 2 * B);
  CONFERE(sim.core.playing());
  p = sim.picos(g1, g1 + 2 * B);
  CONFERE_MSG(mesmosPicos(p, {g1 + 1000, g1 + B + 500, g1 + B + 1000}), "tocar: %s", lista(p, g1).c_str());

  // Limpar tudo.
  sim.core.clearAll();
  sim.blocos(2);
  CONFERE(!sim.core.anyContent());
  CONFERE_IGUAL(sim.core.baseLength(), 0);
  CONFERE(!sim.core.playing());
  CONFERE_IGUAL(sim.core.trackState(0), LooperCore::Empty);
  CONFERE_IGUAL(sim.core.trackState(1), LooperCore::Empty);
  CONFERE_IGUAL(sim.core.bytesInUse(), 0);
  const int64_t limpo = sim.n;
  sim.ate(limpo + B);
  CONFERE(sim.picos(limpo, limpo + B).empty());
}

TESTE(toque_duplo_acidental_descartado) {
  LooperSim sim;
  CONFERE(sim.core.press(0));
  sim.blocos(10);                    // 2560 amostras < 0,1 s (4800)
  CONFERE(sim.core.press(0));
  sim.blocos(2);
  CONFERE_IGUAL(sim.core.trackState(0), LooperCore::Empty);
  CONFERE_IGUAL(sim.core.baseLength(), 0);
  CONFERE(!sim.core.anyContent());
  CONFERE(!sim.core.playing());
  CONFERE_IGUAL(sim.core.bytesInUse(), 0);
  // Depois do descarte dá para gravar a base de novo normalmente.
  const int64_t g0 = gravarBase(sim);
  CONFERE_IGUAL(sim.core.baseLength(), B);
  sim.ate(g0 + 2 * B);
  CONFERE(mesmosPicos(sim.picos(g0 + B, g0 + 2 * B), {g0 + B + 1000}));
}

int main() { return rodarTestes(); }
