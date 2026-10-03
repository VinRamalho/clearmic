#include "clearmic/platform/linux/device_manager.hpp"

#include <gtk/gtk.h>
#include <gst/gst.h>

#include <algorithm>
#include <cstdio>
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
    GtkWidget* input_meter{};
    GtkWidget* output_meter{};
    GtkWidget* start_button{};
    GtkWidget* stop_button{};
    GtkWidget* refresh_button{};
    GtkWidget* record_button{};
    GtkWidget* play_original_button{};
    GtkWidget* play_processed_button{};
    GtkWidget* stop_playback_button{};
    std::vector<audio::AudioDevice> inputs;
    GSubprocess* service{};
    GSubprocess* test_capture{};
    GDataInputStream* service_output{};
    GCancellable* output_cancel{};
    GstElement* player{};
    guint player_bus_watch{};
    guint device_refresh_source{};
    std::string original_path;
    std::string processed_path;
    bool refreshing_devices{};
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
    gtk_widget_set_sensitive(app.start_button, !running && app.test_capture == nullptr && selected_index(app) >= 0);
    gtk_widget_set_sensitive(app.stop_button, running);
    gtk_widget_set_sensitive(app.devices, !running && app.test_capture == nullptr);
    gtk_widget_set_sensitive(app.refresh_button, !running && app.test_capture == nullptr);
    gtk_widget_set_sensitive(app.record_button, !running && app.test_capture == nullptr && selected_index(app) >= 0);
    gtk_widget_set_sensitive(app.play_original_button, app.player && app.test_capture == nullptr && !app.original_path.empty());
    gtk_widget_set_sensitive(app.play_processed_button, app.player && app.test_capture == nullptr && !app.processed_path.empty());
    gtk_widget_set_sensitive(app.stop_playback_button, app.player != nullptr);
}

void refresh_devices(Application& app) {
    app.refreshing_devices = true;
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
    app.refreshing_devices = false;
}

void on_device_changed(GtkComboBox* combo, gpointer data) {
    auto& app = *static_cast<Application*>(data);
    if (app.refreshing_devices) return;
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

gboolean on_device_refresh_timer(gpointer data) {
    auto& app = *static_cast<Application*>(data);
    if (!app.service && !app.test_capture) refresh_devices(app);
    return G_SOURCE_CONTINUE;
}

gboolean on_player_message(GstBus*, GstMessage* message, gpointer user_data) {
    auto& app = *static_cast<Application*>(user_data);
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
        gst_element_set_state(app.player, GST_STATE_NULL);
        gtk_label_set_text(GTK_LABEL(app.service_status), "A/B sample playback finished.");
        update_controls(app);
    } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        gtk_label_set_text(GTK_LABEL(app.service_status), error ? error->message : "Could not play the A/B sample.");
        if (error) g_error_free(error);
        g_free(debug);
        gst_element_set_state(app.player, GST_STATE_NULL);
        update_controls(app);
    }
    return G_SOURCE_CONTINUE;
}

