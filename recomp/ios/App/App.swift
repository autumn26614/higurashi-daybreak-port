import AVFoundation
import GameController
import SwiftUI

@main
struct DaybreakRecompApp: App {
    var body: some Scene {
        WindowGroup { RootView() }
    }
}

/// Where the game files go: the app's Documents folder, or a "Daybreak" or "data" folder in
/// it (Files › On My iPhone › Higurashi Daybreak).
enum GameFolder {
    static let files = ["daybreak.exe", "d3dx9_25.dll", "daybreak00.dat", "daybreak01.dat", "daybreak02.dat"]

    static var documents: URL { FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0] }

    static var candidates: [URL] {
        [documents.appendingPathComponent("Daybreak"), documents.appendingPathComponent("data"), documents]
    }

    static func find() -> URL? {
        candidates.first { recomp_files_present($0.path) != 0 }
    }

    /// The files missing from the folder that holds the most of them.
    static func missing() -> [String] {
        let best = candidates.map { dir in files.filter { !exists(dir, $0) } }.min { $0.count < $1.count }
        return best ?? files
    }

    private static func exists(_ dir: URL, _ name: String) -> Bool {
        guard let items = try? FileManager.default.contentsOfDirectory(atPath: dir.path) else { return false }
        return items.contains { $0.caseInsensitiveCompare(name) == .orderedSame }
    }
}

struct RootView: View {
    @State private var folder = GameFolder.find()
    @State private var missing = GameFolder.missing()
    @State private var playing = false
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
                GameScreen(folder: nil, preview: preview)
            } else if playing, let folder {
                GameScreen(folder: folder)
            } else {
                setup
            }
        }
        .statusBarHidden(true)
        .persistentSystemOverlays(.hidden)
        .onChange(of: phase) { p in
            if p == .active && !playing { refresh() }
        }
    }

    private func refresh() {
        folder = GameFolder.find()
        missing = GameFolder.missing()
    }

    private var setup: some View {
        ScrollView {
            VStack(spacing: 16) {
                Text("Higurashi Daybreak").font(.largeTitle.bold())
                if recomp_has_game() == 0 {
                    Text("This build does not contain the translated game code. Build it with recomp/ios/build.sh (or the recomp-ipa workflow) from your copy of daybreak.exe.")
                        .multilineTextAlignment(.center)
                } else if folder == nil {
                    Text("Copy the game's data folder into Files › On My iPhone › Higurashi Daybreak (it may also go in a folder named Daybreak or data there), then come back.")
                        .multilineTextAlignment(.center)
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Still missing:").font(.headline)
                        ForEach(missing, id: \.self) { Text("• \($0)").font(.body.monospaced()) }
                    }
                    .accessibilityIdentifier("missing")
                    Button("Check again") { refresh() }
                        .buttonStyle(.borderedProminent)
                } else {
                    Button("Play") { playing = true }
                        .buttonStyle(.borderedProminent)
                        .font(.title2)
                        .accessibilityIdentifier("play")
                    Text("Touch controls appear on screen. Game controllers and keyboards work too; the gear in the corner sets the controls up.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            }
            .padding(32)
            .frame(maxWidth: 560)
            .frame(maxWidth: .infinity)
        }
    }
}

/// The game's frame with the controls over it. The on-screen controls hide while a gamepad is
/// connected (unless the settings say otherwise); the gear opens the control settings.
struct GameScreen: View {
    /// nil: the controls alone, over a stand-in picture (the `-controlsPreview` launch
    /// argument, for screenshots of the controls without the game).
    let folder: URL?
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
                MetalGameView(folder: folder).ignoresSafeArea()
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

    final class LayerView: UIView {
        override class var layerClass: AnyClass { CAMetalLayer.self }
        var started = false
        var folder: URL?

        override func layoutSubviews() {
            super.layoutSubviews()
            guard let layer = layer as? CAMetalLayer else { return }
            let scale = window?.screen.scale ?? UIScreen.main.scale
            layer.contentsScale = scale
            layer.drawableSize = CGSize(width: bounds.width * scale, height: bounds.height * scale)
            if !started, bounds.width > 0, let folder {
                started = true
                recomp_start(folder.path, Unmanaged.passUnretained(layer).toOpaque())
            }
        }
    }

    func makeUIView(context: Context) -> LayerView {
        let v = LayerView()
        v.folder = folder
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
