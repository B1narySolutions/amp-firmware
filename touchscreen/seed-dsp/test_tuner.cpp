// Host tests for the tuner's pitch detector: every guitar string and fret range within 1 cent,
// detuned notes read as detuned, a weak fundamental without an octave error, inharmonic strings
// read steadily and a little sharp as other tuners read them, a decaying pluck with noise, a change
// of string, the Seed's 48014.3 Hz clock, silence and noise as no note, the level, and the cost.
// Build and run: bash seed-dsp/test.sh
#include "tuner.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...)                                                                                                         \
    do {                                                                                                                         \
        ++g_checks;                                                                                                              \
        if (!(cond)) {                                                                                                           \
            ++g_fail;                                                                                                            \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                        \
            printf(__VA_ARGS__);                                                                                                 \
            printf("\n");                                                                                                        \
        }                                                                                                                        \
    } while (0)

constexpr int kBlock = 48;
constexpr float kFs = 48000.0f;

static double Cents(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }

// A plucked string: partials n = 1..count at n * f * sqrt(1 + B n^2) (B: inharmonicity), with
// amplitudes amp[n-1] (1/n if empty), higher partials decaying faster, plus white noise.
struct Pluck {
    double f = 110, inharm = 0, decay_s = 1e9, noise_db = -200, level = 0.3;
    int partials = 12;
    float fundamental = 1.0f; // relative level of the first partial
};

static std::vector<float> Synth(const Pluck &p, double seconds, unsigned seed = 1, double fs = kFs) {
    std::vector<float> v(static_cast<size_t>(seconds * fs));
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    const double noise_amp = std::pow(10.0, p.noise_db / 20.0);
    for (size_t i = 0; i < v.size(); ++i) {
        const double t = i / fs;
        double s = 0;
        for (int n = 1; n <= p.partials; ++n) {
            const double fn = n * p.f * std::sqrt(1 + p.inharm * n * n);
            if (fn > 20000)
                break;
            const double a = (n == 1 ? p.fundamental : 1.0) / n;
            s += a * std::exp(-t * n / p.decay_s) * std::sin(2 * M_PI * fn * t + 0.7 * n);
        }
        v[i] = float(p.level * s / 2.0 + noise_amp * noise(rng));
    }
    return v;
}

// Feeds `sig` in 48-sample blocks and estimates every `every` blocks after `warmup` seconds.
static std::vector<tuner::Detector::Result> Run(tuner::Detector &d, const std::vector<float> &sig, double warmup = 0.05,
                                                 int every = 50) {
    std::vector<tuner::Detector::Result> out;
    d.Reset();
    int blocks = 0;
    for (size_t s = 0; s + kBlock <= sig.size(); s += kBlock) {
        d.Push(sig.data() + s, kBlock);
        if (s / kFs >= warmup && ++blocks % every == 0)
            out.push_back(d.Estimate());
    }
    return out;
}

static tuner::Detector g_d; // about 13 KB: static, as on the Seed

// Worst error in cents over the settled estimates (from `skip` on: Run() estimates every 50 ms
// from 100 ms after the start, so 2 skips up to 200 ms); counts estimates without a pitch.
static double WorstCents(const std::vector<tuner::Detector::Result> &rs, double ref, int &missing, size_t skip = 2) {
    double worst = 0;
    missing = 0;
    for (size_t i = skip; i < rs.size(); ++i) {
        const auto &r = rs[i];
        if (r.hz <= 0) {
            ++missing;
            continue;
        }
        worst = std::fmax(worst, std::fabs(Cents(r.hz, ref)));
    }
    return worst;
}

