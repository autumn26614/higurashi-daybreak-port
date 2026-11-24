import SwiftUI
import MetalKit
import simd

/// A stage with two characters on the 1P and 2P spawn points (env.dat). "Fight" runs the
/// battle core (BattleGame: the game's state machine for movement, dashes, jumps and melee)
/// with 1P on the on-screen controls, a keyboard or a controller and 2P on the computer;
/// "Pose" shows both playing one animation set with a free camera.
struct BattleView: View {
    let urls: [URL]
    @State private var stages: [Int] = []
    @State private var models: [[UInt8]] = []
    @State private var stage = 1
    @State private var p1 = 0
    @State private var p2 = 1
    @State private var anim = 0  // the game's numbering: 0 is the idle stance
    @State private var fight = true
    @State private var scene: StageScene?
    @State private var fighters: [CharacterModel] = []
    @State private var game: BattleGame?
    @State private var hud: [DBFighterState] = []
    @State private var winner: Int?
    @State private var renderer: StageRenderer?
    @State private var orbit: OrbitCamera?
    @State private var base: StageCamera?
    @State private var follow: StageCamera?
    @State private var started = Date()
    @State private var loading = false
    @State private var errorText: String?
    @StateObject private var controls = BattleControls()
    @State private var music = SoundPlayer()
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
            controlsBar
            ZStack {
                Color.black
                if let renderer {
                    if fight, game != nil {
                        BattleMetalView(renderer: renderer)
                    } else if let orbit, let base {
                        MetalStageView(renderer: renderer, orbit: Binding(get: { orbit }, set: { self.orbit = $0 }), base: base)
                    }
                }
                if fight, game != nil {
                    BattleHUD(states: hud, names: fighters.map { label(Array($0.name.utf8)) })
                    TouchControls(controls: controls)
                    if let winner {
                        VStack(spacing: 12) {
                            Text(winner == 0 ? "1P WINS" : "2P WINS").font(.largeTitle.bold()).foregroundStyle(.white)
                            Button("Rematch") { restart() }.buttonStyle(.borderedProminent)
                        }
                        .padding(24).background(.black.opacity(0.6), in: RoundedRectangle(cornerRadius: 12))
                        .accessibilityIdentifier("battleResult")
                    }
                }
                if loading { ProgressView().tint(.white) }
                if let errorText { Text(errorText).foregroundStyle(.red).padding().accessibilityIdentifier("error") }
                if renderer == nil && scene != nil {
                    Text("The battle needs Metal").foregroundStyle(.white)
                }
            }
            if let scene {
                Text(statusLine(scene))
                    .font(.footnote.monospacedDigit()).padding(6)
                    .accessibilityIdentifier("battleStats")
            }
        }
        .navigationTitle("Battle")
        .navigationBarTitleDisplayMode(.inline)
        .task { await start() }
        .onChange(of: stage) { _ in Task { await load() } }
        .onChange(of: p1) { _ in Task { await load() } }
        .onChange(of: p2) { _ in Task { await load() } }
        .onChange(of: anim) { _ in started = Date() }
        .onChange(of: fight) { on in
            if on {
                restart()
            } else {
                renderer?.camera = base
                music.stop()
            }
        }
        .onReceive(tick) { _ in frame() }
        .onDisappear { music.stop() }
    }

    private func statusLine(_ s: StageScene) -> String {
        let stage = "Stage \(String(format: "%02d", s.stage))"
        if fight, hud.count == 2 {
            return "\(stage) · 1P state \(hud[0].state) · 2P state \(hud[1].state)"
        }
        return "\(stage) · \(fighters.count) characters · animation \(anim)"
    }

    private var controlsBar: some View {
        VStack(spacing: 4) {
            HStack {
                Picker("Mode", selection: $fight) {
                    Text("Fight").tag(true)
                    Text("Pose").tag(false)
                }
                .pickerStyle(.segmented)
                .frame(maxWidth: 180)
                .accessibilityIdentifier("battleMode")
                Picker("Stage", selection: $stage) {
                    ForEach(stages, id: \.self) { Text(String(format: "Stage %02d", $0)).tag($0) }
                }
                if !fight {
                    Picker("Animation", selection: $anim) {
                        ForEach(0..<min(fighters.map { $0.animations.count }.min() ?? 0, 200), id: \.self) {
                            Text("Animation \($0)").tag($0)
                        }
                    }
                    .accessibilityIdentifier("battleAnim")
                }
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
        game = nil
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
            restart()
        case .failure(let e):
            errorText = e.localizedDescription
        }
    }

    private func restart() {
        guard let scene, fighters.count == 2 else { return }
        winner = nil
        follow = nil
        do {
            game = try BattleGame(fighters[0], fighters[1], spawns: scene.spawns, stage: scene.stage)
        } catch {
            game = nil
            if fight { errorText = error.localizedDescription }
        }
        frame()
        playBattleMusic()
    }

    /// The battle BGM, data/bgm/%02d.ogg (0x5B436B). In versus the game picks it at random or
    /// from the menu among 01-05, 11-15 and 21; here the stage's own number, else 21.
    static func musicEntries(stage: Int) -> [String] {
        [stage, 21, 1].map { String(format: "data/bgm/%02d.ogg", $0) }
    }

    private func playBattleMusic() {
        guard fight, let model = fighters.first, let stage = scene?.stage else { return }
        let player = music
        Task.detached {
            let song = Self.musicEntries(stage: stage).lazy.compactMap { model.sound($0) }.first
            await MainActor.run {
                if let song { player.play(song) } else { player.stop() }
            }
        }
    }

    /// win.ogg when 1P wins, lose.ogg otherwise (0x5B4C46 / 0x5B4BA6).
    private func playResultMusic(_ winner: Int) {
        guard let model = fighters.first else { return }
        let player = music
        let entry = winner == 0 ? "data/bgm/win.ogg" : "data/bgm/lose.ogg"
        Task.detached {
            let song = model.sound(entry)
            await MainActor.run {
                if let song { player.play(song) } else { player.stop() }
            }
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

    private func frame() {
        guard let scene, let renderer, fighters.count == 2, scene.spawns.count >= 2 else { return }
        if fight, let game {
            if winner == nil { game.step(p1: controls.pad(), p2: nil) }
            let states = [game.fighter(0), game.fighter(1)]
            hud = states
            if winner == nil, let w = game.winner {
                winner = w
                playResultMusic(w)
            }
            var batches: [StageBatch] = []
            for (f, s) in zip(fighters, states) {
                let a = Int(s.anim) < f.animations.count ? Int(s.anim) : -1
                batches += f.pose(anim: a, seconds: s.anim_seconds, at: s.position, yaw: s.yaw)
            }
            renderer.setDynamicBatches(batches)
            follow = game.camera(0)
            renderer.camera = follow
            return
        }
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

/// The Metal view without camera gestures (the battle drives the camera).
struct BattleMetalView: UIViewRepresentable {
    let renderer: StageRenderer

    func makeUIView(context: Context) -> MTKView {
        let v = MTKView(frame: .zero, device: renderer.device)
        v.colorPixelFormat = StageRenderer.colorFormat
        v.depthStencilPixelFormat = StageRenderer.depthFormat
        v.delegate = renderer
        v.preferredFramesPerSecond = 60
        v.isPaused = false
        v.enableSetNeedsDisplay = false
        v.accessibilityIdentifier = "battleMetalView"
        return v
    }

    func updateUIView(_ v: MTKView, context: Context) {}
}

/// HP (with the red drain bar that follows it down) and stamina for both fighters.
struct BattleHUD: View {
    let states: [DBFighterState]
    let names: [String]

    var body: some View {
        VStack {
            HStack(alignment: .top, spacing: 24) {
                ForEach(Array(states.enumerated()), id: \.offset) { i, s in
                    VStack(alignment: i == 0 ? .leading : .trailing, spacing: 3) {
                        Text((i == 0 ? "1P " : "2P ") + (i < names.count ? names[i] : ""))
                            .font(.caption.bold()).foregroundStyle(.white)
                        bar(Double(s.hp) / 10000, drain: Double(s.hp_bar) / 10000, colour: .green, height: 10,
                            leading: i == 0)
                        bar(Double(s.stamina) / 1000, drain: nil, colour: .cyan, height: 5, leading: i == 0)
                    }
                    .accessibilityElement(children: .ignore)
                    .accessibilityLabel("\(i == 0 ? "1P" : "2P") HP \(s.hp)")
                    .accessibilityIdentifier(i == 0 ? "hud1P" : "hud2P")
                }
            }
            .padding(10)
            Spacer()
        }
        .allowsHitTesting(false)
    }

    private func bar(_ v: Double, drain: Double?, colour: Color, height: CGFloat, leading: Bool) -> some View {
        GeometryReader { g in
            ZStack(alignment: leading ? .leading : .trailing) {
                Rectangle().fill(.black.opacity(0.55))
                if let drain { Rectangle().fill(.red).frame(width: g.size.width * max(0, min(1, drain))) }
                Rectangle().fill(colour).frame(width: g.size.width * max(0, min(1, v)))
            }
        }
        .frame(height: height)
        .overlay(Rectangle().stroke(.white.opacity(0.7), lineWidth: 1))
    }
}

/// On-screen stick (left) and Jump / Melee / Shot buttons (right). Double-tap the stick
/// for a dash; hold Jump to keep rising.
struct TouchControls: View {
    @ObservedObject var controls: BattleControls
    @State private var knob = CGSize.zero

    var body: some View {
        VStack {
            Spacer()
            HStack(alignment: .bottom) {
                stick
                Spacer()
                VStack(spacing: 14) {
                    button("Shot", bit: 2)
                    HStack(spacing: 14) {
                        button("Melee", bit: 1)
                        button("Jump", bit: 0)
                    }
                }
            }
            .padding(24)
        }
    }

    private var stick: some View {
        let radius: CGFloat = 56
        return ZStack {
            Circle().fill(.white.opacity(0.12)).frame(width: radius * 2, height: radius * 2)
            Circle().stroke(.white.opacity(0.4), lineWidth: 2).frame(width: radius * 2, height: radius * 2)
            Circle().fill(.white.opacity(0.45)).frame(width: 48, height: 48).offset(knob)
        }
        .contentShape(Circle())
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { g in
                    var d = g.translation
                    let len = sqrt(d.width * d.width + d.height * d.height)
                    if len > radius { d = CGSize(width: d.width / len * radius, height: d.height / len * radius) }
                    knob = d
                    controls.stick = SIMD2(Float(d.width / radius), Float(d.height / radius))
                }
                .onEnded { _ in
                    knob = .zero
                    controls.stick = .zero
                }
        )
        .accessibilityIdentifier("stick")
    }

    private func button(_ title: String, bit: UInt32) -> some View {
        ButtonPad(title: title) { down in
            if down { controls.touchButtons |= 1 << bit } else { controls.touchButtons &= ~(1 << bit) }
        }
    }
}

private struct ButtonPad: View {
    let title: String
    let changed: (Bool) -> Void
    @State private var down = false

    var body: some View {
        Text(title)
            .font(.caption.bold())
            .foregroundStyle(.white)
            .frame(width: 64, height: 64)
            .background(Circle().fill(.white.opacity(down ? 0.45 : 0.18)))
            .overlay(Circle().stroke(.white.opacity(0.5), lineWidth: 2))
            .contentShape(Circle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { _ in
                        if !down {
                            down = true
                            changed(true)
                        }
                    }
                    .onEnded { _ in
                        down = false
                        changed(false)
                    }
            )
            .accessibilityIdentifier("button" + title)
    }
}
