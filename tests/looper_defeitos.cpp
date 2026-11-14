// LooperCore — reproduções de defeitos achados pelo QA (devem passar depois da correção).
// Toque duplo numa sobregravação/faixa nova dentro do mesmo ciclo de áudio (~5 ms, ex.: pedal que
// "quica"): o espelho da principal e a thread de áudio passam a apontar para buffers diferentes.
#include <string>

#include "looper_sim.h"
#include "teste.h"

using pestana::LooperCore;

namespace {
constexpr int L = 1000;
constexpr int64_t B = 256 * 200;

std::string lista(const std::vector<int64_t>& v, int64_t origem) {
  std::string s;
  for (auto x : v) s += std::to_string(x - origem) + " ";
  return s.empty() ? "(nenhum)" : s;
}

int64_t gravarBase(LooperSim& sim) {
  CONFERE(sim.core.press(0));
  const int64_t g0 = sim.n;
  sim.impulso(g0 + 1000 + L, 0.5f);
  sim.ate(g0 + B);
  CONFERE(sim.core.press(0));
  sim.ate(g0 + 2 * B);
  return g0;
}
} // namespace

// Sobregravar e parar antes de o áudio rodar um bloco: o áudio cancela e devolve o `rec`, mas a
// principal (mirrorFinish) já o guardou como `layer`. Na próxima sobregravação esse ponteiro
// liberado vira `old` e a junção (runMerge / mergeOldMainSync) lê memória liberada.
TESTE(toque_duplo_na_sobregravacao_nao_deixa_ponteiro_solto) {
  LooperSim sim;
  const int64_t g0 = gravarBase(sim);
  CONFERE(sim.core.press(0));      // sobregrava...
  CONFERE(sim.core.press(0));      // ...e para no mesmo ciclo de áudio
  sim.ate(g0 + 3 * B);             // o áudio cancela e devolve o buffer; collectGarbage libera
  CONFERE(sim.core.press(0));      // nova sobregravação de verdade
  sim.impulso(g0 + 3 * B + 20000 + L, 0.3f);
  sim.ate(g0 + 4 * B);
  CONFERE(sim.core.press(0));
  sim.ate(g0 + 5 * B);
  CONFERE(sim.core.press(0));      // mais uma (vazia): a camada anterior vira old
  sim.ate(g0 + 6 * B);
  CONFERE(sim.core.press(0));
  sim.ate(g0 + 7 * B);
  LooperCore::MergeJob job{};
  if (sim.core.takeMergeJob(job)) {
    float* merged = LooperCore::runMerge(job);   // com ASan: heap-use-after-free aqui
    sim.core.commitMerge(job, merged);
  }
  sim.ate(g0 + 9 * B);
  const int64_t c = g0 + 8 * B;
  const auto p = sim.picos(c, c + B);
  CONFERE_MSG(mesmosPicos(p, {c + 1000, c + 20000}), "picos: %s", lista(p, c).c_str());
}

// Mesmo defeito pelo caminho da junção em segundo plano (runPendingMerges do AU): a `old` solta
// vai para runMerge na fila de fundo.
TESTE(toque_duplo_na_sobregravacao_juncao_em_fundo) {
  LooperSim sim;
  const int64_t g0 = gravarBase(sim);
  CONFERE(sim.core.press(0));
  CONFERE(sim.core.press(0));
  sim.ate(g0 + 3 * B);
  CONFERE(sim.core.press(0));      // a layer solta vira old
  sim.ate(g0 + 4 * B);
  CONFERE(sim.core.press(0));
  sim.ate(g0 + 5 * B);
  LooperCore::MergeJob job{};
  CONFERE(!sim.core.takeMergeJob(job));   // não deveria haver old (nada foi gravado antes)
  if (job.old != nullptr) {
    float* merged = LooperCore::runMerge(job);   // com ASan: heap-use-after-free aqui
    sim.core.commitMerge(job, merged);
  }
}

// Mesmo caso com parar tudo logo depois de começar a sobregravar.
TESTE(parar_tudo_logo_apos_sobregravar) {
  LooperSim sim;
  const int64_t g0 = gravarBase(sim);
  CONFERE(sim.core.press(0));
  sim.core.stopAll();
  sim.blocos(2);
  sim.core.playAll();
  const int64_t g1 = sim.n;
  sim.ate(g1 + 256);
  CONFERE(sim.core.press(0));
  sim.ate(g1 + B);
  CONFERE(sim.core.press(0));
  sim.ate(g1 + 2 * B);
  CONFERE(sim.core.press(0));      // a camada (que o áudio não tem) vira old
  sim.ate(g1 + 3 * B);
  CONFERE(sim.core.press(0));
  sim.ate(g1 + 4 * B);
  LooperCore::MergeJob job{};
  if (sim.core.takeMergeJob(job)) sim.core.commitMerge(job, LooperCore::runMerge(job));
  sim.ate(g1 + 6 * B);
  CONFERE(!sim.picos(g1 + 5 * B, g1 + 6 * B).empty());
  (void)g0;
}

// Faixa nova (não base) com toque duplo: o áudio cancela (comprimento 0), a principal fica com a
// faixa "tocando" sem conteúdo e todo press seguinte falha com ErrBusy.
TESTE(toque_duplo_em_faixa_nova_volta_a_vazia) {
  LooperSim sim;
  gravarBase(sim);
  CONFERE(sim.core.press(1));
  CONFERE(sim.core.press(1));
  sim.blocos(4);
  CONFERE_MSG(sim.core.trackState(1) == LooperCore::Empty || sim.core.trackLength(1) > 0,
              "faixa 1 ficou no estado %d com comprimento %lld", static_cast<int>(sim.core.trackState(1)),
              static_cast<long long>(sim.core.trackLength(1)));
  const bool deu = sim.core.press(1);
  CONFERE_MSG(deu, "press(1) depois do toque duplo falhou (lastError = %d)", static_cast<int>(sim.core.lastError()));
}

int main() { return rodarTestes(); }
