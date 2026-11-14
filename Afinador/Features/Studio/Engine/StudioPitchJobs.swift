import Foundation

// MARK: - Trabalhos de afinação automática / limpeza de ruído (fora da main thread)
//
// Serviço do StudioEngine: decide quais faixas precisam de cópia processada, roda um trabalho por
// faixa numa tarefa de fundo (StudioPitchJob.run) e avisa o progresso e o fim. Não mexe no projeto:
// quem trata o resultado (tom achado, reconstruir o grafo, mensagem de erro) é o StudioEngine.

@MainActor
final class StudioPitchJobs {
    /// Faixa → progresso 0…1 dos trabalhos em andamento.
    private(set) var progress: [UUID: Double] = [:] {
        didSet { onProgressChange?(progress) }
    }
    var onProgressChange: (@MainActor ([UUID: Double]) -> Void)?
    /// (faixa, chave do trabalho, resultado). Só para o trabalho atual da faixa (não os substituídos).
    var onFinished: (@MainActor (UUID, String, StudioPitchJob.Outcome) -> Void)?

    /// Trabalhos por faixa (chave = o que está sendo gerado, para não repetir).
    private var tasks: [UUID: (key: String, task: Task<Void, Never>)] = [:]
    private var refreshTask: Task<Void, Never>?
    /// Trabalhos que falharam (sem voz para achar o tom): não repete até mudar algo.
    private var failed: Set<String> = []

    /// Agenda uma verificação (com espera curta: o controle de intensidade muda muitas vezes seguidas).
    func scheduleRefresh(_ action: @escaping @MainActor () -> Void) {
        refreshTask?.cancel()
        refreshTask = Task { @MainActor in
            try? await Task.sleep(nanoseconds: 700_000_000)
            if Task.isCancelled { return }
            action()
        }
    }

    /// Sem projeto aberto: cancela tudo.
    func cancelAll() {
        for (_, job) in tasks { job.task.cancel() }
        tasks = [:]
        progress = [:]
    }

    func markFailed(_ jobKey: String) {
        failed.insert(jobKey)
    }

    /// Começa (ou cancela) os trabalhos conforme as faixas do projeto aberto.
    func refresh(tracks: [StudioTrack], folder: URL) {
        var wanted: Set<UUID> = []
        for track in tracks where StudioPitchFiles.needsProcessing(track) && !track.clips.isEmpty {
            let sources: [String] = Array(Set(track.clips.map(\.fileName))).sorted()
            let signature: String? = StudioPitchFiles.processSignature(for: track)
            let noiseKey: String = track.effects.noiseCleanupLevel.map { "|n\($0)" } ?? ""
            let jobKey: String = (signature ?? "detect|\(track.pitch.strength)\(noiseKey)") + "|" + sources.joined(separator: ",")
            if signature != nil && StudioPitchFiles.isReady(track: track, folder: folder) {
                continue    // tudo pronto
            }
            if failed.contains(jobKey) { continue }
            wanted.insert(track.id)
            if let running = tasks[track.id], running.key == jobKey { continue }
            tasks[track.id]?.task.cancel()
            start(trackID: track.id, sources: sources, settings: track.pitch,
                  noise: track.effects.noiseCleanupLevel, learned: track.effects.noiseProfile,
                  learnedRate: track.effects.noiseProfileRate, folder: folder, jobKey: jobKey)
        }
        for (id, job) in tasks where !wanted.contains(id) {
            job.task.cancel()
            tasks[id] = nil
            progress[id] = nil
        }
    }

    private func start(trackID: UUID, sources: [String], settings: PitchCorrection,
                       noise: Float?, learned: [Float]?, learnedRate: Double?,
                       folder: URL, jobKey: String) {
        progress[trackID] = 0
        let task = Task.detached(priority: .userInitiated) { [weak self] in
            let outcome = StudioPitchJob.run(
                folder: folder, sources: sources, settings: settings,
                noise: noise, learnedProfile: learned, learnedRate: learnedRate,
                progress: { value in
                    Task { @MainActor [weak self] in
                        guard let self, self.tasks[trackID]?.key == jobKey else { return }
                        self.progress[trackID] = value
                    }
                },
                isCancelled: { Task.isCancelled }
            )
            if Task.isCancelled { return }
            await MainActor.run { [weak self] in
                self?.finished(trackID: trackID, jobKey: jobKey, outcome: outcome)
            }
        }
        tasks[trackID] = (key: jobKey, task: task)
    }

    private func finished(trackID: UUID, jobKey: String, outcome: StudioPitchJob.Outcome) {
        guard tasks[trackID]?.key == jobKey else { return }    // já substituído por outro
        tasks[trackID] = nil
        progress[trackID] = nil
        onFinished?(trackID, jobKey, outcome)
    }
}
