import SwiftUI

/// Story mode: pick a team (data/scenario/team%02d.dat), then play its six episodes in
/// order, as the game does: the scene before the battle (D%02d-0), the battle on the
/// episode's stage, the scene after it (D%02d-1). Battles are two against two as in the game:
/// 1P plays the team's first member with the second as a computer partner.
struct StoryView: View {
    let urls: [URL]
    @State private var book: TalkBook?
    @State private var teams: [StoryTeam] = []
    @State private var errorText: String?

    /// "Keiichi" for story character 2.
    static func characterName(_ n: Int32) -> String {
        guard let p = db_story_character_dir(n) else { return "?" }
        return String(cString: p).capitalized
    }

    /// The definition the story uses for character n (its 00 variant).
    static func definition(_ n: Int32) -> String? {
        db_story_character_dir(n).map { "data/" + String(cString: $0) + "/00.dat" }
    }

    var body: some View {
        if let art, !teams.isEmpty {
            TeamSelectView(art: art, teams: teams, urls: urls, book: book!)
        } else {
            list
        }
    }

    @State private var art: TeamSelectArt?

    private var list: some View {
        List {
            if let errorText {
                Text(errorText).foregroundStyle(.red).accessibilityIdentifier("error")
            }
            if let book {
                if teams.isEmpty {
                    Text("No story teams in these archives").accessibilityIdentifier("noTeams")
                }
                ForEach(teams) { entry in
                    NavigationLink {
                        StoryRunView(urls: urls, book: book, team: entry.team, teamIndex: entry.index)
                    } label: {
                        VStack(alignment: .leading) {
                            Text("\(Self.characterName(entry.team.members.0)) & \(Self.characterName(entry.team.members.1))")
                            Text("\(entry.team.episodes) episodes").font(.footnote).foregroundStyle(.secondary)
                        }
                    }
                    .accessibilityIdentifier("team.\(entry.index)")
                }
            } else if errorText == nil {
                ProgressView()
            }
        }
        .navigationTitle("Story")
        .task {
            let urls = urls
            let result = await Task.detached { Result { try TalkBook(urls: urls) } }.value
            switch result {
            case .success(let b):
                book = b
                teams = (0..<10).compactMap { i in b.team(i).map { StoryTeam(index: i, team: $0) } }
                let urls = urls
                art = await Task.detached { try? TeamSelectArt(urls: urls) }.value
            case .failure(let e):
                errorText = e.localizedDescription
            }
        }
    }
}

/// A story team file and its number.
struct StoryTeam: Identifiable {
    let index: Int
    let team: DBTeam
    var id: Int { index }
}

/// One team's run through its episodes.
struct StoryRunView: View {
    let urls: [URL]
    let book: TalkBook
    let team: DBTeam
    var teamIndex = 0

    enum Phase: Equatable {
        case before(Int), battle(Int), win(Int), after(Int), ending, end
    }

    @State private var phase: Phase = .before(0)

    private func episode(_ i: Int) -> (number: Int, stage: Int, opponents: [Int32]) {
        withUnsafeBytes(of: team.number) { n in
            withUnsafeBytes(of: team.stage) { s in
                withUnsafeBytes(of: team.opponents) { o in
                    (Int(n.load(fromByteOffset: i * 4, as: Int32.self)),
                     Int(s.load(fromByteOffset: i * 4, as: Int32.self)),
                     [o.load(fromByteOffset: i * 8, as: Int32.self), o.load(fromByteOffset: i * 8 + 4, as: Int32.self)])
                }
            }
        }
    }

    /// The four fighters of episode i: the team, then the opposing pair.
    private func entrants(_ i: Int) -> [BattleEntrant]? {
        let ids = [team.members.0, team.members.1] + episode(i).opponents
        var out: [BattleEntrant] = []
        for n in ids {
            guard let def = StoryView.definition(n), let model = book.chardefModel(def) else { return nil }
            out.append(BattleEntrant(model: model, definition: def))
        }
        return out
    }

    private func next(after i: Int) -> Phase {
        i + 1 < Int(team.episodes) ? .before(i + 1) : (movie.isEmpty ? .end : .ending)
    }

    var body: some View {
        Group {
            switch phase {
            case .before(let i):
                scene(i, part: 0) { phase = .battle(i) }
            case .battle(let i):
                if let fighters = entrants(i) {
                    BattleView(urls: urls, preset: BattlePreset(stage: episode(i).stage, fighters: fighters)) { _ in
                        phase = .win(i)
                    }
                    .id("battle\(i)")
                } else {
                    Button("Skip battle") { phase = .after(i) }
                }
            case .win(let i):
                WinView(book: book, pair: (team.members.0, team.members.1)) { phase = .after(i) }
                    .id("win\(i)")
            case .after(let i):
                scene(i, part: 1) { phase = next(after: i) }
            case .ending:
                MovieView(urls: urls, entry: movie) { phase = .end }
                    .onAppear { StoryProgress.clear(team: teamIndex) }
            case .end:
                VStack(spacing: 16) {
                    Text("Fin").font(.largeTitle.bold())
                }
                .accessibilityIdentifier("storyEnd")
            }
        }
        .navigationTitle(title)
        .navigationBarTitleDisplayMode(.inline)
    }

