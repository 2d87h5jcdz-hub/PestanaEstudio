import AVFoundation
import Combine
import Foundation
import UIKit

// Transporte do StudioEngine: tocar, parar, gravar, buscar; agendamento sincronizado; tomada; timer.

@MainActor
extension StudioEngine {

    // MARK: - Transporte

    func play(from start: TimeInterval? = nil) {
        switch transport {
        case .countIn, .recording: return
        default: break
        }
        guard project != nil else { fail(.noProjectOpen); return }
        if !isActive { activate() }
        guard ensureEngineRunning() else { return }
        if transport == .playing { stopAudio() }

        let total = duration
        var from = max(0, start ?? position)
        if total > 0 && from >= total - 0.05 { from = 0 }   // no fim: recomeça do início

        guard startTransport(from: from, leadIn: 0, recordFile: nil, silencing: nil) else {
            transport = .stopped
            return
        }
        position = from
        transport = .playing
    }

    /// Para a reprodução/gravação. Depois de gravar, a cabeça volta ao ponto onde a tomada começou
    /// (para gravar a próxima camada no mesmo trecho). Parar de novo, já parado, volta ao início.
    func stop() {
        switch transport {
        case .stopped:
            position = 0
        case .playing:
            stopAudio()
            transport = .stopped
            trackLevels = [:]
            if needsRebuild { rebuildEngineIfActive() }
        case .countIn, .recording:
            let reachedRecording = transport == .recording
            stopAudio()
            transport = .stopped
            trackLevels = [:]
            position = take?.startPosition ?? playStartPosition
            recordingStart = nil
            finishTake(reportEmpty: reachedRecording)
            if needsRebuild { rebuildEngineIfActive() }
        }
    }

    func record() {
        switch transport {
        case .countIn, .recording: return
        default: break
        }
        guard let current = project else { fail(.noProjectOpen); return }
        if !isActive { activate() }

        let permission = AVAudioSession.sharedInstance().recordPermission
        if permission == .denied {
            micGranted = false
            fail(.microphoneDenied)
            return
        }
        if permission == .undetermined {
            recordAfterPermission = true
            requestMicPermission()
            return
        }
        if !micGranted {
            micGranted = true
            rebuildEngine()
        }
        guard ensureEngineRunning(), let engine, inputTapInstalled else { fail(.noInput); return }

        let startPosition = max(0, position)
        if transport == .playing {
            stopAudio()
            transport = .stopped
        }

        let inFormat = engine.inputNode.outputFormat(forBus: 0)
        guard inFormat.sampleRate > 0, inFormat.channelCount > 0 else { fail(.noInput); return }

        // Arquivo da tomada: PCM float32 em .caf (o tap entrega float32 não intercalado).
        let fileName = "take-\(UUID().uuidString).caf"
        let folder = store.folder(for: current.id)
        let url = folder.appendingPathComponent(fileName)
        let settings: [String: Any] = [
            AVFormatIDKey: kAudioFormatLinearPCM,
            AVSampleRateKey: inFormat.sampleRate,
            AVNumberOfChannelsKey: inFormat.channelCount,
            AVLinearPCMBitDepthKey: 32,
            AVLinearPCMIsFloatKey: true,
            AVLinearPCMIsBigEndianKey: false,
            AVLinearPCMIsNonInterleaved: false,
        ]
        let file: AVAudioFile
        do {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            file = try AVAudioFile(forWriting: url, settings: settings,
                                   commonFormat: .pcmFormatFloat32, interleaved: false)
        } catch {
            lastError = "Não foi possível criar o arquivo da gravação."
            return
        }

        // Faixa de destino: a armada; sem nada armado, cria uma faixa vazia e a arma (v1: faixa nova).
        // Adicionar faixa vazia não reconstrói o grafo (não muda o que toca), só salva.
        let targetID: UUID
        let created: Bool
        if let armed = armedTrackID, current.tracks.contains(where: { $0.id == armed }) {
            targetID = armed
            created = false
        } else {
            var track = StudioTrack(name: nextTrackName())
            track.effects = monitorEffects
            project?.tracks.append(track)       // didSet: só salva (faixa sem clips)
            armedTrackID = track.id
            targetID = track.id
            created = true
        }

        let beat = 60.0 / max(20, current.bpm)
        let lead = Double(max(0, countInBars) * max(1, current.beatsPerBar)) * beat
        take = PendingTake(fileName: fileName, url: url, startPosition: startPosition,
                           latency: session.inputOutputLatency,
                           sampleRate: inFormat.sampleRate, trackID: targetID, createdTrack: created)

        // A faixa armada não toca durante a gravação: o trecho a partir daqui será substituído.
        guard startTransport(from: startPosition, leadIn: lead, recordFile: file, silencing: targetID) else {
            take = nil
            _ = recorder.finish()
            try? FileManager.default.removeItem(at: url)
            if created { removeEmptyTrack(targetID) }
            return
        }
        position = startPosition
        recordingStart = startPosition
        transport = lead > 0 ? .countIn(beat: 1) : .recording
    }

