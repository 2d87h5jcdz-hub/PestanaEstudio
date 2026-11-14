import Foundation

// MARK: - Treino → Estúdio
//
// O Treino manda a música (ou os instrumentos separados) para o Estúdio sem conhecer o StudioEngine
// (regra 2: Features não se conhecem). O App injeta o StudioEngine, que cumpre este protocolo.
// O Treino não observa nada publicado do Estúdio: só chama estas duas ações.

@MainActor
protocol StudioImporting: AnyObject {
    /// Cria e abre um projeto novo no Estúdio.
    func createProject(named name: String)
    /// Importa um arquivo de áudio como faixa nova do projeto aberto.
    func importTrack(from url: URL, name: String) async throws
}
