import AppKit
import IOKit
import IOKit.hid
import SwiftUI

let tuneMarker = "g920 tune build 1"

struct Tune: Equatable {
    var strength: Double = 100
    var ceiling: Double = 100
    var invert: Bool = false
}

@MainActor
final class WheelModel: ObservableObject {
    @Published var tune = Tune()
    @Published var wheelConnected = false
    @Published var shimInstalled = false
    @Published var compatOff = true
    @Published var gameRunning = false
    @Published var gameHubNeedsRestart = false
    @Published var gameHubFound = false
    @Published var note: String?
    @Published var busy = false

    private var saveTask: Task<Void, Never>?
    private let supportURL: URL
    private let confURL: URL

    init() {
        let home = FileManager.default.homeDirectoryForCurrentUser
        supportURL = home.appendingPathComponent("Library/Application Support/G920")
        confURL = supportURL.appendingPathComponent("ffb.conf")
        tune = Self.readTune(confURL)
        Task { await refresh(force: true) }
        Timer.scheduledTimer(withTimeInterval: 20, repeats: true) { _ in
            Task { @MainActor in await self.refresh() }
        }
    }

    var ready: Bool {
        wheelConnected && shimInstalled && compatOff && gameHubFound && !gameHubNeedsRestart
    }

    var headline: String {
        if !gameHubFound { return "GameHub не найден" }
        if !wheelConnected { return "Руль не найден" }
        if !shimInstalled { return "Отдача не установлена" }
        if !compatOff || gameHubNeedsRestart { return "Нужно перезапустить GameHub" }
        return "Руль готов"
    }

    var detail: String {
        if !gameHubFound { return "Поставь GameHub и ACC, затем нажми Починить" }
        if !wheelConnected { return "Включи блок питания и воткни USB" }
        if gameHubNeedsRestart { return "Полностью закрой GameHub и открой снова" }
        if gameRunning && shimInstalled { return "Регуляторы действуют на ходу" }
        return "Оси, кнопки и отдача подготовлены"
    }

    func setStrength(_ value: Double) {
        tune.strength = value.rounded()
        scheduleSave()
    }

    func setCeiling(_ value: Double) {
        tune.ceiling = value.rounded()
        scheduleSave()
    }

    func setInvert(_ value: Bool) {
        tune.invert = value
        scheduleSave()
    }

    func repair() {
        Task { await refresh(force: true) }
    }

    private func scheduleSave() {
        saveTask?.cancel()
        let tune = tune
        let url = confURL
        let dir = supportURL
        saveTask = Task {
            try? await Task.sleep(nanoseconds: 80_000_000)
            if Task.isCancelled { return }
            Self.writeTune(tune, directory: dir, url: url)
        }
    }

    private func refresh(force: Bool = false) async {
        if busy { return }
        busy = true
        defer { busy = false }
        let conf = confURL
        let dir = supportURL
        let current = tune
        let bundled = Bundle.main.url(forResource: "libSDL2-2.0.0", withExtension: "dylib")
        let report = await Task.detached {
            Preflight.run(tune: current, confURL: conf, supportURL: dir, bundledShim: bundled, force: force)
        }.value
        wheelConnected = report.wheelConnected
        shimInstalled = report.shimInstalled
        compatOff = report.compatOff
        gameRunning = report.gameRunning
        gameHubNeedsRestart = report.gameHubNeedsRestart
        gameHubFound = report.gameHubFound
        note = report.note
    }

    nonisolated static func readTune(_ url: URL) -> Tune {
        guard let text = try? String(contentsOf: url, encoding: .utf8) else { return Tune() }
        var tune = Tune()
        for line in text.split(separator: "\n") {
            let parts = line.split(separator: "=", maxSplits: 1)
            guard parts.count == 2 else { continue }
            let key = parts[0]
            let raw = String(parts[1])
            if key == "strength", let v = Double(raw) { tune.strength = min(120, max(40, v)) }
            if key == "ceiling", let v = Double(raw) { tune.ceiling = min(100, max(50, v)) }
            if key == "invert" { tune.invert = raw != "0" }
        }
        return tune
    }

    nonisolated static func writeTune(_ tune: Tune, directory: URL, url: URL) {
        let fm = FileManager.default
        try? fm.createDirectory(at: directory, withIntermediateDirectories: true)
        let body = "strength=\(Int(tune.strength))\nceiling=\(Int(tune.ceiling))\ninvert=\(tune.invert ? 1 : 0)\n"
        try? body.write(to: url, atomically: true, encoding: .utf8)
    }
}

struct PreflightReport: Sendable {
    var wheelConnected = false
    var shimInstalled = false
    var compatOff = true
    var gameRunning = false
    var gameHubNeedsRestart = false
    var gameHubFound = false
    var note: String?
}

