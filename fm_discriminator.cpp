#include "fm_discriminator.h"

void FmDiscriminator::process(const std::vector<std::complex<float>>& in,
                               std::vector<float>& out)
{
    out.reserve(out.size() + in.size());

    for (const auto& z : in) {
        // Im( z[n] · conj(z[n-1]) ) = Q[n]·I[n-1] − I[n]·Q[n-1]
        const float disc = z.imag() * prev_.real()
                         - z.real() * prev_.imag();
        out.push_back(disc);
        prev_ = z;
    }
}
