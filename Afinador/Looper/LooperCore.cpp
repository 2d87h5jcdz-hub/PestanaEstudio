#include "LooperCore.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace pestana {

namespace {
inline int64_t wrap(int64_t a, int64_t n) {
  const int64_t r = a % n;
  return r < 0 ? r + n : r;
}
inline float clampf(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }
constexpr double kTwoPi = 6.283185307179586;
} // namespace

LooperCore::LooperCore() {
  for (int t = 0; t < kMaxTracks; t++) {
    mTrackLen[t].store(0);
    mTrackAnchor[t].store(0);
    mTrackBusy[t].store(false);
    mTrackPeak[t].store(0.0f);
    mTrackVolume[t].store(1.0f);
  }
}

LooperCore::~LooperCore() {
  // Quem roda junções mantém o núcleo vivo até o commit: aqui nenhum trabalho lê mais nada.
  for (auto& tr : mAudio) {
    delete[] tr.main.data;
    delete[] tr.old.data;
    delete[] tr.layer.data;
    delete[] tr.rec.data;
  }
  int head = mQueueHead.load();
  const int tail = mQueueTail.load();
  while (head != tail) {
    const Command& c = mQueue[head];
    delete[] c.buffer.data;
    head = (head + 1) % kQueueSize;
  }
  int rh = mRetireHead.load();
  while (rh != mRetireTail.load()) {
    delete[] mRetire[rh].data;
    rh = (rh + 1) % kRetireSize;
  }
  for (int i = 0; i < mDeferredCount; i++) delete[] mDeferred[i].data;
}

// ---------------------------------------------------------------- filas

bool LooperCore::push(const Command& c) {
  const int tail = mQueueTail.load(std::memory_order_relaxed);
  const int next = (tail + 1) % kQueueSize;
  if (next == mQueueHead.load(std::memory_order_acquire)) return false;
  mQueue[tail] = c;
  mQueueTail.store(next, std::memory_order_release);
  return true;
}

void LooperCore::emit(EventType type, int track) {
  const int tail = mEventTail.load(std::memory_order_relaxed);
  const int next = (tail + 1) % kEventSize;
  if (next == mEventHead.load(std::memory_order_acquire)) return;
  mEvents[tail] = Event{type, track};
  mEventTail.store(next, std::memory_order_release);
}

void LooperCore::retire(const Buffer& b) {
  if (b.data == nullptr) return;
  const int tail = mRetireTail.load(std::memory_order_relaxed);
  const int next = (tail + 1) % kRetireSize;
  if (next == mRetireHead.load(std::memory_order_acquire)) return;  // cheia: vaza (não deve acontecer)
  mRetire[tail] = Retired{b.data, bytesOf(b)};
  mRetireTail.store(next, std::memory_order_release);
}

bool LooperCore::protectedByMerge(const float* p) const {
  if (p == nullptr) return false;
  for (const auto& j : mMerge) {
    if (j.active && (p == j.main || p == j.old)) return true;
  }
  return false;
}

void LooperCore::freeNow(const Retired& r) {
  if (r.data == nullptr) return;
  if (protectedByMerge(r.data)) {
    // Um trabalho de junção ainda lê este buffer: libera depois do commit.
    if (mDeferredCount < kDeferredSize) {
      mDeferred[mDeferredCount++] = r;
      return;
    }
    return;   // não deve acontecer (cada trabalho protege no máximo 2 buffers): vaza em vez de quebrar
  }
  delete[] r.data;
  mBytesInUse = std::max<int64_t>(0, mBytesInUse - r.bytes);
}

void LooperCore::flushDeferred() {
  int keep = 0;
  for (int i = 0; i < mDeferredCount; i++) {
    const Retired r = mDeferred[i];
    if (protectedByMerge(r.data)) {
      mDeferred[keep++] = r;
    } else {
      delete[] r.data;
      mBytesInUse = std::max<int64_t>(0, mBytesInUse - r.bytes);
    }
  }
  for (int i = keep; i < mDeferredCount; i++) mDeferred[i] = Retired{nullptr, 0};
  mDeferredCount = keep;
}

void LooperCore::collectGarbage() {
  int eh = mEventHead.load(std::memory_order_relaxed);
  const int et = mEventTail.load(std::memory_order_acquire);
  while (eh != et) {
    handleEvent(mEvents[eh]);
    eh = (eh + 1) % kEventSize;
  }
  mEventHead.store(eh, std::memory_order_release);

  int head = mRetireHead.load(std::memory_order_relaxed);
  const int tail = mRetireTail.load(std::memory_order_acquire);
  while (head != tail) {
    freeNow(mRetire[head]);
    mRetire[head] = Retired{nullptr, 0};
    head = (head + 1) % kRetireSize;
  }
  mRetireHead.store(head, std::memory_order_release);
}

LooperCore::Buffer LooperCore::allocate(int64_t frames) {
  Buffer b;
  if (frames <= 0) return b;
  const int64_t bytes = frames * static_cast<int64_t>(kChannels) * static_cast<int64_t>(sizeof(float));
  if (mBudget > 0 && mBytesInUse + bytes > mBudget) {
    mLastError = ErrMemory;
    return b;
  }
  b.data = new (std::nothrow) float[static_cast<size_t>(frames) * kChannels]();
  if (b.data == nullptr) {
    mLastError = ErrMemory;
    return b;
  }
  b.capacity = frames;
  mBytesInUse += bytes;
  return b;
}

