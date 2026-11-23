import CoreGraphics
import CoreText
import Foundation

/// Glyph coverage masks for the talk box. The game asks GDI for bold "ＭＳ ゴシック" at
/// height 24 with GGO_GRAY4_BITMAP (coverage 0...16). MS Gothic is not on iOS, so
/// full-width characters use Hiragino Sans W6 and half-width ones Menlo Bold, each drawn
/// into the game's fixed cell (24 or 12 px wide, 24 tall, baseline 21 like MS Gothic's
/// ascent) and quantised to the same 17 levels.
final class GlyphRasterizer {
    static let cellHeight = 24
    static let baseline = 21  // from the cell top

    private let fullWidth = CTFontCreateWithName("HiraginoSans-W6" as CFString, 22, nil)
    private let halfWidth = CTFontCreateWithName("Menlo-Bold" as CFString, 19.5, nil)
    private var cache: [UInt32: Mask] = [:]

    struct Mask {
        let width: Int, height: Int
        let coverage: [UInt8]  // 0...16, top row first
    }

    /// `code`: one cp932 byte, or lead << 8 | trail.
    func mask(code: UInt32, bytes: Int) -> Mask {
        if let m = cache[code] { return m }
        let raw: [UInt8] = bytes == 2 ? [UInt8(code >> 8), UInt8(code & 0xFF)] : [UInt8(code & 0xFF)]
        let text = String(data: Data(raw), encoding: StageReport.cp932) ?? "?"
        let width = bytes == 2 ? 24 : 12
        let m = draw(text, width: width, font: bytes == 2 ? fullWidth : halfWidth)
        cache[code] = m
        return m
    }

    private func draw(_ text: String, width: Int, font: CTFont) -> Mask {
        let h = Self.cellHeight
        var pixels = [UInt8](repeating: 0, count: width * h)
        pixels.withUnsafeMutableBytes { buf in
            guard let ctx = CGContext(data: buf.baseAddress, width: width, height: h, bitsPerComponent: 8,
                                      bytesPerRow: width, space: CGColorSpaceCreateDeviceGray(),
                                      bitmapInfo: CGImageAlphaInfo.none.rawValue) else { return }
            ctx.setFillColor(gray: 1, alpha: 1)
            ctx.setShouldAntialias(true)
            let line = CTLineCreateWithAttributedString(NSAttributedString(
                string: text, attributes: [NSAttributedString.Key(kCTFontAttributeName as String): font,
                                           NSAttributedString.Key(kCTForegroundColorFromContextAttributeName as String): true]))
            // Centre the glyph in its cell horizontally; baseline 21 px below the top.
            let advance = CGFloat(CTLineGetTypographicBounds(line, nil, nil, nil))
            ctx.textPosition = CGPoint(x: max(0, (CGFloat(width) - advance) / 2), y: CGFloat(h - Self.baseline))
            CTLineDraw(line, ctx)
        }
        // CGContext rows run bottom-up in memory only for flipped contexts; this bitmap
        // context's first row is the top of the image.
        let coverage = pixels.map { UInt8((Int($0) * 16 + 127) / 255) }
        return Mask(width: width, height: h, coverage: coverage)
    }
}

/// One page of a scenario script: the text line and the voice clip before it.
struct TalkPage: Equatable {
    let voice: String
    let text: [UInt8]  // raw cp932, commands included
    var voiceEntry: [UInt8] = []  // archive entry of the voice clip (story scripts)
    var bgm: [UInt8] = []         // archive entry of the music playing on this page
    var index = 0                 // page number in its story
}

/// The talk scene's text box over a script, through the C API (port/src/talk.h).
final class TalkBook {
    let scriptNames: [[UInt8]]
    private let archives: OpaquePointer
    private let window: (width: Int, height: Int, rgba: Data)?
    private let glyphs = GlyphRasterizer()
    private let scopedURLs: [URL]
    private var story: OpaquePointer?  // the script last opened with pages(script:)

