#pragma once

#include "audio_backend.hpp"
#include <alsa/asoundlib.h>
#include <atomic>
#include <string>

namespace crossfeed {

class AlsaBackend : public AudioBackend {
public:
    AlsaBackend();
    ~AlsaBackend() override;

    bool init(CrossfeedDSP* dsp, const std::string& target_sink, uint32_t sample_rate, uint32_t buffer_frames) override;
    bool run() override;
    void stop() override;

    std::string get_backend_name() const override { return "ALSA Direct PCM"; }
    std::string get_active_target() const override { return out_device_; }
    uint32_t get_sample_rate() const override { return sample_rate_; }
    uint32_t get_buffer_frames() const override { return buffer_frames_; }
    bool is_running() const override { return running_.load(); }

    std::vector<SinkDevice> list_sinks() override;

private:
    bool set_hw_params(snd_pcm_t* pcm);
    void cleanup();

    CrossfeedDSP* dsp_ = nullptr;
    std::string in_device_ = "default";
    std::string out_device_ = "default";
    uint32_t sample_rate_ = 48000;
    uint32_t buffer_frames_ = 256;

    snd_pcm_t* capture_handle_ = nullptr;
    snd_pcm_t* playback_handle_ = nullptr;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
};

} // namespace crossfeed
