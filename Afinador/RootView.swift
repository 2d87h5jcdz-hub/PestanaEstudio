import AVFoundation
import SwiftUI
import UIKit

/// Ganho analógico da entrada (o pré-amplificador do aparelho), quando o iOS deixa mexer.
/// É o único jeito de evitar que a entrada estoure ANTES de chegar ao app.
enum HardwareInputGain {
    static var isSettable: Bool { AVAudioSession.sharedInstance().isInputGainSettable }
    static var value: Float { AVAudioSession.sharedInstance().inputGain }
    static func set(_ value: Float) {
        let s = AVAudioSession.sharedInstance()
        guard s.isInputGainSettable else { return }
        try? s.setInputGain(min(1, max(0, value)))
    }
}

/// Pico bruto (0…1) de um buffer, sem escala.
func rawPeak(of buffer: AVAudioPCMBuffer) -> Float {
    guard let data = buffer.floatChannelData else { return 0 }
    let frames = Int(buffer.frameLength)
    let channels = buffer.format.isInterleaved ? 1 : Int(buffer.format.channelCount)
    let count = buffer.format.isInterleaved ? frames * Int(buffer.format.channelCount) : frames
    var peak: Float = 0
    for c in 0..<channels {
        let p = data[c]
        for i in 0..<count { peak = max(peak, abs(p[i])) }
    }
    return peak
}

enum AudioSession {
    /// Opções da sessão em todo o app. `mixWithOthers`: um app de cifra (ou vídeo) tocando som
    /// não interrompe a gravação nem o Treino/Loop em segundo plano.
    static let options: AVAudioSession.CategoryOptions = [.defaultToSpeaker, .allowBluetoothA2DP, .mixWithOthers]

    static func activate() {
        let s = AVAudioSession.sharedInstance()
        if s.category != .playAndRecord || s.categoryOptions != options {
            try? s.setCategory(.playAndRecord, mode: .default, options: options)
        }
        try? s.setActive(true)
    }
}

@MainActor
struct ContentView: View {
    @StateObject private var tuner = TunerEngine()
    @StateObject private var metronome = Metronome()
    @StateObject private var studio = StudioEngine()
    @StateObject private var looper = LooperEngine()
    @State private var tab = 0
    @Environment(\.scenePhase) private var scenePhase
    /// Aviso de boas-vindas (gratuito / doação) só na primeira abertura.
    @AppStorage("about.welcomeShown") private var welcomeShown = false

    init() {
        let appearance = UITabBarAppearance()
        appearance.configureWithOpaqueBackground()
        appearance.backgroundColor = UIColor(red: 0.086, green: 0.071, blue: 0.059, alpha: 1)
        appearance.shadowColor = UIColor(red: 0.227, green: 0.188, blue: 0.161, alpha: 1)
        UITabBar.appearance().standardAppearance = appearance
        UITabBar.appearance().scrollEdgeAppearance = appearance
    }

    var body: some View {
        TabView(selection: $tab) {
            TunerView(tuner: tuner)
                .statusBarCover()
                .tabItem { Label("Afinador", systemImage: "tuningfork") }
                .tag(0)
            ChordsView(a4: tuner.a4)
                .statusBarCover()
                .tabItem { Label("Acordes", systemImage: "guitars") }
                .tag(1)
            MetronomeView(metronome: metronome)
                .statusBarCover()
                .tabItem { Label("Metrônomo", systemImage: "metronome") }
                .tag(2)
            StudioView(studio: studio)
                .statusBarCover()
                .tabItem { Label("Estúdio", systemImage: "waveform") }
                .tag(3)
            PracticeView(studio: studio, openStudio: { tab = 3 })
                .statusBarCover()
                .tabItem { Label("Treino", systemImage: "music.note.list") }
                .tag(4)
            LoopView(looper: looper)
                .statusBarCover()
                .tabItem { Label("Loop", systemImage: "repeat.circle") }
                .tag(5)
        }
        .sheet(isPresented: Binding(get: { !welcomeShown }, set: { if !$0 { welcomeShown = true } })) {
            AboutView(isWelcome: true)
        }
        .tint(Color.brass)
        .onChange(of: scenePhase) { phase in handleScenePhase(phase) }
        .onChange(of: tab) { newTab in
            // O metrônomo da aba própria vazaria nas gravações e brigaria com o clique do Estúdio/Treino.
            if newTab >= 3 && metronome.playing { metronome.stop() }
        }
    }
}

extension ContentView {
    /// Em segundo plano (outro app na frente) o áudio continua só onde há algo acontecendo:
    /// gravando, tocando, contando ou ouvindo a entrada. O resto solta o microfone e a bateria.
    /// O Treino cuida de si mesmo (PracticeView). O metrônomo segue tocando se estiver ligado.
    fileprivate func handleScenePhase(_ phase: ScenePhase) {
        switch phase {
        case .background:
            if tab == 0 { tuner.stop() }
            if tab == 3 && studio.transport == .stopped && !studio.monitorEnabled { studio.deactivate() }
            if tab == 5 && looper.isIdleForBackground { looper.stop() }
        case .active:
            if tab == 0 { tuner.start() }
            if tab == 3 { studio.activate() }
            if tab == 5 { looper.start() }
        default:
            break
        }
    }
}

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
