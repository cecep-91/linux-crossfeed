#include "pipewire_backend.hpp"
#include <spa/param/audio/format-utils.h>
#include <iostream>
#include <sstream>
#include <cstring>
#include <chrono>
#include <unistd.h>
#include <map>
#include <cctype>

#ifndef PW_KEY_PORT_PASSIVE
#define PW_KEY_PORT_PASSIVE "port.passive"
#endif

namespace crossfeed {

bool is_valid_port_name(const std::string& name) {
    if (name.empty() || name.length() > 256) return false;
    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) &&
            c != '_' && c != '-' && c != '.' && c != ':' && c != ' ') {
            return false;
        }
    }
    return true;
}

static inline void run_sys_cmd(const std::string& cmd) {
    int res = system(cmd.c_str());
    (void)res;
}

static std::string find_best_physical_sink() {
    FILE* fp = popen("pw-link -i 2>/dev/null | grep playback_FL | cut -d: -f1", "r");
    if (!fp) return "";
    char buf[256] = {0};
    std::string fallback_sink;
    std::string preferred_sink;

    while (fgets(buf, sizeof(buf), fp)) {
        std::string s(buf);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
        if (s.empty() || !is_valid_port_name(s)) continue;
        if (s == "crossfeed" || s == "crossfeed_sink" || s == "Crossfeed") continue;

        if (fallback_sink.empty()) {
            fallback_sink = s;
        }

        std::string lower = s;
        for (char& c : lower) c = std::tolower(static_cast<unsigned char>(c));

        // Prefer headphone, speaker, analog, and non-HDMI outputs
        if (lower.find("hdmi") == std::string::npos) {
            if (lower.find("headphone") != std::string::npos ||
                lower.find("speaker") != std::string::npos ||
                lower.find("analog") != std::string::npos) {
                pclose(fp);
                return s; // Ideal match found
            }
            if (preferred_sink.empty()) {
                preferred_sink = s;
            }
        }
    }
    pclose(fp);

    if (!preferred_sink.empty()) return preferred_sink;
    return fallback_sink;
}

bool PipeWireBackend::safe_pw_link(const std::string& src, const std::string& dst, bool disconnect) {
    if (!is_valid_port_name(src) || !is_valid_port_name(dst)) {
        return false;
    }
    std::string cmd = "pw-link ";
    if (disconnect) {
        cmd += "-d ";
    }
    cmd += "\"" + src + "\" \"" + dst + "\" 2>/dev/null";
    return system(cmd.c_str()) == 0;
}

static void on_filter_process(void *userdata, struct spa_io_position *position) {
    auto *backend = static_cast<PipeWireBackend*>(userdata);
    backend->process_audio(position);
}

static const struct pw_filter_events filter_events = {
    .version = PW_VERSION_FILTER_EVENTS,
    .destroy = nullptr,
    .state_changed = [](void* data, enum pw_filter_state, enum pw_filter_state state, const char* error) {
        static_cast<PipeWireBackend*>(data)->filter_state_changed(state, error);
    },
    .io_changed = nullptr,
    .param_changed = nullptr,
    .add_buffer = nullptr,
    .remove_buffer = nullptr,
    .process = on_filter_process,
    .drained = nullptr,
    .command = nullptr
};

PipeWireBackend::PipeWireBackend() = default;

PipeWireBackend::~PipeWireBackend() {
    cleanup();
}

std::string PipeWireBackend::get_active_target() const {
    std::lock_guard<std::mutex> lock(target_mutex_);
    return reported_target_;
}

void PipeWireBackend::set_active_target(const std::string& target) {
    active_target_sink_ = target; // Lifecycle/link thread only.
    std::lock_guard<std::mutex> lock(target_mutex_);
    reported_target_ = target;
}

std::string PipeWireBackend::resolve_default_sink() {
    auto is_self = [](const std::string& name) {
        return name == "crossfeed" || name == "crossfeed_sink" || name == "Crossfeed";
    };

    FILE* fp = popen("pactl info 2>/dev/null | grep 'Default Sink:' | cut -d' ' -f3", "r");
    if (fp) {
        char buf[256] = {0};
        if (fgets(buf, sizeof(buf), fp)) {
            pclose(fp);
            std::string s(buf);
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
            if (!s.empty() && is_valid_port_name(s)) {
                if (is_self(s)) {
                    // Default sink is currently crossfeed itself, so no device change occurred.
                    if (!active_target_sink_.empty()) {
                        return active_target_sink_;
                    }
                    return find_best_physical_sink();
                }
                return s;
            }
        } else {
            pclose(fp);
        }
    }

    if (!active_target_sink_.empty()) {
        return active_target_sink_;
    }
    return find_best_physical_sink();
}

