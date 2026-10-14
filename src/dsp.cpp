#include "dsp.hpp"
#include "audio_math.hpp"
#include <algorithm>
#include <cstring>

namespace crossfeed {

constexpr double PI = 3.14159265358979323846;

CrossfeedDSP::CrossfeedDSP() {
    recompute_coeffs(control_params_);
    control_snapshot_.current = control_params_;
    for (auto& p : param_buffers_) p = control_snapshot_;
    pending_snapshot_ = control_snapshot_;
    pending_params_ = control_params_;
}

bool CrossfeedDSP::valid_sample_rate(float rate) noexcept {
    return std::isfinite(rate) && rate >= MIN_SAMPLE_RATE && rate <= MAX_SAMPLE_RATE;
}

bool CrossfeedDSP::normalize_params(FilterParams& p) {
    if (!valid_sample_rate(p.sample_rate) || !std::isfinite(p.level_db) ||
        !std::isfinite(p.freq_hz) || !std::isfinite(p.delay_us) ||
        !std::isfinite(p.phase_apf_hz) || !std::isfinite(p.center_trim_db) ||
        !std::isfinite(p.shadow_hz) || !std::isfinite(p.preamp_db)) return false;
    // Keep distance from the degenerate Nyquist boundary, including at 8 kHz.
    const float ceiling = p.sample_rate * 0.45f;
    p.preamp_db = std::clamp(p.preamp_db, -24.0f, 12.0f);
    p.level_db = std::clamp(p.level_db, MIN_LEVEL_DB, MAX_LEVEL_DB);
    p.freq_hz = std::clamp(p.freq_hz, MIN_FREQ_HZ, std::min(MAX_FREQ_HZ, ceiling));
    p.delay_us = std::clamp(p.delay_us, MIN_DELAY_US, MAX_DELAY_US);
    p.phase_apf_hz = std::clamp(p.phase_apf_hz, MIN_PHASE_APF_HZ, std::min(MAX_PHASE_APF_HZ, ceiling));
    p.center_trim_db = std::clamp(p.center_trim_db, MIN_CENTER_TRIM_DB, MAX_CENTER_TRIM_DB);
    p.shadow_hz = std::clamp(p.shadow_hz, MIN_SHADOW_HZ, std::min(MAX_SHADOW_HZ, ceiling));
    return true;
}

bool CrossfeedDSP::publish_locked(FilterParams p) {
    const uint32_t requested = requested_rate_.load(std::memory_order_acquire);
    if (requested != 0) p.sample_rate = static_cast<float>(requested);
    if (!normalize_params(p)) return false;
    p.generation = control_params_.generation + 1;
    recompute_coeffs(p);
    for (unsigned slot = 0; slot < 2; ++slot) {
        auto& saved = control_snapshot_.slots[slot];
        saved.sample_rate = p.sample_rate;
        normalize_params(saved);
        recompute_coeffs(saved);
    }
    control_params_ = p;
    control_snapshot_.current = p;
    param_buffers_[write_idx_] = control_snapshot_;
    // Acquire recovers a slot released by the reader; release publishes ours.
    write_idx_ = exchange_idx_.exchange(write_idx_ | DIRTY, std::memory_order_acq_rel) & ~DIRTY;
    return true;
}

bool CrossfeedDSP::set_params(float sample_rate, float level_db, float freq_hz, bool enabled) {
    if (!valid_sample_rate(sample_rate)) return false;
    std::lock_guard<std::mutex> lock(write_mutex_);
    auto p = control_params_;
    p.sample_rate = sample_rate;
    p.level_db = level_db;
    p.freq_hz = freq_hz;
    p.enabled = enabled;
    return publish_locked(p);
}

bool CrossfeedDSP::set_all_params(float sample_rate, float level_db, float freq_hz,
                                  float delay_us, float phase_apf_hz, float center_trim_db,
                                  float shadow_hz, bool advanced_effects, bool enabled) {
    if (!valid_sample_rate(sample_rate)) return false;
    std::lock_guard<std::mutex> lock(write_mutex_);
    auto p = control_params_;
    p.sample_rate = sample_rate;
    p.level_db = level_db;
    p.freq_hz = freq_hz;
    p.delay_us = delay_us;
    p.phase_apf_hz = phase_apf_hz;
    p.center_trim_db = center_trim_db;
    p.shadow_hz = shadow_hz;
    p.advanced_effects = advanced_effects;
    p.enabled = enabled;
    return publish_locked(p);
}

bool CrossfeedDSP::set_numeric(float FilterParams::*field, float value) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    auto p = control_params_;
    p.*field = value;
    return publish_locked(p);
}

