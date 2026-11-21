import SwiftUI
import UniformTypeIdentifiers

/// Launch argument used by the UI test: load the bundled synthetic fixture instead of
/// waiting for the file picker.
let useBundledFixtureArgument = "-useBundledFixture"
/// With -useBundledFixture: also search Fixtures/fixture-overlay.dat first (Kai mode).
let kaiArgument = "-kai"

/// Which archives are searched. Base game: daybreak00.dat alone, stage 01.
/// Kai expansion: daybreak04.dat, then daybreak00.dat, stage 16 (Kai-only).
enum ArchiveMode: String, CaseIterable, Identifiable {
    case base = "Base game"
    case kai = "Kai"
    var id: Self { self }
    var stage: Int32 { self == .kai ? 16 : 1 }
    var pickLabel: String {
        self == .kai ? "Choose daybreak00.dat + daybreak04.dat" : "Choose daybreak00.dat"
    }
}

struct ContentView: View {
    @State private var mode = ArchiveMode.base
    @State private var showPicker = false
    @State private var fileNames: [String] = []
    @State private var stage: Int32 = 1
    @State private var report: StageReport?
    @State private var expected = ExpectedValues.retailStage01
    @State private var errorText: String?
    @State private var loading = false

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Picker("Archives", selection: $mode) {
                        ForEach(ArchiveMode.allCases) { Text($0.rawValue).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    .disabled(loading)
                    .accessibilityIdentifier("mode")
                    Button(loading ? "Reading…" : mode.pickLabel) { showPicker = true }
                        .disabled(loading)
                    if loading { ProgressView() }
                    if !fileNames.isEmpty {
                        LabeledContent("Search order", value: fileNames.joined(separator: " → "))
                            .accessibilityIdentifier("searchOrder")
                    }
                    if let errorText {
                        Text(errorText).foregroundStyle(.red).accessibilityIdentifier("error")
                    }
                }
                if let r = report { results(r) }
            }
            .navigationTitle(String(format: "Stage %02d", stage))
            .fileImporter(isPresented: $showPicker, allowedContentTypes: [.item],
                          allowsMultipleSelection: mode == .kai) { result in
                switch result {
                case .failure(let error):
                    errorText = error.localizedDescription
                case .success(let urls):
                    if mode == .kai {
                        do {
                            let ordered = try ArchiveOrder.kai(urls)
                            open(ordered, stage: 16, expecting: .kaiStage16)
                        } catch {
                            report = nil
                            errorText = error.localizedDescription
                        }
                    } else if let url = urls.first {
                        open([url], stage: 1, expecting: .retailStage01)
                    }
                }
            }
            .task {
                let args = ProcessInfo.processInfo.arguments
                guard args.contains(useBundledFixtureArgument) else { return }
                guard let base = Bundle.main.url(forResource: "fixture", withExtension: "dat") else {
                    errorText = "fixture.dat is not in the app bundle"
                    return
                }
                if args.contains(kaiArgument) {
                    mode = .kai
                    guard let overlay = Bundle.main.url(forResource: "fixture-overlay", withExtension: "dat") else {
                        errorText = "fixture-overlay.dat is not in the app bundle"
                        return
                    }
                    open([overlay, base], stage: 1, expecting: .fixtureOverlay)
                } else {
                    open([base], stage: 1, expecting: .fixture)
                }
            }
        }
    }

    @ViewBuilder
    private func results(_ r: StageReport) -> some View {
        Section("Meshes (slots 00–07), expected: \(expected.label)") {
            check("Plain set (game default)", "\(r.plainMeshes) meshes / \(r.plainTriangles) tris",
                  r.plainMeshes == expected.plainMeshes && r.plainTriangles == expected.plainTriangles,
                  id: "plainSet")
            check("b set", "\(r.bMeshes) meshes / \(r.bTriangles) tris",
                  r.bMeshes == expected.bMeshes && r.bTriangles == expected.bTriangles, id: "bSet")
            if r.parseErrors > 0 {
                Text("\(r.parseErrors) .x files failed to parse").foregroundStyle(.red)
            }
        }
        Section("Textures") {
            check("Referenced", "\(r.textures.count)", r.textures.count == expected.textures, id: "texturesReferenced")
            check("Resolved in archive", "\(r.resolvedCount)", r.resolvedCount == expected.resolved,
                  id: "texturesResolved")
        }
        Section("Japanese filename (cp932 → String)") {
            if let jp = r.firstJapanese {
                check("Texture", jp.name, jp.name == expected.firstJapaneseName, id: "japaneseName")
                check("Archive entry", jp.archiveName, jp.archiveName == expected.firstJapaneseArchiveName,
                      id: "japaneseArchiveName")
            } else {
                Text("No Japanese texture names found").foregroundStyle(.red)
            }
        }
        Section("All textures") {
            ForEach(r.textures) { t in
                HStack {
                    Text(t.name).lineLimit(1)
                    Spacer()
                    Text(t.found ? (t.mips > 0 ? "ok +\(t.mips)" : "ok") : "missing")
                        .foregroundStyle(t.found ? Color.secondary : Color.red)
                }
            }
        }
    }

    private func check(_ label: String, _ value: String, _ ok: Bool, id: String) -> some View {
        LabeledContent(label) {
            HStack(spacing: 6) {
                Text(value).accessibilityIdentifier(id)
                Image(systemName: ok ? "checkmark.circle.fill" : "xmark.circle.fill")
                    .foregroundStyle(ok ? .green : .red)
                    .accessibilityIdentifier(id + (ok ? ".pass" : ".fail"))
            }
        }
    }

    /// `urls` are in search order: the first archive that has a name wins.
    private func open(_ urls: [URL], stage: Int32, expecting: ExpectedValues) {
        fileNames = urls.map { $0.lastPathComponent }
        self.stage = stage
        report = nil
        errorText = nil
        expected = expecting
        loading = true
        Task.detached(priority: .userInitiated) {
            // Files picked from the Files app are outside the sandbox; the archives are read
            // in place (daybreak00 is ~1.5 GB and daybreak04 ~1.3 GB, so they are not copied).
            let scoped = urls.map { $0.startAccessingSecurityScopedResource() }
            defer {
                for (url, ok) in zip(urls, scoped) where ok { url.stopAccessingSecurityScopedResource() }
            }
            let outcome = Result { try StageReport.load(paths: urls.map(\.path), stage: stage) }
            await MainActor.run {
                loading = false
                switch outcome {
                case .success(let r): report = r
                case .failure(let error): errorText = error.localizedDescription
                }
            }
        }
    }
}
