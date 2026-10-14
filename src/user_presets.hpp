#pragma once
#include "config.hpp"
#include "dsp.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace crossfeed {
struct UserPreset { std::string name; FilterParams params; };

// Portable, bounded text format. Names are quoted; numeric fields use the C locale.
// Import parses into a temporary collection so malformed files never partially apply.
class UserPresets {
public:
    static std::string path() { return Config::get_config_dir() + "/presets.crossfeed"; }
    static bool valid_name(const std::string& name) {
        if (name.empty() || name.size() > 80 || name.find_first_not_of(' ') == std::string::npos) return false;
        for (unsigned char c : name) if (c < 32 || c == 127) return false;
        return true;
    }
    static bool valid_params(const FilterParams& p) {
        return std::isfinite(p.level_db) && p.level_db >= MIN_LEVEL_DB && p.level_db <= MAX_LEVEL_DB &&
            std::isfinite(p.freq_hz) && p.freq_hz >= MIN_FREQ_HZ && p.freq_hz <= MAX_FREQ_HZ &&
            std::isfinite(p.delay_us) && p.delay_us >= MIN_DELAY_US && p.delay_us <= MAX_DELAY_US &&
            std::isfinite(p.phase_apf_hz) && p.phase_apf_hz >= MIN_PHASE_APF_HZ && p.phase_apf_hz <= MAX_PHASE_APF_HZ &&
            std::isfinite(p.center_trim_db) && p.center_trim_db >= MIN_CENTER_TRIM_DB && p.center_trim_db <= MAX_CENTER_TRIM_DB &&
            std::isfinite(p.shadow_hz) && p.shadow_hz >= MIN_SHADOW_HZ && p.shadow_hz <= MAX_SHADOW_HZ &&
            std::isfinite(p.preamp_db) && p.preamp_db >= -24 && p.preamp_db <= 12;
    }
    static std::string encode(const std::vector<UserPreset>& presets) {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << "CROSSFEED_PRESETS 1\n" << std::setprecision(9);
        for (const auto& entry : presets) {
            const auto& p = entry.params;
            out << std::quoted(entry.name) << ' ' << p.level_db << ' ' << p.freq_hz << ' '
                << p.delay_us << ' ' << p.phase_apf_hz << ' ' << p.center_trim_db << ' '
                << p.shadow_hz << ' ' << int(p.advanced_effects) << ' ' << p.preamp_db << '\n';
        }
        return out.str();
    }
    static bool decode(const std::string& text, std::vector<UserPreset>& output, std::string& error) {
        if (text.size() > 65536) { error = "Preset file exceeds 64 KiB."; return false; }
        std::istringstream input(text); input.imbue(std::locale::classic());
        std::string line;
        if (!std::getline(input, line) || line != "CROSSFEED_PRESETS 1") {
            error = "Unsupported preset format. Expected CROSSFEED_PRESETS 1."; return false;
        }
        std::vector<UserPreset> parsed;
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            UserPreset entry; int advanced = 0;
            std::istringstream row(line); row.imbue(std::locale::classic());
            auto& p = entry.params;
            if (!(row >> std::quoted(entry.name) >> p.level_db >> p.freq_hz >> p.delay_us >>
                  p.phase_apf_hz >> p.center_trim_db >> p.shadow_hz >> advanced >> p.preamp_db) ||
                !valid_name(entry.name) || !valid_params(p) || (advanced != 0 && advanced != 1)) {
                error = "Invalid preset name or parameter values."; return false;
            }
            row >> std::ws;
            if (!row.eof()) { error = "Unexpected trailing preset data."; return false; }
            for (const auto& previous : parsed) if (previous.name == entry.name) {
                error = "Duplicate preset name in file."; return false;
            }
            p.advanced_effects = advanced != 0;
            parsed.push_back(entry);
            if (parsed.size() > 128) { error = "At most 128 presets are supported."; return false; }
        }
        output = std::move(parsed);
        return true;
    }
    static bool load(const std::string& filename, std::vector<UserPreset>& output, std::string& error) {
        std::ifstream file(filename, std::ios::binary);
        if (!file) { error = "Could not open preset file."; return false; }
        std::string text(65537, '\0');
        file.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<size_t>(file.gcount()));
        return decode(text, output, error);
    }
    static bool save(const std::string& filename, const std::vector<UserPreset>& presets, std::string& error) {
        std::vector<UserPreset> checked;
        const auto text = encode(presets);
        if (!decode(text, checked, error)) return false;
        std::error_code ec;
        const auto parent = std::filesystem::path(filename).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        if (ec) { error = "Could not create preset directory."; return false; }
        std::string temporary = filename + ".tmp-XXXXXX";
        int fd = mkstemp(temporary.data());
        if (fd < 0) { error = "Could not create preset file."; return false; }
        size_t offset = 0;
        while (offset < text.size()) {
            const ssize_t n = write(fd, text.data() + offset, text.size() - offset);
            if (n <= 0) break;
            offset += static_cast<size_t>(n);
        }
        const bool complete = offset == text.size() && fsync(fd) == 0;
        const bool closed = close(fd) == 0;
        const bool ok = complete && closed && rename(temporary.c_str(), filename.c_str()) == 0;
        if (!ok) { unlink(temporary.c_str()); error = "Could not save presets atomically."; }
        return ok;
    }
};
} // namespace crossfeed
