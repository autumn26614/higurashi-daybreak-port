import AVFoundation
import GameController
import SwiftUI

@main
struct DaybreakDXApp: App {
    var body: some Scene {
        WindowGroup { RootView() }
    }
}

/// The two games a build can hold: the base game (daybreak.exe) and Kai (DaybreakDX.exe,
/// which also reads the base game's archives).
enum GameKind: Int, CaseIterable, Identifiable {
    case base = 0, kai = 1

    var id: Int { rawValue }
    var title: String { self == .base ? "DAYBREAK" : "DAYBREAK KAI" }
    var files: [String] {
        let shared = ["d3dx9_25.dll", "daybreak00.dat", "daybreak01.dat", "daybreak02.dat"]
        return self == .base ? ["daybreak.exe"] + shared
            : ["DaybreakDX.exe"] + shared + ["daybreak04.dat", "daybreak05.dat", "daybreak06.dat"]
    }
    /// This build holds the game's translated code.
    var translated: Bool { recomp_has_game(Int32(rawValue)) != 0 }
}

/// Where the game files go: the app's Documents folder, or a "Daybreak" or "data" folder in
/// it (Files › On My iPhone › Daybreak DX). One folder can hold both games.
enum GameFolder {
    static var documents: URL { FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0] }

    static var candidates: [URL] {
        [documents.appendingPathComponent("Daybreak"), documents.appendingPathComponent("data"), documents]
    }

    static func find(_ game: GameKind) -> URL? {
        candidates.first { recomp_files_present($0.path, Int32(game.rawValue)) != 0 }
    }

    /// The files missing from the folder that holds the most of them.
    static func missing(_ game: GameKind) -> [String] {
        let best = candidates.map { dir in game.files.filter { !exists(dir, $0) } }.min { $0.count < $1.count }
        return best ?? game.files
    }

    private static func exists(_ dir: URL, _ name: String) -> Bool {
        guard let items = try? FileManager.default.contentsOfDirectory(atPath: dir.path) else { return false }
        return items.contains { $0.caseInsensitiveCompare(name) == .orderedSame }
    }
}

struct RootView: View {
    @State private var folders: [GameKind: URL] = [:]
    @State private var playing: GameKind?
    @Environment(\.scenePhase) private var phase

    /// Launch arguments for screenshots of the controls (CI has no game to run).
    private let preview: GameScreen.Preview? = {
        let args = ProcessInfo.processInfo.arguments
        if args.contains("-controlsPanel") { return .panel }
        if args.contains("-controlsPreview") { return .pressed }
        return nil
    }()

    var body: some View {
        Group {
            if let preview {
                GameScreen(folder: nil, game: .base, preview: preview)
            } else if let playing, let folder = folders[playing] {
                GameScreen(folder: folder, game: playing)
            } else {
                setup
            }
        }
        .statusBarHidden(true)
        .persistentSystemOverlays(.hidden)
        .onAppear { refresh() }
        .onChange(of: phase) { p in
            if p == .active && playing == nil { refresh() }
        }
    }

    private func refresh() {
        var found: [GameKind: URL] = [:]
        for g in GameKind.allCases where g.translated { found[g] = GameFolder.find(g) }
        folders = found
    }

    private var games: [GameKind] { GameKind.allCases.filter(\.translated) }

    private var setup: some View {
        ZStack {
            LinearGradient(colors: [Color(red: 0.16, green: 0.02, blue: 0.04), .black], startPoint: .top, endPoint: .bottom)
                .ignoresSafeArea()
            ScrollView {
                VStack(spacing: 22) {
                    SlantTitle(text: "DAYBREAK DX", size: 26)
                        .padding(.top, 8)
                    if games.isEmpty {
                        Text("This build does not contain the translated game code. Build it with recomp/ios/build.sh (or the recomp-ipa workflow) from your copy of daybreak.exe.")
                            .multilineTextAlignment(.center)
                    }
                    ForEach(games) { game in
                        if folders[game] != nil {
                            Button { playing = game } label: { SlantTitle(text: "PLAY  " + game.title, size: 22) }
                                .accessibilityIdentifier(game == .base ? "play" : "play-kai")
                        } else {
                            missingFiles(game)
                        }
                    }
                    if !games.isEmpty && folders.isEmpty {
                        Text("Copy the game's data folder into Files › On My iPhone › Daybreak DX (it may also go in a folder named Daybreak or data there), then come back.")
                            .multilineTextAlignment(.center)
                            .font(.callout)
                    }
                    if !folders.isEmpty {
                        Text("Touch controls appear on screen. Game controllers and keyboards work too; the gear in the corner sets the controls up.")
                            .font(.footnote)
                            .foregroundStyle(.white.opacity(0.6))
                            .multilineTextAlignment(.center)
                    }
                }
                .foregroundStyle(.white)
                .padding(32)
                .frame(maxWidth: 600)
                .frame(maxWidth: .infinity)
            }
        }
    }

    private func missingFiles(_ game: GameKind) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("\(game.title): still missing").font(Theme.title(16)).foregroundStyle(Theme.ember)
            ForEach(GameFolder.missing(game), id: \.self) { Text("• \($0)").font(.body.monospaced()) }
            Button("Check again") { refresh() }
                .buttonStyle(.borderedProminent)
                .tint(Theme.red)
                .padding(.top, 4)
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .overlay(Rectangle().stroke(Color.white.opacity(0.7), lineWidth: 1.5))
        .accessibilityIdentifier("missing")
    }
}

