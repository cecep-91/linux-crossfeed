#include "dsp.hpp"
#include "config.hpp"
#include "ipc.hpp"
#include "audio_backend.hpp"
#include "benchmark.hpp"
#include "gui.hpp"
#include "presets.hpp"
#include "user_presets.hpp"
#include "shutdown.hpp"
#include <iostream>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>
#include <iomanip>
#include <vector>
#include <limits.h>

using namespace crossfeed;

static bool parse_cli_float(const std::string& opt_name, const char* str_val, float& out_val) {
    try {
        size_t idx = 0;
        out_val = std::stof(str_val, &idx);
        if (idx != std::string(str_val).length() || !std::isfinite(out_val)) {
            std::cerr << "[crossfeed] Error: Invalid numeric value for " << opt_name << ": '" << str_val << "'\n";
            return false;
        }
        return true;
    } catch (const std::exception&) {
        std::cerr << "[crossfeed] Error: Invalid numeric value for " << opt_name << ": '" << str_val << "'\n";
        return false;
    }
}

static bool parse_cli_uint(const std::string& opt_name, const char* str_val, uint32_t& out_val) {
    try {
        size_t idx = 0;
        std::string s(str_val);
        if (s.find('-') != std::string::npos) {
            std::cerr << "[crossfeed] Error: Invalid integer value for " << opt_name << ": '" << str_val << "'\n";
            return false;
        }
        unsigned long v = std::stoul(str_val, &idx);
        if (idx != s.length() || v > 0xFFFFFFFFUL) {
            std::cerr << "[crossfeed] Error: Invalid integer value for " << opt_name << ": '" << str_val << "'\n";
            return false;
        }
        out_val = static_cast<uint32_t>(v);
        return true;
    } catch (const std::exception&) {
        std::cerr << "[crossfeed] Error: Invalid integer value for " << opt_name << ": '" << str_val << "'\n";
        return false;
    }
}

static void print_help(const char* prog) {
    std::cout << "Usage: " << prog << " [command] [options]\n\n"
              << "A standalone, ultra-low-latency headphone crossfeed audio processor\n"
              << "compatible with any Linux distribution and sound server.\n\n"
              << "Commands:\n"
              << "  gui, app          Launch the graphical user interface with system tray (default)\n"
              << "  run               Run the crossfeed engine in the foreground\n"
              << "  start             Start the crossfeed engine in the background (daemon)\n"
              << "  stop              Stop the running crossfeed engine\n"
              << "  restart           Restart the crossfeed engine\n"
              << "  status            Display engine status and active audio routing\n"
              << "  toggle            Toggle crossfeed filtering on/off (bypass)\n"
              << "  on                Enable crossfeed filtering\n"
              << "  off               Bypass crossfeed (active output trim and preamp still apply)\n"
              << "  presets           List available psychoacoustic presets\n"
              << "  preset <name>     Apply a psychoacoustic preset (e.g. meier, chumoy, bs2b, linkwitz, studio)\n"
              << "  sinks             List available physical audio output devices\n"
              << "  target <name>     Switch target output device (or 'auto' for smart hotplug)\n"
              << "  set               Adjust filter parameters live (including --preamp <dB>)\n"
              << "  ab store a|b      Save current sound to an A/B slot (engine session)\n"
              << "  ab a|b|dry        Listen to slot A, slot B, or dry audio with the same output trim\n"
              << "  meters reset      Clear peak holds and clip indicators\n"
              << "  bench             Run DSP throughput and CPU performance benchmark\n"
              << "  upgrade, update   Upgrade crossfeed to the latest release\n"
              << "  check-update      Check if a newer version is available on GitHub\n"
              << "  help, --help      Show this help message\n"
              << "  --version         Show version information\n\n"
              << "Run Options:\n"
              << "  --backend <name>  Audio backend: 'auto' (default), 'pipewire', 'pulse', 'alsa'\n"
              << "  --target <sink>   Target output sink (default: auto non-crossfeed sink)\n"
              << "  --level <dB>      Crossfeed blend level in dB (-30.0 to -3.0, default: -10.0)\n"
              << "  --freq <Hz>       Crossover frequency in Hz (200 to 2000, default: 700)\n"
              << "  --delay <us>      Acoustic delay in microseconds (0 to 800, default: 280)\n"
              << "  --phase <Hz>      Phase alignment all-pass frequency (200 to 4000, default: 1500)\n"
              << "  --trim <dB>       Center summing gain trim in dB (-6.0 to 0.0, default: -1.5)\n"
              << "  --shadow <Hz>     Head acoustic shadow cutoff in Hz (1000 to 8000, default: 3000)\n"
              << "  --advanced <0|1>  Enable all acoustic spatial effects (1) or pure crossfeed (0)\n"
              << "  --pure            Pure crossfeed mode (bypass delay, phase, shadow, and trim)\n"
              << "  --rate <Hz>       Sample rate (8000–192000, default: 48000)\n"
              << "  --buffer <frames> Buffer size; PipeWire reports negotiated quantum (default: 256)\n"
              << "  --bypass          Start in bypassed state\n\n"
              << "GUI Options:\n"
              << "  gui --preview    Explore the interface without starting or contacting the engine\n\n"
              << "Set Options:\n"
              << "  --level <dB>      Set blend level in dB\n"
              << "  --freq <Hz>       Set crossover frequency in Hz\n"
              << "  --delay <us>      Set acoustic delay in microseconds\n"
              << "  --phase <Hz>      Set phase alignment all-pass frequency in Hz\n"
              << "  --trim <dB>       Set center summing gain trim in dB\n"
              << "  --shadow <Hz>     Set head shadow cutoff in Hz\n"
              << "  --advanced <0|1>  Set all effects (1) or pure crossfeed (0)\n"
              << "  --pure            Switch to pure crossfeed mode\n"
              << "  --effects         Switch to all effects mode\n"
              << "  --enabled <0|1>   Set filter active (1) or bypassed (0)\n\n"
              << "Status Options:\n"
              << "  --json            Output status in JSON format\n";
}

