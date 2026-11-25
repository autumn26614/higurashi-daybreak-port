import GameController
import UIKit

/// DirectInput key codes the game reads. Its default layout (the base game and Kai alike):
/// the arrows, and eight buttons, Z X C A S D Q W.
enum DIK {
    static let escape: UInt32 = 0x01, enter: UInt32 = 0x1C, space: UInt32 = 0x39
    static let z: UInt32 = 0x2C, x: UInt32 = 0x2D, c: UInt32 = 0x2E
    static let a: UInt32 = 0x1E, s: UInt32 = 0x1F, d: UInt32 = 0x20
    static let q: UInt32 = 0x10, w: UInt32 = 0x11
    static let up: UInt32 = 0xC8, down: UInt32 = 0xD0, left: UInt32 = 0xCB, right: UInt32 = 0xCD
}

/// The game's buttons, what they do, and where they sit on a controller. In battle: button 0
/// jumps (and boosts), 1 is melee, 2 shoots (hold to charge), S is the special (melee + shot)
/// and D guards (jump + melee). In menus Z confirms and X goes back.
struct GameAction: Identifiable {
    let id: String
    let name: String
    let key: UInt32
    let keyLabel: String
    let padLabel: String
    let pad: (GCExtendedGamepad) -> GCControllerButtonInput?

    static let all: [GameAction] = [
        GameAction(id: "jump", name: "Jump · Confirm", key: DIK.z, keyLabel: "Z", padLabel: "A") { $0.buttonA },
        GameAction(id: "melee", name: "Melee · Back", key: DIK.x, keyLabel: "X", padLabel: "X") { $0.buttonX },
        GameAction(id: "shot", name: "Shot (hold to charge)", key: DIK.c, keyLabel: "C", padLabel: "B") { $0.buttonB },
        GameAction(id: "special", name: "Special", key: DIK.s, keyLabel: "S", padLabel: "L1") { $0.leftShoulder },
        GameAction(id: "guard", name: "Guard", key: DIK.d, keyLabel: "D", padLabel: "R1") { $0.rightShoulder },
        GameAction(id: "a", name: "Button A", key: DIK.a, keyLabel: "A", padLabel: "Y") { $0.buttonY },
        GameAction(id: "q", name: "Button Q", key: DIK.q, keyLabel: "Q", padLabel: "L2") { $0.leftTrigger },
        GameAction(id: "w", name: "Button W", key: DIK.w, keyLabel: "W", padLabel: "R2") { $0.rightTrigger },
        GameAction(id: "start", name: "Start", key: DIK.enter, keyLabel: "Enter", padLabel: "Menu") { $0.buttonMenu },
        GameAction(id: "esc", name: "Esc", key: DIK.escape, keyLabel: "Esc", padLabel: "Options") { $0.buttonOptions },
    ]

    static func named(_ id: String) -> GameAction { all.first { $0.id == id }! }
}

/// Every input source (the touch controls, each controller, a keyboard) holds its own set of
/// keys; the game sees their union, so one source letting go of a key does not release it
/// while another still holds it.
final class InputHub {
    static let shared = InputHub()
    private var sources: [String: Set<UInt32>] = [:]
    private var held = Set<UInt32>()
    private let lock = NSLock()

    func set(_ source: String, _ keys: Set<UInt32>) {
        lock.lock()
        defer { lock.unlock() }
        sources[source] = keys.isEmpty ? nil : keys
        let now = sources.values.reduce(into: Set<UInt32>()) { $0.formUnion($1) }
        for k in held.subtracting(now) { recomp_key(k, 0) }
        for k in now.subtracting(held) { recomp_key(k, 1) }
        held = now
    }
}

/// Game controllers (Xbox, PlayStation, MFi, Switch) and hardware keyboards, as the game's
/// keyboard keys (its default controls do not use a joystick). Several controllers work at
/// once; a PlayStation controller's light turns the game's red.
final class ControllerInput {
    static let shared = ControllerInput()
    private var started = false
    private var keyboardHeld = Set<UInt32>()

