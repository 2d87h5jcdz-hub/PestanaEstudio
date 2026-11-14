// LooperCore — camadas, junção em segundo plano, desfazer e memória.
// Rode também com ASan e valgrind: vários casos só "falham" como uso após liberar ou vazamento.
#include <string>
#include <thread>

#include "looper_sim.h"
#include "teste.h"

using pestana::LooperCore;

namespace {
constexpr int L = 1000;
constexpr int64_t B = 256 * 200;                         // 51200
constexpr int64_t kMaxBase = 4 * 48000;                  // prepare(48000, 4.0)
constexpr int64_t kBytesBase = kMaxBase * 2 * 4;         // a base reserva o máximo
constexpr int64_t kBytesCiclo = B * 2 * 4;               // camada de um ciclo

std::string lista(const std::vector<int64_t>& v, int64_t origem) {
  std::string s;
  for (auto x : v) s += std::to_string(x - origem) + " ";
  return s.empty() ? "(nenhum)" : s;
}

struct Loop {
  LooperSim sim;
  int64_t g0 = 0;
  int64_t ciclo = 0;   // próximo ciclo livre (as ações acontecem nos inícios de ciclo)

  Loop() {
    CONFERE(sim.core.press(0));
    g0 = sim.n;
    sim.impulso(g0 + 1000 + L, 0.5f);
    sim.ate(g0 + B);
    CONFERE(sim.core.press(0));
    ciclo = 1;
    irPara(2);
  }
  int64_t inicio(int64_t k) const { return g0 + k * B; }
  void irPara(int64_t k) { sim.ate(inicio(k)); ciclo = k; }
  /// Sobregrava um ciclo inteiro com um impulso na posição `pos`.
  void sobregravar(int64_t pos, float amp) {
    CONFERE(sim.core.press(0));
    CONFERE_IGUAL(sim.core.trackState(0), LooperCore::Overdubbing);
    sim.impulso(inicio(ciclo) + pos + L, amp);
    irPara(ciclo + 1);
    CONFERE(sim.core.press(0));
    irPara(ciclo + 1);   // a cauda (L) termina dentro deste ciclo
  }
  /// Toca um ciclo inteiro e devolve a saída.
  std::vector<float> tocarCiclo() {
    const int64_t a = inicio(ciclo);
    irPara(ciclo + 1);
    return sim.trecho(a, B);
  }
  std::vector<int64_t> picosDoCiclo() {
    const int64_t a = inicio(ciclo);
    irPara(ciclo + 1);
    return sim.picos(a, a + B);
  }
};

bool iguais(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) if (a[i] != b[i]) return false;
  return true;
}
} // namespace

TESTE(duas_sobregravacoes_tocam_somadas) {
  Loop lp;
  lp.sobregravar(20000, 0.3f);
  lp.sobregravar(30000, 0.2f);
  auto p = lp.picosDoCiclo();
  int64_t a = lp.inicio(lp.ciclo - 1);
  CONFERE_MSG(mesmosPicos(p, {a + 1000, a + 20000, a + 30000}), "picos: %s", lista(p, a).c_str());
  // 3ª sobregravação com a junção anterior pendente (ninguém chamou takeMergeJob): soma na hora.
  lp.sobregravar(40000, 0.1f);
  p = lp.picosDoCiclo();
  a = lp.inicio(lp.ciclo - 1);
  CONFERE_MSG(mesmosPicos(p, {a + 1000, a + 20000, a + 30000, a + 40000}), "picos: %s", lista(p, a).c_str());
  CONFERE_PERTO(lp.sim.s(a + 20000), 0.3, 1e-6);
  CONFERE_PERTO(lp.sim.s(a + 40000), 0.1, 1e-6);
}

