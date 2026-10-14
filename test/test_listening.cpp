#include "dsp.hpp"
#include "presets.hpp"
#include "response.hpp"
#include "user_presets.hpp"
#include <array>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <thread>

using namespace crossfeed;

static void preset(CrossfeedDSP& dsp, unsigned index, float rate) {
    const auto& p = g_presets[index];
    assert(dsp.set_all_params(rate, p.level_db, p.freq_hz, p.delay_us, p.phase_apf_hz,
        p.center_trim_db, p.shadow_hz, true, true));
}

// Dry follows the same scalar trim as processed audio, independently of input
// content. Compare processed samples against an otherwise identical 0 dB engine.
static void shared_trim() {
    for (float rate : {8000.f, 44100.f, 48000.f, 96000.f, 192000.f}) {
        for (bool advanced : {false, true}) {
            for (float trim : {-6.f, -2.f, 0.f}) {
                CrossfeedDSP wet, reference, dry, planar;
                for (auto* dsp : {&wet, &reference, &dry, &planar}) {
                    preset(*dsp, 0, rate);
                    dsp->set_advanced_effects(advanced);
                    dsp->set_preamp_db(-3);
                    dsp->set_center_trim_db(dsp == &reference ? 0 : trim);
                }
                dry.set_enabled(false); planar.set_enabled(false);
                std::array<float, 512> in{}, output{}, neutral{}, bypass{};
                std::array<float, 256> left{}, right{}, out_left{}, out_right{};
                const float trim_gain = std::pow(10.f, (advanced ? trim : 0) / 20.f);
                const float gain = trim_gain * std::pow(10.f, -3.f / 20.f);
                for (unsigned block = 0; block < 12; ++block) {
                    for (unsigned n = 0; n < 256; ++n) {
                        left[n] = in[2*n] = .2f * std::sin(6.28318530718 * 410 * (block*256+n) / rate);
                        right[n] = in[2*n+1] = .15f * std::cos(6.28318530718 * 2000 * (block*256+n) / rate);
                    }
                    wet.process_interleaved(in.data(), output.data(), 256);
                    reference.process_interleaved(in.data(), neutral.data(), 256);
                    dry.process_interleaved(in.data(), bypass.data(), 256);
                    planar.process_planar(left.data(), right.data(), out_left.data(), out_right.data(), 256);
                    for (unsigned n = 0; n < 512; ++n) {
                        assert(std::abs(output[n] - neutral[n] * trim_gain) < 1e-6);
                        assert(std::abs(bypass[n] - in[n] * gain) < 1e-6);
                    }
                    for (unsigned n = 0; n < 256; ++n) {
                        assert(out_left[n] == bypass[2*n] && out_right[n] == bypass[2*n+1]);
                    }
                }
                for (double hz : {20., 410., 2000.}) {
                    const auto response = response_at(dry.get_params(), hz);
                    const double expected = (advanced ? trim : 0) - 3;
                    assert(std::abs(response.mono_db() - expected) < 1e-5);
                    assert(std::abs(response.side_db() - expected) < 1e-5);
                }
            }
        }
    }
    CrossfeedDSP dsp;
    dsp.set_enabled(false);
    dsp.set_center_trim_db(0);
    std::array<float, 512> in{}, out{};
    in.fill(.25f);
    dsp.process_interleaved(in.data(), out.data(), 256);
    dsp.set_center_trim_db(-2);
    float previous = out.back();
    for (unsigned block = 0; block < 5; ++block) {
        dsp.process_interleaved(in.data(), out.data(), 256);
        for (float sample : out) {
            assert(sample <= previous + 1e-7f);
            assert(std::abs(sample - previous) < .001f);
            previous = sample;
        }
    }
    const float expected = .25f * std::pow(10.f, -2.f / 20.f);
    assert(std::abs(out.back() - expected) < 1e-6);
    dsp.store_slot(0);
    dsp.set_center_trim_db(-4); dsp.store_slot(1);
    dsp.recall_slot(0); dsp.set_enabled(false);
    for (int i = 0; i < 10; ++i) dsp.process_interleaved(in.data(), out.data(), 256);
    assert(std::abs(out.back() - expected) < 1e-6);
    dsp.recall_slot(1); dsp.set_enabled(false);
    for (int i = 0; i < 10; ++i) dsp.process_interleaved(in.data(), out.data(), 256);
    assert(std::abs(out.back() - .25f * std::pow(10.f, -.2f)) < 1e-6);
    assert(!dsp.store_slot(2) && !dsp.recall_slot(2) && !dsp.set_preamp_db(NAN));
    // Rate fallback stays unscaled; prepared recovery fades from that exact gain.
    dsp.request_sample_rate(96000);
    dsp.process_interleaved(in.data(), out.data(), 256);
    assert(in == out);
    dsp.service_sample_rate();
    dsp.process_interleaved(in.data(), out.data(), 256);
    assert(std::abs(out[0] - in[0]) < .001f);
    for (int i = 0; i < 10; ++i) dsp.process_interleaved(in.data(), out.data(), 256);
    assert(std::abs(out.back() - .25f * std::pow(10.f, -.2f)) < 1e-6);
}

