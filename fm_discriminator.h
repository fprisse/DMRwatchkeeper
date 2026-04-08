#pragma once

#include <complex>
#include <vector>

/*
 * FmDiscriminator
 *
 * Converts a complex baseband IQ stream to instantaneous frequency deviation.
 *
 * Algorithm: atan2-free differential phase detector.
 *
 *   disc(n) = Im( z[n] · conj(z[n-1]) )
 *           = Q[n]·I[n-1] − I[n]·Q[n-1]
 *
 * This computes sin(Δφ) ≈ Δφ for small phase increments.
 * For DMR at 25 kSPS and outer deviation ±1944 Hz:
 *   Δφ_max = 2π·1944/25000 = ±0.489 rad
 *   sin(0.489) = 0.470   vs   0.489   → ~4% error — adequate for slicing.
 *
 * Output is NOT divided by magnitude², so amplitude variations affect the
 * output level. The FskSlicer uses adaptive thresholds to compensate.
 *
 * Input:  complex<float> at 25 kSPS
 * Output: float (instantaneous frequency proxy) at 25 kSPS
 */
class FmDiscriminator {
public:
    FmDiscriminator() = default;

    /*
     * process()
     * Append discriminated samples to 'out'.
     * One output sample per input sample.
     */
    void process(const std::vector<std::complex<float>>& in,
                 std::vector<float>& out);

private:
    std::complex<float> prev_{1.0f, 0.0f};
};
