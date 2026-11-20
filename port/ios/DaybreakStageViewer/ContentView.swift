import SwiftUI
import UniformTypeIdentifiers

/// Launch argument used by the UI test: load the bundled synthetic fixture instead of
/// waiting for the file picker.
let useBundledFixtureArgument = "-useBundledFixture"

struct ContentView: View {
    @State private var showPicker = false
    @State private var fileName: String?
    @State private var report: StageReport?
    @State private var expected = ExpectedValues.retailStage01
    @State private var errorText: String?
    @State private var loading = false

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Button(loading ? "Reading…" : "Choose daybreak00.dat") { showPicker = true }
                        .disabled(loading)
                    if loading { ProgressView() }
                    if let fileName { LabeledContent("File", value: fileName) }
                    if let errorText {
                        Text(errorText).foregroundStyle(.red).accessibilityIdentifier("error")
                    }
                }
                if let r = report { results(r) }
            }
            .navigationTitle("Stage 01")
            .fileImporter(isPresented: $showPicker, allowedContentTypes: [.item]) { result in
                switch result {
                case .failure(let error):
                    errorText = error.localizedDescription
                case .success(let url):
                    open(url, expecting: .retailStage01)
                }
            }
            .task {
                guard ProcessInfo.processInfo.arguments.contains(useBundledFixtureArgument) else { return }
                if let url = Bundle.main.url(forResource: "fixture", withExtension: "dat") {
                    open(url, expecting: .fixture)
                } else {
                    errorText = "fixture.dat is not in the app bundle"
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

    private func open(_ url: URL, expecting: ExpectedValues) {
        fileName = url.lastPathComponent
        report = nil
        errorText = nil
        expected = expecting
        loading = true
        Task.detached(priority: .userInitiated) {
            // Files picked from the Files app are outside the sandbox; the archive is read
            // in place (the real one is ~1.5 GB, so it is not copied).
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            let outcome = Result { try StageReport.load(path: url.path, stage: 1) }
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
