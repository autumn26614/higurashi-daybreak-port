import GameController

/// Game controllers (Xbox, PlayStation, MFi) as the game's keyboard keys, which its default
/// controls use: stick and d-pad = arrows, A = Z (jump), X = X (melee), B/Y = C (shot),
/// R1 = D (guard), L1 = S, Menu = Enter, Options = Esc.
final class ControllerInput {
    static let shared = ControllerInput()
    private var held = Set<UInt32>()

    func start() {
        NotificationCenter.default.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] note in
            if let c = note.object as? GCController { self?.attach(c) }
        }
        NotificationCenter.default.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] _ in
            self?.update([])
        }
        GCController.controllers().forEach(attach)
        GCController.startWirelessControllerDiscovery {}
    }

    private func attach(_ c: GCController) {
        guard let pad = c.extendedGamepad else { return }
        pad.valueChangedHandler = { [weak self] pad, _ in self?.read(pad) }
    }

    private func read(_ p: GCExtendedGamepad) {
        var keys = Set<UInt32>()
        let x = p.leftThumbstick.xAxis.value + p.dpad.xAxis.value
        let y = p.leftThumbstick.yAxis.value + p.dpad.yAxis.value
        if x < -0.4 { keys.insert(DIK.left) }
        if x > 0.4 { keys.insert(DIK.right) }
        if y > 0.4 { keys.insert(DIK.up) }
        if y < -0.4 { keys.insert(DIK.down) }
        if p.buttonA.isPressed { keys.insert(DIK.z) }
        if p.buttonX.isPressed { keys.insert(DIK.x) }
        if p.buttonB.isPressed || p.buttonY.isPressed { keys.insert(DIK.c) }
        if p.rightShoulder.isPressed || p.rightTrigger.isPressed { keys.insert(DIK.d) }
        if p.leftShoulder.isPressed || p.leftTrigger.isPressed { keys.insert(DIK.s) }
        if p.buttonMenu.isPressed { keys.insert(DIK.enter) }
        if p.buttonOptions?.isPressed == true { keys.insert(DIK.escape) }
        update(keys)
    }

    private func update(_ keys: Set<UInt32>) {
        for k in held.subtracting(keys) { recomp_key(k, 0) }
        for k in keys.subtracting(held) { recomp_key(k, 1) }
        held = keys
    }
}
