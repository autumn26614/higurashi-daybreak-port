import Foundation

/// Which daybreakNN.dat a file is, decided from its contents (port/src/identify.h), so a
/// renamed copy is still recognised and a wrong pick gets a clear message.
enum ArchiveRole: Int32, CaseIterable {
    case unknown = 0, base00, base01, base02, kai04, kai05, kai06

    var fileName: String {
        String(cString: db_role_file_name(rawValue))
    }
}

/// What db_identify_archive found in one picked or imported file.
struct ArchiveInfo: Equatable {
    let url: URL
    let role: ArchiveRole
    let entries: Int
    let scripts: Int
    let englishScripts: Int

    var hasEnglish: Bool { englishScripts > 0 }

    /// "English (88 of 504 scripts)" / "Japanese" / "no scripts".
    var languageSummary: String {
        if scripts == 0 { return "no scripts" }
        return hasEnglish ? "English (\(englishScripts) of \(scripts) scripts)" : "Japanese"
    }

    /// Reads the index and the scenario scripts (a few hundred small entries, not the
    /// whole file). The caller must hold security-scoped access. Call off the main thread.
    static func identify(_ url: URL) throws -> ArchiveInfo {
        var info = DBArchiveInfo()
        var err = [CChar](repeating: 0, count: 256)
        guard db_identify_archive(url.path, &info, &err, Int32(err.count)) == 1 else {
            throw ReportError.open("\(url.lastPathComponent): \(message(err))")
        }
        return ArchiveInfo(url: url, role: ArchiveRole(rawValue: info.role) ?? .unknown,
                           entries: Int(info.entries), scripts: Int(info.scripts),
                           englishScripts: Int(info.english_scripts))
    }
}

/// Archive search order. DaybreakDX.exe attaches daybreak04.dat before daybreak00.dat
/// and File::Open (0x6510a0) takes the first archive that has the name
/// (analysis/07-kai-archive-lookup.md).
enum ArchiveOrder {
    static func roles(for mode: ArchiveMode) -> [ArchiveRole] {
        mode == .kai ? [.kai04, .base00] : [.base00]
    }

    /// Music archives used when present, searched after the required ones: the base game
    /// opens daybreak01 (BGM); Kai opens daybreak05 (its BGM) and never daybreak01.
    static func optionalRoles(for mode: ArchiveMode) -> [ArchiveRole] {
        mode == .kai ? [.kai05] : [.base01]
    }

    enum OrderError: LocalizedError, Equatable {
        case missing([ArchiveRole], picked: [String])
        var errorDescription: String? {
            switch self {
            case .missing(let roles, let picked):
                let need = roles.map(\.fileName).joined(separator: " and ")
                let got = picked.isEmpty ? "nothing" : picked.joined(separator: ", ")
                return "Needs \(need) (picked: \(got))"
            }
        }
    }

    /// Puts identified archives in the game's search order for `mode`, then the mode's
    /// music archive if present. Other archives (movies, duplicates) are ignored; the
    /// first file of each role wins.
    static func order(_ infos: [ArchiveInfo], for mode: ArchiveMode) throws -> [ArchiveInfo] {
        let want = roles(for: mode)
        let ordered = want.compactMap { role in infos.first { $0.role == role } }
        guard ordered.count == want.count else {
            let missing = want.filter { role in !infos.contains { $0.role == role } }
            let picked = infos.map { info -> String in
                let name = info.url.lastPathComponent
                if info.role == .unknown { return "\(name) (not a Daybreak archive)" }
                // A renamed copy: say what it really is.
                return name.lowercased() == info.role.fileName ? name : "\(name) (is \(info.role.fileName))"
            }
            throw OrderError.missing(missing, picked: picked)
        }
        return ordered + optionalRoles(for: mode).compactMap { role in infos.first { $0.role == role } }
    }
}

/// Remembers the picked archives across launches with bookmarks (the files stay where
/// they are; daybreak00 is ~1.5 GB and daybreak04 ~1.3 GB, so they are not copied).
struct BookmarkStore {
    var defaults: UserDefaults = .standard

    private func key(_ mode: ArchiveMode) -> String { "archives.\(mode == .kai ? "kai" : "base")" }

    /// Call while security-scoped access to `urls` is held.
    func save(_ urls: [URL], for mode: ArchiveMode) throws {
        let data = try urls.map { try $0.bookmarkData(options: [], includingResourceValuesForKeys: nil,
                                                       relativeTo: nil) }
        defaults.set(data, forKey: key(mode))
    }

    /// The saved URLs, in search order, or nil if none are saved or one can no longer be
    /// resolved (moved to another provider, deleted).
    func load(for mode: ArchiveMode) -> [URL]? {
        guard let data = defaults.array(forKey: key(mode)) as? [Data], !data.isEmpty else { return nil }
        var urls: [URL] = []
        for item in data {
            var stale = false
            guard let url = try? URL(resolvingBookmarkData: item, options: [], relativeTo: nil,
                                     bookmarkDataIsStale: &stale) else { return nil }
            urls.append(url)
        }
        return urls
    }

    func clear(for mode: ArchiveMode) { defaults.removeObject(forKey: key(mode)) }
}

/// Archives the user copied into the app's own Documents folder (Files app: On My iPhone >
/// Daybreak, or Finder file sharing). These need no security scope.
enum DocumentsFolder {
    static var url: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    }

    /// Every .dat file directly inside `folder`, identified. Unreadable files are skipped.
    static func scan(_ folder: URL = url) -> [ArchiveInfo] {
        let files = (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? []
        return files
            .filter { $0.pathExtension.lowercased() == "dat" }
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
            .compactMap { try? ArchiveInfo.identify($0) }
    }
}

/// Runs `body` with security-scoped access to every URL (a no-op for files inside the
/// sandbox), released afterwards.
func withScopedAccess<R>(_ urls: [URL], _ body: () throws -> R) rethrows -> R {
    let scoped = urls.map { $0.startAccessingSecurityScopedResource() }
    defer {
        for (url, ok) in zip(urls, scoped) where ok { url.stopAccessingSecurityScopedResource() }
    }
    return try body()
}
