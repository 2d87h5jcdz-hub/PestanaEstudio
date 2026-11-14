// Núcleo do Loop (pedaleira de loop) do Pestana Estúdio — C++ puro, testável fora do iOS.
//
// Relógio: G = amostras desde o início do ciclo 0 (pode ser negativo durante a contagem).
// - A primeira gravação define o CICLO (base). As outras faixas têm 1, 2 ou 4 ciclos e ficam
//   sempre sincronizadas por G.
// - Cada faixa tem até quatro buffers estéreo: `main` (consolidado), `old` (camada anterior
//   esperando ser somada ao `main`), `layer` (última camada, que dá para desfazer) e `rec` (a camada
//   sendo gravada). A reprodução soma os quatro.
// - Junção de camadas: ao começar a gravar por cima, a `layer` vira `old` na hora (sem copiar nada).
//   A soma main + old num `main` novo é feita FORA da thread principal: a principal pega o trabalho
//   (takeMergeJob), uma fila de fundo soma (runMerge) e a principal entrega o resultado (commitMerge),
//   que o áudio troca por comando. Enquanto o trabalho roda, os buffers que ele lê não são liberados
//   (ficam adiados até o commit). Só se uma nova sobregravação começar com `old` ainda pendente a
//   principal soma main + old na hora (caso raro).
// - Gravação manual começa já (no bloco em que o comando chega). Se ela fechar sem ter escrito
//   nenhuma amostra (toque duplo), o áudio devolve o buffer e avisa a principal (EvCancelled leva o
//   ponteiro): espelho e áudio nunca ficam com buffers diferentes.
// - Desfazer tira a `layer`; sem `layer`, tira a `old` (se a junção ainda não aconteceu); sem as
//   duas, apaga a faixa.
// - Memória: toda alocação passa por um orçamento opcional (setMemoryBudget); quando estoura, a ação
//   é recusada e lastError() = ErrMemory.
// - Gravação = janela [startG, stopG) no tempo do músico. A entrada que chega em G foi tocada
//   ouvindo G − L (L = latência de ida e volta), então só a entrada que chega em
//   [startG + L, stopG + L) é gravada, na posição G − L. Depois de stopG a faixa ainda recebe
//   L amostras (a "cauda") e então a camada fecha.
// - Modo automático: tempo (BPM) e compassos definem o ciclo; a base começa depois de um compasso
//   de contagem ou na primeira nota (gatilho por som) e fecha sozinha. Gravações por cima ficam
//   "armadas" até o próximo início de ciclo e gravam exatamente uma volta. Opcionalmente, as faixas
//   seguintes já ficam agendadas em sequência (gravar todas sem tocar em nada).
//
// Threads: métodos [principal] só de UMA thread; process() só da thread de áudio. Comunicação por
// filas sem lock (comandos principal → áudio, eventos áudio → principal) e atomics. Toda memória é
// alocada e liberada na thread principal (a exceção é runMerge, que aloca numa fila de fundo e
// entrega à principal). Quem roda junções em segundo plano precisa manter o LooperCore vivo até o
// commitMerge (ex.: shared_ptr capturado pelo trabalho).
#pragma once
#include <atomic>
#include <cstdint>

namespace pestana {

class LooperCore {
public:
  static constexpr int kMaxTracks = 6;
  static constexpr int kChannels = 2;

  /// Estado lógico de cada faixa (o que a tela mostra).
  enum TrackState : int { Empty = 0, Recording = 1, Playing = 2, Overdubbing = 3, Armed = 4 };
  /// Fase da base no modo automático.
  enum Phase : int { PhaseNone = 0, PhaseCountIn = 1, PhaseWaitingSound = 2 };
  enum StartMode : int { StartCountIn = 0, StartOnSound = 1 };
  /// Por que o último press() não deu certo.
  enum PressError : int { ErrNone = 0, ErrBusy = 1, ErrMemory = 2 };

  /// Trabalho de junção (main + old) para rodar fora da thread principal.
  struct MergeJob {
    int track;
    const float* main;
    const float* old;
    int64_t mainCapacity;
    int64_t oldCapacity;
    int64_t length;
  };

  LooperCore();
  ~LooperCore();
  LooperCore(const LooperCore&) = delete;
  LooperCore& operator=(const LooperCore&) = delete;

