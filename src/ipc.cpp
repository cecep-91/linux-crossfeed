#include "ipc.hpp"
#include "presets.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <iostream>
#include <sstream>
#include <iomanip>

namespace crossfeed {

IpcServer::IpcServer(CrossfeedDSP* dsp, AudioBackend* backend, ConfigState* config)
    : dsp_(dsp), backend_(backend), config_(config) {}

IpcServer::~IpcServer() {
    stop();
}

bool IpcServer::start(const std::string& socket_path) {
    socket_path_ = socket_path;
    unlink(socket_path_.c_str());

    server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        return false;
    }

    // Set non-blocking
    int flags = fcntl(server_fd_, F_GETFL, 0);
    fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    chmod(socket_path_.c_str(), 0600);

    if (listen(server_fd_, 5) < 0) {
        close(server_fd_);
        server_fd_ = -1;
        unlink(socket_path_.c_str());
        return false;
    }

    running_.store(true);
    server_thread_ = std::thread(&IpcServer::run_loop, this);
    return true;
}

void IpcServer::stop() {
    if (running_.load()) {
        running_.store(false);
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }
    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }
    if (!socket_path_.empty()) {
        unlink(socket_path_.c_str());
        socket_path_.clear();
    }
}

void IpcServer::run_loop() {
    while (running_.load()) {
        struct pollfd pfd{};
        pfd.fd = server_fd_;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 200); // 200ms timeout
        if (ret > 0 && (pfd.revents & POLLIN)) {
            int client_fd = accept(server_fd_, nullptr, nullptr);
            if (client_fd >= 0) {
                struct timeval tv{};
                tv.tv_sec = 0;
                tv.tv_usec = 500000; // 500ms timeout
                setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

                char buf[1024];
                ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
                if (n > 0) {
                    buf[n] = '\0';
                    std::string cmd(buf);
                    while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == '\n' || cmd.back() == ' ')) {
                        cmd.pop_back();
                    }
                    std::string reply = handle_command(cmd) + "\n";
                    ssize_t w = send(client_fd, reply.data(), reply.size(), MSG_NOSIGNAL);
                    (void)w;
                }
                close(client_fd);
            }
        }
    }
}

static std::string build_status_json(CrossfeedDSP* dsp, AudioBackend* backend) {
    const auto params = dsp->get_params();
    const auto* p = detect_active_preset(
        params.level_db,
        params.freq_hz,
        params.delay_us,
        params.phase_apf_hz,
        params.center_trim_db,
        params.shadow_hz,
        params.advanced_effects
    );

    const auto listening = dsp->get_listening_status();
    FilterParams a, b;
    const bool slot_a = dsp->get_slot(0, a), slot_b = dsp->get_slot(1, b);
    std::ostringstream ss;
    ss << "{\n"
       << "  \"status\": \"" << (backend && backend->is_running() ? "running" : "stopped") << "\",\n"
       << "  \"enabled\": " << (params.enabled ? "true" : "false") << ",\n"
       << "  \"preset\": \"" << (p ? p->name : "Custom") << "\",\n"
       << std::fixed << std::setprecision(1)
       << "  \"level_db\": " << params.level_db << ",\n"
       << std::setprecision(0)
       << "  \"freq_hz\": " << params.freq_hz << ",\n"
       << std::setprecision(1)
       << "  \"delay_us\": " << params.delay_us << ",\n"
       << std::setprecision(0)
       << "  \"phase_apf_hz\": " << params.phase_apf_hz << ",\n"
       << std::setprecision(1)
       << "  \"center_trim_db\": " << params.center_trim_db << ",\n"
       << std::setprecision(0)
       << "  \"shadow_hz\": " << params.shadow_hz << ",\n"
       << "  \"advanced_effects\": " << (params.advanced_effects ? "true" : "false") << ",\n"
       << "  \"backend\": \"" << (backend ? backend->get_backend_name() : "unknown") << "\",\n"
       << "  \"target\": \"" << (backend ? backend->get_active_target() : "") << "\",\n"
       << "  \"sample_rate\": " << (backend ? backend->get_sample_rate() : 48000) << ",\n"
       << "  \"buffer_frames\": " << (backend ? backend->get_buffer_frames() : 256) << ",\n"
       << std::setprecision(2)
       << "  \"preamp_db\": " << params.preamp_db << ",\n"
       << "  \"bypass_follows_trim\": true,\n"
       << "  \"slot_a\": " << (slot_a ? "true" : "false") << ",\n"
       << "  \"slot_b\": " << (slot_b ? "true" : "false") << ",\n"
       << "  \"slot_a_active\": " << (params.enabled && slot_a && CrossfeedDSP::same_sound(params, a) ? "true" : "false") << ",\n"
       << "  \"slot_b_active\": " << (params.enabled && slot_b && CrossfeedDSP::same_sound(params, b) ? "true" : "false") << ",\n"
       << std::setprecision(6)
       << "  \"input_peak\": " << listening.input_peak << ",\n"
       << "  \"output_peak\": " << listening.output_peak << ",\n"
       << "  \"input_hold\": " << listening.input_hold << ",\n"
       << "  \"output_hold\": " << listening.output_hold << ",\n"
       << "  \"input_clip\": " << (listening.input_clip ? "true" : "false") << ",\n"
       << "  \"output_clip\": " << (listening.output_clip ? "true" : "false") << "\n"
       << "}";
    return ss.str();
}

