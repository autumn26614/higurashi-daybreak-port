import CoreFoundation
import Foundation

/// One texture referenced by the stage's visual meshes, resolved the way the engine does
/// (stage dir + name, extension swapped to "cnv"; see analysis/05-stage-loading.md).
struct TextureResult: Identifiable {
    let id: Int
    let name: String          // decoded from cp932
    let archiveName: String   // decoded from cp932
    let archiveNameBytes: [UInt8]  // raw cp932, for looking the entry up again
    let found: Bool
    let mips: Int
    let isJapanese: Bool      // name contains non-ASCII bytes
}

enum ReportError: LocalizedError {
    case open(String)
    var errorDescription: String? {
        switch self {
        case .open(let message): return "Could not read the archive: \(message)"
        }
    }
}

/// Swift view of the C API in port/src/daybreak_c.h.
struct StageReport {
    let stage: Int
    let plainMeshes: Int
    let plainTriangles: Int64
    let bMeshes: Int
    let bTriangles: Int64
    let parseErrors: Int
    let textures: [TextureResult]

    var resolvedCount: Int { textures.filter { $0.found }.count }
    var firstJapanese: TextureResult? { textures.first { $0.isJapanese } }

    /// Windows code page 932 (Shift-JIS with Microsoft extensions), the encoding of
    /// every name stored in the archive.
    static let cp932 = String.Encoding(rawValue: CFStringConvertEncodingToNSStringEncoding(
        CFStringEncoding(CFStringEncodings.dosJapanese.rawValue)))

    private static func decode(_ pointer: UnsafePointer<CChar>?, _ length: Int32) -> (text: String, nonASCII: Bool) {
        guard let pointer, length > 0 else { return ("", false) }
        let data = Data(bytes: pointer, count: Int(length))
        let nonASCII = data.contains { $0 >= 0x80 }
        if let text = String(data: data, encoding: cp932) { return (text, nonASCII) }
        // Undecodable bytes: show hex rather than dropping the entry.
        return (data.map { String(format: "%02x", $0) }.joined(separator: " "), nonASCII)
    }

    /// Reads the whole index plus every stage .x file; call off the main thread.
    static func load(path: String, stage: Int32) throws -> StageReport {
        try load(paths: [path], stage: stage)
    }

    /// Same, over several archives searched in order; the first that has a name wins
    /// (Kai: daybreak04.dat, then daybreak00.dat; see ArchiveOrder.order).
    static func load(paths: [String], stage: Int32) throws -> StageReport {
        var err = [CChar](repeating: 0, count: 256)
        let opened = withCPaths(paths) { argv, count in
            db_stage_report_open_set(argv, count, stage, &err, Int32(err.count))
        }
        guard let report = opened else { throw ReportError.open(message(err)) }
        defer { db_stage_report_free(report) }

        var textures: [TextureResult] = []
        let count = db_stage_texture_count(report)
        for i in 0..<max(count, 0) {
            var nameLength: Int32 = 0
            var archiveLength: Int32 = 0
            let name = decode(db_stage_texture_name(report, i, &nameLength), nameLength)
            let archivePointer = db_stage_texture_archive_name(report, i, &archiveLength)
            let archive = decode(archivePointer, archiveLength)
            let archiveBytes = archivePointer.map {
                Array(UnsafeRawBufferPointer(start: $0, count: Int(max(archiveLength, 0))))
            } ?? []
            textures.append(TextureResult(
                id: Int(i),
                name: name.text,
                archiveName: archive.text,
                archiveNameBytes: archiveBytes,
                found: db_stage_texture_found(report, i) == 1,
                mips: Int(db_stage_texture_mips(report, i)),
                isJapanese: name.nonASCII))
        }
        return StageReport(
            stage: Int(stage),
            plainMeshes: Int(db_stage_mesh_count(report, 0)),
            plainTriangles: db_stage_triangle_count(report, 0),
            bMeshes: Int(db_stage_mesh_count(report, 1)),
            bTriangles: db_stage_triangle_count(report, 1),
            parseErrors: Int(db_stage_error_count(report)),
            textures: textures)
    }
}

/// Calls `body` with a C array of the paths (`const char* const*`, count), valid only
/// during the call.
func withCPaths<R>(_ paths: [String], _ body: (UnsafePointer<UnsafePointer<CChar>?>?, Int32) -> R) -> R {
    let cPaths = paths.map { strdup($0) }
    defer { cPaths.forEach { free($0) } }
    let cArgs: [UnsafePointer<CChar>?] = cPaths.map { $0.map { UnsafePointer<CChar>($0) } }
    return cArgs.withUnsafeBufferPointer { body($0.baseAddress, Int32($0.count)) }
}

/// The NUL-terminated message a C API call wrote into `err`.
func message(_ err: [CChar]) -> String {
    String(decoding: err.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)
}

/// Values the screen checks against (green ✓ / red ✗).
struct ExpectedValues {
    let label: String
    let plainMeshes: Int
    let plainTriangles: Int64
    let bMeshes: Int
    let bTriangles: Int64
    let textures: Int
    let resolved: Int
    let firstJapaneseName: String
    let firstJapaneseArchiveName: String

    /// Real daybreak00.dat, stage 01, as measured on x86-64 Linux, Windows and aarch64
    /// (QEMU) builds of the same reader. See port/ios/README.md.
    static let retailStage01 = ExpectedValues(
        label: "daybreak00.dat stage 01",
        plainMeshes: 27, plainTriangles: 28_372, bMeshes: 26, bTriangles: 28_148,
        textures: 85, resolved: 85,
        firstJapaneseName: "くさむら06.bmp",
        firstJapaneseArchiveName: "data/stage01/くさむら06.cnv")

    /// Kai: daybreak04.dat searched before daybreak00.dat, stage 16. Stage 16 exists only
    /// in daybreak04, so these values cannot come from the base archive alone. Measured
    /// with `stageinfo daybreak00.dat --overlay daybreak04.dat 16` on x86-64 Linux.
    static let kaiStage16 = ExpectedValues(
        label: "Kai (04 → 00) stage 16",
        plainMeshes: 40, plainTriangles: 23_961, bMeshes: 40, bTriangles: 23_961,
        textures: 38, resolved: 38,
        firstJapaneseName: "アスファルト05.bmp",
        firstJapaneseArchiveName: "data/stage16/アスファルト05.cnv")

    /// Synthetic Fixtures/fixture.dat written by port/tools/make_fixture.py.
    static let fixture = ExpectedValues(
        label: "test fixture",
        plainMeshes: 1, plainTriangles: 1, bMeshes: 0, bTriangles: 0,
        textures: 1, resolved: 1,
        firstJapaneseName: "テスト草01.bmp",
        firstJapaneseArchiveName: "data/stage01/テスト草01.cnv")

    /// Fixtures/fixture-overlay.dat searched before fixture.dat: the overlay's two-triangle
    /// mesh replaces the base one, and the texture still resolves from the base.
    static let fixtureOverlay = ExpectedValues(
        label: "test fixture + overlay",
        plainMeshes: 1, plainTriangles: 2, bMeshes: 0, bTriangles: 0,
        textures: 1, resolved: 1,
        firstJapaneseName: "テスト草01.bmp",
        firstJapaneseArchiveName: "data/stage01/テスト草01.cnv")
}
