import AVFoundation
import Combine
import Foundation
import UIKit

// MARK: - StudioEngine
//
// Motor do Estúdio (gravação em camadas). API pública conforme o contrato em StudioModels.swift.
//
// Grafo ao vivo (um AVAudioEngine recriado sempre que o conjunto de faixas ou a rota muda):
//
//   por faixa:  player → conversor(mixer) → gate → EQ → compressor → guitarra (drive + amp + modulação) → delay → reverb → mixer da faixa ─┐
//                                                                                     (volume/pan, tap de nível)│
//   master:     ┌──────────────────────────────────────────────────────────────────────────────────────────────┘
//               └→ masterMixer → limitador → mainMixer → saída
//   clique:     AVAudioSourceNode (mono) → mainMixer
//   monitor:    inputNode → mixer → [cadeia de efeitos com monitorEffects] → mixer (0 ou 1) → mainMixer
//   entrada:    tap no inputNode → medidor + gravação (áudio SECO em .caf PCM float32)
//
// Sessão de áudio: .playAndRecord, modo .default, [.defaultToSpeaker, .allowBluetoothA2DP], buffer ~5 ms.
// Escolhemos .default (e não .measurement) porque é o mesmo modo que AudioSessionService.activate() usa nas
// outras abas — trocar de modo ao gravar reconfigura a rota (corte/estalo) e muda o ganho do microfone
// entre o monitoramento e a tomada. Em .playAndRecord/.default não há processamento de voz (só no .voiceChat).
//
// Sincronia: players, clique e gravação usam o mesmo instante de host (mach_absolute_time) como origem.
// Compensação de latência: o gravador descarta as amostras capturadas antes do instante em que a
// música chega à posição inicial (pelo hostTime do tap, o que já cobre o atraso do buffer de I/O);
// o atraso restante — ida até o alto-falante/fone (outputLatency) e volta pelo conversor de entrada
// (inputLatency) — vira offset negativo do clip novo: offset = posiçãoInicial − (inputLatency + outputLatency).
// Se isso der < 0 (gravação a partir do 0), o começo é aparado: offset = 0 e fileStart = −offset.
//
// v2 (linha do tempo com clips): cada faixa tem `clips`. Reprodução/exportação agendam todos os clips
// (StudioTrackChain/StudioClipLane); a gravação entra na faixa ARMADA a partir da cabeça de reprodução
// e substitui o que houver por baixo (punch-in, StudioTimeline.punchIn). Os campos legados da faixa
// (fileName/offset/duration) são mantidos como espelho dos clips (studioSyncLegacyFields).
//
// Organização (arquivos em Features/Studio/Engine):
//   StudioEngine.swift              estado publicado, init, projetos
//   StudioEngine+Transport.swift    tocar/parar/gravar/buscar, agendamento, tomada, timer
//   StudioEngine+Tracks.swift       faixas, clips, importação, exportação
//   StudioEngine+Session.swift      ciclo de vida, sessão de áudio, entrada, rota, interrupções
//   StudioEngine+Graph.swift        grafo do AVAudioEngine, monitoramento, ruído do monitor
//   StudioEngine+Persistence.swift  observação do projeto e gravação em disco
//   StudioEngine+Pitch.swift        ligação com StudioPitchJobs (afinação automática / ruído)
// Serviços injetados: AudioSessionManaging (sessão), ProjectStore (disco). O estado abaixo não é
// `private` porque as extensões em outros arquivos o usam; só o motor deve escrevê-lo.

@MainActor
final class StudioEngine: ObservableObject {

    // MARK: Estado publicado (contrato)

