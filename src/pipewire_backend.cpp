#include "pipewire_backend.hpp"
#include <spa/param/audio/format-utils.h>
#include <iostream>
#include <sstream>
#include <cstring>
#include <chrono>
#include <unistd.h>
#include <map>
#include <cctype>
#include <algorithm>

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

static int score_physical_sink(const std::string& name) {
    if (name.empty()) return -1;
    if (name == "crossfeed" || name == "crossfeed_sink" || name == "Crossfeed") return -1;

    std::string lower = name;
    for (char& c : lower) c = std::tolower(static_cast<unsigned char>(c));

    int score = 1000;

    // USB Audio Interfaces / DACs (e.g. FiiO, Topping, Schiit, Dragonfly, Focusrite, etc.)
    if (lower.find("usb") != std::string::npos ||
        lower.find("dac") != std::string::npos ||
        lower.find("fiio") != std::string::npos ||
        lower.find("topping") != std::string::npos ||
        lower.find("dragonfly") != std::string::npos ||
        lower.find("audioquest") != std::string::npos ||
        lower.find("focusrite") != std::string::npos ||
        lower.find("schiit") != std::string::npos ||
        lower.find("motu") != std::string::npos ||
        lower.find("rme") != std::string::npos ||
        lower.find("soundblaster") != std::string::npos ||
        lower.find("creative") != std::string::npos) {
        score += 4000;
    }

    // Bluetooth Audio (A2DP headphones/headset)
    if (lower.find("bluez") != std::string::npos ||
        lower.find("bluetooth") != std::string::npos ||
        lower.find("a2dp") != std::string::npos) {
        score += 3000;
    }

    // Built-in Headphones / Headset
    if (lower.find("headphone") != std::string::npos ||
        lower.find("headphones") != std::string::npos ||
        lower.find("headset") != std::string::npos) {
        score += 2500;
    }

    // Analog line out / Stereo
    if (lower.find("analog") != std::string::npos ||
        lower.find("lineout") != std::string::npos) {
        score += 1500;
    }

    // Built-in Speaker
    if (lower.find("speaker") != std::string::npos) {
        score += 500;
    }

    // HDMI / DisplayPort (least preferred for crossfeed headphone listening)
    if (lower.find("hdmi") != std::string::npos ||
        lower.find("displayport") != std::string::npos ||
        lower.find("dp") != std::string::npos) {
        score -= 800;
    }

    return score;
}

static bool is_crossfeed_sink(const std::string& name) {
    return name == "crossfeed" || name == "crossfeed_sink" || name == "Crossfeed" || name == "crossfeed-dsp";
}

struct OutputRoute { std::string sink, left, right; };

// Enumerate one coherent port listing. A reappearing node may initially expose
// just one channel, and its channel names can change with its device profile.
static bool get_output_routes(std::vector<OutputRoute>& routes) {
    FILE* fp = popen("pw-link -i 2>/dev/null", "r");
    if (!fp) return false;
    std::set<std::string> ports;
    char buf[1024];
    while (fgets(buf, sizeof(buf), fp)) {
        std::string port(buf);
        while (!port.empty() && std::isspace(static_cast<unsigned char>(port.back()))) port.pop_back();
        if (is_valid_port_name(port)) ports.insert(port);
    }
    if (pclose(fp) != 0) return false;
    const std::pair<const char*, const char*> channels[] = {
        {"playback_FL", "playback_FR"}, {"playback_0", "playback_1"}, {"playback_AUX0", "playback_AUX1"}
    };
    std::set<std::string> sinks;
    for (const auto& port : ports) {
        const auto colon = port.rfind(':');
        if (colon != std::string::npos && !is_crossfeed_sink(port.substr(0, colon)))
            sinks.insert(port.substr(0, colon));
    }
    for (const auto& sink : sinks) {
        for (const auto& pair : channels) {
            const std::string left = sink + ":" + pair.first, right = sink + ":" + pair.second;
            if (ports.count(left) && ports.count(right)) {
                routes.push_back({sink, left, right});
                break;
            }
        }
    }
    return true;
}

static std::vector<std::string> get_available_physical_sinks() {
    std::vector<OutputRoute> routes;
    get_output_routes(routes);
    std::vector<std::string> sinks;
    for (const auto& route : routes) sinks.push_back(route.sink);
    return sinks;
}