// ---------------------------------------------------------------- junção em segundo plano

bool LooperCore::takeMergeJob(MergeJob& job) {
  for (int t = 0; t < kMaxTracks; t++) {
    MergeSlot& slot = mMerge[t];
    const MainTrack& m = mMain[t];
    if (slot.active || !m.main.data || !m.old.data) continue;
    const int64_t length = mTrackLen[t].load();
    if (length <= 0) continue;
    slot.active = true;
    slot.abandoned = false;
    slot.main = m.main.data;
    slot.old = m.old.data;
    job = MergeJob{t, m.main.data, m.old.data, m.main.capacity, m.old.capacity, length};
    return true;
  }
  return false;
}

float* LooperCore::runMerge(const MergeJob& job) {
  if (job.main == nullptr || job.old == nullptr || job.length <= 0) return nullptr;
  const int64_t len = job.length;
  const int64_t n = std::min(len, std::min(job.mainCapacity, job.oldCapacity));
  float* out = new (std::nothrow) float[static_cast<size_t>(len) * kChannels];
  if (out == nullptr) return nullptr;
  for (int c = 0; c < kChannels; c++) {
    const float* a = job.main + static_cast<size_t>(c) * static_cast<size_t>(job.mainCapacity);
    const float* b = job.old + static_cast<size_t>(c) * static_cast<size_t>(job.oldCapacity);
    float* o = out + static_cast<size_t>(c) * static_cast<size_t>(len);
    for (int64_t i = 0; i < n; i++) o[i] = a[i] + b[i];
    for (int64_t i = std::max<int64_t>(0, n); i < len; i++) o[i] = 0.0f;
  }
  return out;
}

void LooperCore::commitMerge(const MergeJob& job, float* merged) {
  if (job.track < 0 || job.track >= kMaxTracks) { delete[] merged; return; }
  MergeSlot& slot = mMerge[job.track];
  if (!slot.active || slot.main != job.main || slot.old != job.old) {
    delete[] merged;   // não é o trabalho em andamento desta faixa
    return;
  }
  const bool abandoned = slot.abandoned;
  slot = MergeSlot();
  MainTrack& m = mMain[job.track];
  if (merged != nullptr && !abandoned && m.main.data == job.main && m.old.data == job.old) {
    Buffer b;
    b.data = merged;
    b.capacity = job.length;
    if (push(Command{CmdSwapMain, job.track, b, job.length, 0, 0, false})) {
      mBytesInUse += bytesOf(b);
      m.main = b;
      m.old = Buffer();
      merged = nullptr;
    }
    // Fila cheia: descarta; a faixa continua com old e um próximo takeMergeJob tenta de novo.
  }
  delete[] merged;
  flushDeferred();
}

// ---------------------------------------------------------------- configuração

void LooperCore::prepare(double sampleRate, double maxBaseSeconds) {
  // Junções em andamento: o resultado será descartado no commit; os buffers que elas leem ficam
  // adiados (freeNow) até lá.
  for (auto& j : mMerge) if (j.active) j.abandoned = true;
  for (auto& tr : mAudio) {
    freeOwned(tr.main);
    freeOwned(tr.old);
    freeOwned(tr.layer);
    freeOwned(tr.rec);
    tr = AudioTrack();
  }
  int head = mQueueHead.load();
  while (head != mQueueTail.load()) {
    freeOwned(mQueue[head].buffer);
    mQueue[head].buffer = Buffer();
    head = (head + 1) % kQueueSize;
  }
  mQueueHead.store(0);
  mQueueTail.store(0);
  mEventHead.store(0);
  mEventTail.store(0);
  int rh = mRetireHead.load();
  while (rh != mRetireTail.load()) {
    freeNow(mRetire[rh]);
    mRetire[rh] = Retired{nullptr, 0};
    rh = (rh + 1) % kRetireSize;
  }
  mRetireHead.store(0);
  mRetireTail.store(0);
  for (int t = 0; t < kMaxTracks; t++) resetMain(t);
  // Contagem do zero: só sobra o que está adiado para junções abandonadas.
  mBytesInUse = 0;
  for (int i = 0; i < mDeferredCount; i++) mBytesInUse += mDeferred[i].bytes;
  mLastError = ErrNone;

  mSampleRate = sampleRate > 0 ? sampleRate : 48000.0;
  mMaxBase = static_cast<int64_t>(mSampleRate * std::max(2.0, maxBaseSeconds));
  mPrerollLen = std::min(kPrerollMax, static_cast<int>(mSampleRate * 0.01));
  mPrerollPos = 0;
  mBaseTrack = -1;
  mG = 0;
  mPlayingAudio = false;
  mCountIn = false;
  mWaitingSound = false;
  mWaitingTrack = -1;
  mClickRemain = 0;
  mBaseLen.store(0);
  mPosition.store(0);
  mPlaying.store(false);
  mPhase.store(PhaseNone);
  for (int t = 0; t < kMaxTracks; t++) {
    mTrackLen[t].store(0);
    mTrackAnchor[t].store(0);
    mTrackBusy[t].store(false);
    mTrackPeak[t].store(0.0f);
  }
  setTempo(mBpm, mBeatsPerBar.load());   // a batida em amostras depende da taxa
}

