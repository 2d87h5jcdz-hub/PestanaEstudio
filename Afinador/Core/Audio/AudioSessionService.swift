import AVFoundation
import Foundation

// MARK: - Sessão de áudio do app (um lugar só)
//
// Todas as abas usam .playAndRecord no modo .default com as mesmas opções: trocar de categoria ou de
// modo entre as abas reconfigura a rota (corte/estalo) e muda o ganho do microfone. O Estúdio e o
// Loop pedem buffer de 5 ms (baixa latência) e devolvem o valor anterior ao sair.
//
// Sem estado próprio: o buffer anterior fica com quem pediu (inout), para cada motor restaurar o seu.
// AVAudioSession é segura entre threads: este serviço não é @MainActor (o Afinador, o Metrônomo e os
// Acordes chamam de fora do MainActor).

/// A escolha de entrada salva não pôde ser aplicada.
struct AudioInputSelectionError: Error {
    /// Nome amigável da entrada (ex.: "Microfone do iPhone").
    let inputTitle: String
}

protocol AudioSessionManaging: AnyObject {
    /// Opções da sessão em todo o app. `mixWithOthers`: um app de cifra (ou vídeo) tocando som
    /// não interrompe a gravação nem o Treino/Loop em segundo plano.
    var options: AVAudioSession.CategoryOptions { get }

    /// Garante a categoria do app e ativa a sessão (erros ignorados).
    func activate()
    /// O mesmo que activate(), mas devolve o erro.
    func activateOrThrow() throws
    /// Categoria do app + buffer de 5 ms + ativa. Guarda em `previousBuffer` o buffer preferido de
    /// antes (só se ainda não houver um guardado).
    func configureLowLatency(previousBuffer: inout TimeInterval?) throws
    /// Volta ao buffer guardado por configureLowLatency (0 = nunca definido → 23 ms).
    func restoreIOBuffer(_ previousBuffer: inout TimeInterval?)

    /// Aplica a entrada preferida: a escolha salva; senão interface USB; senão o microfone do iPhone
    /// (com fone plugado, o iOS trocaria para o microfone do fone, longe do instrumento).
    func applyPreferredInput() throws
    /// Troca a entrada e salva a escolha pelo tipo de entrada. false se a entrada não existe mais.
    func selectInput(uid: String) throws -> Bool

    var availableInputs: [AVAudioSessionPortDescription] { get }
    var currentInput: AVAudioSessionPortDescription? { get }
    /// Saída por fone/interface (sem risco de microfonia pelo alto-falante).
    var isHeadphoneConnected: Bool { get }
    /// Nome amigável de uma entrada. `markBluetooth`: acrescenta "(Bluetooth)" no microfone Bluetooth.
    func inputTitle(_ port: AVAudioSessionPortDescription, markBluetooth: Bool) -> String

    /// Latência de entrada + saída (compensada nas gravações do Estúdio).
    var inputOutputLatency: TimeInterval { get }
    /// Ida e volta completa: entrada + saída + 2 buffers de I/O (Loop).
    var roundTripLatency: TimeInterval { get }
    var sampleRate: Double { get }

    /// Ganho analógico da entrada (pré do aparelho), quando o iOS deixa mexer.
    var isInputGainSettable: Bool { get }
    var inputGain: Float { get }
    func setInputGain(_ value: Float)
}

final class AudioSessionService: AudioSessionManaging, @unchecked Sendable {
    static let shared = AudioSessionService()

    /// Mesma chave de sempre (a escolha vale para o Estúdio e o Loop).
    static let preferredInputKey = "studio.preferredInputType"
    static let lowLatencyBuffer: TimeInterval = 0.005

    let options: AVAudioSession.CategoryOptions = [.defaultToSpeaker, .allowBluetoothA2DP, .mixWithOthers]

    private var session: AVAudioSession { AVAudioSession.sharedInstance() }

    func activate() {
        let s = session
        if s.category != .playAndRecord || s.categoryOptions != options {
            try? s.setCategory(.playAndRecord, mode: .default, options: options)
        }
        try? s.setActive(true)
    }

    func activateOrThrow() throws {
        let s = session
        if s.category != .playAndRecord || s.categoryOptions != options {
            try s.setCategory(.playAndRecord, mode: .default, options: options)
        }
        try s.setActive(true)
    }

    func configureLowLatency(previousBuffer: inout TimeInterval?) throws {
        let s = session
        if previousBuffer == nil { previousBuffer = s.preferredIOBufferDuration }
        try s.setCategory(.playAndRecord, mode: .default, options: options)
        try s.setPreferredIOBufferDuration(Self.lowLatencyBuffer)
        try s.setActive(true)
    }

    func restoreIOBuffer(_ previousBuffer: inout TimeInterval?) {
        guard let previous = previousBuffer else { return }
        previousBuffer = nil
        // 0 = nunca definido; volta a um valor padrão razoável para as outras abas.
        try? session.setPreferredIOBufferDuration(previous > 0 ? previous : 0.023)
    }

    func applyPreferredInput() throws {
        let s = session
        let inputs = s.availableInputs ?? []
        guard !inputs.isEmpty else { return }
        let saved = UserDefaults.standard.string(forKey: Self.preferredInputKey)
        let target = inputs.first { $0.portType.rawValue == saved }
            ?? inputs.first { $0.portType == .usbAudio }
            ?? inputs.first { $0.portType == .builtInMic }
        guard let target else { return }
        if s.currentRoute.inputs.first?.uid == target.uid { return }
        do {
            try s.setPreferredInput(target)
        } catch {
            throw AudioInputSelectionError(inputTitle: inputTitle(target, markBluetooth: true))
        }
    }

    func selectInput(uid: String) throws -> Bool {
        let s = session
        guard let port = (s.availableInputs ?? []).first(where: { $0.uid == uid }) else { return false }
        UserDefaults.standard.set(port.portType.rawValue, forKey: Self.preferredInputKey)
        do {
            try s.setPreferredInput(port)
        } catch {
            throw AudioInputSelectionError(inputTitle: inputTitle(port, markBluetooth: true))
        }
        return true
    }

    var availableInputs: [AVAudioSessionPortDescription] { session.availableInputs ?? [] }

    var currentInput: AVAudioSessionPortDescription? { session.currentRoute.inputs.first }

    var isHeadphoneConnected: Bool {
        let safePorts: Set<AVAudioSession.Port> = [
            .headphones, .bluetoothA2DP, .bluetoothLE, .bluetoothHFP, .usbAudio, .lineOut,
        ]
        return session.currentRoute.outputs.contains { safePorts.contains($0.portType) }
    }

    func inputTitle(_ port: AVAudioSessionPortDescription, markBluetooth: Bool) -> String {
        switch port.portType {
        case .builtInMic: return "Microfone do iPhone"
        case .headsetMic: return "Microfone do fone"
        case .bluetoothHFP where markBluetooth: return "\(port.portName) (Bluetooth)"
        default: return port.portName
        }
    }

    var inputOutputLatency: TimeInterval {
        let s = session
        return s.inputLatency + s.outputLatency
    }

    var roundTripLatency: TimeInterval {
        let s = session
        return s.inputLatency + s.outputLatency + 2 * s.ioBufferDuration
    }

    var sampleRate: Double { session.sampleRate }

    var isInputGainSettable: Bool { HardwareInputGain.isSettable }
    var inputGain: Float { HardwareInputGain.value }
    func setInputGain(_ value: Float) { HardwareInputGain.set(value) }
}
