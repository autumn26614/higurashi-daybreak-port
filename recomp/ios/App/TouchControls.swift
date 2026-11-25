import SwiftUI
import UIKit

/// The look of the game's own menus: black diamond panels with white edges, red when active.
enum Theme {
    static let red = Color(red: 0.86, green: 0.07, blue: 0.10)
    static let ember = Color(red: 1.0, green: 0.42, blue: 0.16)
    static let panel = Color.black.opacity(0.55)
    static let edge = Color.white.opacity(0.92)

    static func title(_ size: CGFloat) -> Font { .system(size: size, weight: .black).italic() }
}

/// A square standing on its corner, as the game's menus and team select are built of.
struct Diamond: Shape {
    func path(in r: CGRect) -> Path {
        var p = Path()
        p.move(to: CGPoint(x: r.midX, y: r.minY))
        p.addLine(to: CGPoint(x: r.maxX, y: r.midY))
        p.addLine(to: CGPoint(x: r.midX, y: r.maxY))
        p.addLine(to: CGPoint(x: r.minX, y: r.midY))
        p.closeSubpath()
        return p
    }
}

/// Where everything sits, for a screen size: used both to draw the controls and to find what
/// a touch is on. Sizes in points; `radius` is a diamond's centre-to-corner distance.
struct ControlLayout {
    struct Button {
        let action: GameAction
        let title: String?  // nil: the key letter alone
        let center: CGPoint
        let radius: CGFloat
        var pill = false  // a slanted bar (Esc, Start) instead of a diamond

        /// 1 at the edge: diamonds in the L1 distance (a diamond is the points within its
        /// radius), bars as rectangles 2.6 radii wide.
        func distance(_ p: CGPoint) -> CGFloat {
            let dx = abs(p.x - center.x), dy = abs(p.y - center.y)
            return pill ? max(dx / (radius * 1.3), dy / (radius * 0.6)) : (dx + dy) / radius
        }
    }

    var buttons: [Button] = []
    var top: [Button] = []
    var stickHome: CGPoint = .zero
    var stickRadius: CGFloat = 0
    var stickZone: CGRect = .zero

    init(size: CGSize, safe: UIEdgeInsets, scale: CGFloat) {
        guard size.width > 0, size.height > 0 else { return }
        let r = min(size.height * 0.105, 58) * scale  // a main button
        let d = r * 1.1  // centre spacing in the cluster: the diamonds tile with a small gap
        let c = CGPoint(x: size.width - max(safe.right, 12) - r * 2.2,
                        y: size.height - max(safe.bottom, 10) - r * 2.2)
        func at(_ dx: CGFloat, _ dy: CGFloat) -> CGPoint { CGPoint(x: c.x + dx * d, y: c.y + dy * d) }
        func b(_ id: String, _ title: String?, _ p: CGPoint, _ k: CGFloat) -> Button {
            Button(action: GameAction.named(id), title: title, center: p, radius: r * k)
        }
        buttons = [
            b("jump", "JUMP", at(0, 1), 1),
            b("melee", "MELEE", at(-1, 0), 1),
            b("shot", "SHOT", at(1, 0), 1),
            b("special", "SPECIAL", at(0, -1), 1),
            b("guard", "GUARD", at(-2.05, 0.95), 0.72),
            b("a", nil, at(-2.05, -0.95), 0.55),
            b("q", nil, at(-1.15, -1.9), 0.55),
            b("w", nil, at(1.15, -1.9), 0.55),
        ]
        let tr = min(size.height * 0.045, 24) * max(scale, 0.9)
        let ty = max(safe.top, 8) + tr * 0.6 + 6
        top = [
            Button(action: GameAction.named("esc"), title: "ESC",
                   center: CGPoint(x: max(safe.left, 12) + tr * 1.3 + 56, y: ty), radius: tr, pill: true),
            Button(action: GameAction.named("start"), title: "START",
                   center: CGPoint(x: size.width - max(safe.right, 12) - tr * 1.3, y: ty), radius: tr, pill: true),
        ]
        stickRadius = min(size.height * 0.16, 84) * scale
        stickHome = CGPoint(x: max(safe.left, 16) + stickRadius * 1.25,
                            y: size.height - max(safe.bottom, 12) - stickRadius * 1.25)
        stickZone = CGRect(x: 0, y: size.height * 0.22, width: size.width * 0.45, height: size.height * 0.78)
    }