static int cmd_run(int argc, char** argv) {
    ConfigState config;
    Config::load_state(config);

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--backend") {
            if (i + 1 >= argc) {
                std::cerr << "[crossfeed] Error: Option " << arg << " requires an argument.\n";
                return 1;
            }
            config.backend = argv[++i];
        } else if (arg == "--target") {
            if (i + 1 >= argc) {
                std::cerr << "[crossfeed] Error: Option " << arg << " requires an argument.\n";
                return 1;
            }
            config.target_sink = argv[++i];
        } else if (arg == "--level") {
            if (i + 1 >= argc) {
                std::cerr << "[crossfeed] Error: Option " << arg << " requires an argument.\n";
                return 1;
            }
            if (!parse_cli_float(arg, argv[++i], config.level_db)) {
                return 1;
            }
        } else if (arg == "--freq") {
            if (i + 1 >= argc) {
                std::cerr << "[crossfeed] Error: Option " << arg << " requires an argument.\n";
                return 1;
            }
            if (!parse_cli_float(arg, argv[++i], config.freq_hz)) {
                return 1;
            }
        } else if (arg == "--delay") {
            if (i + 1 >= argc) {
                std::cerr << "[crossfeed] Error: Option " << arg << " requires an argument.\n";
                return 1;
            }
            if (!parse_cli_float(arg, argv[++i], config.delay_us)) {
                return 1;
            }
        } else if (arg == "--phase") {
            if (i + 1 >= argc) {
                std::cerr << "[crossfeed] Error: Option " << arg << " requires an argument.\n";
                return 1;
            }
            if (!parse_cli_float(arg, argv[++i], config.phase_apf_hz)) {
                return 1;
            }
        } else if (arg == "--trim") {
            if (i + 1 >= argc) {
                std::cerr << "[crossfeed] Error: Option " << arg << " requires an argument.\n";
                return 1;
            }
            if (!parse_cli_float(arg, argv[++i], config.center_trim_db)) {
                return 1;
            }
        } else if (arg == "--shadow") {
            if (i + 1 >= argc) {
                std::cerr << "[crossfeed] Error: Option " << arg << " requires an argument.\n";
                return 1;
            }
            if (!parse_cli_float(arg, argv[++i], config.shadow_hz)) {
                return 1;
            }
        } else if (arg == "--advanced") {
            if (i + 1 >= argc) {
                std::cerr << "[crossfeed] Error: Option " << arg << " requires an argument.\n";
                return 1;
            }
            config.advanced_effects = (std::string(argv[++i]) == "1");
        } else if (arg == "--pure") {
            config.advanced_effects = false;
        } else if (arg == "--effects") {
            config.advanced_effects = true;
        } else if (arg == "--rate") {
            if (i + 1 >= argc) {
                std::cerr << "[crossfeed] Error: Option " << arg << " requires an argument.\n";
                return 1;
            }
            if (!parse_cli_uint(arg, argv[++i], config.sample_rate)) {
                return 1;
            }
        } else if (arg == "--buffer") {
            if (i + 1 >= argc) {
                std::cerr << "[crossfeed] Error: Option " << arg << " requires an argument.\n";
                return 1;
            }
            if (!parse_cli_uint(arg, argv[++i], config.buffer_frames)) {
                return 1;
            }
        } else if (arg == "--bypass") {
            config.enabled = false;
        } else if (arg == "--enabled") {
            config.enabled = true;
        }
    }

    std::cout << "[crossfeed] Starting standalone crossfeed audio engine...\n"
              << "  Backend: " << config.backend << "\n"
              << "  Target: " << config.target_sink << "\n"
              << "  Mode: " << (config.advanced_effects ? "All Effects (ITD, APF, Shadow, Trim)" : "Pure Crossfeed") << "\n"
              << "  Level: " << config.level_db << " dB\n"
              << "  Freq: " << config.freq_hz << " Hz\n"
              << "  Delay: " << config.delay_us << " µs\n"
              << "  Phase APF: " << config.phase_apf_hz << " Hz\n"
              << "  Center Trim: " << config.center_trim_db << " dB\n"
              << "  Head Shadow: " << config.shadow_hz << " Hz\n"
              << "  Filter: " << (config.enabled ? "ENABLED" : "BYPASSED") << "\n";

    if (!CrossfeedDSP::valid_sample_rate(static_cast<float>(config.sample_rate)) ||
        config.buffer_frames == 0 || config.buffer_frames > 65536) {
        std::cerr << "[crossfeed] Rate must be 8000–192000 Hz and buffer 1–65536 frames.\n";
        return 1;
    }
    ShutdownSignals signals;
    CrossfeedDSP dsp;
    if (!dsp.set_all_params(static_cast<float>(config.sample_rate), config.level_db, config.freq_hz,
                       config.delay_us, config.phase_apf_hz, config.center_trim_db, config.shadow_hz,
                       config.advanced_effects, config.enabled)) {
        std::cerr << "[crossfeed] Invalid non-finite DSP settings.\n";
        return 1;
    }

    dsp.set_preamp_db(config.preamp_db);

    auto backend = create_backend(config.backend);
    if (!backend) {
        std::cerr << "[crossfeed] Error: Failed to create backend '" << config.backend << "'\n";
        return 1;
    }

    if (!backend->init(&dsp, config.target_sink, config.sample_rate, config.buffer_frames)) {
        std::cerr << "[crossfeed] Error: Failed to initialize backend.\n";
        return 1;
    }

    ShutdownWatcher shutdown(signals, *backend);

    std::string sock_path = Config::get_socket_path();
    IpcServer ipc(&dsp, backend.get(), &config);
    ipc.set_stop_callback([backend_ptr = backend.get()]() {
        if (backend_ptr) backend_ptr->stop();
    });

    if (!ipc.start(sock_path)) {
        std::cerr << "[crossfeed] Warning: Failed to start IPC socket server at " << sock_path << "\n";
    } else {
        std::cout << "[crossfeed] IPC control listening at " << sock_path << "\n";
    }

    bool ok = backend->run();

    ipc.stop();
    std::cout << "[crossfeed] Engine shutdown cleanly.\n";
    return ok ? 0 : 1;
}