    /// Quanto a cabeça pode passar do fim da música (para gravar mais adiante numa faixa vazia/armada).
    static let seekBeyondEnd: TimeInterval = 600

    func seek(to time: TimeInterval) {
        guard time.isFinite else { return }
        let total = duration
        let limit = max(total, position) + Self.seekBeyondEnd
        let target = min(max(0, time), limit)
        switch transport {
        case .stopped:
            position = target
        case .playing:
            stopAudio()
            if total > 0 && target >= total - 0.05 {
                // Depois do fim não há o que tocar: para e deixa a cabeça ali (pronta para gravar).
                transport = .stopped
                trackLevels = [:]
                position = target
                if needsRebuild { rebuildEngineIfActive() }
                return
            }
            if startTransport(from: target, leadIn: 0, recordFile: nil, silencing: nil) {
                position = target
            } else {
                transport = .stopped
            }
        case .countIn, .recording:
            break   // não dá para mover a cabeça durante a gravação
        }
    }

    /// Leva a cabeça de reprodução ao fim da faixa ("continuar de onde parou").
    func seekToEnd(of trackID: UUID) {
        guard let track = project?.tracks.first(where: { $0.id == trackID }) else { return }
        seek(to: track.end)
    }

    // MARK: - Agendamento

    /// Agenda todas as faixas, o clique e (opcional) a gravação a partir do mesmo instante de host.
    /// `leadIn` = duração da contagem: a posição `from` da música soa leadIn segundos após o início.
    /// `silencing` = faixa que não toca (a faixa armada durante a gravação).
    private func startTransport(from: TimeInterval, leadIn: TimeInterval, recordFile: AVAudioFile?,
                                silencing: UUID?) -> Bool {
        guard let engine, engine.isRunning, let project else {
            lastError = "O áudio do Estúdio não está pronto. Tente de novo."
            return false
        }
        let startHost = StudioHostClock.now() &+ StudioHostClock.ticks(forSeconds: 0.08)
        let startTime = AVAudioTime(hostTime: startHost)

        for track in project.tracks {
            guard let chain = chains[track.id] else { continue }
            if track.id == silencing {
                chain.stop()
                continue
            }
            if chain.schedule(songPosition: from, leadIn: leadIn) {
                chain.play(at: startTime)
            }
        }
        if let recordFile {
            recorder.arm(file: recordFile, startHost: startHost &+ StudioHostClock.ticks(forSeconds: leadIn))
        }
        click?.start(startHost: startHost,
                     songTimeAtStart: from - leadIn,
                     countInEnd: recordFile != nil ? from : -Double.infinity,
                     bpm: project.bpm,
                     beatsPerBar: project.beatsPerBar,
                     clickOn: clickEnabled)

        playStartHost = startHost
        playStartPosition = from
        self.leadIn = leadIn
        UIApplication.shared.isIdleTimerDisabled = true
        return true
    }

    private func stopAudio() {
        for chain in chains.values { chain.stop() }
        click?.stop()
        UIApplication.shared.isIdleTimerDisabled = false
    }

    // MARK: - Tomada

