import AVFoundation
import SwiftUI
import UIKit

@MainActor
struct MainTabView: View {
    @StateObject private var tuner = TunerEngine()
    @StateObject private var metronome = Metronome()
    @StateObject private var studio = StudioEngine()
    @StateObject private var looper = LooperEngine()
    @State private var tab = 0
    @Environment(\.scenePhase) private var scenePhase
    /// Fase atual da cena guardada (para soltar o áudio quando a atividade termina em segundo plano).
    @State private var inBackground = false
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
        .onChange(of: studio.transport) { _ in releaseIfFinishedInBackground() }
        .onChange(of: studio.monitorEnabled) { _ in releaseIfFinishedInBackground() }
        .onChange(of: looper.isIdleForBackground) { idle in
            if idle { releaseIfFinishedInBackground() }
        }
        .onChange(of: tab) { newTab in
            // O metrônomo da aba própria vazaria nas gravações e brigaria com o clique do Estúdio/Treino.
            if newTab >= 3 && metronome.playing { metronome.stop() }
        }
    }
}

extension MainTabView {
    /// Em segundo plano (outro app na frente) o áudio continua só onde há algo acontecendo:
    /// gravando, tocando, contando ou ouvindo a entrada. O resto solta o microfone e a bateria.
    /// O Treino cuida de si mesmo (PracticeView). O metrônomo segue tocando se estiver ligado.
    fileprivate func handleScenePhase(_ phase: ScenePhase) {
        inBackground = phase == .background
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

    /// Já em segundo plano: quando a música do Estúdio acaba ou o Loop fica ocioso, desliga o motor
    /// como ao entrar em segundo plano. Ao voltar (.active) a aba religa sozinha.
    fileprivate func releaseIfFinishedInBackground() {
        guard inBackground else { return }
        if tab == 3 && studio.transport == .stopped && !studio.monitorEnabled { studio.deactivate() }
        if tab == 5 && looper.running && looper.isIdleForBackground { looper.stop() }
    }
}
