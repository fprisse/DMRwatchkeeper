#pragma once

#include "channel_filter.h"
#include "fm_discriminator.h"
#include "fsk_slicer.h"
#include "timing_recovery.h"

#include <cstdint>
#include <functional>
#include <string>

/*
 * DemodChain
 *
 * Complete demodulation pipeline for one DMR channel.
 *
 *   raw IQ bytes (uint8_t, 250 kSPS)
 *       │
 *       ▼  ChannelFilter
 *   complex<float> at 25 kSPS  (frequency-shifted, LPF, decimated)
 *       │
 *       ▼  FmDiscriminator
 *   float (instantaneous frequency proxy) at 25 kSPS
 *       │
 *       ▼  TimingRecovery  (Gardner TED + PI loop filter)
 *   float symbol value at 4800 symbols/sec
 *       │
 *       ▼  FskSlicer  (adaptive 4-level decision)
 *   uint8_t dibit  (0=00, 1=01, 2=10, 3=11)
 *       │
 *       ▼  DibitCallback  ← Phase 3 burst sync will attach here
 *
 * All processing is synchronous — call process() from the channelizer thread.
 * No internal threads.
 */
class DemodChain {
public:
    // Called for every recovered dibit.
    // Phase 3 will replace this with the burst sync detector.
    using DibitCallback = std::function<void(uint8_t dibit, int channel_idx)>;

    struct Config {
        float   channel_freq_hz;             // absolute channel frequency (Hz)
        float   centre_freq_hz = 446'099'000.0f;
        int     channel_idx    = 0;          // identifier passed through to callback
        std::string label;                   // e.g. "446.006"

        ChannelFilter::Config  filter{};
        TimingRecovery::Config timing{};
        FskSlicer::Config      slicer{};
    };

    explicit DemodChain(const Config& cfg, DibitCallback cb);

    /*
     * process()
     * Feed a block of raw IQ bytes. Drives the complete pipeline.
     * num_pairs = bytes / 2.
     */
    void process(const uint8_t* raw_iq, size_t num_pairs);

    // Diagnostics
    uint64_t symbols_recovered() const { return timing_.symbol_count(); }
    float    timing_omega()      const { return timing_.omega(); }
    float    slicer_outer()      const { return slicer_.outer_level(); }
    const std::string& label()   const { return cfg_.label; }

private:
    Config          cfg_;
    DibitCallback   dibit_cb_;

    ChannelFilter   filter_;
    FmDiscriminator disc_;
    TimingRecovery  timing_;
    FskSlicer       slicer_;

    // Scratch buffers — reused across process() calls to avoid allocations
    std::vector<std::complex<float>> iq_buf_;
    std::vector<float>               disc_buf_;
};
