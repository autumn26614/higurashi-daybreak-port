import SwiftUI

/// Every character (and item / effect) model in the archives.
struct CharacterList: View {
    let urls: [URL]
    @State private var names: [[UInt8]]?

    var body: some View {
        List {
            if let names {
                if names.isEmpty { Text("No models in these archives").accessibilityIdentifier("noModels") }
                ForEach(Array(names.enumerated()), id: \.offset) { i, n in
                    NavigationLink(String(data: Data(n), encoding: StageReport.cp932) ?? "?") {
                        CharacterView(urls: urls, entry: n)
                    }
                    .accessibilityIdentifier("model.\(i)")
                }
            } else {
                ProgressView()
            }
        }
        .navigationTitle("Characters")
        .task {
            let urls = urls
            names = await Task.detached { CharacterModel.list(urls: urls) }.value
        }
    }
}

/// One model, animated at 60 fps with the game's animation sets, orbit camera.
struct CharacterView: View {
    let urls: [URL]
    let entry: [UInt8]
    @State private var model: CharacterModel?
    @State private var renderer: StageRenderer?
    @State private var orbit: OrbitCamera?
    @State private var base: StageCamera?
    @State private var anim = -1  // bind pose
    @State private var playing = true
    @State private var started = Date()
    @State private var paused: Double = 0
    @State private var cpuImage: CGImage?
    @State private var errorText: String?
    private let tick = Timer.publish(every: 1.0 / 60, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(spacing: 0) {
            if let model {
                HStack {
                    Picker("Animation", selection: $anim) {
                        Text("Bind pose").tag(-1)
                        ForEach(Array(model.animations.enumerated()), id: \.offset) { i, a in
                            // The game's animation number (D3DX order), then the name in the file.
                            Text("\(i): \(a.name) (\(String(format: "%.1f", a.seconds)) s)").tag(i)
                        }
                    }
                    .accessibilityIdentifier("animPicker")
                    Button(playing ? "Pause" : "Play") {
                        if playing { paused = Date().timeIntervalSince(started) } else { started = Date().addingTimeInterval(-paused) }
                        playing.toggle()
                    }
                    Button("Reset") { if let base { orbit = OrbitCamera(base) } }
                }
                .padding(.horizontal)
            }
            ZStack {
                Color.black
                if let renderer, let orbit, let base {
                    MetalStageView(renderer: renderer, orbit: Binding(get: { orbit }, set: { self.orbit = $0 }), base: base)
                } else if let cpuImage {
                    Image(decorative: cpuImage, scale: 1).resizable().aspectRatio(contentMode: .fit)
                }
                if model == nil && errorText == nil { ProgressView().tint(.white) }
                if let errorText { Text(errorText).foregroundStyle(.red).padding().accessibilityIdentifier("error") }
            }
            if let model {
                Text("\(model.animations.count) animations · \(model.textures.count) textures · " +
                     (renderer != nil ? "Metal" : "CPU reference") + " · approximate shading")
                    .font(.footnote).padding(6)
                    .accessibilityIdentifier("modelStats")
            }
        }
        .navigationTitle(String(data: Data(entry), encoding: StageReport.cp932) ?? "")
        .navigationBarTitleDisplayMode(.inline)
        .task { await load() }
        .onChange(of: anim) { _ in
            started = Date()
            paused = 0
            update()
        }
        .onReceive(tick) { _ in if playing && anim >= 0 { update() } }
    }

    private var seconds: Double { playing ? Date().timeIntervalSince(started) : paused }

    private func load() async {
        let urls = urls, entry = entry
        let result = await Task.detached { Result { try CharacterModel(urls: urls, entry: entry) } }.value
        switch result {
        case .success(let m):
            model = m
            renderer = try? StageRenderer()
            renderer?.setTextures(m.textures)
            let p = m.pose(anim: -1, seconds: 0)
            base = p.camera
            if let cam = p.camera { orbit = OrbitCamera(cam); renderer?.camera = cam }
            renderer?.setBatches(p.batches)
            if renderer == nil { renderCPU() }
        case .failure(let e):
            errorText = e.localizedDescription
        }
    }

    private func update() {
        guard let model else { return }
        let p = model.pose(anim: anim, seconds: seconds)
        renderer?.setBatches(p.batches)
        if renderer == nil && !playing { renderCPU() }
    }

    private func renderCPU() {
        guard let model, let orbit, let base else { return }
        let (w, h) = (360, 480)
        let rgba = model.renderReference(anim: anim, seconds: seconds, camera: orbit.camera(from: base), width: w, height: h)
        cpuImage = TalkBook.cgImage(rgba, width: w, height: h)
    }
}