static std::string system_default_sink() {
    FILE* fp = popen("LC_ALL=C pactl info 2>/dev/null", "r");
    if (!fp) return {};
    std::string sink;
    char buf[1024];
    const std::string prefix = "Default Sink:";
    while (fgets(buf, sizeof(buf), fp)) {
        std::string line(buf);
        if (line.compare(0, prefix.size(), prefix) != 0) continue;
        sink = line.substr(prefix.size());
        while (!sink.empty() && std::isspace(static_cast<unsigned char>(sink.back()))) sink.pop_back();
        const auto begin = sink.find_first_not_of(" \t");
        sink = begin == std::string::npos ? "" : sink.substr(begin);
    }
    if (pclose(fp) != 0 || !is_valid_port_name(sink)) return {};
    return sink;
}

static std::string find_best_physical_sink() {
    const auto sinks = get_available_physical_sinks();
    std::string best_sink;
    int best_score = -1;
    for (const auto& s : sinks) {
        int sc = score_physical_sink(s);
        if (sc > best_score) {
            best_score = sc;
            best_sink = s;
        }
    }
    return best_sink;
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

std::string PipeWireBackend::get_requested_target() const {
    std::lock_guard<std::mutex> lock(request_mutex_);
    return requested_target_;
}

std::string PipeWireBackend::get_output_state() const {
    switch (output_state_.load()) {
        case OutputState::Ready: return "ready";
        case OutputState::Waiting: return "waiting";
        default: return "reconnecting";
    }
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
    const auto raw_sinks = get_available_physical_sinks();
    std::string current_target = get_active_target();
    for (const auto& s : raw_sinks) {
        SinkDevice dev;
        dev.name = s;
        dev.description = s;
        dev.is_default = (s == current_target);
        sinks.push_back(dev);
    }
    return sinks;
}

bool PipeWireBackend::set_target(const std::string& target) {
    const std::string requested = target.empty() ? "auto" : target;
    if (requested != "auto" && (!is_valid_port_name(requested) || is_crossfeed_sink(requested))) return false;
    std::lock_guard<std::mutex> lock(request_mutex_);
    requested_target_ = requested;
    return true;
}

bool PipeWireBackend::reconcile_target(const LinkGraph& graph) {
    std::vector<OutputRoute> routes;
    if (!get_output_routes(routes)) {
        output_state_.store(OutputState::Reconnecting);
        return false; // A failed query is not proof that a device was unplugged.
    }
    observed_default_sink_ = system_default_sink();
    // The server also changes its default automatically during unplug/replug.
    // After initial selection, keep Auto's remembered preference rather than
    // interpreting a transient fallback default as an explicit user request.
    const auto find_route = [&](const std::string& name) -> const OutputRoute* {
        for (const auto& route : routes) if (route.sink == name) return &route;
        return nullptr;
    };
    const OutputRoute* best = nullptr;
    for (const auto& route : routes)
        if (!best || score_physical_sink(route.sink) > score_physical_sink(best->sink)) best = &route;

    const std::string requested = get_requested_target();
    if (requested != applied_request_) {
        applied_request_ = requested;
        auto_preferred_sink_.clear();
        auto_preferred_missing_ = false;
    }
    const OutputRoute* desired = nullptr;
    if (requested != "auto") {
        desired = find_route(requested);
    } else {
        const auto* system_default = find_route(observed_default_sink_);
        const auto* preferred = find_route(auto_preferred_sink_);
        const bool returning = preferred && (auto_preferred_missing_ || active_target_sink_ != auto_preferred_sink_);
        if (auto_preferred_sink_.empty()) {
            desired = system_default ? system_default : best;
            if (desired) auto_preferred_sink_ = desired->sink;
        } else if (!preferred) {
            auto_preferred_missing_ = true;
            desired = best; // Temporary fallback; remember the disconnected DAC.
        } else {
            desired = preferred;
            if (!returning && best && !previous_physical_sinks_.count(best->sink) &&
                score_physical_sink(best->sink) > score_physical_sink(preferred->sink)) {
                desired = best;
                auto_preferred_sink_ = desired->sink;
            }
        }
    }
    previous_physical_sinks_.clear();
    for (const auto& route : routes) previous_physical_sinks_.insert(route.sink);

    const auto linked = [&](const std::string& source, const std::string& destination) {
        const auto found = graph.find(source);
        return found != graph.end() && std::find(found->second.outputs.begin(), found->second.outputs.end(), destination) != found->second.outputs.end();
    };
    if (!desired) {
        // An explicit missing target waits rather than playing on another device.
        for (const char* source : {"crossfeed-dsp:out_FL", "crossfeed-dsp:out_FR"}) {
            const auto found = graph.find(source);
            if (found != graph.end()) for (const auto& destination : found->second.outputs)
                safe_pw_link(source, destination, true);
        }
        output_state_.store(OutputState::Waiting);
        return false;
    }

    // Connect both new channels before retiring a working route. On partial
    // failure, undo only links created by this attempt and retry next pass.
    const bool had_left = linked("crossfeed-dsp:out_FL", desired->left);
    const bool had_right = linked("crossfeed-dsp:out_FR", desired->right);
    const bool left = had_left || safe_pw_link("crossfeed-dsp:out_FL", desired->left);
    const bool right = had_right || safe_pw_link("crossfeed-dsp:out_FR", desired->right);
    if (!left || !right) {
        if (left && !had_left) safe_pw_link("crossfeed-dsp:out_FL", desired->left, true);
        if (right && !had_right) safe_pw_link("crossfeed-dsp:out_FR", desired->right, true);
        output_state_.store(OutputState::Reconnecting);
        return false;
    }
    for (const auto& channel : {std::make_pair("crossfeed-dsp:out_FL", desired->left),
                                std::make_pair("crossfeed-dsp:out_FR", desired->right)}) {
        const auto found = graph.find(channel.first);
        if (found != graph.end()) for (const auto& destination : found->second.outputs)
            if (destination != channel.second) safe_pw_link(channel.first, destination, true);
    }
    if (desired->sink == auto_preferred_sink_) auto_preferred_missing_ = false;
    set_active_target(desired->sink);
    original_default_sink_ = desired->sink;
    target_playback_fl_ = desired->left;
    target_playback_fr_ = desired->right;
    return true;
}

bool PipeWireBackend::init(CrossfeedDSP* dsp, const std::string& target_sink, uint32_t sample_rate, uint32_t buffer_frames) {
    dsp_ = dsp;
    sample_rate_ = sample_rate;
    buffer_frames_ = buffer_frames;
    if (!set_target(target_sink)) {
        std::cerr << "[crossfeed] Error: Invalid target sink name.\n";
        return false;
    }
    // Keep the service alive if no output is present yet. Routing is activated
    // only after the worker has verified the complete stereo bridge.
    std::cout << "[crossfeed] Native PipeWire filter waiting for output routing.\n";

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
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    int consecutive_stable = 0;
    while (!stop_requested_.load()) {
        // Discovery runs on EVERY pass, even after an arbitrarily long stable
        // session. Health-based backoff must never disable hotplug detection.
        const bool healthy = service_routing();
        consecutive_stable = healthy ? std::min(consecutive_stable + 1, 4) : 0;
        const int sleep_ms = consecutive_stable >= 4 ? 1000 : 250;
        for (int elapsed = 0; elapsed < sleep_ms && !stop_requested_.load(); elapsed += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

bool PipeWireBackend::service_routing() {
    if (stop_requested_.load()) return false;
    // Parse PipeWire link graph to ensure bridge links remain intact
    LinkGraph graph;
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
        if (pclose(fp) != 0) {
            output_state_.store(OutputState::Reconnecting);
            return false;
        }
    } else {
        output_state_.store(OutputState::Reconnecting);
        return false;
    }
    const bool output_ready = reconcile_target(graph);

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
        in_fl_linked = safe_pw_link("crossfeed:monitor_FL", "crossfeed-dsp:in_FL");
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
        in_fr_linked = safe_pw_link("crossfeed:monitor_FR", "crossfeed-dsp:in_FR");
    }

    // 3. Auto-reroute any stream playing directly to target physical sink into crossfeed virtual sink
    bool rogue_target_found = false;
    if (output_ready && !target_playback_fl_.empty()) {
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

    if (output_ready && !target_playback_fr_.empty()) {
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

    const bool bridge_ready = in_fl_linked && in_fr_linked && output_ready;
    if (bridge_ready) {
        if (!routing_active_) activate_routing();
        else if (!observed_default_sink_.empty() && observed_default_sink_ != "crossfeed")
            run_sys_cmd("pactl set-default-sink crossfeed 2>/dev/null");
        output_state_.store(OutputState::Ready);
    } else if (output_ready) {
        output_state_.store(OutputState::Reconnecting);
    }
    return bridge_ready && !rogue_target_found;
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