enum Preflight {
    nonisolated static func run(tune: Tune, confURL: URL, supportURL: URL, bundledShim: URL?, force: Bool) -> PreflightReport {
        var report = PreflightReport()
        WheelModel.writeTune(tune, directory: supportURL, url: confURL)

        let mfiBefore = shell("/bin/launchctl", ["getenv", "SDL_JOYSTICK_MFI"]).trimmingCharacters(in: .whitespacesAndNewlines)
        _ = shell("/bin/launchctl", ["setenv", "SDL_JOYSTICK_MFI", "0"])
        _ = shell("/bin/launchctl", ["unsetenv", "WINEENGINE_STEAM_GAMEPAD_COMPAT_MODE"])

        report.wheelConnected = wheelIsConnected()
        report.gameRunning = commandList().contains("AC2-Win64-Shipping")
        let gameHubRunning = !NSRunningApplication.runningApplications(withBundleIdentifier: "com.gamemac.www").isEmpty
        report.gameHubFound = gameHubPresent()

        let compat = silenceGamepadCompat()
        report.compatOff = compat.nowOff

        if let bundledShim {
            let shim = installShim(bundled: bundledShim, force: force)
            report.shimInstalled = shim.installed
            if !report.gameHubFound {
                report.note = shim.problem ?? "Сначала установи GameHub и Assetto Corsa Competizione."
            } else if shim.replaced && report.gameRunning {
                report.note = "Выйди из ACC и зайди снова, чтобы подхватить отдачу."
            } else if !shim.installed {
                report.note = shim.problem
            }
        } else {
            report.shimInstalled = false
            report.note = "В приложении нет файла отдачи. Возьми свежий G920.app из репозитория."
        }

        let mfiNeedsRestart = mfiBefore != "0" && gameHubRunning
        if (compat.changed || mfiNeedsRestart) && gameHubRunning {
            report.gameHubNeedsRestart = true
            report.note = "Закрой GameHub полностью (не только окно) и открой снова — иначе оси руля не подхватятся."
        }

        if !report.wheelConnected && report.note == nil {
            report.note = "Блок питания руля должен быть включён, не только USB."
        }
        return report
    }

    nonisolated private static func gameHubPresent() -> Bool {
        let app = FileManager.default.fileExists(atPath: "/Applications/GameHub.app")
        let support = FileManager.default.fileExists(
            atPath: FileManager.default.homeDirectoryForCurrentUser
                .appendingPathComponent("Library/Application Support/com.gamemac.www").path
        )
        return app || support
    }

    nonisolated private static func wheelIsConnected() -> Bool {
        let manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
        let matching: NSDictionary = [
            kIOHIDVendorIDKey: 0x046d,
            kIOHIDProductIDKey: 0xc262,
        ]
        IOHIDManagerSetDeviceMatching(manager, matching)
        guard IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone)) == kIOReturnSuccess else { return false }
        defer { IOHIDManagerClose(manager, IOOptionBits(kIOHIDOptionsTypeNone)) }
        guard let raw = IOHIDManagerCopyDevices(manager) else { return false }
        return CFSetGetCount(raw) > 0
    }

    /// Turns off GameHub's gamepad compat for ACC only — compat hides the real G920 from Wine.
    nonisolated private static func silenceGamepadCompat() -> (changed: Bool, nowOff: Bool) {
        let root = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/com.gamemac.www/gamehub/game-settings")
        let urls = (try? FileManager.default.contentsOfDirectory(at: root, includingPropertiesForKeys: nil)) ?? []
        var changed = false
        var saw = false
        var allOff = true
        for url in urls where url.pathExtension == "json" {
            guard var text = try? String(contentsOf: url, encoding: .utf8) else { continue }
            guard text.contains("\"platform_app_id\": \"805550\"") || text.contains("\"platform_app_id\":\"805550\"") else { continue }
            saw = true
            var edited = text
            if edited.contains("\"gamepad_compat_mode\": true") {
                edited = edited.replacingOccurrences(of: "\"gamepad_compat_mode\": true", with: "\"gamepad_compat_mode\": false")
            }
            if edited.contains("\"gamepad_compat_mode\":true") {
                edited = edited.replacingOccurrences(of: "\"gamepad_compat_mode\":true", with: "\"gamepad_compat_mode\":false")
            }
            if edited != text, (try? edited.write(to: url, atomically: true, encoding: .utf8)) != nil {
                changed = true
                text = edited
            }
            if text.contains("\"gamepad_compat_mode\": true") || text.contains("\"gamepad_compat_mode\":true") {
                allOff = false
            }
        }
        return (changed, !saw || allOff)
    }

    nonisolated private static func wineLibDirs() -> [URL] {
        let root = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/com.gamemac.www/wine-engine/containers/wine_installations")
        let installs = (try? FileManager.default.contentsOfDirectory(at: root, includingPropertiesForKeys: [.isDirectoryKey], options: [.skipsHiddenFiles])) ?? []
        return installs
            .map { $0.appendingPathComponent("lib") }
            .filter { FileManager.default.fileExists(atPath: $0.appendingPathComponent("libSDL2-2.0.0.dylib").path)
                || FileManager.default.fileExists(atPath: $0.appendingPathComponent("libSDL2-2.0.0.real.dylib").path) }
    }

    nonisolated private static func installShim(bundled: URL, force: Bool) -> (installed: Bool, replaced: Bool, problem: String?) {
        let libs = wineLibDirs()
        if libs.isEmpty {
            return (false, false, "Бутылка GameHub не найдена. Установи GameHub, один раз запусти ACC, потом нажми Починить.")
        }
        var installed = false
        var replaced = false
        for lib in libs {
            let current = lib.appendingPathComponent("libSDL2-2.0.0.dylib")
            let real = lib.appendingPathComponent("libSDL2-2.0.0.real.dylib")
            let fm = FileManager.default

            if !fm.fileExists(atPath: real.path) {
                guard fm.fileExists(atPath: current.path) else { continue }
                if containsMarker(current) {
                    return (false, false, "Оригинальная SDL потеряна. Переустанови слой совместимости в GameHub или напиши в Issues.")
                }
                do {
                    try fm.copyItem(at: current, to: real)
                    _ = shell("/usr/bin/install_name_tool", ["-id", "@loader_path/libSDL2-2.0.0.real.dylib", real.path])
                } catch {
                    return (false, false, "Не удалось сохранить оригинальную SDL.")
                }
            }

            if containsMarker(current) && !force {
                installed = true
                continue
            }

            do {
                if fm.fileExists(atPath: current.path) {
                    try fm.removeItem(at: current)
                }
                try fm.copyItem(at: bundled, to: current)
                _ = shell("/usr/bin/codesign", ["-s", "-", "--force", current.path])
                if containsMarker(current) {
                    installed = true
                    replaced = true
                }
            } catch {
                return (false, false, "Не удалось поставить отдачу в бутылку GameHub.")
            }
        }
        return (installed, replaced, installed ? nil : "Отдача в бутылку не встала.")
    }

    nonisolated private static func containsMarker(_ url: URL) -> Bool {
        guard let data = try? Data(contentsOf: url) else { return false }
        return data.range(of: Data(tuneMarker.utf8)) != nil
    }

    nonisolated private static func commandList() -> String {
        shell("/bin/ps", ["-ax", "-o", "command="])
    }

    @discardableResult
    nonisolated private static func shell(_ launch: String, _ args: [String]) -> String {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: launch)
        process.arguments = args
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = FileHandle.nullDevice
        do {
            try process.run()
        } catch {
            return ""
        }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        return String(data: data, encoding: .utf8) ?? ""
    }
}