static void meters_and_preamp() {
    CrossfeedDSP dsp;
    dsp.set_enabled(false);
    dsp.set_center_trim_db(-2);
    dsp.set_preamp_db(6);
    std::array<float, 128> input{}, output{};
    input.fill(.75f);
    dsp.process_interleaved(input.data(), output.data(), 64);
    auto stats = dsp.get_listening_status();
    assert(stats.output_clip && !stats.input_clip);
    assert(std::abs(stats.output_hold - .75 * std::pow(10, .2)) < 1e-5);
    assert(std::abs(response_at(dsp.get_params(), 1000).mono_db() - 4) < 1e-5);
    input.fill(0);
    dsp.process_interleaved(input.data(), output.data(), 64);
    assert(dsp.get_listening_status().output_clip);
    dsp.reset_meters();
    dsp.process_interleaved(input.data(), output.data(), 64);
    assert(!dsp.get_listening_status().output_clip);
    assert(dsp.get_listening_status().output_hold == 0);
    // Telemetry readers and control publication run concurrently with audio.
    std::atomic<bool> done{false};
    std::thread control([&] {
        for (int i = 0; i < 1000; ++i) {
            dsp.set_center_trim_db(-(i % 6)); dsp.store_slot(i % 2); dsp.recall_slot(i % 2);
            dsp.set_preamp_db(i % 12 - 12); dsp.set_enabled(i % 2);
            if (i % 5 == 0) dsp.reset_meters();
            (void)dsp.get_listening_status();
        }
        done = true;
    });
    std::thread reader([&] { while (!done) (void)dsp.get_listening_status(); });
    input.fill(.1f);
    while (!done) dsp.process_interleaved(input.data(), output.data(), 64);
    control.join(); reader.join();
}

static void library() {
    std::vector<UserPreset> presets{{"My \"quoted\" sound", FilterParams{}}, {"Studio / café", FilterParams{}}};
    presets[0].params.preamp_db = -4;
    presets[1].params.advanced_effects = false;
    std::vector<UserPreset> decoded;
    std::string error;
    assert(UserPresets::decode(UserPresets::encode(presets), decoded, error));
    assert(decoded[0].name == presets[0].name && decoded[0].params.preamp_db == -4);
    assert(!decoded[1].params.advanced_effects);
    const auto original = decoded;
    for (const auto& malformed : {std::string("CROSSFEED_PRESETS 2\n"), std::string(65537, 'x'),
         std::string("CROSSFEED_PRESETS 1\n\"bad\" -10 700 280 1500 -1.5 3000 1 nan\n")}) {
        assert(!UserPresets::decode(malformed, decoded, error));
        assert(decoded.size() == original.size() && decoded[0].name == original[0].name);
    }
    presets.push_back(presets[0]);
    assert(!UserPresets::decode(UserPresets::encode(presets), decoded, error));
    presets.pop_back();
    char folder[] = "/tmp/crossfeed-presets-XXXXXX";
    assert(mkdtemp(folder));
    const std::string filename = std::string(folder) + "/nested/test.crossfeed";
    assert(UserPresets::save(filename, presets, error));
    assert(UserPresets::load(filename, decoded, error));
    assert(decoded[0].name == presets[0].name);
    std::filesystem::remove_all(folder);
}

int main() {
    shared_trim(); meters_and_preamp(); library();
    std::cout << "PASS: dry/processed share active trim at 8–192 kHz; smooth trim changes, A/B recall, rate recovery, meters, concurrent telemetry and preset round-trips.\n";
}
