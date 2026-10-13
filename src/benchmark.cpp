#include "benchmark.hpp"
#include "dsp.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>

namespace crossfeed {

static void bench_rate(float rate, size_t frames, bool decay, bool changes) {
    CrossfeedDSP dsp;
    dsp.set_params(rate, -10.0f, 700.0f, true);
    std::vector<float> in(frames * 2, 0.0f), out(in.size());
    if (decay) in[0] = 0.5f;
    else for (size_t n = 0; n < frames; ++n) {
        in[2*n] = 0.4f * std::sin(6.283185307179586 * 440.0 * n / rate);
        in[2*n+1] = 0.3f * std::sin(6.283185307179586 * 660.0 * n / rate);
    }
    dsp.process_interleaved(in.data(), out.data(), frames);
    if (decay) std::fill(in.begin(), in.end(), 0.f);
    double elapsed = 0.0, worst = 0.0;
    constexpr int blocks = 2000;
    for (int block = 0; block < blocks; ++block) {
        // Model control updates outside the measured callback interval.
        if (changes && block % 32 == 0)
            dsp.set_delay_us(block % 64 == 0 ? 100.f : 700.f);
        const auto start = std::chrono::steady_clock::now();
        dsp.process_interleaved(in.data(), out.data(), frames);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        elapsed += seconds;
        worst = std::max(worst, seconds);
    }
    std::cout << std::fixed << std::setprecision(0) << "  " << rate << " Hz, " << frames
              << " frames, " << (decay ? "decay  " : changes ? "changes" : "steady ")
              << std::setprecision(2) << ": mean " << elapsed / blocks * 1e6
              << " us, max observed " << worst * 1e6 << " us, "
              << (elapsed / blocks) / (frames / rate) * 100.0 << "% of quantum on average\n";
}

void run_benchmark() {
    std::cout << "Crossfeed DSP callback computation benchmark\n";
    for (float rate : {44100.f, 48000.f, 96000.f, 192000.f}) {
        const size_t frames = rate >= 192000 ? 1024 : rate >= 96000 ? 512 : 256;
        bench_rate(rate, frames, false, false);
        bench_rate(rate, frames, true, false);
        bench_rate(rate, frames, false, true);
    }
    std::cout << "These are offline processing times, not graph/device latency or a guarantee against xruns.\n";
}

} // namespace crossfeed
