// Exercise the actual GTK UI in its disconnected preview mode. Including this
// implementation keeps test-only widget access out of the public GUI API.
#include "../src/gui.cpp"
#include <cassert>
#include <iostream>
#include <filesystem>

static void settle() {
    for (int i = 0; i < 20; ++i) {
        while (gtk_events_pending()) gtk_main_iteration();
        g_usleep(10000);
    }
}

static void screenshot(crossfeed::CrossfeedGuiApp& app, const std::string& path) {
    settle();
    const int width = gtk_widget_get_allocated_width(app.window);
    const int height = gtk_widget_get_allocated_height(app.window);
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t* cr = cairo_create(surface);
    gtk_widget_draw(app.window, cr);
    assert(cairo_surface_write_to_png(surface, path.c_str()) == CAIRO_STATUS_SUCCESS);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);
    const std::string destination = argc > 1 ? argv[1] : "/tmp/crossfeed-gui-preview";
    std::filesystem::create_directories(destination);
    crossfeed::CrossfeedGuiApp app(true);
    app.build_ui();
    settle();
    assert(app.state.running && app.preview_mode && !app.indicator);
    assert(!app.poll_timeout_id);
    assert(std::string(gtk_stack_get_visible_child_name(GTK_STACK(app.tuning_stack))) == "tune");
    const auto initial_sound = app.current_sound();
    for (const char* page : {"spatial", "presets", "saved", "tune"}) {
        gtk_stack_set_visible_child_name(GTK_STACK(app.tuning_stack), page);
        settle();
        assert(crossfeed::CrossfeedDSP::same_sound(initial_sound, app.current_sound()));
    }
    gtk_stack_set_visible_child_name(GTK_STACK(app.tuning_stack), "presets");
    settle();
    // Shelf previews are visual only, for pointer and keyboard users alike.
    auto* candidate = app.preset_buttons[1].second;
    crossfeed::CrossfeedGuiApp::on_preset_enter(candidate, nullptr, &app);
    assert(app.response_graph.has_ghost);
    assert(app.response_graph.ghost_params.level_db == crossfeed::g_presets[1].level_db);
    assert(crossfeed::CrossfeedDSP::same_sound(initial_sound, app.current_sound()));
    screenshot(app, destination + "/preset-shelf.png");
    crossfeed::CrossfeedGuiApp::on_preset_leave(candidate, nullptr, &app);
    assert(!app.response_graph.has_ghost);
    crossfeed::CrossfeedGuiApp::on_preset_focus(candidate, nullptr, &app);
    assert(app.response_graph.has_ghost);
    crossfeed::CrossfeedGuiApp::on_preset_blur(candidate, nullptr, &app);
    assert(!app.response_graph.has_ghost);
    gtk_switch_set_active(GTK_SWITCH(app.effects_switch), FALSE);
    crossfeed::CrossfeedGuiApp::on_preset_enter(candidate, nullptr, &app);
    assert(app.response_graph.ghost_params.advanced_effects); // Applying a factory preset selects spatial mode.
    assert(!app.state.advanced_effects);
    crossfeed::CrossfeedGuiApp::on_preset_leave(candidate, nullptr, &app);
    gtk_switch_set_active(GTK_SWITCH(app.effects_switch), TRUE);
    gtk_stack_set_visible_child_name(GTK_STACK(app.tuning_stack), "tune");
    settle();
    // Dragging the chart marker updates the actual crossover control and clamps
    // out-of-plot motion, with no effect from presses away from the marker.
    auto& graph = app.response_graph;
    const double plot_width = gtk_widget_get_allocated_width(graph.widget) - 74.0;
    GdkEventButton marker_press{};
    marker_press.button = 1; marker_press.y = 50;
    marker_press.x = 54 + std::log(graph.params.freq_hz / 20.0) / std::log(graph.upper_frequency / 20.0) * plot_width;
    assert(crossfeed::ResponseGraph::on_press(graph.widget, &marker_press, &graph));
    GdkEventMotion marker_motion{};
    marker_motion.x = 54 + std::log(1000.0 / 20.0) / std::log(graph.upper_frequency / 20.0) * plot_width;
    marker_motion.y = 50;
    crossfeed::ResponseGraph::on_motion(graph.widget, &marker_motion, &graph);
    assert(std::abs(app.state.freq_hz - 1000) < 1);
    marker_motion.x = 10000;
    crossfeed::ResponseGraph::on_motion(graph.widget, &marker_motion, &graph);
    assert(app.state.freq_hz == crossfeed::MAX_FREQ_HZ);
    marker_motion.x = -100;
    crossfeed::ResponseGraph::on_motion(graph.widget, &marker_motion, &graph);
    assert(app.state.freq_hz == crossfeed::MIN_FREQ_HZ);
    assert(crossfeed::ResponseGraph::on_release(graph.widget, &marker_press, &graph));
    assert(!graph.dragging_crossover);
    marker_press.x = 0;
    assert(!crossfeed::ResponseGraph::on_press(graph.widget, &marker_press, &graph));
    crossfeed::ResponseGraph::on_leave(graph.widget, nullptr, &graph);
    crossfeed::CrossfeedGuiApp::on_preset_button_clicked(nullptr, const_cast<crossfeed::CrossfeedPreset*>(&crossfeed::g_presets[0]));
    assert(!app.show_comparison && !gtk_widget_get_visible(app.listening_card));
    gtk_widget_show_all(app.window); // Reopening the window must keep tools hidden.
    assert(!gtk_widget_get_visible(app.listening_card));
    const auto before = app.current_sound();
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app.comparison_menu_item), TRUE);
    assert(app.show_comparison && gtk_widget_get_visible(app.listening_card));
    assert(crossfeed::CrossfeedDSP::same_sound(before, app.current_sound()));
    assert(before.enabled == app.state.enabled && before.preamp_db == app.state.preamp_db);
    assert(!gtk_expander_get_expanded(GTK_EXPANDER(app.compare_expander)));
    assert(!gtk_expander_get_expanded(GTK_EXPANDER(app.volume_expander)));
    assert(!gtk_widget_get_sensitive(app.library_load_button));
    // The two main choices are exclusive and invoke the actual bypass action.
    gtk_button_clicked(GTK_BUTTON(app.comparison_buttons[0]));
    assert(!app.state.enabled && !gtk_switch_get_active(GTK_SWITCH(app.master_switch)));
    assert(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.comparison_buttons[0])));
    assert(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.comparison_buttons[1])));
    gtk_button_clicked(GTK_BUTTON(app.comparison_buttons[1]));
    assert(app.state.enabled && gtk_switch_get_active(GTK_SWITCH(app.master_switch)));
    assert(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.comparison_buttons[0])));
    assert(app.response_graph.points[100].mono != app.response_graph.points[100].side);
    const double advanced_dc = crossfeed::response_at(app.response_graph.params, 0).mono_db();
    gtk_switch_set_active(GTK_SWITCH(app.effects_switch), FALSE);
    assert(!app.state.advanced_effects && !app.response_graph.params.advanced_effects);
    assert(!gtk_widget_get_sensitive(app.advanced_controls_box));
    assert(crossfeed::response_at(app.response_graph.params, 0).mono_db() > advanced_dc + 1);
    gtk_switch_set_active(GTK_SWITCH(app.effects_switch), TRUE);
    assert(app.state.advanced_effects && app.response_graph.params.advanced_effects);
    gtk_switch_set_active(GTK_SWITCH(app.master_switch), FALSE);
    assert(!app.state.enabled);
    for (const auto& p : app.response_graph.points) assert(std::abs(p.mono - app.state.center_trim_db) < .001 && std::abs(p.side - app.state.center_trim_db) < .001);
    assert(gtk_widget_get_sensitive(app.controls_box));
    gtk_switch_set_active(GTK_SWITCH(app.master_switch), TRUE);
    // User click interactions emit state-set; verify active and state stay synchronized.
    gboolean switch_handled = FALSE;
    g_signal_emit_by_name(app.master_switch, "state-set", FALSE, &switch_handled);
    assert(switch_handled && !app.state.enabled && !gtk_switch_get_active(GTK_SWITCH(app.master_switch)) && !gtk_switch_get_state(GTK_SWITCH(app.master_switch)));
    g_signal_emit_by_name(app.master_switch, "state-set", TRUE, &switch_handled);
    assert(switch_handled && app.state.enabled && gtk_switch_get_active(GTK_SWITCH(app.master_switch)) && gtk_switch_get_state(GTK_SWITCH(app.master_switch)));
    g_signal_emit_by_name(app.effects_switch, "state-set", FALSE, &switch_handled);
    assert(switch_handled && !app.state.advanced_effects && !gtk_switch_get_active(GTK_SWITCH(app.effects_switch)) && !gtk_switch_get_state(GTK_SWITCH(app.effects_switch)));
    g_signal_emit_by_name(app.effects_switch, "state-set", TRUE, &switch_handled);
    assert(switch_handled && app.state.advanced_effects && gtk_switch_get_active(GTK_SWITCH(app.effects_switch)) && gtk_switch_get_state(GTK_SWITCH(app.effects_switch)));
    gtk_adjustment_set_value(app.level_adj, -20);
    assert(app.response_graph.params.level_db == -20);
    crossfeed::CrossfeedGuiApp::on_preset_button_clicked(nullptr, const_cast<crossfeed::CrossfeedPreset*>(&crossfeed::g_presets[0]));
    assert(app.response_graph.params.level_db == -9.5f);
    app.state.sample_rate = 8000; app.update_ui();
    assert(app.response_graph.upper_frequency == 3920);
    app.state.sample_rate = 48000; app.update_ui();
    // Test hover motion & leave events on frequency response graph
    GdkEventMotion motion_ev{};
    motion_ev.type = GDK_MOTION_NOTIFY;
    motion_ev.x = 250;
    motion_ev.y = 100;
    crossfeed::ResponseGraph::on_motion(app.response_graph.widget, &motion_ev, &app.response_graph);
    assert(app.response_graph.is_hovering);
    assert(std::string(gtk_label_get_text(GTK_LABEL(app.response_summary))).find("Mono") != std::string::npos);

    GdkEventCrossing leave_ev{};
    leave_ev.type = GDK_LEAVE_NOTIFY;
    crossfeed::ResponseGraph::on_leave(app.response_graph.widget, &leave_ev, &app.response_graph);
    assert(!app.response_graph.is_hovering);
    assert(std::string(gtk_label_get_text(GTK_LABEL(app.response_summary))).find("Mono at DC") != std::string::npos);

    // Test double-click reset on slider
    gtk_adjustment_set_value(app.level_adj, -25.0);
    assert(gtk_adjustment_get_value(app.level_adj) == -25.0);
    GdkEventButton btn_ev{};
    btn_ev.type = GDK_2BUTTON_PRESS;
    gboolean btn_handled = FALSE;
    g_signal_emit_by_name(app.level_scale, "button-press-event", &btn_ev, &btn_handled);
    assert(gtk_adjustment_get_value(app.level_adj) == crossfeed::DEFAULT_LEVEL_DB);
    crossfeed::CrossfeedGuiApp::on_preset_button_clicked(nullptr, const_cast<crossfeed::CrossfeedPreset*>(&crossfeed::g_presets[0]));
    assert(app.state.level_db == -9.5f);

    // Exercise session A/B controls and user-preset recall without touching disk.
    assert(!gtk_widget_get_sensitive(app.comparison_buttons[2]));
    gtk_button_clicked(GTK_BUTTON(app.store_buttons[0]));
    assert(app.state.slots[0] && gtk_widget_get_sensitive(app.comparison_buttons[2]));
    assert(std::string(gtk_button_get_label(GTK_BUTTON(app.store_buttons[0]))) == "Replace A with current");
    gtk_adjustment_set_value(app.level_adj, -20);
    gtk_button_clicked(GTK_BUTTON(app.store_buttons[1]));
    gtk_button_clicked(GTK_BUTTON(app.comparison_buttons[2]));
    assert(app.state.level_db == -9.5f);
    assert(app.response_graph.has_ghost);
    assert(app.response_graph.points[50].phase_deg != 0.0);
    gtk_button_clicked(GTK_BUTTON(app.comparison_buttons[3]));
    assert(app.state.level_db == -20);
    assert(app.response_graph.has_ghost);
    assert(app.listening_command("AB DRY") && !app.state.enabled);
    assert(!app.response_graph.has_ghost);
    gtk_adjustment_set_value(app.preamp_adj, -6);
    assert(app.state.preamp_db == -6);
    for (const auto& point : app.response_graph.points) assert(std::abs(point.mono - (app.state.center_trim_db - 6)) < .001);
    auto custom = app.current_sound(); custom.preamp_db = -3; custom.level_db = -14;
    assert(app.save_library({{"My listening preset", custom}}));
    assert(gtk_widget_get_sensitive(app.library_load_button));
    gtk_button_clicked(GTK_BUTTON(app.library_load_button));
    assert(app.state.enabled && app.state.level_db == -14 && app.state.preamp_db == -3);
    gtk_adjustment_set_value(app.preamp_adj, 6);
    assert(app.response_graph.upper_db >= 6);
    gtk_adjustment_set_value(app.preamp_adj, 0);
    crossfeed::CrossfeedGuiApp::on_preset_button_clicked(nullptr, const_cast<crossfeed::CrossfeedPreset*>(&crossfeed::g_presets[0]));
    // The graph and copy track the same active trim without calibration.
    gtk_button_clicked(GTK_BUTTON(app.comparison_buttons[0]));
    gtk_adjustment_set_value(app.trim_adj, -2);
    for (const auto& point : app.response_graph.points) assert(std::abs(point.mono + 2) < .001);
    assert(std::string(gtk_label_get_text(GTK_LABEL(app.trim_label))).find("-2.0 dB on both") != std::string::npos);
    gtk_adjustment_set_value(app.trim_adj, -4);
    for (const auto& point : app.response_graph.points) assert(std::abs(point.mono + 4) < .001);
    gtk_switch_set_active(GTK_SWITCH(app.effects_switch), FALSE);
    for (const auto& point : app.response_graph.points) assert(std::abs(point.mono) < .001);
    crossfeed::CrossfeedGuiApp::on_preset_button_clicked(nullptr, const_cast<crossfeed::CrossfeedPreset*>(&crossfeed::g_presets[0]));
    app.preview_mode = false;
    app.state.listening.output_clip = true;
    app.update_ui();
    assert(gtk_widget_get_visible(app.clip_notice));
    assert(gtk_widget_get_sensitive(app.comparison_buttons[0]));
    assert(!gtk_expander_get_expanded(GTK_EXPANDER(app.volume_expander)));
    app.state.comparison_available = false;
    app.update_ui();
    assert(!gtk_widget_get_sensitive(app.store_buttons[0]));
    app.state.comparison_available = true;
    app.state.listening = {};
    app.preview_mode = true;
    app.update_ui();
    assert(!gtk_widget_get_visible(app.clip_notice));
    gtk_switch_set_active(GTK_SWITCH(app.master_switch), TRUE);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app.comparison_menu_item), FALSE);
    assert(!gtk_widget_get_visible(app.listening_card));
    // Output availability is separate from filter bypass and does not alter sound.
    const auto before_reconnect = app.current_sound();
    app.preview_mode = false; // Render the production status copy without any engine calls.
    app.state.output_state = "waiting";
    app.state.requested_target = "usb.dac";
    app.update_ui();
    assert(std::string(gtk_label_get_text(GTK_LABEL(app.status_dot_label))) == "Waiting for output");
    assert(std::string(gtk_header_bar_get_subtitle(GTK_HEADER_BAR(app.header_bar))) == "Waiting for output");
    assert(crossfeed::CrossfeedDSP::same_sound(before_reconnect, app.current_sound()));
    app.state.output_state = "reconnecting";
    app.update_ui();
    assert(std::string(gtk_label_get_text(GTK_LABEL(app.status_dot_label))) == "Reconnecting");
    app.preview_mode = true;
    app.state.output_state = "ready";
    app.state.requested_target = "auto";
    app.update_ui();
    assert(std::string(gtk_label_get_text(GTK_LABEL(app.status_dot_label))) == "Preview");
    // Capture the desktop theme selected by GTK_THEME; external test invocations
    // cover Adwaita light/dark, HighContrast and Breeze without changing settings.
    screenshot(app, destination + "/desktop.png");
    gtk_stack_set_visible_child_name(GTK_STACK(app.tuning_stack), "spatial");
    screenshot(app, destination + "/spatial.png");
    gtk_stack_set_visible_child_name(GTK_STACK(app.tuning_stack), "saved");
    screenshot(app, destination + "/saved.png");
    gtk_stack_set_visible_child_name(GTK_STACK(app.tuning_stack), "tune");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app.comparison_menu_item), TRUE);
    gtk_button_clicked(GTK_BUTTON(app.comparison_buttons[0]));
    gtk_adjustment_set_value(app.trim_adj, -2);
    settle();
    auto* vertical = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(app.scrolled_window));
    int x = 0, y = 0;
    assert(gtk_widget_translate_coordinates(app.listening_card, app.controls_box, 0, 0, &x, &y));
    gtk_adjustment_set_value(vertical, y);
    screenshot(app, destination + "/listening.png");
    gtk_switch_set_active(GTK_SWITCH(app.master_switch), TRUE);
    crossfeed::CrossfeedGuiApp::on_preset_button_clicked(nullptr, const_cast<crossfeed::CrossfeedPreset*>(&crossfeed::g_presets[0]));
    gtk_adjustment_set_value(vertical, 0);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app.comparison_menu_item), FALSE);
    gtk_window_resize(GTK_WINDOW(app.window), 520, 740);
    settle();
    screenshot(app, destination + "/compact.png");
    // Themes such as Breeze include sizable CSD shadow margins in the outer
    // window allocation. Check the actual content area, excluding that shadow.
    assert(gtk_widget_get_allocated_width(app.main_stack) <= 540);
    auto* scroll = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(app.scrolled_window));
    assert(gtk_adjustment_get_upper(scroll) <= gtk_adjustment_get_page_size(scroll) + 1);
    for (const char* page : {"presets", "saved", "spatial", "tune"}) {
        gtk_stack_set_visible_child_name(GTK_STACK(app.tuning_stack), page);
        settle();
        assert(gtk_widget_get_allocated_width(app.main_stack) <= 540);
        assert(gtk_adjustment_get_upper(scroll) <= gtk_adjustment_get_page_size(scroll) + 1);
    }
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app.comparison_menu_item), TRUE);
    gtk_expander_set_expanded(GTK_EXPANDER(app.compare_expander), TRUE);
    gtk_expander_set_expanded(GTK_EXPANDER(app.volume_expander), TRUE);
    settle();
    assert(gtk_widget_get_allocated_width(app.main_stack) <= 540);
    assert(gtk_adjustment_get_upper(scroll) <= gtk_adjustment_get_page_size(scroll) + 1);
    assert(gtk_widget_translate_coordinates(app.listening_card, app.controls_box, 0, 0, &x, &y));
    gtk_adjustment_set_value(vertical, y);
    screenshot(app, destination + "/comparison-options.png");
    gtk_stack_set_visible_child_name(GTK_STACK(app.tuning_stack), "spatial");
    settle();
    assert(gtk_widget_translate_coordinates(app.advanced_controls_box, app.controls_box, 0, 0, &x, &y));
    gtk_adjustment_set_value(vertical, y);
    settle();
    screenshot(app, destination + "/expanded.png");
    assert(gtk_widget_get_visible(app.advanced_controls_box));
    app.stop_engine();
    assert(std::string(gtk_stack_get_visible_child_name(GTK_STACK(app.main_stack))) == "stopped");
    app.start_engine();
    assert(std::string(gtk_stack_get_visible_child_name(GTK_STACK(app.main_stack))) == "controls");
    // GUI preferences are isolated from engine state; preview toggles never save.
    char temporary[] = "/tmp/crossfeed-gui-settings-XXXXXX";
    assert(mkdtemp(temporary));
    const char* previous_config = std::getenv("XDG_CONFIG_HOME");
    const std::string previous_path = previous_config ? previous_config : "";
    const bool had_config = previous_config != nullptr;
    setenv("XDG_CONFIG_HOME", temporary, 1);
    assert(!app.load_comparison_preference());
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app.comparison_menu_item), FALSE);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app.comparison_menu_item), TRUE);
    assert(!std::filesystem::exists(crossfeed::Config::get_config_dir()));
    assert(app.save_comparison_preference(true) && app.load_comparison_preference());
    assert(crossfeed::Config::save_state(crossfeed::ConfigState{}));
    assert(app.load_comparison_preference());
    assert(app.save_comparison_preference(false) && !app.load_comparison_preference());
    if (had_config) setenv("XDG_CONFIG_HOME", previous_path.c_str(), 1);
    else unsetenv("XDG_CONFIG_HOME");
    std::filesystem::remove_all(temporary);
    gtk_widget_destroy(app.window);
    app.window = nullptr;
    std::cout << "PASS: GTK workspace navigation, visual-only preset previews, graph dragging, effects/bypass, presets, rate bounds, compact layout and stopped view.\n";
}