    /// The ending movie's entry ("movie/e00.mpg").
    private var movie: String {
        withUnsafeBytes(of: team.movie) { raw in
            String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
        }
    }

    private var title: String {
        switch phase {
        case .before(let i), .battle(let i), .win(let i), .after(let i):
            return String(format: "Episode %d of %d", i + 1, Int(team.episodes))
        case .ending, .end:
            return "Story"
        }
    }

    @ViewBuilder
    private func scene(_ i: Int, part: Int, done: @escaping () -> Void) -> some View {
        let file = String(format: "D%02d-%d.txt", episode(i).number, part)
        if let name = book.script(endingWith: file) {
            TalkView(book: book, scriptName: name, onFinish: done)
                .id(file)
        } else {
            VStack(spacing: 12) {
                Text("\(file) is not in these archives")
                Button("Continue", action: done)
            }
        }
    }
}

/// Cleared story teams, kept between launches. The game opens teams as they are cleared:
/// clamp(cleared + 1, 3, 7) of the base seven (DAT_0065F848 & 0xF); Kai's three follow.
enum StoryProgress {
    private static let key = "story.cleared"

    static var cleared: Set<Int> { Set(UserDefaults.standard.array(forKey: key) as? [Int] ?? []) }

    static func clear(team: Int) {
        UserDefaults.standard.set(Array(cleared.union([team])).sorted(), forKey: key)
    }

    static func unlocked(available: [Int]) -> [Int] {
        let base = available.filter { $0 < 7 }, kai = available.filter { $0 >= 7 }
        let open = min(7, max(3, cleared.filter { $0 < 7 }.count + 1))
        return Array(base.prefix(open)) + (base.count <= open ? kai : [])
    }
}

/// The team select's pictures (scene 5, ctor 0x40B0F0): data/teamselect/bg00 (800x600),
/// t%02d (512x512, drawn centred at (400, 300)) and the name plates p00 (256x160 cells, team i
/// at column i / 3, row i % 3) and p01 (team 6).
final class TeamSelectArt {
    let background: CGImage?
    let pictures: [CGImage?]
    let plates: [CGImage?]
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
        background = TitleArt.image(a, "data/teamselect/bg00.bmp", crop: nil)
        pictures = (0..<7).map { TitleArt.image(a, String(format: "data/teamselect/t%02d.bmp", $0), crop: nil) }
        let sheet = TitleArt.image(a, "data/teamselect/p00.bmp", crop: nil)
        var plates: [CGImage?] = (0..<6).map { i in
            sheet?.cropping(to: CGRect(x: (i / 3) * 256, y: (i % 3) * 160, width: 256, height: 160))
        }
        plates.append(TitleArt.image(a, "data/teamselect/p01.bmp", crop: CGRect(x: 0, y: 0, width: 256, height: 160)))
        self.plates = plates
        if background == nil { throw ReportError.open("no team select pictures") }
    }

    deinit {
        db_archives_free(archives)
        scopedURLs.forEach { $0.stopAccessingSecurityScopedResource() }
    }

    func sound(_ entry: String) -> DecodedSound? { try? DecodedSound.load(archives, Array(entry.utf8)) }
}

/// The team select on the game's 800x600 canvas. approx.: the plate column's exact sprites
/// and scale are not decoded; plates sit at half size at x 156, y 50 + 70 k as the list does.
struct TeamSelectView: View {
    let art: TeamSelectArt
    let teams: [StoryTeam]
    let urls: [URL]
    let book: TalkBook
    @State private var cursor = 0
    @State private var chosen: StoryTeam?
    @State private var music = SoundPlayer(kind: .music)
    @State private var effects = SoundPlayer()

    private var open: [StoryTeam] {
        let ids = StoryProgress.unlocked(available: teams.map(\.index))
        return teams.filter { ids.contains($0.index) }
    }

