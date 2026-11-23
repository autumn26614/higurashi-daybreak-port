import SwiftUI
import simd

/// A battle set-up without gameplay: a stage with two characters standing on the 1P and 2P
/// spawn points (env.dat), facing each other and playing one animation set, drawn with
/// Metal (stage passes, then the characters with their toon shading and outlines).
struct BattleView: View {
    let urls: [URL]
    @State private var stages: [Int] = []
    @State private var models: [[UInt8]] = []
    @State private var stage = 1
    @State private var p1 = 0
    @State private var p2 = 1
    @State private var anim = 2
    @State private var scene: StageScene?
    @State private var fighters: [CharacterModel] = []
    @State private var renderer: StageRenderer?
    @State private var orbit: OrbitCamera?
    @State private var base: StageCamera?
    @State private var started = Date()
    @State private var loading = false
    @State private var errorText: String?
    private let tick = Timer.publish(every: 1.0 / 60, on: .main, in: .common).autoconnect()

    /// Character models only: data/<name>/0N.X outside the item / effect folders.
    static func isCharacter(_ name: [UInt8]) -> Bool {
        let s = String(decoding: name, as: UTF8.self).lowercased()
        return !["data/item/", "data/effect/", "data/aura/", "data/camera/"].contains { s.hasPrefix($0) }
    }

    private func label(_ n: [UInt8]) -> String {
        String(String(decoding: n, as: UTF8.self).dropFirst(5))  // drop "data/"
    }

    var body: some View {
        VStack(spacing: 0) {
            controls
            ZStack {
                Color.black
                if let renderer, let orbit, let base {
                    MetalStageView(renderer: renderer, orbit: Binding(get: { orbit }, set: { self.orbit = $0 }), base: base)
                }
                if loading { ProgressView().tint(.white) }
                if let errorText { Text(errorText).foregroundStyle(.red).padding().accessibilityIdentifier("error") }
                if renderer == nil && scene != nil {
                    Text("The battle preview needs Metal").foregroundStyle(.white)
                }
            }
            if let scene {
                Text("Stage \(String(format: "%02d", scene.stage)) · \(fighters.count) characters · Anim-\(anim + 1)")
                    .font(.footnote.monospacedDigit()).padding(6)
                    .accessibilityIdentifier("battleStats")
            }
        }
        .navigationTitle("Battle preview")
        .navigationBarTitleDisplayMode(.inline)
        .task { await start() }
        .onChange(of: stage) { _ in Task { await load() } }
        .onChange(of: p1) { _ in Task { await load() } }
        .onChange(of: p2) { _ in Task { await load() } }
        .onChange(of: anim) { _ in started = Date() }
        .onReceive(tick) { _ in pose() }
    }

    private var controls: some View {
        VStack(spacing: 4) {
            HStack {
                Picker("Stage", selection: $stage) {
                    ForEach(stages, id: \.self) { Text(String(format: "Stage %02d", $0)).tag($0) }
                }
                Picker("Animation", selection: $anim) {
                    ForEach(0..<min(fighters.map { $0.animations.count }.min() ?? 0, 200), id: \.self) {
                        Text("Anim-\($0 + 1)").tag($0)
                    }
                }
                .accessibilityIdentifier("battleAnim")
            }
            HStack {
                Picker("1P", selection: $p1) {
                    ForEach(Array(models.enumerated()), id: \.offset) { i, n in Text("1P " + label(n)).tag(i) }
                }
                Picker("2P", selection: $p2) {
                    ForEach(Array(models.enumerated()), id: \.offset) { i, n in Text("2P " + label(n)).tag(i) }
                }
            }
        }
        .padding(.horizontal)
        .disabled(loading)
    }

    private func start() async {
        let urls = urls
        let (s, m) = await Task.detached {
            (StageScene.availableStages(urls: urls), CharacterModel.list(urls: urls).filter(BattleView.isCharacter))
        }.value
        stages = s
        models = m
        if let first = s.first, !s.contains(stage) { stage = first }
        if m.count < 2 { p2 = 0 }
        renderer = try? StageRenderer()
        await load()
    }

    private func load() async {
        guard !stages.isEmpty, !models.isEmpty else {
            errorText = stages.isEmpty ? "No stages in these archives" : "No character models in these archives"
            return
        }
        loading = true
        errorText = nil
        let urls = urls, stage = stage
        let picks = [models[min(p1, models.count - 1)], models[min(p2, models.count - 1)]]
        let result = await Task.detached { () -> Result<(StageScene, [CharacterModel]), Error> in
            Result { (try StageScene(urls: urls, stage: stage), try picks.map { try CharacterModel(urls: urls, entry: $0) }) }
        }.value
        loading = false
        switch result {
        case .success(let (s, f)):
            scene = s
            fighters = f
            renderer?.camera = nil
            renderer?.load(s)
            for c in f { renderer?.addTextures(c.textures) }
            let cam = Self.battleCamera(s)
            base = cam
            orbit = OrbitCamera(cam)
            renderer?.camera = cam
            started = Date()
            pose()
        case .failure(let e):
            errorText = e.localizedDescription
        }
    }

    /// Looks across the line between 1P and 2P from the side, a little above.
    static func battleCamera(_ s: StageScene) -> StageCamera {
        let a = s.spawns[0], b = s.spawns[1]
        let mid = (a + b) / 2 + SIMD3(0, 90, 0)
        let along = b - a
        let sep = max(simd_length(SIMD2(along.x, along.z)), 300)
        var side = SIMD3<Float>(-along.z, 0, along.x)  // perpendicular in x/z
        side = simd_length(side) > 0 ? simd_normalize(side) : SIMD3(0, 0, -1)
        if side.z > 0 { side = -side }  // stand on the -z side, as the default view does
        return StageCamera(eye: mid + side * sep * 1.2 + SIMD3(0, sep * 0.35, 0), target: mid)
    }

    private func pose() {
        guard let scene, let renderer, fighters.count == 2, scene.spawns.count >= 2 else { return }
        let t = Date().timeIntervalSince(started)
        var batches: [StageBatch] = []
        for (i, f) in fighters.enumerated() {
            let at = scene.spawns[i], other = scene.spawns[1 - i]
            let a = anim < f.animations.count ? anim : -1
            batches += f.pose(anim: a, seconds: t, at: at, facing: other)
        }
        renderer.setDynamicBatches(batches)
    }
}