TESTE(juncao_em_thread_separada_saida_identica) {
  Loop lp;
  lp.sobregravar(20000, 0.3f);
  lp.sobregravar(30000, 0.2f);
  CONFERE_IGUAL(lp.sim.core.bytesInUse(), kBytesBase + 2 * kBytesCiclo);
  LooperCore::MergeJob job{};
  CONFERE(lp.sim.core.takeMergeJob(job));
  CONFERE_IGUAL(job.track, 0);
  CONFERE_IGUAL(job.length, B);
  LooperCore::MergeJob outro{};
  CONFERE(!lp.sim.core.takeMergeJob(outro));   // já tem junção em andamento nesta faixa

  float* merged = nullptr;
  std::thread fundo([&] { merged = LooperCore::runMerge(job); });
  const auto antes = lp.tocarCiclo();          // o áudio continua processando enquanto a junção roda
  fundo.join();
  CONFERE(merged != nullptr);
  lp.sim.core.commitMerge(job, merged);
  const auto depois1 = lp.tocarCiclo();        // o comando de troca entra no 1º bloco deste ciclo
  const auto depois2 = lp.tocarCiclo();
  CONFERE(iguais(antes, depois1));
  CONFERE(iguais(antes, depois2));
  CONFERE(!lp.sim.core.takeMergeJob(outro));   // nada mais a juntar
  // main (base, capacidade máxima) + old viraram um main do tamanho do ciclo; a layer continua.
  CONFERE_IGUAL(lp.sim.core.bytesInUse(), 2 * kBytesCiclo);
  // Desfazer agora tira só a última camada; o próximo desfazer apaga a faixa (old já foi somada).
  lp.sim.core.undo(0);
  auto p = lp.picosDoCiclo();
  int64_t a = lp.inicio(lp.ciclo - 1);
  CONFERE_MSG(mesmosPicos(p, {a + 1000, a + 20000}), "picos: %s", lista(p, a).c_str());
  lp.sim.core.undo(0);
  lp.sim.blocos(2);
  CONFERE_IGUAL(lp.sim.core.trackState(0), LooperCore::Empty);
  CONFERE_IGUAL(lp.sim.core.bytesInUse(), 0);
}

TESTE(desfazer_em_3_niveis) {
  Loop lp;
  lp.sobregravar(20000, 0.3f);
  lp.sobregravar(30000, 0.2f);
  lp.sim.core.undo(0);                         // tira a layer (30000)
  auto p = lp.picosDoCiclo();
  int64_t a = lp.inicio(lp.ciclo - 1);
  CONFERE_MSG(mesmosPicos(p, {a + 1000, a + 20000}), "1º desfazer: %s", lista(p, a).c_str());
  lp.sim.core.undo(0);                         // tira a old (20000)
  p = lp.picosDoCiclo();
  a = lp.inicio(lp.ciclo - 1);
  CONFERE_MSG(mesmosPicos(p, {a + 1000}), "2º desfazer: %s", lista(p, a).c_str());
  CONFERE_IGUAL(lp.sim.core.bytesInUse(), kBytesBase);
  lp.sim.core.undo(0);                         // apaga a faixa
  lp.sim.blocos(2);
  CONFERE_IGUAL(lp.sim.core.trackState(0), LooperCore::Empty);
  CONFERE(!lp.sim.core.anyContent());
  CONFERE_IGUAL(lp.sim.core.bytesInUse(), 0);
}

TESTE(desfazer_com_old_pendente_durante_juncao) {
  Loop lp;
  lp.sobregravar(20000, 0.3f);
  lp.sobregravar(30000, 0.2f);
  LooperCore::MergeJob job{};
  CONFERE(lp.sim.core.takeMergeJob(job));
  lp.sim.core.undo(0);                         // layer
  lp.sim.core.undo(0);                         // old (o trabalho ainda lê este buffer!)
  lp.sim.blocos(4);                            // o áudio devolve a old; a principal tem de adiar a liberação
  float* merged = nullptr;
  std::thread fundo([&] { merged = LooperCore::runMerge(job); });   // lê main + old
  fundo.join();
  lp.sim.core.commitMerge(job, merged);        // a faixa mudou: descarta
  auto p = lp.picosDoCiclo();
  int64_t a = lp.inicio(lp.ciclo - 1);
  p = lp.picosDoCiclo();
  a = lp.inicio(lp.ciclo - 1);
  CONFERE_MSG(mesmosPicos(p, {a + 1000}), "picos: %s", lista(p, a).c_str());
  CONFERE_IGUAL(lp.sim.core.bytesInUse(), kBytesBase);
  LooperCore::MergeJob outro{};
  CONFERE(!lp.sim.core.takeMergeJob(outro));
}