bool CrossfeedDSP::set_level_db(float value) {
    return set_numeric(&FilterParams::level_db, value);
}

bool CrossfeedDSP::set_freq_hz(float value) {
    return set_numeric(&FilterParams::freq_hz, value);
}

bool CrossfeedDSP::set_delay_us(float value) {
    return set_numeric(&FilterParams::delay_us, value);
}

bool CrossfeedDSP::set_phase_apf_hz(float value) {
    return set_numeric(&FilterParams::phase_apf_hz, value);
}

bool CrossfeedDSP::set_center_trim_db(float value) {
    return set_numeric(&FilterParams::center_trim_db, value);
}

bool CrossfeedDSP::set_shadow_hz(float value) {
    return set_numeric(&FilterParams::shadow_hz, value);
}

void CrossfeedDSP::set_enabled(bool value) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    auto p = control_params_;
    p.enabled = value;
    publish_locked(p);
}

void CrossfeedDSP::set_advanced_effects(bool value) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    auto p = control_params_;
    p.advanced_effects = value;
    publish_locked(p);
}

FilterParams CrossfeedDSP::get_params() const {
    std::lock_guard<std::mutex> lock(write_mutex_);
    return control_params_;
}

float CrossfeedDSP::get_level_db() const { return get_params().level_db; }

float CrossfeedDSP::get_freq_hz() const { return get_params().freq_hz; }

float CrossfeedDSP::get_delay_us() const { return get_params().delay_us; }

float CrossfeedDSP::get_phase_apf_hz() const { return get_params().phase_apf_hz; }

float CrossfeedDSP::get_center_trim_db() const { return get_params().center_trim_db; }

float CrossfeedDSP::get_shadow_hz() const { return get_params().shadow_hz; }

float CrossfeedDSP::get_sample_rate() const { return get_params().sample_rate; }

bool CrossfeedDSP::get_advanced_effects() const { return get_params().advanced_effects; }

bool CrossfeedDSP::is_enabled() const { return get_params().enabled; }

void CrossfeedDSP::request_sample_rate(uint32_t rate) noexcept {
    requested_rate_.store(rate, std::memory_order_release);
}

bool CrossfeedDSP::service_sample_rate() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    const auto rate = requested_rate_.load(std::memory_order_acquire);
    if (rate == 0 || static_cast<float>(rate) == control_params_.sample_rate) return true;
    if (!valid_sample_rate(static_cast<float>(rate))) return false;
    return publish_locked(control_params_);
}

void CrossfeedDSP::reset() {
    reset_requested_.store(true, std::memory_order_release);
}

void CrossfeedDSP::recompute_coeffs(FilterParams& params) {
    params.preamp_linear = static_cast<float>(std::pow(10.0, params.preamp_db / 20.0));
    params.gain2 = std::pow(10.0f, params.level_db / 20.0f);
    params.trim_linear = std::pow(10.0f, params.center_trim_db / 20.0f);
    params.delay_samples = (params.delay_us / 1000000.0f) * params.sample_rate;

    double dir_gain_db = FULL_DIR_GAIN * (params.gain2 / FULL_GAIN2);

    compute_lowshelf(params.dir_coeffs, params.sample_rate, params.freq_hz, 0.7071f, dir_gain_db);
    compute_lowpass(params.cross_coeffs, params.sample_rate, params.freq_hz, 0.5f);
    compute_allpass(params.apf_coeffs, params.sample_rate, params.phase_apf_hz, 0.7071f);
    compute_lowpass(params.shadow_coeffs, params.sample_rate, params.shadow_hz, 0.7071f);
}

