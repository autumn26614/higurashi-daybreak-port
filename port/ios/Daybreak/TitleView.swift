import SwiftUI

/// The title screen as the game draws it (scene 1: ctor 0x416D40, Update 0x416F40, Draw
/// 0x4173E0) on its 800x600 canvas: data/title/back00 at (0, 0); the top 512x300 of
/// data/title/logo anchored at its top centre at (400, -20); six menu cells of 192x48 from
/// data/title/title (column 0 the chosen look, column 1 dimmed) anchored at their top centre at
/// (400, 285 + 48 i). The cursor wraps; moving plays system sound 2, choosing 0. Items: Story,
/// Versus, Network, Options, Controls, Exit.
final class TitleArt {
    let background: CGImage?
    let logo: CGImage?
    let menu: CGImage?
    private let archives: OpaquePointer
    private let scopedURLs: [URL]

    init(urls: [URL]) throws {
        let scoped = urls.filter { $0.startAccessingSecurityScopedResource() }
        var err = [CChar](repeating: 0, count: 256)
        guard let a = withCPaths(urls.map(\.path), { db_archives_open($0, $1, &err, Int32(err.count)) }) else {
            scoped.forEach { $0.stopAccessingSecurityScopedResource() }
            throw ReportError.open(message(err))
        }
        archives = a
        scopedURLs = scoped
        background = TitleArt.image(a, "data/title/back00.bmp", crop: nil)
        logo = TitleArt.image(a, "data/title/logo.bmp", crop: CGRect(x: 0, y: 0, width: 512, height: 300))
        menu = TitleArt.image(a, "data/title/title.bmp", crop: nil)
    }

    deinit {
        db_archives_free(archives)
        scopedURLs.forEach { $0.stopAccessingSecurityScopedResource() }
    }

    /// A sound from the same archives ("data/sysse/002.wav", "data/bgm/title.ogg").
    func sound(_ entry: String) -> DecodedSound? { try? DecodedSound.load(archives, Array(entry.utf8)) }

    /// Menu cell `item` (row) in its chosen (column 0) or dimmed (column 1) look.
    func cell(_ item: Int, chosen: Bool) -> CGImage? {
        menu?.cropping(to: CGRect(x: chosen ? 0 : 192, y: item * 48, width: 192, height: 48))
    }

    private static func image(_ a: OpaquePointer, _ name: String, crop: CGRect?) -> CGImage? {
        var err = [CChar](repeating: 0, count: 256)
        let b = Array(name.utf8)
        let img = b.withUnsafeBufferPointer { p in
            p.baseAddress!.withMemoryRebound(to: CChar.self, capacity: b.count) {
                db_image_load(a, $0, Int32(b.count), &err, Int32(err.count))
            }
        }
        guard let img else { return nil }
        defer { db_image_free(img) }
        let w = Int(db_image_width(img)), h = Int(db_image_height(img))
        let data = Data(bytes: db_image_rgba(img), count: w * h * 4)
        guard let provider = CGDataProvider(data: data as CFData),
              let cg = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                               space: CGColorSpace(name: CGColorSpace.sRGB)!,
                               bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue),
                               provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
        else { return nil }
        return crop.flatMap { cg.cropping(to: $0) } ?? cg
    }
}

struct TitleView: View {
    let urls: [URL]
    @State private var art: TitleArt?
    @State private var cursor = 0
    @State private var open: Int?
    @State private var music = SoundPlayer()
    @State private var effects = SoundPlayer()
    @State private var notice: String?
    @Environment(\.dismiss) private var dismiss
    static let items = ["Story", "Versus", "Network", "Options", "Controls", "Exit"]

    var body: some View {
        GeometryReader { g in
            let scale = min(g.size.width / 800, g.size.height / 600)
            ZStack(alignment: .topLeading) {
                Color.black
                ZStack(alignment: .topLeading) {
                    if let art {
                        if let b = art.background { place(b, x: 0, y: 0, w: 800, h: 600) }
                        if let l = art.logo { place(l, x: 400 - 256, y: -20, w: 512, h: 300) }
                        ForEach(0..<6, id: \.self) { i in
                            if let c = art.cell(i, chosen: i == cursor) {
                                place(c, x: 400 - 96, y: 285 + 48 * CGFloat(i), w: 192, h: 48)
                                    .contentShape(Rectangle())
                                    .onTapGesture { choose(i) }
                                    .accessibilityElement()
                                    .accessibilityLabel(Self.items[i])
                                    .accessibilityAddTraits(.isButton)
                                    .accessibilityIdentifier("title.\(i)")
                            }
                        }
                    }
                }
                .frame(width: 800, height: 600, alignment: .topLeading)
                .clipped()
                .scaleEffect(scale, anchor: .topLeading)
                .offset(x: (g.size.width - 800 * scale) / 2, y: (g.size.height - 600 * scale) / 2)
                if let notice {
                    Text(notice).font(.footnote).foregroundStyle(.white).padding(8)
                        .background(.black.opacity(0.6), in: RoundedRectangle(cornerRadius: 6))
                        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom).padding()
                }
            }
        }
        .ignoresSafeArea()
        .navigationBarHidden(true)
        .navigationDestination(isPresented: Binding(get: { open != nil }, set: { if !$0 { open = nil } })) {
            destination
        }
        .task {
            if art == nil {
                let urls = urls
                art = await Task.detached { try? TitleArt(urls: urls) }.value
            }
            playMusic()
        }
        .onChange(of: open) { o in if o == nil { playMusic() } else { music.stop() } }
        .onDisappear { music.stop() }
    }

    @ViewBuilder
    private var destination: some View {
        switch open {
        case 0: StoryView(urls: urls)
        case 1: BattleView(urls: urls)
        case 4: ControlsHelp()
        default: EmptyView()
        }
    }

    private func place(_ img: CGImage, x: CGFloat, y: CGFloat, w: CGFloat, h: CGFloat) -> some View {
        Image(decorative: img, scale: 1).resizable().frame(width: w, height: h).offset(x: x, y: y)
    }

    private func playMusic() {
        guard let art else { return }
        let player = music
        Task.detached {
            let song = art.sound("data/bgm/title.ogg")
            await MainActor.run { if let song { player.play(song) } }
        }
    }

    private func choose(_ i: Int) {
        if i != cursor {
            cursor = i
            if let s = art?.sound("data/sysse/002.wav") { effects.play(s) }
            return
        }
        if let s = art?.sound("data/sysse/000.wav") { effects.play(s) }
        switch i {
        case 0, 1, 4:
            notice = nil
            open = i
        case 2:
            notice = "Network play (DirectPlay) is not part of the port."
        case 3:
            notice = "Options are not done yet."
        default:
            music.stop()
            dismiss()  // Exit: back to the game files screen
        }
    }
}

/// The controls, for the title's Controls item.
struct ControlsHelp: View {
    var body: some View {
        List {
            Section("Touch") {
                Text("Stick: move (up is toward the opponent). Double-tap or Dash: dash.")
                Text("Jump (hold to keep rising), Melee, Shot (tap; hold to charge), Guard.")
            }
            Section("Keyboard (the game's defaults)") {
                Text("Arrows: move. Z jump, X melee, C shot, D guard (Z + X), S (X + C).")
            }
            Section("Controller") {
                Text("Left stick or d-pad: move. A jump, X melee, B / Y shot, R1 guard.")
            }
        }
        .navigationTitle("Controls")
    }
}