bool PipeWireBackend::load_null_sink() {
    // 1. Check if module-null-sink for crossfeed is already loaded
    FILE* fp = popen("pactl list short modules 2>/dev/null | grep module-null-sink | grep 'sink_name=crossfeed' | awk '{print $1}'", "r");
    if (fp) {
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), fp)) {
            try {
                null_sink_module_index_ = std::stoul(buf);
                own_module_ = false;
            } catch (...) {}
        }
        pclose(fp);
    }

    // 2. Load module-null-sink
    if (null_sink_module_index_ == 0xFFFFFFFFU) {
        fp = popen("pactl load-module module-null-sink sink_name=crossfeed sink_properties=device.description=Crossfeed 2>/dev/null", "r");
        if (fp) {
            char buf[64] = {0};
            if (fgets(buf, sizeof(buf), fp)) {
                try {
                    null_sink_module_index_ = std::stoul(buf);
                    own_module_ = true;
                } catch (...) {}
            }
            pclose(fp);
        }
    }

    if (null_sink_module_index_ == 0xFFFFFFFFU) {
        return false;
    }

    return true;
}

void PipeWireBackend::activate_routing() {
    // 3. Move active physical sink inputs to crossfeed so ongoing audio immediately routes through filter,
    // preserving intermediate filter chains like EasyEffects.
    std::string init_move_cmd =
        "cf_idx=$(pactl list short sinks 2>/dev/null | grep -w 'crossfeed' | awk '{print $1}'); "
        "ee_idx=$(pactl list short sinks 2>/dev/null | grep -w 'easyeffects_sink' | awk '{print $1}'); "
        "pactl list short sink-inputs 2>/dev/null | while read -r id s_idx rest; do "
        "  [ -n \"$id\" ] && [ -n \"$s_idx\" ] && [ \"$s_idx\" != \"$cf_idx\" ] && [ \"$s_idx\" != \"$ee_idx\" ] && pactl move-sink-input \"$id\" crossfeed 2>/dev/null; "
        "done";
    run_sys_cmd(init_move_cmd);

    // 4. Set crossfeed as default sink
    run_sys_cmd("pactl set-default-sink crossfeed 2>/dev/null");
    routing_active_ = true;
}

void PipeWireBackend::unload_null_sink() {
    // 1. Restore original default sink and move active crossfeed streams back
    std::string restore_sink = original_default_sink_;
    if (restore_sink.empty() || restore_sink == "crossfeed" || restore_sink == "Crossfeed") {
        restore_sink = active_target_sink_;
    }
    if (restore_sink.empty() || restore_sink == "crossfeed" || restore_sink == "Crossfeed") {
        restore_sink = find_best_physical_sink();
    }

    if (routing_active_ && !restore_sink.empty() && restore_sink != "crossfeed" && restore_sink != "Crossfeed") {
        std::string move_cmd =
            "cf_idx=$(pactl list short sinks 2>/dev/null | grep -w 'crossfeed' | awk '{print $1}'); "
            "pactl list short sink-inputs 2>/dev/null | while read -r id s_idx rest; do "
            "  [ -n \"$id\" ] && [ -n \"$s_idx\" ] && [ \"$s_idx\" = \"$cf_idx\" ] && pactl move-sink-input \"$id\" \"" + restore_sink + "\" 2>/dev/null; "
            "done";
        run_sys_cmd(move_cmd);
        std::string def_cmd = "pactl set-default-sink \"" + restore_sink + "\" 2>/dev/null";
        run_sys_cmd(def_cmd);
    }

    // Never unload a sink that was already present before this instance.
    if (own_module_ && null_sink_module_index_ != 0xFFFFFFFFU) {
        run_sys_cmd("pactl unload-module " + std::to_string(null_sink_module_index_) + " >/dev/null 2>&1");
    }
    null_sink_module_index_ = 0xFFFFFFFFU;
    own_module_ = false;
    routing_active_ = false;
}