static int cmd_start(int argc, char** argv) {
    std::string resp;
    if (IpcClient::send_command(Config::get_socket_path(), "PING", resp) && resp == "PONG") {
        std::cout << "Crossfeed engine is already running.\n";
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "Failed to fork process.\n";
        return 1;
    }
    if (pid > 0) {
        // Parent: wait a moment for daemon to start and verify
        for (int i = 0; i < 20; ++i) {
            usleep(100000); // 100ms
            if (IpcClient::send_command(Config::get_socket_path(), "PING", resp) && resp == "PONG") {
                std::cout << "Crossfeed engine started successfully in background (PID: " << pid << ").\n";
                return 0;
            }
        }
        std::cout << "Crossfeed background process launched (PID: " << pid << ").\n";
        return 0;
    }

    // Child process: redirect standard streams and run
    setsid();
    (void)!freopen("/dev/null", "r", stdin);
    (void)!freopen("/dev/null", "w", stdout);
    (void)!freopen("/dev/null", "w", stderr);

    char* run_argv[argc + 1];
    run_argv[0] = argv[0];
    run_argv[1] = const_cast<char*>("run");
    for (int i = 2; i < argc; ++i) {
        run_argv[i] = argv[i];
    }
    run_argv[argc] = nullptr;

    return cmd_run(argc, run_argv);
}

