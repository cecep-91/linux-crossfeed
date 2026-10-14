#include "gui.hpp"
#include "ipc.hpp"
#include "config.hpp"
#include "dsp.hpp"
#include "presets.hpp"
#include "gui_response.hpp"
#include "user_presets.hpp"
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <libayatana-appindicator/app-indicator.h>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <cstring>
#include <unistd.h>
#include <limits.h>
#include <thread>

#if !GLIB_CHECK_VERSION(2, 74, 0)
#ifndef G_APPLICATION_DEFAULT_FLAGS
#define G_APPLICATION_DEFAULT_FLAGS G_APPLICATION_FLAGS_NONE
#endif
#endif

namespace crossfeed {


struct GuiEngineState {
    bool running = false;
    bool enabled = true;
    bool advanced_effects = true;
    float level_db = DEFAULT_LEVEL_DB;
    float freq_hz = DEFAULT_FREQ_HZ;
    float delay_us = DEFAULT_DELAY_US;
    float phase_apf_hz = DEFAULT_PHASE_APF_HZ;
    float center_trim_db = DEFAULT_CENTER_TRIM_DB;
    float shadow_hz = DEFAULT_SHADOW_HZ;
    float preamp_db = 0;
    bool comparison_available = true;
    bool slots[2] = {false, false};
    bool slot_active[2] = {false, false};
    ListeningStatus listening;
    std::string backend = "pipewire";
    std::string target = "";
    uint32_t sample_rate = 48000;
    uint32_t buffer_frames = 256;
};

class CrossfeedGuiApp {
public:
    static CrossfeedGuiApp* instance;

    GtkWidget* window = nullptr;
    GtkWidget* header_bar = nullptr;
    GtkWidget* master_switch = nullptr;
    GtkWidget* enable_box = nullptr;
    GtkWidget* main_stack = nullptr;
    GtkWidget* scrolled_window = nullptr;
    GtkWidget* controls_box = nullptr;
    GtkWidget* stopped_box = nullptr;

    // Primary Sliders
    GtkAdjustment* level_adj = nullptr;
    GtkWidget* level_scale = nullptr;
    GtkWidget* level_spin = nullptr;

    GtkAdjustment* freq_adj = nullptr;
    GtkWidget* freq_scale = nullptr;
    GtkWidget* freq_spin = nullptr;

    // Spatial effects controls
    GtkWidget* expander = nullptr;
    GtkWidget* effects_switch = nullptr;
    GtkWidget* effects_desc_label = nullptr;
    GtkWidget* advanced_controls_box = nullptr;
    GtkWidget* preset_desc_label = nullptr;

    GtkAdjustment* delay_adj = nullptr;
    GtkWidget* delay_scale = nullptr;
    GtkWidget* delay_spin = nullptr;

    GtkAdjustment* phase_adj = nullptr;
    GtkWidget* phase_scale = nullptr;
    GtkWidget* phase_spin = nullptr;

    GtkAdjustment* trim_adj = nullptr;
    GtkWidget* trim_scale = nullptr;
    GtkWidget* trim_spin = nullptr;

    GtkAdjustment* shadow_adj = nullptr;
    GtkWidget* shadow_scale = nullptr;
    GtkWidget* shadow_spin = nullptr;

    // Status display widgets
    GtkWidget* status_dot_label = nullptr;
    GtkWidget* preset_val_label = nullptr;
    GtkWidget* backend_val_label = nullptr;
    GtkWidget* target_val_label = nullptr;
    GtkWidget* latency_val_label = nullptr;
    GtkApplication* g_app = nullptr;

    // Header menu and presets tracking
    GtkWidget* menu_button = nullptr;
    std::vector<std::pair<const CrossfeedPreset*, GtkWidget*>> preset_buttons;
    guint slider_ipc_timeout_id = 0;

    // Tray Indicator
    AppIndicator* indicator = nullptr;
    GtkWidget* tray_menu = nullptr;
    GtkWidget* tray_status_item = nullptr;
    GtkWidget* tray_toggle_item = nullptr;
    GtkWidget* tray_effects_item = nullptr;
    GtkWidget* tray_engine_item = nullptr;

    ResponseGraph response_graph;
    CrossfeedDSP response_model; // GUI-owned; never shared with the engine.
    GtkWidget* response_caption = nullptr;
    GtkWidget* response_summary = nullptr;
    guint poll_timeout_id = 0;
    bool preview_mode = false;
    GuiEngineState state;
    bool suppress_events = false;

#include "gui_listening.inc"

    explicit CrossfeedGuiApp(bool preview = false) : preview_mode(preview) {
        instance = this;
        if (!preview) show_comparison = load_comparison_preference();
        std::error_code library_ec;
        if (!preview && std::filesystem::exists(UserPresets::path(), library_ec)) {
            UserPresets::load(UserPresets::path(), user_presets, library_error);
            for (const auto& preset : user_presets) if (!g_utf8_validate(preset.name.c_str(), -1, nullptr)) {
                user_presets.clear(); library_error = "Preset library contains an invalid UTF-8 name."; break;
            }
        }
        if (preview) {
            const auto& p = g_presets[0];
            state.running = true;
            state.level_db = p.level_db; state.freq_hz = p.freq_hz;
            state.delay_us = p.delay_us; state.phase_apf_hz = p.phase_apf_hz;
            state.center_trim_db = p.center_trim_db; state.shadow_hz = p.shadow_hz;
            state.target = "Preview output";
        }
    }

    ~CrossfeedGuiApp() {
        if (slider_ipc_timeout_id) g_source_remove(slider_ipc_timeout_id);
        if (poll_timeout_id) g_source_remove(poll_timeout_id);
        if (indicator) g_object_unref(indicator);
        instance = nullptr;
    }

    static std::string get_self_executable_path() {
        char buf[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len > 0) {
            buf[len] = '\0';
            return std::string(buf);
        }
        return "crossfeed";
    }

    static std::string parse_json_value(const std::string& json, const std::string& key) {
        std::string search = "\"" + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return "";
        pos = json.find(':', pos + search.length());
        if (pos == std::string::npos) return "";
        size_t start = pos + 1;
        while (start < json.size() && (json[start] == ' ' || json[start] == '\t' || json[start] == '\r' || json[start] == '\n')) start++;
        if (start >= json.size()) return "";
        if (json[start] == '\"') {
            size_t end = json.find('\"', start + 1);
            if (end == std::string::npos) return "";
            return json.substr(start + 1, end - start - 1);
        } else {
            size_t end = json.find_first_of(",}\r\n", start);
            if (end == std::string::npos) end = json.size();
            return json.substr(start, end - start);
        }
    }

    bool query_engine_state(GuiEngineState& out_state) {
        if (preview_mode) { out_state = state; return state.running; }
        std::string resp;
        if (!IpcClient::send_command(Config::get_socket_path(), "STATUS", resp)) {
            out_state.running = false;
            return false;
        }

        out_state.running = (parse_json_value(resp, "status") == "running");
        out_state.enabled = (parse_json_value(resp, "enabled") == "true");

        std::string lvl = parse_json_value(resp, "level_db");
        if (!lvl.empty()) {
            try { out_state.level_db = std::stof(lvl); } catch (...) {}
        }

        std::string frq = parse_json_value(resp, "freq_hz");
        if (!frq.empty()) {
            try { out_state.freq_hz = std::stof(frq); } catch (...) {}
        }

        std::string del = parse_json_value(resp, "delay_us");
        if (!del.empty()) {
            try { out_state.delay_us = std::stof(del); } catch (...) {}
        }

        std::string phs = parse_json_value(resp, "phase_apf_hz");
        if (!phs.empty()) {
            try { out_state.phase_apf_hz = std::stof(phs); } catch (...) {}
        }

        std::string trm = parse_json_value(resp, "center_trim_db");
        if (!trm.empty()) {
            try { out_state.center_trim_db = std::stof(trm); } catch (...) {}
        }

        std::string shd = parse_json_value(resp, "shadow_hz");
        if (!shd.empty()) {
            try { out_state.shadow_hz = std::stof(shd); } catch (...) {}
        }

        std::string adv = parse_json_value(resp, "advanced_effects");
        if (!adv.empty()) {
            out_state.advanced_effects = (adv == "true" || adv == "1");
        }

        auto number = [&](const char* key, float fallback) {
            try { const float v = std::stof(parse_json_value(resp, key)); return std::isfinite(v) ? v : fallback; }
            catch (...) { return fallback; }
        };
        out_state.preamp_db = number("preamp_db", 0);
        out_state.comparison_available = parse_json_value(resp, "bypass_follows_trim") == "true";
        out_state.slots[0] = parse_json_value(resp, "slot_a") == "true";
        out_state.slots[1] = parse_json_value(resp, "slot_b") == "true";
        out_state.slot_active[0] = parse_json_value(resp, "slot_a_active") == "true";
        out_state.slot_active[1] = parse_json_value(resp, "slot_b_active") == "true";
        out_state.listening.input_peak = number("input_peak", 0);
        out_state.listening.output_peak = number("output_peak", 0);
        out_state.listening.input_hold = number("input_hold", 0);
        out_state.listening.output_hold = number("output_hold", 0);
        out_state.listening.input_clip = parse_json_value(resp, "input_clip") == "true";
        out_state.listening.output_clip = parse_json_value(resp, "output_clip") == "true";

        out_state.backend = parse_json_value(resp, "backend");
        out_state.target = parse_json_value(resp, "target");

        std::string sr = parse_json_value(resp, "sample_rate");
        if (!sr.empty()) {
            try { out_state.sample_rate = std::stoul(sr); } catch (...) {}
        }

        std::string buf = parse_json_value(resp, "buffer_frames");
        if (!buf.empty()) {
            try { out_state.buffer_frames = std::stoul(buf); } catch (...) {}
        }

        return true;
    }

