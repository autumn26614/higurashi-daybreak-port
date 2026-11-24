import Foundation
import GameController
import simd

/// The battle core (port/src/fighter.h): two fighters on the game's state machine. One
/// `step` is one game frame.
final class BattleGame {
    private let battle: OpaquePointer
    private let models: [CharacterModel]  // the battle reads their skeletons: keep them alive

    /// Floor triangles of the stage's collision model (0: flat floor at the spawn heights).
    private(set) var floorTriangles = 0

    init(_ a: CharacterModel, _ b: CharacterModel, spawns: [SIMD3<Float>], stage: Int) throws {
        guard spawns.count >= 2 else { throw ReportError.open("The stage has no 1P / 2P spawn points") }
        var s1 = [spawns[0].x, spawns[0].y, spawns[0].z], s2 = [spawns[1].x, spawns[1].y, spawns[1].z]
        var err = [CChar](repeating: 0, count: 256)
        guard let p = a.openBattle(with: b, &s1, &s2, &err) else { throw ReportError.open(message(err)) }
        battle = p
        models = [a, b]
        floorTriangles = a.setBattleStage(p, stage: stage)
    }

    deinit { db_battle_free(battle) }

    /// nil pad: computer controlled.
    func step(p1: DBPad?, p2: DBPad?) {
        var a = p1 ?? DBPad(), b = p2 ?? DBPad()
        withUnsafePointer(to: &a) { pa in
            withUnsafePointer(to: &b) { pb in
                db_battle_step(battle, p1 == nil ? nil : pa, p2 == nil ? nil : pb)
            }
        }
    }

    func fighter(_ i: Int) -> DBFighterState {
        var s = DBFighterState()
        db_battle_fighter(battle, Int32(i), &s)
        return s
    }

    /// The game's battle camera for fighter i (0x5B1400).
    func camera(_ i: Int) -> StageCamera? {
        var c = DBCamera()
        return db_battle_camera(battle, Int32(i), &c) == 1 ? StageCamera(c) : nil
    }

    /// Bullets in flight.
    func bullets() -> [DBBulletState] {
        (0..<db_battle_bullet_count(battle)).map { i in
            var s = DBBulletState()
            db_battle_bullet(battle, i, &s)
            return s
        }
    }

    /// The archive entry of a bullet's model ("data/bullet/001/03.X"), or nil.
    func bulletModel(owner: Int, slot: Int) -> String? {
        var buf = [CChar](repeating: 0, count: 128)
        let n = db_battle_bullet_model(battle, Int32(owner), Int32(slot), &buf, Int32(buf.count))
        return n > 0 ? String(cString: buf) : nil
    }

    /// Voices and sound effects the fighters started since the last call.
    func takeSounds() -> [DBSoundEvent] {
        var buf = [DBSoundEvent](repeating: DBSoundEvent(), count: 32)
        let n = Int(db_battle_take_sounds(battle, &buf, Int32(buf.count)))
        return Array(buf.prefix(n))
    }

    /// nil while both stand.
    var winner: Int? {
        let w = Int(db_battle_winner(battle))
        return w < 0 ? nil : w
    }
}

extension DBFighterState {
    var position: SIMD3<Float> { SIMD3(pos.0, pos.1, pos.2) }
}

/// Controls for 1P: the on-screen stick and buttons, a hardware keyboard (the game's defaults:
/// arrows, Z jump, X melee, C shot, S = X + C, D = Z + X guard) or a game controller (A jump,
/// X melee, B / Y shot, R1 guard). Buttons: 0 jump / boost, 1 melee, 2 shot.
final class BattleControls: ObservableObject {
    /// On-screen stick, -1...1 each axis (y < 0 = up on screen = toward the opponent).
    var stick = SIMD2<Float>(0, 0)
    private var held: [String: UInt32] = [:]  // on-screen buttons down, by name

    /// Buttons held on screen (a button can stand for several, like the game's macros).
    var touchButtons: UInt32 { held.values.reduce(0, |) }

    func set(_ name: String, mask: UInt32, down: Bool) {
        held[name] = down ? mask : nil
    }

