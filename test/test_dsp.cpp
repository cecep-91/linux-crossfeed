#include "dsp.hpp"
#include "config.hpp"
#include "presets.hpp"
#include "audio_math.hpp"
#include <limits>
#include <array>

#include <iostream>
#include <cmath>
#include <cassert>
#include <complex>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <thread>
#include <atomic>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

namespace fs = std::filesystem;
using namespace crossfeed;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Utility: bit-level IEEE-754 float inspection (immune to -ffast-math optimizations)
static inline bool is_nan_or_inf(float x) noexcept {
    uint32_t b;
    std::memcpy(&b, &x, sizeof(b));
    return ((b >> 23) & 0xFF) == 0xFF;
}

static inline bool is_denormal(float x) noexcept {
    uint32_t b;
    std::memcpy(&b, &x, sizeof(b));
    return ((b >> 23) & 0xFF) == 0 && (b & 0x7FFFFF) != 0;
}

static inline bool is_invalid_float(float x) noexcept {
    return is_nan_or_inf(x) || is_denormal(x);
}

// Compute frequency response H(e^{j*w}) of biquad filter
static std::complex<double> biquad_transfer_function(const BiquadCoeffs& c, double f, double fs) {
    double w = 2.0 * M_PI * f / fs;
    std::complex<double> z1(std::cos(-w), std::sin(-w));
    std::complex<double> z2(std::cos(-2.0 * w), std::sin(-2.0 * w));
    std::complex<double> num = static_cast<double>(c.b0) + static_cast<double>(c.b1) * z1 + static_cast<double>(c.b2) * z2;
    std::complex<double> den = 1.0 + static_cast<double>(c.a1) * z1 + static_cast<double>(c.a2) * z2;
    return num / den;
}

static void test_biquad_lowshelf() {
    std::cout << "[TEST] Biquad Low-Shelf Filter..." << std::endl;

    const float sample_rates[] = {44100.0f, 48000.0f, 96000.0f};
    const float gain_dbs[] = {-12.0f, -6.0f, -1.5f, 3.0f};
    const float cutoffs[] = {500.0f, 700.0f, 1200.0f};

    for (float fs : sample_rates) {
        for (float gain_db : gain_dbs) {
            for (float fc : cutoffs) {
                BiquadCoeffs coeffs;
                CrossfeedDSP::compute_lowshelf(coeffs, fs, fc, 0.7071f, gain_db);

                // 1. DC gain check (f -> 0)
                double h_dc = std::abs(biquad_transfer_function(coeffs, 0.0, fs));
                double expected_dc_gain = std::pow(10.0, gain_db / 20.0);
                assert(std::abs(h_dc - expected_dc_gain) < 5e-4);

                // 2. Nyquist gain check (f -> fs / 2): should be ~0 dB (ratio 1.0 +/- 0.05)
                double h_nyq = std::abs(biquad_transfer_function(coeffs, fs / 2.0, fs));
                assert(std::abs(h_nyq - 1.0) <= 0.05);

                // 3. Time-domain simulation verification with Biquad::process
                Biquad biquad;
                float dc_out = 0.0f;
                for (int i = 0; i < 2000; ++i) {
                    dc_out = biquad.process(1.0f, coeffs);
                }
                assert(std::abs(dc_out - static_cast<float>(expected_dc_gain)) < 5e-4f);

                // Nyquist alternating signal (+1, -1, +1, -1, ...)
                biquad.reset();
                float nyq_out = 0.0f;
                for (int i = 0; i < 2000; ++i) {
                    float in = (i % 2 == 0) ? 1.0f : -1.0f;
                    nyq_out = biquad.process(in, coeffs);
                }
                assert(std::abs(std::abs(nyq_out) - 1.0f) <= 0.05f);
            }
        }
    }

    std::cout << "  PASS: Low-shelf DC gain matches 10^(gain_db/20) and Nyquist gain is ~1.0 (0 dB)" << std::endl;
}

