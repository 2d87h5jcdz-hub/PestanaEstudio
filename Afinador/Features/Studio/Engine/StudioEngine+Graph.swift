import AVFoundation
import Combine
import Foundation

// Grafo de áudio do StudioEngine (AVAudioEngine), monitoramento e aprendizado do ruído do monitor.

@MainActor
extension StudioEngine {

    // MARK: - Grafo de áudio

    func rebuildEngineIfActive() {
        if isActive { rebuildEngine() } else { needsRebuild = false }
    }

    func rebuildEngine() {
        teardownEngine()
        needsRebuild = false
        guard isActive else { return }

        let newEngine = AVAudioEngine()
        let outFormat = newEngine.outputNode.outputFormat(forBus: 0)
        let sampleRate = outFormat.sampleRate > 0 ? outFormat.sampleRate : 44_100
        guard let format = AVAudioFormat(standardFormatWithSampleRate: sampleRate, channels: 2) else {
            lastError = "Formato de áudio do aparelho não suportado."
            return
        }
        engine = newEngine

        let master = AVAudioMixerNode()
        let limiter = AppleAudioUnits.makeLimiter()
        newEngine.attach(master)
        newEngine.attach(limiter)
        newEngine.connect(master, to: limiter, format: format)
        newEngine.connect(limiter, to: newEngine.mainMixerNode, format: format)
        masterMixer = master

        buildTrackChains(in: newEngine, destination: master, format: format)
        buildClick(in: newEngine, sampleRate: sampleRate)
        if micGranted { wireInput(in: newEngine, format: format) }

        engineConfigCancellable = NotificationCenter.default
            .publisher(for: .AVAudioEngineConfigurationChange, object: newEngine)
            .receive(on: DispatchQueue.main)
            .sink { [weak self] _ in self?.handleEngineConfigurationChange() }

        updateRouteInfo()
        newEngine.prepare()
        do {
            try newEngine.start()
        } catch {
            lastError = "Não foi possível iniciar o áudio do Estúdio. Feche outros apps de áudio e tente de novo."
        }
    }

    private func buildTrackChains(in engine: AVAudioEngine, destination: AVAudioNode, format: AVAudioFormat) {
        guard let project else { return }
        let folder = store.folder(for: project.id)
        var missing: [String] = []
        for track in project.tracks where !track.clips.isEmpty {
            let clips: [StudioClip] = StudioPitchFiles.playbackClips(for: track, folder: folder)
            let chain = StudioTrackChain(trackID: track.id, clips: clips, folder: folder)
            if chain.missingClips > 0 { missing.append(track.name) }
            guard chain.isPlayable else { continue }
            chain.install(in: engine, destination: destination, format: format)
            chain.output.installTap(onBus: 0, bufferSize: 1024, format: nil,
                                    block: StudioTaps.trackTap(id: track.id, meters: meters))
            chains[track.id] = chain
        }
        applyMixToChains()
        if !missing.isEmpty {
            lastError = "Áudio não encontrado para: \(missing.joined(separator: ", "))."
        }
    }

    private func buildClick(in engine: AVAudioEngine, sampleRate: Double) {
        guard let generator = StudioClickGenerator(sampleRate: sampleRate) else { return }
        let node = generator.makeNode()
        engine.attach(node)
        engine.connect(node, to: engine.mainMixerNode, format: generator.format)
        generator.setClickOn(clickEnabled)
        click = generator
    }

    private func wireInput(in engine: AVAudioEngine, format: AVAudioFormat) {
        let input = engine.inputNode
        let inFormat = input.outputFormat(forBus: 0)
        guard inFormat.sampleRate > 0, inFormat.channelCount > 0 else { return }

        // Ganho da entrada no monitoramento (a gravação recebe o mesmo ganho no tap).
        let boost = AVAudioUnitEQ(numberOfBands: 1)
        boost.bands[0].bypass = true
        boost.globalGain = inputGainDB
        let monitorIn = AVAudioMixerNode()
        let effects = EffectChain(liveNoise: true)
        let out = AVAudioMixerNode()
        engine.attach(boost)
        engine.attach(monitorIn)
        engine.attach(out)
        engine.connect(input, to: boost, format: inFormat)
        engine.connect(boost, to: monitorIn, format: inFormat)
        inputBoost = boost
        effects.install(in: engine, from: monitorIn, to: out, format: format)
        engine.connect(out, to: engine.mainMixerNode, format: format)
        effects.apply(monitorEffects)
        out.outputVolume = monitorEnabled ? 1 : 0

        input.installTap(onBus: 0, bufferSize: 1024, format: nil,
                         block: StudioTaps.inputTap(meters: meters, recorder: recorder, gain: inputGain))
        inputTapInstalled = true
        monitorChain = effects
        monitorOut = out
    }

    func teardownEngine() {
        engineConfigCancellable = nil
        guard let old = engine else { return }
        if inputTapInstalled {
            old.inputNode.removeTap(onBus: 0)
            inputTapInstalled = false
        }
        for chain in chains.values {
            chain.output.removeTap(onBus: 0)
            chain.stop()
        }
        click?.stop()
        old.stop()
        chains = [:]
        masterMixer = nil
        monitorChain = nil
        monitorOut = nil
        inputBoost = nil
        click = nil
        engine = nil
    }

    func ensureEngineRunning() -> Bool {
        if engine == nil && isActive { rebuildEngine() }
        guard let engine else { return false }
        if engine.isRunning { return true }
        do {
            try AVAudioSession.sharedInstance().setActive(true)
            engine.prepare()
            try engine.start()
            return true
        } catch {
            lastError = "Não foi possível iniciar o áudio do Estúdio. Feche outros apps de áudio e tente de novo."
            return false
        }
    }

    func applyMixToChains() {
        guard let project else { return }
        let anySolo = project.tracks.contains { $0.isSolo }
        for track in project.tracks {
            chains[track.id]?.apply(track, anySolo: anySolo)
        }
    }

    /// Fique em silêncio ~1,5 s: aprende o ruído do ambiente pela entrada e liga a redução de ruído.
    func learnMonitorNoise() {
        guard !learningNoise else { return }
        guard let chain = monitorChain else {
            lastError = "Abra uma música no Estúdio (com o microfone liberado) para aprender o ruído."
            return
        }
        learningNoise = true
        chain.learnNoise { [weak self] profile, rate in
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.learningNoise = false
                var s = self.monitorEffects
                if let profile {
                    s.noiseProfile = profile
                    s.noiseProfileRate = rate
                    s.noiseEnabled = true
                    self.monitorEffects = s
                } else {
                    self.monitorChain?.apply(s)
                    self.lastError = "Não deu para ouvir o ruído. Confira se a entrada está ligada e tente de novo."
                }
            }
        }
    }

    func monitorDidChange() {
        monitorOut?.outputVolume = monitorEnabled ? 1 : 0
        guard monitorEnabled else { return }
        if !isHeadphoneConnected { lastError = Self.feedbackWarning }
        if isActive && !micGranted {
            let permission = AVAudioSession.sharedInstance().recordPermission
            if permission == .undetermined {
                requestMicPermission()
            } else if permission == .denied {
                fail(.microphoneDenied)
            } else {
                micGranted = true
                if transport == .stopped { rebuildEngine() }
            }
        }
    }
}
