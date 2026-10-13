#include "dsp.hpp"
#include "presets.hpp"
#include <complex>
#include <iomanip>
#include <iostream>
#include <vector>

int main() {
    constexpr size_t frames = 16384;
    constexpr double rate = 48000;
    constexpr double pi = 3.14159265358979323846;
    std::cout << std::setprecision(8) << "{\"rate\":48000,\"frames\":16384,\"presets\":[";
    bool first = true;
    for (const auto& p : crossfeed::g_presets) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"id\":\"" << p.id << "\",\"name\":\"" << p.name << "\",\"modes\":[";
        for (bool advanced : {false, true}) {
            if (advanced) std::cout << ',';
            crossfeed::CrossfeedDSP dsp;
            dsp.set_all_params(rate, p.level_db, p.freq_hz, p.delay_us, p.phase_apf_hz,
                               p.center_trim_db, p.shadow_hz, advanced, true);
            std::vector<float> input(frames * 2, 0.f), output(input.size());
            input[0] = 1.f;
            dsp.process_interleaved(input.data(), output.data(), frames);
            std::cout << "{\"name\":\"" << (advanced ? "Advanced" : "Pure") << "\",\"points\":[";
            for (int point = 0; point <= 160; ++point) {
                if (point) std::cout << ',';
                const double f = point == 0 ? 0. : 20. * std::pow(1000., (point - 1) / 159.);
                const auto step = std::exp(std::complex<double>(0, -2 * pi * f / rate));
                std::complex<double> phase(1, 0), direct(0, 0), cross(0, 0);
                for (size_t n = 0; n < frames; ++n, phase *= step) {
                    direct += static_cast<double>(output[2*n]) * phase;
                    cross += static_cast<double>(output[2*n+1]) * phase;
                }
                auto db = [](std::complex<double> v) { return 20. * std::log10(std::max(1e-6, std::abs(v))); };
                std::cout << '[' << f << ',' << db(direct + cross) << ',' << db(direct - cross) << ']';
            }
            std::cout << "]}";
        }
        std::cout << "]}";
    }
    std::cout << "]}\n";
}
