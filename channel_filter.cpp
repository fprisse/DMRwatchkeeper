#include "channel_filter.h"

#include <cmath>
#include <stdexcept>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ── Constructor ──────────────────────────────────────────────────────────────

ChannelFilter::ChannelFilter(const Config& cfg)
    : cfg_(cfg)
    , delay_(cfg.num_taps, {0.0f, 0.0f})
{
    if (cfg_.num_taps % 2 == 0) {
        throw std::invalid_argument("ChannelFilter: num_taps must be odd");
    }

    design_filter();

    // Offset from SDR centre to this channel (may be negative)
    const float offset_hz = cfg_.channel_freq_hz - cfg_.centre_freq_hz;

    // Per-sample oscillator step: e^(-j·2π·offset/fs)
    // Negative exponent = downconversion (shift channel to DC)
    const float phase_inc = -2.0f * static_cast<float>(M_PI) * offset_hz
                            / cfg_.input_rate;
    osc_      = {1.0f, 0.0f};
    osc_step_ = {std::cos(phase_inc), std::sin(phase_inc)};
}

// ── Filter design: windowed-sinc, Hamming window ──────────────────────────────

void ChannelFilter::design_filter() {
    const int   N  = cfg_.num_taps;
    const float fc = cfg_.cutoff_hz / cfg_.input_rate;  // normalised cutoff

    taps_.resize(N);

    const int half = N / 2;

    for (int i = 0; i < N; ++i) {
        const int n = i - half;

        // Ideal sinc
        float h;
        if (n == 0) {
            h = 2.0f * static_cast<float>(M_PI) * fc;
        } else {
            h = std::sin(2.0f * static_cast<float>(M_PI) * fc * n)
                / (static_cast<float>(M_PI) * n);
        }

        // Hamming window
        const float w = 0.54f - 0.46f * std::cos(
            2.0f * static_cast<float>(M_PI) * i / (N - 1));

        taps_[i] = h * w;
    }

    // Normalise for unity DC gain
    float sum = 0.0f;
    for (float t : taps_) sum += t;
    for (float& t : taps_) t /= sum;
}

// ── process() ────────────────────────────────────────────────────────────────

void ChannelFilter::process(const uint8_t* raw_iq, size_t num_pairs,
                             std::vector<std::complex<float>>& out)
{
    const int N = cfg_.num_taps;
    const int D = cfg_.decimation;

    for (size_t k = 0; k < num_pairs; ++k) {
        // 1. Convert uint8 IQ → complex<float> in ±1.0 range
        //    RTL-SDR delivers I then Q, DC offset at 127.5
        const float i_f = (static_cast<float>(raw_iq[2 * k])     - 127.5f) / 127.5f;
        const float q_f = (static_cast<float>(raw_iq[2 * k + 1]) - 127.5f) / 127.5f;
        std::complex<float> sample{i_f, q_f};

        // 2. Frequency-shift: multiply by downconversion oscillator
        sample *= osc_;

        // Advance oscillator
        osc_ *= osc_step_;

        // Renormalise every 1024 samples to prevent amplitude drift
        if (++osc_norm_counter_ >= 1024) {
            osc_norm_counter_ = 0;
            osc_ /= std::abs(osc_);
        }

        // 3. Push into FIR delay line (newest sample at index 0)
        //    Shift delay line right by one — compiler typically unrolls small shifts
        for (int j = N - 1; j > 0; --j) {
            delay_[j] = delay_[j - 1];
        }
        delay_[0] = sample;

        // 4. Decimate: only compute FIR output every D-th sample
        if (++decim_count_ >= D) {
            decim_count_ = 0;

            // FIR dot product: real taps × complex delay line
            std::complex<float> acc{0.0f, 0.0f};
            for (int j = 0; j < N; ++j) {
                acc += taps_[j] * delay_[j];
            }

            out.push_back(acc);
        }
    }
}
