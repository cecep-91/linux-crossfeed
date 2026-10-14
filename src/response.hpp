#pragma once

#include "dsp.hpp"
#include <complex>

namespace crossfeed {

// Steady-state response of a prepared DSP snapshot. For display/analysis on
// control threads only; includes the actual linear fractional-delay response.
struct StereoResponse {
    std::complex<double> direct;
    std::complex<double> cross;
    double mono_db() const { return db(direct + cross); }
    double side_db() const { return db(direct - cross); }
    static double db(std::complex<double> value) {
        return 20.0 * std::log10(std::max(1e-6, std::abs(value)));
    }
};

inline StereoResponse response_at(const FilterParams& p, double frequency) {
    if (!p.enabled) return {{p.preamp_linear * (p.advanced_effects ? p.trim_linear : 1.0f), 0.0}, {0.0, 0.0}};
    const double omega = 6.28318530717958647692 * frequency / p.sample_rate;
    const auto z = std::polar(1.0, -omega);
    const auto biquad = [&](const BiquadCoeffs& c) {
        return (double(c.b0) + double(c.b1) * z + double(c.b2) * z * z) /
               (1.0 + double(c.a1) * z + double(c.a2) * z * z);
    };
    StereoResponse response{biquad(p.dir_coeffs), double(p.gain2) * biquad(p.cross_coeffs)};
    if (p.advanced_effects) {
        const double delay = p.delay_samples <= 0.001f ? 0.0 : p.delay_samples;
        const double integral = std::floor(delay), fraction = delay - integral;
        response.cross *= biquad(p.shadow_coeffs) * biquad(p.apf_coeffs) *
                          std::polar(1.0, -omega * integral) * (1.0 - fraction + fraction * z);
        response.direct *= p.trim_linear;
        response.cross *= p.trim_linear;
    }
    response.direct *= p.preamp_linear;
    response.cross *= p.preamp_linear;
    return response;
}

} // namespace crossfeed
