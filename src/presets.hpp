#pragma once

#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <cctype>

namespace crossfeed {

struct CrossfeedPreset {
    const char* id;
    const char* name;
    const char* description;
    float level_db;
    float freq_hz;
    float delay_us;
    float phase_apf_hz;
    float center_trim_db;
    float shadow_hz;
};

inline const CrossfeedPreset g_presets[] = {
    {"meier", "Jan Meier", "Jan Meier (Corda) — natural presentation for fatigue-free listening", -9.5f, 650.0f, 280.0f, 1500.0f, -1.5f, 3200.0f},
    {"chumoy", "Chu Moy", "Chu Moy (HeadWize) — classic analog RC circuit crossfeed emulation", -6.0f, 700.0f, 260.0f, 2000.0f, -2.0f, 2800.0f},
    {"bs2b", "Bauer BS2B", "Bauer BS2B — stereophonic-to-binaural high blend simulation", -4.5f, 700.0f, 350.0f, 1200.0f, -2.5f, 2500.0f},
    {"linkwitz", "Linkwitz", "Siegfried Linkwitz — loudspeaker simulation for wide spatial staging", -7.0f, 1200.0f, 220.0f, 1600.0f, -1.8f, 4000.0f},
    {"studio", "Studio 30°", "Natural Studio — simulates near-field stereo monitors at 30° triangle", -8.0f, 850.0f, 250.0f, 1800.0f, -1.5f, 3500.0f},
    {"jm1", "JM-1 Diffuse", "JM-1 Diffuse — optimized for B&K 5128 diffuse-field / pinna-bypassing IEMs", -10.0f, 750.0f, 260.0f, 1600.0f, -1.2f, 3600.0f},
};

inline const CrossfeedPreset* find_preset(const std::string& query) {
    auto normalize = [](const std::string& str) -> std::string {
        std::string res;
        for (char c : str) {
            if (std::isalnum(static_cast<unsigned char>(c))) {
                res += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
        }
        return res;
    };

    std::string q = normalize(query);
    if (q.empty()) return nullptr;

    for (const auto& p : g_presets) {
        std::string pid = normalize(p.id);
        std::string pname = normalize(p.name);

        if (q == pid || pname.find(q) != std::string::npos || q.find(pid) != std::string::npos) {
            return &p;
        }
    }
    return nullptr;
}

inline const CrossfeedPreset* detect_active_preset(float level, float freq, float delay, float phase, float trim, float shadow, bool advanced) {
    if (!advanced) return nullptr;
    for (const auto& p : g_presets) {
        if (std::abs(p.level_db - level) < 0.2f &&
            std::abs(p.freq_hz - freq) < 6.0f &&
            std::abs(p.delay_us - delay) < 6.0f &&
            std::abs(p.phase_apf_hz - phase) < 20.0f &&
            std::abs(p.center_trim_db - trim) < 0.2f &&
            std::abs(p.shadow_hz - shadow) < 30.0f) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace crossfeed