    /// The button under a point, with a little slack, nearest first.
    func button(at p: CGPoint) -> Button? {
        (buttons + top)
            .map { ($0, $0.distance(p)) }
            .filter { $0.1 <= 1.18 }
            .min { $0.1 < $1.1 }?.0
    }

    /// The stick's base for a touch that starts at p: there, kept inside the screen.
    func stickBase(for p: CGPoint, in size: CGSize) -> CGPoint {
        CGPoint(x: min(max(p.x, stickRadius + 8), size.width * 0.5 - stickRadius),
                y: min(max(p.y, stickRadius + 8), size.height - stickRadius - 4))
    }
}

/// What the controls show: pressed buttons, and the stick where it is.
final class ControlState: ObservableObject {
    @Published var pressed = Set<String>()
    @Published var stickBase: CGPoint?
    @Published var knob = CGSize.zero
    @Published var directions = Set<UInt32>()
}

/// Takes every touch on the screen. A finger may slide from one button to the next (as on a
/// pad); the stick appears where the left thumb lands and follows it if it strays far.
final class TouchSurface: UIView {
    var layout = ControlLayout(size: .zero, safe: .zero, scale: 1)
    var state: ControlState?
    var haptics = true
    private var buttonTouches: [UITouch: String] = [:]
    private var stickTouch: UITouch?
    private var stickBase = CGPoint.zero
    private let feedback = UIImpactFeedbackGenerator(style: .light)

    override init(frame: CGRect) {
        super.init(frame: frame)
        isMultipleTouchEnabled = true
        backgroundColor = .clear
        isAccessibilityElement = true
        accessibilityLabel = "Game controls"
    }

    required init?(coder: NSCoder) { fatalError() }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        for t in touches {
            let p = t.location(in: self)
            if let b = layout.button(at: p) {
                buttonTouches[t] = b.action.id
            } else if stickTouch == nil, layout.stickZone.contains(p) {
                stickTouch = t
                stickBase = layout.stickBase(for: p, in: bounds.size)
                moveStick(to: p)
            }
        }
        publish()
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        for t in touches {
            let p = t.location(in: self)
            if t == stickTouch {
                moveStick(to: p)
            } else if buttonTouches[t] != nil, let b = layout.button(at: p) {
                buttonTouches[t] = b.action.id
            }
        }
        publish()
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) { lift(touches) }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) { lift(touches) }

    private func lift(_ touches: Set<UITouch>) {
        for t in touches {
            buttonTouches[t] = nil
            if t == stickTouch { stickTouch = nil }
        }
        publish()
    }

    func releaseAll() {
        buttonTouches = [:]
        stickTouch = nil
        publish()
    }

    private func moveStick(to p: CGPoint) {
        var v = CGVector(dx: p.x - stickBase.x, dy: p.y - stickBase.y)
        let len = hypot(v.dx, v.dy), r = layout.stickRadius
        if len > r * 1.5 {  // the thumb has wandered: the base follows it
            let k = (len - r * 1.5) / len
            stickBase.x += v.dx * k
            stickBase.y += v.dy * k
            v = CGVector(dx: p.x - stickBase.x, dy: p.y - stickBase.y)
        }
        stickKnob = v
    }

    private var stickKnob = CGVector.zero

    /// Eight directions: a direction counts when the thumb is within 67.5 degrees of it.
    private var directions: Set<UInt32> {
        guard stickTouch != nil else { return [] }
        let len = hypot(stickKnob.dx, stickKnob.dy)
        guard len > layout.stickRadius * 0.28 else { return [] }
        let nx = stickKnob.dx / len, ny = stickKnob.dy / len
        var keys = Set<UInt32>()
        if nx < -0.38 { keys.insert(DIK.left) }
        if nx > 0.38 { keys.insert(DIK.right) }
        if ny < -0.38 { keys.insert(DIK.up) }
        if ny > 0.38 { keys.insert(DIK.down) }
        return keys
    }

    private func publish() {
        let ids = Set(buttonTouches.values)
        let dirs = directions
        var keys = dirs
        for id in ids { keys.insert(GameAction.named(id).key) }
        InputHub.shared.set("touch", keys)
        guard let state else { return }
        if haptics, !ids.subtracting(state.pressed).isEmpty { feedback.impactOccurred() }
        if state.pressed != ids { state.pressed = ids }
        if state.directions != dirs { state.directions = dirs }
        state.stickBase = stickTouch == nil ? nil : stickBase
        let r = layout.stickRadius, len = max(1, hypot(stickKnob.dx, stickKnob.dy))
        let k = stickTouch == nil ? 0 : min(len, r * 0.62) / len
        state.knob = CGSize(width: stickKnob.dx * k, height: stickKnob.dy * k)
    }
}

