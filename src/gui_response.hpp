#pragma once

#include "response.hpp"
#include <gtk/gtk.h>
#include <array>
#include <cstdio>
#include <functional>
#include <string>

namespace crossfeed {

// GTK-thread-only chart. Coefficients and curve points are cached on parameter
// changes; repainting never touches the engine or the real-time audio thread.
class ResponseGraph {
public:
    GtkWidget* widget = nullptr;
    FilterParams params;
    struct Point { double frequency, mono, side, phase_deg, delay_us; };
    std::array<Point, 321> points{};
    bool has_ghost = false;
    std::string ghost_label = "Alternate slot";
    FilterParams ghost_params;
    std::array<Point, 321> ghost_points{};
    double upper_frequency = 20000.0;
    double lower_db = -12.0;
    double upper_db = 3.0;

    // Real-time interactive cursor state
    bool is_hovering = false;
    bool dragging_crossover = false;
    std::function<void(double frequency)> crossover_callback = nullptr;
    double hover_x = -1.0;
    double hover_y = -1.0;
    std::function<void(double freq, double mono, double side, double phase, double delay)> hover_callback = nullptr;
    std::function<void()> hover_leave_callback = nullptr;

    GtkWidget* create() {
        widget = gtk_drawing_area_new();
        gtk_widget_set_size_request(widget, 280, 150);
        gtk_widget_set_hexpand(widget, TRUE);
        gtk_widget_add_events(widget, GDK_POINTER_MOTION_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
        gtk_widget_set_has_tooltip(widget, TRUE);
        atk_object_set_name(gtk_widget_get_accessible(widget), "Frequency response");
        g_signal_connect(widget, "draw", G_CALLBACK(draw), this);
        g_signal_connect(widget, "motion-notify-event", G_CALLBACK(on_motion), this);
        g_signal_connect(widget, "button-press-event", G_CALLBACK(on_press), this);
        g_signal_connect(widget, "button-release-event", G_CALLBACK(on_release), this);
        g_signal_connect(widget, "leave-notify-event", G_CALLBACK(on_leave), this);
        g_signal_connect(widget, "query-tooltip", G_CALLBACK(tooltip), this);
        return widget;
    }

    void update_ghost(const FilterParams& ghost) {
        ghost_params = ghost;
        has_ghost = true;
        for (size_t i = 0; i < ghost_points.size(); ++i) {
            const double f = 20.0 * std::pow(upper_frequency / 20.0, double(i) / (ghost_points.size() - 1));
            const auto r = response_at(ghost_params, f);
            const double d = group_delay_us(ghost_params, f);
            ghost_points[i] = {f, r.mono_db(), r.side_db(), r.phase_deg(), d};
            upper_db = std::max(upper_db, std::ceil(std::max(r.mono_db(), r.side_db()) / 3.0) * 3.0);
            lower_db = std::min(lower_db, std::floor(std::min(r.mono_db(), r.side_db()) / 6.0) * 6.0);
        }
        lower_db = std::max(-60.0, lower_db);
        if (widget) gtk_widget_queue_draw(widget);
    }

    void clear_ghost() {
        if (has_ghost) {
            has_ghost = false;
            if (widget) gtk_widget_queue_draw(widget);
        }
    }

    void update(const FilterParams& prepared) {
        params = prepared;
        upper_frequency = std::min(20000.0, double(params.sample_rate) * 0.49);
        lower_db = -12.0;
        upper_db = 3.0;
        for (size_t i = 0; i < points.size(); ++i) {
            const double f = 20.0 * std::pow(upper_frequency / 20.0, double(i) / (points.size() - 1));
            const auto r = response_at(params, f);
            const double d = group_delay_us(params, f);
            points[i] = {f, r.mono_db(), r.side_db(), r.phase_deg(), d};
            upper_db = std::max(upper_db, std::ceil(std::max(r.mono_db(), r.side_db()) / 3.0) * 3.0);
            lower_db = std::min(lower_db, std::floor(std::min(r.mono_db(), r.side_db()) / 6.0) * 6.0);
        }
        lower_db = std::max(-60.0, lower_db);
        if (widget) {
            char summary[256];
            const auto dc = response_at(params, 0);
            std::snprintf(summary, sizeof(summary),
                "Calculated steady-state response, %.1f kHz sample rate. Mono at DC: %.2f dB; side: %.2f dB. Solid line: mono, dashed line: side. %s",
                params.sample_rate / 1000.0, dc.mono_db(), dc.side_db(), params.enabled ? "" : "Filter bypassed: flat response.");
            atk_object_set_description(gtk_widget_get_accessible(widget), summary);
            gtk_widget_queue_draw(widget);
        }
    }

private:
    static void text(GtkWidget* w, cairo_t* cr, const char* value, double x, double y, double align = 0) {
        PangoLayout* layout = gtk_widget_create_pango_layout(w, value);
        PangoAttrList* attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_scale_new(0.88));
        pango_layout_set_attributes(layout, attrs);
        int width, height;
        pango_layout_get_pixel_size(layout, &width, &height);
        cairo_move_to(cr, x - width * align, y - height / 2.0);
        pango_cairo_show_layout(cr, layout);
        pango_attr_list_unref(attrs);
        g_object_unref(layout);
    }

