#include "tuner.h"

namespace tuner {

namespace {
// 4th-order Butterworth low-pass at 5 kHz for 48 kHz (two RBJ sections, Q 0.5412 and 1.3066),
// worked out offline: the Seed's code links no libm.
constexpr float kLp[2][5] = {
    {6.613022282e-02f, 1.322604456e-01f, 6.613022282e-02f, -1.015542825e+00f, 2.800637168e-01f},
    {8.380084160e-02f, 1.676016832e-01f, 8.380084160e-02f, -1.286905439e+00f, 6.221088058e-01f},
};
// 1st-order high-pass at 30 Hz: 1 / (1 + 2 pi 30 / 48000).
constexpr float kHpA = 0.996088370f;

// 10 * log10(x) for x > 0, without libm: x = m * 2^e with m in [1, 2), and ln(m) from the atanh
// series (t <= 1/3, so four terms are within 0.0001 dB).
float PowerToDb(float x) {
    if (!(x > 1e-12f))
        return -120.0f;
    uint32_t bits;
    __builtin_memcpy(&bits, &x, sizeof bits);
    const int32_t e = static_cast<int32_t>((bits >> 23) & 0xffu) - 127;
    bits = (bits & 0x007fffffu) | 0x3f800000u;
    float m;
    __builtin_memcpy(&m, &bits, sizeof m);
    const float t = (m - 1.0f) / (m + 1.0f);
    const float t2 = t * t;
    const float ln_m = 2.0f * t * (1.0f + t2 * (1.0f / 3.0f + t2 * (0.2f + t2 * (1.0f / 7.0f))));
    return 3.0103000f * static_cast<float>(e) + 4.3429448f * ln_m;
}

// The vertex of the parabola through (-1, a), (0, b), (1, c), if b is a minimum; else 0.
float ParabolaShift(float a, float b, float c) {
    const float denom = a - 2.0f * b + c;
    if (!(denom > 0))
        return 0.0f;
    const float s = 0.5f * (a - c) / denom;
    return s > -1.0f && s < 1.0f ? s : 0.0f;
}
} // namespace

void Detector::Reset() {
    for (int s = 0; s < 2; ++s)
        lowpass_[s] = Biquad{kLp[s][0], kLp[s][1], kLp[s][2], kLp[s][3], kLp[s][4]};
    hp_x_ = hp_y_ = 0;
    for (float &v : ring_)
        v = 0;
    written_.store(0, std::memory_order_relaxed);
}

void Detector::Push(const float *x, std::size_t n) {
    uint32_t w = written_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < n; ++i) {
        // DC and rumble out, then band-limit for pass 1's 24 kHz.
        hp_y_ = kHpA * (hp_y_ + x[i] - hp_x_);
        hp_x_ = x[i];
        ring_[w & (kRing - 1)] = lowpass_[1].Process(lowpass_[0].Process(hp_y_));
        ++w;
    }
    written_.store(w, std::memory_order_release);
}

float Detector::Difference(const float *x, int window, int lag) const {
    float d = 0;
    const float *y = x + lag;
    for (int j = 0; j < window; ++j) {
        const float e = x[j] - y[j];
        d += e * e;
    }
    return d;
}

Detector::Result Detector::Estimate() {
    Result r{0.0f, -120.0f, 0.0f};
    const uint32_t end = written_.load(std::memory_order_acquire);
    if (end < static_cast<uint32_t>(kNeeded))
        return r;

    // The newest samples. Push() writes ahead of `end` meanwhile, 45 ms away from these.
    const uint32_t coarse_start = end - 2 * kCoarse;
    for (int i = 0; i < kCoarse; ++i)
        coarse_[i] = ring_[(coarse_start + 2 * static_cast<uint32_t>(i)) & (kRing - 1)];
    const uint32_t fine_start = end - kFine;
    for (int i = 0; i < kFine; ++i)
        frame_[i] = ring_[(fine_start + static_cast<uint32_t>(i)) & (kRing - 1)];

    float power = 0;
    for (int j = 0; j < kWindow; ++j)
        power += coarse_[j] * coarse_[j];
    r.level_db = PowerToDb(power / kWindow);
    if (r.level_db < kMinLevelDb)
        return r;

    // Difference function and its cumulative mean normalisation (YIN steps 2 and 3).
    diff_[0] = 0;
    cmnd_[0] = 1;
    float running = 0;
    for (int tau = 1; tau <= kMaxLag + 1; ++tau) {
        const float d = Difference(coarse_, kWindow, tau);
        diff_[tau] = d;
        running += d;
        cmnd_[tau] = running > 0 ? d * static_cast<float>(tau) / running : 1.0f;
    }

    // The first dip under the threshold, followed down to its bottom (YIN step 4).
    int best = -1;
    for (int tau = kMinLag; tau <= kMaxLag; ++tau) {
        if (cmnd_[tau] < kThreshold) {
            while (tau + 1 <= kMaxLag && cmnd_[tau + 1] < cmnd_[tau])
                ++tau;
            best = tau;
            break;
        }
    }
    if (best < 0)
        return r;
    r.clarity = 1.0f - cmnd_[best];
    const float coarse_lag = static_cast<float>(best) + ParabolaShift(diff_[best - 1], diff_[best], diff_[best + 1]);
    float lag = 2.0f * coarse_lag; // in 48 kHz samples; pass 2 refines it, or keeps it if it can't

    // Pass 2 at 48 kHz: the lags around pass 1's.
    constexpr int kLags = 2 * kFineReach + 1;
    const int first = static_cast<int>(lag + 0.5f) - kFineReach;
    float d[kLags];
    int lowest = 0;
    for (int k = 0; k < kLags; ++k) {
        d[k] = Difference(frame_, kFineWindow, first + k);
        if (d[k] < d[lowest])
            lowest = k;
    }
    // A minimum at the edge means pass 2 found nothing near pass 1: keep pass 1's reading.
    if (lowest > 0 && lowest < kLags - 1)
        lag = static_cast<float>(first + lowest) + ParabolaShift(d[lowest - 1], d[lowest], d[lowest + 1]);
    r.hz = rate_scale_ * kRate / lag;
    return r;
}

} // namespace tuner