std::vector<SinkDevice> PipeWireBackend::list_sinks() {
    std::vector<SinkDevice> sinks;
    FILE* fp = popen("pw-link -i 2>/dev/null | grep playback_FL | cut -d: -f1", "r");
    if (!fp) return sinks;
    char buf[256];
    std::string def = resolve_default_sink();
    while (fgets(buf, sizeof(buf), fp)) {
        std::string s(buf);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
        if (!s.empty() && is_valid_port_name(s) && s != "crossfeed" && s != "Crossfeed" && s != "crossfeed_sink") {
            SinkDevice dev;
            dev.name = s;
            dev.description = s;
            dev.is_default = (s == def);
            sinks.push_back(dev);
        }
    }
    pclose(fp);
    return sinks;
}

bool PipeWireBackend::init(CrossfeedDSP* dsp, const std::string& target_sink, uint32_t sample_rate, uint32_t buffer_frames) {
    dsp_ = dsp;
    sample_rate_ = sample_rate;
    buffer_frames_ = buffer_frames;
    requested_target_ = target_sink;

    if (!requested_target_.empty() && requested_target_ != "auto") {
        if (!is_valid_port_name(requested_target_)) {
            std::cerr << "[crossfeed] Error: Invalid target sink name: " << requested_target_ << std::endl;
            return false;
        }
        set_active_target(requested_target_);
    } else {
        set_active_target(resolve_default_sink());
    }

    if (active_target_sink_.empty() || !is_valid_port_name(active_target_sink_)) {
        std::cerr << "[crossfeed] Error: Could not determine active output device." << std::endl;
        return false;
    }

    original_default_sink_ = active_target_sink_;
    target_playback_fl_ = active_target_sink_ + ":playback_FL";
    target_playback_fr_ = active_target_sink_ + ":playback_FR";

    std::cout << "[crossfeed] Native PipeWire filter active for device: " << active_target_sink_ << std::endl;

    if (!load_null_sink()) {
        std::cerr << "[crossfeed] Failed to initialize 'crossfeed' virtual sink." << std::endl;
        return false;
    }

    pw_init(nullptr, nullptr);
    pw_initialized_ = true;

    loop_ = pw_main_loop_new(nullptr);
    if (!loop_) return false;
    control_timer_ = pw_loop_add_timer(pw_main_loop_get_loop(loop_),
        [](void* data, uint64_t) { static_cast<PipeWireBackend*>(data)->service_control(); }, this);
    if (!control_timer_) return false;
    timespec interval{0, 5000000}; // 5 ms; runs off the RT data thread.
    if (pw_loop_update_timer(pw_main_loop_get_loop(loop_), control_timer_, &interval, &interval, false) < 0)
        return false;

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Filter",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_NODE_NAME, "crossfeed-dsp",
        PW_KEY_NODE_DESCRIPTION, "Crossfeed DSP Filter",
        PW_KEY_NODE_PASSIVE, "true",
        PW_KEY_NODE_AUTOCONNECT, "false",
        NULL
    );

    filter_ = pw_filter_new_simple(
        pw_main_loop_get_loop(loop_),
        "crossfeed-dsp",
        props,
        &filter_events,
        this
    );
    if (!filter_) return false;

    in_port_l_ = pw_filter_add_port(
        filter_, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        0, pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
            PW_KEY_PORT_NAME, "in_FL",
            PW_KEY_AUDIO_CHANNEL, "FL",
            PW_KEY_PORT_PASSIVE, "true",
            NULL),
        NULL, 0
    );
    in_port_r_ = pw_filter_add_port(
        filter_, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        0, pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
            PW_KEY_PORT_NAME, "in_FR",
            PW_KEY_AUDIO_CHANNEL, "FR",
            PW_KEY_PORT_PASSIVE, "true",
            NULL),
        NULL, 0
    );

    out_port_l_ = pw_filter_add_port(
        filter_, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        0, pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
            PW_KEY_PORT_NAME, "out_FL",
            PW_KEY_AUDIO_CHANNEL, "FL",
            PW_KEY_PORT_PASSIVE, "true",
            NULL),
        NULL, 0
    );
    out_port_r_ = pw_filter_add_port(
        filter_, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        0, pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
            PW_KEY_PORT_NAME, "out_FR",
            PW_KEY_AUDIO_CHANNEL, "FR",
            PW_KEY_PORT_PASSIVE, "true",
            NULL),
        NULL, 0
    );

    if (!in_port_l_ || !in_port_r_ || !out_port_l_ || !out_port_r_) return false;

    if (pw_filter_connect(filter_, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0) {
        std::cerr << "[crossfeed] Failed to connect pw_filter." << std::endl;
        return false;
    }

    dsp_->request_sample_rate(sample_rate_.load());
    if (!dsp_->service_sample_rate()) return false;
    return true;
}

