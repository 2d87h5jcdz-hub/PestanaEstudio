import SwiftUI

/// Barra de nível simples (0…1).
struct LevelBar: View {
    let level: Float
    var body: some View {
        GeometryReader { geo in
            let clamped = CGFloat(min(1, max(0, level)))
            ZStack(alignment: .leading) {
                Capsule().fill(Color.line)
                Capsule()
                    .fill(clamped > 0.9 ? Color.off : Color.ok)
                    .frame(width: geo.size.width * clamped)
            }
        }
    }
}