struct TouchSurfaceView: UIViewRepresentable {
    let layout: ControlLayout
    let state: ControlState
    let haptics: Bool

    func makeUIView(context: Context) -> TouchSurface {
        let v = TouchSurface(frame: .zero)
        v.state = state
        return v
    }

    func updateUIView(_ v: TouchSurface, context: Context) {
        v.layout = layout
        v.haptics = haptics
    }

    static func dismantleUIView(_ v: TouchSurface, coordinator: ()) { v.releaseAll() }
}

/// The on-screen controls: the stick on the left, the game's buttons on the right in a
/// cluster of diamonds, Esc and Start at the top.
struct TouchControls: View {
    @AppStorage("controls.size") private var size = 1.0
    @AppStorage("controls.opacity") private var opacity = 0.85
    @AppStorage("controls.haptics") private var haptics = true
    @StateObject private var state = ControlState()
    /// Shows a few buttons held and the stick pushed (screenshots of the controls).
    var demo = false

    var body: some View {
        GeometryReader { g in
            let safe = UIEdgeInsets(top: g.safeAreaInsets.top, left: g.safeAreaInsets.leading,
                                    bottom: g.safeAreaInsets.bottom, right: g.safeAreaInsets.trailing)
            let layout = ControlLayout(size: g.size, safe: safe, scale: CGFloat(size))
            ZStack(alignment: .topLeading) {
                StickView(layout: layout, state: state)
                ForEach(layout.buttons + layout.top, id: \.action.id) { b in
                    PadButtonView(button: b, pressed: state.pressed.contains(b.action.id))
                }
            }
            .opacity(opacity)
            .allowsHitTesting(false)
            .accessibilityHidden(true)
            .background(TouchSurfaceView(layout: layout, state: state, haptics: haptics))
            .onAppear {
                guard demo else { return }
                state.pressed = ["shot", "guard"]
                state.directions = [DIK.up, DIK.right]
                state.stickBase = CGPoint(x: layout.stickHome.x + 20, y: layout.stickHome.y - 10)
                state.knob = CGSize(width: layout.stickRadius * 0.42, height: -layout.stickRadius * 0.42)
            }
        }
        .ignoresSafeArea()
    }
}

struct PadButtonView: View {
    let button: ControlLayout.Button
    let pressed: Bool

    private var fill: AnyShapeStyle {
        pressed
            ? AnyShapeStyle(LinearGradient(colors: [Theme.ember, Theme.red], startPoint: .top, endPoint: .bottom))
            : AnyShapeStyle(LinearGradient(colors: [Color.black.opacity(0.62), Theme.red.opacity(0.28)],
                                           startPoint: .top, endPoint: .bottom))
    }

    var body: some View {
        Group {
            if button.pill { bar } else { diamond }
        }
        .shadow(color: pressed ? Theme.ember.opacity(0.9) : .black.opacity(0.4), radius: pressed ? button.radius * 0.3 : 3)
        .scaleEffect(pressed ? 0.93 : 1)
        .animation(.easeOut(duration: 0.08), value: pressed)
        .position(button.center)
    }

