// LooperCore — modo automático: contagem, base que fecha sozinha, sequência, "ao tocar",
// gravar por cima armado, cancelar.
#include <algorithm>
#include <string>

#include "looper_sim.h"
#include "teste.h"

using pestana::LooperCore;

namespace {
constexpr int L = 1000;
constexpr int64_t BEAT = 24000;          // 120 BPM a 48 kHz
constexpr int64_t C = 4 * BEAT;          // ciclo de 1 compasso 4/4 = 96000 (múltiplo de 256? 375 blocos)

std::string lista(const std::vector<int64_t>& v, int64_t origem) {
  std::string s;
  for (auto x : v) s += std::to_string(x - origem) + " ";
  return s.empty() ? "(nenhum)" : s;
}

void configurar(LooperSim& sim, int modo, bool sequencia, int faixas) {
  sim.core.setTempo(120.0, 4);
  sim.core.setAuto(true, 1, modo, sequencia, 0, faixas);
  sim.core.setClick(false, 0.5f);
}

/// Inícios de clique: primeira amostra (com tolerância de 2) acima do limiar depois de silêncio.
std::vector<int64_t> iniciosDeClique(const LooperSim& sim, int64_t de, int64_t ate) {
  std::vector<int64_t> r;
  int64_t ultimo = -1000000;
  for (int64_t i = de; i < ate; i++) {
    if (std::fabs(sim.s(i)) > 1e-3f) {
      if (i - ultimo > 2000) r.push_back(i);
      ultimo = i;
    }
  }
  return r;
}

/// Base automática com contagem; devolve o índice global de G = 0 (fim da contagem).
int64_t baseComContagem(LooperSim& sim) {
  CONFERE(sim.core.press(0));
  const int64_t inicio = sim.n;            // G = −C aqui
  const int64_t g0 = inicio + C;
  sim.impulso(g0 + 1000 + L, 0.5f);
  sim.ate(g0 + C + L + 512);
  return g0;
}
} // namespace

TESTE(contagem_de_1_compasso_com_4_cliques_no_tempo) {
  LooperSim sim(48000.0, 10.0, L);
  configurar(sim, LooperCore::StartCountIn, false, 6);
  CONFERE_IGUAL(sim.core.autoCycleLength(), C);
  CONFERE(sim.core.press(0));
  const int64_t inicio = sim.n;
  sim.blocos(1);
  CONFERE_IGUAL(sim.core.phase(), LooperCore::PhaseCountIn);
  CONFERE(!sim.core.playing());   // durante a contagem não conta como tocando
  sim.ate(inicio + C + C / 2);
  CONFERE_IGUAL(sim.core.phase(), LooperCore::PhaseNone);
  const auto cliques = iniciosDeClique(sim, inicio, inicio + C + C / 2);
  CONFERE_IGUAL(cliques.size(), 4);
  for (size_t k = 0; k < cliques.size() && k < 4; k++) {
    const int64_t esperado = inicio + static_cast<int64_t>(k) * BEAT;
    CONFERE_MSG(cliques[k] >= esperado && cliques[k] <= esperado + 2, "clique %zu em %lld, esperado %lld",
                k, static_cast<long long>(cliques[k] - inicio), static_cast<long long>(esperado - inicio));
  }
  // O 1º clique do compasso é mais agudo (1600 Hz) que os outros (1000 Hz): conta cruzamentos por zero.
  auto cruzamentos = [&](int64_t a) {
    int z = 0;
    for (int64_t i = a + 2; i < a + 1000; i++) if ((sim.s(i) > 0) != (sim.s(i - 1) > 0)) z++;
    return z;
  };
  if (cliques.size() == 4) CONFERE(cruzamentos(cliques[0]) > cruzamentos(cliques[1]) + 10);
}

TESTE(base_automatica_fecha_sozinha_exatamente_no_ciclo) {
  LooperSim sim(48000.0, 10.0, L);
  configurar(sim, LooperCore::StartCountIn, false, 6);
  CONFERE(sim.core.press(0));
  const int64_t g0 = sim.n + C;
  sim.impulso(g0 + 1000 + L, 0.5f);         // dentro do ciclo
  sim.impulso(g0 + C - 100 + L, 0.4f);      // fim do ciclo: chega na cauda
  sim.impulso(g0 + C + 100 + L, 0.9f);      // depois do ciclo: NÃO pode entrar
  sim.ate(g0 + C - 256);
  CONFERE_IGUAL(sim.core.baseLength(), 0);
  CONFERE_IGUAL(sim.core.trackState(0), LooperCore::Recording);
  sim.ate(g0 + C + 512);
  CONFERE_IGUAL(sim.core.baseLength(), C);
  sim.ate(g0 + C + L + 512);
  CONFERE_IGUAL(sim.core.trackState(0), LooperCore::Playing);
  sim.ate(g0 + 3 * C);
  const int64_t c = g0 + 2 * C;
  const auto p = sim.picos(c, c + C);
  CONFERE_MSG(mesmosPicos(p, {c + 1000, c + C - 100}), "picos: %s", lista(p, c).c_str());
}

