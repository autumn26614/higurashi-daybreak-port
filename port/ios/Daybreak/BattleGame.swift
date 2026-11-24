import Foundation
import GameController
import simd

/// The battle core (port/src/fighter.h): two fighters on the game's state machine. One
/// `step` is one game frame.
final class BattleGame {
    private let battle: OpaquePointer

    init(_ a: CharacterModel, _ b: CharacterModel, spawns: [SIMD3<Float>]) throws {
        guard spawns.count >= 2 else { throw ReportError.open("The stage has no 1P / 2P spawn points") }
        var s1 = [spawns[0].x, spawns[0].y, spawns[0].z], s2 = [spawns[1].x, spawns[1].y, spawns[1].z]
        var err = [CChar](repeating: 0, count: 256)
        guard let p = a.openBattle(with: b, &s1, &s2, &err) else { throw ReportError.open(message(err)) }
        battle = p
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

    /// nil while both stand.
    var winner: Int? {
        let w = Int(db_battle_winner(battle))
        return w < 0 ? nil : w
    }
}

extension DBFighterState {
    var position: SIMD3<Float> { SIMD3(pos.0, pos.1, pos.2) }
}

/// Controls for 1P: the on-screen stick and buttons, a hardware keyboard (arrows + Z / X / C,
/// the game's defaults) or a game controller. Buttons: 0 jump / boost, 1 melee, 2 shot.
final class BattleControls: ObservableObject {
    /// On-screen stick, -1...1 each axis (y < 0 = up on screen = toward the opponent).
    var stick = SIMD2<Float>(0, 0)
    var touchButtons: UInt32 = 0

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
        }
        if let g = GCController.current?.extendedGamepad {
            let s = SIMD2(g.leftThumbstick.xAxis.value, -g.leftThumbstick.yAxis.value)
            let d = SIMD2(g.dpad.xAxis.value, -g.dpad.yAxis.value)
            if simd_length(s) > simd_length(v) { v = s }
            if simd_length(d) > simd_length(v) { v = d }
            if g.buttonA.isPressed { buttons |= 1 }
            if g.buttonX.isPressed { buttons |= 2 }
            if g.buttonB.isPressed || g.buttonY.isPressed { buttons |= 4 }
        }
        // The game reads directions digitally.
        func axis(_ f: Float) -> Int32 { f < -0.4 ? -1 : f > 0.4 ? 1 : 0 }
        return DBPad(x: axis(v.x), y: axis(v.y), buttons: buttons)
    }
}
