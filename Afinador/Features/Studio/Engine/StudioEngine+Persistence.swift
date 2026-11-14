import Foundation

// Observação do projeto aberto (o que reconstruir) e gravação em disco (via ProjectStore).

@MainActor
extension StudioEngine {

    // MARK: - Persistência

    func projectDidChange(from old: StudioProject?) {
        guard !suppressObservation else { return }
        let new = project

        if old?.id != new?.id {
            // Troca de projeto (open/new/close, ou a UI atribuiu outro projeto direto).
            if saveTask != nil, let old {
                saveTask?.cancel()
                saveTask = nil
                var stamped = old
                stamped.updatedAt = Date()
                writeProject(stamped)
            }
            if transport != .stopped { abortTransport() }
            position = 0
            armedTrackID = nil
            if let new { upsertInList(new) }
            rebuildEngineIfActive()
            schedulePitchRefresh()
            return
        }

        guard let new, let old, new != old else { return }
        if let armed = armedTrackID, !new.tracks.contains(where: { $0.id == armed }) {
            armedTrackID = nil      // faixa armada apagada
        }
        let oldIDs = old.tracks.map { $0.id }
        let newIDs = new.tracks.map { $0.id }
        schedulePitchRefresh()
        if Self.playbackLayout(old) != Self.playbackLayout(new) || Self.pitchLayout(old) != Self.pitchLayout(new) {
            // Mudou o que toca (faixa com clips entrou/saiu, clip movido/aparado/apagado).
            if transport == .playing { stop() }
            if transport == .stopped { rebuildEngineIfActive() } else { needsRebuild = true }
            saveNow()       // mudança estrutural: salva já
        } else if oldIDs != newIDs {
            // Só faixas vazias entraram/saíram ou a ordem mudou: o grafo continua válido.
            applyMixToChains()
            saveNow()
        } else {
            applyMixToChains()
            upsertInList(new)
            scheduleSave()  // ajustes de volume/pan/efeito: salva com debounce
        }
        if old.bpm != new.bpm || old.beatsPerBar != new.beatsPerBar {
            click?.setTempo(bpm: new.bpm, beatsPerBar: new.beatsPerBar)
        }
    }

    /// Assinatura da afinação por faixa: se muda, os arquivos tocados mudam (reconstrói o grafo).
    private static func pitchLayout(_ p: StudioProject) -> [UUID: String] {
        var map: [UUID: String] = [:]
        for track in p.tracks where StudioPitchFiles.needsProcessing(track) {
            map[track.id] = track.effectsOn ? (StudioPitchFiles.processSignature(for: track) ?? "") : "fx-off"
        }
        return map
    }

    /// Clips por faixa (só faixas com clips): se isso muda, o grafo precisa ser reconstruído.
    private static func playbackLayout(_ p: StudioProject) -> [UUID: [StudioClip]] {
        var map: [UUID: [StudioClip]] = [:]
        for track in p.tracks where !track.clips.isEmpty { map[track.id] = track.clips }
        return map
    }

    private func scheduleSave() {
        saveTask?.cancel()
        saveTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 500_000_000)
            guard !Task.isCancelled else { return }
            self?.saveNow()
        }
    }

    func flushSave() {
        if saveTask != nil { saveNow() }
    }

    private func saveNow() {
        saveTask?.cancel()
        saveTask = nil
        guard var current = project else { return }
        current.updatedAt = Date()
        // Rede de segurança: se a UI mexeu em `clips` direto, o espelho legado é corrigido antes de salvar
        // (senão uma faixa esvaziada voltaria a ter o clip antigo ao reabrir, pela migração v1 do decoder).
        for i in current.tracks.indices { current.tracks[i].studioSyncLegacyFields() }
        suppressObservation = true
        project = current
        suppressObservation = false
        writeProject(current)
    }

    func writeProject(_ p: StudioProject) {
        do {
            try store.save(p)
        } catch {
            lastError = "Não foi possível salvar o projeto “\(p.name)”."
        }
        upsertInList(p)
    }

    private func upsertInList(_ p: StudioProject) {
        var list = projects
        if let index = list.firstIndex(where: { $0.id == p.id }) {
            list[index] = p
        } else {
            list.append(p)
        }
        list.sort { $0.updatedAt > $1.updatedAt }
        projects = list
    }

    func stopForProjectSwitch() {
        if transport != .stopped { stop() }
        flushSave()
    }
}
