import SwiftUI

/// Lists the scenario scripts in the archives.
struct TalkScriptList: View {
    let urls: [URL]
    @State private var book: TalkBook?
    @State private var errorText: String?

    var body: some View {
        List {
            if let errorText {
                Text(errorText).foregroundStyle(.red).accessibilityIdentifier("error")
            }
            if let book {
                if book.scriptNames.isEmpty {
                    Text("No scenario scripts in these archives").accessibilityIdentifier("noScripts")
                }
                ForEach(Array(book.scriptNames.enumerated()), id: \.offset) { i, name in
                    NavigationLink(String(data: Data(name), encoding: StageReport.cp932) ?? "?") {
                        TalkView(book: book, scriptName: name)
                    }
                    .accessibilityIdentifier("script.\(i)")
                }
            } else if errorText == nil {
                ProgressView()
            }
        }
        .navigationTitle("Scenario scripts")
        .task {
            let urls = urls
            let result = await Task.detached { Result { try TalkBook(urls: urls) } }.value
            switch result {
            case .success(let b): book = b
            case .failure(let e): errorText = e.localizedDescription
            }
        }
    }
}

/// One script in the talk scene's text box, drawn as the game draws it (port/src/talk.h):
/// typewriter reveal one cell per frame after 30 frames, tap to show all, tap again for
/// the next page.
struct TalkView: View {
    let book: TalkBook
    let scriptName: [UInt8]
    @State private var pages: [TalkPage] = []
    @State private var index = 0
    @State private var counter = -30  // frames; the counter starts after a 30-frame delay
    @State private var end = 0
    @State private var image: CGImage?
    @State private var errorText: String?
    @State private var background: [UInt8]?
    @State private var player = SoundPlayer()
    @State private var musicPlayer = SoundPlayer()
    @State private var playingMusic: [UInt8] = []
    private let tick = Timer.publish(every: 1.0 / 60, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(spacing: 8) {
            ZStack {
                Color.black
                if let image {
                    Image(decorative: image, scale: 1).resizable().interpolation(.high)
                        .aspectRatio(4.0 / 3.0, contentMode: .fit)
                        .accessibilityIdentifier("talkFrame")
                }
            }
            .aspectRatio(4.0 / 3.0, contentMode: .fit)
            .contentShape(Rectangle())
            .onTapGesture { advance() }
            if let errorText {
                Text(errorText).foregroundStyle(.red).accessibilityIdentifier("error")
            }
            if !pages.isEmpty {
                Text("Page \(index + 1) of \(pages.count)" + (pages[index].voice.isEmpty ? "" : " · \(pages[index].voice)"))
                    .font(.footnote.monospacedDigit())
                    .accessibilityIdentifier("talkPage")
                if !book.hasWindowImage {
                    Text("data/talk/window.bmp not found; drawing text only").font(.footnote)
                }
            }
            Spacer()
        }
        .navigationTitle(String(data: Data(scriptName), encoding: StageReport.cp932) ?? "")
        .navigationBarTitleDisplayMode(.inline)
        .task {
            do {
                pages = try book.pages(script: scriptName)
                openPage()

                redraw()
            } catch {
                errorText = error.localizedDescription
            }
        }
        .onDisappear { player.stop(); musicPlayer.stop() }
        .onReceive(tick) { _ in
            guard !pages.isEmpty else { return }
            if counter < end {
                counter += 1
                if counter >= 0 { redraw() }
            } else {
                // Page fully shown: the arrow is on for 30 frames, off for 30.
                arrowFrame += 1
                if arrowFrame % 30 == 0 { redraw() }
            }
        }
    }

    @State private var arrowFrame = 0  // frames since the page was fully shown

    private func advance() {
        guard !pages.isEmpty else { return }
        if counter < end {
            counter = end  // a press shows the whole page
        } else if index + 1 < pages.count {
            index += 1
            counter = -30
            openPage()
            arrowFrame = 0
        }
        redraw()
    }

    private func redraw() {
        guard index < pages.count else { return }
        let arrow = counter >= end && (arrowFrame % 60) < 30
        let f = book.frame(pages[index], reveal: max(0, counter), arrow: arrow, background: background)
        end = f.end
        image = TalkBook.cgImage(f.rgba)
    }
}

extension TalkView {
    /// What the talk scene does when a page opens: its scene (background and portraits),
    /// its music if it changed, and its voice.
    fileprivate func openPage() {
        guard index < pages.count else { return }
        let page = pages[index]
        background = book.storyBackground(page: page.index)
        if page.bgm != playingMusic {
            playingMusic = page.bgm
            if let m = book.music(page.bgm) { musicPlayer.play(m) } else { musicPlayer.stop() }
        }
        if let v = book.voice(page) { player.play(v) } else { player.stop() }
    }
}