struct PanelView: View {
    @ObservedObject var model: WheelModel
    private let amber = Color(red: 0.93, green: 0.62, blue: 0.22)
    private let paper = Color(red: 0.10, green: 0.12, blue: 0.14)
    private let ink = Color(red: 0.93, green: 0.91, blue: 0.86)

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(alignment: .center, spacing: 12) {
                Circle()
                    .fill(model.ready ? amber : Color(red: 0.55, green: 0.22, blue: 0.18))
                    .frame(width: 10, height: 10)
                    .shadow(color: (model.ready ? amber : .clear).opacity(0.9), radius: 6)
                VStack(alignment: .leading, spacing: 2) {
                    Text(model.headline)
                        .font(.system(size: 16, weight: .semibold))
                        .foregroundStyle(ink)
                    Text(model.detail)
                        .font(.system(size: 12))
                        .foregroundStyle(ink.opacity(0.62))
                }
                Spacer(minLength: 0)
            }

            slider("Сила", value: model.tune.strength, range: 40...120) { model.setStrength($0) }
            slider("Потолок", value: model.tune.ceiling, range: 50...100) { model.setCeiling($0) }

            Toggle(isOn: Binding(get: { model.tune.invert }, set: { model.setInvert($0) })) {
                Text("Инверсия")
                    .foregroundStyle(ink)
            }
            .toggleStyle(.switch)
            .tint(amber)

            Text("Угол поворота 900°")
                .font(.system(size: 12))
                .foregroundStyle(ink.opacity(0.5))

            HStack {
                Button(model.busy ? "Проверяю…" : "Починить") { model.repair() }
                    .buttonStyle(.borderedProminent)
                    .tint(amber)
                    .disabled(model.busy)
                Spacer()
                Button("Закрыть") {
                    NSApplication.shared.terminate(nil)
                }
                .buttonStyle(.borderless)
                .font(.system(size: 12))
                .foregroundStyle(ink.opacity(0.55))
                .keyboardShortcut("q", modifiers: .command)
            }

            if let note = model.note {
                Text(note)
                    .font(.system(size: 12))
                    .foregroundStyle(ink.opacity(0.85))
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(16)
        .frame(width: 292)
        .background(paper)
    }

    private func slider(_ title: String, value: Double, range: ClosedRange<Double>, change: @escaping (Double) -> Void) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(title)
                    .foregroundStyle(ink)
                Spacer()
                Text("\(Int(value))%")
                    .monospacedDigit()
                    .foregroundStyle(amber)
            }
            .font(.system(size: 13, weight: .medium))
            Slider(value: Binding(get: { value }, set: change), in: range, step: 1)
                .tint(amber)
        }
    }
}

@main
struct G920App: App {
    @StateObject private var model = WheelModel()

    var body: some Scene {
        MenuBarExtra {
            PanelView(model: model)
        } label: {
            Image(systemName: "steeringwheel")
        }
        .menuBarExtraStyle(.window)
    }
}
