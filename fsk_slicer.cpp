#include "fsk_slicer.h"

#include <stdexcept>

// ── Constructor ──────────────────────────────────────────────────────────────

FskSlicer::FskSlicer(const Config& cfg, DibitCallback cb)
    : cfg_(cfg)
    , cb_(std::move(cb))
    , pos_peak_( cfg.init_outer)
    , neg_peak_(-cfg.init_outer)
{
    if (!cb_) {
        throw std::invalid_argument("FskSlicer: callback must not be null");
    }
}

// ── decide() ─────────────────────────────────────────────────────────────────

void FskSlicer::decide(float v) {
    // ── Update outer level estimates ──────────────────────────────────────────

    const float outer_thresh_pos =  cfg_.outer_thresh * pos_peak_;
    const float outer_thresh_neg =  cfg_.outer_thresh * neg_peak_;  // negative

    if (v > outer_thresh_pos) {
        // Positive outer symbol — update pos_peak_ EMA
        pos_peak_ += cfg_.ema_alpha * (v - pos_peak_);
    } else if (v < outer_thresh_neg) {
        // Negative outer symbol — update neg_peak_ EMA
        neg_peak_ += cfg_.ema_alpha * (v - neg_peak_);
    }

    // ── Compute thresholds ────────────────────────────────────────────────────
    const float thr_high = pos_peak_ * 0.5f;   // between +outer and +inner
    const float thr_zero = 0.0f;               // between +inner and -inner
    const float thr_low  = neg_peak_ * 0.5f;   // between -inner and -outer (neg)

    // ── Decision ──────────────────────────────────────────────────────────────
    uint8_t dibit;
    if      (v >= thr_high) dibit = 1;   // +outer  → 01
    else if (v >= thr_zero) dibit = 0;   // +inner  → 00
    else if (v >= thr_low)  dibit = 2;   // -inner  → 10
    else                    dibit = 3;   // -outer  → 11

    cb_(dibit);
}