TESTE(sequencia_de_3_faixas_sem_folga) {
  LooperSim sim(48000.0, 10.0, L);
  configurar(sim, LooperCore::StartCountIn, true, 3);
  CONFERE(sim.core.press(0));
  const int64_t g0 = sim.n + C;
  sim.blocos(1);
  CONFERE_IGUAL(sim.core.trackState(1), LooperCore::Armed);
  CONFERE_IGUAL(sim.core.trackState(2), LooperCore::Armed);
  CONFERE_IGUAL(sim.core.trackState(3), LooperCore::Empty);   // fora da sequência (chainCount = 3)
  sim.impulso(g0 + 1000 + L, 0.5f);              // faixa 0 (ciclo 0)
  sim.impulso(g0 + C + 2000 + L, 0.4f);          // faixa 1 (ciclo 1)
  sim.impulso(g0 + C + 50000 + L, 0.1f);         // faixa 1 (ciclo 1)
  sim.impulso(g0 + 2 * C + 3000 + L, 0.3f);      // faixa 2 (ciclo 2)
  sim.ate(g0 + C + 512);
  CONFERE_IGUAL(sim.core.trackState(1), LooperCore::Recording);
  sim.ate(g0 + 3 * C + L + 512);
  for (int t = 0; t < 3; t++) {
    CONFERE_IGUAL(sim.core.trackState(t), LooperCore::Playing);
    CONFERE_IGUAL(sim.core.trackLength(t), C);
  }
  sim.ate(g0 + 5 * C);
  int64_t c = g0 + 4 * C;
  auto p = sim.picos(c, c + C);
  CONFERE_MSG(mesmosPicos(p, {c + 1000, c + 2000, c + 3000, c + 50000}), "picos: %s", lista(p, c).c_str());
  CONFERE_PERTO(sim.s(c + 2000), 0.4, 1e-6);
  // Mudo na faixa 1: somem exatamente os dois impulsos dela.
  sim.core.toggleMute(1);
  sim.ate(g0 + 7 * C);
  c = g0 + 6 * C;
  p = sim.picos(c, c + C);
  CONFERE_MSG(mesmosPicos(p, {c + 1000, c + 3000}), "mudo f1: %s", lista(p, c).c_str());
}

TESTE(ao_tocar_espera_a_nota_e_pre_gravacao_pega_o_comeco) {
  LooperSim sim(48000.0, 10.0, L);
  configurar(sim, LooperCore::StartOnSound, false, 6);
  sim.core.setTriggerLevel(0.08f);
  CONFERE(sim.core.press(0));
  const int64_t S = sim.n + 30000;   // a nota começa 30000 amostras depois (rampa de 200 amostras)
  auto nota = [S](int64_t i) -> float {
    if (i < S) return 0.0f;
    const double t = static_cast<double>(i - S);
    if (t < 200) return static_cast<float>(0.5 * t / 200.0);
    return static_cast<float>(0.5 * std::exp(-(t - 200) / 20000.0) * std::cos((t - 200) * 0.01));
  };
  sim.fonte = [&](int64_t i) { return i < S + 60000 ? nota(i) : 0.0f; };
  sim.ate(S - 512);
  CONFERE_IGUAL(sim.core.phase(), LooperCore::PhaseWaitingSound);
  CONFERE(!sim.core.playing());
  // Gatilho: 1ª amostra com |x| > 0,08.
  int64_t gat = S;
  while (nota(gat) <= 0.08f) gat++;
  sim.ate(gat + 2 * C + 2 * L);
  CONFERE_IGUAL(sim.core.baseLength(), C);
  CONFERE_IGUAL(sim.core.trackState(0), LooperCore::Playing);
  // No gatilho G = L + 480 (a amostra é escrita na posição 480 = G − L). A reprodução lê a posição
  // wrap(G, C): a amostra global g toca a posição wrap(g − gat + L + 480, C) do loop.
  const int pre = 480;
  sim.fonte = nullptr;
  const int64_t c0 = gat - pre - L + 2 * C;   // início de um ciclo de reprodução (posição 0)
  sim.ate(c0 + C + 256);
  int64_t primeiro = -1;
  for (int64_t k = 0; k < C; k++) if (std::fabs(sim.s(c0 + k)) > 1e-6f) { primeiro = k; break; }
  // A nota começa (amostra S + 1 é a 1ª não nula) em pre − (gat − S) + 1.
  const int64_t esperado = pre - (gat - S) + 1;
  CONFERE_MSG(primeiro == esperado, "1ª amostra da nota no loop em %lld, esperado %lld (gatilho %lld amostras após o início)",
              static_cast<long long>(primeiro), static_cast<long long>(esperado), static_cast<long long>(gat - S));
  double erro = 0;
  for (int64_t k = 0; k < 2000; k++) erro = std::max(erro, static_cast<double>(std::fabs(sim.s(c0 + esperado - 1 + k) - nota(S + k))));
  CONFERE_MSG(erro < 1e-6, "loop difere da nota original (erro máx %.3g)", erro);
}