    func start() {
        guard !started else { return }
        started = true
        let nc = NotificationCenter.default
        nc.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] note in
            if let c = note.object as? GCController { self?.attach(c) }
        }
        nc.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { note in
            if let c = note.object as? GCController { InputHub.shared.set(ControllerInput.source(c), []) }
        }
        nc.addObserver(forName: .GCKeyboardDidConnect, object: nil, queue: .main) { [weak self] note in
            if let k = note.object as? GCKeyboard { self?.attach(k) }
        }
        nc.addObserver(forName: .GCKeyboardDidDisconnect, object: nil, queue: .main) { [weak self] _ in
            self?.keyboardHeld = []
            InputHub.shared.set("keyboard", [])
        }
        GCController.controllers().forEach(attach)
        if let k = GCKeyboard.coalesced { attach(k) }
        GCController.startWirelessControllerDiscovery {}
    }

    static var gamepadConnected: Bool { GCController.controllers().contains { $0.extendedGamepad != nil } }

    private static func source(_ c: GCController) -> String { "pad-\(ObjectIdentifier(c).hashValue)" }

    private func attach(_ c: GCController) {
        guard let pad = c.extendedGamepad else { return }
        c.light?.color = GCColor(red: 0.85, green: 0.06, blue: 0.08)
        let source = ControllerInput.source(c)
        pad.valueChangedHandler = { pad, _ in InputHub.shared.set(source, ControllerInput.keys(pad)) }
    }

    static func keys(_ p: GCExtendedGamepad) -> Set<UInt32> {
        var keys = Set<UInt32>()
        let x = max(-1, min(1, p.leftThumbstick.xAxis.value + p.dpad.xAxis.value))
        let y = max(-1, min(1, p.leftThumbstick.yAxis.value + p.dpad.yAxis.value))
        if x < -0.4 { keys.insert(DIK.left) }
        if x > 0.4 { keys.insert(DIK.right) }
        if y > 0.4 { keys.insert(DIK.up) }
        if y < -0.4 { keys.insert(DIK.down) }
        for a in GameAction.all where a.pad(p)?.isPressed == true { keys.insert(a.key) }
        return keys
    }

    private func attach(_ k: GCKeyboard) {
        k.keyboardInput?.keyChangedHandler = { [weak self] _, _, code, pressed in
            guard let self, let dik = ControllerInput.dik(code.rawValue) else { return }
            if pressed { self.keyboardHeld.insert(dik) } else { self.keyboardHeld.remove(dik) }
            InputHub.shared.set("keyboard", self.keyboardHeld)
        }
    }

    /// HID keyboard usage -> DirectInput key code (letters, digits, the keys the game reads).
    static func dik(_ usage: Int) -> UInt32? {
        let letters: [UInt32] = [0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C]  // a-z
        switch usage {
        case 0x04...0x1D: return letters[usage - 0x04]
        case 0x1E...0x26: return UInt32(usage - 0x1E + 0x02)  // 1-9
        case 0x27: return 0x0B  // 0
        case 0x28, 0x58: return DIK.enter  // Return, keypad Enter
        case 0x29: return DIK.escape
        case 0x2A: return 0x0E  // Backspace
        case 0x2B: return 0x0F  // Tab
        case 0x2C: return DIK.space
        case 0x2D: return 0x0C  // -
        case 0x2E: return 0x0D  // =
        case 0x3A...0x43: return UInt32(usage - 0x3A + 0x3B)  // F1-F10
        case 0x44: return 0x57  // F11
        case 0x45: return 0x58  // F12
        case 0x4F: return DIK.right
        case 0x50: return DIK.left
        case 0x51: return DIK.down
        case 0x52: return DIK.up
        case 0xE0: return 0x1D  // left Control
        case 0xE1: return 0x2A  // left Shift
        case 0xE2: return 0x38  // left Option
        case 0xE5: return 0x36  // right Shift
        default: return nil
        }
    }
}
