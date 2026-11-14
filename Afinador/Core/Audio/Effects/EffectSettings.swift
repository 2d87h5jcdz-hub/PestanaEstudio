import Foundation

// MARK: - Ajustes da cadeia de efeitos (compartilhados por Estúdio e Loop)
//
// Contrato Codable salvo nos projetos do Estúdio e nos ajustes do Loop: NÃO renomear propriedades
// nem chaves (projetos antigos têm de abrir).

/// Presets prontos de efeito.
enum EffectPreset: String, Codable, CaseIterable, Identifiable {
    case none, steelGuitar, nylonGuitar, vocal, closeMic, electricClean, electricDrive
    // Guitarra (v3): drives com estilos diferentes e modulações.
    case crunch, highGain, fuzz, solo, cleanChorus, flanger80, phaser, tremolo
    case custom
    var id: String { rawValue }

    var title: String {
        switch self {
        case .none: return "Sem efeito"
        case .steelGuitar: return "Violão aço"
        case .nylonGuitar: return "Violão nylon"
        case .vocal: return "Voz"
        case .closeMic: return "Microfone de perto"
        case .electricClean: return "Guitarra limpa"
        case .electricDrive: return "Guitarra drive"
        case .crunch: return "Crunch"
        case .highGain: return "Distorção pesada"
        case .fuzz: return "Fuzz"
        case .solo: return "Solo"
        case .cleanChorus: return "Limpa com chorus"
        case .flanger80: return "Flanger anos 80"
        case .phaser: return "Phaser"
        case .tremolo: return "Tremolo"
        case .custom: return "Personalizado"
        }
    }
}

/// Cadeia de efeitos de uma faixa (e do monitoramento ao vivo).
/// Ordem no motor: redução de ruído → noise gate → EQ (3 bandas) → compressor → guitarra (drive + amp NAM + modulação) → delay → reverb.
struct EffectSettings: Codable, Equatable {
    var preset: EffectPreset = .none
    /// Redução de ruído (ar-condicionado, ventilador, rua ao longe). Fora dos presets.
    /// Ao vivo usa o perfil aprendido (`noiseProfile`, FFT 512 na taxa `noiseProfileRate`); nas
    /// faixas do Estúdio a limpeza é feita na gravação (cópia), com esse perfil ou estimando do silêncio.
    var noiseEnabled = false
    var noiseAmount: Float = 0.5         // 0 … 1
    var noiseProfile: [Float]?
    var noiseProfileRate: Double?
    var gateEnabled = false
    var gateThresholdDB: Float = -50     // -80 … -20
    var eqLowDB: Float = 0               // -12 … +12  (100 Hz, shelf)
    var eqMidDB: Float = 0               // -12 … +12  (1 kHz, bell)
    var eqHighDB: Float = 0              // -12 … +12  (8 kHz, shelf)
    /// Corta abaixo de 80 Hz (ronco, pancadas no pedestal, excesso de grave do microfone de perto).
    var eqLowCut = false
    /// "Tirar embolado": sino em 300 Hz (-12 … 0). Microfone muito perto reforça essa região.
    var eqMuddyDB: Float = 0
    /// Presença: sino em 4 kHz (-6 … +8). Devolve clareza e definição.
    var eqPresenceDB: Float = 0
    var compressorEnabled = false
    var compressorAmount: Float = 0.5    // 0 … 1  (motor mapeia para threshold/ratio)
    var driveAmount: Float = 0           // 0 … 1  (0 = desligado)
    var delayMix: Float = 0              // 0 … 1
    var delayTime: Float = 0.35          // s
    var delayFeedback: Float = 0.3       // 0 … 0.9
    var reverbMix: Float = 0             // 0 … 1
    /// Estilo do drive (saturador do bloco Guitarra, em C++).
    var driveStyle: DriveStyle = .distortion
    /// Bloco Guitarra: amplificador neural (.nam importado, nome do arquivo em Amps/) e ganhos.
    var ampModel: String?
    var ampInputDB: Float = 0            // -12 … +12
    var ampOutputDB: Float = 0           // -12 … +12
    /// Modulação (depois do amp): chorus, flanger, phaser, tremolo.
    var modType: ModulationType = .none
    var modRate: Float = 0.6             // Hz
    var modDepth: Float = 0.5            // 0 … 1
    var modMix: Float = 0.5              // 0 … 1