void LooperCore::setLatencySamples(int samples) {
  mLatency.store(std::max(0, std::min(samples, static_cast<int>(mSampleRate))));
}

void LooperCore::setTrackVolume(int track, float volume) {
  if (track < 0 || track >= kMaxTracks) return;
  mTrackVolume[track].store(clampf(volume, 0.0f, 2.0f));
}

void LooperCore::setTrackMultiplier(int track, int multiplier) {
  if (track < 0 || track >= kMaxTracks) return;
  if (mMain[track].state != Empty) return;
  mMain[track].multiplier = (multiplier == 2 || multiplier == 4) ? multiplier : 1;
}

int LooperCore::trackMultiplier(int track) const {
  if (track < 0 || track >= kMaxTracks) return 1;
  return mMain[track].multiplier;
}

void LooperCore::setAuto(bool on, int bars, int startMode, bool chain, int gapCycles, int chainCount) {
  mAuto = on;
  mBars = std::max(1, std::min(bars, 32));
  mStartMode = startMode == StartOnSound ? StartOnSound : StartCountIn;
  mChain = chain;
  mGap = std::max(0, std::min(gapCycles, 8));
  mChainCount = std::max(1, std::min(chainCount, kMaxTracks));
  mTempoActive.store(on);
}

void LooperCore::setTempo(double bpm, int beatsPerBar) {
  const double b = std::max(30.0, std::min(bpm, 300.0));
  mBpm = b;
  mBeatLen.store(std::max<int64_t>(1, static_cast<int64_t>(std::llround(mSampleRate * 60.0 / b))));
  mBeatsPerBar.store(std::max(1, std::min(beatsPerBar, 12)));
}

void LooperCore::setClick(bool duringLoop, float volume) {
  mClickDuringLoop.store(duringLoop);
  mClickVolume.store(clampf(volume, 0.0f, 1.0f));
}

int64_t LooperCore::autoCycleLength() const {
  return static_cast<int64_t>(mBars) * mBeatsPerBar.load() * mBeatLen.load();
}

// ---------------------------------------------------------------- ações (principal)

bool LooperCore::anyContent() const {
  for (const auto& m : mMain) if (m.state != Empty) return true;
  return false;
}

bool LooperCore::othersHaveContent(int t) const {
  for (int i = 0; i < kMaxTracks; i++) if (i != t && mMain[i].state != Empty) return true;
  return false;
}

void LooperCore::resetMain(int t) {
  const int mult = mMain[t].multiplier;
  mMain[t] = MainTrack();
  mMain[t].multiplier = mult < 1 ? 1 : mult;
}

void LooperCore::mirrorFinish(MainTrack& m) {
  if (m.rec.data) {
    if (!m.main.data) m.main = m.rec;
    else m.layer = m.rec;
  }
  m.rec = Buffer();
  m.state = Playing;
}

bool LooperCore::startRecordingMain(int t) {
  MainTrack& m = mMain[t];
  const bool wasEmpty = m.state == Empty;

  if (wasEmpty && !othersHaveContent(t)) {
    // Primeira gravação (manual): define o ciclo.
    Buffer b = allocate(mMaxBase);
    if (!b.data) return false;   // allocate marcou ErrMemory
    if (!push(Command{CmdStartBase, t, b, 0, 0, StartCountIn, false})) {
      freeOwned(b);
      mLastError = ErrBusy;
      return false;
    }
    m.rec = b;
    m.state = Recording;
    m.muted = false;
    mBaseTrack = t;
    return true;
  }

  const int64_t base = mBaseLen.load();
  if (base <= 0) { mLastError = ErrBusy; return false; }          // ciclo ainda não informado
  if (mTrackBusy[t].load()) { mLastError = ErrBusy; return false; }  // cauda da gravação anterior
  if (!prepareOverdubMain(t)) return false;
  const int64_t length = wasEmpty ? base * m.multiplier : mTrackLen[t].load();
  if (length <= 0) { mLastError = ErrBusy; return false; }
  Buffer b = allocate(length);
  if (!b.data) return false;
  if (!mPlaying.load()) push(Command{CmdPlay, t, {}, 0, 0, 0, false});
  if (m.muted) {
    push(Command{CmdMute, t, {}, 0, 0, 0, false});
    m.muted = false;
  }
  if (!push(Command{CmdStartRecord, t, b, length, 0, 0, false})) {
    freeOwned(b);
    mLastError = ErrBusy;
    return false;
  }
  m.rec = b;
  m.state = wasEmpty ? Recording : Overdubbing;
  return true;
}