    @Published internal(set) var projects: [StudioProject] = []
    @Published var project: StudioProject? = nil {
        didSet { projectDidChange(from: oldValue) }
    }
    @Published internal(set) var transport: TransportState = .stopped
    @Published internal(set) var position: TimeInterval = 0
    @Published internal(set) var inputLevel: Float = 0
    @Published internal(set) var trackLevels: [UUID: Float] = [:]
    @Published var clickEnabled: Bool = true {
        didSet {
            click?.setClickOn(clickEnabled)
            UserDefaults.standard.set(clickEnabled, forKey: Keys.click)
        }
    }
    @Published var countInBars: Int = 1 {
        didSet {
            let clamped = min(2, max(0, countInBars))
            if clamped != countInBars { countInBars = clamped }
            UserDefaults.standard.set(clamped, forKey: Keys.countIn)
        }
    }
    @Published var monitorEnabled: Bool = false {
        didSet { monitorDidChange() }
    }
    /// Aprendendo o ruído do ambiente pela entrada (redução de ruído do monitoramento).
    @Published internal(set) var learningNoise = false
    @Published var monitorEffects: EffectSettings = .preset(.none) {
        didSet { monitorChain?.apply(monitorEffects) }
    }
    @Published internal(set) var inputName: String = "Microfone do iPhone"
    @Published internal(set) var isHeadphoneConnected: Bool = false
    /// Entradas disponíveis agora (microfone do iPhone, microfone do fone, interface USB…).
    @Published internal(set) var availableInputs: [StudioInputOption] = []
    /// uid da entrada em uso.
    @Published internal(set) var selectedInputID: String?
    /// Ganho digital da entrada, em dB (0…+24). Vale para o monitoramento, o medidor e a gravação.
    /// A entrada está estourando antes do app (fica true por ~2 s depois do último corte).
    @Published internal(set) var inputClipping = false
    var clipUntil = Date.distantPast
    /// Ganho do aparelho (0…1), quando a entrada deixa ajustar.
    @Published internal(set) var hardwareGainSettable = false
    @Published var hardwareGain: Float = 0.5 {
        didSet { if !syncingHardwareGain { session.setInputGain(hardwareGain) } }
    }
    var syncingHardwareGain = false

    @Published var inputGainDB: Float = 0 {
        didSet {
            let clamped = min(24, max(0, inputGainDB))
            if clamped != inputGainDB { inputGainDB = clamped; return }
            inputGain.setDecibels(clamped)
            inputBoost?.globalGain = clamped
            UserDefaults.standard.set(clamped, forKey: Keys.inputGain)
        }
    }
    @Published internal(set) var lastError: String?

    /// Maior faixa do projeto (fim do último clip).
    var duration: TimeInterval {
        guard let project else { return 0 }
        var longest: TimeInterval = 0
        for track in project.tracks {
            longest = max(longest, track.end)
        }
        return max(0, longest)
    }

    // MARK: v2: linha do tempo / clips (contrato)

    /// Faixa que recebe a próxima gravação. nil = record() cria uma faixa nova e a arma.
    /// Só em memória: zera ao trocar/fechar o projeto e quando a faixa armada é apagada.
    @Published var armedTrackID: UUID? = nil {
        didSet {
            guard armedTrackID != oldValue, let id = armedTrackID else { return }
            if project?.tracks.contains(where: { $0.id == id }) != true { armedTrackID = nil }
        }
    }
    /// Começo da gravação em andamento na linha do tempo (s) = cabeça de reprodução quando record()
    /// começou (sem a compensação de latência). Não-nil desde record() (inclui a contagem) até o fim
    /// da tomada; o fim do clip "ao vivo" é `position`.
    @Published internal(set) var recordingStart: TimeInterval? = nil

    // MARK: Extras (além do contrato)

    /// true enquanto exportMix está renderizando.
    @Published internal(set) var isExporting = false
    /// Afinação automática em andamento: faixa → progresso 0…1.
    @Published internal(set) var pitchProgress: [UUID: Double] = [:]

    /// Latência que será compensada na próxima gravação (s).
    var latencyCompensation: TimeInterval {
        session.inputOutputLatency
    }

    /// URL do arquivo de áudio de uma faixa do projeto aberto (v2: o do primeiro clip).
    func fileURL(for track: StudioTrack) -> URL? {
        guard let project else { return nil }
        let name = track.clips.first?.fileName ?? track.fileName
        guard !name.isEmpty else { return nil }
        return store.folder(for: project.id).appendingPathComponent(name)
    }

    /// URL do arquivo de áudio de um clip do projeto aberto.
    func fileURL(for clip: StudioClip) -> URL? {
        guard let project, !clip.fileName.isEmpty else { return nil }
        return store.folder(for: project.id).appendingPathComponent(clip.fileName)
    }

    func clearError() { lastError = nil }

    // MARK: Estado interno

    enum Keys {
        static let click = "studio.clickEnabled"
        static let countIn = "studio.countInBars"
        static let inputGain = "studio.inputGainDB"
        // A entrada preferida ("studio.preferredInputType") é guardada pelo AudioSessionService.
    }

