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
    // Exercise session A/B controls and user-preset recall without touching disk.
    assert(!gtk_widget_get_sensitive(app.comparison_buttons[2]));
    gtk_button_clicked(GTK_BUTTON(app.store_buttons[0]));
    assert(app.state.slots[0] && gtk_widget_get_sensitive(app.comparison_buttons[2]));
    assert(std::string(gtk_button_get_label(GTK_BUTTON(app.store_buttons[0]))) == "Replace A with current");
    gtk_adjustment_set_value(app.level_adj, -20);
    gtk_button_clicked(GTK_BUTTON(app.store_buttons[1]));
    gtk_button_clicked(GTK_BUTTON(app.comparison_buttons[2]));
    assert(app.state.level_db == -9.5f);
    gtk_button_clicked(GTK_BUTTON(app.comparison_buttons[3]));
    assert(app.state.level_db == -20);
    assert(app.listening_command("AB DRY") && !app.state.enabled);
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
    // Capture the desktop theme selected by GTK_THEME; external test invocations
    // cover Adwaita light/dark, HighContrast and Breeze without changing settings.
    screenshot(app, destination + "/desktop.png");
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
    screenshot(app, destination + "/compact.png");
    // Themes such as Breeze include sizable CSD shadow margins in the outer
    // window allocation. Check the actual content area, excluding that shadow.
    assert(gtk_widget_get_allocated_width(app.main_stack) <= 540);
    auto* scroll = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(app.scrolled_window));
    assert(gtk_adjustment_get_upper(scroll) <= gtk_adjustment_get_page_size(scroll) + 1);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app.comparison_menu_item), TRUE);
    gtk_expander_set_expanded(GTK_EXPANDER(app.compare_expander), TRUE);
    gtk_expander_set_expanded(GTK_EXPANDER(app.volume_expander), TRUE);
    settle();
    assert(gtk_widget_get_allocated_width(app.main_stack) <= 540);
    assert(gtk_adjustment_get_upper(scroll) <= gtk_adjustment_get_page_size(scroll) + 1);
    assert(gtk_widget_translate_coordinates(app.listening_card, app.controls_box, 0, 0, &x, &y));
    gtk_adjustment_set_value(vertical, y);
    screenshot(app, destination + "/comparison-options.png");
    gtk_expander_set_expanded(GTK_EXPANDER(app.expander), TRUE);
    settle();
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
    std::cout << "PASS: GTK preview, graph updates, effects/bypass, presets, rate bounds, compact layout and stopped view.\n";
}