static int cmd_stop() {
    std::string resp;
    if (!IpcClient::send_command(Config::get_socket_path(), "STOP", resp)) {
        std::cout << "Crossfeed is not running.\n";
        return 0;
    }
    std::cout << "Sent stop command to Crossfeed engine.\n";
    for (int i = 0; i < 20; ++i) {
        usleep(100000);
        if (!IpcClient::send_command(Config::get_socket_path(), "PING", resp)) {
            std::cout << "Crossfeed stopped successfully.\n";
            return 0;
        }
    }
    return 0;
}

static int cmd_status(bool json_output) {
    std::string resp;
    if (!IpcClient::send_command(Config::get_socket_path(), "STATUS", resp)) {
        if (json_output) {
            std::cout << "{\"status\":\"stopped\"}\n";
        } else {
            std::cout << "Crossfeed is NOT running.\n"
                      << "Start it with: crossfeed start\n";
        }
        return 1;
    }

    if (json_output) {
        std::cout << resp << "\n";
        return 0;
    }

    // Human-friendly formatted output
    std::cout << "========================================\n"
              << "       Crossfeed Engine Status          \n"
              << "========================================\n";

    auto get_field = [&](const std::string& key) -> std::string {
        std::string search = "\"" + key + "\"";
        size_t pos = resp.find(search);
        if (pos == std::string::npos) return "";
        pos = resp.find(':', pos + search.length());
        if (pos == std::string::npos) return "";
        size_t start = pos + 1;
        while (start < resp.size() && (resp[start] == ' ' || resp[start] == '\t' || resp[start] == '\r' || resp[start] == '\n')) start++;
        if (start >= resp.size()) return "";
        if (resp[start] == '\"') {
            size_t end = resp.find('\"', start + 1);
            if (end == std::string::npos) return "";
            return resp.substr(start + 1, end - start - 1);
        } else {
            size_t end = resp.find_first_of(",}\r\n", start);
            if (end == std::string::npos) end = resp.size();
            return resp.substr(start, end - start);
        }
    };

    std::string st = get_field("status");
    std::string en = get_field("enabled");
    std::string lvl = get_field("level_db");
    std::string frq = get_field("freq_hz");
    std::string del = get_field("delay_us");
    std::string phs = get_field("phase_apf_hz");
    std::string trm = get_field("center_trim_db");
    std::string shd = get_field("shadow_hz");
    std::string adv = get_field("advanced_effects");
    std::string prs = get_field("preset");
    std::string bk = get_field("backend");
    std::string tgt = get_field("target");
    std::string sr = get_field("sample_rate");
    std::string buf = get_field("buffer_frames");

    std::cout << "  State:          " << (st == "running" ? "● Running" : "○ Stopped") << "\n"
              << "  Filter:         " << (en == "true" ? "ON (Processing active)" : "OFF (Bypassed)") << "\n"
              << "  Preset:         " << (prs.empty() ? "Custom" : prs) << "\n"
              << "  Mode:           " << (adv == "false" ? "Pure Crossfeed (Basic blend only)" : "All Effects (ITD, APF, Shadow, Trim)") << "\n"
              << "  Blend Level:    " << lvl << " dB\n"
              << "  Crossover:      " << frq << " Hz\n"
              << "  Delay (ITD):    " << (del.empty() ? "280" : del) << " µs\n"
              << "  Phase All-Pass: " << (phs.empty() ? "1500" : phs) << " Hz\n"
              << "  Center Trim:    " << (trm.empty() ? "-1.5" : trm) << " dB\n"
              << "  Head Shadow:    " << (shd.empty() ? "3000" : shd) << " Hz\n"
              << "  Preamp:         " << get_field("preamp_db") << " dB\n"
              << "  Bypass trim:    " << (get_field("bypass_follows_trim") == "true" ? "Follows active output trim" : "Not reported by engine") << "\n"
              << "  Backend:        " << bk << "\n"
              << "  Output Sink:    " << tgt << "\n"
              << "  Sample Rate:    " << sr << " Hz\n"
              << "  Buffer Size:    " << buf << " frames\n"
              << "========================================\n";
    return 0;
}

