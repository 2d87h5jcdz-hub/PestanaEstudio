import SwiftUI

/// Cartão com fundo e borda usado em todas as abas.
struct StudioPanelModifier: ViewModifier {
    var padding: CGFloat = 14
    var stroke: Color = .line

    func body(content: Content) -> some View {
        content
            .padding(padding)
            .background(RoundedRectangle(cornerRadius: 18).fill(Color.panel))
            .overlay(RoundedRectangle(cornerRadius: 18).stroke(stroke))
    }
}

extension View {
    func studioPanel(padding: CGFloat = 14, stroke: Color = .line) -> some View {
        modifier(StudioPanelModifier(padding: padding, stroke: stroke))
    }
}

/// Botão-pílula pequeno usado em vários lugares (M, S, FX…).
struct StudioChip: View {
    let title: String
    var systemImage: String? = nil
    var isOn: Bool = false
    var onColor: Color = .brass
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 5) {
                if let systemImage = systemImage {
                    Image(systemName: systemImage).font(.system(size: 12, weight: .semibold))
                }
                Text(title).font(.system(size: 13, weight: .bold, design: .monospaced))
            }
            .padding(.horizontal, 12)
            .frame(minHeight: 34)
            .background(Capsule().fill(isOn ? onColor : Color.bg))
            .overlay(Capsule().stroke(isOn ? onColor : Color.line))
            .foregroundColor(isOn ? Color.bg : Color.fg)
        }
        .buttonStyle(.plain)
    }
}