    static func preset(_ p: EffectPreset) -> EffectSettings {
        var s = EffectSettings()
        s.preset = p
        switch p {
        case .none, .custom: break
        case .steelGuitar:
            s.gateEnabled = true; s.gateThresholdDB = -55
            s.eqLowDB = -2; s.eqMidDB = 1; s.eqHighDB = 3
            s.compressorEnabled = true; s.compressorAmount = 0.4
            s.reverbMix = 0.22
        case .nylonGuitar:
            s.gateEnabled = true; s.gateThresholdDB = -58
            s.eqLowDB = 1; s.eqMidDB = 0; s.eqHighDB = 1.5
            s.compressorEnabled = true; s.compressorAmount = 0.3
            s.reverbMix = 0.25
        case .vocal:
            s.gateEnabled = true; s.gateThresholdDB = -45
            s.eqLowDB = -4; s.eqMidDB = 1.5; s.eqHighDB = 2.5
            s.compressorEnabled = true; s.compressorAmount = 0.6
            s.delayMix = 0.08; s.delayTime = 0.28; s.delayFeedback = 0.2
            s.reverbMix = 0.18
        case .closeMic:
            // Microfone de estúdio a ~10 cm: o efeito de proximidade engorda 100–400 Hz.
            s.eqLowCut = true; s.eqMuddyDB = -6; s.eqPresenceDB = 4; s.eqHighDB = 2
            s.compressorEnabled = true; s.compressorAmount = 0.3
            s.reverbMix = 0.12
        case .electricClean:
            s.eqLowDB = 0; s.eqMidDB = 2; s.eqHighDB = 2
            s.compressorEnabled = true; s.compressorAmount = 0.5
            s.delayMix = 0.15; s.reverbMix = 0.2
        case .electricDrive:
            s.gateEnabled = true; s.gateThresholdDB = -62
            s.eqLowDB = 2; s.eqMidDB = 3; s.eqHighDB = 1
            s.driveAmount = 0.6; s.reverbMix = 0.15
        case .crunch:
            s.gateEnabled = true; s.gateThresholdDB = -62
            s.eqLowDB = 1; s.eqMidDB = 2; s.eqHighDB = 1.5
            s.driveStyle = .crunch; s.driveAmount = 0.45
            s.reverbMix = 0.15
        case .highGain:
            s.gateEnabled = true; s.gateThresholdDB = -60
            s.eqLowDB = 3; s.eqMidDB = -2; s.eqHighDB = 2
            s.driveStyle = .distortion; s.driveAmount = 0.85
            s.compressorEnabled = true; s.compressorAmount = 0.3
            s.reverbMix = 0.12
        case .fuzz:
            s.gateEnabled = true; s.gateThresholdDB = -60
            s.eqLowDB = 2; s.eqMidDB = 1; s.eqHighDB = -1
            s.driveStyle = .fuzz; s.driveAmount = 0.7
            s.reverbMix = 0.1
        case .solo:
            s.gateEnabled = true; s.gateThresholdDB = -62
            s.eqLowDB = 0; s.eqMidDB = 3; s.eqHighDB = 1.5
            s.compressorEnabled = true; s.compressorAmount = 0.5
            s.driveStyle = .distortion; s.driveAmount = 0.7
            s.delayMix = 0.25; s.delayTime = 0.38; s.delayFeedback = 0.35
            s.reverbMix = 0.25
        case .cleanChorus:
            s.eqLowDB = -1; s.eqMidDB = 0; s.eqHighDB = 2
            s.compressorEnabled = true; s.compressorAmount = 0.4
            s.modType = .chorus; s.modRate = 0.8; s.modDepth = 0.55; s.modMix = 0.5
            s.reverbMix = 0.2
        case .flanger80:
            s.eqLowDB = 0; s.eqMidDB = 1; s.eqHighDB = 2
            s.driveStyle = .crunch; s.driveAmount = 0.3
            s.modType = .flanger; s.modRate = 0.25; s.modDepth = 0.8; s.modMix = 0.6
            s.reverbMix = 0.15
        case .phaser:
            s.eqLowDB = 0; s.eqMidDB = 1; s.eqHighDB = 1
            s.driveStyle = .crunch; s.driveAmount = 0.25
            s.modType = .phaser; s.modRate = 0.5; s.modDepth = 0.7; s.modMix = 0.6
            s.reverbMix = 0.15
        case .tremolo:
            s.eqLowDB = 0; s.eqMidDB = 0; s.eqHighDB = 1.5
            s.modType = .tremolo; s.modRate = 5; s.modDepth = 0.7; s.modMix = 1
            s.reverbMix = 0.25
        }
        return s
    }
}

/// Estilo do drive (curva de saturação no GuitarFXCore; mesma numeração do DriveType em C++).
enum DriveStyle: Int, Codable, CaseIterable, Identifiable {
    case distortion = 0, crunch, funk, fuzz
    var id: Int { rawValue }
    var title: String {
        switch self {
        case .distortion: return "Distorção"
        case .crunch: return "Crunch"
        case .funk: return "Funk"
        case .fuzz: return "Fuzz"
        }
    }
}

/// Modulação do bloco Guitarra (o número vai para o Audio Unit em C++).
enum ModulationType: Int, Codable, CaseIterable, Identifiable {
    case none = 0, chorus, flanger, phaser, tremolo
    var id: Int { rawValue }
    var title: String {
        switch self {
        case .none: return "Nenhuma"
        case .chorus: return "Chorus"
        case .flanger: return "Flanger"
        case .phaser: return "Phaser"
        case .tremolo: return "Tremolo"
        }
    }
}