bool LooperCore::startAutoBase(int t) {
  const int64_t cycle = autoCycleLength();
  // Ciclo maior que o limite da base (memória reservada para ela): conta como falta de memória.
  if (cycle <= 0 || cycle > mMaxBase) { mLastError = ErrMemory; return false; }
  Buffer b = allocate(cycle);
  if (!b.data) return false;
  const int64_t countIn = mStartMode == StartCountIn ? static_cast<int64_t>(mBeatsPerBar.load()) * mBeatLen.load() : 0;
  if (!push(Command{CmdStartBase, t, b, cycle, countIn, mStartMode, false})) {
    freeOwned(b);
    mLastError = ErrBusy;
    return false;
  }
  MainTrack& m = mMain[t];
  m.rec = b;
  m.state = Armed;
  m.muted = false;
  mBaseTrack = t;
  if (mChain) {
    int64_t cursor = cycle * (1 + mGap);
    for (int i = t + 1; i < mChainCount; i++) {
      if (mMain[i].state != Empty) continue;
      const int64_t len = cycle * mMain[i].multiplier;
      if (!scheduleMain(i, cursor)) break;
      cursor += len + cycle * mGap;
    }
  }
  return true;
}

bool LooperCore::scheduleMain(int t, int64_t startG) {
  MainTrack& m = mMain[t];
  if (mTrackBusy[t].load()) { mLastError = ErrBusy; return false; }
  if (!prepareOverdubMain(t)) return false;
  int64_t cycle = mBaseLen.load();
  if (cycle <= 0) cycle = autoCycleLength();
  const int64_t length = m.state == Empty ? cycle * m.multiplier : mTrackLen[t].load();
  if (length <= 0) { mLastError = ErrBusy; return false; }
  Buffer b = allocate(length);
  if (!b.data) return false;
  if (startG < 0 && !mPlaying.load()) push(Command{CmdPlay, t, {}, 0, 0, 0, false});
  if (m.muted) {
    push(Command{CmdMute, t, {}, 0, 0, 0, false});
    m.muted = false;
  }
  if (!push(Command{CmdSchedule, t, b, length, startG, 0, false})) {
    freeOwned(b);
    mLastError = ErrBusy;
    return false;
  }
  m.rec = b;
  m.state = Armed;
  mTrackBusy[t].store(true);   // até o áudio confirmar (evita armar duas vezes)
  return true;
}

void LooperCore::stopRecordingMain(int t) {
  MainTrack& m = mMain[t];
  if (m.state != Recording && m.state != Overdubbing) return;
  push(Command{CmdStopRecord, t, {}, 0, 0, 0, false});
  mirrorFinish(m);
  if (mBaseTrack == t) mBaseTrack = -1;
}

bool LooperCore::mergeOldMainSync(int t) {
  // Caso raro: nova sobregravação com a junção anterior ainda pendente. Soma main + old aqui.
  MainTrack& m = mMain[t];
  const int64_t length = mTrackLen[t].load();
  if (length <= 0 || !m.main.data || !m.old.data) { mLastError = ErrBusy; return false; }
  Buffer merged = allocate(length);
  if (!merged.data) return false;
  const int64_t n = std::min(length, std::min(m.main.capacity, m.old.capacity));
  for (int c = 0; c < kChannels; c++) {
    const float* a = m.main.ch(c);
    const float* b = m.old.ch(c);
    float* o = merged.ch(c);
    for (int64_t i = 0; i < n; i++) o[i] = a[i] + b[i];
  }
  if (!push(Command{CmdSwapMain, t, merged, length, 0, 0, false})) {
    freeOwned(merged);
    mLastError = ErrBusy;
    return false;
  }
  // Um trabalho em segundo plano desta faixa (se houver) vai ver ponteiros diferentes e descartar.
  m.main = merged;
  m.old = Buffer();
  return true;
}

bool LooperCore::prepareOverdubMain(int t) {
  MainTrack& m = mMain[t];
  if (!m.layer.data) return true;
  if (m.old.data && !mergeOldMainSync(t)) return false;
  // Instantâneo: a camada anterior vira `old` e será somada ao main em segundo plano.
  if (!push(Command{CmdLayerToOld, t, {}, 0, 0, 0, false})) { mLastError = ErrBusy; return false; }
  m.old = m.layer;
  m.layer = Buffer();
  return true;
}

bool LooperCore::press(int track) {
  mLastError = ErrNone;
  if (track < 0 || track >= kMaxTracks) return false;
  collectGarbage();   // estado em dia antes de decidir
  MainTrack& m = mMain[track];

  if (mAuto) {
    if (mBaseTrack >= 0) {     // base automática em andamento: cancela tudo
      clearAll();
      return true;
    }
    switch (m.state) {
      case Empty:
        return othersHaveContent(track) ? scheduleMain(track, -1) : startAutoBase(track);
      case Armed:
        push(Command{CmdCancelArm, track, {}, 0, 0, 0, false});
        return true;
      case Recording:
      case Overdubbing:
        stopRecordingMain(track);
        return true;
      case Playing:
        if (!mPlaying.load()) { playAll(); if (m.muted) toggleMute(track); return true; }
        if (m.muted) { toggleMute(track); return true; }
        return scheduleMain(track, -1);
    }
    return false;
  }

  if (mBaseTrack >= 0) {       // gravando o ciclo base: qualquer botão fecha o ciclo
    stopRecordingMain(mBaseTrack);
    return true;
  }
  switch (m.state) {
    case Empty:
      return startRecordingMain(track);
    case Recording:
    case Overdubbing:
      stopRecordingMain(track);
      return true;
    case Armed:
      push(Command{CmdCancelArm, track, {}, 0, 0, 0, false});
      return true;
    case Playing:
      if (!mPlaying.load()) { playAll(); if (m.muted) toggleMute(track); return true; }
      if (m.muted) { toggleMute(track); return true; }
      return startRecordingMain(track);
  }
  return false;
}