    static gboolean on_slider_scroll_event(GtkWidget* /*widget*/, GdkEventScroll* event, gpointer user_data) {
        GtkScrolledWindow* scrolled = GTK_SCROLLED_WINDOW(user_data);
        if (!scrolled) return GDK_EVENT_STOP;

        GtkAdjustment* vadj = gtk_scrolled_window_get_vadjustment(scrolled);
        if (!vadj) return GDK_EVENT_STOP;

        double step = gtk_adjustment_get_step_increment(vadj);
        if (step <= 1.0) step = 28.0;

        double delta = 0.0;
        if (event->direction == GDK_SCROLL_UP) {
            delta = -step * 2.5;
        } else if (event->direction == GDK_SCROLL_DOWN) {
            delta = step * 2.5;
        } else if (event->direction == GDK_SCROLL_SMOOTH) {
            double dx = 0.0, dy = 0.0;
            gdk_event_get_scroll_deltas(reinterpret_cast<GdkEvent*>(event), &dx, &dy);
            delta = dy * step * 2.5;
        }

        if (delta != 0.0) {
            double val = gtk_adjustment_get_value(vadj) + delta;
            double lower = gtk_adjustment_get_lower(vadj);
            double page_size = gtk_adjustment_get_page_size(vadj);
            double upper = gtk_adjustment_get_upper(vadj) - page_size;
            if (upper < lower) upper = lower;
            if (val < lower) val = lower;
            if (val > upper) val = upper;
            gtk_adjustment_set_value(vadj, val);
        }

        return GDK_EVENT_STOP;
    }

    static void protect_from_accidental_scroll(GtkWidget* widget, GtkWidget* scrolled_win) {
        if (!widget || !scrolled_win) return;
        gtk_widget_add_events(widget, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
        g_signal_connect(widget, "scroll-event", G_CALLBACK(on_slider_scroll_event), scrolled_win);
    }

    static std::string format_friendly_sink_name(const std::string& raw) {
        if (raw.empty()) return "Auto Device";
        if (raw == "crossfeed" || raw == "Crossfeed") return "Crossfeed Virtual Sink";

        std::string s = raw;
        if (s.rfind("alsa_output.", 0) == 0) s = s.substr(12);

        std::string lower = s;
        for (char& c : lower) c = std::tolower(static_cast<unsigned char>(c));

        if (lower.find("headphone") != std::string::npos) return "Headphones";
        if (lower.find("speaker") != std::string::npos) return "Speakers";
        if (lower.find("hdmi") != std::string::npos) return "HDMI Audio";
        if (lower.find("bluetooth") != std::string::npos || lower.find("bluez") != std::string::npos) return "Bluetooth Audio";
        if (lower.find("usb") != std::string::npos) return "USB Audio Device";

        size_t pos = s.find("HiFi__");
        if (pos != std::string::npos) {
            std::string sub = s.substr(pos + 6);
            size_t end = sub.find("__sink");
            if (end != std::string::npos) sub = sub.substr(0, end);
            return sub;
        }

        size_t end = s.find("__sink");
        if (end != std::string::npos) s = s.substr(0, end);
        return s;
    }

    static gboolean on_scale_button_press(GtkWidget* widget, GdkEventButton* event, gpointer user_data) {
        if (event->type == GDK_2BUTTON_PRESS) {
            auto* adj = GTK_ADJUSTMENT(user_data);
            gpointer p_def = g_object_get_data(G_OBJECT(widget), "crossfeed_default_val");
            if (p_def && adj) {
                float def_val = 0.0f;
                std::memcpy(&def_val, &p_def, sizeof(float));
                gtk_adjustment_set_value(adj, def_val);
                return GDK_EVENT_STOP;
            }
        }
        return GDK_EVENT_PROPAGATE;
    }

    static void setup_scale_double_click_reset(GtkWidget* scale, GtkAdjustment* adj, float default_val) {
        if (!scale || !adj) return;
        gpointer p_def = nullptr;
        std::memcpy(&p_def, &default_val, sizeof(float));
        g_object_set_data(G_OBJECT(scale), "crossfeed_default_val", p_def);
        gtk_widget_add_events(scale, GDK_BUTTON_PRESS_MASK);
        g_signal_connect(scale, "button-press-event", G_CALLBACK(on_scale_button_press), adj);
    }

    void start_engine() {
        if (preview_mode) { state.running = true; update_ui(); return; }
        std::string exe = get_self_executable_path();
        std::string cmd = "\"" + exe + "\" start";
        int res = system(cmd.c_str());
        (void)res;
        usleep(150000);
        refresh_from_engine();
    }

    void stop_engine() {
        if (preview_mode) { state.running = false; update_ui(); return; }
        std::string resp;
        IpcClient::send_command(Config::get_socket_path(), "STOP", resp);
        usleep(100000);
        refresh_from_engine();
    }

    void restart_engine() {
        stop_engine();
        usleep(200000);
        start_engine();
    }

    void send_current_params_to_engine() {
        if (preview_mode) return;
        bool en = master_switch ? gtk_switch_get_active(GTK_SWITCH(master_switch)) : state.enabled;
        float lvl = static_cast<float>(gtk_adjustment_get_value(level_adj));
        float frq = static_cast<float>(gtk_adjustment_get_value(freq_adj));
        float del = delay_adj ? static_cast<float>(gtk_adjustment_get_value(delay_adj)) : state.delay_us;
        float phs = phase_adj ? static_cast<float>(gtk_adjustment_get_value(phase_adj)) : state.phase_apf_hz;
        float trm = trim_adj ? static_cast<float>(gtk_adjustment_get_value(trim_adj)) : state.center_trim_db;
        float shd = shadow_adj ? static_cast<float>(gtk_adjustment_get_value(shadow_adj)) : state.shadow_hz;
        bool adv = effects_switch ? gtk_switch_get_active(GTK_SWITCH(effects_switch)) : state.advanced_effects;

        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1);
        ss << "SET enabled=" << (en ? "1" : "0")
           << " level=" << lvl
           << std::setprecision(0)
           << " freq=" << frq
           << std::setprecision(1)
           << " delay=" << del
           << std::setprecision(0)
           << " phase=" << phs
           << std::setprecision(1)
           << " trim=" << trm
           << std::setprecision(0)
           << " shadow=" << shd
           << " advanced=" << (adv ? "1" : "0")
           << std::setprecision(2) << " preamp=" << state.preamp_db;
        std::string resp;
        if (!preview_mode) IpcClient::send_command(Config::get_socket_path(), ss.str(), resp);
    }

    static gboolean on_slider_debounce_timeout(gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        app->slider_ipc_timeout_id = 0;
        app->send_current_params_to_engine();
        return G_SOURCE_REMOVE;
    }