void CrossfeedDSP::compute_lowshelf(BiquadCoeffs& out, float sample_rate, float freq, float q, float gain_db) {
    if (!valid_sample_rate(sample_rate) || !std::isfinite(freq) || freq <= 0.0f ||
        freq >= sample_rate * 0.5f || !std::isfinite(q) || q <= 0.0f || !std::isfinite(gain_db)) {
        out = BiquadCoeffs{};
        return;
    }
    double A = std::pow(10.0, static_cast<double>(gain_db) / 40.0);
    double w0 = 2.0f * PI * freq / sample_rate;
    double cos_w = std::cos(w0);
    double sin_w = std::sin(w0);
    double alpha = sin_w / (2.0f * q);
    double sqrtA = std::sqrt(A);

    double a0 = (A + 1.0f) + (A - 1.0f) * cos_w + 2.0f * sqrtA * alpha;
    if (std::abs(a0) < 1e-9f) {
        out = BiquadCoeffs{};
        return;
    }
    double inv_a0 = 1.0f / a0;

    out.b0 = (A * ((A + 1.0f) - (A - 1.0f) * cos_w + 2.0f * sqrtA * alpha)) * inv_a0;
    out.b1 = (2.0f * A * ((A - 1.0f) - (A + 1.0f) * cos_w)) * inv_a0;
    out.b2 = (A * ((A + 1.0f) - (A - 1.0f) * cos_w - 2.0f * sqrtA * alpha)) * inv_a0;
    out.a1 = (-2.0f * ((A - 1.0f) + (A + 1.0f) * cos_w)) * inv_a0;
    out.a2 = ((A + 1.0f) + (A - 1.0f) * cos_w - 2.0f * sqrtA * alpha) * inv_a0;
}

void CrossfeedDSP::compute_lowpass(BiquadCoeffs& out, float sample_rate, float freq, float q) {
    if (!valid_sample_rate(sample_rate) || !std::isfinite(freq) || freq <= 0.0f ||
        freq >= sample_rate * 0.5f || !std::isfinite(q) || q <= 0.0f) {
        out = BiquadCoeffs{};
        return;
    }
    double w0 = 2.0f * PI * freq / sample_rate;
    double cos_w = std::cos(w0);
    double sin_w = std::sin(w0);
    double alpha = sin_w / (2.0f * q);

    double a0 = 1.0f + alpha;
    if (std::abs(a0) < 1e-9f) {
        out = BiquadCoeffs{};
        return;
    }
    double inv_a0 = 1.0f / a0;

    out.b0 = ((1.0f - cos_w) * 0.5f) * inv_a0;
    out.b1 = (1.0f - cos_w) * inv_a0;
    out.b2 = ((1.0f - cos_w) * 0.5f) * inv_a0;
    out.a1 = (-2.0f * cos_w) * inv_a0;
    out.a2 = (1.0f - alpha) * inv_a0;
}

void CrossfeedDSP::compute_allpass(BiquadCoeffs& out, float sample_rate, float freq, float q) {
    if (!valid_sample_rate(sample_rate) || !std::isfinite(freq) || freq <= 0.0f ||
        freq >= sample_rate * 0.5f || !std::isfinite(q) || q <= 0.0f) {
        out = BiquadCoeffs{};
        return;
    }
    double w0 = 2.0f * PI * freq / sample_rate;
    double cos_w = std::cos(w0);
    double sin_w = std::sin(w0);
    double alpha = sin_w / (2.0f * q);

    double a0 = 1.0f + alpha;
    if (std::abs(a0) < 1e-9f) {
        out = BiquadCoeffs{};
        return;
    }
    double inv_a0 = 1.0f / a0;

    out.b0 = (1.0f - alpha) * inv_a0;
    out.b1 = (-2.0f * cos_w) * inv_a0;
    out.b2 = 1.0f;
    out.a1 = (-2.0f * cos_w) * inv_a0;
    out.a2 = (1.0f - alpha) * inv_a0;
}


void CrossfeedDSP::Engine::reset(const FilterParams& p) noexcept {
    dir_l.reset(); dir_r.reset(); cross_l.reset(); cross_r.reset();
    apf_l.reset(); apf_r.reset(); shadow_l.reset(); shadow_r.reset();
    delay_l.reset(); delay_r.reset();
    params = p;
    delay_samples = p.delay_samples;
    delay_step = 0.0f;
    delay_remaining = 0;
}

void CrossfeedDSP::Engine::retarget(const FilterParams& p) noexcept {
    params = p;
    delay_remaining = std::max(1u, static_cast<unsigned>(p.sample_rate * TRANSITION_SECONDS));
    delay_step = (p.delay_samples - delay_samples) / static_cast<float>(delay_remaining);
}