void LooperCore::toggleMute(int track) {
  if (track < 0 || track >= kMaxTracks) return;
  MainTrack& m = mMain[track];
  if (m.state == Empty || m.state == Armed) return;
  if (m.state == Recording || m.state == Overdubbing) stopRecordingMain(track);
  m.muted = !m.muted;
  push(Command{CmdMute, track, {}, 0, 0, 0, m.muted});
}

void LooperCore::undo(int track) {
  if (track < 0 || track >= kMaxTracks) return;
  collectGarbage();
  MainTrack& m = mMain[track];
  if (m.state != Playing || mTrackBusy[track].load()) return;
  // O espelho só muda se o comando entrou na fila (espelho e áudio precisam apontar para os mesmos
  // buffers: a junção em segundo plano depende disso).
  if (m.layer.data) {
    if (push(Command{CmdDropLayer, track, {}, 0, 0, 0, false})) m.layer = Buffer();
  } else if (m.old.data) {
    // Junção ainda não aconteceu: dá para tirar a camada anterior também.
    if (push(Command{CmdDropOld, track, {}, 0, 0, 0, false})) m.old = Buffer();
  } else {
    clearTrack(track);
  }
}

void LooperCore::clearTrack(int track) {
  if (track < 0 || track >= kMaxTracks) return;
  if (mMain[track].state == Empty) return;
  push(Command{CmdClearTrack, track, {}, 0, 0, 0, false});
  resetMain(track);
  if (mBaseTrack == track) mBaseTrack = -1;
}

void LooperCore::stopAll() {
  collectGarbage();
  if (mAuto && mBaseTrack >= 0) { clearAll(); return; }
  for (int t = 0; t < kMaxTracks; t++) stopRecordingMain(t);
  // Faixas armadas são canceladas pela thread de áudio (eventos).
  push(Command{CmdStop, 0, {}, 0, 0, 0, false});
}

void LooperCore::playAll() {
  bool any = false;
  for (const auto& m : mMain) if (m.state == Playing) any = true;
  if (!any) return;
  push(Command{CmdPlay, 0, {}, 0, 0, 0, false});
}

void LooperCore::clearAll() {
  push(Command{CmdClearAll, 0, {}, 0, 0, 0, false});
  for (int t = 0; t < kMaxTracks; t++) resetMain(t);
  mBaseTrack = -1;
}

void LooperCore::handleEvent(const Event& e) {
  if (e.track < 0 || e.track >= kMaxTracks) return;
  MainTrack& m = mMain[e.track];
  switch (e.type) {
    case EvStarted:
      if (m.state == Armed) m.state = m.main.data ? Overdubbing : Recording;
      break;
    case EvFinished:
      if (m.state == Recording || m.state == Overdubbing) mirrorFinish(m);
      break;
    case EvBaseClosed:
      if (mBaseTrack == e.track) mBaseTrack = -1;
      if (m.state == Recording || m.state == Armed) mirrorFinish(m);
      break;
    case EvCancelled:
      if (m.state == Armed) {
        m.rec = Buffer();   // a thread de áudio já devolveu o buffer
        m.state = m.main.data ? Playing : Empty;
      }
      break;
    case EvBaseDiscarded:
      if (m.state != Empty) resetMain(e.track);
      if (mBaseTrack == e.track) mBaseTrack = -1;
      break;
  }
}

// ---------------------------------------------------------------- leitura (principal)

LooperCore::TrackState LooperCore::trackState(int track) const {
  if (track < 0 || track >= kMaxTracks) return Empty;
  return mMain[track].state;
}

bool LooperCore::trackMuted(int track) const {
  if (track < 0 || track >= kMaxTracks) return false;
  return mMain[track].muted;
}

bool LooperCore::trackCanUndo(int track) const {
  if (track < 0 || track >= kMaxTracks) return false;
  return mMain[track].state == Playing && !mTrackBusy[track].load();
}

float LooperCore::trackProgress(int track) const {
  if (track < 0 || track >= kMaxTracks) return 0.0f;
  const int64_t len = mTrackLen[track].load();
  if (len <= 0) return 0.0f;
  return static_cast<float>(wrap(mPosition.load() - mTrackAnchor[track].load(), len)) / static_cast<float>(len);
}

float LooperCore::trackLevel(int track) const {
  if (track < 0 || track >= kMaxTracks) return 0.0f;
  return mTrackPeak[track].load();
}

int64_t LooperCore::trackLength(int track) const {
  if (track < 0 || track >= kMaxTracks) return 0;
  return mTrackLen[track].load();
}

// ---------------------------------------------------------------- thread de áudio

void LooperCore::resetAudioTrack(int t) {
  AudioTrack& tr = mAudio[t];
  retire(tr.main); retire(tr.old); retire(tr.layer); retire(tr.rec);
  const bool muted = false;
  tr = AudioTrack();
  tr.muted = muted;
  mTrackLen[t].store(0);
  mTrackAnchor[t].store(0);
  mTrackBusy[t].store(false);
  mTrackPeak[t].store(0.0f);
}