TESTE(apagar_faixa_durante_juncao) {
  Loop lp;
  lp.sobregravar(20000, 0.3f);
  lp.sobregravar(30000, 0.2f);
  LooperCore::MergeJob job{};
  CONFERE(lp.sim.core.takeMergeJob(job));
  float* merged = nullptr;
  std::thread fundo([&] { merged = LooperCore::runMerge(job); });
  lp.sim.core.clearTrack(0);                   // enquanto a junção pode estar lendo
  lp.sim.blocos(4);
  fundo.join();
  CONFERE(lp.sim.core.bytesInUse() > 0);       // main/old adiados até o commit
  lp.sim.core.commitMerge(job, merged);
  CONFERE_IGUAL(lp.sim.core.bytesInUse(), 0);
  CONFERE(!lp.sim.core.anyContent());
  // Leitura depois de liberar (ordem inversa: primeiro apaga, depois a junção lê).
  Loop lp2;
  lp2.sobregravar(20000, 0.3f);
  lp2.sobregravar(30000, 0.2f);
  CONFERE(lp2.sim.core.takeMergeJob(job));
  lp2.sim.core.clearTrack(0);
  lp2.sim.blocos(4);
  merged = LooperCore::runMerge(job);          // ASan acusa se main/old já foram liberados
  lp2.sim.core.commitMerge(job, merged);
  CONFERE_IGUAL(lp2.sim.core.bytesInUse(), 0);
}

TESTE(limpar_tudo_durante_juncao) {
  Loop lp;
  lp.sobregravar(20000, 0.3f);
  lp.sobregravar(30000, 0.2f);
  LooperCore::MergeJob job{};
  CONFERE(lp.sim.core.takeMergeJob(job));
  lp.sim.core.clearAll();
  lp.sim.blocos(4);
  float* merged = nullptr;
  std::thread fundo([&] { merged = LooperCore::runMerge(job); });
  lp.sim.blocos(4);
  fundo.join();
  lp.sim.core.commitMerge(job, merged);
  lp.sim.blocos(2);
  CONFERE_IGUAL(lp.sim.core.bytesInUse(), 0);
  CONFERE(!lp.sim.core.anyContent());
  // Depois dá para gravar de novo normalmente.
  CONFERE(lp.sim.core.press(0));
  lp.sim.blocos(50);
  CONFERE(lp.sim.core.press(0));
  lp.sim.blocos(10);
  CONFERE_IGUAL(lp.sim.core.baseLength(), 50 * 256);
}

TESTE(prepare_durante_juncao) {
  Loop lp;
  lp.sobregravar(20000, 0.3f);
  lp.sobregravar(30000, 0.2f);
  LooperCore::MergeJob job{};
  CONFERE(lp.sim.core.takeMergeJob(job));
  float* merged = nullptr;
  std::thread fundo([&] { merged = LooperCore::runMerge(job); });
  lp.sim.core.prepare(44100.0, 4.0);           // áudio parado, taxa nova: tudo some
  fundo.join();
  CONFERE(lp.sim.core.bytesInUse() > 0);       // adiado para a junção abandonada
  lp.sim.core.commitMerge(job, merged);
  CONFERE_IGUAL(lp.sim.core.bytesInUse(), 0);
  CONFERE(!lp.sim.core.anyContent());
  LooperCore::MergeJob outro{};
  CONFERE(!lp.sim.core.takeMergeJob(outro));
  // Ordem inversa: prepare antes de a junção ler.
  Loop lp2;
  lp2.sobregravar(20000, 0.3f);
  lp2.sobregravar(30000, 0.2f);
  CONFERE(lp2.sim.core.takeMergeJob(job));
  lp2.sim.core.prepare(48000.0, 4.0);
  merged = LooperCore::runMerge(job);
  lp2.sim.core.commitMerge(job, merged);
  CONFERE_IGUAL(lp2.sim.core.bytesInUse(), 0);
}