static int cmd_toggle() {
    std::string resp;
    if (!IpcClient::send_command(Config::get_socket_path(), "TOGGLE", resp)) {
        std::cerr << "Error: Crossfeed engine is not running.\n"
                  << "Start it first with: crossfeed start\n";
        return 1;
    }
    bool enabled = resp.find("\"enabled\": true") != std::string::npos;
    std::cout << "Crossfeed: " << (enabled ? "ON" : "OFF (Bypassed)") << "\n";
    return 0;
}

static int cmd_set_enabled(bool enabled) {
    std::string cmd = enabled ? "ON" : "OFF";
    std::string resp;
    if (!IpcClient::send_command(Config::get_socket_path(), cmd, resp)) {
        std::cerr << "Error: Crossfeed engine is not running.\n";
        return 1;
    }
    std::cout << "Crossfeed: " << (enabled ? "ON" : "OFF (Bypassed)") << "\n";
    return 0;
}

static int cmd_set(int argc, char** argv) {
    std::string cmd = "SET";
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--level") {
            if (i + 1 >= argc) {
                std::cerr << "[crossfeed] Error: Option " << arg << " requires an argument.\n";
                return 1;
            }
            float val;
            if (!parse_cli_float(arg, argv[++i], val)) return 1;
            cmd += " level=" + std::string(argv[i]);
        } else if (arg == "--freq") {
            if (i + 1 >= argc) {
                std::cerr << "[crossfeed] Error: Option " << arg << " requires an argument.\n";
                return 1;
            }
            float val;
            if (!parse_cli_float(arg, argv[++i], val)) return 1;
            cmd += " freq=" + std::string(argv[i]);
        } else if (arg == "--delay") {
            if (i + 1 >= argc) {
                std::cerr << "[crossfeed] Error: Option " << arg << " requires an argument.\n";
                return 1;
            }
            float val;
            if (!parse_cli_float(arg, argv[++i], val)) return 1;
            cmd += " delay=" + std::string(argv[i]);
        } else if (arg == "--phase") {
            if (i + 1 >= argc) {
                std::cerr << "[crossfeed] Error: Option " << arg << " requires an argument.\n";
                return 1;
            }
            float val;
            if (!parse_cli_float(arg, argv[++i], val)) return 1;
            cmd += " phase=" + std::string(argv[i]);
        } else if (arg == "--preamp") {
            float value;
            if (i + 1 >= argc || !parse_cli_float(arg, argv[++i], value)) return 1;
            cmd += " preamp=" + std::string(argv[i]);
        } else if (arg == "--trim") {
            if (i + 1 >= argc) {
                std::cerr << "[crossfeed] Error: Option " << arg << " requires an argument.\n";
                return 1;
            }
            float val;
            if (!parse_cli_float(arg, argv[++i], val)) return 1;
            cmd += " trim=" + std::string(argv[i]);
        } else if (arg == "--shadow") {
            if (i + 1 >= argc) {
                std::cerr << "[crossfeed] Error: Option " << arg << " requires an argument.\n";
                return 1;
            }
            float val;
            if (!parse_cli_float(arg, argv[++i], val)) return 1;
            cmd += " shadow=" + std::string(argv[i]);
        } else if (arg == "--advanced") {
            if (i + 1 >= argc) {
                std::cerr << "[crossfeed] Error: Option " << arg << " requires an argument.\n";
                return 1;
            }
            std::string val = argv[++i];
            if (val != "0" && val != "1") {
                std::cerr << "[crossfeed] Error: Invalid boolean value for " << arg << ": '" << val << "' (expected 0 or 1)\n";
                return 1;
            }
            cmd += " advanced=" + val;
        } else if (arg == "--pure") {
            cmd += " pure=1";
        } else if (arg == "--effects") {
            cmd += " advanced=1";
        } else if (arg == "--enabled") {
            if (i + 1 >= argc) {
                std::cerr << "[crossfeed] Error: Option " << arg << " requires an argument.\n";
                return 1;
            }
            std::string val = argv[++i];
            if (val != "0" && val != "1") {
                std::cerr << "[crossfeed] Error: Invalid boolean value for " << arg << ": '" << val << "' (expected 0 or 1)\n";
                return 1;
            }
            cmd += " enabled=" + val;
        }
    }
    if (cmd == "SET") {
        std::cerr << "Usage: crossfeed set [--level <dB>] [--freq <Hz>] [--delay <us>] [--phase <Hz>] [--trim <dB>] [--shadow <Hz>] [--advanced <0|1>] [--pure] [--effects] [--enabled <0|1>]\n";
        return 1;
    }

    std::string resp;
    if (!IpcClient::send_command(Config::get_socket_path(), cmd, resp)) {
        std::cerr << "Error: Crossfeed engine is not running.\n";
        return 1;
    }
    if (resp.find("\"error\"") != std::string::npos) { std::cerr << resp << "\n"; return 1; }
    std::cout << "Settings applied successfully.\n";
    return cmd_status(false);
}

