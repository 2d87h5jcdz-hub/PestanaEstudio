import SwiftUI

/// Faixa da cor do fundo atrás da barra de status: o conteúdo que rola não passa por baixo
/// do relógio e da bateria (na horizontal a altura é zero e nada aparece).
struct StatusBarCover: ViewModifier {
    func body(content: Content) -> some View {
        content.overlay(alignment: .top) {
            // O leitor ocupa a tela toda; safeAreaInsets.top é a altura da barra de status.
            GeometryReader { geo in
                Color.bg
                    .frame(maxWidth: .infinity)
                    .frame(height: geo.safeAreaInsets.top)
            }
            .ignoresSafeArea()
            .allowsHitTesting(false)
        }
    }
}

extension View {
    func statusBarCover() -> some View { modifier(StatusBarCover()) }
}
