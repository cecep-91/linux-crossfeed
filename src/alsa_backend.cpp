#include "alsa_backend.hpp"
#include <iostream>
#include <vector>

namespace crossfeed {

AlsaBackend::AlsaBackend() = default;

AlsaBackend::~AlsaBackend() {
    cleanup();
}

std::vector<SinkDevice> AlsaBackend::list_sinks() {
    std::vector<SinkDevice> list;
    void** hints;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) {
        return list;
    }

    for (void** n = hints; *n != nullptr; ++n) {
        char* name = snd_device_name_get_hint(*n, "NAME");
        char* desc = snd_device_name_get_hint(*n, "DESC");
        char* ioid = snd_device_name_get_hint(*n, "IOID");

        if (name && (!ioid || std::string(ioid) == "Output")) {
            SinkDevice s;
            s.name = name;
            s.description = desc ? desc : name;
            list.push_back(s);
        }
        if (name) free(name);
        if (desc) free(desc);
        if (ioid) free(ioid);
    }
    snd_device_name_free_hint(hints);
    return list;
}

bool AlsaBackend::set_hw_params(snd_pcm_t* pcm) {
    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);

    if (snd_pcm_hw_params_any(pcm, params) < 0) return false;
    if (snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) return false;
    if (snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_FLOAT_LE) < 0) return false;
    if (snd_pcm_hw_params_set_channels(pcm, params, 2) < 0) return false;

    unsigned int rate = sample_rate_;
    if (snd_pcm_hw_params_set_rate_near(pcm, params, &rate, nullptr) < 0) return false;
    sample_rate_ = rate;

    snd_pcm_uframes_t period_size = buffer_frames_;
    if (snd_pcm_hw_params_set_period_size_near(pcm, params, &period_size, nullptr) < 0) return false;
    buffer_frames_ = static_cast<uint32_t>(period_size);

    snd_pcm_uframes_t buf_size = period_size * 4;
    if (snd_pcm_hw_params_set_buffer_size_near(pcm, params, &buf_size) < 0) return false;

    if (snd_pcm_hw_params(pcm, params) < 0) return false;
    return true;
}

bool AlsaBackend::init(CrossfeedDSP* dsp, const std::string& target_sink, uint32_t sample_rate, uint32_t buffer_frames) {
    dsp_ = dsp;
    sample_rate_ = sample_rate;
    buffer_frames_ = buffer_frames;

    if (!target_sink.empty() && target_sink != "auto") {
        out_device_ = target_sink;
    } else {
        out_device_ = "default";
    }

    in_device_ = "default";

    if (snd_pcm_open(&capture_handle_, in_device_.c_str(), SND_PCM_STREAM_CAPTURE, 0) < 0) {
        std::cerr << "[crossfeed] ALSA: Failed to open capture device " << in_device_ << std::endl;
        return false;
    }

    if (!set_hw_params(capture_handle_)) {
        std::cerr << "[crossfeed] ALSA: Failed to set capture hw params." << std::endl;
        snd_pcm_close(capture_handle_);
        capture_handle_ = nullptr;
        return false;
    }

    if (snd_pcm_open(&playback_handle_, out_device_.c_str(), SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        std::cerr << "[crossfeed] ALSA: Failed to open playback device " << out_device_ << std::endl;
        snd_pcm_close(capture_handle_);
        capture_handle_ = nullptr;
        return false;
    }

    if (!set_hw_params(playback_handle_)) {
        std::cerr << "[crossfeed] ALSA: Failed to set playback hw params." << std::endl;
        snd_pcm_close(playback_handle_);
        snd_pcm_close(capture_handle_);
        playback_handle_ = nullptr;
        capture_handle_ = nullptr;
        return false;
    }

    dsp_->set_params(static_cast<float>(sample_rate_), dsp_->get_level_db(), dsp_->get_freq_hz(), dsp_->is_enabled());
    return true;
}

bool AlsaBackend::run() {
    if (!capture_handle_ || !playback_handle_ || !dsp_) return false;

    running_.store(true);
    std::cout << "[crossfeed] ALSA audio loop running (" << sample_rate_ << " Hz, " << buffer_frames_ << " frames)..." << std::endl;

    std::vector<float> in_buf(buffer_frames_ * 2);
    std::vector<float> out_buf(buffer_frames_ * 2);

    while (!stop_requested_.load()) {
        snd_pcm_sframes_t r = snd_pcm_readi(capture_handle_, in_buf.data(), buffer_frames_);
        if (r < 0) {
            r = snd_pcm_recover(capture_handle_, static_cast<int>(r), 0);
            if (r < 0) {
                if (running_.load()) std::cerr << "[crossfeed] ALSA read error." << std::endl;
                break;
            }
            continue;
        }

        dsp_->process_interleaved(in_buf.data(), out_buf.data(), static_cast<size_t>(r));

        snd_pcm_sframes_t w = snd_pcm_writei(playback_handle_, out_buf.data(), static_cast<size_t>(r));
        if (w < 0) {
            w = snd_pcm_recover(playback_handle_, static_cast<int>(w), 0);
            if (w < 0) {
                if (running_.load()) std::cerr << "[crossfeed] ALSA write error." << std::endl;
                break;
            }
        }
    }

    running_.store(false);
    cleanup();
    return true;
}

void AlsaBackend::stop() {
    stop_requested_.store(true);
}

void AlsaBackend::cleanup() {
    running_.store(false);
    if (playback_handle_) {
        snd_pcm_drop(playback_handle_);
        snd_pcm_close(playback_handle_);
        playback_handle_ = nullptr;
    }
    if (capture_handle_) {
        snd_pcm_close(capture_handle_);
        capture_handle_ = nullptr;
    }
}

} // namespace crossfeed