    static gboolean draw(GtkWidget* w, cairo_t* cr, gpointer data) {
        auto& graph = *static_cast<ResponseGraph*>(data);
        const double width = gtk_widget_get_allocated_width(w);
        const double height = gtk_widget_get_allocated_height(w);
        constexpr double left = 54, right = 20, top = 28, bottom = 27;
        const double plot_width = width - left - right, plot_height = height - top - bottom;
        if (plot_width <= 0 || plot_height <= 0) return FALSE;
        GtkStyleContext* style = gtk_widget_get_style_context(w);
        GdkRGBA fg, accent, background;
        gtk_style_context_get_color(style, GTK_STATE_FLAG_NORMAL, &fg);
        if (!gtk_style_context_lookup_color(style, "theme_selected_bg_color", &accent)) accent = fg;
        // Selection backgrounds can be too dark to use as a thin line on dark
        // themes. Move toward the theme foreground until contrast reaches 3:1.
        if (gtk_style_context_lookup_color(style, "theme_base_color", &background)) {
            const auto luminance = [](const GdkRGBA& c) {
                const auto linear = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
                return 0.2126 * linear(c.red) + 0.7152 * linear(c.green) + 0.0722 * linear(c.blue);
            };
            for (int i = 0; i < 12; ++i) {
                const double a = luminance(accent), b = luminance(background);
                if ((std::max(a, b) + 0.05) / (std::min(a, b) + 0.05) >= 3.0) break;
                accent.red += 0.15 * (fg.red - accent.red);
                accent.green += 0.15 * (fg.green - accent.green);
                accent.blue += 0.15 * (fg.blue - accent.blue);
            }
        }
        const auto x = [&](double f) { return left + std::log(f / 20.0) / std::log(graph.upper_frequency / 20.0) * plot_width; };
        const auto y = [&](double db) { return top + (graph.upper_db - db) / (graph.upper_db - graph.lower_db) * plot_height; };
        const auto ink = [&](const GdkRGBA& c, double alpha) { cairo_set_source_rgba(cr, c.red, c.green, c.blue, alpha); };
        // Legend uses both line patterns and text, including in high-contrast themes.
        for (int series = 0; series < 2; ++series) {
            const double lx = left + series * 146;
            ink(series == 0 ? accent : fg, 1);
            const double dash[] = {5, 4};
            cairo_set_dash(cr, dash, series ? 2 : 0, 0);
            cairo_set_line_width(cr, 2);
            cairo_move_to(cr, lx, 10); cairo_line_to(cr, lx + 22, 10); cairo_stroke(cr);
            cairo_set_dash(cr, nullptr, 0, 0);
            ink(fg, 0.85);
            text(w, cr, series ? "Side · L − R" : "Mono · L + R", lx + 30, 10);
        }
        if (graph.has_ghost && plot_width >= 380) {
            const double lx = left + 292;
            ink(fg, 0.45);
            const double gdash[] = {3, 3};
            cairo_set_dash(cr, gdash, 2, 0);
            cairo_set_line_width(cr, 1.4);
            cairo_move_to(cr, lx, 10); cairo_line_to(cr, lx + 22, 10); cairo_stroke(cr);
            cairo_set_dash(cr, nullptr, 0, 0);
            ink(fg, 0.55);
            text(w, cr, graph.ghost_label.c_str(), lx + 30, 10);
        }
        const double db_step = graph.lower_db < -24 ? 12 : 6;
        for (double db = std::floor(graph.upper_db / db_step) * db_step; db >= graph.lower_db; db -= db_step) {
            ink(fg, db == 0 ? 0.28 : 0.10);
            cairo_set_line_width(cr, 1);
            cairo_move_to(cr, left, y(db)); cairo_line_to(cr, width - right, y(db)); cairo_stroke(cr);
            char label[32]; std::snprintf(label, sizeof(label), "%.0f dB", db);
            ink(fg, 0.7); text(w, cr, label, left - 9, y(db), 1);
        }
        const double frequencies[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
        double last_label = -100;
        for (double f : frequencies) {
            if (f > graph.upper_frequency + 0.01) continue;
            const double fx = x(f);
            ink(fg, 0.08);
            cairo_move_to(cr, fx, top); cairo_line_to(cr, fx, height - bottom); cairo_stroke(cr);
            if (fx - last_label < 44) continue;
            char label[24];
            if (f >= 1000) std::snprintf(label, sizeof(label), "%.0fk", f / 1000);
            else std::snprintf(label, sizeof(label), "%.0f", f);
            ink(fg, 0.7); text(w, cr, label, fx, height - 10, 0.5);
            last_label = fx;
        }
        cairo_save(cr);
        cairo_rectangle(cr, left, top, plot_width, plot_height); cairo_clip(cr);
        if (graph.has_ghost) {
            ink(fg, 0.22);
            cairo_set_line_width(cr, 1.3);
            const double ghost_dash_mono[] = {3, 3};
            cairo_set_dash(cr, ghost_dash_mono, 2, 0);
            bool first = true;
            for (const auto& p : graph.ghost_points) {
                if (first) cairo_move_to(cr, x(p.frequency), y(p.mono));
                else cairo_line_to(cr, x(p.frequency), y(p.mono));
                first = false;
            }
            cairo_stroke(cr);

            const double ghost_dash_side[] = {2, 4};
            cairo_set_dash(cr, ghost_dash_side, 2, 0);
            first = true;
            for (const auto& p : graph.ghost_points) {
                if (first) cairo_move_to(cr, x(p.frequency), y(p.side));
                else cairo_line_to(cr, x(p.frequency), y(p.side));
                first = false;
            }
            cairo_stroke(cr);
            cairo_set_dash(cr, nullptr, 0, 0);
        }
        // Draw side first so the solid mono trace stays visible when bypassed.
        for (int series : {1, 0}) {
            ink(series == 0 ? accent : fg, series == 0 ? 1.0 : 0.65);
            cairo_set_line_width(cr, series == 0 ? 2.5 : 1.8);
            const double dash[] = {5, 4};
            cairo_set_dash(cr, dash, series ? 2 : 0, 0);
            bool first = true;
            for (const auto& p : graph.points) {
                if (first) cairo_move_to(cr, x(p.frequency), y(series ? p.side : p.mono));
                else cairo_line_to(cr, x(p.frequency), y(series ? p.side : p.mono));
                first = false;
            }
            cairo_stroke(cr);
        }
        // Crossover handle edits the same adjustment as the native slider.
        const double cutoff_x = x(graph.params.freq_hz);
        ink(accent, graph.dragging_crossover ? 0.85 : 0.35);
        cairo_set_line_width(cr, 1);
        cairo_set_dash(cr, nullptr, 0, 0);
        cairo_move_to(cr, cutoff_x, top); cairo_line_to(cr, cutoff_x, height - bottom);
        cairo_stroke(cr);
        ink(accent, 1);
        cairo_arc(cr, cutoff_x, top + 7, graph.dragging_crossover ? 7 : 5, 0, 6.2831853);
        cairo_fill(cr);
        // Interactive hover crosshair & intersection markers
        if (graph.is_hovering && graph.hover_x >= left && graph.hover_x <= width - right) {
            const double hf = 20.0 * std::pow(graph.upper_frequency / 20.0,
                std::clamp((graph.hover_x - left) / plot_width, 0.0, 1.0));
            const auto hr = response_at(graph.params, hf);

            // Vertical cursor line
            ink(accent, 0.45);
            cairo_set_line_width(cr, 1.2);
            const double hair_dash[] = {2, 2};
            cairo_set_dash(cr, hair_dash, 2, 0);
            cairo_move_to(cr, graph.hover_x, top);
            cairo_line_to(cr, graph.hover_x, height - bottom);
            cairo_stroke(cr);
            cairo_set_dash(cr, nullptr, 0, 0);

            // Curve intersection dots
            ink(accent, 1.0);
            cairo_arc(cr, graph.hover_x, y(hr.mono_db()), 4.0, 0, 6.2831853);
            cairo_fill(cr);

            ink(fg, 0.95);
            cairo_arc(cr, graph.hover_x, y(hr.side_db()), 3.5, 0, 6.2831853);
            cairo_fill(cr);
        }
        cairo_restore(cr);

        // Floating HUD badge when hovering
        if (graph.is_hovering && graph.hover_x >= left && graph.hover_x <= width - right) {
            const double hf = 20.0 * std::pow(graph.upper_frequency / 20.0,
                std::clamp((graph.hover_x - left) / plot_width, 0.0, 1.0));
            const auto hr = response_at(graph.params, hf);
            char badge[128];
            if (graph.params.enabled) {
                std::snprintf(badge, sizeof(badge), "%.0f Hz · M %+.1f dB · S %+.1f dB · Δθ %+.0f°",
                    hf, hr.mono_db(), hr.side_db(), hr.phase_deg());
            } else {
                std::snprintf(badge, sizeof(badge), "%.0f Hz · Bypassed", hf);
            }
            const double bx = width - right;
            ink(fg, 0.90);
            text(w, cr, badge, bx, 10, 1.0);
        }
        return FALSE;
    }

public:
    static gboolean on_press(GtkWidget* w, GdkEventButton* event, gpointer data) {
        auto& graph = *static_cast<ResponseGraph*>(data);
        const double plot_width = gtk_widget_get_allocated_width(w) - 74.0;
        if (event->button != 1 || plot_width <= 0 || !graph.crossover_callback) return FALSE;
        const double cutoff_x = 54.0 + std::log(graph.params.freq_hz / 20.0) /
            std::log(graph.upper_frequency / 20.0) * plot_width;
        if (std::abs(event->x - cutoff_x) > 14 || event->y < 28 ||
            event->y > gtk_widget_get_allocated_height(w) - 27) return FALSE;
        graph.dragging_crossover = true;
        gtk_widget_queue_draw(w);
        return TRUE;
    }

    static gboolean on_release(GtkWidget* w, GdkEventButton* event, gpointer data) {
        auto& graph = *static_cast<ResponseGraph*>(data);
        if (event->button != 1 || !graph.dragging_crossover) return FALSE;
        graph.dragging_crossover = false;
        gtk_widget_queue_draw(w);
        return TRUE;
    }

    static gboolean on_motion(GtkWidget* w, GdkEventMotion* event, gpointer data) {
        auto& graph = *static_cast<ResponseGraph*>(data);
        graph.is_hovering = true;
        graph.hover_x = event->x;
        graph.hover_y = event->y;
        constexpr double left = 54, right = 20;
        const double width = gtk_widget_get_allocated_width(w);
        const double plot_width = width - left - right;
        if (graph.dragging_crossover && plot_width > 0 && graph.crossover_callback) {
            const double frequency = 20.0 * std::pow(graph.upper_frequency / 20.0,
                std::clamp((event->x - left) / plot_width, 0.0, 1.0));
            graph.crossover_callback(std::clamp(frequency, double(MIN_FREQ_HZ), double(MAX_FREQ_HZ)));
        }
        if (graph.hover_callback && plot_width > 0 && event->x >= left && event->x <= width - right) {
            const double f = 20.0 * std::pow(graph.upper_frequency / 20.0,
                std::clamp((event->x - left) / plot_width, 0.0, 1.0));
            const auto r = response_at(graph.params, f);
            const double d = group_delay_us(graph.params, f);
            graph.hover_callback(f, r.mono_db(), r.side_db(), r.phase_deg(), d);
        }
        gtk_widget_queue_draw(w);
        return FALSE;
    }

    static gboolean on_leave(GtkWidget* w, GdkEventCrossing*, gpointer data) {
        auto& graph = *static_cast<ResponseGraph*>(data);
        graph.is_hovering = false;
        graph.hover_x = -1.0;
        graph.hover_y = -1.0;
        if (graph.hover_leave_callback) {
            graph.hover_leave_callback();
        }
        gtk_widget_queue_draw(w);
        return FALSE;
    }

    static gboolean tooltip(GtkWidget* w, gint x, gint, gboolean keyboard, GtkTooltip* tip, gpointer data) {
        auto& graph = *static_cast<ResponseGraph*>(data);
        const double width = gtk_widget_get_allocated_width(w) - 74.0;
        if (keyboard || width <= 0) {
            gtk_tooltip_set_text(tip, "Calculated filter response. Solid: mono (L + R). Dashed: side (L − R). Hover over a frequency for exact values. Drag the crossover marker to adjust its frequency, or use the Crossover slider.");
        } else {
            const double f = 20.0 * std::pow(graph.upper_frequency / 20.0, std::clamp((x - 54.0) / width, 0.0, 1.0));
            const auto r = response_at(graph.params, f);
            const double d = group_delay_us(graph.params, f);
            char label[256];
            if (graph.params.enabled) {
                std::snprintf(label, sizeof(label), "%.0f Hz\nMono: %+.2f dB\nSide: %+.2f dB\nPhase Δθ: %+.1f°\nGroup delay: %.0f µs",
                              f, r.mono_db(), r.side_db(), r.phase_deg(), d);
            } else {
                std::snprintf(label, sizeof(label), "%.0f Hz\nFilter bypassed (%+.2f dB)", f, r.mono_db());
            }
            gtk_tooltip_set_text(tip, label);
        }
        return TRUE;
    }
};

} // namespace crossfeed
