#pragma once

#include <cstddef>
#include <cmath>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cstdint>

namespace crossfeed {

constexpr float DEFAULT_LEVEL_DB = -10.0f;
constexpr float DEFAULT_FREQ_HZ = 700.0f;
constexpr float MIN_LEVEL_DB = -30.0f;
constexpr float MAX_LEVEL_DB = -3.0f;
constexpr float MIN_FREQ_HZ = 200.0f;
constexpr float MAX_FREQ_HZ = 2000.0f;
constexpr float FULL_GAIN2 = 0.316f;
constexpr float FULL_DIR_GAIN = -1.5f;
constexpr float MIN_SAMPLE_RATE = 8000.0f;
constexpr float MAX_SAMPLE_RATE = 192000.0f;
constexpr float TRANSITION_SECONDS = 0.020f;

// Extended / Advanced Crossfeed Constants
constexpr float DEFAULT_DELAY_US = 280.0f;
constexpr float MIN_DELAY_US = 0.0f;
constexpr float MAX_DELAY_US = 800.0f;

constexpr float DEFAULT_PHASE_APF_HZ = 1500.0f;
constexpr float MIN_PHASE_APF_HZ = 200.0f;
constexpr float MAX_PHASE_APF_HZ = 4000.0f;

constexpr float DEFAULT_CENTER_TRIM_DB = -1.5f;
constexpr float MIN_CENTER_TRIM_DB = -6.0f;
constexpr float MAX_CENTER_TRIM_DB = 0.0f;

constexpr float DEFAULT_SHADOW_HZ = 3000.0f;
constexpr float MIN_SHADOW_HZ = 1000.0f;
constexpr float MAX_SHADOW_HZ = 8000.0f;

struct BiquadCoeffs {
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
};

class Biquad {
public:
    inline float process(float in, const BiquadCoeffs& c) noexcept {
        float out = c.b0 * in + s1_;
        s1_ = c.b1 * in - c.a1 * out + s2_;
        s2_ = c.b2 * in - c.a2 * out;
        return out;
    }

    void reset() noexcept {
        s1_ = 0.0f;
        s2_ = 0.0f;
    }

private:
    float s1_ = 0.0f;
    float s2_ = 0.0f;
};

class DelayLine {
public:
    static constexpr size_t BUFFER_SIZE = 2048; // Power of two for fast mask wrap

    void reset() noexcept {
        std::fill(buffer_, buffer_ + BUFFER_SIZE, 0.0f);
        write_idx_ = 0;
    }

    inline void write(float sample) noexcept {
        buffer_[write_idx_] = sample;
        write_idx_ = (write_idx_ + 1) & (BUFFER_SIZE - 1);
    }

    inline float read(float delay_samples) const noexcept {
        if (!std::isfinite(delay_samples)) return 0.0f;
        if (delay_samples <= 0.001f) {
            size_t idx = (write_idx_ - 1) & (BUFFER_SIZE - 1);
            return buffer_[idx];
        }

        if (delay_samples > static_cast<float>(BUFFER_SIZE - 2)) {
            delay_samples = static_cast<float>(BUFFER_SIZE - 2);
        }

        int int_delay = static_cast<int>(delay_samples);
        float frac = delay_samples - static_cast<float>(int_delay);

        size_t idx1 = (write_idx_ - 1 - int_delay) & (BUFFER_SIZE - 1);
        size_t idx2 = (idx1 - 1) & (BUFFER_SIZE - 1);

        return (1.0f - frac) * buffer_[idx1] + frac * buffer_[idx2];
    }

private:
    float buffer_[BUFFER_SIZE] = {0.0f};
    size_t write_idx_ = 0;
};

struct FilterParams {
    uint64_t generation = 0;
    float sample_rate = 48000.0f;
    float level_db = DEFAULT_LEVEL_DB;
    float freq_hz = DEFAULT_FREQ_HZ;
    float delay_us = DEFAULT_DELAY_US;
    float phase_apf_hz = DEFAULT_PHASE_APF_HZ;
    float center_trim_db = DEFAULT_CENTER_TRIM_DB;
    float shadow_hz = DEFAULT_SHADOW_HZ;
    bool advanced_effects = true;
    bool enabled = true;
    float preamp_db = 0.0f;
    float preamp_linear = 1.0f;

    BiquadCoeffs dir_coeffs;
    BiquadCoeffs cross_coeffs;
    BiquadCoeffs apf_coeffs;
    BiquadCoeffs shadow_coeffs;

    float gain2 = FULL_GAIN2;
    float trim_linear = 0.8414f;
    float delay_samples = 13.44f;
};

struct ListeningStatus {
    float input_peak = 0, output_peak = 0;
    float input_hold = 0, output_hold = 0;
    bool input_clip = false, output_clip = false;
};

class CrossfeedDSP {
public:
    CrossfeedDSP();

    bool set_params(float sample_rate, float level_db, float freq_hz, bool enabled);
    bool set_all_params(float sample_rate, float level_db, float freq_hz,
                        float delay_us, float phase_apf_hz, float center_trim_db,
                        float shadow_hz, bool advanced_effects, bool enabled);