void LooperCore::finishRecording(int t) {
  AudioTrack& tr = mAudio[t];
  if (tr.rec.data) {
    if (!tr.main.data) {
      tr.main = tr.rec;
    } else if (!tr.layer.data) {
      tr.layer = tr.rec;
    } else {
      // Não deveria acontecer (a principal junta antes de sobregravar): soma na camada.
      for (int c = 0; c < kChannels; c++) {
        float* d = tr.layer.ch(c);
        const float* s = tr.rec.ch(c);
        for (int64_t i = 0; i < tr.length; i++) d[i] += s[i];
      }
      retire(tr.rec);
    }
  }
  tr.rec = Buffer();
  tr.hasRec = false;
  tr.started = false;
  tr.isBase = false;
  tr.fixedBase = 0;
  mTrackBusy[t].store(false);
  emit(EvFinished, t);
}

void LooperCore::closeBase(int t, int64_t base) {
  AudioTrack& tr = mAudio[t];
  base = std::min(base, tr.rec.capacity);
  if (base < static_cast<int64_t>(mSampleRate * 0.1)) {
    // Toque duplo sem querer: descarta.
    resetAudioTrack(t);
    mPlayingAudio = false;
    mCountIn = false;
    mG = 0;
    mPhase.store(PhaseNone);
    emit(EvBaseDiscarded, t);
    return;
  }
  tr.length = base;
  tr.anchor = 0;
  tr.isBase = false;
  tr.fixedBase = 0;
  tr.stopG = base;
  if (mG > base) mG = base;
  mBaseLen.store(base);
  mTrackLen[t].store(base);
  mTrackAnchor[t].store(0);
  emit(EvBaseClosed, t);
}

void LooperCore::writePreroll(AudioTrack& tr) {
  // O anel guarda as últimas mPrerollLen amostras (a mais antiga em mPrerollPos).
  for (int k = 0; k < mPrerollLen && k < tr.rec.capacity; k++) {
    const int idx = (mPrerollPos + k) % mPrerollLen;
    tr.rec.ch(0)[k] += mPreroll[0][idx];
    tr.rec.ch(1)[k] += mPreroll[1][idx];
  }
}

