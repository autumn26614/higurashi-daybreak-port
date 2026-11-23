import AVFoundation

/// A sound decoded by the C API (port/src/audio.h): 16-bit interleaved PCM, with the
/// game's loop region for music (.sfl).
struct DecodedSound {
    let channels: Int
    let rate: Double
    let samples: [Int16]
    let loopStart: Int   // frames; -1: play once
    let loopEnd: Int

    var frames: Int { channels > 0 ? samples.count / channels : 0 }
    var seconds: Double { rate > 0 ? Double(frames) / rate : 0 }

    static func load(_ archives: OpaquePointer, _ name: [UInt8]) throws -> DecodedSound {
        var err = [CChar](repeating: 0, count: 256)
        let s = name.withUnsafeBufferPointer { b in
            b.baseAddress!.withMemoryRebound(to: CChar.self, capacity: b.count) {
                db_sound_load(archives, $0, Int32(b.count), &err, Int32(err.count))
            }
        }
        guard let s else { throw ReportError.open(message(err)) }
        defer { db_sound_free(s) }
        let ch = Int(db_sound_channels(s))
        let n = Int(db_sound_frames(s)) * ch
        return DecodedSound(channels: ch, rate: Double(db_sound_rate(s)),
                            samples: Array(UnsafeBufferPointer(start: db_sound_samples(s), count: n)),
                            loopStart: Int(db_sound_loop_start(s)), loopEnd: Int(db_sound_loop_end(s)))
    }
}

/// Plays one sound at a time: once, or intro then the loop region forever (music).
final class SoundPlayer {
    private let engine = AVAudioEngine()
    private let node = AVAudioPlayerNode()
    private var format: AVAudioFormat?

    init() {
        engine.attach(node)
        try? AVAudioSession.sharedInstance().setCategory(.playback)
    }

    private func buffer(_ s: DecodedSound, from: Int, to: Int, format: AVAudioFormat) -> AVAudioPCMBuffer? {
        let count = max(0, min(to, s.frames) - from)
        guard count > 0, let b = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(count)),
              let out = b.floatChannelData else { return nil }
        b.frameLength = AVAudioFrameCount(count)
        for c in 0..<s.channels {
            let dst = out[c]
            var i = (from * s.channels) + c
            for f in 0..<count {
                dst[f] = Float(s.samples[i]) / 32768
                i += s.channels
            }
        }
        return b
    }

    func play(_ s: DecodedSound) {
        stop()
        guard s.channels > 0, s.rate > 0,
              let fmt = AVAudioFormat(standardFormatWithSampleRate: s.rate, channels: AVAudioChannelCount(s.channels))
        else { return }
        if format != fmt {
            engine.disconnectNodeOutput(node)
            engine.connect(node, to: engine.mainMixerNode, format: fmt)
            format = fmt
        }
        do {
            try AVAudioSession.sharedInstance().setActive(true)
            if !engine.isRunning { try engine.start() }
        } catch {
            return
        }
        if s.loopStart >= 0, s.loopEnd > s.loopStart {
            if let intro = buffer(s, from: 0, to: s.loopEnd, format: fmt) { node.scheduleBuffer(intro) }
            if let loop = buffer(s, from: s.loopStart, to: s.loopEnd, format: fmt) {
                node.scheduleBuffer(loop, at: nil, options: .loops)
            }
        } else if let all = buffer(s, from: 0, to: s.frames, format: fmt) {
            node.scheduleBuffer(all)
        }
        node.play()
    }

    func stop() {
        node.stop()
    }
}
