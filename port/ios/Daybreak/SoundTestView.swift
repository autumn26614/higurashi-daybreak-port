import SwiftUI

/// The game's music (looping as the game loops it) and sound effects.
struct SoundTestView: View {
    let urls: [URL]
    @State private var library: SoundLibrary?
    @State private var playing: Int?
    @State private var errorText: String?
    @State private var player = SoundPlayer()

    var body: some View {
        List {
            if let errorText { Text(errorText).foregroundStyle(.red).accessibilityIdentifier("error") }
            if let library {
                if library.names.isEmpty { Text("No music or sound effects in these archives").accessibilityIdentifier("noSounds") }
                ForEach(Array(library.names.enumerated()), id: \.offset) { i, n in
                    Button {
                        toggle(i)
                    } label: {
                        HStack {
                            Image(systemName: playing == i ? "stop.circle.fill" : "play.circle")
                            Text(String(data: Data(n), encoding: StageReport.cp932) ?? "?")
                        }
                    }
                    .accessibilityIdentifier("sound.\(i)")
                }
            } else if errorText == nil {
                ProgressView()
            }
        }
        .navigationTitle("Sound test")
        .task {
            let urls = urls
            let r = await Task.detached { Result { try SoundLibrary(urls: urls) } }.value
            switch r {
            case .success(let l): library = l
            case .failure(let e): errorText = e.localizedDescription
            }
        }
        .onDisappear { player.stop() }
    }

    private func toggle(_ i: Int) {
        guard let library else { return }
        if playing == i {
            player.stop()
            playing = nil
            return
        }
        do {
            player.play(try library.sound(i))
            playing = i
        } catch {
            errorText = error.localizedDescription
        }
    }
}

/// Archives kept open for decoding sounds on demand.
final class SoundLibrary {
    let names: [[UInt8]]
    let archives: OpaquePointer
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
        names = (0..<db_archives_sound_count(a)).compactMap { i in
            var len: Int32 = 0
            guard let p = db_archives_sound_name(a, i, &len) else { return nil }
            return Array(UnsafeRawBufferPointer(start: p, count: Int(len)))
        }
    }

    deinit {
        db_archives_free(archives)
        scopedURLs.forEach { $0.stopAccessingSecurityScopedResource() }
    }

    func sound(_ i: Int) throws -> DecodedSound { try DecodedSound.load(archives, names[i]) }

    /// The voice clip of a scenario VOICE argument, if the archives have it.
    func voice(_ file: String) -> DecodedSound? {
        var out = [CChar](repeating: 0, count: 256)
        let bytes = Array(file.utf8)
        let n = bytes.withUnsafeBufferPointer { b in
            b.baseAddress.map { p in
                p.withMemoryRebound(to: CChar.self, capacity: b.count) { db_voice_entry(archives, $0, Int32(b.count), &out, Int32(out.count)) }
            } ?? 0
        }
        guard n > 0 else { return nil }
        let entry = out.prefix(Int(n)).map { UInt8(bitPattern: $0) }
        return try? DecodedSound.load(archives, entry)
    }
}