void LooperCore::applyCommand(const Command& c) {
  const int t = c.track;
  const int64_t base = mBaseLen.load();
  switch (c.type) {
    case CmdStartBase: {
      // Tudo recomeça do zero.
      resetAudioTrack(t);
      AudioTrack& tr = mAudio[t];
      tr.rec = c.buffer;
      tr.hasRec = true;
      tr.isBase = true;
      tr.started = false;
      tr.startG = 0;
      tr.fixedBase = c.length;
      tr.stopG = c.length > 0 ? c.length : kNever;
      mBaseLen.store(0);
      mTrackBusy[t].store(true);
      mCountIn = false;
      mWaitingSound = false;
      mWaitingTrack = -1;
      if (c.length > 0 && c.mode == StartOnSound) {
        mWaitingSound = true;
        mWaitingTrack = t;
        mPlayingAudio = false;
        mG = 0;
        mPhase.store(PhaseWaitingSound);
      } else if (c.length > 0 && c.startG > 0) {
        mG = -c.startG;
        mCountIn = true;
        mPlayingAudio = true;
        mPhase.store(PhaseCountIn);
      } else {
        mG = 0;
        mPlayingAudio = true;
        mPhase.store(PhaseNone);
      }
      break;
    }
    case CmdStartRecord: {
      AudioTrack& tr = mAudio[t];
      if (tr.hasRec) finishRecording(t);
      if (tr.length == 0) {
        tr.length = c.length;
        tr.anchor = (c.length > base && base > 0) ? (mG / base) * base : 0;
        mTrackLen[t].store(tr.length);
        mTrackAnchor[t].store(tr.anchor);
      }
      tr.rec = c.buffer;
      tr.hasRec = true;
      tr.started = false;
      tr.startG = mG;
      tr.stopG = kNever;
      mTrackBusy[t].store(true);
      break;
    }
    case CmdSchedule: {
      AudioTrack& tr = mAudio[t];
      if (tr.hasRec) finishRecording(t);
      int64_t start = c.startG;
      if (start < 0) {
        start = (base > 0 && mG > 0) ? ((mG + base - 1) / base) * base : 0;
      }
      if (tr.length == 0) {
        tr.length = c.length;
        tr.anchor = start;
        mTrackLen[t].store(tr.length);
        mTrackAnchor[t].store(tr.anchor);
      }
      tr.rec = c.buffer;
      tr.hasRec = true;
      tr.started = false;
      tr.startG = start;
      tr.stopG = start + tr.length;
      mTrackBusy[t].store(true);
      break;
    }
    case CmdStopRecord: {
      AudioTrack& tr = mAudio[t];
      if (!tr.hasRec) break;
      if (tr.isBase) {
        if (!tr.started) {
          resetAudioTrack(t);
          mWaitingSound = false;
          mCountIn = false;
          mPlayingAudio = false;
          mG = 0;
          mPhase.store(PhaseNone);
          emit(EvBaseDiscarded, t);
        } else {
          closeBase(t, mG);
        }
        break;
      }
      if (!tr.started) {
        // Ainda armada: cancela.
        retire(tr.rec);
        tr.rec = Buffer();
        tr.hasRec = false;
        if (!tr.main.data) { tr.length = 0; tr.anchor = 0; mTrackLen[t].store(0); mTrackAnchor[t].store(0); }
        mTrackBusy[t].store(false);
        emit(EvCancelled, t);
        break;
      }
      tr.stopG = std::min(tr.stopG, mG);
      break;
    }
    case CmdCancelArm: {
      AudioTrack& tr = mAudio[t];
      if (!tr.hasRec) break;
      if (!tr.started && !tr.isBase) {
        retire(tr.rec);
        tr.rec = Buffer();
        tr.hasRec = false;
        if (!tr.main.data) { tr.length = 0; tr.anchor = 0; mTrackLen[t].store(0); mTrackAnchor[t].store(0); }
        mTrackBusy[t].store(false);
        emit(EvCancelled, t);
      } else if (tr.started) {
        // Já começou: vira um "parar já" (o evento de fim atualiza a tela).
        tr.stopG = std::min(tr.stopG, mG);
      }
      break;
    }
    case CmdSwapMain: {
      // main novo = main + old; a layer fica como está.
      AudioTrack& tr = mAudio[t];
      retire(tr.main);
      retire(tr.old);
      tr.main = c.buffer;
      tr.old = Buffer();
      break;
    }
    case CmdLayerToOld: {
      AudioTrack& tr = mAudio[t];
      if (!tr.layer.data) break;
      if (tr.old.data) {
        // Não deveria acontecer (a principal junta antes): soma old na layer, que vira a nova old,
        // para os ponteiros continuarem iguais aos do espelho.
        const int64_t n = std::min(tr.length, std::min(tr.old.capacity, tr.layer.capacity));
        for (int ch = 0; ch < kChannels; ch++) {
          float* d = tr.layer.ch(ch);
          const float* s = tr.old.ch(ch);
          for (int64_t i = 0; i < n; i++) d[i] += s[i];
        }
        retire(tr.old);
      }
      tr.old = tr.layer;
      tr.layer = Buffer();
      break;
    }
    case CmdDropLayer: {
      AudioTrack& tr = mAudio[t];
      retire(tr.layer);
      tr.layer = Buffer();
      break;
    }
    case CmdDropOld: {
      AudioTrack& tr = mAudio[t];
      retire(tr.old);
      tr.old = Buffer();
      break;
    }
    case CmdClearTrack: {
      resetAudioTrack(t);
      bool any = false;
      for (const auto& o : mAudio) if (o.length > 0 || o.hasRec) any = true;
      if (!any) {
        mBaseLen.store(0);
        mPlayingAudio = false;
        mCountIn = false;
        mWaitingSound = false;
        mG = 0;
        mPhase.store(PhaseNone);
      }
      break;
    }
    case CmdClearAll: {
      for (int i = 0; i < kMaxTracks; i++) resetAudioTrack(i);
      mBaseLen.store(0);
      mPlayingAudio = false;
      mCountIn = false;
      mWaitingSound = false;
      mWaitingTrack = -1;
      mG = 0;
      mPhase.store(PhaseNone);
      break;
    }
    case CmdPlay:
      if (base > 0) {
        mG = 0;
        mPlayingAudio = true;
      }
      break;
    case CmdStop:
      for (int i = 0; i < kMaxTracks; i++) {
        AudioTrack& tr = mAudio[i];
        if (!tr.hasRec) continue;
        if (tr.isBase) {
          if (tr.started) { closeBase(i, mG); if (mAudio[i].hasRec) finishRecording(i); }
          else { resetAudioTrack(i); emit(EvBaseDiscarded, i); }
        } else if (tr.started) {
          finishRecording(i);
        } else {
          retire(tr.rec);
          tr.rec = Buffer();
          tr.hasRec = false;
          if (!tr.main.data) { tr.length = 0; tr.anchor = 0; mTrackLen[i].store(0); mTrackAnchor[i].store(0); }
          mTrackBusy[i].store(false);
          emit(EvCancelled, i);
        }
      }
      mPlayingAudio = false;
      mCountIn = false;
      mWaitingSound = false;
      mG = 0;
      mPhase.store(PhaseNone);
      break;
    case CmdMute:
      mAudio[t].muted = c.flag;
      break;
  }
}