    private func finishTake(reportEmpty: Bool) {
        recordingStart = nil
        guard let pending = take else {
            _ = recorder.finish()
            return
        }
        take = nil
        let result = recorder.finish()     // fecha o arquivo
        let seconds = pending.sampleRate > 0 ? Double(result.frames) / pending.sampleRate : 0

        func discard() {
            try? FileManager.default.removeItem(at: pending.url)
            if pending.createdTrack { removeEmptyTrack(pending.trackID) }
        }

        if let failure = result.failure {
            discard()
            lastError = "Erro ao gravar: \(failure)."
            return
        }
        guard seconds >= 0.2 else {
            discard()
            if reportEmpty { lastError = "A gravação ficou curta demais e não foi salva." }
            return
        }
        guard let current = project else {
            try? FileManager.default.removeItem(at: pending.url)
            return
        }

        // Clip novo, com a compensação de latência. Começo antes de 0 é aparado (fileStart).
        var clip = StudioClip(fileName: pending.fileName, offset: pending.startPosition - pending.latency,
                              fileStart: 0, duration: seconds)
        if clip.offset < 0 {
            let cut = -clip.offset
            clip.offset = 0
            clip.fileStart = cut
            clip.duration = seconds - cut
        }
        guard clip.duration >= 0.05 else {
            discard()
            if reportEmpty { lastError = "A gravação ficou curta demais e não foi salva." }
            return
        }

        if let index = current.tracks.firstIndex(where: { $0.id == pending.trackID }) {
            var track = current.tracks[index]
            let previousFiles = Set(track.clips.map { $0.fileName })
            // Primeira gravação numa faixa sem efeito: herda o efeito do monitoramento (como na v1).
            if track.clips.isEmpty && track.effects == EffectSettings.preset(.none) {
                track.effects = monitorEffects
            }
            track.clips = StudioTimeline.punchIn(clip, into: track.clips)
            track.studioSyncLegacyFields()
            project?.tracks[index] = track      // didSet (transporte já parado) reconstrói o grafo e salva
            if let updated = project {
                StudioTimeline.deleteUnreferenced(previousFiles, project: updated,
                                                  folder: store.folder(for: current.id))
            }
        } else {
            // A faixa sumiu durante a gravação: a tomada vira uma faixa nova (comportamento v1).
            var track = StudioTrack(name: nextTrackName())
            track.effects = monitorEffects
            track.isImported = false
            track.clips = [clip]
            track.studioSyncLegacyFields()
            project?.tracks.append(track)
            armedTrackID = track.id
        }
    }

    /// Descarta a tomada em andamento (usado quando o projeto é trocado por fora no meio da gravação).
    func abortTransport() {
        stopAudio()
        transport = .stopped
        trackLevels = [:]
        recordingStart = nil
        if let pending = take {
            take = nil
            _ = recorder.finish()
            try? FileManager.default.removeItem(at: pending.url)
        }
    }

    func nextTrackName() -> String {
        let names = Set(project?.tracks.map { $0.name } ?? [])
        var n = (project?.tracks.count ?? 0) + 1
        while names.contains("Faixa \(n)") { n += 1 }
        return "Faixa \(n)"
    }

    // MARK: - Timer (~20 Hz): posição, contagem e medidores

    func startTimer() {
        guard timerCancellable == nil else { return }
        timerCancellable = Timer.publish(every: 0.05, on: .main, in: .common)
            .autoconnect()
            .sink { [weak self] _ in self?.tick() }
    }

    private func tick() {
        if meters.takeClip() { clipUntil = Date().addingTimeInterval(2) }
        let clipping = Date() < clipUntil
        if clipping != inputClipping { inputClipping = clipping }
        let levels = meters.take()
        let smoothedInput = max(levels.input, inputLevel * 0.82)
        let newInput: Float = smoothedInput < 0.01 ? 0 : smoothedInput
        if abs(newInput - inputLevel) > 0.002 { inputLevel = newInput }

        guard transport != .stopped else { return }
        let elapsed = max(0, StudioHostClock.seconds(from: playStartHost, to: StudioHostClock.now()))
        let songTime = playStartPosition - leadIn + elapsed

        switch transport {
        case .countIn:
            if songTime >= playStartPosition {
                transport = .recording
            } else {
                let beat = countInBeat(at: songTime)
                if transport != .countIn(beat: beat) { transport = .countIn(beat: beat) }
            }
            position = playStartPosition
        case .recording:
            position = max(playStartPosition, songTime)
        case .playing:
            position = max(0, songTime)
            let total = duration
            if total > 0 && position >= total + 0.1 {
                stop()
                position = 0
                return
            }
        case .stopped:
            break
        }

        var newLevels: [UUID: Float] = [:]
        for id in chains.keys {
            let previous = trackLevels[id] ?? 0
            let value = max(levels.tracks[id] ?? 0, previous * 0.82)
            newLevels[id] = value < 0.01 ? 0 : value
        }
        if newLevels != trackLevels { trackLevels = newLevels }
    }

    /// Tempo do compasso (1…beatsPerBar) na grade da música — o mesmo usado pelo clique.
    private func countInBeat(at songTime: TimeInterval) -> Int {
        let bpm = max(20, project?.bpm ?? 90)
        let bpb = max(1, project?.beatsPerBar ?? 4)
        let index = Int((songTime / (60.0 / bpm)).rounded(.down))
        return ((index % bpb) + bpb) % bpb + 1
    }
}
