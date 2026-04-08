#include "demod_chain.h"

DemodChain::DemodChain(const Config& cfg, Layer2Callback cb)
    : cfg_(cfg)
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
                  burst_sync_.push_dibit(dibit);
              })
    , burst_sync_([this](const BurstSync::Burst& b) {
                      slot_mgr_.on_burst(b);
                  }, cfg.channel_idx)
    , slot_mgr_(std::move(cb), cfg.channel_idx)
{}

void DemodChain::process(const uint8_t* raw_iq, size_t num_pairs) {
    iq_buf_.clear();
    disc_buf_.clear();

    filter_.process(raw_iq, num_pairs, iq_buf_);
    if (iq_buf_.empty()) return;

    disc_.process(iq_buf_, disc_buf_);
    timing_.process(disc_buf_);
    // timing_ -> slicer_ -> burst_sync_ -> slot_mgr_ -> Layer2Callback
    // all driven by the lambdas wired in the constructor above
}