static void TestStrings() {
    // Standard tuning, drop D, a 7-string's low B, drop A, and up the neck to the 24th fret.
    const struct {
        const char *name;
        double hz;
    } notes[] = {
        {"A1 (drop A)", 55.000},  {"B1 (7-string)", 61.735}, {"D2 (drop D)", 73.416}, {"E2", 82.407},
        {"A2", 110.000},          {"D3", 146.832},           {"G3", 195.998},         {"B3", 246.942},
        {"E4", 329.628},          {"A4", 440.000},           {"E5 (12th fret)", 659.255},
        {"B5", 987.767},          {"E6 (24th fret)", 1318.510},
    };
    double worst_all = 0;
    for (const auto &n : notes) {
        Pluck p;
        p.f = n.hz;
        int missing;
        const double worst = WorstCents(Run(g_d, Synth(p, 0.6)), n.hz, missing);
        CHECK(missing == 0, "%s: %d estimates without a pitch", n.name, missing);
        CHECK(worst < 1.0, "%s: off by %.2f cents", n.name, worst);
        worst_all = std::fmax(worst_all, worst);
    }
    printf("  strings A1..E6: worst %.3f cents\n", worst_all);
}

static void TestDetuned() {
    // The reading follows the string, not the nearest note.
    for (double cents : {-45.0, -20.0, -5.0, -1.0, 1.0, 5.0, 20.0, 45.0}) {
        for (double base : {82.407, 246.942}) {
            Pluck p;
            p.f = base * std::pow(2.0, cents / 1200.0);
            int missing;
            const auto rs = Run(g_d, Synth(p, 0.4));
            const double worst = WorstCents(rs, p.f, missing);
            CHECK(missing == 0 && worst < 1.0, "%.0f cents from %.1f Hz: off by %.2f, %d missing", cents, base, worst, missing);
        }
    }
}

static void TestWeakFundamental() {
    // A bridge pickup's low E: the fundamental well under the second harmonic. No octave error.
    for (float fund : {0.5f, 0.25f, 0.1f}) {
        Pluck p;
        p.f = 82.407;
        p.fundamental = fund;
        int missing;
        const auto rs = Run(g_d, Synth(p, 0.6));
        const double worst = WorstCents(rs, p.f, missing);
        CHECK(missing == 0 && worst < 1.0, "low E with the fundamental at %.2f: off by %.1f cents (an octave is 1200), %d missing", fund,
              worst, missing);
    }
}

static void TestInharmonic() {
    // Real strings: the partials run sharp (wound low strings and the plain G, B about 1e-4). Like
    // other tuners, the detector reads the whole waveform, so a little above the fundamental alone,
    // by no more than an inharmonic string's partials pull it. Its reading wanders a little: with
    // its partials out of step the waveform keeps changing shape, as other tuners see too; the
    // touchscreen's median and needle easing smooth it, well inside the +-3 cent zone.
    for (double b : {5e-5, 1e-4, 3e-4}) {
        Pluck p;
        p.f = 82.407;
        p.inharm = b;
        const auto rs = Run(g_d, Synth(p, 0.6));
        double lo = 1e9, hi = -1e9;
        int missing = 0;
        for (size_t i = 1; i < rs.size(); ++i) {
            if (rs[i].hz <= 0) {
                ++missing;
                continue;
            }
            const double c = Cents(rs[i].hz, p.f);
            lo = std::fmin(lo, c);
            hi = std::fmax(hi, c);
        }
        CHECK(missing == 0 && lo > 0 && hi < 20.0 && hi - lo < 2.5, "low E, inharmonicity %g: %.2f to %.2f cents, %d missing", b, lo, hi,
              missing);
        printf("  low E, inharmonicity %g: %+.2f cents above the fundamental (wandering by %.2f)\n", b, hi, hi - lo);
    }
}