    void update_preset_and_subtitle() {
        const auto* active_p = detect_active_preset(
            state.level_db, state.freq_hz, state.delay_us,
            state.phase_apf_hz, state.center_trim_db, state.shadow_hz,
            state.advanced_effects
        );

        for (auto& item : preset_buttons) {
            if (active_p && item.first == active_p) {
                gtk_style_context_add_class(gtk_widget_get_style_context(item.second), "suggested-action");
            } else {
                gtk_style_context_remove_class(gtk_widget_get_style_context(item.second), "suggested-action");
            }
        }

        if (preset_desc_label) {
            if (active_p) {
                gtk_label_set_text(GTK_LABEL(preset_desc_label), active_p->description);
            } else if (!state.advanced_effects) {
                gtk_label_set_text(GTK_LABEL(preset_desc_label), "Direct shelf and low-pass crossfeed. Spatial effects are off.");
            } else {
                gtk_label_set_text(GTK_LABEL(preset_desc_label), "Custom acoustic parameters");
            }
        }

        if (preset_val_label) {
            gtk_label_set_text(GTK_LABEL(preset_val_label), active_p ? active_p->name : "Custom");
        }

        std::ostringstream sub;
        sub << std::fixed << std::setprecision(1);
        if (state.enabled) {
            std::string preset_badge = active_p ? std::string(active_p->name) : "Custom";
            sub << preset_badge << " (" << state.level_db << " dB, "
                << std::setprecision(0) << state.freq_hz << " Hz)";
            if (!state.target.empty()) {
                sub << " · " << format_friendly_sink_name(state.target);
            }
        } else {
            sub << "Original audio · crossfeed off";
            if (!state.target.empty()) {
                sub << " · " << format_friendly_sink_name(state.target);
            }
        }
        gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header_bar), preview_mode ? "Preview · audio is disconnected" : sub.str().c_str());
    }

    void apply_params(bool enabled, float level_db, float freq_hz,
                      float delay_us, float phase_apf_hz, float center_trim_db,
                      float shadow_hz, bool advanced_effects) {
        if (slider_ipc_timeout_id != 0) {
            g_source_remove(slider_ipc_timeout_id);
            slider_ipc_timeout_id = 0;
        }

        state.enabled = enabled;
        state.level_db = level_db;
        state.freq_hz = freq_hz;
        state.delay_us = delay_us;
        state.phase_apf_hz = phase_apf_hz;
        state.center_trim_db = center_trim_db;
        state.shadow_hz = shadow_hz;
        state.advanced_effects = advanced_effects;

        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1);
        ss << "SET enabled=" << (enabled ? "1" : "0")
           << " level=" << level_db
           << std::setprecision(0)
           << " freq=" << freq_hz
           << std::setprecision(1)
           << " delay=" << delay_us
           << std::setprecision(0)
           << " phase=" << phase_apf_hz
           << std::setprecision(1)
           << " trim=" << center_trim_db
           << std::setprecision(0)
           << " shadow=" << shadow_hz
           << " advanced=" << (advanced_effects ? "1" : "0")
           << std::setprecision(2) << " preamp=" << state.preamp_db;
        std::string resp;
        if (!preview_mode) IpcClient::send_command(Config::get_socket_path(), ss.str(), resp);
        update_ui();
    }

    void reset_to_defaults() {
        apply_params(true, DEFAULT_LEVEL_DB, DEFAULT_FREQ_HZ, DEFAULT_DELAY_US,
                     DEFAULT_PHASE_APF_HZ, DEFAULT_CENTER_TRIM_DB, DEFAULT_SHADOW_HZ, true);
    }

    void toggle_enabled() {
        if (preview_mode) { state.enabled = !state.enabled; update_ui(); return; }
        if (slider_ipc_timeout_id != 0) {
            g_source_remove(slider_ipc_timeout_id);
            slider_ipc_timeout_id = 0;
        }
        std::string resp;
        IpcClient::send_command(Config::get_socket_path(), "TOGGLE", resp);
        refresh_from_engine();
    }

    void update_ui() {
        suppress_events = true;

        if (!state.running) {
            gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header_bar), "Engine Stopped");
            gtk_widget_hide(enable_box);
            gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "stopped");

            if (indicator) {
                app_indicator_set_icon_full(indicator, "audio-volume-muted", "Crossfeed stopped");
            }
            if (tray_status_item) {
                gtk_menu_item_set_label(GTK_MENU_ITEM(tray_status_item), "Crossfeed: Stopped");
            }
            if (tray_engine_item) {
                gtk_menu_item_set_label(GTK_MENU_ITEM(tray_engine_item), "Start Engine");
            }
            if (tray_toggle_item) {
                gtk_widget_set_sensitive(tray_toggle_item, FALSE);
            }
        } else {
            gtk_widget_show(enable_box);
            if (master_switch) {
                if (gtk_switch_get_active(GTK_SWITCH(master_switch)) != state.enabled) {
                    gtk_switch_set_active(GTK_SWITCH(master_switch), state.enabled);
                }
                if (gtk_switch_get_state(GTK_SWITCH(master_switch)) != state.enabled) {
                    gtk_switch_set_state(GTK_SWITCH(master_switch), state.enabled);
                }
            }
            gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "controls");

            // Update primary sliders
            if (std::fabs(gtk_adjustment_get_value(level_adj) - state.level_db) > 0.05) {
                gtk_adjustment_set_value(level_adj, state.level_db);
            }
            if (std::fabs(gtk_adjustment_get_value(freq_adj) - state.freq_hz) > 1.0) {
                gtk_adjustment_set_value(freq_adj, state.freq_hz);
            }

            // Update spatial controls
            if (delay_adj && std::fabs(gtk_adjustment_get_value(delay_adj) - state.delay_us) > 1.0) {
                gtk_adjustment_set_value(delay_adj, state.delay_us);
            }
            if (phase_adj && std::fabs(gtk_adjustment_get_value(phase_adj) - state.phase_apf_hz) > 5.0) {
                gtk_adjustment_set_value(phase_adj, state.phase_apf_hz);
            }
            if (trim_adj && std::fabs(gtk_adjustment_get_value(trim_adj) - state.center_trim_db) > 0.05) {
                gtk_adjustment_set_value(trim_adj, state.center_trim_db);
            }
            if (shadow_adj && std::fabs(gtk_adjustment_get_value(shadow_adj) - state.shadow_hz) > 10.0) {
                gtk_adjustment_set_value(shadow_adj, state.shadow_hz);
            }

            // Update mode switch & advanced widgets sensitivity
            if (effects_switch) {
                if (gtk_switch_get_active(GTK_SWITCH(effects_switch)) != state.advanced_effects) {
                    gtk_switch_set_active(GTK_SWITCH(effects_switch), state.advanced_effects);
                }
                if (gtk_switch_get_state(GTK_SWITCH(effects_switch)) != state.advanced_effects) {
                    gtk_switch_set_state(GTK_SWITCH(effects_switch), state.advanced_effects);
                }
            }
            if (advanced_controls_box) {
                gtk_widget_set_sensitive(advanced_controls_box, state.advanced_effects);
            }
            if (effects_desc_label) {
                if (state.advanced_effects) {
                    gtk_label_set_text(GTK_LABEL(effects_desc_label), "Delay, phase rotation, head shadow and output trim.");
                } else {
                    gtk_label_set_text(GTK_LABEL(effects_desc_label), "Pure crossfeed · direct shelf and low-pass blend only.");
                }
            }

            // Keep controls and engine actions usable while comparing bypass.
            update_response();

            // Update preset indicator, buttons highlight, and header subtitle
            update_preset_and_subtitle();

            // Details box
            if (status_dot_label) {
                if (state.enabled) {
                    gtk_label_set_text(GTK_LABEL(status_dot_label), preview_mode ? "Preview" : "Active");
                } else {
                    gtk_label_set_text(GTK_LABEL(status_dot_label), "Original audio");
                }
            }
            if (backend_val_label) {
                std::string bk = state.backend;
                if (bk == "pipewire") bk = "PipeWire";
                else if (bk == "pulse") bk = "PulseAudio";
                else if (bk == "alsa") bk = "ALSA";
                gtk_label_set_text(GTK_LABEL(backend_val_label), bk.c_str());
            }
            if (target_val_label) {
                std::string friendly = format_friendly_sink_name(state.target);
                gtk_label_set_text(GTK_LABEL(target_val_label), friendly.c_str());
                gtk_widget_set_tooltip_text(target_val_label, state.target.empty() ? "Auto Default Physical Sink" : state.target.c_str());
            }
            if (latency_val_label) {
                float ms = (state.sample_rate > 0) ? (static_cast<float>(state.buffer_frames) * 1000.0f / state.sample_rate) : 5.3f;
                std::ostringstream lat;
                lat << state.buffer_frames << " frames (" << std::fixed << std::setprecision(1) << ms << " ms @ " << (state.sample_rate / 1000.0) << " kHz)";
                gtk_label_set_text(GTK_LABEL(latency_val_label), lat.str().c_str());
                gtk_widget_set_tooltip_text(latency_val_label, "Audio block duration. This does not include the complete graph, device, or transport latency.");
            }

            // Tray indicator
            if (indicator) {
                if (state.enabled) {
                    app_indicator_set_icon_full(indicator, "audio-headphones", "Crossfeed active");
                } else {
                    app_indicator_set_icon_full(indicator, "audio-volume-muted", "Crossfeed bypassed");
                }
            }
            if (tray_status_item) {
                std::ostringstream t_stat;
                t_stat << "Crossfeed: " << (state.enabled ? "Active (" : "Bypassed (")
                       << std::fixed << std::setprecision(1) << state.level_db << " dB, "
                       << std::setprecision(0) << state.freq_hz << " Hz, "
                       << (state.advanced_effects ? "All Effects)" : "Pure Crossfeed)");
                gtk_menu_item_set_label(GTK_MENU_ITEM(tray_status_item), t_stat.str().c_str());
            }
            if (tray_engine_item) {
                gtk_menu_item_set_label(GTK_MENU_ITEM(tray_engine_item), "Stop Engine");
            }
            if (tray_toggle_item) {
                gtk_widget_set_sensitive(tray_toggle_item, TRUE);
                gtk_menu_item_set_label(GTK_MENU_ITEM(tray_toggle_item), state.enabled ? "Bypass Filter" : "Enable Filter");
            }
            if (tray_effects_item) {
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(tray_effects_item), state.advanced_effects);
                gtk_widget_set_sensitive(tray_effects_item, state.enabled);
            }
        }

        update_listening();
        suppress_events = false;
    }

    void refresh_from_engine() {
        query_engine_state(state);
        update_ui();
    }

    // Callbacks
    static gboolean on_poll_timer(gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        if (!app->suppress_events && app->slider_ipc_timeout_id == 0) {
            GuiEngineState new_st;
            app->query_engine_state(new_st);
            app->state = new_st;
            app->update_ui();
        }
        return TRUE;
    }

    static gboolean on_window_delete(GtkWidget* widget, GdkEvent* /*event*/, gpointer data) {
        auto* app = static_cast<CrossfeedGuiApp*>(data);
        if (app->preview_mode) { on_app_quit(nullptr, app); return TRUE; }
        // HIDE TO SYSTEM TRAY on window close! Do not exit!
        gtk_widget_hide(widget);
        return TRUE;
    }

    static gboolean on_window_key_press(GtkWidget* /*widget*/, GdkEventKey* event, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        if (!app) return GDK_EVENT_PROPAGATE;

        GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(app->window));

        guint state = event->state & gtk_accelerator_get_default_mod_mask();

        // Escape: Hide to tray
        if (event->keyval == GDK_KEY_Escape && state == 0) {
            if (app->preview_mode) on_app_quit(nullptr, app);
            else gtk_widget_hide(app->window);
            return GDK_EVENT_STOP;
        }

        // Ctrl+W: Hide to tray
        if ((event->keyval == GDK_KEY_w || event->keyval == GDK_KEY_W) && (state & GDK_CONTROL_MASK)) {
            if (app->preview_mode) on_app_quit(nullptr, app);
            else gtk_widget_hide(app->window);
            return GDK_EVENT_STOP;
        }

        // Ctrl+Q: Quit application
        if ((event->keyval == GDK_KEY_q || event->keyval == GDK_KEY_Q) && (state & GDK_CONTROL_MASK)) {
            on_app_quit(nullptr, app);
            return GDK_EVENT_STOP;
        }

        // Ctrl+R: Reset to defaults
        if ((event->keyval == GDK_KEY_r || event->keyval == GDK_KEY_R) && (state & GDK_CONTROL_MASK)) {
            app->reset_to_defaults();
            return GDK_EVENT_STOP;
        }

        // Space or Ctrl+B: Toggle bypass (instant A/B testing)
        if (((event->keyval == GDK_KEY_b || event->keyval == GDK_KEY_B) && (state & GDK_CONTROL_MASK)) ||
            (event->keyval == GDK_KEY_space && state == 0 && !focus)) {
            app->toggle_enabled();
            return GDK_EVENT_STOP;
        }

        // Ctrl+1 .. Ctrl+5: Select Presets
        if ((state & GDK_CONTROL_MASK) && event->keyval >= GDK_KEY_1 && event->keyval <= GDK_KEY_5) {
            size_t idx = event->keyval - GDK_KEY_1;
            if (idx < (sizeof(g_presets) / sizeof(g_presets[0]))) {
                const auto& p = g_presets[idx];
                bool en = app->master_switch ? gtk_switch_get_active(GTK_SWITCH(app->master_switch)) : app->state.enabled;
                app->apply_params(en, p.level_db, p.freq_hz, p.delay_us, p.phase_apf_hz, p.center_trim_db, p.shadow_hz, true);
                return GDK_EVENT_STOP;
            }
        }

        return GDK_EVENT_PROPAGATE;
    }

    static gboolean on_switch_state_set(GtkSwitch* sw, gboolean state, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        if (app->suppress_events) return FALSE;
        gtk_switch_set_state(sw, state);
        if (app->state.enabled != state) {
            app->apply_current_ui_params(state);
        }
        return TRUE;
    }

    static void on_switch_changed(GObject* object, GParamSpec*, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        if (app->suppress_events) return;
        gboolean val = gtk_switch_get_active(GTK_SWITCH(object));
        if (app->state.enabled != val) {
            gtk_switch_set_state(GTK_SWITCH(object), val);
            app->apply_current_ui_params(val);
        }
    }

    static gboolean on_effects_state_set(GtkSwitch* sw, gboolean state, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        if (app->suppress_events) return FALSE;
        gtk_switch_set_state(sw, state);
        if (app->state.advanced_effects != state) {
            app->state.advanced_effects = state;
            app->apply_current_ui_params(app->state.enabled);
        }
        return TRUE;
    }

    static void on_effects_changed(GObject* object, GParamSpec*, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        if (app->suppress_events) return;
        gboolean val = gtk_switch_get_active(GTK_SWITCH(object));
        if (app->state.advanced_effects != val) {
            gtk_switch_set_state(GTK_SWITCH(object), val);
            app->state.advanced_effects = val;
            app->apply_current_ui_params(app->state.enabled);
        }
    }

    void apply_current_ui_params(bool en) {
        float lvl = static_cast<float>(gtk_adjustment_get_value(level_adj));
        float frq = static_cast<float>(gtk_adjustment_get_value(freq_adj));
        float del = delay_adj ? static_cast<float>(gtk_adjustment_get_value(delay_adj)) : state.delay_us;
        float phs = phase_adj ? static_cast<float>(gtk_adjustment_get_value(phase_adj)) : state.phase_apf_hz;
        float trm = trim_adj ? static_cast<float>(gtk_adjustment_get_value(trim_adj)) : state.center_trim_db;
        float shd = shadow_adj ? static_cast<float>(gtk_adjustment_get_value(shadow_adj)) : state.shadow_hz;
        bool adv = state.advanced_effects;
        apply_params(en, lvl, frq, del, phs, trm, shd, adv);
    }

    static void on_any_slider_changed(GtkAdjustment* /*adj*/, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        if (app->suppress_events) return;

        // Update the local readouts and cached response immediately.
        app->state.level_db = static_cast<float>(gtk_adjustment_get_value(app->level_adj));
        app->state.freq_hz = static_cast<float>(gtk_adjustment_get_value(app->freq_adj));
        if (app->delay_adj) app->state.delay_us = static_cast<float>(gtk_adjustment_get_value(app->delay_adj));
        if (app->phase_adj) app->state.phase_apf_hz = static_cast<float>(gtk_adjustment_get_value(app->phase_adj));
        if (app->trim_adj) app->state.center_trim_db = static_cast<float>(gtk_adjustment_get_value(app->trim_adj));
        if (app->shadow_adj) app->state.shadow_hz = static_cast<float>(gtk_adjustment_get_value(app->shadow_adj));

        app->update_preset_and_subtitle();
        app->update_response();
        app->suppress_events = true;
        app->update_listening();
        app->suppress_events = false;

        // Debounce IPC write: 30ms
        if (app->slider_ipc_timeout_id == 0) {
            app->slider_ipc_timeout_id = g_timeout_add(30, on_slider_debounce_timeout, app);
        }
    }

    static void on_preset_button_clicked(GtkWidget* /*button*/, gpointer user_data) {
        auto* app = instance;
        auto* preset = static_cast<const CrossfeedPreset*>(user_data);
        bool en = app->master_switch ? gtk_switch_get_active(GTK_SWITCH(app->master_switch)) : app->state.enabled;
        app->apply_params(en, preset->level_db, preset->freq_hz,
                          preset->delay_us, preset->phase_apf_hz,
                          preset->center_trim_db, preset->shadow_hz,
                          true);
    }

    static void on_reset_defaults_clicked(GtkWidget* /*widget*/, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        app->reset_to_defaults();
    }

    struct UpdateCheckData {
        CrossfeedGuiApp* app = nullptr;
        std::string current_ver;
        std::string latest_ver;
        std::string release_url;
        bool has_update = false;
        bool success = false;
        int commits_behind = 0;
    };

    void run_gui_upgrade() {
        if (preview_mode) return;
        GtkWidget* prog_dlg = gtk_message_dialog_new(
            window ? GTK_WINDOW(window) : nullptr,
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_INFO,
            GTK_BUTTONS_NONE,
            "Upgrading Crossfeed..."
        );
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(prog_dlg),
            "Downloading and installing the latest release. Please wait a moment..."
        );
        gtk_widget_show_all(prog_dlg);

        std::thread([this, prog_dlg]() {
            std::string exe = get_self_executable_path();
            std::string cmd = "\"" + exe + "\" upgrade";
            int ret = system(cmd.c_str());
            bool ok = (WIFEXITED(ret) && WEXITSTATUS(ret) == 0);

            struct UpgradeResult {
                CrossfeedGuiApp* app;
                GtkWidget* prog_dlg;
                bool ok;
            };

            auto* res = new UpgradeResult{this, prog_dlg, ok};
            g_idle_add([](gpointer p) -> gboolean {
                auto* r = static_cast<UpgradeResult*>(p);
                gtk_widget_destroy(r->prog_dlg);

                if (r->ok) {
                    GtkWidget* ok_dlg = gtk_message_dialog_new(
                        r->app->window ? GTK_WINDOW(r->app->window) : nullptr,
                        GTK_DIALOG_MODAL,
                        GTK_MESSAGE_INFO,
                        GTK_BUTTONS_OK,
                        "Upgrade Successful!"
                    );
                    gtk_message_dialog_format_secondary_text(
                        GTK_MESSAGE_DIALOG(ok_dlg),
                        "Crossfeed has been upgraded to the latest version and the audio engine has been refreshed."
                    );
                    g_signal_connect(ok_dlg, "response", G_CALLBACK(gtk_widget_destroy), nullptr);
                    gtk_widget_show_all(ok_dlg);
                    r->app->refresh_from_engine();
                } else {
                    GtkWidget* err_dlg = gtk_message_dialog_new(
                        r->app->window ? GTK_WINDOW(r->app->window) : nullptr,
                        GTK_DIALOG_MODAL,
                        GTK_MESSAGE_ERROR,
                        GTK_BUTTONS_OK,
                        "Upgrade Failed"
                    );
                    gtk_message_dialog_format_secondary_text(
                        GTK_MESSAGE_DIALOG(err_dlg),
                        "An error occurred during upgrade. Run 'crossfeed upgrade' in your terminal to view detailed logs."
                    );
                    g_signal_connect(err_dlg, "response", G_CALLBACK(gtk_widget_destroy), nullptr);
                    gtk_widget_show_all(err_dlg);
                }

                delete r;
                return G_SOURCE_REMOVE;
            }, res);
        }).detach();
    }

    static void on_check_updates_clicked(GtkWidget* /*widget*/, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        if (!app || app->preview_mode) return;

        std::thread([app]() {
            std::string exe = get_self_executable_path();
            std::string cmd = "\"" + exe + "\" check-update --json 2>/dev/null";

            std::string json_output;
            FILE* fp = popen(cmd.c_str(), "r");
            if (fp) {
                char buf[512];
                while (fgets(buf, sizeof(buf), fp)) {
                    json_output += buf;
                }
                pclose(fp);
            }

            auto* data = new UpdateCheckData();
            data->app = app;
            if (!json_output.empty() && json_output.find("\"error\"") == std::string::npos) {
                data->success = true;
                data->current_ver = parse_json_value(json_output, "current");
                data->latest_ver = parse_json_value(json_output, "latest");
                data->release_url = parse_json_value(json_output, "release_url");
                data->has_update = (parse_json_value(json_output, "has_update") == "true");
                std::string behind = parse_json_value(json_output, "git_commits_behind");
                if (!behind.empty()) {
                    try { data->commits_behind = std::stoi(behind); } catch (...) {}
                }
            } else {
                data->success = false;
            }

            g_idle_add([](gpointer p) -> gboolean {
                auto* d = static_cast<UpdateCheckData*>(p);
                CrossfeedGuiApp* a = d->app;

                if (!d->success) {
                    GtkWidget* msg = gtk_message_dialog_new(
                        a->window ? GTK_WINDOW(a->window) : nullptr,
                        GTK_DIALOG_MODAL,
                        GTK_MESSAGE_WARNING,
                        GTK_BUTTONS_OK,
                        "Unable to Check for Updates"
                    );
                    gtk_message_dialog_format_secondary_text(
                        GTK_MESSAGE_DIALOG(msg),
                        "Could not connect to GitHub. Please verify your internet connection or visit https://github.com/cecep-91/linux-crossfeed/releases."
                    );
                    g_signal_connect(msg, "response", G_CALLBACK(gtk_widget_destroy), nullptr);
                    gtk_widget_show_all(msg);
                } else if (d->has_update) {
                    GtkWidget* dlg = gtk_dialog_new_with_buttons(
                        "Crossfeed Update Available",
                        a->window ? GTK_WINDOW(a->window) : nullptr,
                        GTK_DIALOG_MODAL,
                        "Cancel", GTK_RESPONSE_CANCEL,
                        "View Release", GTK_RESPONSE_HELP,
                        "Upgrade Now", GTK_RESPONSE_APPLY,
                        nullptr
                    );
                    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_APPLY);

                    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
                    gtk_container_set_border_width(GTK_CONTAINER(content), 16);
                    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

                    GtkWidget* title = gtk_label_new(nullptr);
                    std::string title_str = "<span size='large' weight='bold'>A new version of Crossfeed is available!</span>";
                    gtk_label_set_markup(GTK_LABEL(title), title_str.c_str());
                    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
                    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 0);

                    std::ostringstream desc;
                    desc << "Current version: <b>" << d->current_ver << "</b>\n"
                         << "Latest release:  <b>v" << d->latest_ver << "</b>";
                    if (d->commits_behind > 0) {
                        desc << "\n(" << d->commits_behind << " newer commit(s) on git branch)";
                    }
                    desc << "\n\nWould you like to upgrade now? The audio engine will be automatically updated and refreshed.";

                    GtkWidget* body = gtk_label_new(nullptr);
                    gtk_label_set_markup(GTK_LABEL(body), desc.str().c_str());
                    gtk_label_set_xalign(GTK_LABEL(body), 0.0);
                    gtk_box_pack_start(GTK_BOX(vbox), body, FALSE, FALSE, 0);

                    gtk_box_pack_start(GTK_BOX(content), vbox, TRUE, TRUE, 0);
                    gtk_widget_show_all(dlg);

                    gint res = gtk_dialog_run(GTK_DIALOG(dlg));
                    if (res == GTK_RESPONSE_HELP) {
                        std::string url = d->release_url.empty() ? "https://github.com/cecep-91/linux-crossfeed/releases" : d->release_url;
                        gtk_show_uri_on_window(a->window ? GTK_WINDOW(a->window) : nullptr, url.c_str(), GDK_CURRENT_TIME, nullptr);
                        gtk_widget_destroy(dlg);
                    } else if (res == GTK_RESPONSE_APPLY) {
                        gtk_widget_destroy(dlg);
                        a->run_gui_upgrade();
                    } else {
                        gtk_widget_destroy(dlg);
                    }
                } else {
                    GtkWidget* msg = gtk_message_dialog_new(
                        a->window ? GTK_WINDOW(a->window) : nullptr,
                        GTK_DIALOG_MODAL,
                        GTK_MESSAGE_INFO,
                        GTK_BUTTONS_OK,
                        "Crossfeed is Up to Date"
                    );
                    std::string sec = "You are running Crossfeed version " + d->current_ver + ", which is the latest available release.";
                    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(msg), "%s", sec.c_str());
                    g_signal_connect(msg, "response", G_CALLBACK(gtk_widget_destroy), nullptr);
                    gtk_widget_show_all(msg);
                }

                delete d;
                return G_SOURCE_REMOVE;
            }, data);
        }).detach();
    }

    static void on_about_clicked(GtkWidget* /*widget*/, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        GtkWidget* dialog = gtk_about_dialog_new();
        gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(app->window));
        gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
        gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog), "PipeWire Crossfeed");
        gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), "2.6.1");
        gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog),
            "Standalone ultra-low-latency headphone crossfeed audio processor.\n"
            "Reduces headphone listening fatigue and spatializes stereo imaging.\n\n"
            "Keyboard Shortcuts:\n"
            "• Ctrl+B: Toggle bypass (A/B test)\n"
            "• Ctrl+1 .. Ctrl+5: Select Presets\n"
            "• Ctrl+R: Reset to Defaults\n"
            "• Escape / Ctrl+W: Hide window to tray\n"
            "• Ctrl+Q: Quit Crossfeed\n\n"
            "Easy Upgrades:\n"
            "• Menu > Check for Updates... (in-app one-click update)\n"
            "• Terminal: 'crossfeed upgrade'");
        gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog), "https://github.com/cecep-91/linux-crossfeed");
        gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(dialog), "GitHub Repository");
        gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(dialog), GTK_LICENSE_MIT_X11);
        gtk_about_dialog_set_logo_icon_name(GTK_ABOUT_DIALOG(dialog), "audio-headphones");

        const char* authors[] = {
            "Jan Meier (Corda natural crossfeed filter)",
            "Chu Moy (HeadWize analog crossfeed)",
            "Siegfried Linkwitz (Acoustic crossfeed research)",
            "Bauer (Stereophonic-to-Binaural BS2B DSP)",
            "PipeWire Crossfeed contributors",
            nullptr
        };
        gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dialog), authors);

        g_signal_connect(dialog, "response", G_CALLBACK(gtk_widget_destroy), nullptr);
        gtk_widget_show_all(dialog);
    }

    static void on_start_engine_clicked(GtkWidget* /*button*/, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        app->start_engine();
    }

    static void on_restart_engine_clicked(GtkWidget* /*button*/, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        app->restart_engine();
    }

    static void on_stop_engine_clicked(GtkWidget* /*button*/, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        app->stop_engine();
    }

    static void on_tray_show_window(GtkWidget* /*item*/, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        if (app->window) {
            gtk_widget_show_all(app->window);
            gtk_window_present(GTK_WINDOW(app->window));
            app->refresh_from_engine();
        }
    }

    static void on_tray_toggle(GtkWidget* /*item*/, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        app->toggle_enabled();
    }

    static void on_tray_toggle_effects(GtkCheckMenuItem* item, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        if (app->suppress_events) return;
        gboolean active = gtk_check_menu_item_get_active(item);
        app->state.advanced_effects = active;
        bool en = app->master_switch ? gtk_switch_get_active(GTK_SWITCH(app->master_switch)) : app->state.enabled;
        app->apply_current_ui_params(en);
    }

    static void on_tray_engine_action(GtkWidget* /*item*/, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        if (app->state.running) {
            app->stop_engine();
        } else {
            app->start_engine();
        }
    }

    static void on_app_quit(GtkWidget* /*item*/, gpointer user_data) {
        auto* app = static_cast<CrossfeedGuiApp*>(user_data);
        if (app->slider_ipc_timeout_id != 0) {
            g_source_remove(app->slider_ipc_timeout_id);
            app->slider_ipc_timeout_id = 0;
        }
        app->stop_engine();
        if (app->g_app) {
            g_application_release(G_APPLICATION(app->g_app));
            g_application_quit(G_APPLICATION(app->g_app));
        } else {
            gtk_main_quit();
        }
    }

    void build_tray() {
        indicator = app_indicator_new("pipewire-crossfeed", "audio-headphones", APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
        app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
        app_indicator_set_title(indicator, "Crossfeed");

        tray_menu = gtk_menu_new();

        tray_status_item = gtk_menu_item_new_with_label("Crossfeed: Initializing...");
        gtk_widget_set_sensitive(tray_status_item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(tray_menu), tray_status_item);

        GtkWidget* sep1 = gtk_separator_menu_item_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(tray_menu), sep1);

        GtkWidget* item_show = gtk_menu_item_new_with_label("Show Window");
        g_signal_connect(item_show, "activate", G_CALLBACK(on_tray_show_window), this);
        gtk_menu_shell_append(GTK_MENU_SHELL(tray_menu), item_show);

        tray_toggle_item = gtk_menu_item_new_with_label("Toggle Bypass");
        g_signal_connect(tray_toggle_item, "activate", G_CALLBACK(on_tray_toggle), this);
        gtk_menu_shell_append(GTK_MENU_SHELL(tray_menu), tray_toggle_item);

        tray_effects_item = gtk_check_menu_item_new_with_label("All Spatial Effects");
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(tray_effects_item), TRUE);
        g_signal_connect(tray_effects_item, "toggled", G_CALLBACK(on_tray_toggle_effects), this);
        gtk_menu_shell_append(GTK_MENU_SHELL(tray_menu), tray_effects_item);

        // Presets submenu
        GtkWidget* item_presets = gtk_menu_item_new_with_label("Presets");
        GtkWidget* presets_menu = gtk_menu_new();
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(item_presets), presets_menu);

        for (const auto& preset : g_presets) {
            std::ostringstream ss;
            ss << preset.name << " (" << std::fixed << std::setprecision(0) << preset.freq_hz << " Hz, "
               << std::setprecision(1) << preset.level_db << " dB, "
               << std::setprecision(0) << preset.delay_us << " µs)";
            GtkWidget* it = gtk_menu_item_new_with_label(ss.str().c_str());
            g_signal_connect(it, "activate", G_CALLBACK(on_preset_button_clicked), const_cast<CrossfeedPreset*>(&preset));
            gtk_menu_shell_append(GTK_MENU_SHELL(presets_menu), it);
        }

        gtk_menu_shell_append(GTK_MENU_SHELL(tray_menu), item_presets);

        GtkWidget* item_updates = gtk_menu_item_new_with_label("Check for Updates...");
        g_signal_connect(item_updates, "activate", G_CALLBACK(on_check_updates_clicked), this);
        gtk_menu_shell_append(GTK_MENU_SHELL(tray_menu), item_updates);

        GtkWidget* sep2 = gtk_separator_menu_item_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(tray_menu), sep2);

        tray_engine_item = gtk_menu_item_new_with_label("Stop Engine");
        g_signal_connect(tray_engine_item, "activate", G_CALLBACK(on_tray_engine_action), this);
        gtk_menu_shell_append(GTK_MENU_SHELL(tray_menu), tray_engine_item);

        GtkWidget* sep3 = gtk_separator_menu_item_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(tray_menu), sep3);

        GtkWidget* item_quit = gtk_menu_item_new_with_label("Quit Crossfeed");
        g_signal_connect(item_quit, "activate", G_CALLBACK(on_app_quit), this);
        gtk_menu_shell_append(GTK_MENU_SHELL(tray_menu), item_quit);

        gtk_widget_show_all(tray_menu);
        app_indicator_set_menu(indicator, GTK_MENU(tray_menu));
    }

    static void add_class(GtkWidget* widget, const char* name) {
        gtk_style_context_add_class(gtk_widget_get_style_context(widget), name);
    }

    static GtkWidget* make_label(const char* text, const char* style = nullptr) {
        GtkWidget* label = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
        if (style) add_class(label, style);
        return label;
    }

    static GtkWidget* make_card() {
        GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        add_class(card, "cf-card");
        return card;
    }

    static void install_style() {
        // Keep native controls, fonts, titlebar buttons and desktop colors.
        // Scoped selectors avoid styling dialogs or unrelated GTK applications.
        static const char* css = R"CSS(
            .crossfeed-window .cf-card {
                background-color: @theme_base_color;
                border: 1px solid alpha(@theme_fg_color, 0.12);
                border-radius: 12px;
                padding: 18px;
            }
            .crossfeed-window .cf-title { font-size: 1.6em; font-weight: bold; }
            .crossfeed-window .cf-heading { font-size: 1.05em; font-weight: bold; }
            .crossfeed-window .cf-caption { font-size: 0.9em; }
            .crossfeed-window .cf-pill {
                border-radius: 20px;
                padding: 5px 12px;
                background-color: alpha(@theme_selected_bg_color, 0.14);
                font-weight: bold;
            }
            .crossfeed-window .cf-presets button { padding: 8px 12px; border-radius: 8px; }
            .crossfeed-window .cf-control spinbutton { font-weight: bold; }
            .crossfeed-window .cf-control scale { margin: 2px 0; }
            .crossfeed-window .cf-summary { font-size: 0.9em; }
        )CSS";
        GtkCssProvider* provider = gtk_css_provider_new();
        gtk_css_provider_load_from_data(provider, css, -1, nullptr);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(provider);
    }

    static GtkWidget* responsive_grid() {
        GtkWidget* flow = gtk_flow_box_new();
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
        gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
        gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 1);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 2);
        gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 24);
        gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 12);
        g_signal_connect(flow, "size-allocate", G_CALLBACK(+[](GtkWidget* widget, GtkAllocation* allocation, gpointer) {
            const guint columns = allocation->width < 660 ? 1 : 2;
            if (gtk_flow_box_get_max_children_per_line(GTK_FLOW_BOX(widget)) != columns)
                gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(widget), columns);
        }), nullptr);
        return flow;
    }

    GtkWidget* add_control(const char* title, const char* description, const char* unit,
                          double initial, double lower, double upper, double step, int digits,
                          GtkAdjustment*& adj, GtkWidget*& scale, GtkWidget*& spin) {
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        add_class(box, "cf-control");
        gtk_widget_set_size_request(box, 275, -1);
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget* label = make_label(title, "cf-heading");
        gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
        adj = gtk_adjustment_new(initial, lower, upper, step, step * 5, 0);
        spin = gtk_spin_button_new(adj, step, digits);
        gtk_entry_set_width_chars(GTK_ENTRY(spin), 5);
        atk_object_set_name(gtk_widget_get_accessible(spin), title);
        gtk_box_pack_end(GTK_BOX(row), make_label(unit, "dim-label"), FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(row), spin, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), make_label(description, "dim-label"), FALSE, FALSE, 0);
        scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, adj);
        gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
        gtk_scale_set_digits(GTK_SCALE(scale), digits);
        atk_object_set_name(gtk_widget_get_accessible(scale), title);
        std::string tip = std::string(description) + " Double-click the slider to reset.";
        gtk_widget_set_tooltip_text(scale, tip.c_str());
        setup_scale_double_click_reset(scale, adj, initial);
        protect_from_accidental_scroll(scale, scrolled_window);
        protect_from_accidental_scroll(spin, scrolled_window);
        gtk_box_pack_start(GTK_BOX(box), scale, FALSE, FALSE, 0);
        g_signal_connect(adj, "value-changed", G_CALLBACK(on_any_slider_changed), this);
        return box;
    }

    void update_response() {
        if (!response_graph.widget) return;
        if (!CrossfeedDSP::valid_sample_rate(state.sample_rate)) {
            gtk_widget_hide(response_graph.widget);
            gtk_label_set_text(GTK_LABEL(response_caption), "Unsupported sample rate");
            gtk_label_set_text(GTK_LABEL(response_summary), "The engine passes audio through at this rate.");
            return;
        }
        gtk_widget_show(response_graph.widget);
        response_model.set_all_params(state.sample_rate, state.level_db, state.freq_hz,
            state.delay_us, state.phase_apf_hz, state.center_trim_db, state.shadow_hz,
            state.advanced_effects, state.enabled);
        response_model.set_preamp_db(state.preamp_db);
        auto graph_params = response_model.get_params();
        if (!state.comparison_available && !state.enabled) graph_params.trim_linear = 1;
        response_graph.update(graph_params);
        char caption[80];
        std::snprintf(caption, sizeof(caption), "%s · %.1f kHz", state.enabled ? "Calculated" : "Bypassed", state.sample_rate / 1000.0);
        gtk_label_set_text(GTK_LABEL(response_caption), caption);
        const auto dc = response_at(response_graph.params, 0);
        char summary[160];
        std::snprintf(summary, sizeof(summary), "Mono at DC  %+.2f dB     ·     Side at DC  %+.2f dB     ·     Hover for values", dc.mono_db(), dc.side_db());
        gtk_label_set_text(GTK_LABEL(response_summary), summary);
    }

    void build_ui(GtkApplication* app_param = nullptr) {
        g_app = app_param;
        if (g_app) {
            window = gtk_application_window_new(g_app);
            g_application_hold(G_APPLICATION(g_app));
        } else {
            window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        }
        gtk_window_set_title(GTK_WINDOW(window), "Crossfeed");
        add_class(window, "crossfeed-window");
        install_style();
        gtk_window_set_default_size(GTK_WINDOW(window), 920, 900);
        gtk_window_set_resizable(GTK_WINDOW(window), TRUE);
        gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
        gtk_window_set_icon_name(GTK_WINDOW(window), "audio-headphones");

        // Intercept delete-event to hide window to tray instead of quitting
        g_signal_connect(window, "delete-event", G_CALLBACK(on_window_delete), this);
        g_signal_connect(window, "key-press-event", G_CALLBACK(on_window_key_press), this);

        // Header bar
        header_bar = gtk_header_bar_new();
        gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header_bar), TRUE);
        gtk_header_bar_set_title(GTK_HEADER_BAR(header_bar), "Crossfeed");
        gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header_bar), "Initializing...");
        gtk_window_set_titlebar(GTK_WINDOW(window), header_bar);

        // Header bar menu button (☰)
        menu_button = gtk_menu_button_new();
        GtkWidget* menu_icon = gtk_image_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_button_set_image(GTK_BUTTON(menu_button), menu_icon);
        gtk_widget_set_tooltip_text(menu_button, "Menu");

        GtkWidget* app_menu = gtk_menu_new();

        GtkWidget* m_reset = gtk_menu_item_new_with_label("Reset to Defaults (Ctrl+R)");
        g_signal_connect(m_reset, "activate", G_CALLBACK(on_reset_defaults_clicked), this);
        gtk_menu_shell_append(GTK_MENU_SHELL(app_menu), m_reset);

        GtkWidget* m_presets_item = gtk_menu_item_new_with_label("Presets");
        GtkWidget* m_presets_menu = gtk_menu_new();
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(m_presets_item), m_presets_menu);
        for (size_t i = 0; i < (sizeof(g_presets) / sizeof(g_presets[0])); ++i) {
            const auto& preset = g_presets[i];
            std::ostringstream ss;
            ss << preset.name << " (Ctrl+" << (i + 1) << ") - "
               << std::fixed << std::setprecision(0) << preset.freq_hz << " Hz, "
               << std::setprecision(1) << preset.level_db << " dB";
            GtkWidget* it = gtk_menu_item_new_with_label(ss.str().c_str());
            g_signal_connect(it, "activate", G_CALLBACK(on_preset_button_clicked), const_cast<CrossfeedPreset*>(&preset));
            gtk_menu_shell_append(GTK_MENU_SHELL(m_presets_menu), it);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(app_menu), m_presets_item);

        comparison_menu_item = gtk_check_menu_item_new_with_label("Show comparison tools");
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(comparison_menu_item), show_comparison);
        g_signal_connect(comparison_menu_item, "toggled", G_CALLBACK(+[](GtkCheckMenuItem* item, gpointer data) {
            auto* app = static_cast<CrossfeedGuiApp*>(data);
            app->show_comparison = gtk_check_menu_item_get_active(item);
            gtk_widget_set_visible(app->listening_card, app->show_comparison);
            if (!app->preview_mode && !save_comparison_preference(app->show_comparison))
                app->show_listening_error("Could not save this display preference. It will apply until the window is closed.");
        }), this);
        gtk_menu_shell_append(GTK_MENU_SHELL(app_menu), comparison_menu_item);

        gtk_menu_shell_append(GTK_MENU_SHELL(app_menu), gtk_separator_menu_item_new());

        GtkWidget* m_update_item = gtk_menu_item_new_with_label("Check for Updates...");
        g_signal_connect(m_update_item, "activate", G_CALLBACK(on_check_updates_clicked), this);
        gtk_menu_shell_append(GTK_MENU_SHELL(app_menu), m_update_item);

        GtkWidget* m_about = gtk_menu_item_new_with_label("About Crossfeed");
        g_signal_connect(m_about, "activate", G_CALLBACK(on_about_clicked), this);
        gtk_menu_shell_append(GTK_MENU_SHELL(app_menu), m_about);

        gtk_menu_shell_append(GTK_MENU_SHELL(app_menu), gtk_separator_menu_item_new());

        GtkWidget* m_quit = gtk_menu_item_new_with_label("Quit Crossfeed (Ctrl+Q)");
        g_signal_connect(m_quit, "activate", G_CALLBACK(on_app_quit), this);
        gtk_menu_shell_append(GTK_MENU_SHELL(app_menu), m_quit);

        gtk_widget_show_all(app_menu);
        gtk_menu_button_set_popup(GTK_MENU_BUTTON(menu_button), app_menu);

        gtk_header_bar_pack_end(GTK_HEADER_BAR(header_bar), menu_button);

        // Master bypass switch in header bar
        master_switch = gtk_switch_new();
        gtk_widget_set_valign(master_switch, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(master_switch, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(master_switch, "Enable crossfeed or hear the original audio (Ctrl+B). Both use the same active output trim and output volume.");
        g_signal_connect(master_switch, "state-set", G_CALLBACK(on_switch_state_set), this);
        g_signal_connect(master_switch, "notify::active", G_CALLBACK(on_switch_changed), this);
        atk_object_set_name(gtk_widget_get_accessible(master_switch), "Enable crossfeed");
        enable_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(enable_box), make_label("Crossfeed", "dim-label"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(enable_box), master_switch, FALSE, FALSE, 0);
        gtk_header_bar_pack_start(GTK_HEADER_BAR(header_bar), enable_box);

        // Stack container for running vs stopped views
        main_stack = gtk_stack_new();
        gtk_stack_set_transition_type(GTK_STACK(main_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
        gtk_container_add(GTK_CONTAINER(window), main_stack);

        // A scrollable, theme-aware dashboard. Flow boxes collapse to one column
        // on small windows; native controls retain keyboard and theme behavior.
        scrolled_window = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        controls_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
        gtk_container_set_border_width(GTK_CONTAINER(controls_box), 22);
        gtk_container_add(GTK_CONTAINER(scrolled_window), controls_box);

        GtkWidget* intro = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
        GtkWidget* intro_icon = gtk_image_new_from_icon_name("audio-headphones-symbolic", GTK_ICON_SIZE_DIALOG);
        gtk_box_pack_start(GTK_BOX(intro), intro_icon, FALSE, FALSE, 0);
        GtkWidget* intro_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_box_pack_start(GTK_BOX(intro_text), make_label("Headphone crossfeed", "cf-title"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(intro_text), make_label("A little of each channel in the opposite ear.", "dim-label"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(intro), intro_text, TRUE, TRUE, 0);
        status_dot_label = make_label("Active", "cf-pill");
        gtk_widget_set_valign(status_dot_label, GTK_ALIGN_CENTER);
        gtk_box_pack_end(GTK_BOX(intro), status_dot_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(controls_box), intro, FALSE, FALSE, 2);

        GtkWidget* presets = make_card();
        GtkWidget* preset_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_box_pack_start(GTK_BOX(preset_header), make_label("Listening presets", "cf-heading"), TRUE, TRUE, 0);
        GtkWidget* reset = gtk_button_new_from_icon_name("edit-undo-symbolic", GTK_ICON_SIZE_BUTTON);
        add_class(reset, "flat");
        gtk_widget_set_tooltip_text(reset, "Reset all settings (Ctrl+R)");
        atk_object_set_name(gtk_widget_get_accessible(reset), "Reset all settings");
        g_signal_connect(reset, "clicked", G_CALLBACK(on_reset_defaults_clicked), this);
        gtk_box_pack_end(GTK_BOX(preset_header), reset, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(presets), preset_header, FALSE, FALSE, 0);
        GtkWidget* preset_grid = gtk_flow_box_new();
        add_class(preset_grid, "cf-presets");
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(preset_grid), GTK_SELECTION_NONE);
        gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(preset_grid), TRUE);
        gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(preset_grid), 1);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(preset_grid), 5);
        gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(preset_grid), 5);
        gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(preset_grid), 5);
        for (const auto& preset : g_presets) {
            GtkWidget* button = gtk_button_new_with_label(preset.name);
            gtk_widget_set_tooltip_text(button, preset.description);
            g_signal_connect(button, "clicked", G_CALLBACK(on_preset_button_clicked), const_cast<CrossfeedPreset*>(&preset));
            gtk_container_add(GTK_CONTAINER(preset_grid), button);
            preset_buttons.emplace_back(&preset, button);
        }
        gtk_box_pack_start(GTK_BOX(presets), preset_grid, FALSE, FALSE, 0);
        preset_desc_label = make_label("", "dim-label");
        add_class(preset_desc_label, "cf-caption");
        gtk_box_pack_start(GTK_BOX(presets), preset_desc_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(presets), build_library(), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(controls_box), presets, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(controls_box), build_listening(), FALSE, FALSE, 0);

        GtkWidget* graph_card = make_card();
        GtkWidget* graph_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_box_pack_start(GTK_BOX(graph_header), make_label("Frequency response", "cf-heading"), TRUE, TRUE, 0);
        response_caption = make_label("Calculated · 48 kHz", "dim-label");
        add_class(response_caption, "cf-caption");
        gtk_box_pack_end(GTK_BOX(graph_header), response_caption, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(graph_card), graph_header, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(graph_card), response_graph.create(), FALSE, FALSE, 0);
        response_summary = make_label("", "dim-label");
        add_class(response_summary, "cf-summary");
        gtk_box_pack_start(GTK_BOX(graph_card), response_summary, FALSE, FALSE, 0);
        gtk_widget_set_tooltip_text(graph_card, "Steady-state frequency response of the filter, not a live audio spectrum. Mono = direct + crossfeed; side = direct − crossfeed. Includes phase, fractional delay, output trim and preamp.");
        gtk_box_pack_start(GTK_BOX(controls_box), graph_card, FALSE, FALSE, 0);

        GtkWidget* mix = make_card();
        GtkWidget* mix_grid = responsive_grid();
        gtk_container_add(GTK_CONTAINER(mix_grid), add_control("Crossfeed blend", "How much reaches the opposite ear.", "dB",
            DEFAULT_LEVEL_DB, MIN_LEVEL_DB, MAX_LEVEL_DB, 0.5, 1, level_adj, level_scale, level_spin));
        gtk_container_add(GTK_CONTAINER(mix_grid), add_control("Crossover", "Upper range of the crossfeed blend.", "Hz",
            DEFAULT_FREQ_HZ, MIN_FREQ_HZ, MAX_FREQ_HZ, 10, 0, freq_adj, freq_scale, freq_spin));
        gtk_box_pack_start(GTK_BOX(mix), mix_grid, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(controls_box), mix, FALSE, FALSE, 0);

        GtkWidget* spatial = make_card();
        GtkWidget* mode_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        GtkWidget* mode_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_box_pack_start(GTK_BOX(mode_text), make_label("Spatial effects", "cf-heading"), FALSE, FALSE, 0);
        effects_desc_label = make_label("", "dim-label");
        gtk_box_pack_start(GTK_BOX(mode_text), effects_desc_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(mode_row), mode_text, TRUE, TRUE, 0);
        effects_switch = gtk_switch_new();
        gtk_widget_set_valign(effects_switch, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(effects_switch, GTK_ALIGN_CENTER);
        atk_object_set_name(gtk_widget_get_accessible(effects_switch), "Enable spatial effects");
        gtk_widget_set_tooltip_text(effects_switch, "Off: pure crossfeed. On: add delay, phase rotation, head shadow and output trim.");
        g_signal_connect(effects_switch, "state-set", G_CALLBACK(on_effects_state_set), this);
        g_signal_connect(effects_switch, "notify::active", G_CALLBACK(on_effects_changed), this);
        gtk_box_pack_end(GTK_BOX(mode_row), effects_switch, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(spatial), mode_row, FALSE, FALSE, 0);
        expander = gtk_expander_new("Fine-tune spatial effects");
        advanced_controls_box = responsive_grid();
        gtk_widget_set_margin_top(advanced_controls_box, 16);
        gtk_container_add(GTK_CONTAINER(advanced_controls_box), add_control("Cross-ear delay", "Additional delay in the crossfeed path.", "µs",
            DEFAULT_DELAY_US, MIN_DELAY_US, MAX_DELAY_US, 10, 0, delay_adj, delay_scale, delay_spin));
        gtk_container_add(GTK_CONTAINER(advanced_controls_box), add_control("Phase rotation", "All-pass transition frequency.", "Hz",
            DEFAULT_PHASE_APF_HZ, MIN_PHASE_APF_HZ, MAX_PHASE_APF_HZ, 50, 0, phase_adj, phase_scale, phase_spin));
        gtk_container_add(GTK_CONTAINER(advanced_controls_box), add_control("Output trim", "Attenuation applied to the entire mix.", "dB",
            DEFAULT_CENTER_TRIM_DB, MIN_CENTER_TRIM_DB, MAX_CENTER_TRIM_DB, 0.5, 1, trim_adj, trim_scale, trim_spin));
        gtk_container_add(GTK_CONTAINER(advanced_controls_box), add_control("Head shadow", "Extra low-pass on the crossfeed path.", "Hz",
            DEFAULT_SHADOW_HZ, MIN_SHADOW_HZ, MAX_SHADOW_HZ, 100, 0, shadow_adj, shadow_scale, shadow_spin));
        gtk_container_add(GTK_CONTAINER(expander), advanced_controls_box);
        gtk_box_pack_start(GTK_BOX(spatial), expander, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(controls_box), spatial, FALSE, FALSE, 0);

        GtkWidget* details = make_card();
        GtkWidget* detail_grid = responsive_grid();
        GtkWidget* output_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        gtk_box_pack_start(GTK_BOX(output_box), make_label("OUTPUT", "dim-label"), FALSE, FALSE, 0);
        target_val_label = make_label("", "cf-heading");
        gtk_label_set_line_wrap(GTK_LABEL(target_val_label), FALSE);
        gtk_label_set_ellipsize(GTK_LABEL(target_val_label), PANGO_ELLIPSIZE_MIDDLE);
        gtk_box_pack_start(GTK_BOX(output_box), target_val_label, FALSE, FALSE, 0);
        backend_val_label = make_label("", "dim-label");
        gtk_box_pack_start(GTK_BOX(output_box), backend_val_label, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(detail_grid), output_box);
        GtkWidget* buffer_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        gtk_box_pack_start(GTK_BOX(buffer_box), make_label("BUFFER / QUANTUM", "dim-label"), FALSE, FALSE, 0);
        latency_val_label = make_label("", "cf-heading");
        gtk_box_pack_start(GTK_BOX(buffer_box), latency_val_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(buffer_box), make_label("Block duration, not total latency.", "dim-label"), FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(detail_grid), buffer_box);
        gtk_box_pack_start(GTK_BOX(details), detail_grid, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(controls_box), details, FALSE, FALSE, 0);

        GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget* hint = make_label(preview_mode ? "Preview only · no audio routing changes" : "Ctrl+B to compare with bypass", "dim-label");
        gtk_box_pack_start(GTK_BOX(actions), hint, TRUE, TRUE, 0);
        GtkWidget* restart = gtk_button_new_with_label("Restart");
        g_signal_connect(restart, "clicked", G_CALLBACK(on_restart_engine_clicked), this);
        gtk_box_pack_end(GTK_BOX(actions), restart, FALSE, FALSE, 0);
        GtkWidget* stop = gtk_button_new_with_label("Stop engine");
        g_signal_connect(stop, "clicked", G_CALLBACK(on_stop_engine_clicked), this);
        gtk_box_pack_end(GTK_BOX(actions), stop, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(controls_box), actions, FALSE, FALSE, 0);
        gtk_stack_add_named(GTK_STACK(main_stack), scrolled_window, "controls");

        // ================= STOPPED VIEW =================
        stopped_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
        gtk_widget_set_valign(stopped_box, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(stopped_box, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_start(stopped_box, 30);
        gtk_widget_set_margin_end(stopped_box, 30);
        gtk_widget_set_margin_top(stopped_box, 30);
        gtk_widget_set_margin_bottom(stopped_box, 30);

        GtkWidget* icon = gtk_image_new_from_icon_name("audio-headphones", GTK_ICON_SIZE_DIALOG);
        gtk_box_pack_start(GTK_BOX(stopped_box), icon, FALSE, FALSE, 0);

        GtkWidget* msg_title = gtk_label_new(nullptr);
        gtk_label_set_markup(GTK_LABEL(msg_title), "<span size='large' weight='bold'>Crossfeed Engine Stopped</span>");
        gtk_box_pack_start(GTK_BOX(stopped_box), msg_title, FALSE, FALSE, 0);

        GtkWidget* msg_sub = gtk_label_new("The standalone audio filter engine is currently inactive.\nClick below to start it.");
        gtk_label_set_justify(GTK_LABEL(msg_sub), GTK_JUSTIFY_CENTER);
        gtk_label_set_line_wrap(GTK_LABEL(msg_sub), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(msg_sub), 38);
        gtk_style_context_add_class(gtk_widget_get_style_context(msg_sub), "dim-label");
        gtk_box_pack_start(GTK_BOX(stopped_box), msg_sub, FALSE, FALSE, 0);

        GtkWidget* start_btn = gtk_button_new_with_label("Start Crossfeed Engine");
        gtk_widget_set_size_request(start_btn, 220, 42);
        gtk_style_context_add_class(gtk_widget_get_style_context(start_btn), "suggested-action");
        g_signal_connect(start_btn, "clicked", G_CALLBACK(on_start_engine_clicked), this);
        gtk_box_pack_start(GTK_BOX(stopped_box), start_btn, FALSE, FALSE, 8);

        gtk_stack_add_named(GTK_STACK(main_stack), stopped_box, "stopped");

        // Build Tray Icon and Menu
        if (!preview_mode) build_tray();

        // Ensure all constructed child widgets in window hierarchy are visible
        gtk_widget_show_all(window);

        // Initial sync: if not running, auto-start engine for instant playback
        if (!query_engine_state(state)) {
            start_engine();
        } else {
            update_ui();
        }

        // Refresh settings and meter telemetry every 200 ms.
        if (!preview_mode) poll_timeout_id = g_timeout_add(200, on_poll_timer, this);
    }
};

CrossfeedGuiApp* CrossfeedGuiApp::instance = nullptr;

static void on_app_activate(GApplication* g_app, gpointer user_data) {
    auto* app = static_cast<CrossfeedGuiApp*>(user_data);
    if (!app->window) {
        app->build_ui(GTK_APPLICATION(g_app));
    }
    gtk_widget_show_all(app->window);
    gtk_window_present(GTK_WINDOW(app->window));
    app->refresh_from_engine();
}

int run_gui(int argc, char** argv) {
    bool preview = false;
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--preview") preview = true;
    g_set_prgname("crossfeed");
    g_set_application_name("Crossfeed");

    GtkApplication* g_app = gtk_application_new(preview ? "io.github.pipewire_crossfeed.Preview" : "io.github.pipewire_crossfeed.App", G_APPLICATION_DEFAULT_FLAGS);
    CrossfeedGuiApp app(preview);
    CrossfeedGuiApp::instance = &app;

    g_signal_connect(g_app, "activate", G_CALLBACK(on_app_activate), &app);

    int status = g_application_run(G_APPLICATION(g_app), 1, argv);
    g_object_unref(g_app);
    return status;
}

} // namespace crossfeed
