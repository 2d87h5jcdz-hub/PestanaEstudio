import AudioToolbox
import AVFoundation
import Foundation

// MARK: - Audio Units da Apple

enum AppleAudioUnits {
    /// IDs de parâmetro do AUDynamicsProcessor (AudioUnitParameters.h).
    /// Usamos os valores literais para não depender de nomes que mudaram entre SDKs
    /// (kDynamicsProcessorParam_MasterGain → _OverallGain).
    enum Dynamics {
        static let threshold: AudioUnitParameterID = 0           // -40 … 20 dB
        static let headRoom: AudioUnitParameterID = 1            // 0.1 … 40 dB
        static let expansionRatio: AudioUnitParameterID = 2      // 1 … 50
        static let expansionThreshold: AudioUnitParameterID = 3  // dB
        static let attackTime: AudioUnitParameterID = 4          // 0.0001 … 0.2 s
        static let releaseTime: AudioUnitParameterID = 5         // 0.01 … 3 s
        static let overallGain: AudioUnitParameterID = 6         // -40 … 40 dB
    }

    static var dynamicsDescription: AudioComponentDescription {
        AudioComponentDescription(componentType: kAudioUnitType_Effect,
                                  componentSubType: kAudioUnitSubType_DynamicsProcessor,
                                  componentManufacturer: kAudioUnitManufacturer_Apple,
                                  componentFlags: 0,
                                  componentFlagsMask: 0)
    }

    static var peakLimiterDescription: AudioComponentDescription {
        AudioComponentDescription(componentType: kAudioUnitType_Effect,
                                  componentSubType: kAudioUnitSubType_PeakLimiter,
                                  componentManufacturer: kAudioUnitManufacturer_Apple,
                                  componentFlags: 0,
                                  componentFlagsMask: 0)
    }

    /// Limitador no barramento master: evita clipping quando várias faixas somam (ao vivo e na exportação).
    static func makeLimiter() -> AVAudioUnitEffect {
        AVAudioUnitEffect(audioComponentDescription: peakLimiterDescription)
    }

    static func set(_ unit: AVAudioUnit, _ parameter: AudioUnitParameterID, _ value: Float) {
        _ = AudioUnitSetParameter(unit.audioUnit, parameter, kAudioUnitScope_Global, 0, value, 0)
    }
}
