#pragma once
// Pitch detection for the touchscreen's tuner: the Seed hears the guitar, the touchscreen only
// shows the result (docs/TUNER.md).
//
// Two passes over the newest 40 ms of input:
// 1. YIN (de Cheveigne & Kawahara 2002) at 24 kHz finds the period: the cumulative mean normalised
//    difference function's first dip under a threshold. Starting from the shortest lag picks the
//    period rather than a multiple of it, even when the fundamental is weak.
// 2. At the full 48 kHz, the raw difference at the lags around pass 1's, refined by a parabola:
//    the finer lags make high notes exact.
// Both read the whole band-limited waveform, as clip-on, pedal and phone tuners do. On a real
// string the upper partials run a little sharp (inharmonicity), so this reads a few cents above
// the fundamental alone on the low E and G strings; it is what players tune to. (An earlier
// version low-passed pass 2 down to the fundamental: tested against a reference tuner on
// 2026-11-17 it read those strings 7 to 8 cents flat.)
// 55 Hz (drop A; a 7-string's low B is 61.7 Hz) to 1400 Hz (the high E string's 24th fret is
// 1319 Hz). Accuracy and cost: test_tuner.cpp.
//
// Threading: Push() from the audio callback (two biquads, a one-pole and a ring write per sample);
// Estimate() from the main loop (about 230k multiply-adds; call it some 20 times a second while the
// tuner is open). Push() never waits for Estimate(): the ring holds well over twice what
// Estimate() reads, so the samples it copies are never being overwritten. No allocation, no libm.
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace tuner {

class Detector {
  public:
    // The filters and lags are designed for this rate; SetSampleRate() corrects the readings for a
    // clock that runs a little off it (the Seed's runs at 48014.3 Hz).
    static constexpr float kRate = 48000.0f;
    static constexpr float kMinHz = 55.0f;
    static constexpr float kMaxHz = 1400.0f;

    // Pass 1, at 24 kHz.
    static constexpr float kCoarseRate = kRate / 2;
    static constexpr int kMinLag = static_cast<int>(kCoarseRate / kMaxHz);     // 17
    static constexpr int kMaxLag = static_cast<int>(kCoarseRate / kMinHz) + 2; // 438
    static constexpr int kWindow = 512;                                       // samples summed per lag
    // YIN's threshold on the normalised difference: a dip under it is a period. Lower rejects more
    // noisy notes; higher risks an octave error on a weak fundamental.
    static constexpr float kThreshold = 0.15f;
    static constexpr int kCoarse = kWindow + kMaxLag + 2; // 952 samples at 24 kHz, about 40 ms

    // Pass 2, at 48 kHz: the window, and the lags searched either side of pass 1's.
    static constexpr int kFineWindow = 2 * kWindow;
    static constexpr int kFineReach = 3;
    static constexpr int kFine = kFineWindow + 2 * kMaxLag + 2 * kFineReach + 2; // 1908, about 40 ms
    static constexpr int kNeeded = 2 * kCoarse > kFine ? 2 * kCoarse : kFine;
    // A power of two. Estimate() copies the newest kNeeded samples while Push() goes on writing:
    // the rest (about 2190 samples, 45 ms) is the margin before Push() could reach them.
    static constexpr int kRing = 4096;

    // Quieter than this (RMS of the analysed window) counts as no note. A guitar arrived at -50 to
    // -58 dBFS on the amp board, silence at -102: -75 keeps a ringing string well into its decay.
    static constexpr float kMinLevelDb = -75.0f;

    struct Result {
        float hz;       // 0 when there is no clear pitch
        float level_db; // RMS of the analysed window, dBFS (-120 for silence)
        float clarity;  // 1 - the dip's depth: near 1 for a clean note, 0 when there is none
    };

    Detector() { Reset(); }

    // The input's true sample rate (main loop, before or between Estimate() calls).
    void SetSampleRate(float hz) { rate_scale_ = hz / kRate; }
    // Audio thread: remove DC, band-limit and store the input.
    void Push(const float *x, std::size_t n);
    // Main loop: the pitch of the newest kNeeded samples. Before that many have arrived since
    // Reset(), returns no pitch.
    Result Estimate();
    // With Push() not running (e.g. before the tuner is switched on).
    void Reset();

  private:
    struct Biquad {
        float b0, b1, b2, a1, a2;
        float z1 = 0, z2 = 0;
        float Process(float x) {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    float Difference(const float *x, int window, int lag) const;

    // Audio thread state.
    Biquad lowpass_[2];         // 4th-order Butterworth at 5 kHz, against aliasing at 24 kHz
    float hp_x_ = 0, hp_y_ = 0; // 1st-order high-pass at 30 Hz: DC and rumble
    float ring_[kRing];         // the band-limited input
    std::atomic<uint32_t> written_{0}; // samples written since Reset

    // Main-loop state and scratch.
    float rate_scale_ = 1.0f;
    float frame_[kFine];    // 48 kHz
    float coarse_[kCoarse]; // 24 kHz
    float diff_[kMaxLag + 2];
    float cmnd_[kMaxLag + 2];
};

} // namespace tuner