static void test_biquad_lowpass() {
    std::cout << "[TEST] Biquad Low-Pass Filter..." << std::endl;

    const float sample_rates[] = {44100.0f, 48000.0f, 96000.0f};
    const float cutoffs[] = {400.0f, 700.0f, 2000.0f};

    for (float fs : sample_rates) {
        for (float fc : cutoffs) {
            BiquadCoeffs coeffs;
            CrossfeedDSP::compute_lowpass(coeffs, fs, fc, 0.5f);

            // 1. DC gain check (f = 0): passes unattenuated (gain ~ 1.0)
            double h_dc = std::abs(biquad_transfer_function(coeffs, 0.0, fs));
            assert(std::abs(h_dc - 1.0) < 1e-4);

            // 2. Nyquist gain check (f = fs / 2): strongly attenuated (< 0.1)
            double h_nyq = std::abs(biquad_transfer_function(coeffs, fs / 2.0, fs));
            assert(h_nyq < 0.1);

            // 3. Time-domain simulation
            Biquad biquad;
            float dc_out = 0.0f;
            for (int i = 0; i < 2000; ++i) {
                dc_out = biquad.process(1.0f, coeffs);
            }
            assert(std::abs(dc_out - 1.0f) < 1e-4f);

            biquad.reset();
            float nyq_out = 0.0f;
            for (int i = 0; i < 2000; ++i) {
                float in = (i % 2 == 0) ? 1.0f : -1.0f;
                nyq_out = biquad.process(in, coeffs);
            }
            assert(std::abs(nyq_out) < 0.1f);
        }
    }

    std::cout << "  PASS: Low-pass DC passes unattenuated (~1.0) and Nyquist is attenuated (<0.1)" << std::endl;
}

static void test_biquad_allpass() {
    std::cout << "[TEST] Biquad All-Pass Filter..." << std::endl;

    const float fs = 48000.0f;
    const float f0 = 1500.0f;
    const float q = 0.7071f;

    BiquadCoeffs coeffs;
    CrossfeedDSP::compute_allpass(coeffs, fs, f0, q);

    // Verify unit magnitude across spectrum
    const double test_freqs[] = {20.0, 100.0, 500.0, 1000.0, 1500.0, 3000.0, 8000.0, 15000.0, 22000.0};
    for (double f : test_freqs) {
        auto H = biquad_transfer_function(coeffs, f, fs);
        double mag = std::abs(H);
        assert(std::abs(mag - 1.0) < 1e-4);
    }

    // Verify phase shift is introduced across frequencies
    auto H_low = biquad_transfer_function(coeffs, 50.0, fs);
    auto H_f0 = biquad_transfer_function(coeffs, f0, fs);
    auto H_high = biquad_transfer_function(coeffs, 10000.0, fs);

    double phase_low = std::arg(H_low);
    double phase_f0 = std::arg(H_f0);
    double phase_high = std::arg(H_high);

    // Near cutoff frequency f0, APF introduces ~ -pi phase shift
    double diff_from_pi = std::min(std::abs(phase_f0 - (-M_PI)), std::abs(phase_f0 - M_PI));
    assert(diff_from_pi < 0.05);

    // Phase shift is non-zero and varies across spectrum
    assert(phase_low != phase_f0);
    assert(phase_f0 != phase_high);

    // Time-domain verification with sine wave
    Biquad biquad;
    float peak_amp = 0.0f;
    for (int i = 0; i < 4000; ++i) {
        float in = std::sin(2.0 * static_cast<float>(M_PI) * f0 * static_cast<float>(i) / fs);
        float out = biquad.process(in, coeffs);
        if (i > 3000) {
            peak_amp = std::max(peak_amp, std::abs(out));
        }
    }
    assert(std::abs(peak_amp - 1.0f) < 0.05f);

    std::cout << "  PASS: All-pass preserves unit magnitude while introducing phase shift" << std::endl;
}