  // ---------- [principal] configuração ----------
  /// Com o áudio parado. Zera tudo (as gravações somem).
  void prepare(double sampleRate, double maxBaseSeconds);
  void setLatencySamples(int samples);
  void setMonitor(bool on) { mMonitor.store(on); }
  void setTrackVolume(int track, float volume);
  void setTrackMultiplier(int track, int multiplier);   // 1, 2 ou 4; só vale com a faixa vazia
  int trackMultiplier(int track) const;

  /// Modo automático. bars: compassos do ciclo. chainCount: quantas faixas existem na tela
  /// (a sequência vai até a última). gapCycles: ciclos livres entre uma faixa e a próxima.
  void setAuto(bool on, int bars, int startMode, bool chain, int gapCycles, int chainCount);
  void setTempo(double bpm, int beatsPerBar);
  void setClick(bool duringLoop, float volume);
  /// Nível (0…1) que dispara a gravação no modo "ao tocar".
  void setTriggerLevel(float level) { mTriggerLevel.store(level); }
  bool autoMode() const { return mAuto; }
  /// Comprimento do ciclo no modo automático, em amostras.
  int64_t autoCycleLength() const;

  // ---------- [principal] ações (botões / pedal) ----------
  /// Botão da faixa. Manual: vazia → grava; gravando → toca (fecha); tocando → sobregrava;
  /// sobregravando → toca. Automático: vazia/tocando → arma (entra no próximo ciclo e grava uma
  /// volta); armada → cancela; gravando → para já. Durante a base automática, cancela tudo.
  /// false se não deu agora (memória, ciclo ainda não informado, cauda da gravação anterior);
  /// o motivo fica em lastError().
  bool press(int track);
  /// Motivo da última recusa de press() (ErrNone se deu certo).
  PressError lastError() const { return mLastError; }
  void toggleMute(int track);
  void undo(int track);
  void clearTrack(int track);
  void stopAll();
  void playAll();
  void clearAll();
  /// Processa eventos da thread de áudio e libera buffers devolvidos. Chamar periodicamente.
  void collectGarbage();

  // ---------- [principal] memória ----------
  /// Limite de bytes dos buffers de áudio (0 = sem limite).
  void setMemoryBudget(int64_t bytes) { mBudget = bytes > 0 ? bytes : 0; }
  /// Bytes alocados agora (inclui buffers devolvidos ainda não liberados e a reserva das junções
  /// em andamento).
  int64_t bytesInUse() const { return mBytesInUse; }

  // ---------- [qualquer thread] diagnóstico ----------
  /// Eventos/buffers descartados por fila cheia (não deve passar de 0; buffer descartado = vazamento).
  int droppedEvents() const { return mDropped.load(std::memory_order_relaxed); }
  /// Vezes em que o áudio achou um estado que a principal garante não acontecer (não deve passar de 0).
  int inconsistencies() const { return mInconsistent.load(std::memory_order_relaxed); }

  // ---------- junção de camadas em segundo plano ----------
  /// [principal] Pega uma faixa com `old` e sem junção em andamento. Reserva no orçamento os bytes do
  /// resultado (devolvidos se a junção for descartada); se a reserva não couber, não pega agora.
  /// false se não há nada a juntar (ou não coube).
  bool takeMergeJob(MergeJob& job);
  /// [qualquer thread] Soma main + old num buffer novo (planar, capacity = length). nullptr se faltar memória.
  static float* runMerge(const MergeJob& job);
  /// [principal] Entrega o resultado (ou nullptr). Se a faixa mudou nesse meio-tempo, descarta.
  void commitMerge(const MergeJob& job, float* merged);

  // ---------- [principal] leitura de estado ----------
  TrackState trackState(int track) const;
  bool trackMuted(int track) const;
  bool trackCanUndo(int track) const;
  float trackProgress(int track) const;
  float trackLevel(int track) const;
  int64_t trackLength(int track) const;
  int64_t baseLength() const { return mBaseLen.load(); }
  bool playing() const { return mPlaying.load(); }
  int64_t position() const { return mPosition.load(); }
  Phase phase() const { return static_cast<Phase>(mPhase.load()); }
  /// Batida atual (0…beats−1) e se o relógio está andando com tempo (modo automático).
  int currentBeat() const { return mBeat.load(); }
  float inputLevel() const { return mInputPeak.load(); }
  double sampleRate() const { return mSampleRate; }
  bool anyContent() const;