static int cmd_presets() {
    std::cout << "=================================================================\n"
              << "             Crossfeed Acoustic Emulation Presets                \n"
              << "=================================================================\n\n";
    for (const auto& p : g_presets) {
        std::cout << "  " << std::left << std::setw(10) << p.id
                  << " " << p.name << "\n"
                  << "             " << p.description << "\n"
                  << "             Blend: " << std::fixed << std::setprecision(1) << p.level_db << " dB | "
                  << "Cutoff: " << std::setprecision(0) << p.freq_hz << " Hz | "
                  << "Delay: " << p.delay_us << " µs | "
                  << "APF: " << p.phase_apf_hz << " Hz\n\n";
    }
    std::vector<UserPreset> saved;
    std::string library_error;
    if (UserPresets::load(UserPresets::path(), saved, library_error) && !saved.empty()) {
        std::cout << "Saved user presets (exact names):\n";
        for (const auto& entry : saved) std::cout << "  " << entry.name << "\n";
        std::cout << '\n';
    }
    std::cout << "Usage:\n"
              << "  crossfeed preset <name>    Apply preset live or save to configuration\n"
              << "=================================================================\n";
    return 0;
}

static int cmd_preset(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Error: Preset name required.\n\n";
        cmd_presets();
        return 1;
    }
    std::string name = argv[2];
    std::vector<UserPreset> saved;
    std::string library_error;
    if (UserPresets::load(UserPresets::path(), saved, library_error)) for (const auto& entry : saved) {
        if (entry.name != name) continue;
        const auto& p = entry.params;
        std::ostringstream command; command.imbue(std::locale::classic()); command << std::setprecision(9);
        command << "SET level=" << p.level_db << " freq=" << p.freq_hz << " delay=" << p.delay_us
                << " phase=" << p.phase_apf_hz << " trim=" << p.center_trim_db << " shadow=" << p.shadow_hz
                << " advanced=" << int(p.advanced_effects) << " preamp=" << p.preamp_db;
        std::string response;
        if (IpcClient::send_command(Config::get_socket_path(), command.str(), response)) {
            if (response.find("\"error\"") != std::string::npos) { std::cerr << response << '\n'; return 1; }
            return cmd_status(false);
        }
        ConfigState cfg; Config::load_state(cfg);
        cfg.level_db = p.level_db; cfg.freq_hz = p.freq_hz; cfg.delay_us = p.delay_us;
        cfg.phase_apf_hz = p.phase_apf_hz; cfg.center_trim_db = p.center_trim_db;
        cfg.shadow_hz = p.shadow_hz; cfg.advanced_effects = p.advanced_effects; cfg.preamp_db = p.preamp_db;
        if (!Config::save_state(cfg)) { std::cerr << "Could not save preset settings.\n"; return 1; }
        std::cout << "Engine is stopped. Saved preset '" << name << "' to configuration.\n"; return 0;
    }
    const auto* p = find_preset(name);
    if (!p) {
        std::cerr << "Error: Unknown preset '" << name << "'.\n\n";
        cmd_presets();
        return 1;
    }

    std::string resp;
    if (IpcClient::send_command(Config::get_socket_path(), "PRESET " + std::string(p->id), resp)) {
        std::cout << "Applied preset '" << p->name << "' successfully.\n";
        return cmd_status(false);
    } else {
        ConfigState cfg;
        Config::load_state(cfg);
        cfg.level_db = p->level_db;
        cfg.freq_hz = p->freq_hz;
        cfg.delay_us = p->delay_us;
        cfg.phase_apf_hz = p->phase_apf_hz;
        cfg.center_trim_db = p->center_trim_db;
        cfg.shadow_hz = p->shadow_hz;
        cfg.advanced_effects = true;
        Config::save_state(cfg);
        std::cout << "Engine is stopped. Saved preset '" << p->name << "' to configuration.\n";
        return 0;
    }
}

