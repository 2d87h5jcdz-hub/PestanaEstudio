// LooperCore — estresse com threads de verdade: áudio numa thread, principal em outra, junções numa
// terceira. Não confere valores de áudio: serve para ASan/TSan/valgrind acharem corridas, uso após
// liberar e vazamentos. No fim confere que a memória volta a zero depois de limpar tudo.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "LooperCore.h"
#include "teste.h"

using pestana::LooperCore;

TESTE(estresse_principal_audio_e_juncao) {
  auto core = std::make_shared<LooperCore>();
  core->prepare(48000.0, 2.0);
  core->setLatencySamples(300);
  core->setMemoryBudget(64LL * 1024 * 1024);
  std::atomic<bool> fim{false};
  std::atomic<bool> finito{true};

  std::thread audio([&] {
    std::vector<float> inL(256), inR(256), outL(256), outR(256);
    uint32_t r = 1;
    while (!fim.load()) {
      for (int i = 0; i < 256; i++) {
        r = r * 1664525u + 1013904223u;
        inL[i] = inR[i] = (static_cast<float>(r >> 8) / 16777216.0f - 0.5f) * 0.2f;
      }
      const float* in[2] = {inL.data(), inR.data()};
      float* out[2] = {outL.data(), outR.data()};
      core->process(in, out, 256);
      for (int i = 0; i < 256; i++) if (!std::isfinite(outL[i])) finito.store(false);
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  });

  // Junções: a "fila de fundo" devolve resultados para a principal por uma lista protegida.
  struct Pronto { LooperCore::MergeJob job; float* merged; };
  std::mutex mx;
  std::vector<Pronto> prontos;
  std::vector<std::thread> fundos;

  std::mt19937 rng(1234);
  const auto t0 = std::chrono::steady_clock::now();
  int acoes = 0;
  while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2500)) {
    const int t = static_cast<int>(rng() % 3);
    switch (rng() % 12) {
      case 0: case 1: case 2: case 3: core->press(t); break;
      case 4: core->undo(t); break;
      case 5: core->toggleMute(t); break;
      case 6: if (rng() % 8 == 0) core->clearTrack(t); break;
      case 7: if (rng() % 20 == 0) core->clearAll(); break;
      case 8: if (rng() % 10 == 0) { core->stopAll(); } else { core->playAll(); } break;
      case 9: {
        LooperCore::MergeJob job{};
        while (core->takeMergeJob(job)) {
          fundos.emplace_back([&mx, &prontos, job, core] {   // core vivo até o commit
            float* m = LooperCore::runMerge(job);
            std::lock_guard<std::mutex> g(mx);
            prontos.push_back({job, m});
          });
        }
        break;
      }
      case 10: {
        std::vector<Pronto> agora;
        { std::lock_guard<std::mutex> g(mx); agora.swap(prontos); }
        for (auto& p : agora) core->commitMerge(p.job, p.merged);
        break;
      }
      default:
        if (rng() % 3 == 0) core->setAuto(rng() % 2 == 0, 1, static_cast<int>(rng() % 2), rng() % 2 == 0, 0, 3);
        break;
    }
    core->collectGarbage();
    acoes++;
    std::this_thread::sleep_for(std::chrono::microseconds(300 + rng() % 3000));
  }
  for (auto& f : fundos) f.join();
  for (auto& p : prontos) core->commitMerge(p.job, p.merged);
  core->clearAll();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  fim.store(true);
  audio.join();
  core->collectGarbage();
  teste::info("%g ações na principal", acoes);
  CONFERE(finito.load());
  CONFERE(!core->anyContent());
  CONFERE_IGUAL(core->bytesInUse(), 0);
}

int main() { return rodarTestes(); }