static void test_delay_line_interpolation_and_wrap() {
    std::cout << "[TEST] DelayLine Sub-Sample Interpolation and Wrap-Around..." << std::endl;

    DelayLine dl;
    dl.reset();

    // 1. Basic write and integer delays
    dl.write(10.0f); // 4 steps ago
    dl.write(20.0f); // 3 steps ago
    dl.write(30.0f); // 2 steps ago
    dl.write(40.0f); // 1 step ago
    dl.write(50.0f); // 0 steps ago (most recent)

    assert(dl.read(0.0f) == 50.0f);
    assert(dl.read(1.0f) == 40.0f);
    assert(dl.read(2.0f) == 30.0f);
    assert(dl.read(3.0f) == 20.0f);
    assert(dl.read(4.0f) == 10.0f);

    // 2. Sub-sample linear interpolation at 2.5f
    // sample 2 is 30.0f, sample 3 is 20.0f -> 0.5 * 30.0 + 0.5 * 20.0 = 25.0f
    float read_2_5 = dl.read(2.5f);
    assert(std::abs(read_2_5 - 25.0f) < 1e-6f);

    // Fractional interpolation weights
    float read_2_25 = dl.read(2.25f); // 0.75 * 30.0 + 0.25 * 20.0 = 27.5f
    assert(std::abs(read_2_25 - 27.5f) < 1e-6f);

    float read_2_75 = dl.read(2.75f); // 0.25 * 30.0 + 0.75 * 20.0 = 22.5f
    assert(std::abs(read_2_75 - 22.5f) < 1e-6f);

    // 3. Step impulse test
    dl.reset();
    dl.write(0.0f);
    dl.write(0.0f);
    dl.write(1.0f); // impulse at delay 2
    dl.write(0.0f);
    dl.write(0.0f);

    assert(dl.read(2.0f) == 1.0f);
    assert(dl.read(3.0f) == 0.0f);
    assert(std::abs(dl.read(2.5f) - 0.5625f) < 1e-6f); // Hermite cubic h0(0.5)

    // 4. Circular buffer wrap-around past index 2048
    dl.reset();
    constexpr size_t TOTAL_SAMPLES = 5000;
    for (size_t i = 0; i < TOTAL_SAMPLES; ++i) {
        dl.write(static_cast<float>(i));
    }
    // Most recently written sample is (TOTAL_SAMPLES - 1)
    const float last_val = static_cast<float>(TOTAL_SAMPLES - 1);

    for (int d = 0; d < 1000; ++d) {
        float expected = last_val - static_cast<float>(d);
        float actual = dl.read(static_cast<float>(d));
        assert(std::abs(actual - expected) < 1e-4f);
    }

    // Fractional read across buffer boundaries
    float wrap_frac = dl.read(2.5f);
    assert(std::abs(wrap_frac - (last_val - 2.5f)) < 1e-4f);

    std::cout << "  PASS: DelayLine fractional interpolation and circular wrap verified" << std::endl;
}

static void test_crossfeed_dsp_stability() {
    std::cout << "[TEST] CrossfeedDSP Processing Stability (48,000 frames)..." << std::endl;

    CrossfeedDSP dsp;
    constexpr size_t FRAMES = 48000;

    for (bool adv : {true, false}) {
        dsp.set_all_params(48000.0f, -10.0f, 700.0f, 280.0f, 1500.0f, -1.5f, 3000.0f, adv, true);

        // --- 1. Interleaved Silence ---
        dsp.reset();
        std::vector<float> in_silence(FRAMES * 2, 0.0f);
        std::vector<float> out_silence(FRAMES * 2, 0.0f);
        dsp.process_interleaved(in_silence.data(), out_silence.data(), FRAMES);
        for (size_t i = 0; i < FRAMES * 2; ++i) {
            assert(!is_invalid_float(out_silence[i]));
            assert(out_silence[i] == 0.0f);
        }

        // --- 2. Interleaved Impulse ---
        dsp.reset();
        std::vector<float> in_impulse(FRAMES * 2, 0.0f);
        in_impulse[0] = 1.0f; // Left impulse
        in_impulse[1] = 0.5f; // Right impulse
        std::vector<float> out_impulse(FRAMES * 2, 0.0f);
        dsp.process_interleaved(in_impulse.data(), out_impulse.data(), FRAMES);
        for (size_t i = 0; i < FRAMES * 2; ++i) {
            assert(!is_invalid_float(out_impulse[i]));
        }

        // --- 3. Planar Silence ---
        dsp.reset();
        std::vector<float> in_l_silence(FRAMES, 0.0f), in_r_silence(FRAMES, 0.0f);
        std::vector<float> out_l_silence(FRAMES, 0.0f), out_r_silence(FRAMES, 0.0f);
        dsp.process_planar(in_l_silence.data(), in_r_silence.data(),
                           out_l_silence.data(), out_r_silence.data(), FRAMES);
        for (size_t i = 0; i < FRAMES; ++i) {
            assert(!is_invalid_float(out_l_silence[i]));
            assert(!is_invalid_float(out_r_silence[i]));
            assert(out_l_silence[i] == 0.0f);
            assert(out_r_silence[i] == 0.0f);
        }

        // --- 4. Planar Impulse ---
        dsp.reset();
        std::vector<float> in_l_impulse(FRAMES, 0.0f), in_r_impulse(FRAMES, 0.0f);
        in_l_impulse[0] = 1.0f;
        in_r_impulse[0] = 0.0f;
        std::vector<float> out_l_impulse(FRAMES, 0.0f), out_r_impulse(FRAMES, 0.0f);
        dsp.process_planar(in_l_impulse.data(), in_r_impulse.data(),
                           out_l_impulse.data(), out_r_impulse.data(), FRAMES);
        for (size_t i = 0; i < FRAMES; ++i) {
            assert(!is_invalid_float(out_l_impulse[i]));
            assert(!is_invalid_float(out_r_impulse[i]));
        }
    }

    // --- 5. Pure-mode bypass (the loop above ends with effects off) ---
    dsp.set_enabled(false);
    assert(!dsp.is_enabled());
    std::vector<float> test_in(256 * 2, 0.75f);
    std::vector<float> test_out(256 * 2, 0.0f);
    for (int block = 0; block < 5; ++block)
        dsp.process_interleaved(test_in.data(), test_out.data(), 256);
    for (size_t i = 0; i < 256 * 2; ++i) {
        assert(test_out[i] == 0.75f);
    }

    std::cout << "  PASS: Silence and impulse processed with no NaN, Inf, or denormals" << std::endl;
}