static int cmd_sinks(bool json_output = false) {
    std::string resp;
    if (!IpcClient::send_command(Config::get_socket_path(), "SINKS", resp)) {
        std::cerr << "Crossfeed engine is not running.\n";
        return 1;
    }
    if (json_output) {
        std::cout << resp << "\n";
        return 0;
    }

    std::string active_sink;
    size_t act_pos = resp.find("\"active\": \"");
    if (act_pos != std::string::npos) {
        act_pos += 11;
        size_t end = resp.find('"', act_pos);
        if (end != std::string::npos) active_sink = resp.substr(act_pos, end - act_pos);
    }

    std::cout << "================================================================================\n"
              << "                         Audio Output Devices                           \n"
              << "================================================================================\n"
              << "  STATUS    DEVICE NAME / ENDPOINT\n"
              << "--------------------------------------------------------------------------------\n";

    size_t pos = 0;
    while ((pos = resp.find("\"name\": \"", pos)) != std::string::npos) {
        pos += 9;
        size_t end = resp.find('"', pos);
        if (end == std::string::npos) break;
        std::string name = resp.substr(pos, end - pos);
        pos = end + 1;

        bool is_active = (!active_sink.empty() && name == active_sink);
        std::cout << "  " << (is_active ? "● ACTIVE  " : "          ") << name << "\n";
    }

    std::cout << "================================================================================\n"
              << "Target switch: crossfeed target <device|auto>\n";
    return 0;
}

static int cmd_target(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: crossfeed target <sink_name|auto>\n";
        return 1;
    }
    std::string target = argv[2];
    std::string resp;
    if (IpcClient::send_command(Config::get_socket_path(), "TARGET " + target, resp)) {
        if (resp.find("\"error\"") != std::string::npos) {
            std::cerr << resp << "\n";
            return 1;
        }
        std::cout << "Target output set to: " << target << "\n";
        return 0;
    }
    ConfigState cfg;
    Config::load_state(cfg);
    cfg.target_sink = target;
    Config::save_state(cfg);
    std::cout << "Engine is stopped. Saved target output '" << target << "' to configuration.\n";
    return 0;
}

