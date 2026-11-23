import SwiftUI

/// Decodes one stage texture from the archives (same search order as the report) and
/// shows it at its stored pixels, without smoothing.
struct TexturePreview: View {
    let urls: [URL]               // search order, as passed to StageReport.load
    let texture: TextureResult

    @State private var image: TextureImage?
    @State private var errorText: String?

    var body: some View {
        List {
            Section {
                if let image {
                    Image(decorative: image.cgImage, scale: 1)
                        .interpolation(.none)
                        .resizable()
                        .aspectRatio(contentMode: .fit)
                        .frame(maxWidth: .infinity, maxHeight: 360)
                        .background(Color.gray.opacity(0.25))
                    LabeledContent("Size") {
                        Text("\(image.width) × \(image.height) px, \(image.bpp) bpp")
                            .accessibilityIdentifier("textureSize")
                    }
                } else if let errorText {
                    Text(errorText).foregroundStyle(.red).accessibilityIdentifier("error")
                } else {
                    ProgressView()
                }
                LabeledContent("Entry", value: texture.archiveName)
            }
        }
        .navigationTitle(texture.name)
        .navigationBarTitleDisplayMode(.inline)
        .task {
            let urls = urls
            let name = texture.archiveNameBytes
            let outcome = await Task.detached(priority: .userInitiated) { () -> Result<TextureImage, Error> in
                let scoped = urls.map { $0.startAccessingSecurityScopedResource() }
                defer {
                    for (url, ok) in zip(urls, scoped) where ok { url.stopAccessingSecurityScopedResource() }
                }
                return Result { try TextureImage.load(paths: urls.map(\.path), name: name) }
            }.value
            switch outcome {
            case .success(let decoded): image = decoded
            case .failure(let error): errorText = error.localizedDescription
            }
        }
    }
}