extension EffectSettings {
    private enum CodingKeys: String, CodingKey {
        case preset, gateEnabled, gateThresholdDB, eqLowDB, eqMidDB, eqHighDB
        case compressorEnabled, compressorAmount, driveAmount, delayMix, delayTime, delayFeedback, reverbMix
        case driveStyle, ampModel, ampInputDB, ampOutputDB, modType, modRate, modDepth, modMix
        case noiseEnabled, noiseAmount, noiseProfile, noiseProfileRate
        case eqLowCut, eqMuddyDB, eqPresenceDB
    }

    /// Limiar do gate dos presets com drive até a v1.2.
    private static let oldDriveGate: [EffectPreset: Float] = [
        .electricDrive: -48, .crunch: -52, .highGain: -45, .fuzz: -48, .solo: -48
    ]

    /// Lê projetos antigos: campos que não existiam ficam no padrão.
    init(from decoder: Decoder) throws {
        self.init()
        let c = try decoder.container(keyedBy: CodingKeys.self)
        preset = (try? c.decodeIfPresent(EffectPreset.self, forKey: .preset)) ?? .custom
        gateEnabled = try c.decodeIfPresent(Bool.self, forKey: .gateEnabled) ?? gateEnabled
        gateThresholdDB = try c.decodeIfPresent(Float.self, forKey: .gateThresholdDB) ?? gateThresholdDB
        eqLowDB = try c.decodeIfPresent(Float.self, forKey: .eqLowDB) ?? eqLowDB
        eqMidDB = try c.decodeIfPresent(Float.self, forKey: .eqMidDB) ?? eqMidDB
        eqHighDB = try c.decodeIfPresent(Float.self, forKey: .eqHighDB) ?? eqHighDB
        compressorEnabled = try c.decodeIfPresent(Bool.self, forKey: .compressorEnabled) ?? compressorEnabled
        compressorAmount = try c.decodeIfPresent(Float.self, forKey: .compressorAmount) ?? compressorAmount
        driveAmount = try c.decodeIfPresent(Float.self, forKey: .driveAmount) ?? driveAmount
        delayMix = try c.decodeIfPresent(Float.self, forKey: .delayMix) ?? delayMix
        delayTime = try c.decodeIfPresent(Float.self, forKey: .delayTime) ?? delayTime
        delayFeedback = try c.decodeIfPresent(Float.self, forKey: .delayFeedback) ?? delayFeedback
        reverbMix = try c.decodeIfPresent(Float.self, forKey: .reverbMix) ?? reverbMix
        driveStyle = (try? c.decodeIfPresent(DriveStyle.self, forKey: .driveStyle)) ?? driveStyle
        // v1.3: os presets com drive tinham o gate alto demais (cortava a cauda da nota).
        // Se o valor salvo ainda é o antigo do preset, passa para o novo.
        if let old = Self.oldDriveGate[preset], abs(gateThresholdDB - old) < 0.01 {
            gateThresholdDB = Self.preset(preset).gateThresholdDB
        }
        ampModel = try c.decodeIfPresent(String.self, forKey: .ampModel)
        ampInputDB = try c.decodeIfPresent(Float.self, forKey: .ampInputDB) ?? ampInputDB
        ampOutputDB = try c.decodeIfPresent(Float.self, forKey: .ampOutputDB) ?? ampOutputDB
        modType = (try? c.decodeIfPresent(ModulationType.self, forKey: .modType)) ?? modType
        modRate = try c.decodeIfPresent(Float.self, forKey: .modRate) ?? modRate
        modDepth = try c.decodeIfPresent(Float.self, forKey: .modDepth) ?? modDepth
        modMix = try c.decodeIfPresent(Float.self, forKey: .modMix) ?? modMix
        eqLowCut = try c.decodeIfPresent(Bool.self, forKey: .eqLowCut) ?? eqLowCut
        eqMuddyDB = try c.decodeIfPresent(Float.self, forKey: .eqMuddyDB) ?? eqMuddyDB
        eqPresenceDB = try c.decodeIfPresent(Float.self, forKey: .eqPresenceDB) ?? eqPresenceDB
        noiseEnabled = try c.decodeIfPresent(Bool.self, forKey: .noiseEnabled) ?? noiseEnabled
        noiseAmount = try c.decodeIfPresent(Float.self, forKey: .noiseAmount) ?? noiseAmount
        noiseProfile = try? c.decodeIfPresent([Float].self, forKey: .noiseProfile)
        noiseProfileRate = try? c.decodeIfPresent(Double.self, forKey: .noiseProfileRate)
    }

    /// Mantém os ajustes de ruído ao trocar de preset (o ruído é do ambiente, não do timbre).
    mutating func keepNoise(from other: EffectSettings) {
        noiseEnabled = other.noiseEnabled
        noiseAmount = other.noiseAmount
        noiseProfile = other.noiseProfile
        noiseProfileRate = other.noiseProfileRate
    }

    /// Intensidade da limpeza na gravação (nil = desligada).
    var noiseCleanupLevel: Float? {
        guard noiseEnabled, noiseAmount > 0.001 else { return nil }
        return noiseAmount
    }
}