    init(urls: [URL]) throws {
        let scoped = urls.filter { $0.startAccessingSecurityScopedResource() }
        var err = [CChar](repeating: 0, count: 256)
        guard let a = withCPaths(urls.map(\.path), { db_archives_open($0, $1, &err, Int32(err.count)) }) else {
            scoped.forEach { $0.stopAccessingSecurityScopedResource() }
            throw ReportError.open(message(err))
        }
        archives = a
        scopedURLs = scoped
        scriptNames = (0..<db_archives_script_count(a)).compactMap { i in
            var len: Int32 = 0
            guard let p = db_archives_script_name(a, i, &len) else { return nil }
            return Array(UnsafeRawBufferPointer(start: p, count: Int(len)))
        }
        let name = Array("data/talk/window.bmp".utf8)
        let image = name.withUnsafeBufferPointer { b in
            b.baseAddress!.withMemoryRebound(to: CChar.self, capacity: b.count) {
                db_image_load(a, $0, Int32(b.count), &err, Int32(err.count))
            }
        }
        if let image {
            let w = Int(db_image_width(image)), h = Int(db_image_height(image))
            window = (w, h, Data(bytes: db_image_rgba(image), count: w * h * 4))
            db_image_free(image)
        } else {
            window = nil
        }
    }

    deinit {
        if let story { db_story_free(story) }
        db_archives_free(archives)
        scopedURLs.forEach { $0.stopAccessingSecurityScopedResource() }
    }

    var hasWindowImage: Bool { window != nil }

    static func pages(ofScript bytes: [UInt8]) -> [TalkPage] {
        guard let s = db_script_parse(bytes, Int32(bytes.count)) else { return [] }
        defer { db_script_free(s) }
        return pages(s)
    }

    /// Opens a script as the talk scene plays it (port/src/story.h): text pages with their
    /// voice, music and scene. Keeps it open for storyBackground(page:).
    func pages(script name: [UInt8]) throws -> [TalkPage] {
        var err = [CChar](repeating: 0, count: 256)
        let s = name.withUnsafeBufferPointer { b in
            b.baseAddress!.withMemoryRebound(to: CChar.self, capacity: b.count) {
                db_story_load(archives, $0, Int32(b.count), &err, Int32(err.count))
            }
        }
        guard let s else { throw ReportError.open(message(err)) }
        if let story { db_story_free(story) }
        story = s
        func bytes(_ p: UnsafePointer<CChar>?, _ len: Int32) -> [UInt8] {
            p.map { Array(UnsafeRawBufferPointer(start: $0, count: Int(len))) } ?? []
        }
        return (0..<db_story_page_count(s)).map { i in
            var len: Int32 = 0
            let text = bytes(db_story_page_text(s, i, &len), len)
            let voice = bytes(db_story_page_voice(s, i, &len), len)
            let bgm = bytes(db_story_page_bgm(s, i, &len), len)
            let shown = String(decoding: voice.split(separator: UInt8(ascii: "/")).last ?? [], as: UTF8.self)
            return TalkPage(voice: shown, text: text, voiceEntry: voice, bgm: bgm, index: Int(i))
        }
    }

    /// The page's 800x600 background (scene and portraits) from the open script.
    func storyBackground(page: Int) -> [UInt8]? {
        guard let story else { return nil }
        var out = [UInt8](repeating: 0, count: Int(DB_TALK_WIDTH) * Int(DB_TALK_HEIGHT) * 4)
        return db_story_background(archives, story, Int32(page), &out) == 1 ? out : nil
    }

    /// The music entry decoded (music loops at its .sfl points).
    func music(_ entry: [UInt8]) -> DecodedSound? {
        entry.isEmpty ? nil : try? DecodedSound.load(archives, entry)
    }

    private static func pages(_ s: OpaquePointer) -> [TalkPage] {
        (0..<db_script_page_count(s)).map { p in
            var len: Int32 = 0
            let v = db_script_page_voice(s, p, &len)
            let voice = v.map { String(decoding: UnsafeRawBufferPointer(start: $0, count: Int(len)), as: UTF8.self) } ?? ""
            var text: [UInt8] = []
            for t in 0..<db_script_token_count(s, p) {
                if let b = db_script_token_bytes(s, p, t, &len) {
                    text += UnsafeRawBufferPointer(start: b, count: Int(len))
                }
            }
            return TalkPage(voice: voice, text: text)
        }
    }

