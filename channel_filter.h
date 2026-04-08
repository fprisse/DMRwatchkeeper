#pragma once

#include <complex>
#include <cstdint>
#include <vector>

/*
 * ChannelFilter
 *
 * Extracts one 12.5 kHz DMR channel from the wideband RTL-SDR IQ stream.
 *
 * Pipeline per input sample:
 *   1. Convert uint8_t I,Q → complex<float>  (range ±1.0)
 *   2. Multiply by e^(-j·2π·offset·n/fs)    → shift channel to baseband
 *   3. FIR low-pass filter                   → reject adjacent channels
 *   4. Decimate by 10                        → 25 kSPS output
 *
 * Filter design: windowed-sinc, Hamming window, 129 taps.
 * Cutoff 5 kHz / 250 kSPS = 0.02 normalised.
 * This gives ~40 dB adjacent-channel rejection at 12.5 kHz spacing.
 *
 * Output sample rate: 250 000 / 10 = 25 000 S/s
 * This provides 5.208 samples per DMR symbol (4800 baud).
 */
class ChannelFilter {
public:
    struct Config {
        float channel_freq_hz;                   // absolute channel centre (Hz)
        float centre_freq_hz  = 446'099'000.0f;  // SDR tuned frequency (Hz)
        float input_rate      = 250'000.0f;      // RTL-SDR sample rate
        float cutoff_hz       =   5'000.0f;      // LPF cutoff
        int   num_taps        = 129;             // FIR length (odd)
        int   decimation      = 10;              // → 25 kSPS output
    };

    explicit ChannelFilter(const Config& cfg);

    /*
     * process()
     * Feed raw IQ bytes from the ring buffer.
     * num_pairs = number of IQ pairs (bytes / 2).
     * Appends decimated complex<float> samples to 'out'.
     */
    void process(const uint8_t* raw_iq, size_t num_pairs,
                 std::vector<std::complex<float>>& out);

    float output_rate() const {
        return cfg_.input_rate / static_cast<float>(cfg_.decimation);
    }

private:
    Config cfg_;
    std::vector<float>               taps_;    // FIR coefficients
    std::vector<std::complex<float>> delay_;   // FIR delay line

    // Rotating phasor downconverter — avoids sin/cos per sample
    std::complex<float> osc_;       // current phasor value
    std::complex<float> osc_step_;  // per-sample rotation

    int decim_count_ = 0;           // decimation counter (0..decimation-1)
    int osc_norm_counter_ = 0;      // renormalisation counter

    void design_filter();
};