TESTE(commit_com_nullptr_mantem_a_old) {
  Loop lp;
  lp.sobregravar(20000, 0.3f);
  lp.sobregravar(30000, 0.2f);
  const auto antes = lp.tocarCiclo();
  LooperCore::MergeJob job{};
  CONFERE(lp.sim.core.takeMergeJob(job));
  lp.sim.core.commitMerge(job, nullptr);       // faltou memória na fila de fundo
  CONFERE(iguais(antes, lp.tocarCiclo()));
  CONFERE_IGUAL(lp.sim.core.bytesInUse(), kBytesBase + 2 * kBytesCiclo);
  CONFERE(lp.sim.core.takeMergeJob(job));      // tenta de novo depois
  lp.sim.core.commitMerge(job, LooperCore::runMerge(job));
  lp.tocarCiclo();
  CONFERE(iguais(antes, lp.tocarCiclo()));
  // Commit repetido / de trabalho velho: descarta sem quebrar.
  lp.sim.core.commitMerge(job, nullptr);
  LooperCore::MergeJob invalido{99, nullptr, nullptr, 0, 0, 0};
  lp.sim.core.commitMerge(invalido, nullptr);
}

TESTE(err_busy_na_cauda) {
  LooperSim sim(48000.0, 4.0, 4800);           // latência grande: cauda de 4800 amostras
  CONFERE(sim.core.press(0));
  const int64_t g0 = sim.n;
  sim.ate(g0 + B);
  CONFERE(sim.core.press(0));
  sim.blocos(1);
  CONFERE(!sim.core.press(0));                 // ainda escrevendo a cauda da base
  CONFERE_IGUAL(sim.core.lastError(), LooperCore::ErrBusy);
  CONFERE_IGUAL(sim.core.trackState(0), LooperCore::Playing);
  sim.ate(g0 + B + 4800 + 256);
  CONFERE(sim.core.press(0));                  // agora sobregrava
  CONFERE_IGUAL(sim.core.lastError(), LooperCore::ErrNone);
  sim.blocos(30);
  CONFERE(sim.core.press(0));
  sim.blocos(1);
  CONFERE(!sim.core.press(0));                 // cauda da sobregravação
  CONFERE_IGUAL(sim.core.lastError(), LooperCore::ErrBusy);
  // Faixa nova antes de o ciclo existir (base ainda gravando): fecha a base, não dá erro.
}

TESTE(err_memory_com_orcamento_pequeno) {
  LooperSim sim(48000.0, 4.0, L);
  sim.core.setMemoryBudget(kBytesBase - 1);
  CONFERE(!sim.core.press(0));
  CONFERE_IGUAL(sim.core.lastError(), LooperCore::ErrMemory);
  CONFERE_IGUAL(sim.core.trackState(0), LooperCore::Empty);
  CONFERE_IGUAL(sim.core.bytesInUse(), 0);

  sim.core.setMemoryBudget(kBytesBase + kBytesCiclo - 8);   // cabe a base, não cabe uma camada
  CONFERE(sim.core.press(0));
  const int64_t g0 = sim.n;
  sim.ate(g0 + B);
  CONFERE(sim.core.press(0));
  sim.ate(g0 + 2 * B);
  CONFERE_IGUAL(sim.core.bytesInUse(), kBytesBase);
  CONFERE(!sim.core.press(0));
  CONFERE_IGUAL(sim.core.lastError(), LooperCore::ErrMemory);
  CONFERE_IGUAL(sim.core.trackState(0), LooperCore::Playing);
  CONFERE(!sim.core.press(1));
  CONFERE_IGUAL(sim.core.lastError(), LooperCore::ErrMemory);
  CONFERE_IGUAL(sim.core.trackState(1), LooperCore::Empty);
  CONFERE_IGUAL(sim.core.bytesInUse(), kBytesBase);

  sim.core.setMemoryBudget(0);                // sem limite
  CONFERE(sim.core.press(1));
  sim.ate(g0 + 3 * B);
  CONFERE(sim.core.press(1));
  sim.ate(g0 + 4 * B);
  CONFERE_IGUAL(sim.core.bytesInUse(), kBytesBase + kBytesCiclo);
  sim.core.clearAll();
  sim.blocos(2);
  CONFERE_IGUAL(sim.core.bytesInUse(), 0);

  // Modo automático: ciclo maior que o máximo da base também é ErrMemory.
  sim.core.setTempo(30.0, 12);
  sim.core.setAuto(true, 32, LooperCore::StartCountIn, false, 0, 6);
  CONFERE(!sim.core.press(0));
  CONFERE_IGUAL(sim.core.lastError(), LooperCore::ErrMemory);
}

int main() { return rodarTestes(); }
