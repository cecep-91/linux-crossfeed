#pragma once

#include "audio_backend.hpp"
#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <atomic>
#include <string>
#include <vector>
#include <thread>
#include <set>
#include <mutex>

namespace crossfeed {

bool is_valid_port_name(const std::string& name);

struct InterceptedStream {
    std::string out_l;
    std::string out_r;
};

class PipeWireBackend : public AudioBackend {
public:
    PipeWireBackend();
    ~PipeWireBackend() override;

    bool init(CrossfeedDSP* dsp, const std::string& target_sink, uint32_t sample_rate, uint32_t buffer_frames) override;
    bool run() override;
    void stop() override;

    std::string get_backend_name() const override { return "PipeWire Native Filter (In-Line)"; }
    std::string get_active_target() const override;
    uint32_t get_sample_rate() const override { return sample_rate_.load(); }
    uint32_t get_buffer_frames() const override { return buffer_frames_.load(); }
    bool is_running() const override { return running_.load(); }

    std::vector<SinkDevice> list_sinks() override;

    // Called from real-time process callback
    void process_audio(struct spa_io_position *position);
    void service_control(); // Main-loop timer, never the audio thread.
    void filter_state_changed(enum pw_filter_state state, const char* error);

private:
    static bool safe_pw_link(const std::string& src, const std::string& dst, bool disconnect = false);
    std::string resolve_default_sink();
    bool load_null_sink();
    void unload_null_sink();
    void link_manager_loop();
    void restore_all_links();
    void cleanup();
    void activate_routing();
    void set_active_target(const std::string& target);

    CrossfeedDSP* dsp_ = nullptr;
    std::string requested_target_;
    std::string active_target_sink_;
    std::string target_playback_fl_;
    std::string target_playback_fr_;
    std::string original_default_sink_;
    uint32_t null_sink_module_index_ = 0xFFFFFFFFU;
    bool own_module_ = false;

    std::atomic<uint32_t> sample_rate_{48000};
    std::atomic<uint32_t> buffer_frames_{256};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> failed_{false};
    bool pw_initialized_ = false;
    bool routing_active_ = false;
    mutable std::mutex target_mutex_;
    std::string reported_target_;
    struct spa_source* control_timer_ = nullptr;

    struct pw_main_loop* loop_ = nullptr;
    struct pw_filter* filter_ = nullptr;
    void* in_port_l_ = nullptr;
    void* in_port_r_ = nullptr;
    void* out_port_l_ = nullptr;
    void* out_port_r_ = nullptr;

    std::atomic<bool> running_{false};
    std::thread link_thread_;

    // Link worker owns this journal; cleanup reads only after joining it.
    struct ReroutedLink { std::string source, original, redirected; };
    std::vector<ReroutedLink> rerouted_links_;
};

} // namespace crossfeed
