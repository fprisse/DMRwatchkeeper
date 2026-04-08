#pragma once

#include <functional>
#include <vector>

/*
 * TimingRecovery
 *
 * Symbol timing synchronisation using the Gardner Timing Error Detector (TED).
 *
 * ── Background ──────────────────────────────────────────────────────────────
 *
 * The discriminator output is a continuous float stream at 25 kSPS.
 * DMR symbol rate is 4800 baud.
 * Nominal samples per symbol: 25000 / 4800 = 5.2083̄
 *
 * We need to sample the discriminator output at exactly the right moment
 * to read each symbol reliably — the "optimal sampling instant".
 *
 * The Gardner TED estimates timing error from the signal itself:
 *
 *   e(k) = x(k − T/2) · [ x(k) − x(k − T) ]
 *
 * where:
 *   x(k)       = discriminator sample at current symbol strobe
 *   x(k − T)   = sample at previous symbol strobe
 *   x(k − T/2) = sample at midpoint between strobes (interpolated)
 *
 * The error e(k) is fed through a PI loop filter that adjusts omega
 * (samples-per-symbol estimate) to drive e(k) → 0.
 *
 * ── Implementation ──────────────────────────────────────────────────────────
 *
 * A circular history buffer (8 samples) holds recent discriminator values.
 * Linear interpolation resolves sub-sample timing.
 *
 * Loop filter:
 *   omega(k+1) = omega(k) + K1·e(k) + v_p(k)
 *   v_p(k+1)   = v_p(k)   + K2·e(k)
 *
 * Omega is clamped to [4.5, 6.0] samples/symbol to prevent runaway.
 *
 * Output: one float per symbol (the interpolated symbol value),
 *         delivered via callback.
 */
class TimingRecovery {
public:
    struct Config {
        float nominal_sps = 5.2083f;  // nominal samples per symbol (25000/4800)
        float K1          = 0.02f;    // proportional gain
        float K2          = 0.002f;   // integral gain
        float omega_min   = 4.5f;
        float omega_max   = 6.0f;
    };

    using SymbolCallback = std::function<void(float symbol_value)>;

    explicit TimingRecovery(const Config& cfg, SymbolCallback cb);

    /*
     * process()
     * Feed discriminator samples. Callback is invoked for each recovered symbol.
     */
    void process(const std::vector<float>& samples);

    // Diagnostic: symbols recovered since construction
    uint64_t symbol_count() const { return symbol_count_; }

    // Current omega estimate (samples per symbol)
    float omega() const { return omega_; }

private:
    // Linear interpolation in the history buffer at fractional position frac
    // (0 = most recent sample, 1 = one sample earlier)
    float interpolate(float frac) const;

    Config         cfg_;
    SymbolCallback cb_;

    float    mu_       ;  // fractional timing phase (0..omega_)
    float    omega_    ;  // samples per symbol estimate
    float    v_p_      ;  // loop filter integrator
    float    prev_sym_ ;  // previous symbol value (for Gardner error)

    // Circular sample history — 8 samples is enough for interpolation at
    // omega ≤ 6 samples/symbol
    static constexpr int kHistLen = 8;
    float    hist_[kHistLen]{};
    int      hist_idx_ = 0;

    uint64_t symbol_count_ = 0;
};