    static let feedbackWarning =
        "Sem fone de ouvido: ouvir a entrada pelo alto-falante pode causar microfonia. Use fone para monitorar."

    struct PendingTake {
        let fileName: String
        let url: URL
        let startPosition: TimeInterval
        let latency: TimeInterval
        let sampleRate: Double
        /// Faixa que recebe a tomada.
        let trackID: UUID
        /// true se record() criou a faixa (sem nada armado): sai se a tomada não for salva.
        let createdTrack: Bool
    }

    var engine: AVAudioEngine?
    var chains: [UUID: StudioTrackChain] = [:]
    var masterMixer: AVAudioMixerNode?
    var monitorChain: EffectChain?
    var monitorOut: AVAudioMixerNode?
    var click: StudioClickGenerator?
    var inputTapInstalled = false

    // Serviços
    let session: AudioSessionManaging
    let store: ProjectStore

    let meters = StudioLevelMeters()
    let recorder = StudioTakeRecorder()
    let inputGain = StudioInputGain()
    var inputBoost: AVAudioUnitEQ?

    var isActive = false
    var micGranted = false
    var recordAfterPermission = false
    var previousIOBuffer: TimeInterval?

    var playStartHost: UInt64 = 0
    var playStartPosition: TimeInterval = 0
    var leadIn: TimeInterval = 0
    var take: PendingTake?
    var needsRebuild = false

    var suppressObservation = false
    var saveTask: Task<Void, Never>?
    var rebuildTask: Task<Void, Never>?
    /// Afinação automática / limpeza de ruído das faixas (tarefas de fundo).
    let pitchJobs: StudioPitchJobs
    var timerCancellable: AnyCancellable?
    var engineConfigCancellable: AnyCancellable?
    var sessionCancellables = Set<AnyCancellable>()

    // MARK: Init

    init(session: AudioSessionManaging = AudioSessionService.shared,
         store: ProjectStore = DiskProjectStore()) {
        self.session = session
        self.store = store
        self.pitchJobs = StudioPitchJobs()
        let defaults = UserDefaults.standard
        if defaults.object(forKey: Keys.click) != nil {
            clickEnabled = defaults.bool(forKey: Keys.click)
        }
        if defaults.object(forKey: Keys.countIn) != nil {
            countInBars = min(2, max(0, defaults.integer(forKey: Keys.countIn)))
        }
        // No init os didSet não rodam: aplica o ganho salvo à mão.
        let savedGain = min(24, max(0, defaults.float(forKey: Keys.inputGain)))
        inputGainDB = savedGain
        inputGain.setDecibels(savedGain)
        projects = store.loadAll()
        pitchJobs.onProgressChange = { [weak self] value in self?.pitchProgress = value }
        pitchJobs.onFinished = { [weak self] trackID, jobKey, outcome in
            self?.pitchJobFinished(trackID: trackID, jobKey: jobKey, outcome: outcome)
        }
        observeSession()
        updateRouteInfo()
    }

    // MARK: - Projetos

    func newProject(name: String) -> StudioProject {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        let project = StudioProject(name: trimmed.isEmpty ? "Música \(projects.count + 1)" : trimmed)
        stopForProjectSwitch()
        writeProject(project)
        self.project = project      // didSet: troca de projeto → reconstrói o grafo
        return project
    }

    func open(_ id: UUID) {
        guard project?.id != id else { return }
        guard let target = projects.first(where: { $0.id == id }) else {
            lastError = "Projeto não encontrado."
            return
        }
        stopForProjectSwitch()
        project = target
    }

    func rename(_ id: UUID, to name: String) {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        if project?.id == id {
            project?.name = trimmed     // didSet salva
            return
        }
        guard var target = projects.first(where: { $0.id == id }) else { return }
        target.name = trimmed
        target.updatedAt = Date()
        writeProject(target)
    }

    func deleteProject(_ id: UUID) {
        if project?.id == id {
            if transport != .stopped { stop() }
            saveTask?.cancel()
            saveTask = nil
            project = nil
        }
        do { try store.deleteProject(id) } catch {
            lastError = "Não foi possível apagar os arquivos do projeto."
        }
        projects.removeAll { $0.id == id }
    }

    func closeProject() {
        guard project != nil else { return }
        stopForProjectSwitch()
        project = nil
    }

    @discardableResult
    func fail(_ error: StudioEngineError) -> StudioEngineError {
        lastError = error.errorDescription
        return error
    }
}
