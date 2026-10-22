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
    double phase_deg() const {
        if (std::abs(cross) < 1e-9 || std::abs(direct) < 1e-9) return 0.0;
        double diff = std::arg(cross) - std::arg(direct);
        constexpr double pi = 3.14159265358979323846;
        while (diff > pi) diff -= 2.0 * pi;
        while (diff <= -pi) diff += 2.0 * pi;
        return diff * (180.0 / pi);
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
        std::complex<double> frac_filter;
        if (integral < 1.0) {
            frac_filter = 1.0 - fraction + fraction * z;
        } else {
            const double f = fraction;
            const double h_minus1 = -0.5 * f * f * f + 1.0 * f * f - 0.5 * f;
            const double h_0 = 1.5 * f * f * f - 2.5 * f * f + 1.0;
            const double h_1 = -1.5 * f * f * f + 2.0 * f * f + 0.5 * f;
            const double h_2 = 0.5 * f * f * f - 0.5 * f * f;
            frac_filter = h_minus1 * std::conj(z) + h_0 + h_1 * z + h_2 * z * z;
        }
        response.cross *= biquad(p.shadow_coeffs) * biquad(p.apf_coeffs) *
                          std::polar(1.0, -omega * integral) * frac_filter;
        response.direct *= p.trim_linear;
        response.cross *= p.trim_linear;
    }
    response.direct *= p.preamp_linear;
    response.cross *= p.preamp_linear;
    return response;
}

inline double group_delay_us(const FilterParams& p, double frequency) {
    if (!p.enabled || frequency < 1.0) return 0.0;
    const double df = std::max(1.0, frequency * 0.005);
    const auto r1 = response_at(p, frequency - df);
    const auto r2 = response_at(p, frequency + df);
    if (std::abs(r1.cross) < 1e-9 || std::abs(r1.direct) < 1e-9 ||
        std::abs(r2.cross) < 1e-9 || std::abs(r2.direct) < 1e-9) return 0.0;
    double p1 = std::arg(r1.cross) - std::arg(r1.direct);
    double p2 = std::arg(r2.cross) - std::arg(r2.direct);
    constexpr double pi = 3.14159265358979323846;
    double dphi = p2 - p1;
    while (dphi > pi) dphi -= 2.0 * pi;
    while (dphi <= -pi) dphi += 2.0 * pi;
    const double domega = 2.0 * pi * (2.0 * df);
    const double tau_sec = -dphi / domega;
    return tau_sec * 1e6;
}

} // namespace crossfeed
