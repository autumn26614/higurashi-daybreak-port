import SwiftUI

/// DirectInput key codes the game reads (its default keyboard layout).
enum DIK {
    static let escape: UInt32 = 0x01, enter: UInt32 = 0x1C, space: UInt32 = 0x39
    static let z: UInt32 = 0x2C, x: UInt32 = 0x2D, c: UInt32 = 0x2E
    static let a: UInt32 = 0x1E, s: UInt32 = 0x1F, d: UInt32 = 0x20
    static let up: UInt32 = 0xC8, down: UInt32 = 0xD0, left: UInt32 = 0xCB, right: UInt32 = 0xCD
}

/// On-screen controls: a stick (arrow keys) on the left, the game's buttons on the right,
/// Enter and Esc at the top. Each control presses its key while touched.
struct TouchControls: View {
    var body: some View {
        VStack {
            HStack {
                KeyButton(title: "Esc", key: DIK.escape, size: 44)
                Spacer()
                KeyButton(title: "Enter", key: DIK.enter, size: 44)
            }
            Spacer()
            HStack(alignment: .bottom) {
                Stick().frame(width: 170, height: 170)
                Spacer()
                VStack(spacing: 10) {
                    HStack(spacing: 10) {
                        KeyButton(title: "S", key: DIK.s)
                        KeyButton(title: "D", key: DIK.d)
                    }
                    HStack(spacing: 10) {
                        KeyButton(title: "Z", key: DIK.z)
                        KeyButton(title: "X", key: DIK.x)
                        KeyButton(title: "C", key: DIK.c)
                    }
                }
            }
        }
        .padding(20)
    }
}

struct KeyButton: View {
    let title: String
    let key: UInt32
    var size: CGFloat = 64
    @State private var down = false

    var body: some View {
        Text(title)
            .font(.system(size: size * 0.32, weight: .bold))
            .foregroundStyle(.white)
            .frame(width: size, height: size)
            .background(Circle().fill(.white.opacity(down ? 0.45 : 0.18)))
            .overlay(Circle().stroke(.white.opacity(0.5), lineWidth: 1.5))
            .contentShape(Circle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { _ in
                        if !down {
                            down = true
                            recomp_key(key, 1)
                        }
                    }
                    .onEnded { _ in
                        down = false
                        recomp_key(key, 0)
                    }
            )
            .accessibilityLabel(title)
            .accessibilityAddTraits(.isButton)
    }
}

/// Eight-way stick: holds the arrow keys for the direction of the touch.
struct Stick: View {
    @State private var offset: CGSize = .zero
    @State private var held: Set<UInt32> = []

    var body: some View {
        GeometryReader { g in
            let r = min(g.size.width, g.size.height) / 2
            ZStack {
                Circle().fill(.white.opacity(0.12)).overlay(Circle().stroke(.white.opacity(0.4), lineWidth: 1.5))
                Circle().fill(.white.opacity(0.35)).frame(width: r * 0.8, height: r * 0.8).offset(offset)
            }
            .contentShape(Circle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { v in
                        let dx = v.location.x - r, dy = v.location.y - r
                        let len = max(1, sqrt(dx * dx + dy * dy))
                        let k = min(len, r * 0.6) / len
                        offset = CGSize(width: dx * k, height: dy * k)
                        var keys = Set<UInt32>()
                        if len > r * 0.25 {
                            let t = r * 0.38
                            if dx < -t { keys.insert(DIK.left) }
                            if dx > t { keys.insert(DIK.right) }
                            if dy < -t { keys.insert(DIK.up) }
                            if dy > t { keys.insert(DIK.down) }
                        }
                        set(keys)
                    }
                    .onEnded { _ in
                        offset = .zero
                        set([])
                    }
            )
        }
        .accessibilityLabel("Stick")
    }

    private func set(_ keys: Set<UInt32>) {
        for k in held.subtracting(keys) { recomp_key(k, 0) }
        for k in keys.subtracting(held) { recomp_key(k, 1) }
        held = keys
    }
}
