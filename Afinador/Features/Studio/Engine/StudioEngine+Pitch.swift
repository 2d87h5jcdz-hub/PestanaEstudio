import Foundation

// Afinação automática das faixas: ligação entre o StudioEngine e o serviço StudioPitchJobs.

@MainActor
extension StudioEngine {

    // MARK: - Afinação automática (StudioPitchJobs faz o trabalho; aqui só a ligação)

    /// Agenda uma verificação (com espera curta: o controle de intensidade muda muitas vezes seguidas).
    func schedulePitchRefresh() {
        pitchJobs.scheduleRefresh { [weak self] in
            self?.refreshPitchCorrection()
        }
    }

    /// Começa (ou cancela) os trabalhos de afinação conforme as faixas do projeto aberto.
    func refreshPitchCorrection() {
        guard let project else {
            pitchJobs.cancelAll()
            return
        }
        pitchJobs.refresh(tracks: project.tracks, folder: store.folder(for: project.id))
    }

    /// Fim de um trabalho (StudioPitchJobs já tirou o progresso dele).
    func pitchJobFinished(trackID: UUID, jobKey: String, outcome: StudioPitchJob.Outcome) {
        guard let index = project?.tracks.firstIndex(where: { $0.id == trackID }) else { return }
        if outcome.noVoice {
            pitchJobs.markFailed(jobKey)
            let name = project?.tracks[index].name ?? "a faixa"
            lastError = "Não encontrei voz suficiente em “\(name)” para descobrir o tom. Escolha o tom à mão."
            return
        }
        if let key = outcome.detectedKey, project?.tracks[index].pitch.isAuto == true {
            // Guarda o tom achado (dispara projectDidChange → reconstrói com os arquivos novos).
            project?.tracks[index].pitch.detectedKey = key
            project?.tracks[index].pitch.detectedScale = outcome.detectedScale
        }
        if outcome.completed {
            if transport == .stopped { rebuildEngineIfActive() } else { needsRebuild = true }
        }
    }
}
