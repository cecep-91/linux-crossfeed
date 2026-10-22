#include "ipc.hpp"
#include "shutdown.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <unistd.h>

using namespace crossfeed;

class TestBackend : public AudioBackend {
public:
    bool init(CrossfeedDSP*, const std::string&, uint32_t, uint32_t) override { return true; }
    bool run() override { return true; }
    void stop() override { stopped.store(true); }
    std::string get_backend_name() const override { return "test"; }
    std::string get_active_target() const override { return "test-sink"; }
    std::string get_requested_target() const override { return requested_target; }
    std::string get_output_state() const override { return "waiting"; }
    bool set_target(const std::string& target) override {
        if (target == "invalid") return false;
        requested_target = target;
        return true;
    }
    std::string requested_target = "auto";
    uint32_t get_sample_rate() const override { return 48000; }
    uint32_t get_buffer_frames() const override { return 1024; }
    bool is_running() const override { return !stopped.load(); }
    std::atomic<bool> stopped{false};
};

int main() {
    ShutdownSignals signals; // Before any threads are spawned.
    TestBackend backend;
    ShutdownWatcher watcher(signals, backend);
    char temporary[] = "/tmp/crossfeed-ipc-XXXXXX";
    const char* directory = mkdtemp(temporary);
    assert(directory);
    const char* old_config = std::getenv("XDG_CONFIG_HOME");
    const std::string old_value = old_config ? old_config : "";
    const bool had_config = old_config != nullptr;
    setenv("XDG_CONFIG_HOME", directory, 1);
    CrossfeedDSP dsp;
    ConfigState config;
    IpcServer server(&dsp, &backend, &config);
    server.set_stop_callback([&backend] { backend.stop(); });
    const std::string socket = std::string(directory) + "/control.sock";
    assert(server.start(socket));
    auto send = [&](const std::string& command) {
        std::string response;
        assert(IpcClient::send_command(socket, command, response));
        return response;
    };

    assert(send("PING") == "PONG");
    assert(send("STATUS").find("\"output_state\": \"waiting\"") != std::string::npos);
    const auto target_response = send("TARGET usb.dac");
    assert(target_response.find("\"requested_target\": \"usb.dac\"") != std::string::npos);
    ConfigState saved_target;
    assert(Config::load_state(saved_target) && saved_target.target_sink == "usb.dac");
    assert(send("TARGET invalid").find("error") != std::string::npos);
    assert(Config::load_state(saved_target) && saved_target.target_sink == "usb.dac");
    assert(send("TARGET auto").find("error") == std::string::npos);
    assert(Config::load_state(saved_target) && saved_target.target_sink == "auto");
    auto before = dsp.get_params();
    for (const char* command : {"SET level=-20 delay=nan", "SET freq=inf", "SET trim=-inf",
                                "SET freq=123junk", "SET advanced=maybe"}) {
        assert(send(command).find("error") != std::string::npos);
        assert(dsp.get_params().generation == before.generation);
        assert(config.level_db == DEFAULT_LEVEL_DB);
    }
    assert(send("SET level=-14 freq=800 pure=1").find("error") == std::string::npos);
    assert(dsp.get_params().generation == before.generation + 1);
    assert(dsp.get_level_db() == -14 && !dsp.get_advanced_effects());
    before = dsp.get_params();
    assert(send("PRESET meier").find("Jan Meier") != std::string::npos);
    assert(dsp.get_params().generation == before.generation + 1);
    assert(send("STATUS").find("1024") != std::string::npos);
    assert(send("SET preamp=nan level=-20").find("error") != std::string::npos);
    assert(send("SET preamp=-6").find("error") == std::string::npos);
    assert(dsp.get_params().preamp_db == -6);
    assert(send("AB A").find("empty_slot") != std::string::npos);
    assert(send("AB STORE A").find("\"slot_a\": true") != std::string::npos);
    send("SET level=-20");
    assert(send("AB A").find("error") == std::string::npos);
    assert(dsp.get_level_db() == -9.5f);
    assert(send("AB DRY").find("\"enabled\": false") != std::string::npos);
    assert(send("STATUS").find("\"bypass_follows_trim\": true") != std::string::npos);
    ConfigState saved;
    assert(Config::load_state(saved) && saved.preamp_db == -6);
    assert(send("METERS RESET").find("error") == std::string::npos);
    for (int i = 0; i < 4; ++i) assert(send("STOP").find("stopping") != std::string::npos);
    assert(backend.stopped.load());
    server.stop(); // Joins the response thread; no detached callback remains.

    backend.stopped.store(false);
    assert(kill(getpid(), SIGTERM) == 0);
    for (int i = 0; i < 100 && !backend.stopped.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(backend.stopped.load());
    std::filesystem::remove_all(directory);
    if (had_config) setenv("XDG_CONFIG_HOME", old_value.c_str(), 1);
    else unsetenv("XDG_CONFIG_HOME");
    std::cout << "PASS: IPC rejects partial/invalid updates, presets publish once, STOP and SIGTERM request joined shutdown.\n";
}
