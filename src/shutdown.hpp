#pragma once

#include "audio_backend.hpp"
#include <csignal>
#include <poll.h>
#include <pthread.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <thread>
#include <stdexcept>

namespace crossfeed {

// Construct before backends/IPC create threads so they inherit the blocked mask.
class ShutdownSignals {
public:
    ShutdownSignals() {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);
        if (pthread_sigmask(SIG_BLOCK, &mask, &previous_) != 0)
            throw std::runtime_error("Cannot block shutdown signals");
        fd_ = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
        if (fd_ < 0) {
            pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
            throw std::runtime_error("Cannot create shutdown signal descriptor");
        }
    }
    ~ShutdownSignals() {
        close(fd_);
        pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    }
    int fd() const { return fd_; }
    ShutdownSignals(const ShutdownSignals&) = delete;
    ShutdownSignals& operator=(const ShutdownSignals&) = delete;
private:
    sigset_t previous_{};
    int fd_ = -1;
};

// Declare after the backend: this joined watcher must die before its target.
class ShutdownWatcher {
public:
    ShutdownWatcher(const ShutdownSignals& signals, AudioBackend& backend)
        : thread_([this, &signals, &backend] {
            while (!done_.load()) {
                pollfd pfd{signals.fd(), POLLIN, 0};
                if (poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN)) {
                    signalfd_siginfo info{};
                    if (read(signals.fd(), &info, sizeof(info)) == sizeof(info)) {
                        backend.stop();
                    }
                }
            }
        }) {}
    ~ShutdownWatcher() { done_.store(true); thread_.join(); }
    ShutdownWatcher(const ShutdownWatcher&) = delete;
    ShutdownWatcher& operator=(const ShutdownWatcher&) = delete;
private:
    std::atomic<bool> done_{false};
    std::thread thread_;
};

} // namespace crossfeed