void PipeWireBackend::service_control() {
    if (stop_requested_.load()) {
        pw_main_loop_quit(loop_);
        return;
    }
    dsp_->service_sample_rate();
}

void PipeWireBackend::filter_state_changed(enum pw_filter_state state, const char* error) {
    if (state == PW_FILTER_STATE_ERROR) {
        std::cerr << "[crossfeed] PipeWire filter error: " << (error ? error : "unknown") << '\n';
        failed_.store(true);
        stop();
    }
}

void PipeWireBackend::process_audio(struct spa_io_position *position) {
    const uint32_t n_samples = position ? position->clock.duration : buffer_frames_.load();
    if (n_samples == 0) return;
    buffer_frames_.store(n_samples, std::memory_order_relaxed);
    if (position) {
        const auto& rate = position->clock.rate;
        const uint32_t hz = rate.num > 0 ? rate.denom / rate.num : 0;
        sample_rate_.store(hz, std::memory_order_relaxed);
        // A nonzero invalid rate keeps DSP in dry fallback until negotiation
        // supplies a supported rate. No getters, mutexes, or coefficient math.
        dsp_->request_sample_rate(hz == 0 ? 1 : hz);
    }

    float *in_l = static_cast<float*>(pw_filter_get_dsp_buffer(in_port_l_, n_samples));
    float *in_r = static_cast<float*>(pw_filter_get_dsp_buffer(in_port_r_, n_samples));
    float *out_l = static_cast<float*>(pw_filter_get_dsp_buffer(out_port_l_, n_samples));
    float *out_r = static_cast<float*>(pw_filter_get_dsp_buffer(out_port_r_, n_samples));

    if (!out_l || !out_r) return;

    if (in_l && in_r) {
        dsp_->process_planar(in_l, in_r, out_l, out_r, n_samples);
    } else if (in_l) {
        dsp_->process_planar(in_l, in_l, out_l, out_r, n_samples);
    } else if (in_r) {
        dsp_->process_planar(in_r, in_r, out_l, out_r, n_samples);
    } else {
        std::memset(out_l, 0, n_samples * sizeof(float));
        std::memset(out_r, 0, n_samples * sizeof(float));
    }
}

