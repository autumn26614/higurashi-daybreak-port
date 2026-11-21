import CoreGraphics
import Foundation

/// One decoded .cnv texture (port/src/cnv.h through the C API in daybreak_c.h).
struct TextureImage {
    let width: Int
    let height: Int
    let bpp: Int
    let rgba: Data           // width*height*4, R,G,B,A, top row first, straight alpha
    let cgImage: CGImage

    /// Opens `paths` (in search order) and decodes the entry named by raw cp932 bytes.
    /// A ".bmp" name is mapped to ".cnv" as the engine does. Call off the main thread.
    static func load(paths: [String], name: [UInt8]) throws -> TextureImage {
        guard !name.isEmpty else { throw ReportError.open("no texture name") }
        var err = [CChar](repeating: 0, count: 256)
        let opened = withCPaths(paths) { argv, count in
            db_archives_open(argv, count, &err, Int32(err.count))
        }
        guard let archives = opened else { throw ReportError.open(message(err)) }
        defer { db_archives_free(archives) }

        let loaded = name.withUnsafeBufferPointer { bytes in
            bytes.baseAddress!.withMemoryRebound(to: CChar.self, capacity: bytes.count) {
                db_image_load(archives, $0, Int32(bytes.count), &err, Int32(err.count))
            }
        }
        guard let image = loaded else { throw ReportError.open(message(err)) }
        defer { db_image_free(image) }

        let width = Int(db_image_width(image))
        let height = Int(db_image_height(image))
        let rgba = Data(bytes: db_image_rgba(image), count: width * height * 4)
        guard let provider = CGDataProvider(data: rgba as CFData),
              let cgImage = CGImage(
                width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: width * 4,
                space: CGColorSpace(name: CGColorSpace.sRGB)!,
                bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue),
                provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
        else { throw ReportError.open("could not make a CGImage of \(width)x\(height)") }
        return TextureImage(width: width, height: height, bpp: Int(db_image_bpp(image)), rgba: rgba,
                            cgImage: cgImage)
    }
}
