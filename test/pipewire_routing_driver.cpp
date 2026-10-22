// Run the production routing worker against fake commands, never a sound server.
#include "pipewire_backend.hpp"
#include <iostream>
#include <chrono>
#include <thread>

namespace crossfeed {
struct PipeWireRoutingTestAccess {
    static void step(PipeWireBackend& backend) { backend.service_routing(); }
    static void loop(PipeWireBackend& backend) { backend.link_manager_loop(); }
};
}

int main(int argc, char**) {
    crossfeed::PipeWireBackend backend;
    const bool loop = argc > 1;
    std::thread worker;
    if (loop) worker = std::thread([&] { crossfeed::PipeWireRoutingTestAccess::loop(backend); });
    std::string command;
    while (std::getline(std::cin, command)) {
        if (command == "QUIT") break;
        bool accepted = true;
        if (command.compare(0, 7, "TARGET ") == 0) accepted = backend.set_target(command.substr(7));
        else if (command == "STEP" && !loop) crossfeed::PipeWireRoutingTestAccess::step(backend);
        else if (command == "STRESS") {
            std::thread writer([&] {
                for (int i = 0; i < 1000; ++i) {
                    backend.set_target(i % 2 ? "auto" : "usb.dac");
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            });
            for (int i = 0; i < 10000; ++i) {
                backend.get_requested_target();
                backend.get_active_target();
                backend.get_output_state();
            }
            writer.join();
            backend.set_target("auto");
        }
        std::cout << (accepted ? "ok" : "error") << '\t' << backend.get_output_state()
                  << '\t' << backend.get_active_target() << '\t' << backend.get_requested_target() << std::endl;
    }
    backend.stop();
    if (worker.joinable()) worker.join();
}