void play_sample(Application& app, const std::string& path, const char* label) {
    if (!app.player) return;
    gst_element_set_state(app.player, GST_STATE_NULL);
    GError* error = nullptr;
    gchar* uri = gst_filename_to_uri(path.c_str(), &error);
    if (!uri) {
        gtk_label_set_text(GTK_LABEL(app.service_status), error ? error->message : "Could not open the A/B recording.");
        if (error) g_error_free(error);
        return;
    }
    g_object_set(app.player, "uri", uri, nullptr);
    g_free(uri);
    if (gst_element_set_state(app.player, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        gtk_label_set_text(GTK_LABEL(app.service_status), "Could not start A/B sample playback.");
        return;
    }
    std::string status = std::string("Playing ") + label + " sample.";
    gtk_label_set_text(GTK_LABEL(app.service_status), status.c_str());
    update_controls(app);
}

void on_play_original(GtkButton*, gpointer data) {
    auto& app = *static_cast<Application*>(data);
    play_sample(app, app.original_path, "original");
}

void on_play_processed(GtkButton*, gpointer data) {
    auto& app = *static_cast<Application*>(data);
    play_sample(app, app.processed_path, "processed");
}

void on_stop_playback(GtkButton*, gpointer data) {
    auto& app = *static_cast<Application*>(data);
    if (app.player) gst_element_set_state(app.player, GST_STATE_NULL);
    gtk_label_set_text(GTK_LABEL(app.service_status), "A/B sample playback stopped.");
    update_controls(app);
}

void on_record_sample(GtkButton*, gpointer data) {
    auto& app = *static_cast<Application*>(data);
    const int index = selected_index(app);
    if (index < 0 || app.test_capture) return;
    gchar* directory = g_build_filename(g_get_user_cache_dir(), "clearmic", "ab-tests", nullptr);
    g_mkdir_with_parents(directory, 0700);
    gchar* original = g_build_filename(directory, "original.wav", nullptr);
    gchar* processed = g_build_filename(directory, "processed.wav", nullptr);
    app.original_path = original;
    app.processed_path = processed;
    const auto device_id = app.inputs[static_cast<std::size_t>(index)].id;
    GError* error = nullptr;
    app.test_capture = g_subprocess_new(static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE), &error,
                                        app.executable, "record-test", "5", original, processed,
                                        device_id.c_str(), nullptr);
    g_free(directory);
    g_free(original);
    g_free(processed);
    if (!app.test_capture) {
        gtk_label_set_text(GTK_LABEL(app.service_status), error ? error->message : "Could not start A/B recording.");
        if (error) g_error_free(error);
        app.original_path.clear();
        app.processed_path.clear();
        update_controls(app);
        return;
    }
    gtk_label_set_text(GTK_LABEL(app.service_status), "Recording five seconds for local A/B comparison…");
    gtk_widget_set_sensitive(app.devices, FALSE);
    gtk_widget_set_sensitive(app.refresh_button, FALSE);
    update_controls(app);
    g_subprocess_wait_check_async(app.test_capture, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer user_data) {
            auto& state = *static_cast<Application*>(user_data);
            GError* wait_error = nullptr;
            if (!g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &wait_error)) {
                gtk_label_set_text(GTK_LABEL(state.service_status), wait_error ? wait_error->message : "A/B recording failed.");
                if (wait_error) g_error_free(wait_error);
                state.original_path.clear();
                state.processed_path.clear();
            } else {
                gtk_label_set_text(GTK_LABEL(state.service_status), "A/B sample ready. Play the original or processed recording.");
            }
            g_clear_object(&state.test_capture);
            gtk_widget_set_sensitive(state.devices, TRUE);
            gtk_widget_set_sensitive(state.refresh_button, TRUE);
            update_controls(state);
        }, &app);
}

void read_service_output(GObject* source, GAsyncResult* result, gpointer user_data) {
    auto& app = *static_cast<Application*>(user_data);
    GError* error = nullptr;
    gsize length = 0;
    gchar* line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source), result, &length, &error);
    if (line && app.service) {
        float input = 0.0F;
        float output = 0.0F;
        if (std::sscanf(line, "METER %f %f", &input, &output) == 2) {
            input = std::clamp(input, 0.0F, 1.0F);
            output = std::clamp(output, 0.0F, 1.0F);
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app.input_meter), input);
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app.output_meter), output);
            char text[32];
            std::snprintf(text, sizeof(text), "Input %.0f%%", input * 100.0F);
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app.input_meter), text);
            std::snprintf(text, sizeof(text), "Processed %.0f%%", output * 100.0F);
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app.output_meter), text);
        }
        g_free(line);
        g_data_input_stream_read_line_async(app.service_output, G_PRIORITY_DEFAULT, app.output_cancel,
                                            read_service_output, &app);
        return;
    }
    g_free(line);
    if (error) g_error_free(error);
}

void on_start(GtkButton*, gpointer data) {
    auto& app = *static_cast<Application*>(data);
    const int index = selected_index(app);
    if (index < 0) return;
    const auto& id = app.inputs[static_cast<std::size_t>(index)].id;
    const int preset_index = std::clamp(gtk_combo_box_get_active(GTK_COMBO_BOX(app.preset)), 0, 2);
    const char* presets[] = {"natural", "meeting", "strong"};
    GError* error = nullptr;
    app.service = g_subprocess_new(static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE), &error,
                                   app.executable, "serve", id.c_str(), presets[preset_index], nullptr);
    if (!app.service) {
        gtk_label_set_text(GTK_LABEL(app.service_status), error ? error->message : "Could not start audio service.");
        if (error) g_error_free(error);
        return;
    }
    gtk_label_set_text(GTK_LABEL(app.service_status), "Starting ClearMic virtual microphone…");
    gtk_widget_set_sensitive(app.devices, FALSE);
    update_controls(app);
    app.service_output = g_data_input_stream_new(g_subprocess_get_stdout_pipe(app.service));
    app.output_cancel = g_cancellable_new();
    gtk_label_set_text(GTK_LABEL(app.service_status), "ClearMic is processing locally. Select its virtual microphone in your audio application.");
    g_data_input_stream_read_line_async(app.service_output, G_PRIORITY_DEFAULT, app.output_cancel,
                                        read_service_output, &app);
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
            if (state.output_cancel) g_cancellable_cancel(state.output_cancel);
            g_clear_object(&state.service_output);
            g_clear_object(&state.output_cancel);
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
        if (app.output_cancel) g_cancellable_cancel(app.output_cancel);
        g_subprocess_send_signal(app.service, SIGTERM);
        g_subprocess_wait(app.service, nullptr, nullptr);
    }
    if (app.test_capture) {
        g_subprocess_send_signal(app.test_capture, SIGTERM);
        g_subprocess_wait(app.test_capture, nullptr, nullptr);
    }
    if (app.player) gst_element_set_state(app.player, GST_STATE_NULL);
    if (app.device_refresh_source) g_source_remove(app.device_refresh_source);
    if (app.player_bus_watch) g_source_remove(app.player_bus_watch);
    g_clear_object(&app.player);
    g_clear_object(&app.service_output);
    g_clear_object(&app.output_cancel);
    g_clear_object(&app.service);
    g_clear_object(&app.test_capture);
    gtk_main_quit();
}