    var body: some View {
        GeometryReader { g in
            let scale = min(g.size.width / 800, g.size.height / 600)
            ZStack(alignment: .topLeading) {
                Color.black
                ZStack(alignment: .topLeading) {
                    if let b = art.background { picture(b, x: 0, y: 0, w: 800, h: 600) }
                    if cursor < open.count, open[cursor].index < art.pictures.count, let t = art.pictures[open[cursor].index] {
                        picture(t, x: 400 - 256, y: 300 - 256, w: 512, h: 512)
                            .onTapGesture { start() }
                    }
                    ForEach(Array(open.enumerated()), id: \.offset) { k, team in
                        let plate = team.index < art.plates.count ? art.plates[team.index] : nil
                        Group {
                            if let plate {
                                picture(plate, x: 156 - 64, y: 50 + 70 * CGFloat(k) - 20, w: 128, h: 80)
                            } else {
                                Text("\(StoryView.characterName(team.team.members.0)) & \(StoryView.characterName(team.team.members.1))")
                                    .foregroundStyle(.white).offset(x: 40, y: 50 + 70 * CGFloat(k))
                            }
                        }
                        .opacity(k == cursor ? 1 : 0.5)
                        .onTapGesture { pick(k) }
                        .accessibilityElement()
                        .accessibilityLabel("Team \(team.index + 1)")
                        .accessibilityAddTraits(.isButton)
                        .accessibilityIdentifier("team.\(team.index)")
                    }
                }
                .frame(width: 800, height: 600, alignment: .topLeading)
                .clipped()
                .scaleEffect(scale, anchor: .topLeading)
                .offset(x: (g.size.width - 800 * scale) / 2, y: (g.size.height - 600 * scale) / 2)
            }
        }
        .ignoresSafeArea()
        .navigationTitle("Story")
        .navigationBarTitleDisplayMode(.inline)
        .navigationDestination(isPresented: Binding(get: { chosen != nil }, set: { if !$0 { chosen = nil } })) {
            if let chosen {
                StoryRunView(urls: urls, book: book, team: chosen.team, teamIndex: chosen.index)
            }
        }
        .task { playMusic() }
        .onChange(of: chosen?.index) { c in if c == nil { playMusic() } else { music.stop() } }
        .onDisappear { music.stop() }
    }

    private func picture(_ img: CGImage, x: CGFloat, y: CGFloat, w: CGFloat, h: CGFloat) -> some View {
        Image(decorative: img, scale: 1).resizable().frame(width: w, height: h).offset(x: x, y: y)
    }

    private func playMusic() {
        let art = art, player = music
        Task.detached {
            let song = art.sound("data/bgm/select01.ogg")
            await MainActor.run { if let song { player.play(song) } }
        }
    }

    private func pick(_ k: Int) {
        if k == cursor {
            start()
        } else {
            cursor = k
            if let s = art.sound("data/sysse/002.wav") { effects.play(s) }
        }
    }

    private func start() {
        guard cursor < open.count else { return }
        if let s = art.sound("data/sysse/000.wav") { effects.play(s) }
        chosen = open[cursor]
    }
}

/// The win screen after a battle (battle phase 4, 0x5B3750): the winning pair's portraits
/// (data/win/%02da) and a random quote from data/scenario/win/%02d-%02d-%d.txt (lower character
/// number first) with its voice from data/voice/win/. approx.: the game picks the file's last
/// number (1-4) from the round's margin; here grade 1. The portraits stand in the talk
/// scene's slots (x 200 and 600).
struct WinView: View {
    let book: TalkBook
    let pair: (Int32, Int32)
    let onContinue: () -> Void
    @State private var image: CGImage?
    @State private var voice = SoundPlayer(kind: .voice)
    @State private var music = SoundPlayer(kind: .music)

    var body: some View {
        ZStack {
            Color.black
            if let image {
                Image(decorative: image, scale: 1).resizable().interpolation(.high)
                    .aspectRatio(4.0 / 3.0, contentMode: .fit)
                    .accessibilityIdentifier("winFrame")
            } else {
                Text("WIN").font(.largeTitle.bold()).foregroundStyle(.white)
            }
        }
        .ignoresSafeArea()
        .contentShape(Rectangle())
        .onTapGesture {
            voice.stop()
            music.stop()
            onContinue()
        }
        .task { show() }
        .onDisappear { voice.stop(); music.stop() }
    }

    private func show() {
        if let m = book.sound("data/bgm/win.ogg") { music.play(m) }
        let a = min(pair.0, pair.1), b = max(pair.0, pair.1)
        guard let name = book.script(endingWith: String(format: "%02d-%02d-%d.txt", a, b, 1)),
              let pages = try? book.pages(script: name), !pages.isEmpty else { return }
        let page = pages[Int.random(in: 0..<pages.count)]
        if !page.voice.isEmpty, let v = book.sound("data/voice/win/" + page.voice) { voice.play(v) }
        let bg = portraits([pair.0, pair.1])
        let f = book.frame(page, reveal: 100_000, arrow: true, background: bg)
        image = TalkBook.cgImage(f.rgba)
    }

    /// 800x600 RGBA: the two portraits over black.
    private func portraits(_ who: [Int32]) -> [UInt8]? {
        var rgba = [UInt8](repeating: 0, count: 800 * 600 * 4)
        let drawn: Bool = rgba.withUnsafeMutableBytes { raw in
            guard let ctx = CGContext(data: raw.baseAddress, width: 800, height: 600, bitsPerComponent: 8, bytesPerRow: 800 * 4,
                                      space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return false }
            ctx.setFillColor(CGColor(gray: 0, alpha: 1))
            ctx.fill(CGRect(x: 0, y: 0, width: 800, height: 600))
            for (slot, n) in who.enumerated() {
                guard let img = book.image(String(format: "data/win/%02da.bmp", n)) else { continue }
                let x = CGFloat(slot == 0 ? 200 : 600) - 256
                // CoreGraphics' origin is the bottom left; the game's is the top left (y = 88).
                ctx.draw(img, in: CGRect(x: x, y: 600 - 88 - 512, width: 512, height: 512))
            }
            return true
        }
        return drawn ? rgba : nil
    }
}
