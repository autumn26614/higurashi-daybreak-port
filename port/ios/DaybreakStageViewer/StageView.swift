import MetalKit
import SwiftUI
import UIKit

/// Orbit camera around a target, in the game's left-handed world (y up). yaw 0 looks
/// along +z (eye on the -z side), as the default battle-like view does.
struct OrbitCamera: Equatable {
    var target: SIMD3<Float>
    var yaw: Float
    var pitch: Float
    var distance: Float

    init(_ c: StageCamera) {
        target = c.target
        let d = c.target - c.eye
        distance = max(simd_length(d), 1)
        pitch = asin(max(-1, min(1, -d.y / distance)))
        yaw = atan2(d.x, d.z)
    }

    var eye: SIMD3<Float> {
        let dir = SIMD3(cos(pitch) * sin(yaw), -sin(pitch), cos(pitch) * cos(yaw))  // eye -> target
        return target - dir * distance
    }

    func camera(from base: StageCamera) -> StageCamera {
        var c = base
        c.eye = eye
        c.target = target
        return c
    }

    mutating func orbit(dx: Float, dy: Float) {
        yaw += dx * 0.005
        pitch = max(-1.5, min(1.5, pitch + dy * 0.005))
    }

    mutating func zoom(_ scale: Float) {
        distance = max(50, min(150_000, distance / max(scale, 0.01)))
    }

    /// Moves the target in the view plane, proportional to the distance.
    mutating func pan(dx: Float, dy: Float) {
        let forward = simd_normalize(target - eye)
        let right = simd_normalize(simd_cross(SIMD3<Float>(0, 1, 0), forward))  // LH: up x forward = right
        let up = simd_cross(forward, right)
        let k = distance * 0.0015
        target += (-right * dx + up * dy) * k
    }
}

/// UIKit host for the Metal view with the camera gestures (one finger orbits, two pan,
/// pinch zooms).
struct MetalStageView: UIViewRepresentable {
    let renderer: StageRenderer
    @Binding var orbit: OrbitCamera
    let base: StageCamera

