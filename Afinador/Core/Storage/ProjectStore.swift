import Foundation

// MARK: - Projetos em disco
//
// Documents/Projects/<id>/project.json + os arquivos de áudio na mesma pasta (visível no app Arquivos).
// O protocolo é genérico no tipo do projeto: Core não conhece os modelos do Estúdio (regra 1).

/// O que um projeto precisa para ser guardado.
protocol StoredProject: Codable {
    var id: UUID { get }
    var updatedAt: Date { get }
}

protocol ProjectStore {
    /// Pasta do projeto (os arquivos de áudio ficam nela).
    func folder(for id: UUID) -> URL
    /// Todos os projetos salvos, mais recente primeiro. Pastas sem project.json válido são ignoradas.
    func loadAll<Project: StoredProject>() -> [Project]
    func save<Project: StoredProject>(_ project: Project) throws
    /// Apaga a pasta do projeto inteira (se existir).
    func deleteProject(_ id: UUID) throws
    /// Nome de arquivo seguro a partir do nome do projeto (exportação).
    func sanitizedFileName(_ name: String) -> String
}

extension ProjectStore {
    /// Arquivo dentro da pasta do projeto.
    func fileURL(projectID: UUID, fileName: String) -> URL {
        folder(for: projectID).appendingPathComponent(fileName)
    }
}

/// Implementação em Documents/Projects (o formato de sempre).
struct DiskProjectStore: ProjectStore {
    var projectsRoot: URL {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        return docs.appendingPathComponent("Projects", isDirectory: true)
    }

    func folder(for id: UUID) -> URL {
        projectsRoot.appendingPathComponent(id.uuidString, isDirectory: true)
    }

    func manifestURL(for id: UUID) -> URL {
        folder(for: id).appendingPathComponent("project.json")
    }

    static func makeEncoder() -> JSONEncoder {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        return encoder
    }

    static func makeDecoder() -> JSONDecoder {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return decoder
    }

    func loadAll<Project: StoredProject>() -> [Project] {
        let fm = FileManager.default
        let root = projectsRoot
        try? fm.createDirectory(at: root, withIntermediateDirectories: true)
        guard let dirs = try? fm.contentsOfDirectory(at: root, includingPropertiesForKeys: nil,
                                                       options: [.skipsHiddenFiles]) else { return [] }
        let decoder = Self.makeDecoder()
        var result: [Project] = []
        for dir in dirs {
            let manifest = dir.appendingPathComponent("project.json")
            guard let data = try? Data(contentsOf: manifest),
                  let project = try? decoder.decode(Project.self, from: data) else { continue }
            result.append(project)
        }
        return result.sorted { $0.updatedAt > $1.updatedAt }
    }

    func save<Project: StoredProject>(_ project: Project) throws {
        let dir = folder(for: project.id)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let data = try Self.makeEncoder().encode(project)
        try data.write(to: manifestURL(for: project.id), options: .atomic)
    }

    func deleteProject(_ id: UUID) throws {
        let dir = folder(for: id)
        let fm = FileManager.default
        if fm.fileExists(atPath: dir.path) {
            try fm.removeItem(at: dir)
        }
    }

    func sanitizedFileName(_ name: String) -> String {
        let invalid = CharacterSet(charactersIn: "/\\:?%*|\"<>")
        let cleaned = name.components(separatedBy: invalid).joined(separator: "-")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        return cleaned.isEmpty ? "Mixagem" : cleaned
    }
}
