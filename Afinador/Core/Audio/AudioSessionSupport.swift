import AVFoundation

/// Ganho analógico da entrada (o pré-amplificador do aparelho), quando o iOS deixa mexer.
/// É o único jeito de evitar que a entrada estoure ANTES de chegar ao app.
enum HardwareInputGain {
    static var isSettable: Bool { AVAudioSession.sharedInstance().isInputGainSettable }
    static var value: Float { AVAudioSession.sharedInstance().inputGain }
    static func set(_ value: Float) {
        let s = AVAudioSession.sharedInstance()
        guard s.isInputGainSettable else { return }
        try? s.setInputGain(min(1, max(0, value)))
    }
}

/// Pico bruto (0…1) de um buffer, sem escala.
func rawPeak(of buffer: AVAudioPCMBuffer) -> Float {
    guard let data = buffer.floatChannelData else { return 0 }
    let frames = Int(buffer.frameLength)
    let channels = buffer.format.isInterleaved ? 1 : Int(buffer.format.channelCount)
    let count = buffer.format.isInterleaved ? frames * Int(buffer.format.channelCount) : frames
    var peak: Float = 0
    for c in 0..<channels {
        let p = data[c]
        for i in 0..<count { peak = max(peak, abs(p[i])) }
    }
    return peak
}

enum AudioSession {
    /// Opções da sessão em todo o app. `mixWithOthers`: um app de cifra (ou vídeo) tocando som
    /// não interrompe a gravação nem o Treino/Loop em segundo plano.
    static let options: AVAudioSession.CategoryOptions = [.defaultToSpeaker, .allowBluetoothA2DP, .mixWithOthers]

    static func activate() {
        let s = AVAudioSession.sharedInstance()
        if s.category != .playAndRecord || s.categoryOptions != options {
            try? s.setCategory(.playAndRecord, mode: .default, options: options)
        }
        try? s.setActive(true)
    }
}
