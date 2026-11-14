import AVFoundation
import Foundation

// Faixas, clips, importação e exportação do StudioEngine.

@MainActor
extension StudioEngine: StudioImporting {

    // MARK: - Faixas

    /// Atualiza nome/volume/pan/mudo/solo/efeitos. Os clips (e campos legados) da faixa NÃO vêm do
    /// parâmetro: são preservados do projeto atual, para uma cópia antiga da faixa na UI não desfazer
    /// uma gravação/edição de clips. Para mexer em clips use moveClip/deleteClip/record.
    func updateTrack(_ track: StudioTrack) {
        guard let index = project?.tracks.firstIndex(where: { $0.id == track.id }),
              let existing = project?.tracks[index] else { return }
        var merged = track
        merged.clips = existing.clips
        merged.fileName = existing.fileName
        merged.offset = existing.offset
        merged.duration = existing.duration
        guard existing != merged else { return }
        project?.tracks[index] = merged     // didSet aplica ao vivo e salva
    }

    /// Apaga a faixa e os arquivos de áudio dela que nenhuma outra faixa usa.
    func deleteTrack(_ id: UUID) {
        guard let current = project, let track = current.tracks.first(where: { $0.id == id }) else { return }
        if transport != .stopped { stop() }
        var files = Set(track.clips.map { $0.fileName })
        if !track.fileName.isEmpty { files.insert(track.fileName) }
        // stop() pode ter acabado de gravar nesta faixa: relê os clips atuais.
        if let fresh = project?.tracks.first(where: { $0.id == id }) {
            for clip in fresh.clips { files.insert(clip.fileName) }
        }
        if armedTrackID == id { armedTrackID = nil }
        project?.tracks.removeAll { $0.id == id }   // didSet reconstrói o grafo e salva
        if let updated = project {
            StudioTimeline.deleteUnreferenced(files, project: updated,
                                              folder: store.folder(for: current.id))
        }
    }

    /// Cria uma faixa vazia ("Faixa N" se name for nil/vazio), salva e a deixa armada
    /// (o armar é ignorado durante contagem/gravação). Não reconstrói o grafo.
    @discardableResult
    func addEmptyTrack(name: String? = nil) -> StudioTrack {
        let trimmed = name?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        var track = StudioTrack(name: trimmed.isEmpty ? nextTrackName() : trimmed)
        track.effects = .preset(.none)
        guard project != nil else {
            fail(.noProjectOpen)
            return track
        }
        project?.tracks.append(track)       // didSet salva (faixa sem clips: grafo intacto)
        switch transport {
        case .stopped, .playing: armedTrackID = track.id
        case .countIn, .recording: break
        }
        return track
    }

    /// Arma (ou desarma com nil) a faixa da próxima gravação. Ignorado se o transporte não estiver
    /// parado ou se a faixa não existir no projeto aberto.
    func arm(_ trackID: UUID?) {
        guard transport == .stopped else { return }
        guard let trackID else { armedTrackID = nil; return }
        guard project?.tracks.contains(where: { $0.id == trackID }) == true else { return }
        armedTrackID = trackID
    }

    /// Move um clip na linha do tempo (offset >= 0). Só com o transporte parado.
    func moveClip(_ clipID: UUID, in trackID: UUID, to offset: TimeInterval) {
        guard transport == .stopped, offset.isFinite,
              let t = project?.tracks.firstIndex(where: { $0.id == trackID }),
              var track = project?.tracks[t],
              let c = track.clips.firstIndex(where: { $0.id == clipID }) else { return }
        let target = max(0, offset)
        guard abs(track.clips[c].offset - target) > 0.000_1 else { return }
        track.clips[c].offset = target
        track.studioSyncLegacyFields()      // reordena por offset
        project?.tracks[t] = track          // didSet reconstrói o grafo e salva
    }

