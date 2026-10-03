#include "clearmic/platform/linux/device_manager.hpp"

#include <gtk/gtk.h>

#include <algorithm>
#include <iterator>
#include <signal.h>
#include <string>
#include <vector>

namespace clearmic::platform::pipewire {
namespace {
struct Application {
    const char* executable{};
    GtkWidget* window{};
    GtkWidget* devices{};
    GtkWidget* preset{};
    GtkWidget* device_status{};
    GtkWidget* service_status{};
    GtkWidget* start_button{};
    GtkWidget* stop_button{};
    GtkWidget* refresh_button{};
    std::vector<audio::AudioDevice> inputs;
    GSubprocess* service{};
};

std::string settings_path() {
    gchar* directory = g_build_filename(g_get_user_config_dir(), "clearmic", nullptr);
    g_mkdir_with_parents(directory, 0700);
    gchar* path = g_build_filename(directory, "settings.ini", nullptr);
    std::string result(path);
    g_free(path);
    g_free(directory);
    return result;
}

std::string stored_device_id() {
    GKeyFile* key_file = g_key_file_new();
    gchar* path = g_strdup(settings_path().c_str());
    GError* error = nullptr;
    g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, &error);
    if (error) g_error_free(error);
    gchar* value = g_key_file_get_string(key_file, "audio", "input-device", nullptr);
    std::string result = value ? value : "";
    g_free(value);
    g_free(path);
    g_key_file_unref(key_file);
    return result;
}

std::string stored_preset() {
    GKeyFile* key_file = g_key_file_new();
    const auto path = settings_path();
    GError* error = nullptr;
    g_key_file_load_from_file(key_file, path.c_str(), G_KEY_FILE_NONE, &error);
    if (error) g_error_free(error);
    gchar* value = g_key_file_get_string(key_file, "audio", "preset", nullptr);
    std::string result = value ? value : "natural";
    g_free(value);
    g_key_file_unref(key_file);
    return result;
}

void save_device_id(const std::string& value) {
    GKeyFile* key_file = g_key_file_new();
    const auto path = settings_path();
    GError* error = nullptr;
    g_key_file_load_from_file(key_file, path.c_str(), G_KEY_FILE_NONE, &error);
    if (error) g_error_free(error);
    g_key_file_set_string(key_file, "audio", "input-device", value.c_str());
    gsize length = 0;
    gchar* data = g_key_file_to_data(key_file, &length, nullptr);
    if (data) {
        g_file_set_contents(path.c_str(), data, static_cast<gssize>(length), nullptr);
        g_free(data);
    }
    g_key_file_unref(key_file);
}

void save_preset(const std::string& value) {
    GKeyFile* key_file = g_key_file_new();
    const auto path = settings_path();
    GError* error = nullptr;
    g_key_file_load_from_file(key_file, path.c_str(), G_KEY_FILE_NONE, &error);
    if (error) g_error_free(error);
    g_key_file_set_string(key_file, "audio", "preset", value.c_str());
    gsize length = 0;
    gchar* data = g_key_file_to_data(key_file, &length, nullptr);
    if (data) {
        g_file_set_contents(path.c_str(), data, static_cast<gssize>(length), nullptr);
        g_free(data);
    }
    g_key_file_unref(key_file);
}

int selected_index(const Application& app) {
    const guint selected = gtk_combo_box_get_active(GTK_COMBO_BOX(app.devices));
    return selected < app.inputs.size() ? static_cast<int>(selected) : -1;
}

void update_device_status(Application& app) {
    const int index = selected_index(app);
    if (index < 0 || static_cast<std::size_t>(index) >= app.inputs.size()) {
        gtk_label_set_text(GTK_LABEL(app.device_status), "No PipeWire microphone source is available.");
        return;
    }
    const auto& selected = app.inputs[static_cast<std::size_t>(index)];
    const char* connection = selected.connection == audio::ConnectionState::connected ? "Connected" : "Status unknown";
    std::string status = std::string(connection) + " · " + selected.id;
    if (selected.sample_rate_hz) status += " · " + std::to_string(*selected.sample_rate_hz) + " Hz";
    if (selected.channels) status += " · " + std::to_string(*selected.channels) + " ch";
    gtk_label_set_text(GTK_LABEL(app.device_status), status.c_str());
}

void update_controls(Application& app) {
    const bool running = app.service != nullptr;
    gtk_widget_set_sensitive(app.start_button, !running && selected_index(app) >= 0);
    gtk_widget_set_sensitive(app.stop_button, running);
}

void refresh_devices(Application& app) {
    const auto preferred = stored_device_id();
    try {
        app.inputs = DeviceManager{}.input_devices();
    } catch (const std::exception& error) {
        app.inputs.clear();
        gtk_label_set_text(GTK_LABEL(app.device_status), error.what());
    }

    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app.devices));
    int preferred_index = -1;
    int default_index = -1;
    for (const auto& device : app.inputs) {
        if (!device.selectable) continue;
        const auto index = static_cast<int>(gtk_tree_model_iter_n_children(
            gtk_combo_box_get_model(GTK_COMBO_BOX(app.devices)), nullptr));
        std::string label = device.name;
        if (device.is_default) label += " (default)";
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.devices), label.c_str());
        if (device.id == preferred) preferred_index = index;
        if (device.is_default) default_index = index;
    }

    // Keep selectable endpoints in the same order as the GTK list.
    std::vector<audio::AudioDevice> selectable;
    std::copy_if(app.inputs.begin(), app.inputs.end(), std::back_inserter(selectable),
                 [](const auto& device) { return device.selectable; });
    app.inputs = std::move(selectable);
    const int active = preferred_index >= 0 ? preferred_index
        : (default_index >= 0 ? default_index : (app.inputs.empty() ? -1 : 0));
    if (active >= 0) gtk_combo_box_set_active(GTK_COMBO_BOX(app.devices), active);
    update_device_status(app);
    update_controls(app);
}