  // ---------- thread de áudio ----------
  void process(const float* const* in, float* const* out, int frames);

private:
  struct Buffer {
    float* data = nullptr;   // planar: canal 0 em [0, cap), canal 1 em [cap, 2·cap)
    int64_t capacity = 0;
    float* ch(int c) const { return data + static_cast<size_t>(c) * static_cast<size_t>(capacity); }
  };

  static constexpr int64_t kNever = INT64_MAX / 4;

  enum CommandType : int {
    CmdStartRecord,   // manual: começa já
    CmdStartBase,     // ciclo base (manual ou automático)
    CmdSchedule,      // automático: grava uma volta a partir de startG (−1 = próximo início de ciclo)
    CmdStopRecord, CmdCancelArm,
    CmdSwapMain,      // troca main pelo buffer do comando; main antigo e old vão para o retire
    CmdLayerToOld,    // layer vira old (início de sobregravação)
    CmdDropLayer, CmdDropOld, CmdClearTrack, CmdClearAll,
    CmdPlay, CmdStop, CmdMute,
  };
  struct Command {
    CommandType type;
    int track;
    Buffer buffer;
    int64_t length;     // StartRecord/Schedule: comprimento da faixa; StartBase: comprimento fixo (0 = manual)
    int64_t startG;     // Schedule: início (−1 = próximo ciclo); StartBase: comprimento da contagem
    int mode;           // StartBase: StartMode
    bool flag;          // Mute
  };

  enum EventType : int { EvStarted, EvFinished, EvBaseClosed, EvCancelled, EvBaseDiscarded };
  /// `data`: EvCancelled leva o buffer que o áudio devolveu (a principal tira do espelho).
  struct Event { EventType type; int track; const float* data; };

  struct AudioTrack {
    Buffer main, old, layer, rec;
    bool hasRec = false;        // rec ativo (armado, gravando ou cauda)
    bool started = false;       // já passou de startG
    bool wrote = false;         // a janela de gravação já escreveu alguma amostra
    int64_t startG = 0, stopG = kNever;
    int64_t length = 0;         // 0 = vazia
    int64_t anchor = 0;
    bool muted = false;
    bool isBase = false;        // gravando o ciclo base (comprimento ainda aberto)
    int64_t fixedBase = 0;      // base automática: fecha sozinha neste G
  };

  struct MainTrack {
    TrackState state = Empty;
    int multiplier = 1;
    bool muted = false;
    Buffer main, old, layer, rec;
  };

  /// Junção em andamento numa faixa [principal].
  struct MergeSlot {
    bool active = false;
    bool abandoned = false;     // prepare() rodou no meio: o resultado é descartado
    const float* main = nullptr;
    const float* old = nullptr;
    int64_t reserved = 0;       // bytes do resultado já contados em mBytesInUse
  };
  MergeSlot mMerge[kMaxTracks];

  /// Buffer devolvido (ponteiro + bytes contabilizados).
  struct Retired { float* data; int64_t bytes; };
  static int64_t bytesOf(const Buffer& b) {
    return b.capacity * static_cast<int64_t>(kChannels) * static_cast<int64_t>(sizeof(float));
  }


  // Pior caso realista: a fila de comandos tem 256 posições e cada comando devolve no máximo
  // kMaxTracks × 4 buffers (limpar tudo); os eventos são poucos por comando e por volta, e a principal
  // os lê a 30 Hz. As filas abaixo cobrem isso com folga, sem alocar nada no áudio.
  static constexpr int kQueueSize = 256;
  Command mQueue[kQueueSize];
  std::atomic<int> mQueueHead{0}, mQueueTail{0};
  bool push(const Command& c);

  static constexpr int kEventSize = 4096;
  Event mEvents[kEventSize];
  std::atomic<int> mEventHead{0}, mEventTail{0};
  void emit(EventType type, int track, const float* data = nullptr);   // áudio
  void handleEvent(const Event& e);       // principal

  static constexpr int kRetireSize = kQueueSize * kMaxTracks * 4 + 64;
  Retired mRetire[kRetireSize] = {};
  std::atomic<int> mRetireHead{0}, mRetireTail{0};
  void retire(const Buffer& b);   // áudio

