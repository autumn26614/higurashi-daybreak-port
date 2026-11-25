import GameController
import SwiftUI

/// A header in the game's style: white heavy italics on a red slanted bar.
struct SlantTitle: View {
    let text: String
    var size: CGFloat = 20

    var body: some View {
        Text(text)
            .font(Theme.title(size))
            .foregroundStyle(.white)
            .padding(.horizontal, size * 1.1)
            .padding(.vertical, size * 0.3)
            .background(Slant().fill(LinearGradient(colors: [Theme.ember, Theme.red], startPoint: .top, endPoint: .bottom)))
            .overlay(Slant().stroke(Theme.edge, lineWidth: 1.5))
    }
}

/// The settings gear: a small diamond in the top-left corner.
struct GearButton: View {
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            ZStack {
                Diamond().fill(Color.black.opacity(0.55))
                Diamond().stroke(Theme.edge, lineWidth: 1.5)
                Image(systemName: "gearshape.fill")
                    .font(.system(size: 15, weight: .bold))
                    .foregroundStyle(.white)
            }
            .frame(width: 44, height: 44)
        }
        .accessibilityLabel("Controls settings")
        .accessibilityIdentifier("controls-settings")
    }
}

/// Control settings, and what every button does on screen, on a keyboard and on a controller.
/// The game keeps running behind it.
struct ControlsPanel: View {
    @Binding var shown: Bool
    @AppStorage("controls.size") private var size = 1.0
    @AppStorage("controls.opacity") private var opacity = 0.85
    @AppStorage("controls.haptics") private var haptics = true
    @AppStorage("controls.withController") private var withController = false
    @State private var pad = GCController.controllers().compactMap(\.extendedGamepad).first

    var body: some View {
        ZStack {
            Color.black.opacity(0.45).ignoresSafeArea().onTapGesture { shown = false }
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    SlantTitle(text: "CONTROLS")
                    Spacer()
                    Button { shown = false } label: { SlantTitle(text: "DONE", size: 15) }
                        .accessibilityIdentifier("controls-done")
                }
                ScrollView {
                    VStack(alignment: .leading, spacing: 14) {
                        settings
                        Divider().overlay(Color.white.opacity(0.3))
                        mapping
                    }
                }
            }
            .padding(18)
            .frame(maxWidth: 620, maxHeight: 520)
            .background(Rectangle().fill(Color.black.opacity(0.86)))
            .overlay(Rectangle().stroke(Theme.edge, lineWidth: 2))
            .overlay(Rectangle().inset(by: 5).stroke(Theme.red.opacity(0.7), lineWidth: 1))
            .padding(16)
        }
        .foregroundStyle(.white)
        .tint(Theme.red)
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidConnect)) { _ in refresh() }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidDisconnect)) { _ in refresh() }
    }

    private func refresh() { pad = GCController.controllers().compactMap(\.extendedGamepad).first }

    private var settings: some View {
        VStack(alignment: .leading, spacing: 10) {
            row("Button size") {
                Slider(value: $size, in: 0.75...1.35).accessibilityLabel("Button size")
            }
            row("Opacity") {
                Slider(value: $opacity, in: 0.3...1).accessibilityLabel("Controls opacity")
            }
            Toggle("Vibrate on press", isOn: $haptics)
            Toggle("On-screen controls with a controller", isOn: $withController)
        }
        .font(.system(size: 15, weight: .semibold))
    }

    private func row<C: View>(_ title: String, @ViewBuilder _ content: () -> C) -> some View {
        HStack {
            Text(title).frame(width: 110, alignment: .leading)
            content()
        }
    }

    private var mapping: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text("Action").frame(maxWidth: .infinity, alignment: .leading)
                Text("Keyboard").frame(width: 80)
                Text(pad == nil ? "Controller" : (pad?.controller?.productCategory ?? "Controller"))
                    .lineLimit(1).minimumScaleFactor(0.6).frame(width: 110)
            }
            .font(Theme.title(13))
            .foregroundStyle(Theme.ember)
            mapRow("Move", key: "Arrows", glyph: pad?.leftThumbstick.sfSymbolsName, fallback: "Stick / D-pad")
            ForEach(GameAction.all) { a in
                mapRow(a.name, key: a.keyLabel, glyph: pad.flatMap { a.pad($0)?.sfSymbolsName }, fallback: a.padLabel)
            }
            Text("In menus, Jump confirms and Melee goes back. The game's own key settings (操作設定) change what each key does.")
                .font(.footnote)
                .foregroundStyle(.white.opacity(0.7))
                .padding(.top, 4)
        }
    }

    private func mapRow(_ name: String, key: String, glyph: String?, fallback: String) -> some View {
        HStack {
            Text(name).frame(maxWidth: .infinity, alignment: .leading)
            Text(key)
                .font(.system(size: 13, weight: .bold, design: .rounded))
                .padding(.horizontal, 6)
                .padding(.vertical, 2)
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(Color.white.opacity(0.6)))
                .frame(width: 80)
            Group {
                if let glyph { Image(systemName: glyph).font(.system(size: 20)) } else { Text(fallback) }
            }
            .frame(width: 110)
        }
        .font(.system(size: 14, weight: .medium))
        .accessibilityElement(children: .combine)
    }
}

/// "Controller connected" for a moment when a controller comes or goes.
struct ControllerBanner: View {
    @State private var message: String?
    @State private var token = 0

    var body: some View {
        VStack {
            if let message {
                HStack(spacing: 8) {
                    Image(systemName: "gamecontroller.fill")
                    Text(message).font(Theme.title(15))
                }
                .foregroundStyle(.white)
                .padding(.horizontal, 26)
                .padding(.vertical, 8)
                .background(Slant().fill(Color.black.opacity(0.8)))
                .overlay(Slant().stroke(Theme.red, lineWidth: 2))
                .transition(.move(edge: .top).combined(with: .opacity))
            }
            Spacer()
        }
        .padding(.top, 10)
        .allowsHitTesting(false)
        .animation(.easeOut(duration: 0.25), value: message)
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidConnect)) { note in
            let name = (note.object as? GCController)?.vendorName ?? "Controller"
            show("\(name) connected")
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidDisconnect)) { _ in
            show("Controller disconnected")
        }
    }

    private func show(_ text: String) {
        message = text
        token += 1
        let mine = token
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) {
            if token == mine { message = nil }
        }
    }
}
