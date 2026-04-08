#pragma once

#include "burst_sync.h"
#include "channel_filter.h"
#include "fm_discriminator.h"
#include "fsk_slicer.h"
#include "slot_manager.h"
#include "timing_recovery.h"

#include <cstdint>
#include <functional>
#include <string>

/*
 * DemodChain
 *
 * Complete demodulation + framing pipeline for one DMR channel.
 *
 *   raw IQ bytes (uint8_t, 250 kSPS)
 *       |
 *       v  ChannelFilter
 *   complex<float> at 25 kSPS
 *       |
 *       v  FmDiscriminator
 *   float frequency proxy at 25 kSPS
 *       |
 *       v  TimingRecovery (Gardner TED)
 *   float symbol at 4800 sym/s
 *       |
 *       v  FskSlicer
 *   uint8_t dibit (0-3)
 *       |
 *       v  BurstSync                          <-- Phase 3 addition
 *   BurstSync::Burst (98 payload dibits)
 *       |
 *       v  SlotManager (TS1 / TS2 state)      <-- Phase 3 addition
 *   SlotManager::SlotEvent
 *       |
 *       v  Layer2Callback  <-- Phase 4 will attach BPTC + LC + GPS parser
 */
class DemodChain {
public:
    // Layer 2 callback: invoked for every assembled, slot-assigned burst.
    // Phase 4 replaces the lambda body with BPTC decoder + LC parser.
    using Layer2Callback = SlotManager::Layer2Callback;

    struct Config {
        float       channel_freq_hz;
        float       centre_freq_hz = 446099000.0f;
        int         channel_idx    = 0;
        std::string label;

        ChannelFilter::Config  filter{};
        TimingRecovery::Config timing{};
        FskSlicer::Config      slicer{};
    };

    explicit DemodChain(const Config& cfg, Layer2Callback cb);

    void process(const uint8_t* raw_iq, size_t num_pairs);

    // Diagnostics
    uint64_t symbols_recovered()    const { return timing_.symbol_count(); }
    uint64_t bursts_detected()      const { return burst_sync_.bursts_detected(); }
    float    timing_omega()         const { return timing_.omega(); }
    float    slicer_outer()         const { return slicer_.outer_level(); }
    SlotManager::SlotState slot_state(int ts) const { return slot_mgr_.slot_state(ts); }
    const std::string& label()      const { return cfg_.label; }

private:
    Config           cfg_;

    ChannelFilter    filter_;
    FmDiscriminator  disc_;
    TimingRecovery   timing_;
    FskSlicer        slicer_;
    BurstSync        burst_sync_;
    SlotManager      slot_mgr_;

    std::vector<std::complex<float>> iq_buf_;
    std::vector<float>               disc_buf_;
};