void on_device_changed(GtkComboBox* combo, gpointer data) {
    auto& app = *static_cast<Application*>(data);
    const int index = gtk_combo_box_get_active(combo);
    if (index < 0 || static_cast<std::size_t>(index) >= app.inputs.size()) return;
    save_device_id(app.inputs[static_cast<std::size_t>(index)].id);
    update_device_status(app);
    update_controls(app);
}

void on_preset_changed(GtkComboBox* combo, gpointer) {
    const int index = gtk_combo_box_get_active(combo);
    if (index < 0 || index > 2) return;
    const char* names[] = {"natural", "meeting", "strong"};
    save_preset(names[index]);
}

void on_refresh(GtkButton*, gpointer data) { refresh_devices(*static_cast<Application*>(data)); }

void on_start(GtkButton*, gpointer data) {
    auto& app = *static_cast<Application*>(data);
    const int index = selected_index(app);
    if (index < 0) return;
    const auto& id = app.inputs[static_cast<std::size_t>(index)].id;
    const int preset_index = std::clamp(gtk_combo_box_get_active(GTK_COMBO_BOX(app.preset)), 0, 2);
    const char* presets[] = {"natural", "meeting", "strong"};
    GError* error = nullptr;
    app.service = g_subprocess_new(static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE), &error,
                                   app.executable, "serve", id.c_str(), presets[preset_index], nullptr);
    if (!app.service) {
        gtk_label_set_text(GTK_LABEL(app.service_status), error ? error->message : "Could not start audio service.");
        if (error) g_error_free(error);
        return;
    }
    gtk_label_set_text(GTK_LABEL(app.service_status), "Starting ClearMic virtual microphone…");
    gtk_widget_set_sensitive(app.devices, FALSE);
    update_controls(app);
    g_subprocess_wait_check_async(app.service, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer user_data) {
            auto& state = *static_cast<Application*>(user_data);
            GError* wait_error = nullptr;
            if (!g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &wait_error)) {
                gtk_label_set_text(GTK_LABEL(state.service_status), wait_error ? wait_error->message : "Audio service stopped with an error.");
                if (wait_error) g_error_free(wait_error);
            } else {
                gtk_label_set_text(GTK_LABEL(state.service_status), "Audio service stopped.");
            }
            g_clear_object(&state.service);
            gtk_widget_set_sensitive(state.devices, TRUE);
            refresh_devices(state);
        }, &app);
}

