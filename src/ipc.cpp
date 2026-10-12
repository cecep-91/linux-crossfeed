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
                    ssize_t w = write(client_fd, reply.data(), reply.size());
                    (void)w;
                }
                close(client_fd);
            }
        }
    }
}

static std::string build_status_json(CrossfeedDSP* dsp, AudioBackend* backend) {
    const auto* p = detect_active_preset(
        dsp->get_level_db(),
        dsp->get_freq_hz(),
        dsp->get_delay_us(),
        dsp->get_phase_apf_hz(),
        dsp->get_center_trim_db(),
        dsp->get_shadow_hz(),
        dsp->get_advanced_effects()
    );

    std::ostringstream ss;
    ss << "{\n"
       << "  \"status\": \"" << (backend && backend->is_running() ? "running" : "stopped") << "\",\n"
       << "  \"enabled\": " << (dsp->is_enabled() ? "true" : "false") << ",\n"
       << "  \"preset\": \"" << (p ? p->name : "Custom") << "\",\n"
       << std::fixed << std::setprecision(1)
       << "  \"level_db\": " << dsp->get_level_db() << ",\n"
       << std::setprecision(0)
       << "  \"freq_hz\": " << dsp->get_freq_hz() << ",\n"
       << std::setprecision(1)
       << "  \"delay_us\": " << dsp->get_delay_us() << ",\n"
       << std::setprecision(0)
       << "  \"phase_apf_hz\": " << dsp->get_phase_apf_hz() << ",\n"
       << std::setprecision(1)
       << "  \"center_trim_db\": " << dsp->get_center_trim_db() << ",\n"
       << std::setprecision(0)
       << "  \"shadow_hz\": " << dsp->get_shadow_hz() << ",\n"
       << "  \"advanced_effects\": " << (dsp->get_advanced_effects() ? "true" : "false") << ",\n"
       << "  \"backend\": \"" << (backend ? backend->get_backend_name() : "unknown") << "\",\n"
       << "  \"target\": \"" << (backend ? backend->get_active_target() : "") << "\",\n"
       << "  \"sample_rate\": " << (backend ? backend->get_sample_rate() : 48000) << ",\n"
       << "  \"buffer_frames\": " << (backend ? backend->get_buffer_frames() : 256) << "\n"
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
            dsp_->set_level_db(p->level_db);
            dsp_->set_freq_hz(p->freq_hz);
            dsp_->set_delay_us(p->delay_us);
            dsp_->set_phase_apf_hz(p->phase_apf_hz);
            dsp_->set_center_trim_db(p->center_trim_db);
            dsp_->set_shadow_hz(p->shadow_hz);
            dsp_->set_advanced_effects(true);

            config_->level_db = p->level_db;
            config_->freq_hz = p->freq_hz;
            config_->delay_us = p->delay_us;
            config_->phase_apf_hz = p->phase_apf_hz;
            config_->center_trim_db = p->center_trim_db;
            config_->shadow_hz = p->shadow_hz;
            config_->advanced_effects = true;
            Config::save_state(*config_);
            return build_status_json(dsp_, backend_);
        } else {
            return "{\"error\": \"unknown_preset\"}";
        }
    } else if (cmd.rfind("SET ", 0) == 0) {
        std::istringstream iss(cmd.substr(4));
        std::string token;
        bool changed = false;
        while (iss >> token) {
            size_t eq = token.find('=');
            if (eq != std::string::npos) {
                std::string k = token.substr(0, eq);
                std::string v = token.substr(eq + 1);
                if (k == "enabled") {
                    bool en = (v == "true" || v == "1" || v == "on");
                    dsp_->set_enabled(en);
                    config_->enabled = en;
                    changed = true;
                } else if (k == "level" || k == "level_db") {
                    try {
                        float lvl = std::stof(v);
                        dsp_->set_level_db(lvl);
                        config_->level_db = dsp_->get_level_db();
                        changed = true;
                    } catch (...) {}
                } else if (k == "freq" || k == "freq_hz") {
                    try {
                        float f = std::stof(v);
                        dsp_->set_freq_hz(f);
                        config_->freq_hz = dsp_->get_freq_hz();
                        changed = true;
                    } catch (...) {}
                } else if (k == "delay" || k == "delay_us") {
                    try {
                        float d = std::stof(v);
                        dsp_->set_delay_us(d);
                        config_->delay_us = dsp_->get_delay_us();
                        changed = true;
                    } catch (...) {}
                } else if (k == "phase" || k == "phase_apf_hz" || k == "phase_hz") {
                    try {
                        float p = std::stof(v);
                        dsp_->set_phase_apf_hz(p);
                        config_->phase_apf_hz = dsp_->get_phase_apf_hz();
                        changed = true;
                    } catch (...) {}
                } else if (k == "trim" || k == "center_trim_db" || k == "trim_db") {
                    try {
                        float t = std::stof(v);
                        dsp_->set_center_trim_db(t);
                        config_->center_trim_db = dsp_->get_center_trim_db();
                        changed = true;
                    } catch (...) {}
                } else if (k == "shadow" || k == "shadow_hz") {
                    try {
                        float s = std::stof(v);
                        dsp_->set_shadow_hz(s);
                        config_->shadow_hz = dsp_->get_shadow_hz();
                        changed = true;
                    } catch (...) {}
                } else if (k == "advanced" || k == "advanced_effects" || k == "effects") {
                    bool adv = (v == "true" || v == "1" || v == "on");
                    dsp_->set_advanced_effects(adv);
                    config_->advanced_effects = adv;
                    changed = true;
                } else if (k == "pure" || k == "pure_crossfeed") {
                    bool adv = !(v == "true" || v == "1" || v == "on");
                    dsp_->set_advanced_effects(adv);
                    config_->advanced_effects = adv;
                    changed = true;
                }
            }
        }
        if (changed) {
            Config::save_state(*config_);
        }
        return build_status_json(dsp_, backend_);
    } else if (cmd == "STOP") {
        if (stop_callback_) {
            // Trigger stop in another thread so this response finishes writing
            std::thread([this]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                stop_callback_();
            }).detach();
        }
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
