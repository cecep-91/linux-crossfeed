#include "response.hpp"
#include "presets.hpp"
#include <cassert>
#include <iostream>
#include <vector>

using namespace crossfeed;

int main() {
    // Compare the GUI's closed-form model with actual float DSP impulses, across
    // presets, modes, rates and fractional delays. No GTK/audio server required.
    constexpr size_t frames = 32768;
    for (float rate : {8000.f, 44100.f, 48000.f, 96000.f, 192000.f}) {
        for (const auto& preset : g_presets) {
            for (bool advanced : {false, true}) {
                CrossfeedDSP dsp;
                assert(dsp.set_all_params(rate, preset.level_db, preset.freq_hz,
                    preset.delay_us, preset.phase_apf_hz, preset.center_trim_db,
                    preset.shadow_hz, advanced, true));
                const auto params = dsp.get_params();
                std::vector<float> in(2 * frames), out(2 * frames);
                in[0] = 1;
                dsp.process_interleaved(in.data(), out.data(), frames);
                for (double f : {0.0, 80.0, 410.0, 1500.0, double(rate) * 0.45}) {
                    const auto z = std::polar(1.0, -6.28318530717958647692 * f / rate);
                    std::complex<double> phase{1, 0}, direct{}, cross{};
                    for (size_t n = 0; n < frames; ++n, phase *= z) {
                        direct += double(out[2*n]) * phase;
                        cross += double(out[2*n+1]) * phase;
                    }
                    const auto prediction = response_at(params, f);
                    assert(std::abs(prediction.direct - direct) < 0.001);
                    assert(std::abs(prediction.cross - cross) < 0.001);
                }
                auto bypass = params;
                bypass.enabled = false;
                assert(response_at(bypass, 410).mono_db() == 0);
                assert(response_at(bypass, 410).side_db() == 0);
            }
        }
    }
    std::cout << "PASS: GUI response matches DSP impulses across all presets, modes and 8–192 kHz; bypass is flat.\n";
}