  // [principal] buffers que um trabalho de junção ainda lê: liberados depois do commit
  static constexpr int kDeferredSize = 2 * kMaxTracks + 2;
  Retired mDeferred[kDeferredSize] = {};
  int mDeferredCount = 0;
  bool protectedByMerge(const float* p) const;
  void freeNow(const Retired& r);         // libera já ou adia se um trabalho ainda lê
  void freeOwned(const Buffer& b) { freeNow(Retired{b.data, bytesOf(b)}); }
  void flushDeferred();

  // áudio
  void applyCommand(const Command& c);
  /// Fecha a camada. Sem nenhuma amostra escrita (toque duplo, parar logo depois de começar), a
  /// camada é devolvida e a principal avisada (EvCancelled com o ponteiro) em vez de virar uma camada vazia.
  void finishRecording(int t, bool discardIfEmpty = true);
  void cancelRec(int t);      // devolve o rec ainda vazio e avisa a principal
  void closeBase(int t, int64_t base);
  void resetAudioTrack(int t);
  void writePreroll(AudioTrack& tr);

  // principal
  Buffer allocate(int64_t frames);
  void stopRecordingMain(int t);
  void mirrorFinish(MainTrack& m);
  bool startRecordingMain(int t);
  bool startAutoBase(int t);
  bool scheduleMain(int t, int64_t startG);
  bool prepareOverdubMain(int t);   // layer → old (junta main + old antes, se preciso)
  bool mergeOldMainSync(int t);
  void resetMain(int t);
  bool othersHaveContent(int t) const;

  double mSampleRate = 48000.0;
  int64_t mMaxBase = 0;
  int64_t mBudget = 0;           // [principal] 0 = sem limite
  int64_t mBytesInUse = 0;       // [principal]
  PressError mLastError = ErrNone;
  std::atomic<int> mDropped{0};
  std::atomic<int> mInconsistent{0};
  std::atomic<int64_t> mLatency{0};
  std::atomic<bool> mMonitor{true};

  // modo automático [principal] + tempo/clique (atomics: lidos pelo áudio)
  bool mAuto = false;
  int mBars = 4;
  int mStartMode = StartCountIn;
  bool mChain = false;
  int mGap = 0;
  int mChainCount = kMaxTracks;
  double mBpm = 120.0;                      // [principal]
  std::atomic<int64_t> mBeatLen{24000};
  std::atomic<int> mBeatsPerBar{4};
  std::atomic<bool> mClickDuringLoop{false};
  std::atomic<float> mClickVolume{0.5f};
  std::atomic<bool> mTempoActive{false};    // auto ligado: há grade de batidas
  std::atomic<float> mTriggerLevel{0.08f};

  AudioTrack mAudio[kMaxTracks];
  MainTrack mMain[kMaxTracks];
  int mBaseTrack = -1;                      // [principal] faixa da base em andamento

  std::atomic<int64_t> mBaseLen{0};
  std::atomic<int64_t> mPosition{0};
  std::atomic<bool> mPlaying{false};
  std::atomic<int> mPhase{PhaseNone};
  std::atomic<int> mBeat{0};
  std::atomic<int64_t> mTrackLen[kMaxTracks];
  std::atomic<int64_t> mTrackAnchor[kMaxTracks];
  std::atomic<bool> mTrackBusy[kMaxTracks];
  std::atomic<float> mTrackPeak[kMaxTracks];
  std::atomic<float> mInputPeak{0.0f};
  std::atomic<float> mTrackVolume[kMaxTracks];

  // [áudio]
  int64_t mG = 0;
  bool mPlayingAudio = false;
  bool mCountIn = false;          // G < 0 andando com clique
  bool mWaitingSound = false;     // base armada esperando a primeira nota
  int mWaitingTrack = -1;
  // pré-gravação para o gatilho por som (~10 ms)
  static constexpr int kPrerollMax = 2048;
  float mPreroll[2][kPrerollMax] = {};
  int mPrerollLen = 480;
  int mPrerollPos = 0;
  // clique
  double mClickPhase = 0.0;
  int64_t mClickRemain = 0;
  float mClickFreq = 1000.0f;
};

} // namespace pestana