    bool set_snapshot(FilterParams params);
    bool set_preamp_db(float value);
    void reset_meters();
    bool store_slot(unsigned slot);
    bool recall_slot(unsigned slot);
    bool get_slot(unsigned slot, FilterParams& params) const;
    ListeningStatus get_listening_status() const;
    static bool same_sound(const FilterParams& a, const FilterParams& b) noexcept;

    bool set_level_db(float level_db);
    bool set_freq_hz(float freq_hz);
    bool set_delay_us(float delay_us);
    bool set_phase_apf_hz(float phase_apf_hz);
    bool set_center_trim_db(float center_trim_db);
    bool set_shadow_hz(float shadow_hz);
    void set_advanced_effects(bool enabled);
    void set_enabled(bool enabled);

    bool is_enabled() const;
    bool get_advanced_effects() const;
    float get_level_db() const;
    float get_freq_hz() const;
    float get_delay_us() const;
    float get_phase_apf_hz() const;
    float get_center_trim_db() const;
    float get_shadow_hz() const;
    float get_sample_rate() const;
    FilterParams get_params() const; // Non-RT, coherent control snapshot.

    // The callback only posts a rate. A non-RT timer services it and prepares
    // coefficients. Until a matching snapshot arrives, audio passes through.
    void request_sample_rate(uint32_t rate) noexcept;
    bool service_sample_rate();
    static bool valid_sample_rate(float rate) noexcept;

    void reset();

    // In-place or separate buffer processing of interleaved 2-channel 32-bit float audio
    void process_interleaved(const float* in, float* out, size_t frames) noexcept;

    // Zero-allocation planar (separate L and R channel buffers) processing for native PipeWire graph
    void process_planar(const float* in_l, const float* in_r, float* out_l, float* out_r, size_t frames) noexcept;

    static void compute_lowshelf(BiquadCoeffs& out, float sample_rate, float freq, float q, float gain_db);
    static void compute_lowpass(BiquadCoeffs& out, float sample_rate, float freq, float q);
    static void compute_allpass(BiquadCoeffs& out, float sample_rate, float freq, float q);

private:
    static bool normalize_params(FilterParams& params);
    static void recompute_coeffs(FilterParams& params);
    bool publish_locked(FilterParams params);
    bool set_numeric(float FilterParams::*field, float value);
    bool begin_block() noexcept;
    void process_sample(float l, float r, float& out_l, float& out_r) noexcept;

    // Three owners: writer, atomic exchange slot, reader. Writers serialize;
    // exactly one audio thread may call either process method at a time.
    static constexpr unsigned DIRTY = 4;
    struct ControlSnapshot {
        FilterParams current, slots[2];
        bool valid[2] = {false, false};
        uint64_t meter_reset = 0;
    };
    ControlSnapshot param_buffers_[3], control_snapshot_, pending_snapshot_;

    FilterParams control_params_;
    unsigned write_idx_ = 0; // Protected by write_mutex_.
    unsigned read_idx_ = 1;  // Audio thread only.
    std::atomic<unsigned> exchange_idx_{2};
    mutable std::mutex write_mutex_;
    std::atomic<bool> reset_requested_{false};
    std::atomic<uint32_t> requested_rate_{0};
    static_assert(std::atomic<unsigned>::is_always_lock_free, "RT indices must be lock-free");
    static_assert(std::atomic<uint32_t>::is_always_lock_free, "RT rates must be lock-free");
    static_assert(std::atomic<bool>::is_always_lock_free, "RT reset flag must be lock-free");

    struct Engine {
        FilterParams params;
        Biquad dir_l, dir_r, cross_l, cross_r, apf_l, apf_r, shadow_l, shadow_r;
        DelayLine delay_l, delay_r;
        float delay_samples = 0.0f;
        float delay_step = 0.0f;
        unsigned delay_remaining = 0;
        void reset(const FilterParams& p) noexcept;
        void retarget(const FilterParams& p) noexcept;
        void process(float l, float r, float& out_l, float& out_r) noexcept;
    };
    Engine engines_[2];
    FilterParams pending_params_;
    unsigned active_engine_ = 0;
    unsigned fade_remaining_ = 0;
    unsigned fade_length_ = 1;
    bool started_ = false;
    bool awaiting_rate_ = false;

    uint64_t meter_revision_ = 0;
    ListeningStatus meters_;
    float block_input_peak_ = 0, block_output_peak_ = 0;
    void meter_sample(float l, float r, float out_l, float out_r) noexcept;
    void publish_meters() noexcept;
    // Inverse triple buffer: RT writer, serialized non-RT telemetry readers.
    mutable ListeningStatus meter_buffers_[3];
    unsigned meter_write_ = 0;
    mutable unsigned meter_read_ = 1;
    mutable std::atomic<unsigned> meter_exchange_{2};
    mutable std::mutex meter_read_mutex_;

};

} // namespace crossfeed