std::string IpcServer::handle_command(const std::string& cmd) {
    if (cmd == "STATUS") {
        return build_status_json(dsp_, backend_);
    } else if (cmd == "TOGGLE") {
        bool new_state = !dsp_->is_enabled();
        dsp_->set_enabled(new_state);
        config_->enabled = new_state;
        Config::save_state(*config_);
        return build_status_json(dsp_, backend_);
    } else if (cmd == "ON") {
        dsp_->set_enabled(true);
        config_->enabled = true;
        Config::save_state(*config_);
        return build_status_json(dsp_, backend_);
    } else if (cmd == "OFF") {
        dsp_->set_enabled(false);
        config_->enabled = false;
        Config::save_state(*config_);
        return build_status_json(dsp_, backend_);
    } else if (cmd.rfind("PRESET ", 0) == 0) {
        std::string p_name = cmd.substr(7);
        while (!p_name.empty() && (p_name.front() == ' ' || p_name.front() == '\t')) p_name.erase(p_name.begin());
        while (!p_name.empty() && (p_name.back() == ' ' || p_name.back() == '\t' || p_name.back() == '\r' || p_name.back() == '\n')) p_name.pop_back();
        const auto* p = find_preset(p_name);
        if (p) {
            const auto old = dsp_->get_params();
            if (!dsp_->set_all_params(old.sample_rate, p->level_db, p->freq_hz,
                p->delay_us, p->phase_apf_hz, p->center_trim_db, p->shadow_hz, true, old.enabled))
                return "{\"error\":\"invalid_parameters\"}";
            const auto applied = dsp_->get_params();
            config_->level_db = applied.level_db;
            config_->freq_hz = applied.freq_hz;
            config_->delay_us = applied.delay_us;
            config_->phase_apf_hz = applied.phase_apf_hz;
            config_->center_trim_db = applied.center_trim_db;
            config_->shadow_hz = applied.shadow_hz;
            config_->advanced_effects = applied.advanced_effects;
            Config::save_state(*config_);
            return build_status_json(dsp_, backend_);
        } else {
            return "{\"error\": \"unknown_preset\"}";
        }
    } else if (cmd.rfind("SET ", 0) == 0) {
        // Parse the entire transaction first. Invalid input must not partially
        // update DSP, saved settings, or a preset's coefficient snapshot.
        auto params = dsp_->get_params();
        std::istringstream iss(cmd.substr(4));
        std::string token;
        bool changed = false;
        while (iss >> token) {
            const size_t eq = token.find('=');
            if (eq == std::string::npos) return "{\"error\":\"invalid_parameters\"}";
            const std::string k = token.substr(0, eq), v = token.substr(eq + 1);
            float* field = nullptr;
            if (k == "level" || k == "level_db") field = &params.level_db;
            else if (k == "freq" || k == "freq_hz") field = &params.freq_hz;
            else if (k == "delay" || k == "delay_us") field = &params.delay_us;
            else if (k == "phase" || k == "phase_apf_hz" || k == "phase_hz") field = &params.phase_apf_hz;
            else if (k == "trim" || k == "center_trim_db" || k == "trim_db") field = &params.center_trim_db;
            else if (k == "preamp" || k == "preamp_db") field = &params.preamp_db;
            else if (k == "shadow" || k == "shadow_hz") field = &params.shadow_hz;
            if (field) {
                try {
                    size_t end = 0;
                    const float value = std::stof(v, &end);
                    if (end != v.size() || !std::isfinite(value)) return "{\"error\":\"invalid_number\"}";
                    *field = value;
                } catch (...) { return "{\"error\":\"invalid_number\"}"; }
            } else {
                const bool yes = v == "true" || v == "1" || v == "on";
                const bool no = v == "false" || v == "0" || v == "off";
                if (!yes && !no) return "{\"error\":\"invalid_boolean\"}";
                if (k == "enabled") params.enabled = yes;
                else if (k == "advanced" || k == "advanced_effects" || k == "effects") params.advanced_effects = yes;
                else if (k == "pure" || k == "pure_crossfeed") params.advanced_effects = !yes;
                else return "{\"error\":\"unknown_parameter\"}";
            }
            changed = true;
        }
        if (changed) {
            if (!dsp_->set_snapshot(params))
                return "{\"error\":\"invalid_parameters\"}";
            params = dsp_->get_params();
            config_->enabled = params.enabled;
            config_->advanced_effects = params.advanced_effects;
            config_->level_db = params.level_db;
            config_->freq_hz = params.freq_hz;
            config_->delay_us = params.delay_us;
            config_->phase_apf_hz = params.phase_apf_hz;
            config_->center_trim_db = params.center_trim_db;
            config_->shadow_hz = params.shadow_hz;
            config_->preamp_db = params.preamp_db;
            Config::save_state(*config_);
        }
        return build_status_json(dsp_, backend_);
    } else if (cmd == "AB STORE A" || cmd == "AB STORE B") {
        dsp_->store_slot(cmd.back() == 'A' ? 0 : 1);
        return build_status_json(dsp_, backend_);
    } else if (cmd == "AB A" || cmd == "AB B" || cmd == "AB DRY") {
        if (cmd == "AB DRY") dsp_->set_enabled(false);
        else if (!dsp_->recall_slot(cmd.back() == 'A' ? 0 : 1)) return "{\"error\":\"empty_slot\"}";
        const auto p = dsp_->get_params();
        config_->enabled = p.enabled; config_->advanced_effects = p.advanced_effects;
        config_->level_db = p.level_db; config_->freq_hz = p.freq_hz;
        config_->delay_us = p.delay_us; config_->phase_apf_hz = p.phase_apf_hz;
        config_->center_trim_db = p.center_trim_db; config_->shadow_hz = p.shadow_hz;
        Config::save_state(*config_);
        return build_status_json(dsp_, backend_);
    } else if (cmd == "METERS RESET") {
        dsp_->reset_meters();
        return build_status_json(dsp_, backend_);
    } else if (cmd == "STOP") {
        // stop() only requests cancellation. The processing owner tears down;
        // ipc.stop() joins this thread after it has sent the response.
        if (stop_callback_) stop_callback_();
        return "{\"status\":\"stopping\"}";
    } else if (cmd == "PING") {
        return "PONG";
    }

    return "{\"error\":\"unknown command\"}";
}

bool IpcClient::send_command(const std::string& socket_path, const std::string& cmd, std::string& response) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 300000; // 300ms timeout
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return false;
    }

    std::string to_send = cmd + "\n";
    if (write(fd, to_send.data(), to_send.size()) <= 0) {
        close(fd);
        return false;
    }

    char buf[4096];
    std::string res;
    ssize_t n = 0;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        res.append(buf);
    }
    close(fd);

    while (!res.empty() && (res.back() == '\r' || res.back() == '\n' || res.back() == ' ')) {
        res.pop_back();
    }
    response = res;
    return true;
}

} // namespace crossfeed
