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
    for (const auto& p : app.response_graph.points) assert(p.mono == 0 && p.side == 0);
    assert(gtk_widget_get_sensitive(app.controls_box));
    gtk_switch_set_active(GTK_SWITCH(app.master_switch), TRUE);
    gtk_adjustment_set_value(app.level_adj, -20);
    assert(app.response_graph.params.level_db == -20);
    crossfeed::CrossfeedGuiApp::on_preset_button_clicked(nullptr, const_cast<crossfeed::CrossfeedPreset*>(&crossfeed::g_presets[0]));
    assert(app.response_graph.params.level_db == -9.5f);
    app.state.sample_rate = 8000; app.update_ui();
    assert(app.response_graph.upper_frequency == 3920);
    app.state.sample_rate = 48000; app.update_ui();
    // Capture the desktop theme selected by GTK_THEME; external test invocations
    // cover Adwaita light/dark, HighContrast and Breeze without changing settings.
    screenshot(app, destination + "/desktop.png");
    gtk_window_resize(GTK_WINDOW(app.window), 520, 740);
    screenshot(app, destination + "/compact.png");
    assert(gtk_widget_get_allocated_width(app.window) <= 540);
    auto* scroll = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(app.scrolled_window));
    assert(gtk_adjustment_get_upper(scroll) <= gtk_adjustment_get_page_size(scroll) + 1);
    gtk_expander_set_expanded(GTK_EXPANDER(app.expander), TRUE);
    settle();
    assert(gtk_widget_get_visible(app.advanced_controls_box));
    app.stop_engine();
    assert(std::string(gtk_stack_get_visible_child_name(GTK_STACK(app.main_stack))) == "stopped");
    app.start_engine();
    assert(std::string(gtk_stack_get_visible_child_name(GTK_STACK(app.main_stack))) == "controls");
    gtk_widget_destroy(app.window);
    app.window = nullptr;
    std::cout << "PASS: GTK preview, graph updates, effects/bypass, presets, rate bounds, compact layout and stopped view.\n";
}
