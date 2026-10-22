#pragma once

#include "dsp.hpp"
#include <string>
#include <memory>
#include <vector>

namespace crossfeed {

struct SinkDevice {
    std::string name;
    std::string description;
    bool is_default = false;
};

class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    virtual bool init(CrossfeedDSP* dsp, const std::string& target_sink, uint32_t sample_rate, uint32_t buffer_frames) = 0;
    virtual bool run() = 0;
    // Thread-safe cancellation request only. run() owns processing and cleanup;
    // destroy the backend only after run() has returned and callers have joined.
    virtual void stop() = 0;

    virtual std::string get_backend_name() const = 0;
    virtual std::string get_active_target() const = 0;
    virtual std::string get_requested_target() const { return {}; }
    virtual std::string get_output_state() const { return "ready"; }
    virtual uint32_t get_sample_rate() const = 0;
    virtual uint32_t get_buffer_frames() const = 0;
    virtual bool is_running() const = 0;

    virtual std::vector<SinkDevice> list_sinks() { return {}; }
    virtual bool set_target(const std::string& target) { (void)target; return false; }
};

std::unique_ptr<AudioBackend> create_backend(const std::string& name);

} // namespace crossfeed