    /// Remove um clip (apaga o arquivo se nenhum outro clip do projeto o usa). Só parado.
    func deleteClip(_ clipID: UUID, in trackID: UUID) {
        guard transport == .stopped, let current = project,
              let t = current.tracks.firstIndex(where: { $0.id == trackID }),
              let clip = current.tracks[t].clips.first(where: { $0.id == clipID }) else { return }
        var track = current.tracks[t]
        track.clips.removeAll { $0.id == clipID }
        track.studioSyncLegacyFields()
        project?.tracks[t] = track          // didSet reconstrói o grafo e salva
        if let updated = project {
            StudioTimeline.deleteUnreferenced([clip.fileName], project: updated,
                                              folder: store.folder(for: current.id))
        }
    }

    /// Forma de onda do trecho tocado do clip (fileStart…fileStart+duration): `buckets` picos lineares
    /// 0…1 (máximo absoluto entre canais; não normalizado pelo clip). Calcula fora da main thread e
    /// guarda "<arquivo>.peaks" na pasta do projeto; chamadas seguintes só fatiam. [] em erro.
    func waveform(for clip: StudioClip, buckets: Int) async -> [Float] {
        guard buckets > 0, let url = fileURL(for: clip) else { return [] }
        return await StudioWaveforms.waveform(fileURL: url, fileStart: clip.fileStart,
                                              duration: clip.duration, buckets: buckets)
    }

    /// Tira uma faixa criada por record() que ficou sem nenhum clip.
    func removeEmptyTrack(_ id: UUID) {
        guard let track = project?.tracks.first(where: { $0.id == id }), track.clips.isEmpty else { return }
        if armedTrackID == id { armedTrackID = nil }
        project?.tracks.removeAll { $0.id == id }
    }

    func importAudio(from url: URL, name: String) async throws -> StudioTrack {
        guard let current = project else { throw fail(.noProjectOpen) }
        let projectID = current.id
        let folder = store.folder(for: projectID)
        let trackID = UUID()

        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }

        let result: StudioImportResult
        do {
            result = try await Task.detached(priority: .userInitiated) {
                try StudioFileOps.importCopy(from: url, into: folder, trackID: trackID)
            }.value
        } catch let error as StudioEngineError {
            throw fail(error)
        } catch {
            throw fail(.copyFailed(url.lastPathComponent))
        }

        guard project?.id == projectID else {
            try? FileManager.default.removeItem(at: folder.appendingPathComponent(result.fileName))
            throw fail(.projectChanged)
        }

        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        var track = StudioTrack(name: trimmed.isEmpty ? url.deletingPathExtension().lastPathComponent : trimmed)
        track.id = trackID
        track.isImported = true
        track.clips = [StudioClip(fileName: result.fileName, offset: 0, fileStart: 0, duration: result.duration)]
        track.studioSyncLegacyFields()
        project?.tracks.append(track)       // didSet reconstrói o grafo e salva
        return track
    }

    // MARK: - Exportação

    func exportMix(format: ExportFormat) async throws -> URL {
        guard project != nil else { throw fail(.noProjectOpen) }
        flushSave()
        guard let snapshot = project else { throw fail(.noProjectOpen) }
        let folder = store.folder(for: snapshot.id)
        let baseName = store.sanitizedFileName(snapshot.name)
        let destination = FileManager.default.temporaryDirectory
            .appendingPathComponent("\(baseName).\(format.rawValue)")

        isExporting = true
        defer { isExporting = false }
        do {
            try await Task.detached(priority: .userInitiated) {
                try StudioOfflineRenderer.render(project: snapshot, folder: folder, format: format,
                                                 to: destination)
            }.value
        } catch let error as StudioEngineError {
            throw fail(error)
        } catch is CancellationError {
            throw CancellationError()
        } catch {
            throw fail(.exportFailed(error.localizedDescription))
        }
        return destination
    }


    // MARK: - Treino → Estúdio (StudioImporting)

    func createProject(named name: String) {
        _ = newProject(name: name)
    }

    func importTrack(from url: URL, name: String) async throws {
        _ = try await importAudio(from: url, name: name)
    }
}