    func makeUIView(context: Context) -> MTKView {
        let v = MTKView(frame: .zero, device: renderer.device)
        v.colorPixelFormat = StageRenderer.colorFormat
        v.depthStencilPixelFormat = StageRenderer.depthFormat
        v.delegate = renderer
        v.preferredFramesPerSecond = 60
        v.isPaused = false
        v.enableSetNeedsDisplay = false
        v.accessibilityIdentifier = "metalStageView"
        let c = context.coordinator
        let one = UIPanGestureRecognizer(target: c, action: #selector(Coordinator.orbit(_:)))
        one.maximumNumberOfTouches = 1
        let two = UIPanGestureRecognizer(target: c, action: #selector(Coordinator.pan(_:)))
        two.minimumNumberOfTouches = 2
        let pinch = UIPinchGestureRecognizer(target: c, action: #selector(Coordinator.pinch(_:)))
        [one, two, pinch].forEach { v.addGestureRecognizer($0) }
        two.delegate = c
        pinch.delegate = c
        return v
    }

    func updateUIView(_ v: MTKView, context: Context) {
        context.coordinator.parent = self
        renderer.camera = orbit.camera(from: base)
    }

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    final class Coordinator: NSObject, UIGestureRecognizerDelegate {
        var parent: MetalStageView
        init(_ p: MetalStageView) { parent = p }

        @objc func orbit(_ g: UIPanGestureRecognizer) {
            let t = g.translation(in: g.view)
            parent.orbit.orbit(dx: Float(t.x), dy: Float(t.y))
            g.setTranslation(.zero, in: g.view)
        }

        @objc func pan(_ g: UIPanGestureRecognizer) {
            let t = g.translation(in: g.view)
            parent.orbit.pan(dx: Float(t.x), dy: Float(t.y))
            g.setTranslation(.zero, in: g.view)
        }

        @objc func pinch(_ g: UIPinchGestureRecognizer) {
            parent.orbit.zoom(Float(g.scale))
            g.scale = 1
        }

        func gestureRecognizer(_ g: UIGestureRecognizer,
                               shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer) -> Bool { true }
    }
}

/// The stage in 3D, drawn as the game draws it (port/src/recipe.h). Falls back to the CPU
/// reference renderer when the device has no Metal.
struct StageView: View {
    let urls: [URL]  // search order
    @State var stage: Int
    @State private var variant = 0
    @State private var stages: [Int] = []
    @State private var scene: StageScene?
    @State private var renderer: StageRenderer?
    @State private var rendererError: String?
    @State private var orbit: OrbitCamera?
    @State private var cpuImage: CGImage?
    @State private var errorText: String?
    @State private var loading = false

    var body: some View {
        VStack(spacing: 0) {
            controls
            ZStack {
                Color.black
                if let scene, let orbit {
                    if let renderer {
                        MetalStageView(renderer: renderer, orbit: Binding(get: { orbit }, set: { self.orbit = $0 }),
                                       base: scene.defaultCamera)
                    } else if let cpuImage {
                        Image(decorative: cpuImage, scale: 1).resizable().aspectRatio(contentMode: .fit)
                    }
                }
                if loading { ProgressView().tint(.white) }
                if let errorText {
                    Text(errorText).foregroundStyle(.red).padding().accessibilityIdentifier("error")
                }
            }
            stats
        }
        .navigationTitle(String(format: "Stage %02d in 3D", stage))
        .navigationBarTitleDisplayMode(.inline)
        .task { await start() }
        .onChange(of: stage) { _ in Task { await load() } }
        .onChange(of: variant) { _ in Task { await load() } }
    }

    private var controls: some View {
        HStack {
            Picker("Stage", selection: $stage) {
                ForEach(stages.isEmpty ? [stage] : stages, id: \.self) { Text(String(format: "%02d", $0)).tag($0) }
            }
            .accessibilityIdentifier("stagePicker")
            Picker("Detail", selection: $variant) {
                Text("Full").tag(0)
                Text("Low (b)").tag(1)
            }
            .pickerStyle(.segmented)
            Button("Reset") { if let scene { orbit = OrbitCamera(scene.defaultCamera) } }
        }
        .padding(.horizontal)
        .disabled(loading)
    }

    @ViewBuilder private var stats: some View {
        VStack(alignment: .leading, spacing: 2) {
            if let scene {
                Text("\(scene.batches.count) batches / \(scene.triangleCount) tris / \(scene.textures.count) textures")
                    .accessibilityIdentifier("stageStats")
                Text(renderer != nil ? "Metal" : "CPU reference renderer (\(rendererError ?? "no Metal"))")
                    .accessibilityIdentifier("rendererKind")
            }
        }
        .font(.footnote.monospacedDigit())
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(8)
    }

    private func start() async {
        let urls = urls
        stages = await Task.detached { StageScene.availableStages(urls: urls) }.value
        do {
            renderer = try StageRenderer()
        } catch {
            rendererError = error.localizedDescription
        }
        await load()
    }

    private func load() async {
        let urls = urls, stage = stage, variant = variant
        loading = true
        errorText = nil
        let result = await Task.detached { Result { try StageScene(urls: urls, stage: stage, variant: variant) } }.value
        loading = false
        switch result {
        case .success(let s):
            renderer?.camera = nil
            renderer?.load(s)
            scene = s
            orbit = OrbitCamera(s.defaultCamera)
            if renderer == nil { await renderCPU() }
        case .failure(let e):
            scene = nil
            errorText = e.localizedDescription
        }
    }

    /// Fallback: one CPU frame per load (the reference renderer is too slow to orbit).
    private func renderCPU() async {
        guard let scene, let orbit else { return }
        let cam = orbit.camera(from: scene.defaultCamera)
        let (w, h) = (480, 320)
        let rgba = await Task.detached { scene.renderReference(camera: cam, width: w, height: h).rgba }.value
        guard let provider = CGDataProvider(data: Data(rgba) as CFData) else { return }
        cpuImage = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                           space: CGColorSpace(name: CGColorSpace.sRGB)!,
                           bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
                           provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }
}