/// The game's frame with the controls over it. The on-screen controls hide while a gamepad is
/// connected (unless the settings say otherwise); the gear opens the control settings.
struct GameScreen: View {
    /// nil: the controls alone, over a stand-in picture (the `-controlsPreview` launch
    /// argument, for screenshots of the controls without the game).
    let folder: URL?
    let game: GameKind
    var preview = Preview.none
    @State private var audio = GameAudio()
    @State private var controller = ControllerInput.gamepadConnected
    @State private var panel = false
    @AppStorage("controls.withController") private var withController = false

    enum Preview { case none, pressed, panel }

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            if let folder {
                MetalGameView(folder: folder, kai: game == .kai).ignoresSafeArea()
            } else {
                LinearGradient(colors: [Color(red: 0.35, green: 0.45, blue: 0.6), Color(red: 0.55, green: 0.45, blue: 0.3)],
                               startPoint: .top, endPoint: .bottom).ignoresSafeArea()
            }
            if !controller || withController || preview != .none {  // (the simulator has a virtual controller)
                TouchControls(demo: preview == .pressed)
            }
            VStack {
                HStack {
                    GearButton { panel = true }
                    Spacer()
                }
                Spacer()
            }
            .padding(.leading, 8)
            .padding(.top, 4)
            ControllerBanner()
            if panel { ControlsPanel(shown: $panel).transition(.opacity) }
        }
        .animation(.easeOut(duration: 0.15), value: panel)
        .defersSystemGestures(on: .all)
        .onAppear {
            UIApplication.shared.isIdleTimerDisabled = true
            ControllerInput.shared.start()
            if folder != nil { audio.start() }
            if preview == .panel { panel = true }
        }
        .onDisappear { UIApplication.shared.isIdleTimerDisabled = false }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidConnect)) { _ in update() }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidDisconnect)) { _ in update() }
    }

    private func update() {
        controller = ControllerInput.gamepadConnected
    }
}

/// A view whose layer is the CAMetalLayer the game draws into.
struct MetalGameView: UIViewRepresentable {
    let folder: URL
    let kai: Bool

    final class LayerView: UIView {
        override class var layerClass: AnyClass { CAMetalLayer.self }
        var started = false
        var folder: URL?
        var kai = false

        override func layoutSubviews() {
            super.layoutSubviews()
            guard let layer = layer as? CAMetalLayer else { return }
            let scale = window?.screen.scale ?? UIScreen.main.scale
            layer.contentsScale = scale
            layer.drawableSize = CGSize(width: bounds.width * scale, height: bounds.height * scale)
            if !started, bounds.width > 0, let folder {
                started = true
                recomp_start(folder.path, Unmanaged.passUnretained(layer).toOpaque(), kai ? 1 : 0)
            }
        }
    }

    func makeUIView(context: Context) -> LayerView {
        let v = LayerView()
        v.folder = folder
        v.kai = kai
        v.backgroundColor = .black
        return v
    }

    func updateUIView(_ uiView: LayerView, context: Context) {}
}

/// Plays the game's mixer through AVAudioEngine, and starts it again after interruptions
/// (calls, Siri, other apps) and when the app returns to the foreground.
final class GameAudio {
    private let engine = AVAudioEngine()
    private let capacity = 8192
    private let buffer: UnsafeMutablePointer<Int16>
    private var observers: [NSObjectProtocol] = []

    init() {
        buffer = .allocate(capacity: capacity * 2)
        buffer.initialize(repeating: 0, count: capacity * 2)
    }

    deinit {
        observers.forEach(NotificationCenter.default.removeObserver)
        buffer.deallocate()
    }

    func start() {
        let session = AVAudioSession.sharedInstance()
        try? session.setCategory(.playback, mode: .default)
        try? session.setActive(true)
        guard let format = AVAudioFormat(standardFormatWithSampleRate: 44100, channels: 2) else { return }
        let buffer = self.buffer, capacity = self.capacity
        // The render callback runs on the audio thread: no allocation, no Swift arrays.
        let node = AVAudioSourceNode(format: format) { _, _, frameCount, audioBufferList -> OSStatus in
            let abl = UnsafeMutableAudioBufferListPointer(audioBufferList)
            var done = 0
            let total = Int(frameCount)
            while done < total {
                let n = min(capacity, total - done)
                recomp_audio(buffer, Int32(n))
                for (ch, buf) in abl.enumerated() {
                    guard let out = buf.mData?.assumingMemoryBound(to: Float.self) else { continue }
                    for i in 0..<n { out[done + i] = Float(buffer[i * 2 + min(ch, 1)]) / 32768 }
                }
                done += n
            }
            return noErr
        }
        engine.attach(node)
        engine.connect(node, to: engine.mainMixerNode, format: format)
        try? engine.start()
        let nc = NotificationCenter.default
        observers.append(nc.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: .main) { [weak self] note in
            let type = (note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt).flatMap(AVAudioSession.InterruptionType.init)
            if type == .ended { self?.resume() }
        })
        observers.append(nc.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
            self?.resume()
        })
        observers.append(nc.addObserver(forName: .AVAudioEngineConfigurationChange, object: engine, queue: .main) { [weak self] _ in
            self?.resume()
        })
    }

    private func resume() {
        try? AVAudioSession.sharedInstance().setActive(true)
        if !engine.isRunning { try? engine.start() }
    }
}