static void TestClock() {
    // The Seed's audio clock: 16 MHz / 6 * 295 / 16 / 1024 = 48014.3 Hz, not 48000. Told so, the
    // detector reads true; not told, 0.52 cents flat.
    const double seed_fs = 16e6 / 6 * 295 / 16 / 1024;
    Pluck p;
    p.f = 110.0;
    const auto sig = Synth(p, 0.5, 1, seed_fs);
    int missing;
    g_d.SetSampleRate(float(seed_fs));
    const double corrected = WorstCents(Run(g_d, sig), p.f, missing);
    CHECK(missing == 0 && corrected < 0.1, "at 48014.3 Hz, corrected: off by %.3f cents", corrected);
    g_d.SetSampleRate(48000.0f);
    const double raw = WorstCents(Run(g_d, sig), p.f, missing);
    CHECK(std::fabs(raw - 0.516) < 0.1, "at 48014.3 Hz, uncorrected: %.3f cents (expected 0.52)", raw);
    printf("  Seed clock 48014.3 Hz: %.2f cents flat uncorrected, %.3f corrected\n", raw, corrected);
}

static void TestPluck() {
    // A plucked high E decaying over 2 s with noise 50 dB down: a steady reading while it rings.
    Pluck p;
    p.f = 329.628;
    p.decay_s = 1.2;
    p.noise_db = -70;
    p.level = 0.5;
    const auto rs = Run(g_d, Synth(p, 2.0, 3), 0.05, 25);
    int read = 0, wrong = 0;
    for (size_t i = 0; i < rs.size(); ++i) {
        if (rs[i].hz <= 0)
            continue;
        ++read;
        if (std::fabs(Cents(rs[i].hz, p.f)) > 1.0)
            ++wrong;
    }
    CHECK(read >= int(rs.size() * 9 / 10), "decaying pluck: %d of %zu estimates had a pitch", read, rs.size());
    CHECK(wrong == 0, "decaying pluck: %d estimates more than 1 cent off", wrong);
}

static void TestNoteChange() {
    // E2 for 0.5 s, then straight to A2 (as when moving to the next string): every reading is within
    // a semitone of the note sounding, and within 1 cent from 200 ms after the change.
    Pluck e, a;
    e.f = 82.407;
    a.f = 110.0;
    auto sig = Synth(e, 0.5);
    const auto second = Synth(a, 0.8);
    sig.insert(sig.end(), second.begin(), second.end());
    g_d.Reset();
    int worst_early = 0, late_bad = 0, late = 0;
    double worst_late = 0;
    for (size_t s = 0; s + kBlock <= sig.size(); s += kBlock) {
        g_d.Push(sig.data() + s, kBlock);
        const double t = (s + kBlock) / double(kFs);
        if (t < 0.1 || (s / kBlock) % 10 != 0)
            continue; // every 10 ms
        const auto r = g_d.Estimate();
        const double change = 0.5, since = t - change;
        // The frame is 48 ms long: while it spans the change, either note may be read.
        if (since > 0 && since < 0.05)
            continue;
        const double ref = since > 0 ? a.f : e.f;
        const double c = r.hz > 0 ? std::fabs(Cents(r.hz, ref)) : 0;
        if (since >= 0.2) {
            ++late;
            late_bad += r.hz <= 0 || c > 1.0;
            worst_late = std::fmax(worst_late, c);
        } else {
            worst_early = std::max(worst_early, int(c));
        }
    }
    CHECK(worst_early < 100, "early readings up to %d cents off (a semitone is 100)", worst_early);
    CHECK(late > 50 && late_bad == 0, "after the change: %d of %d settled readings missing or over 1 cent (worst %.2f)", late_bad, late,
          worst_late);
    printf("  note change E2 -> A2: early readings within %d cents, settled within %.2f\n", worst_early + 1, worst_late);
}