static int cmd_update(int argc, char** argv, bool check_only) {
    std::string script_path;
    std::vector<std::string> candidates = {
        "./scripts/update.sh",
        "scripts/update.sh"
    };

    char exe_buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (len > 0) {
        exe_buf[len] = '\0';
        std::string exe_path(exe_buf);
        size_t last_slash = exe_path.find_last_of('/');
        if (last_slash != std::string::npos) {
            std::string dir = exe_path.substr(0, last_slash);
            candidates.push_back(dir + "/../scripts/update.sh");
            candidates.push_back(dir + "/scripts/update.sh");
        }
    }

    const char* home = std::getenv("HOME");
    if (home) {
        candidates.push_back(std::string(home) + "/.local/share/pipewire-crossfeed/update.sh");
    }
    candidates.push_back("/usr/share/pipewire-crossfeed/update.sh");
    candidates.push_back("/usr/local/share/pipewire-crossfeed/update.sh");

    for (const auto& c : candidates) {
        if (access(c.c_str(), X_OK) == 0) {
            script_path = c;
            break;
        }
    }

    std::string cmd;
    std::string args;
    if (check_only) {
        args += " --check";
    }
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--check" && check_only) continue;
        args += " " + a;
    }

    if (!script_path.empty()) {
        cmd = "\"" + script_path + "\"" + args;
    } else {
        cmd = "curl -fsSL https://raw.githubusercontent.com/cecep-91/linux-crossfeed/development/scripts/update.sh | bash -s --" + args;
    }

    int ret = system(cmd.c_str());
    if (WIFEXITED(ret)) {
        return WEXITSTATUS(ret);
    }
    return ret;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        return run_gui(argc, argv);
    }

    std::string cmd = argv[1];
    if (cmd == "gui" || cmd == "app" || cmd == "--gui") {
        return run_gui(argc, argv);
    } else if (cmd == "run") {
        return cmd_run(argc, argv);
    } else if (cmd == "start" || cmd == "--daemon") {
        return cmd_start(argc, argv);
    } else if (cmd == "stop") {
        return cmd_stop();
    } else if (cmd == "restart") {
        cmd_stop();
        std::string ping_resp;
        for (int i = 0; i < 30; ++i) {
            usleep(100000);
            if (!IpcClient::send_command(Config::get_socket_path(), "PING", ping_resp)) {
                break;
            }
        }
        usleep(100000);
        return cmd_start(argc, argv);
    } else if (cmd == "status") {
        bool json = (argc > 2 && std::string(argv[2]) == "--json");
        return cmd_status(json);
    } else if (cmd == "toggle") {
        return cmd_toggle();
    } else if (cmd == "on") {
        return cmd_set_enabled(true);
    } else if (cmd == "off") {
        return cmd_set_enabled(false);
    } else if (cmd == "presets") {
        return cmd_presets();
    } else if (cmd == "preset") {
        return cmd_preset(argc, argv);
    } else if (cmd == "sinks") {
        return cmd_sinks(argc > 2 && std::string(argv[2]) == "--json");
    } else if (cmd == "target") {
        return cmd_target(argc, argv);
    } else if (cmd == "set") {
        return cmd_set(argc, argv);
    } else if (cmd == "ab" || cmd == "meters") {
        std::string request;
        const std::string action = argc > 2 ? argv[2] : "";
        if (cmd == "ab" && argc == 3 && (action == "a" || action == "b" || action == "dry")) {
            request = action == "a" ? "AB A" : action == "b" ? "AB B" : "AB DRY";
        } else if (cmd == "ab" && argc == 4 && action == "store" &&
                   (std::string(argv[3]) == "a" || std::string(argv[3]) == "b")) {
            request = std::string(argv[3]) == "a" ? "AB STORE A" : "AB STORE B";
        } else if (cmd == "meters" && argc == 3 && action == "reset") request = "METERS RESET";
        else { std::cerr << "Usage: crossfeed ab store a|b; ab a|b|dry; meters reset\n"; return 1; }
        std::string response;
        if (!IpcClient::send_command(Config::get_socket_path(), request, response)) {
            std::cerr << "Crossfeed engine is not running.\n"; return 1;
        }
        std::cout << response << '\n';
        return response.find("\"error\"") == std::string::npos ? 0 : 1;
    } else if (cmd == "upgrade" || cmd == "update") {
        return cmd_update(argc, argv, false);
    } else if (cmd == "check-update" || cmd == "check") {
        return cmd_update(argc, argv, true);
    } else if (cmd == "bench") {
        run_benchmark();
        return 0;
    } else if (cmd == "--version" || cmd == "-v") {
        std::cout << "pipewire-crossfeed 2.7.0 (standalone)\n";
        return 0;
    } else if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        print_help(argv[0]);
        return 0;
    } else {
        std::cerr << "Unknown command: " << cmd << "\n\n";
        print_help(argv[0]);
        return 1;
    }
}
