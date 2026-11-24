import AVFoundation
import SwiftUI

/// Plays one of the game's movies (port/src/movie.h: MPEG-1 decoded with pl_mpeg, streamed
/// from the archive). Frames follow the wall clock (late frames are dropped); the audio is
/// decoded on a background thread and queued ahead on its own player. Tap to skip.
final class MoviePlayer: ObservableObject {
    @Published private(set) var image: CGImage?
    @Published private(set) var finished = false
    let width: Int, height: Int
    private let archives: OpaquePointer
    private let movie: OpaquePointer
    private let scopedURLs: [URL]
    private let fps: Double
    private var frame = 0
    private var started: Date?
    private var pixels: [UInt8]
    private let engine = AVAudioEngine()
    private let node = AVAudioPlayerNode()
    private var audioThread: Thread?
    private var stopped = false
    private let lock = NSLock()

    init(urls: [URL], entry: String) throws {
        let scoped = urls.filter { $0.startAccessingSecurityScopedResource() }
        var err = [CChar](repeating: 0, count: 256)
        guard let a = withCPaths(urls.map(\.path), { db_archives_open($0, $1, &err, Int32(err.count)) }) else {
            scoped.forEach { $0.stopAccessingSecurityScopedResource() }
            throw ReportError.open(message(err))
        }
        let name = Array(entry.utf8)
        let opened = name.withUnsafeBufferPointer { b in
            b.baseAddress!.withMemoryRebound(to: CChar.self, capacity: b.count) {
                db_movie_open(a, $0, Int32(b.count), &err, Int32(err.count))
            }
        }
        guard let m = opened else {
            db_archives_free(a)
            scoped.forEach { $0.stopAccessingSecurityScopedResource() }
            throw ReportError.open(message(err))
        }
        archives = a
        movie = m
        scopedURLs = scoped
        width = Int(db_movie_width(m))
        height = Int(db_movie_height(m))
        fps = max(1, db_movie_fps(m))
        pixels = [UInt8](repeating: 0, count: width * height * 4)
    }

    deinit {
        stop()
        audioThread = nil
        db_movie_free(movie)
        db_archives_free(archives)
        scopedURLs.forEach { $0.stopAccessingSecurityScopedResource() }
    }

    /// Starts the audio and the clock; call `tick` every display frame after this.
    func start() {
        started = Date()
        let rate = Int(db_movie_sample_rate(movie))
        guard rate > 0, let format = AVAudioFormat(standardFormatWithSampleRate: Double(rate), channels: 2) else { return }
        engine.attach(node)
        engine.connect(node, to: engine.mainMixerNode, format: format)
        try? AVAudioSession.sharedInstance().setCategory(.playback)
        try? engine.start()
        node.play()
        let movie = movie
        let thread = Thread { [weak self] in
            // About a second per buffer; stay at most ~4 s ahead of the clock.
            var packet = [Int16](repeating: 0, count: 1152 * 2)
            var queued = 0.0
            while let self, !self.isStopped {
                if let s = self.started, queued - Date().timeIntervalSince(s) > 4 {
                    Thread.sleep(forTimeInterval: 0.1)
                    continue
                }
                guard let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(rate)) else { break }
                var filled = 0
                var ended = false
                while filled + 1152 <= rate {
                    let n = Int(db_movie_next_audio(movie, &packet, 1152))
                    if n == 0 {
                        ended = true
                        break
                    }
                    let l = buffer.floatChannelData![0], r = buffer.floatChannelData![1]
                    for i in 0..<n {
                        l[filled + i] = Float(packet[i * 2]) / 32768
                        r[filled + i] = Float(packet[i * 2 + 1]) / 32768
                    }
                    filled += n
                }
                buffer.frameLength = AVAudioFrameCount(filled)
                if filled > 0 { self.node.scheduleBuffer(buffer) }
                queued += Double(filled) / Double(rate)
                if ended { break }
            }
        }
        audioThread = thread
        thread.start()
    }

    private var isStopped: Bool {
        lock.lock()
        defer { lock.unlock() }
        return stopped
    }

    func stop() {
        lock.lock()
        stopped = true
        lock.unlock()
        node.stop()
        engine.stop()
    }

    /// Decodes up to the frame the clock is at (dropping late ones) and shows it.
    func tick() {
        guard let started, !finished else { return }
        let due = Int(Date().timeIntervalSince(started) * fps)
        var shown = false
        while frame <= due {
            var t = 0.0
            if db_movie_next_frame(movie, &pixels, Int32(pixels.count), &t) == 0 {
                finished = true
                stop()
                return
            }
            frame += 1
            shown = true
        }
        if shown { image = TalkBook.cgImage(pixels, width: width, height: height) }
    }
}

struct MovieView: View {
    let urls: [URL]
    let entry: String
    var onFinish: (() -> Void)? = nil
    @StateObject private var holder = MovieHolder()
    private let tick = Timer.publish(every: 1.0 / 60, on: .main, in: .common).autoconnect()

    var body: some View {
        ZStack {
            Color.black
            if let image = holder.player?.image {
                Image(decorative: image, scale: 1).resizable().interpolation(.medium)
                    .aspectRatio(4.0 / 3.0, contentMode: .fit)
                    .accessibilityIdentifier("movieFrame")
            }
            if let error = holder.error {
                VStack(spacing: 12) {
                    Text(error).foregroundStyle(.white).multilineTextAlignment(.center)
                    if let onFinish { Button("Continue", action: onFinish) }
                }
                .padding()
            }
        }
        .ignoresSafeArea()
        .contentShape(Rectangle())
        .onTapGesture { finish() }
        .task {
            let urls = urls, entry = entry
            let result = await Task.detached { Result { try MoviePlayer(urls: urls, entry: entry) } }.value
            switch result {
            case .success(let p):
                holder.player = p
                p.start()
            case .failure(let e):
                holder.error = "\(entry): \(e.localizedDescription)"
            }
        }
        .onReceive(tick) { _ in
            guard let p = holder.player else { return }
            p.tick()
            holder.objectWillChange.send()
            if p.finished { finish() }
        }
        .onDisappear { holder.player?.stop() }
        .navigationBarHidden(onFinish != nil)
    }

    private func finish() {
        holder.player?.stop()
        guard !holder.done else { return }
        holder.done = true
        onFinish?()
    }
}

final class MovieHolder: ObservableObject {
    var player: MoviePlayer?
    var error: String?
    var done = false
}

/// The game's movies, for the gallery.
struct MovieList: View {
    let urls: [URL]
    static let entries = ["movie/op.mpg", "movie/prologue.mpg", "movie/e00.mpg", "movie/e01.mpg", "movie/e02.mpg",
                          "movie/e03.mpg", "movie/e04.mpg", "movie/e05.mpg", "movie/e06.mpg", "movie/e07.mpg",
                          "movie/e08.mpg", "movie/e09.mpg", "movie/staffroll.mpg", "movie/staffroll2.mpg"]

    var body: some View {
        List(Self.entries, id: \.self) { e in
            NavigationLink(e) { MovieView(urls: urls, entry: e) }
        }
        .navigationTitle("Movies")
    }
}
