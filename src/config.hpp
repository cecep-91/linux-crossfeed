#pragma once

#include <string>
#include <cstdint>

namespace crossfeed {

struct ConfigState {
    bool enabled = true;
    float preamp_db = 0.0f;
    float level_db = -10.0f;
    float freq_hz = 700.0f;
    float delay_us = 280.0f;
    float phase_apf_hz = 1500.0f;
    float center_trim_db = -1.5f;
    float shadow_hz = 3000.0f;
    bool advanced_effects = true;
    std::string backend = "auto";
    std::string target_sink = "auto";
    uint32_t sample_rate = 48000;
    uint32_t buffer_frames = 256;
};

class Config {
public:
    static std::string get_config_dir();
    static std::string get_state_file_path();
    static std::string get_socket_path();

    static bool load_state(ConfigState& state);
    static bool save_state(const ConfigState& state);
};

} // namespace crossfeed
