import AudioToolbox
import AVFoundation
import Foundation

// Usada ao vivo (monitoramento do Estúdio, Loop) e na exportação/renderização offline do Estúdio.
// Nada aqui é @MainActor.

// MARK: - Cadeia de efeitos: ruído → gate → EQ → compressor → guitarra (drive + amp + modulação) → delay → reverb

final class EffectChain {
    /// Redução de ruído ao vivo (AU próprio). Só atua em cadeias ao vivo (monitoramento, Loop);
    /// nas faixas do Estúdio a limpeza é feita na gravação (cópia), então aqui fica desligada.
    let noise: AVAudioUnitEffect
    let liveNoise: Bool
    private var appliedProfile: [Float]?
    let gate: AVAudioUnitEffect
    let eq: AVAudioUnitEQ
    let compressor: AVAudioUnitEffect
    /// Bloco próprio (C++): drive (saturador) + amplificador neural NAM + chorus/flanger/phaser/tremolo.
    /// O drive antigo (AVAudioUnitDistortion, presets x²/x³) encurtava a nota; o novo comprime e sustenta.
    let guitar: AVAudioUnitEffect
    let delay: AVAudioUnitDelay
    let reverb: AVAudioUnitReverb

    init(liveNoise: Bool = false) {
        self.liveNoise = liveNoise
        PestanaNoiseAU.registerIfNeeded()
        noise = AVAudioUnitEffect(audioComponentDescription: PestanaNoiseAU.pestanaComponentDescription())
        noise.bypass = true
        gate = AVAudioUnitEffect(audioComponentDescription: AppleAudioUnits.dynamicsDescription)
        eq = AVAudioUnitEQ(numberOfBands: 6)
        compressor = AVAudioUnitEffect(audioComponentDescription: AppleAudioUnits.dynamicsDescription)
        PestanaGuitarAU.registerIfNeeded()
        guitar = AVAudioUnitEffect(audioComponentDescription: PestanaGuitarAU.pestanaComponentDescription())
        guitar.bypass = true
        delay = AVAudioUnitDelay()
        reverb = AVAudioUnitReverb()

        // Preset carregado uma vez; depois só ajustamos o mix (loadFactoryPreset zera os ajustes).
        reverb.loadFactoryPreset(.mediumRoom)

        // Bandas extras (v1.4): corte de graves, "tirar embolado" e presença.
        let lowCut = eq.bands[3]
        lowCut.filterType = .highPass
        lowCut.frequency = 80
        lowCut.bypass = true
        let muddy = eq.bands[4]
        muddy.filterType = .parametric
        muddy.frequency = 300
        muddy.bandwidth = 1.2
        muddy.gain = 0
        muddy.bypass = false
        let presence = eq.bands[5]
        presence.filterType = .parametric
        presence.frequency = 4000
        presence.bandwidth = 1.4
        presence.gain = 0
        presence.bypass = false

        let low = eq.bands[0]
        low.filterType = .lowShelf
        low.frequency = 100
        low.gain = 0
        low.bypass = false

        let mid = eq.bands[1]
        mid.filterType = .parametric
        mid.frequency = 1000
        mid.bandwidth = 1.0     // oitavas
        mid.gain = 0
        mid.bypass = false

        let high = eq.bands[2]
        high.filterType = .highShelf
        high.frequency = 8000
        high.gain = 0
        high.bypass = false
    }

    var nodes: [AVAudioNode] { [noise, gate, eq, compressor, guitar, delay, reverb] }

    private var guitarUnit: PestanaGuitarAU? { guitar.auAudioUnit as? PestanaGuitarAU }
    /// Modelo .nam carregado agora (para não reler o arquivo a cada ajuste).
    private var loadedAmp: String?

    /// Anexa e conecta source → efeitos → destination, todos no mesmo formato.
    func install(in engine: AVAudioEngine, from source: AVAudioNode, to destination: AVAudioNode,
                 format: AVAudioFormat) {
        let chain = nodes
        for node in chain { engine.attach(node) }
        var previous: AVAudioNode = source
        for node in chain {
            engine.connect(previous, to: node, format: format)
            previous = node
        }
        engine.connect(previous, to: destination, format: format)
    }

    /// Aplica as configurações ao vivo. `extraGainDB` (>0 quando volume > 1) vai no ganho global do EQ.
    func apply(_ s: EffectSettings, extraGainDB: Float = 0) {
        applyNoise(s)
        applyGate(s)
        applyEQ(s, extraGainDB: extraGainDB)
        applyCompressor(s)
        applyGuitar(s)
        applyDelay(s)
        reverb.wetDryMix = Self.clamp(s.reverbMix, 0, 1) * 100
        reverb.bypass = s.reverbMix <= 0.001
    }

    private var noiseUnit: PestanaNoiseAU? { noise.auAudioUnit as? PestanaNoiseAU }
    private var learningNoise = false

    private func applyNoise(_ s: EffectSettings) {
        guard let unit = noiseUnit else { noise.bypass = true; return }
        unit.setAmount(Self.clamp(s.noiseAmount, 0, 1))
        if s.noiseProfile != appliedProfile {
            appliedProfile = s.noiseProfile
            unit.setProfile(s.noiseProfile?.map { NSNumber(value: $0) })
        }
        if learningNoise { return }
        noise.bypass = !(liveNoise && s.noiseEnabled && s.noiseProfile != nil)
    }

    /// Atraso (amostras) que a redução de ruído acrescenta agora (0 quando desligada).
    var noiseLatencySamples: Int {
        guard !noise.bypass, let unit = noiseUnit else { return 0 }
        return unit.latencySamples
    }

