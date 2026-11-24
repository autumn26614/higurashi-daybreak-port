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
                        StoryRunView(urls: urls, book: book, team: entry.team)
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

    enum Phase: Equatable {
        case before(Int), battle(Int), after(Int), end
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
        i + 1 < Int(team.episodes) ? .before(i + 1) : .end
    }

    var body: some View {
        Group {
            switch phase {
            case .before(let i):
                scene(i, part: 0) { phase = .battle(i) }
            case .battle(let i):
                if let fighters = entrants(i) {
                    BattleView(urls: urls, preset: BattlePreset(stage: episode(i).stage, fighters: fighters)) { _ in
                        phase = .after(i)
                    }
                    .id("battle\(i)")
                } else {
                    Button("Skip battle") { phase = .after(i) }
                }
            case .after(let i):
                scene(i, part: 1) { phase = next(after: i) }
            case .end:
                VStack(spacing: 16) {
                    Text("Fin").font(.largeTitle.bold())
                    Text("The ending movie (\(movie)) needs the movie player, which is not done yet.")
                        .font(.footnote).multilineTextAlignment(.center).padding()
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
        case .before(let i), .battle(let i), .after(let i):
            return String(format: "Episode %d of %d", i + 1, Int(team.episodes))
        case .end:
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