static void test_config_state_roundtrip() {
    std::cout << "[TEST] Config State Round-Trip..." << std::endl;

    // Use isolated temp directory for XDG_CONFIG_HOME
    std::string template_path = (fs::temp_directory_path() / "crossfeed-config-XXXXXX").string();
    char* created = mkdtemp(template_path.data());
    assert(created);
    fs::path tmp_dir(created);

    const char* prev_xdg = std::getenv("XDG_CONFIG_HOME");
    std::string prev_xdg_val = prev_xdg ? prev_xdg : "";
    setenv("XDG_CONFIG_HOME", tmp_dir.c_str(), 1);

    ConfigState orig;
    orig.enabled = false;
    orig.level_db = -18.5f;
    orig.freq_hz = 620.0f;
    orig.delay_us = 340.0f;
    orig.phase_apf_hz = 1350.0f;
    orig.center_trim_db = -3.2f;
    orig.shadow_hz = 4800.0f;
    orig.advanced_effects = false;
    orig.backend = "alsa";
    orig.target_sink = "test_sink_device_0";
    orig.sample_rate = 96000;
    orig.buffer_frames = 1024;

    bool saved = Config::save_state(orig);
    assert(saved);
    assert(fs::exists(Config::get_state_file_path()));

    ConfigState loaded;
    // Overwrite with initial dummies to ensure values are actually read from file
    loaded.enabled = true;
    loaded.level_db = 0.0f;
    loaded.freq_hz = 0.0f;
    loaded.delay_us = 0.0f;
    loaded.phase_apf_hz = 0.0f;
    loaded.center_trim_db = 0.0f;
    loaded.shadow_hz = 0.0f;
    loaded.advanced_effects = true;
    loaded.backend = "";
    loaded.target_sink = "";
    loaded.sample_rate = 0;
    loaded.buffer_frames = 0;

    bool loaded_ok = Config::load_state(loaded);
    assert(loaded_ok);

    assert(loaded.enabled == orig.enabled);
    assert(std::abs(loaded.level_db - orig.level_db) < 1e-4f);
    assert(std::abs(loaded.freq_hz - orig.freq_hz) < 1e-4f);
    assert(std::abs(loaded.delay_us - orig.delay_us) < 1e-4f);
    assert(std::abs(loaded.phase_apf_hz - orig.phase_apf_hz) < 1e-4f);
    assert(std::abs(loaded.center_trim_db - orig.center_trim_db) < 1e-4f);
    assert(std::abs(loaded.shadow_hz - orig.shadow_hz) < 1e-4f);
    assert(loaded.advanced_effects == orig.advanced_effects);
    assert(loaded.backend == orig.backend);
    assert(loaded.target_sink == orig.target_sink);
    assert(loaded.sample_rate == orig.sample_rate);
    assert(loaded.buffer_frames == orig.buffer_frames);

    // Cleanup
    fs::remove_all(tmp_dir);
    if (!prev_xdg_val.empty()) {
        setenv("XDG_CONFIG_HOME", prev_xdg_val.c_str(), 1);
    } else {
        unsetenv("XDG_CONFIG_HOME");
    }

    std::cout << "  PASS: ConfigState successfully saved, reloaded, and matched" << std::endl;
}