    /// Aprende o ruído com `seconds` de silêncio e devolve o perfil (na thread principal).
    func learnNoise(seconds: Double = 1.5, completion: @escaping ([Float]?, Double) -> Void) {
        guard let unit = noiseUnit else { completion(nil, 0); return }
        learningNoise = true
        noise.bypass = false
        unit.startLearningSeconds(seconds)
        let started = Date()
        func poll() {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak self] in
                guard let self else { return }
                if unit.isLearning && Date().timeIntervalSince(started) < seconds + 3 { poll(); return }
                self.learningNoise = false
                let profile: [Float]? = unit.isLearning ? nil : unit.currentProfile()?.map { $0.floatValue }
                completion(profile, unit.sampleRate)
            }
        }
        poll()
    }

    private func applyGate(_ s: EffectSettings) {
        // Gate = expansor abaixo do limiar; compressão desligada (threshold no máximo).
        // Razão e soltura suaves: a cauda da nota desce aos poucos em vez de ser cortada
        // (com drive, um corte seco fica muito audível).
        let threshold = Self.clamp(s.gateThresholdDB, -80, -20)
        AppleAudioUnits.set(gate, AppleAudioUnits.Dynamics.threshold, 20)
        AppleAudioUnits.set(gate, AppleAudioUnits.Dynamics.headRoom, 5)
        AppleAudioUnits.set(gate, AppleAudioUnits.Dynamics.expansionThreshold, threshold)
        AppleAudioUnits.set(gate, AppleAudioUnits.Dynamics.expansionRatio, 4)
        AppleAudioUnits.set(gate, AppleAudioUnits.Dynamics.attackTime, 0.002)
        AppleAudioUnits.set(gate, AppleAudioUnits.Dynamics.releaseTime, 0.35)
        AppleAudioUnits.set(gate, AppleAudioUnits.Dynamics.overallGain, 0)
        gate.bypass = !s.gateEnabled
    }

    private func applyEQ(_ s: EffectSettings, extraGainDB: Float) {
        let lowGain = Self.clamp(s.eqLowDB, -12, 12)
        let midGain = Self.clamp(s.eqMidDB, -12, 12)
        let highGain = Self.clamp(s.eqHighDB, -12, 12)
        let global = Self.clamp(extraGainDB, -24, 12)
        eq.bands[0].gain = lowGain
        eq.bands[1].gain = midGain
        eq.bands[2].gain = highGain
        let muddyGain = Self.clamp(s.eqMuddyDB, -12, 0)
        let presenceGain = Self.clamp(s.eqPresenceDB, -6, 8)
        eq.bands[3].bypass = !s.eqLowCut
        eq.bands[4].gain = muddyGain
        eq.bands[5].gain = presenceGain
        eq.globalGain = global
        let flat = abs(lowGain) < 0.05 && abs(midGain) < 0.05 && abs(highGain) < 0.05 && abs(global) < 0.05
            && !s.eqLowCut && abs(muddyGain) < 0.05 && abs(presenceGain) < 0.05
        eq.bypass = flat
    }

    private func applyCompressor(_ s: EffectSettings) {
        // amount 0…1 → threshold -10…-35 dB, headroom 6…1.5 dB (mais agressivo), ganho de compensação 2…10 dB.
        let a = Self.clamp(s.compressorAmount, 0, 1)
        AppleAudioUnits.set(compressor, AppleAudioUnits.Dynamics.threshold, -10 - 25 * a)
        AppleAudioUnits.set(compressor, AppleAudioUnits.Dynamics.headRoom, max(1.5, 6 - 4.5 * a))
        AppleAudioUnits.set(compressor, AppleAudioUnits.Dynamics.expansionRatio, 1)
        AppleAudioUnits.set(compressor, AppleAudioUnits.Dynamics.attackTime, 0.004)
        AppleAudioUnits.set(compressor, AppleAudioUnits.Dynamics.releaseTime, 0.15)
        AppleAudioUnits.set(compressor, AppleAudioUnits.Dynamics.overallGain, 2 + 8 * a)
        compressor.bypass = !s.compressorEnabled
    }

    private func applyGuitar(_ s: EffectSettings) {
        guard let unit = guitarUnit else {
            guitar.bypass = true
            return
        }
        if s.ampModel != loadedAmp {
            loadedAmp = s.ampModel
            if let name = s.ampModel, let url = AmpLibrary.url(for: name) {
                var error: NSString?
                if !unit.loadAmpModel(atPath: url.path, error: &error) { unit.clearAmpModel() }
            } else {
                unit.clearAmpModel()
            }
        }
        unit.setAmpInputDB(Self.clamp(s.ampInputDB, -12, 12), outputDB: Self.clamp(s.ampOutputDB, -12, 12))
        unit.setDriveStyle(s.driveStyle.rawValue, amount: Self.clamp(s.driveAmount, 0, 1))
        unit.setModulationType(s.modType.rawValue, rate: s.modRate, depth: s.modDepth, mix: s.modMix)
        guitar.bypass = !(unit.hasDrive || unit.hasAmpModel || s.modType != .none)
    }

    private func applyDelay(_ s: EffectSettings) {
        delay.delayTime = TimeInterval(Self.clamp(s.delayTime, 0.01, 2))
        delay.feedback = Self.clamp(s.delayFeedback, 0, 0.9) * 100
        delay.lowPassCutoff = 6000
        delay.wetDryMix = Self.clamp(s.delayMix, 0, 1) * 100
        delay.bypass = s.delayMix <= 0.001
    }

    static func clamp(_ v: Float, _ lo: Float, _ hi: Float) -> Float { min(hi, max(lo, v)) }
}
