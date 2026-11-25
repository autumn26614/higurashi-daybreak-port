import AVFoundation
import SwiftUI

@main
struct DaybreakRecompApp: App {
    var body: some Scene {
        WindowGroup { RootView() }
    }
}

/// Where the game files go: the app's Documents folder, or a "Daybreak" folder inside it
/// (Files > On My iPhone > Higurashi Daybreak).
enum GameFolder {
    static var candidates: [URL] {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        return [docs.appendingPathComponent("Daybreak"), docs.appendingPathComponent("data"), docs]
    }

    static func find() -> URL? {
        candidates.first { recomp_files_present($0.path) != 0 }
    }
}

struct RootView: View {
    @State private var folder = GameFolder.find()
    @State private var playing = false

    var body: some View {
        Group {
            if playing, let folder {
                GameScreen(folder: folder)
            } else {
                setup
            }
        }
        .statusBarHidden(true)
        .persistentSystemOverlays(.hidden)
    }

    private var setup: some View {
        VStack(spacing: 16) {
            Text("Higurashi Daybreak").font(.largeTitle.bold())
            if recomp_has_game() == 0 {
                Text("This build does not contain the translated game code. Build it with recomp/ios/build.sh and your copy of daybreak.exe.")
                    .multilineTextAlignment(.center)
            } else if folder == nil {
                Text("Copy the game's data folder (daybreak.exe, d3dx9_25.dll and daybreak00–02.dat) into Files › On My iPhone › Higurashi Daybreak, then tap Check again.")
                    .multilineTextAlignment(.center)
                Button("Check again") { folder = GameFolder.find() }
                    .buttonStyle(.borderedProminent)
            } else {
                Button("Play") { playing = true }
                    .buttonStyle(.borderedProminent)
                    .font(.title2)
                    .accessibilityIdentifier("play")
            }
        }
        .padding(32)
        .frame(maxWidth: 560)
    }
}

/// The game's frame with the touch controls over it.
struct GameScreen: View {
    let folder: URL
    @State private var audio = GameAudio()

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            MetalGameView(folder: folder).ignoresSafeArea()
            TouchControls()
        }
        .onAppear {
            audio.start()
            ControllerInput.shared.start()
        }
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

/// Plays the game's mixer through AVAudioEngine.
final class GameAudio {
    private let engine = AVAudioEngine()
    private var buffer = [Int16](repeating: 0, count: 4096 * 2)

    func start() {
        try? AVAudioSession.sharedInstance().setCategory(.playback, mode: .default)
        try? AVAudioSession.sharedInstance().setActive(true)
        guard let format = AVAudioFormat(standardFormatWithSampleRate: 44100, channels: 2) else { return }
        let node = AVAudioSourceNode(format: format) { [weak self] _, _, frameCount, audioBufferList -> OSStatus in
            guard let self else { return noErr }
            let frames = Int(frameCount)
            if self.buffer.count < frames * 2 { self.buffer = [Int16](repeating: 0, count: frames * 2) }
            self.buffer.withUnsafeMutableBufferPointer { recomp_audio($0.baseAddress, Int32(frames)) }
            let abl = UnsafeMutableAudioBufferListPointer(audioBufferList)
            for (ch, buf) in abl.enumerated() {
                guard let out = buf.mData?.assumingMemoryBound(to: Float.self) else { continue }
                for i in 0..<frames { out[i] = Float(self.buffer[i * 2 + min(ch, 1)]) / 32768 }
            }
            return noErr
        }
        engine.attach(node)
        engine.connect(node, to: engine.mainMixerNode, format: format)
        try? engine.start()
    }
}
