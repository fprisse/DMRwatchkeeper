#include "demod_chain.h"

// ── Constructor ──────────────────────────────────────────────────────────────

DemodChain::DemodChain(const Config& cfg, DibitCallback cb)
    : cfg_(cfg)
    , dibit_cb_(std::move(cb))
    , filter_([&]{
        ChannelFilter::Config fc = cfg.filter;
        fc.channel_freq_hz = cfg.channel_freq_hz;
        fc.centre_freq_hz  = cfg.centre_freq_hz;
        return fc;
      }())
    , timing_(cfg.timing,
              [this](float sym) {
                  slicer_.decide(sym);
              })
    , slicer_(cfg.slicer,
              [this](uint8_t dibit) {
                  if (dibit_cb_) dibit_cb_(dibit, cfg_.channel_idx);
              })
{}

// ── process() ────────────────────────────────────────────────────────────────

void DemodChain::process(const uint8_t* raw_iq, size_t num_pairs) {
    // Clear scratch buffers (keeps allocations alive across calls)
    iq_buf_.clear();
    disc_buf_.clear();

    // Stage 1: frequency shift + LPF + decimate → complex IQ at 25 kSPS
    filter_.process(raw_iq, num_pairs, iq_buf_);

    if (iq_buf_.empty()) return;

    // Stage 2: FM discriminate → float frequency proxy at 25 kSPS
    disc_.process(iq_buf_, disc_buf_);

    // Stage 3+4: timing recovery → slicing → dibit callback
    // (timing_ calls slicer_ internally via lambda)
    timing_.process(disc_buf_);
}