void CrossfeedDSP::Engine::process(float l, float r, float& out_l, float& out_r) noexcept {
    if (delay_remaining > 0) {
        delay_samples += delay_step;
        if (--delay_remaining == 0) delay_samples = params.delay_samples;
    }
    const float dl = dir_l.process(l, params.dir_coeffs);
    const float dr = dir_r.process(r, params.dir_coeffs);
    const float cl = cross_l.process(l, params.cross_coeffs);
    const float cr = cross_r.process(r, params.cross_coeffs);
    // Keep histories warm even in pure/bypass mode, so enabling a stage never
    // replays stale samples from the last time it was used.
    delay_l.write(apf_l.process(shadow_l.process(cl, params.shadow_coeffs), params.apf_coeffs));
    delay_r.write(apf_r.process(shadow_r.process(cr, params.shadow_coeffs), params.apf_coeffs));
    if (!params.enabled) {
        // Bypass follows the processed path's active output trim. Pure mode
        // has no trim on either path. Shared preamp is applied by the caller.
        const float trim = params.advanced_effects ? params.trim_linear : 1.0f;
        out_l = l * trim; out_r = r * trim;
    } else if (params.advanced_effects) {
        out_l = (dl + params.gain2 * delay_r.read(delay_samples)) * params.trim_linear;
        out_r = (dr + params.gain2 * delay_l.read(delay_samples)) * params.trim_linear;
    } else {
        out_l = dl + params.gain2 * cr;
        out_r = dr + params.gain2 * cl;
    }
}

bool CrossfeedDSP::begin_block() noexcept {
    if (exchange_idx_.load(std::memory_order_acquire) & DIRTY) {
        read_idx_ = exchange_idx_.exchange(read_idx_, std::memory_order_acq_rel) & ~DIRTY;
        pending_snapshot_ = param_buffers_[read_idx_];
        pending_params_ = pending_snapshot_.current;
    }
    if (meter_revision_ != pending_snapshot_.meter_reset) {
        meters_ = ListeningStatus{};
        meter_revision_ = pending_snapshot_.meter_reset;
    }
    const bool reset = reset_requested_.exchange(false, std::memory_order_acquire);
    const uint32_t rate = requested_rate_.load(std::memory_order_acquire);
    if (rate != 0 && pending_params_.sample_rate != static_cast<float>(rate)) {
        // Do not use coefficients or delay histories belonging to another rate.
        awaiting_rate_ = true;
        started_ = false;
        fade_remaining_ = 0;
        return false;
    }
    auto& current = engines_[active_engine_];
    if (!started_ || reset || current.params.sample_rate != pending_params_.sample_rate) {
        current.reset(pending_params_);
        started_ = true;
        fade_remaining_ = 0;
        if (awaiting_rate_ && !reset) {
            engines_[1 - active_engine_] = current;
            current.params.enabled = false;
            // Recovery fades from the actual unscaled rate-fallback output.
            current.params.advanced_effects = false;
            current.params.preamp_linear = 1.0f;
            fade_length_ = std::max(1u, static_cast<unsigned>(pending_params_.sample_rate * TRANSITION_SECONDS));
            fade_remaining_ = fade_length_;
        }
        awaiting_rate_ = false;
    } else if (fade_remaining_ == 0 && current.params.generation != pending_params_.generation) {
        auto& next = engines_[1 - active_engine_];
        next = current;
        next.retarget(pending_params_);
        fade_length_ = std::max(1u, static_cast<unsigned>(pending_params_.sample_rate * TRANSITION_SECONDS));
        fade_remaining_ = fade_length_;
    }
    return true;
}

void CrossfeedDSP::process_sample(float l, float r, float& out_l, float& out_r) noexcept {
    engines_[active_engine_].process(l, r, out_l, out_r);
    const float gain = engines_[active_engine_].params.preamp_linear;
    out_l *= gain; out_r *= gain;
    if (fade_remaining_ > 0) {
        float next_l, next_r;
        engines_[1 - active_engine_].process(l, r, next_l, next_r);
        const unsigned next_engine = 1 - active_engine_;
        const float next_gain = engines_[next_engine].params.preamp_linear;
        next_l *= next_gain; next_r *= next_gain;
        const float mix = 1.0f - static_cast<float>(fade_remaining_ - 1) / static_cast<float>(fade_length_);
        out_l += mix * (next_l - out_l);
        out_r += mix * (next_r - out_r);
        if (--fade_remaining_ == 0) active_engine_ = 1 - active_engine_;
    }
}

