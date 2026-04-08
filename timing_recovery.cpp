#include "timing_recovery.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

// ── Constructor ──────────────────────────────────────────────────────────────

TimingRecovery::TimingRecovery(const Config& cfg, SymbolCallback cb)
    : cfg_(cfg)
    , cb_(std::move(cb))
    , mu_(0.0f)
    , omega_(cfg.nominal_sps)
    , v_p_(0.0f)
    , prev_sym_(0.0f)
{
    if (!cb_) {
        throw std::invalid_argument("TimingRecovery: callback must not be null");
    }
}

// ── Interpolation ─────────────────────────────────────────────────────────────

/*
 * interpolate()
 *
 * Returns the signal value at 'frac' samples before the most recently
 * added sample.  frac = 0 → current sample.  frac = 1 → previous sample.
 *
 * Uses linear interpolation between adjacent history samples.
 */
float TimingRecovery::interpolate(float frac) const {
    // Split into integer and fractional parts
    const int   n0 = static_cast<int>(frac);        // floor
    const float f  = frac - static_cast<float>(n0); // fractional remainder

    // hist_idx_ points to the slot ABOUT to be written — so the most recent
    // sample is at (hist_idx_ - 1 + kHistLen) % kHistLen, one older is at
    // (hist_idx_ - 2 + kHistLen) % kHistLen, etc.
    const auto idx = [this](int offset) -> float {
        return hist_[(hist_idx_ - 1 - offset + kHistLen * 2) % kHistLen];
    };

    const float x0 = idx(n0);
    const float x1 = idx(n0 + 1);

    return x0 + f * (x1 - x0);
}

// ── process() ────────────────────────────────────────────────────────────────

void TimingRecovery::process(const std::vector<float>& samples) {
    for (const float x : samples) {
        // Push new sample into circular history buffer
        hist_[hist_idx_] = x;
        hist_idx_ = (hist_idx_ + 1) % kHistLen;

        mu_ += 1.0f;

        // Symbol strobe: have we accumulated one full symbol period?
        if (mu_ >= omega_) {
            mu_ -= omega_;

            // ── Interpolate symbol and mid-sample ────────────────────────────

            // Current symbol: mu_ samples before the most recent input sample
            const float curr_sym = interpolate(mu_);

            // Mid-sample: half a symbol period before the current symbol
            const float mid_frac  = mu_ + omega_ * 0.5f;
            const float mid_samp  = interpolate(mid_frac);

            // ── Gardner timing error ──────────────────────────────────────────
            // e = x_mid · (x_curr − x_prev)
            const float e = mid_samp * (curr_sym - prev_sym_);

            // ── PI loop filter ────────────────────────────────────────────────
            v_p_    += cfg_.K2 * e;
            omega_  += cfg_.K1 * e + v_p_;
            omega_   = std::clamp(omega_, cfg_.omega_min, cfg_.omega_max);

            // ── Emit symbol ───────────────────────────────────────────────────
            cb_(curr_sym);
            ++symbol_count_;

            prev_sym_ = curr_sym;
        }
    }
}