    private var diamond: some View {
        let w = button.radius * 2
        return ZStack {
            Diamond().fill(fill)
            Diamond().stroke(Theme.edge, lineWidth: max(1.5, w * 0.03))
            Diamond().inset(by: w * 0.09).stroke(Color.white.opacity(pressed ? 0.6 : 0.22), lineWidth: 1)
            VStack(spacing: 0) {
                if let title = button.title {
                    Text(title)
                        .font(Theme.title(w * (title.count > 5 ? 0.13 : 0.16)))
                        .minimumScaleFactor(0.5)
                        .lineLimit(1)
                    Text(button.action.keyLabel)
                        .font(.system(size: w * 0.11, weight: .bold, design: .rounded))
                        .opacity(0.7)
                } else {
                    Text(button.action.keyLabel).font(Theme.title(w * 0.3))
                }
            }
            .foregroundStyle(.white)
            .shadow(color: .black.opacity(0.8), radius: 1, x: 1, y: 1)
            .frame(width: w * 0.62)
        }
        .frame(width: w, height: w)
    }

    private var bar: some View {
        let w = button.radius * 2.6, h = button.radius * 1.2
        return ZStack {
            Slant().fill(fill)
            Slant().stroke(Theme.edge, lineWidth: 1.5)
            Text(button.title ?? button.action.keyLabel)
                .font(Theme.title(h * 0.48))
                .foregroundStyle(.white)
                .shadow(color: .black.opacity(0.8), radius: 1, x: 1, y: 1)
        }
        .frame(width: w, height: h)
    }
}

/// The game's HUD leans its labels: a bar with slanted ends.
struct Slant: Shape {
    func path(in r: CGRect) -> Path {
        let s = r.height * 0.35
        var p = Path()
        p.move(to: CGPoint(x: r.minX + s, y: r.minY))
        p.addLine(to: CGPoint(x: r.maxX, y: r.minY))
        p.addLine(to: CGPoint(x: r.maxX - s, y: r.maxY))
        p.addLine(to: CGPoint(x: r.minX, y: r.maxY))
        p.closeSubpath()
        return p
    }
}

extension Diamond: InsettableShape {
    func inset(by amount: CGFloat) -> InsetDiamond { InsetDiamond(amount: amount) }
}

struct InsetDiamond: InsettableShape {
    var amount: CGFloat
    func path(in r: CGRect) -> Path { Diamond().path(in: r.insetBy(dx: amount, dy: amount)) }
    func inset(by more: CGFloat) -> InsetDiamond { InsetDiamond(amount: amount + more) }
}

/// A ring with the four directions marked (lit while held) and a diamond for the thumb.
struct StickView: View {
    let layout: ControlLayout
    @ObservedObject var state: ControlState

    var body: some View {
        let r = layout.stickRadius
        let active = state.stickBase != nil
        ZStack {
            Circle().fill(Color.black.opacity(0.4))
            Circle().stroke(Theme.edge.opacity(active ? 0.9 : 0.55), lineWidth: 2)
            Circle().inset(by: r * 0.2).stroke(Color.white.opacity(0.15), lineWidth: 1)
            ForEach(0..<4) { i in
                let key = [DIK.up, DIK.right, DIK.down, DIK.left][i]
                let lit = state.directions.contains(key)
                Triangle()
                    .fill(lit ? Theme.ember : Color.white.opacity(0.55))
                    .frame(width: r * 0.22, height: r * 0.14)
                    .shadow(color: lit ? Theme.red : .clear, radius: 6)
                    .offset(y: -r * 0.8)
                    .rotationEffect(.degrees(Double(i) * 90))
            }
            Diamond()
                .fill(LinearGradient(colors: active ? [Theme.ember, Theme.red] : [Color.white.opacity(0.5), Color.white.opacity(0.25)],
                                     startPoint: .top, endPoint: .bottom))
                .overlay(Diamond().stroke(Theme.edge, lineWidth: 2))
                .frame(width: r * 0.8, height: r * 0.8)
                .shadow(color: active ? Theme.red.opacity(0.8) : .clear, radius: 10)
                .offset(state.knob)
        }
        .frame(width: r * 2, height: r * 2)
        .opacity(active ? 1 : 0.6)
        .position(state.stickBase ?? layout.stickHome)
        .animation(.easeOut(duration: 0.12), value: state.stickBase == nil)
    }
}

struct Triangle: Shape {
    func path(in r: CGRect) -> Path {
        var p = Path()
        p.move(to: CGPoint(x: r.midX, y: r.minY))
        p.addLine(to: CGPoint(x: r.maxX, y: r.maxY))
        p.addLine(to: CGPoint(x: r.minX, y: r.maxY))
        p.closeSubpath()
        return p
    }
}