GtkWidget* make_label(const char* text) {
    auto* label = gtk_label_new(text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    return label;
}
}

int run_desktop_application(const char* executable_path) {
    gst_init(nullptr, nullptr);
    int argc = 0;
    char** argv = nullptr;
    gtk_init(&argc, &argv);
    gchar* resolved = g_find_program_in_path(executable_path);
    Application app;
    app.executable = resolved ? resolved : executable_path;
    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    app.player = gst_element_factory_make("playbin", "clearmic-ab-player");
    gtk_window_set_title(GTK_WINDOW(app.window), "ClearMic");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 640, 520);
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
    app.input_meter = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(app.input_meter), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app.input_meter), "Input");
    gtk_box_pack_start(GTK_BOX(layout), app.input_meter, FALSE, FALSE, 0);
    app.output_meter = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(app.output_meter), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app.output_meter), "Processed output");
    gtk_box_pack_start(GTK_BOX(layout), app.output_meter, FALSE, FALSE, 0);

    auto* service_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app.start_button = gtk_button_new_with_label("Start enhancement");
    app.stop_button = gtk_button_new_with_label("Stop");
    gtk_box_pack_start(GTK_BOX(service_buttons), app.start_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(service_buttons), app.stop_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(layout), service_buttons, FALSE, FALSE, 0);
    auto* comparison_label = make_label("A/B comparison · records five seconds when requested");
    gtk_box_pack_start(GTK_BOX(layout), comparison_label, FALSE, FALSE, 0);
    auto* comparison_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app.record_button = gtk_button_new_with_label("Record A/B sample");
    app.play_original_button = gtk_button_new_with_label("Play original");
    app.play_processed_button = gtk_button_new_with_label("Play processed");
    app.stop_playback_button = gtk_button_new_with_label("Stop playback");
    gtk_box_pack_start(GTK_BOX(comparison_buttons), app.record_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(comparison_buttons), app.play_original_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(layout), comparison_buttons, FALSE, FALSE, 0);
    auto* playback_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(playback_buttons), app.play_processed_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(playback_buttons), app.stop_playback_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(layout), playback_buttons, FALSE, FALSE, 0);
    auto* note = make_label("Audio stays on this computer. Device selection is saved between launches.");
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_box_pack_start(GTK_BOX(layout), note, FALSE, FALSE, 8);

    g_signal_connect(app.window, "destroy", G_CALLBACK(on_window_destroy), &app);
    g_signal_connect(app.devices, "changed", G_CALLBACK(on_device_changed), &app);
    g_signal_connect(app.preset, "changed", G_CALLBACK(on_preset_changed), &app);
    g_signal_connect(app.refresh_button, "clicked", G_CALLBACK(on_refresh), &app);
    g_signal_connect(app.start_button, "clicked", G_CALLBACK(on_start), &app);
    g_signal_connect(app.stop_button, "clicked", G_CALLBACK(on_stop), &app);
    g_signal_connect(app.record_button, "clicked", G_CALLBACK(on_record_sample), &app);
    g_signal_connect(app.play_original_button, "clicked", G_CALLBACK(on_play_original), &app);
    g_signal_connect(app.play_processed_button, "clicked", G_CALLBACK(on_play_processed), &app);
    g_signal_connect(app.stop_playback_button, "clicked", G_CALLBACK(on_stop_playback), &app);
    if (app.player) {
        GstBus* bus = gst_element_get_bus(app.player);
        app.player_bus_watch = gst_bus_add_watch(bus, on_player_message, &app);
        gst_object_unref(bus);
    } else {
        gtk_label_set_text(GTK_LABEL(app.service_status), "A/B playback is unavailable because GStreamer playbin could not load.");
    }
    const auto preset = stored_preset();
    const int preset_index = preset == "meeting" ? 1 : (preset == "strong" ? 2 : 0);
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.preset), preset_index);
    refresh_devices(app);
    app.device_refresh_source = g_timeout_add_seconds(3, on_device_refresh_timer, &app);
    gtk_widget_show_all(app.window);
    gtk_main();
    g_free(resolved);
    return 0;
}
}
