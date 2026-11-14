import AVFoundation
import Combine
import Foundation
import UIKit

// Ciclo de vida, sessão de áudio, permissão do microfone, escolha da entrada, rota e interrupções.

@MainActor
extension StudioEngine {

    // MARK: - Ciclo de vida

    func activate() {
        if isActive {
            if engine == nil || engine?.isRunning == false { rebuildEngine() }
            return
        }
        isActive = true
        configureSession()
        updateRouteInfo()

        let permission = AVAudioSession.sharedInstance().recordPermission
        if permission == .granted {
            micGranted = true
        } else {
            micGranted = false
            if permission == .undetermined { requestMicPermission() }
        }
        rebuildEngine()
        startTimer()
    }

    func deactivate() {
        guard isActive else { return }
        // Marca inativo antes de parar: assim a tomada finalizada não reconstrói o grafo à toa.
        isActive = false
        if transport != .stopped { stop() }
        flushSave()
        rebuildTask?.cancel()
        rebuildTask = nil
        teardownEngine()
        timerCancellable = nil
        inputLevel = 0
        trackLevels = [:]
        recordAfterPermission = false
        restoreIOBuffer()
        UIApplication.shared.isIdleTimerDisabled = false
    }

    // MARK: - Sessão de áudio

    func configureSession() {
        do {
            try session.configureLowLatency(previousBuffer: &previousIOBuffer)
            applyPreferredInput()
        } catch {
            lastError = "Não foi possível configurar o áudio do aparelho. Feche outros apps de áudio e tente de novo."
        }
    }

    func restoreIOBuffer() {
        session.restoreIOBuffer(&previousIOBuffer)
    }

    func requestMicPermission() {
        Self.requestRecordPermission { granted in
            Task { @MainActor [weak self] in
                self?.micPermissionResolved(granted)
            }
        }
    }

    /// Fora do MainActor de propósito: o callback do sistema chega numa thread qualquer.
    nonisolated private static func requestRecordPermission(_ done: @escaping @Sendable (Bool) -> Void) {
        AVAudioSession.sharedInstance().requestRecordPermission { granted in
            done(granted)
        }
    }

    private func micPermissionResolved(_ granted: Bool) {
        micGranted = granted
        // Não reconstrói o grafo no meio da reprodução (mataria o som sem atualizar o transporte).
        if granted && isActive {
            if transport == .stopped { rebuildEngine() } else { needsRebuild = true }
        }
        if recordAfterPermission {
            recordAfterPermission = false
            if granted { record() } else { fail(.microphoneDenied) }
        }
        if !granted && monitorEnabled { fail(.microphoneDenied) }
    }

    func observeSession() {
        let center = NotificationCenter.default
        center.publisher(for: AVAudioSession.routeChangeNotification)
            .receive(on: DispatchQueue.main)
            .sink { [weak self] note in self?.handleRouteChange(note) }
            .store(in: &sessionCancellables)
        center.publisher(for: AVAudioSession.interruptionNotification)
            .receive(on: DispatchQueue.main)
            .sink { [weak self] note in self?.handleInterruption(note) }
            .store(in: &sessionCancellables)
        center.publisher(for: AVAudioSession.mediaServicesWereResetNotification)
            .receive(on: DispatchQueue.main)
            .sink { [weak self] _ in self?.handleMediaServicesReset() }
            .store(in: &sessionCancellables)
    }

    // MARK: - Escolha da entrada

    /// Ao plugar um fone com microfone, o iOS troca a entrada para o microfone do fone (longe do
    /// violão, capta baixo). Aqui vale a escolha do usuário; sem escolha salva: interface USB se
    /// houver, senão o microfone do iPhone.
    func applyPreferredInput() {
        do {
            try session.applyPreferredInput()
        } catch let error as AudioInputSelectionError {
            lastError = "Não foi possível usar \(error.inputTitle) como entrada."
        } catch {}
    }

    /// Troca a entrada (só com o transporte parado). A escolha fica salva pelo tipo de entrada.
    func selectInput(_ id: String) {
        guard transport == .stopped else { return }
        do {
            let found: Bool = try session.selectInput(uid: id)
            if !found { return }
        } catch let error as AudioInputSelectionError {
            lastError = "Não foi possível usar \(error.inputTitle) como entrada."
        } catch {}
        updateRouteInfo()
    }

    func updateRouteInfo() {
        let audio: AudioSessionManaging = session
        availableInputs = audio.availableInputs.map { (port: AVAudioSessionPortDescription) -> AudioInputOption in
            AudioInputOption(id: port.uid, name: audio.inputTitle(port, markBluetooth: true),
                              portType: port.portType.rawValue)
        }
        let current: AVAudioSessionPortDescription? = audio.currentInput
        selectedInputID = current?.uid
        if let input = current {
            inputName = audio.inputTitle(input, markBluetooth: true)
        } else {
            inputName = "Sem entrada"
        }
        isHeadphoneConnected = audio.isHeadphoneConnected
        hardwareGainSettable = audio.isInputGainSettable
        if hardwareGainSettable {
            syncingHardwareGain = true
            hardwareGain = audio.inputGain
            syncingHardwareGain = false
        }
    }

    private func handleRouteChange(_ note: Notification) {
        if isActive && transport == .stopped { applyPreferredInput() }
        updateRouteInfo()
        let raw = note.userInfo?[AVAudioSessionRouteChangeReasonKey] as? UInt
        let reason = raw.flatMap { AVAudioSession.RouteChangeReason(rawValue: $0) }
        // Fone desconectado durante a reprodução: para (comportamento padrão do iOS).
        if reason == .oldDeviceUnavailable && transport == .playing { stop() }
        if monitorEnabled && !isHeadphoneConnected { lastError = Self.feedbackWarning }
    }

    private func handleInterruption(_ note: Notification) {
        guard let raw = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt,
              let type = AVAudioSession.InterruptionType(rawValue: raw) else { return }
        switch type {
        case .began:
            // Ligação, alarme, Siri…: encerra a tomada (o que já foi gravado é salvo).
            if transport != .stopped { stop() }
        case .ended:
            let optionsRaw = note.userInfo?[AVAudioSessionInterruptionOptionKey] as? UInt ?? 0
            let options = AVAudioSession.InterruptionOptions(rawValue: optionsRaw)
            if isActive && options.contains(.shouldResume) {
                configureSession()
                rebuildEngine()
            }
        @unknown default:
            break
        }
    }

    private func handleMediaServicesReset() {
        guard isActive else { return }
        if transport != .stopped { stop() }
        teardownEngine()
        configureSession()
        rebuildEngine()
    }

    func handleEngineConfigurationChange() {
        // A engine se desliga sozinha quando a rota/formato muda (fone, interface USB…).
        // Se ela continua rodando, a notificação é espúria: ignoramos (evita laço de reconstrução).
        if let engine, engine.isRunning { return }
        if transport != .stopped {
            stop()
            lastError = "A saída ou entrada de áudio mudou. A reprodução foi parada."
        }
        rebuildTask?.cancel()
        rebuildTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 150_000_000)
            guard let self, !Task.isCancelled, self.isActive else { return }
            self.updateRouteInfo()
            self.rebuildEngine()
        }
    }
}
