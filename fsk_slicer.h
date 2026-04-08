#pragma once

#include <cstdint>
#include <functional>

/*
 * FskSlicer
 *
 * Maps a float symbol value to a 2-bit dibit (0–3).
 *
 * ── DMR 4FSK symbol mapping ──────────────────────────────────────────────────
 *
 * DMR uses four deviation levels (ETSI TS 102 361-1 §7.1):
 *
 *   Deviation  Dibit
 *   +1944 Hz    01     (positive outer)
 *    +648 Hz    00     (positive inner)
 *    -648 Hz    10     (negative inner)
 *   -1944 Hz    11     (negative outer)
 *
 * After FM discrimination the signal is proportional to frequency deviation.
 * The exact amplitude depends on signal strength, antenna, and gain setting,
 * so fixed thresholds don't work reliably.
 *
 * ── Adaptive threshold estimation ────────────────────────────────────────────
 *
 * The slicer tracks a running exponential moving average of:
 *   pos_peak_  — mean of the positive outer symbol level
 *   neg_peak_  — mean of the negative outer symbol level
 *
 * Thresholds (three, between the four levels):
 *   thr_high_ =  pos_peak_ / 2   (between +outer and +inner)
 *   thr_zero_ =  0               (between +inner and -inner)
 *   thr_low_  =  neg_peak_ / 2   (between -inner and -outer)
 *
 * The outer level estimates are updated only when the current symbol is
 * clearly in the outer region (|value| > 0.6 × current outer estimate).
 *
 * ── Decision ─────────────────────────────────────────────────────────────────
 *
 *   value > thr_high_  → dibit 01 (= 1)
 *   value > thr_zero_  → dibit 00 (= 0)
 *   value > thr_low_   → dibit 10 (= 2)
 *   else               → dibit 11 (= 3)
 *
 * Output dibits use this integer encoding: 0=00, 1=01, 2=10, 3=11.
 */
class FskSlicer {
public:
    using DibitCallback = std::function<void(uint8_t dibit)>;

    struct Config {
        float ema_alpha     = 0.01f;  // EMA smoothing for peak tracking
        float init_outer    = 0.02f;  // initial outer level estimate (±)
        float outer_thresh  = 0.6f;   // fraction of outer estimate to count as outer
    };

    explicit FskSlicer(const Config& cfg, DibitCallback cb);

    /*
     * decide()
     * Map one symbol value to a dibit and invoke the callback.
     */
    void decide(float symbol_value);

    // Current outer level estimate (diagnostic)
    float outer_level() const { return pos_peak_; }

private:
    Config        cfg_;
    DibitCallback cb_;

    float pos_peak_;  // running estimate of positive outer level
    float neg_peak_;  // running estimate of negative outer level (negative value)
};