static void test_lockfree_concurrent_processing() {
    std::cout << "[TEST] Lock-Free Concurrent Parameter Updates & Audio Processing..." << std::endl;

    CrossfeedDSP dsp;
    std::atomic<bool> stop_flag{false};
    constexpr size_t BLOCK_FRAMES = 256;
    std::atomic<size_t> blocks_processed{0};

    // Thread 1: Real-time Audio Processing Thread
    std::thread audio_thread([&]() {
        std::vector<float> in_buf(BLOCK_FRAMES * 2, 0.5f);
        std::vector<float> out_buf(BLOCK_FRAMES * 2, 0.0f);
        std::vector<float> in_l(BLOCK_FRAMES, 0.5f), in_r(BLOCK_FRAMES, -0.5f);
        std::vector<float> out_l(BLOCK_FRAMES, 0.0f), out_r(BLOCK_FRAMES, 0.0f);

        while (!stop_flag.load(std::memory_order_relaxed)) {
            if (blocks_processed.load() % 16 == 0)
                dsp.request_sample_rate(blocks_processed.load() % 32 == 0 ? 48000 : 96000);
            dsp.process_interleaved(in_buf.data(), out_buf.data(), BLOCK_FRAMES);
            for (float s : out_buf) {
                assert(!is_invalid_float(s));
            }

            dsp.process_planar(in_l.data(), in_r.data(), out_l.data(), out_r.data(), BLOCK_FRAMES);
            for (size_t i = 0; i < BLOCK_FRAMES; ++i) {
                assert(!is_invalid_float(out_l[i]));
                assert(!is_invalid_float(out_r[i]));
            }

            blocks_processed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    while (blocks_processed.load() == 0) std::this_thread::yield();

    // Thread 2: Writer Thread updating various individual parameters
    std::thread writer_thread_1([&]() {
        for (int i = 0; i < 500 && !stop_flag.load(std::memory_order_relaxed); ++i) {
            float level = -6.0f - static_cast<float>(i % 20);
            float freq = 300.0f + static_cast<float>((i * 17) % 1500);
            dsp.set_level_db(level);
            dsp.set_freq_hz(freq);
            dsp.set_delay_us(100.0f + static_cast<float>((i * 23) % 600));
            dsp.set_phase_apf_hz(500.0f + static_cast<float>((i * 31) % 3000));
            dsp.set_center_trim_db(-1.0f - static_cast<float>(i % 5));
            dsp.set_shadow_hz(2000.0f + static_cast<float>((i * 47) % 5000));
            dsp.set_advanced_effects(i % 2 == 0);
            dsp.set_enabled(i % 3 != 0);
            std::this_thread::yield();
        }
    });

    // Thread 3: Writer Thread calling set_all_params, set_params, and reset
    std::thread writer_thread_2([&]() {
        for (int i = 0; i < 500 && !stop_flag.load(std::memory_order_relaxed); ++i) {
            dsp.service_sample_rate();
            if (i % 5 == 0) {
                dsp.reset();
            }
            dsp.set_params(48000.0f, -12.0f, 650.0f, true);
            dsp.set_all_params(44100.0f, -15.0f, 750.0f, 320.0f, 1600.0f, -2.0f, 3500.0f, true, true);
            std::this_thread::yield();
        }
    });

    writer_thread_1.join();
    writer_thread_2.join();
    stop_flag.store(true, std::memory_order_relaxed);
    audio_thread.join();

    assert(blocks_processed.load() > 0);
    std::cout << "  PASS: Concurrently updated parameters across multiple threads while processing "
              << blocks_processed.load() << " audio blocks with finite outputs (run test-tsan to check races)" << std::endl;
}

static void test_presets() {
    std::cout << "[TEST] Presets Lookup & Detection..." << std::endl;

    // 1. find_preset lookup
    assert(find_preset("meier") != nullptr);
    assert(std::string(find_preset("meier")->name) == "Jan Meier");
    assert(find_preset("Jan Meier") != nullptr);
    assert(find_preset("chumoy") != nullptr);
    assert(find_preset("bs2b") != nullptr);
    assert(std::string(find_preset("bs2b")->name) == "Bauer BS2B");
    assert(find_preset("linkwitz") != nullptr);
    assert(find_preset("studio") != nullptr);
    assert(find_preset("Studio 30°") != nullptr);
    assert(find_preset("jm1") != nullptr);
    assert(find_preset("diffuse") != nullptr);
    assert(find_preset("unknown_nonexistent") == nullptr);

    // 2. detect_active_preset matching
    const auto* p_meier = detect_active_preset(-9.5f, 650.0f, 280.0f, 1500.0f, -1.5f, 3200.0f, true);
    assert(p_meier != nullptr && std::string(p_meier->id) == "meier");

    // JM-1 Diffuse
    const auto* p_jm1 = detect_active_preset(-10.0f, 750.0f, 260.0f, 1600.0f, -1.2f, 3600.0f, true);
    assert(p_jm1 != nullptr && std::string(p_jm1->id) == "jm1");

    // Tolerant matching (within small float tolerances)
    const auto* p_meier_tol = detect_active_preset(-9.52f, 651.0f, 281.0f, 1505.0f, -1.51f, 3210.0f, true);
    assert(p_meier_tol != nullptr && std::string(p_meier_tol->id) == "meier");

    // Bauer BS2B
    const auto* p_bs2b = detect_active_preset(-4.5f, 700.0f, 350.0f, 1200.0f, -2.5f, 2500.0f, true);
    assert(p_bs2b != nullptr && std::string(p_bs2b->id) == "bs2b");

    // Advanced effects disabled -> must return nullptr (Pure Crossfeed)
    assert(detect_active_preset(-9.5f, 650.0f, 280.0f, 1500.0f, -1.5f, 3200.0f, false) == nullptr);

    // Custom values differing from all presets -> nullptr
    assert(detect_active_preset(-20.0f, 400.0f, 100.0f, 800.0f, 0.0f, 1500.0f, true) == nullptr);

    std::cout << "  PASS: Preset lookup, ID matching, and tolerance detection verified" << std::endl;
}


static bool stable(const BiquadCoeffs& c) {
    const std::complex<double> discriminant(c.a1 * static_cast<double>(c.a1) - 4.0 * c.a2, 0);
    const auto root = std::sqrt(discriminant);
    return std::abs((-static_cast<double>(c.a1) + root) * 0.5) < 1.0 &&
           std::abs((-static_cast<double>(c.a1) - root) * 0.5) < 1.0;
}

static void test_parameter_boundaries() {
    std::cout << "[TEST] Finite validation, Nyquist limits, and 192 kHz precision...\n";
    CrossfeedDSP dsp;
    const auto initial = dsp.get_params();
    for (float bad : {std::numeric_limits<float>::quiet_NaN(),
                      std::numeric_limits<float>::infinity(),
                      -std::numeric_limits<float>::infinity()}) {
        assert(!dsp.set_level_db(bad));
        assert(!dsp.set_freq_hz(bad));
        assert(!dsp.set_delay_us(bad));
        assert(!dsp.set_phase_apf_hz(bad));
        assert(!dsp.set_center_trim_db(bad));
        assert(!dsp.set_shadow_hz(bad));
        assert(!dsp.set_params(bad, -10, 700, true));
        assert(dsp.get_params().generation == initial.generation);
        DelayLine delay;
        assert(delay.read(bad) == 0.0f);
    }
    assert(!dsp.set_params(0, -10, 700, true));
    assert(!dsp.set_params(384000, -10, 700, true));
    for (float rate : {8000.f, 11025.f, 16000.f, 44100.f, 48000.f, 96000.f, 192000.f}) {
        for (float cutoff : {200.f, 650.f, 2000.f}) {
            assert(dsp.set_all_params(rate, -9.5f, cutoff, 800, 4000, -1.5f, 8000, true, true));
            const auto p = dsp.get_params();
            assert(p.shadow_hz <= rate * .45f && p.phase_apf_hz <= rate * .45f);
            for (const auto& c : {p.dir_coeffs, p.cross_coeffs, p.apf_coeffs, p.shadow_coeffs}) assert(stable(c));
            const auto lp = biquad_transfer_function(p.cross_coeffs, 0, rate);
            const auto shelf = biquad_transfer_function(p.dir_coeffs, 0, rate);
            const double expected = std::pow(10.0, (FULL_DIR_GAIN * (p.gain2 / FULL_GAIN2)) / 20.0);
            assert(std::abs(lp.real() - 1.0) < .002);
            assert(std::abs(shelf.real() - expected) < .003);
            dsp.reset();
            std::vector<float> input(32768 * 2, 0.f), output(input.size());
            input[0] = 1.f;
            dsp.process_interleaved(input.data(), output.data(), 32768);
            for (float x : output) assert(!is_invalid_float(x) && std::abs(x) < 2.f);
        }
    }
    BiquadCoeffs invalid;
    CrossfeedDSP::compute_lowpass(invalid, 8000, 5000, .7071f);
    assert(invalid.b0 == 1.f && invalid.a1 == 0.f); // Public helper fails closed to identity.
}

static std::complex<double> cross_response(const FilterParams& p, double f) {
    const double omega = 2.0 * M_PI * f / p.sample_rate;
    const auto z = std::exp(std::complex<double>(0, -omega));
    const int integer = static_cast<int>(p.delay_samples);
    const double fraction = p.delay_samples - integer;
    auto h = static_cast<double>(p.gain2) * biquad_transfer_function(p.cross_coeffs, f, p.sample_rate);
    if (p.advanced_effects) {
        std::complex<double> frac_filter;
        if (integer < 1) {
            frac_filter = 1.0 - fraction + fraction * z;
        } else {
            const double f_frac = fraction;
            const double h_minus1 = -0.5 * f_frac * f_frac * f_frac + 1.0 * f_frac * f_frac - 0.5 * f_frac;
            const double h_0 = 1.5 * f_frac * f_frac * f_frac - 2.5 * f_frac * f_frac + 1.0;
            const double h_1 = -1.5 * f_frac * f_frac * f_frac + 2.0 * f_frac * f_frac + 0.5 * f_frac;
            const double h_2 = 0.5 * f_frac * f_frac * f_frac - 0.5 * f_frac * f_frac;
            frac_filter = h_minus1 * std::conj(z) + h_0 + h_1 * z + h_2 * z * z;
        }
        h *= biquad_transfer_function(p.shadow_coeffs, f, p.sample_rate) *
             biquad_transfer_function(p.apf_coeffs, f, p.sample_rate) *
             std::pow(z, integer) * frac_filter;
    }
    return h;
}

static void test_full_chain_response() {
    std::cout << "[TEST] Meier mono/side impulse response and planar/interleaved equivalence...\n";
    constexpr size_t frames = 32768;
    for (bool advanced : {false, true}) {
        for (float side : {1.f, -1.f}) {
            CrossfeedDSP dsp, planar;
            for (auto* engine : {&dsp, &planar})
                assert(engine->set_all_params(48000, -9.5f, 650, 280, 1500, -1.5f, 3200, advanced, true));
            auto p = dsp.get_params();
            std::vector<float> in(frames * 2, 0.f), out(in.size()), l(frames, 0.f), r(frames, 0.f), ol(frames), oright(frames);
            in[0] = l[0] = 1.f; in[1] = r[0] = side;
            dsp.process_interleaved(in.data(), out.data(), frames);
            planar.process_planar(l.data(), r.data(), ol.data(), oright.data(), frames);
            for (size_t n = 0; n < frames; ++n) {
                assert(out[2*n] == ol[n] && out[2*n+1] == oright[n]);
            }
            for (double f : {0., 200., 410., 650., 1500., 5000.}) {
                std::complex<double> actual(0, 0);
                const auto step = std::exp(std::complex<double>(0, -2.0 * M_PI * f / p.sample_rate));
                std::complex<double> phase(1, 0);
                for (size_t n = 0; n < frames; ++n, phase *= step) actual += static_cast<double>(ol[n]) * phase;
                auto expected = biquad_transfer_function(p.dir_coeffs, f, p.sample_rate) + static_cast<double>(side) * cross_response(p, f);
                if (advanced) expected *= static_cast<double>(p.trim_linear);
                assert(std::abs(actual - expected) < 2e-4);
                if (advanced && side == 1.f && f == 410.) {
                    const double db = 20 * std::log10(std::abs(actual));
                    // Characterize existing voicing; this is NOT a flatness target.
                    assert(db > -5.7 && db < -5.5);
                }
            }
        }
    }
}

static void test_rate_handoff() {
    std::cout << "[TEST] Rate mismatch passthrough and bounded recovery...\n";
    CrossfeedDSP dsp;
    std::array<float, 512> in, out;
    in.fill(.25f);
    dsp.process_interleaved(in.data(), out.data(), 256);
    dsp.request_sample_rate(96000);
    dsp.process_interleaved(in.data(), out.data(), 256);
    assert(in == out); // Control worker has deliberately not run yet.
    assert(dsp.set_level_db(-15)); // Concurrent control change incorporates requested rate.
    assert(dsp.service_sample_rate());
    assert(dsp.get_sample_rate() == 96000 && dsp.get_level_db() == -15);
    for (int i = 0; i < 20; ++i) dsp.process_interleaved(in.data(), out.data(), 256);
    assert(out != in);
    for (float x : out) assert(!is_invalid_float(x));
    dsp.request_sample_rate(4000);
    assert(!dsp.service_sample_rate());
    dsp.process_interleaved(in.data(), out.data(), 256);
    assert(in == out);
    dsp.request_sample_rate(48000);
    assert(dsp.service_sample_rate());
    for (int i = 0; i < 12; ++i) dsp.process_interleaved(in.data(), out.data(), 256);
    assert(out != in && dsp.get_level_db() == -15);

    // Verify that requested cutoffs are not permanently destroyed by a temporary sample-rate dip:
    assert(dsp.set_all_params(48000, -10.0f, 700.0f, 280.0f, 3000.0f, -1.5f, 3800.0f, true, true));
    assert(dsp.get_requested_params().shadow_hz == 3800.0f);
    assert(dsp.get_requested_params().phase_apf_hz == 3000.0f);
    assert(dsp.get_params().shadow_hz == 3800.0f);

    // Drop to 8000 Hz: ceiling is 3600 Hz
    dsp.request_sample_rate(8000);
    assert(dsp.service_sample_rate());
    assert(dsp.get_params().sample_rate == 8000.0f);
    assert(dsp.get_params().shadow_hz <= 3600.0f);
    assert(dsp.get_params().phase_apf_hz <= 3600.0f);
    assert(dsp.get_requested_params().shadow_hz == 3800.0f);
    assert(dsp.get_requested_params().phase_apf_hz == 3000.0f);

    // Restore to 48000 Hz: cutoffs must recover to original values!
    dsp.request_sample_rate(48000);
    assert(dsp.service_sample_rate());
    assert(dsp.get_params().sample_rate == 48000.0f);
    assert(dsp.get_params().shadow_hz == 3800.0f);
    assert(dsp.get_params().phase_apf_hz == 3000.0f);
}

static void test_smooth_transitions() {
    std::cout << "[TEST] Smoothed mode, bypass, and parameter changes...\n";
    CrossfeedDSP dsp;
    std::array<float, 128> in, out;
    in.fill(.25f);
    for (int i = 0; i < 100; ++i) dsp.process_interleaved(in.data(), out.data(), 64);
    float previous = out.back();
    for (int change = 0; change < 8; ++change) {
        dsp.set_all_params(48000, change % 2 ? -3.f : -30.f, change % 2 ? 2000.f : 200.f,
                           change % 2 ? 800.f : 0.f, 200, change % 2 ? -6.f : 0.f, 1000,
                           change % 3 == 0, change % 4 != 0);
        for (int block = 0; block < 40; ++block) {
            dsp.process_interleaved(in.data(), out.data(), 64);
            for (size_t n = 0; n < 64; ++n) {
                const float sample = out[2*n];
                assert(std::isfinite(sample));
                assert(std::abs(sample - previous) < .015f);
                previous = sample;
            }
        }
    }
    // Updates received during a fade must eventually adopt the latest target.
    dsp.set_enabled(true);
    dsp.process_interleaved(in.data(), out.data(), 64);
    dsp.set_enabled(false);
    for (int i = 0; i < 60; ++i) dsp.process_interleaved(in.data(), out.data(), 64);
    assert(in == out);
}

static void test_denormal_scope() {
    std::cout << "[TEST] Production denormal policy and caller FP environment restoration...\n";
#if defined(__SSE2__)
    const unsigned original = _mm_getcsr();
    _mm_setcsr(original & ~((1u << 15) | (1u << 6)));
    const unsigned before = _mm_getcsr();
#endif
    CrossfeedDSP dsp;
    std::vector<float> in(96000 * 2, 0.f), out(in.size());
    in[0] = 1.f;
    dsp.process_interleaved(in.data(), out.data(), 96000);
#if defined(__SSE2__)
    assert(_mm_getcsr() == before);
    _mm_setcsr(original);
#endif
    for (float x : out) assert(!is_invalid_float(x));
}

static void test_soft_knee_limiter() {
    std::cout << "[TEST] Soft-knee limiter ceiling and transparency...\n";
    CrossfeedDSP dsp;
    assert(dsp.set_all_params(48000, -10.0f, 700.0f, 280.0f, 1500.0f, -1.5f, 3000.0f, true, true, true));
    assert(dsp.get_limiter());
    std::array<float, 512> in, out;

    // Transparent below knee threshold T = 0.95
    in.fill(0.70f);
    dsp.process_interleaved(in.data(), out.data(), 256);
    for (int i = 20; i < 256; ++i) {
        assert(std::abs(out[2*i]) < 0.95f);
    }

    // Limit extreme inputs strictly below 1.0
    for (float hot : {1.2f, 1.5f, 2.0f, 5.0f, 10.0f}) {
        in.fill(hot);
        dsp.process_interleaved(in.data(), out.data(), 256);
        for (int i = 0; i < 256; ++i) {
            assert(std::abs(out[2*i]) < 1.0f);
            assert(std::abs(out[2*i+1]) < 1.0f);
        }
    }
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Running Crossfeed DSP & Core Test Suite" << std::endl;
    std::cout << "========================================" << std::endl;

    test_biquad_lowshelf();
    test_biquad_lowpass();
    test_biquad_allpass();
    test_delay_line_interpolation_and_wrap();
    test_crossfeed_dsp_stability();
    test_config_state_roundtrip();
    test_lockfree_concurrent_processing();
    test_presets();
    test_parameter_boundaries();
    test_full_chain_response();
    test_rate_handoff();
    test_smooth_transitions();
    test_denormal_scope();
    test_soft_knee_limiter();

    std::cout << "========================================" << std::endl;
    std::cout << "All DSP and core characterization tests passed!" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