void PipeWireBackend::link_manager_loop() {
    // Initial delay for filter registration in the graph
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    if (stop_requested_.load()) return;
    bool bridged = safe_pw_link("crossfeed:monitor_FL", "crossfeed-dsp:in_FL");
    bridged = safe_pw_link("crossfeed:monitor_FR", "crossfeed-dsp:in_FR") && bridged;
    bridged = safe_pw_link("crossfeed-dsp:out_FL", target_playback_fl_) && bridged;
    bridged = safe_pw_link("crossfeed-dsp:out_FR", target_playback_fr_) && bridged;
    if (!bridged) {
        std::cerr << "[crossfeed] Could not establish all stereo bridge links.\n";
        failed_.store(true);
        stop();
        return;
    }
    activate_routing();

    struct PortLinks {
        std::vector<std::string> inputs;
        std::vector<std::string> outputs;
    };

    while (!stop_requested_.load()) {
        // Dynamically follow default physical output device if requested target is auto
        if (requested_target_.empty() || requested_target_ == "auto") {
            std::string cur_def = resolve_default_sink();
            if (!cur_def.empty() && is_valid_port_name(cur_def) && cur_def != active_target_sink_) {
                safe_pw_link("crossfeed-dsp:out_FL", target_playback_fl_, true);
                safe_pw_link("crossfeed-dsp:out_FR", target_playback_fr_, true);

                set_active_target(cur_def);
                original_default_sink_ = cur_def;
                target_playback_fl_ = active_target_sink_ + ":playback_FL";
                target_playback_fr_ = active_target_sink_ + ":playback_FR";

                safe_pw_link("crossfeed-dsp:out_FL", target_playback_fl_);
                safe_pw_link("crossfeed-dsp:out_FR", target_playback_fr_);

                // Re-assert crossfeed as default sink so subsequent new streams go to crossfeed
                run_sys_cmd("pactl set-default-sink crossfeed 2>/dev/null");
            }
        }

        // Parse PipeWire link graph to ensure bridge links remain intact
        std::map<std::string, PortLinks> graph;
        FILE* fp = popen("pw-link -l 2>/dev/null", "r");
        if (fp) {
            char line[512];
            std::string current_port;

            while (fgets(line, sizeof(line), fp)) {
                std::string l(line);
                while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
                if (l.empty()) continue;

                if (l.find("|<-") != std::string::npos) {
                    size_t pos = l.find("|<-");
                    std::string target = l.substr(pos + 3);
                    while (!target.empty() && target.front() == ' ') target.erase(target.begin());
                    while (!target.empty() && target.back() == ' ') target.pop_back();
                    if (!current_port.empty() && !target.empty() && is_valid_port_name(target)) {
                        graph[current_port].inputs.push_back(target);
                    }
                } else if (l.find("|->") != std::string::npos) {
                    size_t pos = l.find("|->");
                    std::string target = l.substr(pos + 3);
                    while (!target.empty() && target.front() == ' ') target.erase(target.begin());
                    while (!target.empty() && target.back() == ' ') target.pop_back();
                    if (!current_port.empty() && !target.empty() && is_valid_port_name(target)) {
                        graph[current_port].outputs.push_back(target);
                    }
                } else if (l[0] != ' ' && l[0] != '\t' && l[0] != '|') {
                    current_port = l;
                    while (!current_port.empty() && current_port.back() == ' ') current_port.pop_back();
                }
            }
            pclose(fp);
        }

        // 1. Maintain input bridge: crossfeed:monitor -> crossfeed-dsp:in
        // Ensure ONLY crossfeed:monitor is connected to crossfeed-dsp:in.
        // If rogue streams connected directly to crossfeed-dsp:in, redirect them to crossfeed virtual sink.
        bool in_fl_linked = false;
        auto in_fl_it = graph.find("crossfeed-dsp:in_FL");
        if (in_fl_it != graph.end()) {
            for (const auto& src : in_fl_it->second.inputs) {
                if (src == "crossfeed:monitor_FL") {
                    in_fl_linked = true;
                } else {
                    // Rogue stream bypassing virtual mixer directly into DSP port!
                    safe_pw_link(src, "crossfeed-dsp:in_FL", true);
                    safe_pw_link(src, "crossfeed:playback_FL");
                }
            }
        }
        if (!in_fl_linked) {
            safe_pw_link("crossfeed:monitor_FL", "crossfeed-dsp:in_FL");
        }

        bool in_fr_linked = false;
        auto in_fr_it = graph.find("crossfeed-dsp:in_FR");
        if (in_fr_it != graph.end()) {
            for (const auto& src : in_fr_it->second.inputs) {
                if (src == "crossfeed:monitor_FR") {
                    in_fr_linked = true;
                } else {
                    safe_pw_link(src, "crossfeed-dsp:in_FR", true);
                    safe_pw_link(src, "crossfeed:playback_FR");
                }
            }
        }
        if (!in_fr_linked) {
            safe_pw_link("crossfeed:monitor_FR", "crossfeed-dsp:in_FR");
        }

        // 2. Maintain output bridge: crossfeed-dsp:out -> target_playback
        bool out_fl_linked = false;
        auto out_fl_it = graph.find("crossfeed-dsp:out_FL");
        if (out_fl_it != graph.end()) {
            for (const auto& dst : out_fl_it->second.outputs) {
                if (dst == target_playback_fl_) { out_fl_linked = true; break; }
            }
        }
        if (!out_fl_linked) {
            safe_pw_link("crossfeed-dsp:out_FL", target_playback_fl_);
        }

        bool out_fr_linked = false;
        auto out_fr_it = graph.find("crossfeed-dsp:out_FR");
        if (out_fr_it != graph.end()) {
            for (const auto& dst : out_fr_it->second.outputs) {
                if (dst == target_playback_fr_) { out_fr_linked = true; break; }
            }
        }
        if (!out_fr_linked) {
            safe_pw_link("crossfeed-dsp:out_FR", target_playback_fr_);
        }

        // 3. Auto-reroute any stream playing directly to target physical sink into crossfeed virtual sink
        bool rogue_target_found = false;
        if (!target_playback_fl_.empty()) {
            auto tgt_fl_it = graph.find(target_playback_fl_);
            if (tgt_fl_it != graph.end()) {
                for (const auto& src : tgt_fl_it->second.inputs) {
                    if (src.find("crossfeed") == std::string::npos && is_valid_port_name(src)) {
                        const std::string redirected = "crossfeed:playback_FL";
                        if (safe_pw_link(src, redirected)) {
                            if (safe_pw_link(src, target_playback_fl_, true)) {
                                rerouted_links_.push_back({src, target_playback_fl_, redirected});
                                rogue_target_found = true;
                            } else {
                                safe_pw_link(src, redirected, true);
                            }
                        }
                    }
                }
            }
        }

        if (!target_playback_fr_.empty()) {
            auto tgt_fr_it = graph.find(target_playback_fr_);
            if (tgt_fr_it != graph.end()) {
                for (const auto& src : tgt_fr_it->second.inputs) {
                    if (src.find("crossfeed") == std::string::npos && is_valid_port_name(src)) {
                        const std::string redirected = "crossfeed:playback_FR";
                        if (safe_pw_link(src, redirected)) {
                            if (safe_pw_link(src, target_playback_fr_, true)) {
                                rerouted_links_.push_back({src, target_playback_fr_, redirected});
                                rogue_target_found = true;
                            } else {
                                safe_pw_link(src, redirected, true);
                            }
                        }
                    }
                }
            }
        }

        // If rogue streams bypassed Crossfeed to play directly to physical sink,
        // also instruct PulseAudio / WirePlumber to migrate sink-inputs bound to physical sinks to crossfeed
        // so WirePlumber updates its stream restore preference.
        if (rogue_target_found) {
            std::string move_cmd =
                "cf_idx=$(pactl list short sinks 2>/dev/null | grep -w 'crossfeed' | awk '{print $1}'); "
                "ee_idx=$(pactl list short sinks 2>/dev/null | grep -w 'easyeffects_sink' | awk '{print $1}'); "
                "pactl list short sink-inputs 2>/dev/null | while read -r id s_idx rest; do "
                "  [ -n \"$id\" ] && [ -n \"$s_idx\" ] && [ \"$s_idx\" != \"$cf_idx\" ] && [ \"$s_idx\" != \"$ee_idx\" ] && pactl move-sink-input \"$id\" crossfeed 2>/dev/null; "
                "done";
            run_sys_cmd(move_cmd);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

void PipeWireBackend::restore_all_links() {
    for (const auto& link : rerouted_links_) {
        // If the user removed our link in the meantime, leave their route alone.
        if (safe_pw_link(link.source, link.redirected, true))
            safe_pw_link(link.source, link.original);
    }
    rerouted_links_.clear();
    safe_pw_link("crossfeed:monitor_FL", "crossfeed-dsp:in_FL", true);
    safe_pw_link("crossfeed:monitor_FR", "crossfeed-dsp:in_FR", true);
    safe_pw_link("crossfeed-dsp:out_FL", target_playback_fl_, true);
    safe_pw_link("crossfeed-dsp:out_FR", target_playback_fr_, true);
    unload_null_sink();
}

bool PipeWireBackend::run() {
    if (!loop_ || !filter_) return false;

    running_.store(true);
    link_thread_ = std::thread(&PipeWireBackend::link_manager_loop, this);

    std::cout << "[crossfeed] Native PipeWire DSP filter running. Real-time audio active.\n";
    pw_main_loop_run(loop_);

    cleanup();
    return !failed_.load();
}

void PipeWireBackend::stop() {
    stop_requested_.store(true);
}

void PipeWireBackend::cleanup() {
    stop_requested_.store(true);
    running_.store(false);
    if (link_thread_.joinable()) link_thread_.join();
    if (filter_) {
        pw_filter_set_active(filter_, false);
        restore_all_links();
        pw_filter_destroy(filter_);
        filter_ = nullptr;
    } else if (null_sink_module_index_ != 0xFFFFFFFFU) {
        unload_null_sink();
    }
    if (loop_) {
        if (control_timer_) {
            pw_loop_destroy_source(pw_main_loop_get_loop(loop_), control_timer_);
            control_timer_ = nullptr;
        }
        pw_main_loop_destroy(loop_);
        loop_ = nullptr;
    }
    if (pw_initialized_) {
        pw_deinit();
        pw_initialized_ = false;
    }
}

} // namespace crossfeed