    /// Lays the page out and composes the 800x600 talk frame (RGBA). `reveal` is the
    /// game's counter (cells shown); returns the frame and the counter's end (strlen).
    /// The script's 800x600 background: win quotes show both characters' portraits.
    func background(script name: [UInt8]) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: Int(DB_TALK_WIDTH) * Int(DB_TALK_HEIGHT) * 4)
        name.withUnsafeBufferPointer { b in
            guard let p = b.baseAddress else { return }
            p.withMemoryRebound(to: CChar.self, capacity: b.count) { _ = db_talk_background(archives, $0, Int32(b.count), &out) }
        }
        return out
    }

    /// The page's voice clip, decoded, if the archives have it.
    func voice(_ page: TalkPage) -> DecodedSound? {
        if !page.voiceEntry.isEmpty { return try? DecodedSound.load(archives, page.voiceEntry) }
        guard !page.voice.isEmpty else { return nil }
        var out = [CChar](repeating: 0, count: 256)
        let bytes = Array(page.voice.utf8)
        let n = bytes.withUnsafeBufferPointer { b in
            b.baseAddress!.withMemoryRebound(to: CChar.self, capacity: b.count) {
                db_voice_entry(archives, $0, Int32(b.count), &out, Int32(out.count))
            }
        }
        guard n > 0 else { return nil }
        return try? DecodedSound.load(archives, out.prefix(Int(n)).map { UInt8(bitPattern: $0) })
    }

    func frame(_ page: TalkPage, reveal: Int, arrow: Bool, background: [UInt8]? = nil) -> (rgba: [UInt8], end: Int, stopped: Bool) {
        let t = page.text.withUnsafeBufferPointer { b in
            b.baseAddress.map { $0.withMemoryRebound(to: CChar.self, capacity: b.count) { db_talk_layout($0, Int32(b.count)) } }
                ?? db_talk_layout(nil, 0)
        }
        guard let t else { return ([], 0, false) }
        defer { db_talk_free(t) }
        var seen = Set<UInt32>()
        for i in 0..<db_talk_glyph_count(t) {
            var code: UInt32 = 0, bytes: Int32 = 0, x: Int32 = 0, y: Int32 = 0
            guard db_talk_glyph(t, i, &code, &bytes, &x, &y) == 1, seen.insert(code).inserted else { continue }
            let m = glyphs.mask(code: code, bytes: Int(bytes))
            db_talk_set_mask(t, code, m.coverage, Int32(m.width), Int32(m.height), 0, 0)
        }
        let size = Int(DB_TALK_WIDTH) * Int(DB_TALK_HEIGHT) * 4
        var out = [UInt8](repeating: 0, count: size)
        let bg = background?.count == size ? background : nil
        func compose(_ bgp: UnsafePointer<UInt8>?) {
            if let w = window {
                w.rgba.withUnsafeBytes { wb in
                    _ = db_talk_compose(t, bgp, wb.bindMemory(to: UInt8.self).baseAddress, Int32(w.width),
                                        Int32(w.height), Int32(reveal), arrow ? 1 : 0, &out)
                }
            } else {
                _ = db_talk_compose(t, bgp, nil, 0, 0, Int32(reveal), arrow ? 1 : 0, &out)
            }
        }
        if let bg {
            bg.withUnsafeBufferPointer { compose($0.baseAddress) }
        } else {
            compose(nil)
        }
        return (out, Int(db_talk_text_bytes(t)), db_talk_stopped(t) != 0)
    }

    static func cgImage(_ rgba: [UInt8], width: Int = 800, height: Int = 600) -> CGImage? {
        guard rgba.count == width * height * 4, let provider = CGDataProvider(data: Data(rgba) as CFData) else {
            return nil
        }
        return CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: width * 4,
                       space: CGColorSpace(name: CGColorSpace.sRGB)!,
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
    }
}
