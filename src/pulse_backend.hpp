#pragma once

#include "audio_backend.hpp"
#include <pulse/simple.h>
#include <pulse/pulseaudio.h>
#include <atomic>
#include <string>
#include <vector>

namespace crossfeed {

class PulseBackend : public AudioBackend {
public:
    PulseBackend();
    ~PulseBackend() override;

    bool init(CrossfeedDSP* dsp, const std::string& target_sink, uint32_t sample_rate, uint32_t buffer_frames) override;
    bool run() override;
    void stop() override;

    std::string get_backend_name() const override { return "PulseAudio/PipeWire-Pulse"; }
    std::string get_active_target() const override { return active_target_; }
    uint32_t get_sample_rate() const override { return sample_rate_; }
    uint32_t get_buffer_frames() const override { return buffer_frames_; }
    bool is_running() const override { return running_.load(); }

    std::vector<SinkDevice> list_sinks() override;

private:
    void cleanup();
    bool query_sinks(std::string& default_sink, std::vector<SinkDevice>& sinks);
    bool load_null_sink();
    void unload_null_sink();
    std::string resolve_target_sink(const std::string& requested, const std::string& default_sink, const std::vector<SinkDevice>& sinks);

    CrossfeedDSP* dsp_ = nullptr;
    std::string requested_target_;
    std::string active_target_;
    uint32_t sample_rate_ = 48000;
    uint32_t buffer_frames_ = 256;

    uint32_t null_sink_module_index_ = PA_INVALID_INDEX;
    bool own_module_ = false;
    std::string original_default_sink_;

    pa_simple* rec_stream_ = nullptr;
    pa_simple* play_stream_ = nullptr;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
};

} // namespace crossfeed