void on_stop(GtkButton*, gpointer data) {
    auto& app = *static_cast<Application*>(data);
    if (!app.service) return;
    g_subprocess_send_signal(app.service, SIGTERM);
    gtk_label_set_text(GTK_LABEL(app.service_status), "Stopping audio service…");
}

void on_window_destroy(GtkWidget*, gpointer data) {
    auto& app = *static_cast<Application*>(data);
    if (app.service) {
        g_subprocess_send_signal(app.service, SIGTERM);
        g_subprocess_wait(app.service, nullptr, nullptr);
    }
    g_clear_object(&app.service);
}

GtkWidget* make_label(const char* text) {
    auto* label = gtk_label_new(text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    return label;
}
}

int run_desktop_application(const char* executable_path) {
    int argc = 0;
    char** argv = nullptr;
    gtk_init(&argc, &argv);
    gchar* resolved = g_find_program_in_path(executable_path);
    Application app;
    app.executable = resolved ? resolved : executable_path;
    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "ClearMic");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 520, 380);
    gtk_container_set_border_width(GTK_CONTAINER(app.window), 24);

    auto* layout = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_add(GTK_CONTAINER(app.window), layout);
    auto* title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title), "<span size='xx-large' weight='bold'>ClearMic</span>");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(layout), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(layout), make_label("Microphone"), FALSE, FALSE, 0);
    app.devices = gtk_combo_box_text_new();
    gtk_box_pack_start(GTK_BOX(layout), app.devices, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(layout), make_label("Processing profile"), FALSE, FALSE, 0);
    app.preset = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.preset), "Natural");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.preset), "Meeting");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.preset), "Strong Noise Reduction");
    gtk_box_pack_start(GTK_BOX(layout), app.preset, FALSE, FALSE, 0);
    app.device_status = make_label("Discovering PipeWire microphones…");
    gtk_label_set_line_wrap(GTK_LABEL(app.device_status), TRUE);
    gtk_box_pack_start(GTK_BOX(layout), app.device_status, FALSE, FALSE, 0);

    auto* device_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app.refresh_button = gtk_button_new_with_label("Refresh devices");
    gtk_box_pack_start(GTK_BOX(device_buttons), app.refresh_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(layout), device_buttons, FALSE, FALSE, 0);
    app.service_status = make_label("Audio processing is off.");
    gtk_label_set_line_wrap(GTK_LABEL(app.service_status), TRUE);
    gtk_box_pack_start(GTK_BOX(layout), app.service_status, FALSE, FALSE, 4);

    auto* service_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app.start_button = gtk_button_new_with_label("Start enhancement");
    app.stop_button = gtk_button_new_with_label("Stop");
    gtk_box_pack_start(GTK_BOX(service_buttons), app.start_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(service_buttons), app.stop_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(layout), service_buttons, FALSE, FALSE, 0);
    auto* note = make_label("Audio stays on this computer. Device selection is saved between launches.");
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_box_pack_start(GTK_BOX(layout), note, FALSE, FALSE, 8);

    g_signal_connect(app.window, "destroy", G_CALLBACK(on_window_destroy), &app);
    g_signal_connect(app.devices, "changed", G_CALLBACK(on_device_changed), &app);
    g_signal_connect(app.preset, "changed", G_CALLBACK(on_preset_changed), &app);
    g_signal_connect(app.refresh_button, "clicked", G_CALLBACK(on_refresh), &app);
    g_signal_connect(app.start_button, "clicked", G_CALLBACK(on_start), &app);
    g_signal_connect(app.stop_button, "clicked", G_CALLBACK(on_stop), &app);
    const auto preset = stored_preset();
    const int preset_index = preset == "meeting" ? 1 : (preset == "strong" ? 2 : 0);
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.preset), preset_index);
    refresh_devices(app);
    gtk_widget_show_all(app.window);
    gtk_main();
    g_free(resolved);
    return 0;
}
}