void LooperCore::process(const float* const* in, float* const* out, int frames) {
  int head = mQueueHead.load(std::memory_order_relaxed);
  const int tail = mQueueTail.load(std::memory_order_acquire);
  while (head != tail) {
    applyCommand(mQueue[head]);
    head = (head + 1) % kQueueSize;
  }
  mQueueHead.store(head, std::memory_order_release);

  const bool monitor = mMonitor.load();
  const int64_t latency = mLatency.load();
  const bool tempo = mTempoActive.load();
  const bool clickLoop = mClickDuringLoop.load();
  const float clickVol = mClickVolume.load();
  const int64_t beatLen = mBeatLen.load();
  const int64_t barLen = beatLen * mBeatsPerBar.load();
  const float trigger = mTriggerLevel.load();
  const int64_t clickLen = static_cast<int64_t>(mSampleRate * 0.03);
  float volume[kMaxTracks];
  float peak[kMaxTracks];
  for (int t = 0; t < kMaxTracks; t++) {
    volume[t] = mTrackVolume[t].load();
    peak[t] = 0.0f;
  }
  float inPeak = 0.0f;

  for (int i = 0; i < frames; i++) {
    const float x0 = in[0][i];
    const float x1 = in[1][i];
    const float ax = std::max(std::fabs(x0), std::fabs(x1));
    inPeak = std::max(inPeak, ax);
    float y0 = monitor ? x0 : 0.0f;
    float y1 = monitor ? x1 : 0.0f;

    // Base esperando a primeira nota.
    if (mWaitingSound && ax > trigger && mWaitingTrack >= 0) {
      AudioTrack& tr = mAudio[mWaitingTrack];
      writePreroll(tr);
      mG = latency + mPrerollLen;   // esta amostra cai na posição mPrerollLen
      mWaitingSound = false;
      mWaitingTrack = -1;
      mPlayingAudio = true;
      mPhase.store(PhaseNone);
    }
    mPreroll[0][mPrerollPos] = x0;
    mPreroll[1][mPrerollPos] = x1;
    mPrerollPos = (mPrerollPos + 1) % std::max(1, mPrerollLen);

    if (mPlayingAudio) {
      if (mCountIn && mG >= 0) {
        mCountIn = false;
        mPhase.store(PhaseNone);
      }
      // Clique (contagem e, se ligado, durante o loop).
      if (tempo && (mCountIn || clickLoop) && beatLen > 0 && wrap(mG, beatLen) == 0) {
        mClickRemain = clickLen;
        mClickPhase = 0.0;
        mClickFreq = wrap(mG, barLen) == 0 ? 1600.0f : 1000.0f;
      }

      for (int t = 0; t < kMaxTracks; t++) {
        AudioTrack& tr = mAudio[t];
        // Base automática fecha sozinha no fim do ciclo.
        if (tr.isBase && tr.fixedBase > 0 && tr.started && mG == tr.fixedBase) closeBase(t, tr.fixedBase);

        if (tr.length > 0 && !tr.muted) {
          const int64_t idx = wrap(mG - tr.anchor, tr.length);
          float s0 = 0.0f, s1 = 0.0f;
          if (tr.main.data) { s0 += tr.main.ch(0)[idx]; s1 += tr.main.ch(1)[idx]; }
          if (tr.old.data) { s0 += tr.old.ch(0)[idx]; s1 += tr.old.ch(1)[idx]; }
          if (tr.layer.data) { s0 += tr.layer.ch(0)[idx]; s1 += tr.layer.ch(1)[idx]; }
          if (tr.rec.data && !tr.isBase) { s0 += tr.rec.ch(0)[idx]; s1 += tr.rec.ch(1)[idx]; }
          s0 *= volume[t];
          s1 *= volume[t];
          y0 += s0;
          y1 += s1;
          peak[t] = std::max(peak[t], std::max(std::fabs(s0), std::fabs(s1)));
        }

        if (!tr.hasRec || !tr.rec.data) continue;
        if (!tr.started && mG >= tr.startG) {
          tr.started = true;
          emit(EvStarted, t);
        }
        if (!tr.started) continue;
        // Escrita com compensação de latência: janela [startG + L, stopG + L).
        if (mG >= tr.startG + latency && mG < tr.stopG + latency) {
          if (tr.isBase) {
            const int64_t w = mG - latency;
            if (w >= 0 && w < tr.rec.capacity) {
              tr.rec.ch(0)[w] += x0;
              tr.rec.ch(1)[w] += x1;
            }
          } else if (tr.length > 0) {
            const int64_t w = wrap(mG - latency - tr.anchor, tr.length);
            tr.rec.ch(0)[w] += x0;
            tr.rec.ch(1)[w] += x1;
          }
        }
        if (!tr.isBase && mG + 1 >= tr.stopG + latency) finishRecording(t);
      }

      if (mClickRemain > 0) {
        const float env = static_cast<float>(mClickRemain) / static_cast<float>(std::max<int64_t>(1, clickLen));
        const float s = clickVol * env * static_cast<float>(std::sin(mClickPhase));
        mClickPhase += kTwoPi * mClickFreq / mSampleRate;
        mClickRemain--;
        y0 += s;
        y1 += s;
      }
      mG++;
    }
    out[0][i] = clampf(y0, -1.0f, 1.0f);
    out[1][i] = clampf(y1, -1.0f, 1.0f);
  }

  mPosition.store(mG);
  mPlaying.store(mPlayingAudio && !mCountIn);
  if (tempo && beatLen > 0 && barLen > 0) mBeat.store(static_cast<int>(wrap(mG, barLen) / beatLen));
  for (int t = 0; t < kMaxTracks; t++) {
    const float old = mTrackPeak[t].load(std::memory_order_relaxed);
    mTrackPeak[t].store(std::max(peak[t], old * 0.85f), std::memory_order_relaxed);
  }
  const float oldIn = mInputPeak.load(std::memory_order_relaxed);
  mInputPeak.store(std::max(inPeak, oldIn * 0.85f), std::memory_order_relaxed);
}

} // namespace pestana
