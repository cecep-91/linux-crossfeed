    GtkWidget* trim_label = nullptr;
    GtkWidget* comparison_menu_item = nullptr;
    bool show_comparison = false;
    GtkWidget* listening_card = nullptr;
    GtkWidget* compare_expander = nullptr;
    GtkWidget* volume_expander = nullptr;
    GtkWidget* clip_notice = nullptr;
    GtkWidget* comparison_buttons[4]{};
    GtkWidget* store_buttons[2]{};
    GtkWidget* slot_labels[2]{};
    GtkWidget* input_meter = nullptr;
    GtkWidget* output_meter = nullptr;
    GtkWidget* peak_label = nullptr;
    GtkAdjustment* preamp_adj = nullptr;
    GtkWidget* preamp_scale = nullptr;
    GtkWidget* preamp_spin = nullptr;
    GtkWidget* library_combo = nullptr;
    GtkWidget* library_label = nullptr;
    GtkWidget* library_load_button = nullptr;
    std::vector<GtkWidget*> library_selection_actions;
    std::vector<UserPreset> user_presets;
    FilterParams preview_slots[2];
    std::string library_error;

    // GUI preferences live separately from engine state, which IPC may save.
    static bool load_comparison_preference() {
        GKeyFile* file = g_key_file_new();
        g_key_file_load_from_file(file, (Config::get_config_dir() + "/gui.ini").c_str(), G_KEY_FILE_NONE, nullptr);
        const bool visible = g_key_file_get_boolean(file, "Interface", "show_comparison", nullptr);
        g_key_file_unref(file);
        return visible;
    }

    static bool save_comparison_preference(bool visible) {
        if (g_mkdir_with_parents(Config::get_config_dir().c_str(), 0700) != 0) return false;
        const std::string path = Config::get_config_dir() + "/gui.ini";
        GKeyFile* file = g_key_file_new();
        g_key_file_load_from_file(file, path.c_str(), G_KEY_FILE_NONE, nullptr);
        g_key_file_set_boolean(file, "Interface", "show_comparison", visible);
        const bool saved = g_key_file_save_to_file(file, path.c_str(), nullptr);
        g_key_file_unref(file);
        return saved;
    }

    static double gain_db(float gain) { return 20 * std::log10(std::max(1e-6f, gain)); }

    FilterParams current_sound() const {
        FilterParams p;
        p.level_db = state.level_db; p.freq_hz = state.freq_hz;
        p.delay_us = state.delay_us; p.phase_apf_hz = state.phase_apf_hz;
        p.center_trim_db = state.center_trim_db; p.shadow_hz = state.shadow_hz;
        p.advanced_effects = state.advanced_effects; p.enabled = state.enabled;
        p.preamp_db = state.preamp_db; p.sample_rate = state.sample_rate;
        return p;
    }

    void show_listening_error(const std::string& error) {
        GtkWidget* dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", error.c_str());
        gtk_dialog_run(GTK_DIALOG(dialog)); gtk_widget_destroy(dialog);
    }

    bool listening_command(const std::string& command) {
        if (preview_mode) {
            if (command == "AB STORE A" || command == "AB STORE B") {
                unsigned slot = command.back() == 'A' ? 0 : 1;
                preview_slots[slot] = current_sound(); state.slots[slot] = true;
            } else if (command == "AB A" || command == "AB B") {
                unsigned slot = command.back() == 'A' ? 0 : 1;
                if (!state.slots[slot]) return false;
                const auto& p = preview_slots[slot];
                apply_params(true, p.level_db, p.freq_hz, p.delay_us, p.phase_apf_hz,
                             p.center_trim_db, p.shadow_hz, p.advanced_effects);
            } else if (command == "AB DRY" || command == "ON") {
                state.enabled = command == "ON";
            }
            update_ui(); return true;
        }
        // Flush a pending slider update before a store/recall transaction.
        if (slider_ipc_timeout_id) {
            g_source_remove(slider_ipc_timeout_id); slider_ipc_timeout_id = 0;
            send_current_params_to_engine();
        }
        std::string response;
        if (!IpcClient::send_command(Config::get_socket_path(), command, response) ||
            response.find("\"error\"") != std::string::npos) {
            show_listening_error("The engine could not apply this action. Restart an older engine after updating, and store a slot before recalling it.");
            return false;
        }
        refresh_from_engine(); return true;
    }

    GtkWidget* listening_button(const char* label, const char* command) {
        GtkWidget* button = gtk_button_new_with_label(label);
        g_object_set_data_full(G_OBJECT(button), "listening-command", g_strdup(command), g_free);
        g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer data) {
            auto* app = static_cast<CrossfeedGuiApp*>(data);
            app->listening_command(static_cast<const char*>(g_object_get_data(G_OBJECT(b), "listening-command")));
        }), this);
        return button;
    }

    void update_listening() {
        if (!trim_label) return;
        gtk_adjustment_set_value(preamp_adj, state.preamp_db);
        std::ostringstream text;
        if (!preview_mode && !state.comparison_available) text << "Restart Crossfeed after updating so original audio follows output trim.";
        else {
            if (preview_mode) text << "Preview · no audio. ";
            text << std::fixed << std::setprecision(1) << "Output trim: "
                 << (state.advanced_effects ? state.center_trim_db : 0.0f) << " dB on both options.";
            if (!state.advanced_effects) text << " Pure mode bypasses the trim stage.";
        }
        gtk_label_set_text(GTK_LABEL(trim_label), text.str().c_str());
        const bool available = preview_mode || state.comparison_available;
        for (unsigned i = 0; i < 4; ++i)
            gtk_widget_set_sensitive(comparison_buttons[i], available && (i < 2 || state.slots[i - 2]));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(comparison_buttons[state.enabled ? 1 : 0]), TRUE);
        for (unsigned i = 0; i < 2; ++i) {
            const bool active = i == (state.enabled ? 1u : 0u);
            const char* name = i == 0 ? "Original audio" : "With crossfeed";
            const std::string label = std::string(active ? "✓ " : "") + name;
            gtk_button_set_label(GTK_BUTTON(comparison_buttons[i]), label.c_str());
            GtkStyleContext* style = gtk_widget_get_style_context(comparison_buttons[i]);
            if (active) gtk_style_context_add_class(style, "suggested-action");
            else gtk_style_context_remove_class(style, "suggested-action");
            const bool selected = preview_mode ? (state.enabled && state.slots[i] && CrossfeedDSP::same_sound(current_sound(), preview_slots[i])) : state.slot_active[i];
            gtk_widget_set_sensitive(store_buttons[i], available);
            const std::string letter = i == 0 ? "A" : "B";
            gtk_button_set_label(GTK_BUTTON(store_buttons[i]),
                (state.slots[i] ? "Replace " + letter + " with current" : "Save current as " + letter).c_str());
            gtk_label_set_text(GTK_LABEL(slot_labels[i]), !state.slots[i] ? "No settings saved yet." :
                selected ? "Saved · these settings are playing." : "Saved for this session.");
            gtk_widget_set_tooltip_text(comparison_buttons[i + 2], !state.slots[i] ?
                "Choose a preset or adjust the sound, then save it here first." : "Listen with these saved settings.");
        }
        gtk_label_set_text(GTK_LABEL(clip_notice), state.listening.output_clip ?
            "Output is too loud. Open Volume & meters and lower Output volume." :
            "The incoming audio has clipped. Lower the volume in the source app.");
        gtk_widget_set_visible(clip_notice, !preview_mode && (state.listening.input_clip || state.listening.output_clip));
        gtk_level_bar_set_value(GTK_LEVEL_BAR(input_meter), (std::clamp(gain_db(state.listening.input_peak), -60.0, 0.0) + 60) / 60);
        gtk_level_bar_set_value(GTK_LEVEL_BAR(output_meter), (std::clamp(gain_db(state.listening.output_peak), -60.0, 0.0) + 60) / 60);
        char peaks[220];
        std::snprintf(peaks, sizeof(peaks), "Peak hold  In %+.1f dBFS%s   ·   Out %+.1f dBFS%s",
            gain_db(state.listening.input_hold), state.listening.input_clip ? " · CLIP" : "",
            gain_db(state.listening.output_hold), state.listening.output_clip ? " · CLIP" : "");
        gtk_label_set_text(GTK_LABEL(peak_label), preview_mode ? "Preview · no audio meters" : peaks);
    }

    GtkWidget* build_listening() {
        listening_card = make_card();
        GtkWidget* card = listening_card;
        gtk_box_pack_start(GTK_BOX(card), make_label("Hear the difference", "cf-heading"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(card), make_label("Listen with or without crossfeed. Original audio follows the selected settings’ output trim.", "dim-label"), FALSE, FALSE, 0);
        GtkWidget* choices = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_set_homogeneous(GTK_BOX(choices), TRUE);
        add_class(choices, "linked");
        const char* labels[] = {"Original audio", "With crossfeed"};
        const char* commands[] = {"AB DRY", "ON"};
        for (unsigned i = 0; i < 2; ++i) {
            GtkWidget* button = gtk_radio_button_new_with_label_from_widget(
                i == 0 ? nullptr : GTK_RADIO_BUTTON(comparison_buttons[0]), labels[i]);
            comparison_buttons[i] = button;
            gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(button), FALSE);
            atk_object_set_name(gtk_widget_get_accessible(button), labels[i]);
            gtk_widget_set_tooltip_text(button, i == 0 ?
                "Hear your music without crossfeed. The same active output trim and Output volume still apply." :
                "Hear your music with the selected crossfeed settings.");
            g_object_set_data_full(G_OBJECT(button), "listening-command", g_strdup(commands[i]), g_free);
            g_signal_connect(button, "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer data) {
                auto* app = static_cast<CrossfeedGuiApp*>(data);
                if (!app->suppress_events && gtk_toggle_button_get_active(b)) {
                    if (!app->listening_command(static_cast<const char*>(g_object_get_data(G_OBJECT(b), "listening-command"))))
                        app->update_ui(); // Restore the confirmed selection if the action failed.
                }
            }), this);
            gtk_box_pack_start(GTK_BOX(choices), button, TRUE, TRUE, 0);
        }
        gtk_box_pack_start(GTK_BOX(card), choices, FALSE, FALSE, 0);
        trim_label = make_label("", "dim-label");
        gtk_box_pack_start(GTK_BOX(card), trim_label, FALSE, FALSE, 0);

        compare_expander = gtk_expander_new("Compare two settings (optional)");
        GtkWidget* comparison = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_set_margin_top(comparison, 10);
        gtk_box_pack_start(GTK_BOX(comparison), make_label(
            "1. Choose a preset or adjust the sound, then save it as A.\n2. Try different settings and save them as B.\n3. Switch between A and B. Original audio follows the last selected setting’s trim.", "dim-label"), FALSE, FALSE, 0);
        GtkWidget* slots = responsive_grid();
        for (unsigned i = 0; i < 2; ++i) {
            GtkWidget* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
            gtk_box_pack_start(GTK_BOX(column), make_label(i == 0 ? "First setting · A" : "Second setting · B", "cf-heading"), FALSE, FALSE, 0);
            slot_labels[i] = make_label("No settings saved yet.", "dim-label");
            gtk_box_pack_start(GTK_BOX(column), slot_labels[i], FALSE, FALSE, 0);
            store_buttons[i] = listening_button(i == 0 ? "Save current as A" : "Save current as B", i == 0 ? "AB STORE A" : "AB STORE B");
            gtk_widget_set_tooltip_text(store_buttons[i], "Remember the current filter settings here. Output volume stays shared between both choices.");
            comparison_buttons[i + 2] = listening_button(i == 0 ? "Listen to A" : "Listen to B", i == 0 ? "AB A" : "AB B");
            gtk_box_pack_start(GTK_BOX(column), store_buttons[i], FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(column), comparison_buttons[i + 2], FALSE, FALSE, 0);
            gtk_container_add(GTK_CONTAINER(slots), column);
        }
        gtk_box_pack_start(GTK_BOX(comparison), slots, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(comparison), make_label("A and B last until the engine stops. Use My presets to keep a favorite for next time.", "dim-label"), FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(compare_expander), comparison);
        gtk_box_pack_start(GTK_BOX(card), compare_expander, FALSE, FALSE, 0);

        volume_expander = gtk_expander_new("Volume & meters");
        GtkWidget* volume = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_set_margin_top(volume, 10);
        GtkWidget* meter_row = responsive_grid();
        auto meter = [&](const char* label, GtkWidget*& bar) {
            GtkWidget* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
            gtk_box_pack_start(GTK_BOX(column), make_label(label, "dim-label"), FALSE, FALSE, 0);
            bar = gtk_level_bar_new_for_interval(0, 1);
            gtk_level_bar_set_value(GTK_LEVEL_BAR(bar), 0);
            atk_object_set_name(gtk_widget_get_accessible(bar), label);
            gtk_box_pack_start(GTK_BOX(column), bar, FALSE, FALSE, 0);
            gtk_container_add(GTK_CONTAINER(meter_row), column);
        };
        meter("Input · sample peak", input_meter); meter("Output · sample peak", output_meter);
        gtk_box_pack_start(GTK_BOX(volume), meter_row, FALSE, FALSE, 0);
        peak_label = make_label("", "dim-label");
        gtk_box_pack_start(GTK_BOX(volume), peak_label, FALSE, FALSE, 0);
        GtkWidget* clear = listening_button("Clear peak readings", "METERS RESET");
        gtk_widget_set_tooltip_text(clear, "Clear the highest recorded levels and clipping warnings. This does not change the sound.");
        gtk_widget_set_halign(clear, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(volume), clear, FALSE, FALSE, 0);
        GtkWidget* preamp = add_control("Output volume", "Changes both listening options equally. Lower it if the output clips.", "dB",
            0, -24, 12, .5, 1, preamp_adj, preamp_scale, preamp_spin);
        // Preamp has a dedicated transaction and stays shared across A/B slots.
        g_signal_handlers_disconnect_by_func(preamp_adj, reinterpret_cast<gpointer>(on_any_slider_changed), this);
        g_signal_connect(preamp_adj, "value-changed", G_CALLBACK(+[](GtkAdjustment* adjustment, gpointer data) {
            auto* app = static_cast<CrossfeedGuiApp*>(data);
            if (app->suppress_events) return;
            app->state.preamp_db = gtk_adjustment_get_value(adjustment);
            if (!app->preview_mode) {
                std::ostringstream cmd; cmd.imbue(std::locale::classic());
                cmd << "SET preamp=" << app->state.preamp_db;
                app->listening_command(cmd.str());
            } else app->update_ui();
        }), this);
        gtk_box_pack_start(GTK_BOX(volume), preamp, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(volume_expander), volume);
        gtk_box_pack_start(GTK_BOX(card), volume_expander, FALSE, FALSE, 0);
        clip_notice = make_label("", "dim-label");
        gtk_widget_set_no_show_all(clip_notice, TRUE);
        gtk_box_pack_start(GTK_BOX(card), clip_notice, FALSE, FALSE, 0);
        gtk_widget_show_all(card);
        gtk_widget_set_no_show_all(card, TRUE); // Window/tray show_all must not expose opt-in tools.
        gtk_widget_set_visible(card, show_comparison);
        return card;
    }

    void refresh_library(int selected = -1) {
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(library_combo));
        for (const auto& preset : user_presets)
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(library_combo), preset.name.c_str());
        if (!user_presets.empty()) gtk_combo_box_set_active(GTK_COMBO_BOX(library_combo),
            selected >= 0 && selected < int(user_presets.size()) ? selected : 0);
        gtk_widget_set_sensitive(library_load_button, !user_presets.empty());
        for (auto* item : library_selection_actions) {
            const std::string action = static_cast<const char*>(g_object_get_data(G_OBJECT(item), "preset-action"));
            gtk_widget_set_sensitive(item, !user_presets.empty() && !(preview_mode && action == "Export"));
        }
        gtk_label_set_text(GTK_LABEL(library_label), library_error.empty() ?
            (preview_mode ? "Preview library · changes are not saved." : "Save your own sound or share a .crossfeed preset file.") : library_error.c_str());
    }

    bool save_library(const std::vector<UserPreset>& next, int selected = -1) {
        std::string error;
        std::vector<UserPreset> checked;
        if (!UserPresets::decode(UserPresets::encode(next), checked, error)) {
            show_listening_error(error); return false;
        }
        if (!preview_mode && !UserPresets::save(UserPresets::path(), next, error)) {
            show_listening_error(error); return false;
        }
        user_presets = next; library_error.clear(); refresh_library(selected); return true;
    }

    std::string ask_preset_name(const char* title, const std::string& initial) {
        GtkWidget* dialog = gtk_dialog_new_with_buttons(title, GTK_WINDOW(window), GTK_DIALOG_MODAL,
            "Cancel", GTK_RESPONSE_CANCEL, "Save", GTK_RESPONSE_ACCEPT, nullptr);
        GtkWidget* entry = gtk_entry_new(); gtk_entry_set_max_length(GTK_ENTRY(entry), 80);
        gtk_entry_set_text(GTK_ENTRY(entry), initial.c_str());
        gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
        gtk_container_set_border_width(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), 16);
        gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), entry);
        gtk_widget_show_all(dialog);
        std::string name;
        if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) name = gtk_entry_get_text(GTK_ENTRY(entry));
        gtk_widget_destroy(dialog); return name;
    }

    bool confirm_preset(const std::string& message) {
        GtkWidget* dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL, "%s", message.c_str());
        bool accepted = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK;
        gtk_widget_destroy(dialog); return accepted;
    }

    void library_action(const std::string& action) {
        const int selected = gtk_combo_box_get_active(GTK_COMBO_BOX(library_combo));
        if (action == "Load") {
            if (selected < 0) return;
            const auto p = user_presets[selected].params;
            state.preamp_db = p.preamp_db;
            apply_params(true, p.level_db, p.freq_hz, p.delay_us, p.phase_apf_hz,
                         p.center_trim_db, p.shadow_hz, p.advanced_effects);
            return;
        }
        auto next = user_presets;
        if (action == "Save" || action == "Duplicate" || action == "Rename") {
            if (action != "Save" && selected < 0) return;
            const std::string name = ask_preset_name((action + " preset").c_str(),
                action == "Save" ? "My preset" : next[selected].name + (action == "Duplicate" ? " copy" : ""));
            if (name.empty()) return;
            if (!UserPresets::valid_name(name)) { show_listening_error("Use a name of 1–80 bytes without control characters."); return; }
            int existing = -1;
            for (size_t i = 0; i < next.size(); ++i) if (next[i].name == name) existing = static_cast<int>(i);
            if (action == "Rename") {
                if (existing >= 0 && existing != selected) { show_listening_error("That preset name already exists."); return; }
                next[selected].name = name; save_library(next, selected); return;
            }
            UserPreset entry{name, action == "Save" ? current_sound() : next[selected].params};
            if (existing >= 0) {
                if (!confirm_preset("Replace the saved preset “" + name + "”?")) return;
                next[existing] = entry;
            } else { existing = static_cast<int>(next.size()); next.push_back(entry); }
            save_library(next, existing);
        } else if (action == "Delete") {
            if (selected < 0 || !confirm_preset("Delete the saved preset “" + next[selected].name + "”?")) return;
            next.erase(next.begin() + selected); save_library(next);
        } else if (action == "Import" || action == "Export") {
            if (preview_mode) return;
            const bool importing = action == "Import";
            if (!importing && selected < 0) return;
            GtkWidget* dialog = gtk_file_chooser_dialog_new((action + " preset").c_str(), GTK_WINDOW(window),
                importing ? GTK_FILE_CHOOSER_ACTION_OPEN : GTK_FILE_CHOOSER_ACTION_SAVE,
                "Cancel", GTK_RESPONSE_CANCEL, action.c_str(), GTK_RESPONSE_ACCEPT, nullptr);
            GtkFileFilter* filter = gtk_file_filter_new(); gtk_file_filter_set_name(filter, "Crossfeed presets (*.crossfeed)");
            gtk_file_filter_add_pattern(filter, "*.crossfeed"); gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
            if (!importing) {
                gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "preset.crossfeed");
                gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
            }
            std::string filename;
            if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
                char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
                if (path) { filename = path; g_free(path); }
            }
            gtk_widget_destroy(dialog); if (filename.empty()) return;
            std::string error;
            if (importing) {
                std::vector<UserPreset> imported;
                if (!UserPresets::load(filename, imported, error)) { show_listening_error(error); return; }
                for (auto& entry : imported) {
                    if (!g_utf8_validate(entry.name.c_str(), -1, nullptr)) { show_listening_error("Preset names must be valid UTF-8."); return; }
                    const std::string base = entry.name;
                    unsigned suffix = 2;
                    while (std::any_of(next.begin(), next.end(), [&](const UserPreset& p) { return p.name == entry.name; }))
                        entry.name = base + " (" + std::to_string(suffix++) + ")";
                    next.push_back(entry);
                }
                save_library(next);
            } else if (!UserPresets::save(filename, {next[selected]}, error)) show_listening_error(error);
        }
    }

    GtkWidget* build_library() {
        GtkWidget* expander = gtk_expander_new("My presets");
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_set_margin_top(box, 10);
        library_combo = gtk_combo_box_text_new();
        atk_object_set_name(gtk_widget_get_accessible(library_combo), "Saved user presets");
        GList* cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(library_combo));
        for (GList* cell = cells; cell; cell = cell->next)
            g_object_set(cell->data, "ellipsize", PANGO_ELLIPSIZE_END, "max-width-chars", 30, nullptr);
        g_list_free(cells);
        gtk_box_pack_start(GTK_BOX(box), library_combo, FALSE, FALSE, 0);
        GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        const char* names[] = {"Use preset", "Save current…"};
        const char* commands[] = {"Load", "Save"};
        for (unsigned i = 0; i < 2; ++i) {
            GtkWidget* button = gtk_button_new_with_label(names[i]);
            if (i == 0) library_load_button = button;
            g_object_set_data_full(G_OBJECT(button), "preset-action", g_strdup(commands[i]), g_free);
            g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer data) {
                static_cast<CrossfeedGuiApp*>(data)->library_action(
                    static_cast<const char*>(g_object_get_data(G_OBJECT(button), "preset-action")));
            }), this);
            gtk_box_pack_start(GTK_BOX(actions), button, FALSE, FALSE, 0);
        }
        GtkWidget* manage = gtk_menu_button_new();
        gtk_button_set_label(GTK_BUTTON(manage), "More…");
        gtk_widget_set_tooltip_text(manage, "Rename, copy, delete, import or export presets.");
        atk_object_set_name(gtk_widget_get_accessible(manage), "More preset actions");
        GtkWidget* menu = gtk_menu_new();
        const char* menu_labels[] = {"Rename selected…", "Duplicate selected…", "Delete selected…", "Import file…", "Export selected…"};
        const char* menu_actions[] = {"Rename", "Duplicate", "Delete", "Import", "Export"};
        for (unsigned i = 0; i < 5; ++i) {
            GtkWidget* item = gtk_menu_item_new_with_label(menu_labels[i]);
            g_object_set_data_full(G_OBJECT(item), "preset-action", g_strdup(menu_actions[i]), g_free);
            g_signal_connect(item, "activate", G_CALLBACK(+[](GtkMenuItem* item, gpointer data) {
                static_cast<CrossfeedGuiApp*>(data)->library_action(
                    static_cast<const char*>(g_object_get_data(G_OBJECT(item), "preset-action")));
            }), this);
            if (i != 3) library_selection_actions.push_back(item);
            if (preview_mode && i >= 3) gtk_widget_set_sensitive(item, FALSE);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        }
        gtk_widget_show_all(menu);
        gtk_menu_button_set_popup(GTK_MENU_BUTTON(manage), menu);
        gtk_box_pack_end(GTK_BOX(actions), manage, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), actions, FALSE, FALSE, 0);
        library_label = make_label("", "dim-label");
        gtk_box_pack_start(GTK_BOX(box), library_label, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(expander), box);
        refresh_library(); return expander;
    }