static void TestNoNote() {
    // Silence and plain noise are no note, whatever their level.
    std::vector<float> silence(48000, 0.0f);
    for (const auto &r : Run(g_d, silence))
        CHECK(r.hz == 0 && r.level_db <= -119, "silence: %.1f Hz at %.1f dB", r.hz, r.level_db);
    std::mt19937 rng(5);
    std::normal_distribution<float> n(0.0f, 0.1f);
    std::vector<float> noise(48000);
    for (auto &x : noise)
        x = n(rng);
    int pitched = 0;
    for (const auto &r : Run(g_d, noise))
        pitched += r.hz > 0;
    CHECK(pitched == 0, "white noise: %d estimates reported a pitch", pitched);
    // Under the level gate (-75 dBFS).
    Pluck quiet;
    quiet.f = 110;
    quiet.level = 0.00005;
    for (const auto &r : Run(g_d, Synth(quiet, 0.4)))
        CHECK(r.hz == 0 && r.level_db < -75, "a note at %.1f dBFS: reported %.1f Hz", r.level_db, r.hz);
    // Just over it, as a guitar's tail on the amp board: still read.
    Pluck tail;
    tail.f = 110;
    tail.level = 0.0008;
    int read = 0, total = 0;
    for (const auto &r : Run(g_d, Synth(tail, 0.4))) {
        ++total;
        read += r.hz > 0 && std::fabs(Cents(r.hz, 110.0)) < 1.0;
    }
    CHECK(read == total, "a quiet note just over the gate: %d of %d readings right", read, total);
}

static void TestLevel() {
    // A sine at amplitude 0.1 is -23.0 dBFS RMS; the filters pass 440 Hz unchanged.
    std::vector<float> v(24000);
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = 0.1f * std::sin(2 * float(M_PI) * 440.0f * i / kFs);
    const auto rs = Run(g_d, v);
    CHECK(!rs.empty() && std::fabs(rs.back().level_db + 23.01f) < 0.3f, "level of a -23 dBFS sine: %.2f dB", rs.back().level_db);
    CHECK(rs.back().clarity > 0.95f, "clarity of a sine: %.3f", rs.back().clarity);
}

static void TestStartup() {
    // No reading until 48 ms of input have arrived; then one at once.
    g_d.Reset();
    std::vector<float> v(48 * 60);
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = 0.2f * std::sin(2 * float(M_PI) * 196.0f * i / kFs);
    for (size_t s = 0; s < 48 * 20; s += kBlock)
        g_d.Push(v.data() + s, kBlock);
    CHECK(g_d.Estimate().hz == 0, "a reading after 20 ms");
    for (size_t s = 48 * 20; s < v.size(); s += kBlock)
        g_d.Push(v.data() + s, kBlock);
    const auto r = g_d.Estimate();
    CHECK(std::fabs(Cents(r.hz, 196.0)) < 1.0, "after 60 ms of G3: %.2f Hz", r.hz);
}

static void Timing() {
    Pluck p;
    p.f = 82.407;
    const auto sig = Synth(p, 0.5);
    g_d.Reset();
    for (size_t s = 0; s + kBlock <= sig.size(); s += kBlock)
        g_d.Push(sig.data() + s, kBlock);
    constexpr int kRuns = 200;
    auto t0 = std::chrono::steady_clock::now();
    float sink = 0;
    for (int i = 0; i < kRuns; ++i)
        sink += g_d.Estimate().hz;
    auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < kRuns; ++i)
        for (size_t s = 0; s + kBlock <= 48000; s += kBlock)
            g_d.Push(sig.data() + (s % (sig.size() - kBlock)), kBlock);
    auto t2 = std::chrono::steady_clock::now();
    printf("  host cost: Estimate %.1f us, Push %.3f us per 48-sample block (%.0f)\n",
           std::chrono::duration<double, std::micro>(t1 - t0).count() / kRuns,
           std::chrono::duration<double, std::micro>(t2 - t1).count() / (kRuns * 1000.0), double(sink) * 0);
}

int main() {
    TestStrings();
    TestDetuned();
    TestWeakFundamental();
    TestInharmonic();
    TestClock();
    TestPluck();
    TestNoteChange();
    TestNoNote();
    TestLevel();
    TestStartup();
    Timing();
    printf("tuner: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