    func pad() -> DBPad {
        var v = stick
        var buttons = touchButtons
        if let k = GCKeyboard.coalesced?.keyboardInput {
            func down(_ c: GCKeyCode) -> Bool { k.button(forKeyCode: c)?.isPressed ?? false }
            if down(.leftArrow) { v.x = -1 }
            if down(.rightArrow) { v.x = 1 }
            if down(.upArrow) { v.y = -1 }
            if down(.downArrow) { v.y = 1 }
            if down(.keyZ) { buttons |= 1 }
            if down(.keyX) { buttons |= 2 }
            if down(.keyC) { buttons |= 4 }
            if down(.keyS) { buttons |= 2 | 4 }  // macro b4 = b1 + b2
            if down(.keyD) { buttons |= 1 | 2 }  // macro b5 = b0 + b1 (guard)
        }
        if let g = GCController.current?.extendedGamepad {
            let s = SIMD2(g.leftThumbstick.xAxis.value, -g.leftThumbstick.yAxis.value)
            let d = SIMD2(g.dpad.xAxis.value, -g.dpad.yAxis.value)
            if simd_length(s) > simd_length(v) { v = s }
            if simd_length(d) > simd_length(v) { v = d }
            if g.buttonA.isPressed { buttons |= 1 }
            if g.buttonX.isPressed { buttons |= 2 }
            if g.buttonB.isPressed || g.buttonY.isPressed { buttons |= 4 }
            if g.rightShoulder.isPressed { buttons |= 1 | 2 }  // guard
        }
        // The game reads directions digitally.
        func axis(_ f: Float) -> Int32 { f < -0.4 ? -1 : f > 0.4 ? 1 : 0 }
        return DBPad(x: axis(v.x), y: axis(v.y), buttons: buttons)
    }
}

/// Plays the battle's voices (one player per fighter: a new line cuts the last) and sound
/// effects (a few players in turn). Decoded clips are cached; a voice category has variants
/// 000, 001, ... and one is picked at random, as the game does (0x43A8F0).
final class BattleSounds {
    private let models: [CharacterModel]
    private let voiceDirs: [String]
    private var cache: [String: DecodedSound] = [:]
    private var missing: Set<String> = []
    private var variants: [String: Int] = [:]
    private let voices = [SoundPlayer(), SoundPlayer()]
    private let effects = [SoundPlayer(), SoundPlayer(), SoundPlayer()]
    private var nextEffect = 0

    init(models: [CharacterModel]) {
        self.models = models
        voiceDirs = models.map { BattleSounds.voiceDir(model: $0.name) }
    }

    /// data/rena/00.X and data/rena_B/00.X -> data/voice/rena
    static func voiceDir(model: String) -> String {
        let parts = model.split(separator: "/")
        var name = parts.count >= 2 ? String(parts[parts.count - 2]) : model
        if name.lowercased().hasSuffix("_b") { name.removeLast(2) }
        return "data/voice/" + name
    }

    private func load(_ entry: String) -> DecodedSound? {
        if let s = cache[entry] { return s }
        if missing.contains(entry) { return nil }
        guard let s = models.first?.sound(entry) else {
            missing.insert(entry)
            return nil
        }
        cache[entry] = s
        return s
    }

    private func voice(fighter: Int, category: String) -> DecodedSound? {
        let dir = voiceDirs[fighter] + "/" + category + "/"
        let count: Int
        if let c = variants[dir] {
            count = c
        } else {
            var c = 0
            while c < 16, load(dir + String(format: "%03d.wav", c)) != nil { c += 1 }
            variants[dir] = c
            count = c
        }
        guard count > 0 else { return nil }
        return load(dir + String(format: "%03d.wav", Int.random(in: 0..<count)))
    }

    func play(_ events: [DBSoundEvent]) {
        for e in events {
            if e.kind == 0 {
                guard e.fighter >= 0, Int(e.fighter) < voiceDirs.count, let name = db_voice_category(e.id),
                      let s = voice(fighter: Int(e.fighter), category: String(cString: name)) else { continue }
                voices[Int(e.fighter)].play(s)
            } else if let s = load(String(format: "data/se/%03d.wav", Int(e.id))) {
                effects[nextEffect].play(s)
                nextEffect = (nextEffect + 1) % effects.count
            }
        }
    }

    func stop() {
        (voices + effects).forEach { $0.stop() }
    }
}