void CrossfeedDSP::process_interleaved(const float* in, float* out, size_t frames) noexcept {
    if (frames == 0) return;
    ScopedDenormals denormals;
    block_input_peak_ = block_output_peak_ = 0;
    const bool ready = begin_block();
    for (size_t i = 0; i < frames; ++i) {
        const float l = in[2 * i], r = in[2 * i + 1];
        if (ready) process_sample(l, r, out[2 * i], out[2 * i + 1]);
        else { out[2 * i] = l; out[2 * i + 1] = r; }
        meter_sample(l, r, out[2 * i], out[2 * i + 1]);
    }
    publish_meters();
}

void CrossfeedDSP::process_planar(const float* in_l, const float* in_r, float* out_l, float* out_r, size_t frames) noexcept {
    if (frames == 0) return;
    ScopedDenormals denormals;
    block_input_peak_ = block_output_peak_ = 0;
    const bool ready = begin_block();
    for (size_t i = 0; i < frames; ++i) {
        const float l = in_l[i], r = in_r[i];
        if (ready) process_sample(l, r, out_l[i], out_r[i]);
        else { out_l[i] = l; out_r[i] = r; }
        meter_sample(l, r, out_l[i], out_r[i]);
    }
    publish_meters();
}

bool CrossfeedDSP::same_sound(const FilterParams& a, const FilterParams& b) noexcept {
    return a.level_db == b.level_db && a.freq_hz == b.freq_hz && a.delay_us == b.delay_us &&
        a.phase_apf_hz == b.phase_apf_hz && a.center_trim_db == b.center_trim_db &&
        a.shadow_hz == b.shadow_hz && a.advanced_effects == b.advanced_effects;
}

bool CrossfeedDSP::set_snapshot(FilterParams params) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    return publish_locked(params);
}

bool CrossfeedDSP::set_preamp_db(float value) { return set_numeric(&FilterParams::preamp_db, value); }

void CrossfeedDSP::reset_meters() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    ++control_snapshot_.meter_reset;
    publish_locked(control_params_);
}

bool CrossfeedDSP::store_slot(unsigned slot) {
    if (slot > 1) return false;
    std::lock_guard<std::mutex> lock(write_mutex_);
    control_snapshot_.slots[slot] = control_params_;
    control_snapshot_.valid[slot] = true;
    return publish_locked(control_params_);
}

bool CrossfeedDSP::recall_slot(unsigned slot) {
    if (slot > 1) return false;
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!control_snapshot_.valid[slot]) return false;
    auto p = control_snapshot_.slots[slot];
    p.enabled = true;
    p.preamp_db = control_params_.preamp_db; // Shared listening level, not part of an A/B candidate.
    p.sample_rate = control_params_.sample_rate;
    return publish_locked(p);
}

bool CrossfeedDSP::get_slot(unsigned slot, FilterParams& params) const {
    if (slot > 1) return false;
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!control_snapshot_.valid[slot]) return false;
    params = control_snapshot_.slots[slot];
    return true;
}

ListeningStatus CrossfeedDSP::get_listening_status() const {
    std::lock_guard<std::mutex> lock(meter_read_mutex_);
    if (meter_exchange_.load(std::memory_order_acquire) & DIRTY)
        meter_read_ = meter_exchange_.exchange(meter_read_, std::memory_order_acq_rel) & ~DIRTY;
    return meter_buffers_[meter_read_];
}

void CrossfeedDSP::meter_sample(float l, float r, float out_l, float out_r) noexcept {
    const float input = std::max(std::abs(l), std::abs(r));
    const float output = std::max(std::abs(out_l), std::abs(out_r));
    if (std::isfinite(input)) block_input_peak_ = std::max(block_input_peak_, input);
    if (std::isfinite(output)) block_output_peak_ = std::max(block_output_peak_, output);
    meters_.input_clip |= input >= 1 || !std::isfinite(l) || !std::isfinite(r);
    meters_.output_clip |= output >= 1 || !std::isfinite(out_l) || !std::isfinite(out_r);
}

void CrossfeedDSP::publish_meters() noexcept {
    meters_.input_peak = block_input_peak_;
    meters_.output_peak = block_output_peak_;
    meters_.input_hold = std::max(meters_.input_hold, block_input_peak_);
    meters_.output_hold = std::max(meters_.output_hold, block_output_peak_);
    meter_buffers_[meter_write_] = meters_;
    meter_write_ = meter_exchange_.exchange(meter_write_ | DIRTY, std::memory_order_acq_rel) & ~DIRTY;
}

} // namespace crossfeed