TESTE(gravar_por_cima_armado_entra_no_proximo_ciclo) {
  LooperSim sim(48000.0, 10.0, L);
  configurar(sim, LooperCore::StartCountIn, false, 6);
  const int64_t g0 = baseComContagem(sim);
  sim.ate(g0 + C + 40000);               // meio do ciclo 1
  CONFERE(sim.core.press(0));
  CONFERE_IGUAL(sim.core.trackState(0), LooperCore::Armed);
  const int64_t inicio = g0 + 2 * C;
  sim.impulso(inicio - 100 + L, 0.7f);   // antes do ciclo: ignorado
  sim.impulso(inicio + 3000 + L, 0.3f);  // dentro: gravado na posição 3000
  sim.impulso(inicio + C + 50 + L, 0.6f);// depois de uma volta: ignorado
  sim.ate(inicio - 256);
  CONFERE_IGUAL(sim.core.trackState(0), LooperCore::Armed);
  sim.ate(inicio + 512);
  CONFERE_IGUAL(sim.core.trackState(0), LooperCore::Overdubbing);
  sim.ate(inicio + C + L + 512);
  CONFERE_IGUAL(sim.core.trackState(0), LooperCore::Playing);
  sim.ate(g0 + 5 * C);
  const int64_t c = g0 + 4 * C;
  const auto p = sim.picos(c, c + C);
  CONFERE_MSG(mesmosPicos(p, {c + 1000, c + 3000}), "picos: %s", lista(p, c).c_str());
}

TESTE(cancelar_armada) {
  LooperSim sim(48000.0, 10.0, L);
  configurar(sim, LooperCore::StartCountIn, false, 6);
  const int64_t g0 = baseComContagem(sim);
  const int64_t bytesBase = sim.core.bytesInUse();
  sim.ate(g0 + C + 10000);
  CONFERE(sim.core.press(0));            // arma sobregravação
  CONFERE(sim.core.press(1));            // arma faixa vazia
  sim.blocos(1);
  CONFERE_IGUAL(sim.core.trackState(0), LooperCore::Armed);
  CONFERE_IGUAL(sim.core.trackState(1), LooperCore::Armed);
  CONFERE(sim.core.press(0));            // cancela
  CONFERE(sim.core.press(1));            // cancela
  sim.blocos(2);
  CONFERE_IGUAL(sim.core.trackState(0), LooperCore::Playing);
  CONFERE_IGUAL(sim.core.trackState(1), LooperCore::Empty);
  CONFERE_IGUAL(sim.core.trackLength(1), 0);
  CONFERE_IGUAL(sim.core.bytesInUse(), bytesBase);
  sim.impulso(g0 + 2 * C + 3000 + L, 0.3f);
  sim.ate(g0 + 4 * C);
  const int64_t c = g0 + 3 * C;
  const auto p = sim.picos(c, c + C);
  CONFERE_MSG(mesmosPicos(p, {c + 1000}), "picos: %s", lista(p, c).c_str());
}

TESTE(cancelar_durante_a_contagem) {
  LooperSim sim(48000.0, 10.0, L);
  configurar(sim, LooperCore::StartCountIn, true, 3);
  CONFERE(sim.core.press(0));
  sim.blocos(100);
  CONFERE_IGUAL(sim.core.phase(), LooperCore::PhaseCountIn);
  CONFERE(sim.core.press(2));            // qualquer botão durante a base automática cancela tudo
  sim.blocos(2);
  for (int t = 0; t < 6; t++) CONFERE_IGUAL(sim.core.trackState(t), LooperCore::Empty);
  CONFERE_IGUAL(sim.core.phase(), LooperCore::PhaseNone);
  CONFERE_IGUAL(sim.core.baseLength(), 0);
  CONFERE(!sim.core.playing());
  CONFERE(!sim.core.anyContent());
  CONFERE_IGUAL(sim.core.bytesInUse(), 0);
  const int64_t a = sim.n;
  sim.ate(a + 2 * C);
  CONFERE(sim.picos(a, a + 2 * C).empty());   // o clique parou
}

TESTE(parar_tudo_com_faixa_armada) {
  LooperSim sim(48000.0, 10.0, L);
  configurar(sim, LooperCore::StartCountIn, false, 6);
  const int64_t g0 = baseComContagem(sim);
  const int64_t bytesBase = sim.core.bytesInUse();
  sim.ate(g0 + C + 10000);
  CONFERE(sim.core.press(0));
  CONFERE(sim.core.press(1));
  sim.blocos(1);
  sim.core.stopAll();
  sim.blocos(2);
  CONFERE(!sim.core.playing());
  CONFERE_IGUAL(sim.core.trackState(0), LooperCore::Playing);
  CONFERE_IGUAL(sim.core.trackState(1), LooperCore::Empty);
  CONFERE_IGUAL(sim.core.bytesInUse(), bytesBase);
  const int64_t a = sim.n;
  sim.ate(a + 2 * C);
  CONFERE(sim.picos(a, a + 2 * C).empty());
}

int main() { return rodarTestes(); }
