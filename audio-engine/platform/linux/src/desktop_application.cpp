#include "clearmic/platform/linux/device_manager.hpp"
#include "clearmic/audio/processing.hpp"
#include "clearmic/audio/wav.hpp"

#include <gtk/gtk.h>
#include <gst/gst.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <iomanip>
#include <signal.h>
#include <sstream>
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
    GtkWidget* background_toggle{};
    GtkWidget* noise_suppression{};
    GtkWidget* noise_gate{};
    GtkWidget* automatic_gain{};
    GtkWidget* compressor{};
    GtkWidget* enhancement{};
    GtkWidget* input_gain{};
    GtkWidget* input_gain_label{};
    std::vector<audio::AudioDevice> inputs;
    GSubprocess* service{};
    GSubprocess* test_capture{};
    GDataInputStream* service_output{};
    GCancellable* output_cancel{};
    GstElement* player{};
    GtkStatusIcon* tray_icon{};
    guint player_bus_watch{};
    guint device_refresh_source{};
    std::string original_path;
    std::string processed_path;
    std::string active_device_id;
    bool refreshing_devices{};
    bool updating_preferences{};
    bool reconnect_enabled{};
    bool closing{};
    bool user_stopping{};
    unsigned int restart_attempt{};
    guint restart_source{};
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

bool stored_background_mode() {
    GKeyFile* key_file = g_key_file_new();
    const auto path = settings_path();
    GError* error = nullptr;
    g_key_file_load_from_file(key_file, path.c_str(), G_KEY_FILE_NONE, nullptr);
    const gboolean value = g_key_file_get_boolean(key_file, "ui", "background-on-close", &error);
    const bool result = error ? false : value != FALSE;
    if (error) g_error_free(error);
    g_key_file_unref(key_file);
    return result;
}

void save_background_mode(const bool enabled) {
    GKeyFile* key_file = g_key_file_new();
    const auto path = settings_path();
    g_key_file_load_from_file(key_file, path.c_str(), G_KEY_FILE_NONE, nullptr);
    g_key_file_set_boolean(key_file, "ui", "background-on-close", enabled);
    gsize length = 0;
    gchar* data = g_key_file_to_data(key_file, &length, nullptr);
    if (data) {
        g_file_set_contents(path.c_str(), data, static_cast<gssize>(length), nullptr);
        g_free(data);
    }
    g_key_file_unref(key_file);
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

bool stored_processing_toggle(const char* key, const bool fallback) {
    GKeyFile* key_file = g_key_file_new();
    const auto path = settings_path();
    GError* error = nullptr;
    g_key_file_load_from_file(key_file, path.c_str(), G_KEY_FILE_NONE, nullptr);
    const gboolean value = g_key_file_get_boolean(key_file, "processing", key, &error);
    const bool result = error ? fallback : value != FALSE;
    if (error) g_error_free(error);
    g_key_file_unref(key_file);
    return result;
}

float stored_input_gain() {
    GKeyFile* key_file = g_key_file_new();
    const auto path = settings_path();
    g_key_file_load_from_file(key_file, path.c_str(), G_KEY_FILE_NONE, nullptr);
    GError* error = nullptr;
    const double value = g_key_file_get_double(key_file, "processing", "input-gain-db", &error);
    const float result = error ? 0.0F : static_cast<float>(std::clamp(value, -12.0, 12.0));
    if (error) g_error_free(error);
    g_key_file_unref(key_file);
    return result;
}

void save_input_gain(const float value) {
    GKeyFile* key_file = g_key_file_new();
    const auto path = settings_path();
    g_key_file_load_from_file(key_file, path.c_str(), G_KEY_FILE_NONE, nullptr);
    g_key_file_set_double(key_file, "processing", "input-gain-db", std::clamp(value, -12.0F, 12.0F));
    gsize length = 0;
    gchar* data = g_key_file_to_data(key_file, &length, nullptr);
    if (data) {
        g_file_set_contents(path.c_str(), data, static_cast<gssize>(length), nullptr);
        g_free(data);
    }
    g_key_file_unref(key_file);
}

void save_processing_toggle(const char* key, const bool value) {
    GKeyFile* key_file = g_key_file_new();
    const auto path = settings_path();
    GError* error = nullptr;
    g_key_file_load_from_file(key_file, path.c_str(), G_KEY_FILE_NONE, nullptr);
    if (error) g_error_free(error);
    g_key_file_set_boolean(key_file, "processing", key, value);
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
    if (selected.bluetooth_address) status += " · Bluetooth " + *selected.bluetooth_address;
    if (selected.bluetooth_profile) status += " · Profile " + *selected.bluetooth_profile;
    if (selected.bluetooth_codec) status += " · Codec " + *selected.bluetooth_codec;
    if (selected.sample_rate_hz) status += " · " + std::to_string(*selected.sample_rate_hz) + " Hz";
    if (selected.channels) status += " · " + std::to_string(*selected.channels) + " ch";
    if (selected.usb_vendor_id && selected.usb_product_id) {
        std::ostringstream usb_ids;
        usb_ids << " · USB VID:PID " << std::uppercase << std::hex << std::setfill('0')
                << std::setw(4) << *selected.usb_vendor_id << ':' << std::setw(4) << *selected.usb_product_id;
        status += usb_ids.str();
    }
    if (selected.capabilities.battery && selected.capabilities.battery->percentage) {
        status += " · Battery " + std::to_string(*selected.capabilities.battery->percentage) + "%";
        switch (selected.capabilities.battery->charging) {
        case audio::ChargingState::charging: status += " (charging)"; break;
        case audio::ChargingState::not_charging: status += " (not charging)"; break;
        case audio::ChargingState::full: status += " (full)"; break;
        case audio::ChargingState::unknown: break;
        }
    } else if (selected.bluetooth_address) {
        status += " · Battery not available";
    }
    gtk_label_set_text(GTK_LABEL(app.device_status), status.c_str());
}

void update_controls(Application& app) {
    const bool running = app.service != nullptr;
    const bool service_active = running || app.restart_source != 0;
    gtk_widget_set_sensitive(app.start_button, !service_active && app.test_capture == nullptr && selected_index(app) >= 0);
    gtk_widget_set_sensitive(app.stop_button, service_active);
    gtk_button_set_label(GTK_BUTTON(app.stop_button), app.restart_source ? "Cancel reconnect" : "Stop");
    gtk_widget_set_sensitive(app.devices, !service_active && app.test_capture == nullptr);
    gtk_widget_set_sensitive(app.refresh_button, !service_active && app.test_capture == nullptr);
    gtk_widget_set_sensitive(app.preset, !service_active && app.test_capture == nullptr);
    gtk_widget_set_sensitive(app.noise_suppression, !service_active && app.test_capture == nullptr);
    gtk_widget_set_sensitive(app.noise_gate, !service_active && app.test_capture == nullptr);
    gtk_widget_set_sensitive(app.automatic_gain, !service_active && app.test_capture == nullptr);
    gtk_widget_set_sensitive(app.compressor, !service_active && app.test_capture == nullptr);
    gtk_widget_set_sensitive(app.enhancement, !service_active && app.test_capture == nullptr);
    gtk_widget_set_sensitive(app.input_gain, !service_active && app.test_capture == nullptr);
    gtk_widget_set_sensitive(app.record_button, !service_active && app.test_capture == nullptr && selected_index(app) >= 0);
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
    if (active >= 0) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(app.devices), active);
        if (preferred_index < 0) save_device_id(app.inputs[static_cast<std::size_t>(active)].id);
    }
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

void on_preset_changed(GtkComboBox* combo, gpointer data) {
    auto& app = *static_cast<Application*>(data);
    if (app.updating_preferences) return;
    const int index = gtk_combo_box_get_active(combo);
    if (index < 0 || index > 2) return;
    const char* names[] = {"natural", "meeting", "strong"};
    save_preset(names[index]);
    const auto settings = audio::settings_for_preset(static_cast<audio::Preset>(index));
    app.updating_preferences = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.noise_suppression), settings.noise_suppression_enabled);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.noise_gate), settings.noise_gate_enabled);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.automatic_gain), settings.automatic_gain_enabled);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.compressor), settings.compressor_enabled);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.enhancement), settings.enhancement_enabled);
    gtk_range_set_value(GTK_RANGE(app.input_gain), settings.input_gain_db);
    app.updating_preferences = false;
    save_processing_toggle("noise-suppression", settings.noise_suppression_enabled);
    save_processing_toggle("noise-gate", settings.noise_gate_enabled);
    save_processing_toggle("automatic-gain", settings.automatic_gain_enabled);
    save_processing_toggle("compressor", settings.compressor_enabled);
    save_processing_toggle("enhancement-enabled", settings.enhancement_enabled);
    save_input_gain(settings.input_gain_db);
}

void on_processing_toggle(GtkToggleButton* button, gpointer user_data) {
    auto& app = *static_cast<Application*>(user_data);
    if (app.updating_preferences) return;
    const auto* key = static_cast<const char*>(g_object_get_data(G_OBJECT(button), "clearmic-setting-key"));
    if (key) save_processing_toggle(key, gtk_toggle_button_get_active(button) != FALSE);
}

void on_input_gain_changed(GtkRange* range, gpointer user_data) {
    auto& app = *static_cast<Application*>(user_data);
    const float value = static_cast<float>(gtk_range_get_value(range));
    gchar* text = g_strdup_printf("Input gain: %+.0f dB", static_cast<double>(value));
    gtk_label_set_text(GTK_LABEL(app.input_gain_label), text);
    g_free(text);
    if (!app.updating_preferences) save_input_gain(value);
}

void on_refresh(GtkButton*, gpointer data) { refresh_devices(*static_cast<Application*>(data)); }

gboolean on_device_refresh_timer(gpointer data) {
    auto& app = *static_cast<Application*>(data);
    if (!app.service && !app.test_capture && !app.restart_source) refresh_devices(app);
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
    const int preset_index = std::clamp(gtk_combo_box_get_active(GTK_COMBO_BOX(app.preset)), 0, 2);
    const char* presets[] = {"natural", "meeting", "strong"};
    gchar* gain_option = g_strdup_printf("--input-gain-db=%.0f", gtk_range_get_value(GTK_RANGE(app.input_gain)));
    GError* error = nullptr;
    app.test_capture = g_subprocess_new(static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE), &error,
                                        app.executable, "record-test", "5", original, processed,
                                        device_id.c_str(), presets[preset_index],
                                        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.enhancement)) ? "--enhancement=on" : "--enhancement=off",
                                        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.noise_suppression)) ? "--noise-suppression=on" : "--noise-suppression=off",
                                        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.noise_gate)) ? "--noise-gate=on" : "--noise-gate=off",
                                        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.automatic_gain)) ? "--automatic-gain=on" : "--automatic-gain=off",
                                        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.compressor)) ? "--compressor=on" : "--compressor=off",
                                        gain_option, nullptr);
    g_free(gain_option);
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
    gtk_widget_set_sensitive(app.preset, FALSE);
    gtk_widget_set_sensitive(app.noise_suppression, FALSE);
    gtk_widget_set_sensitive(app.noise_gate, FALSE);
    gtk_widget_set_sensitive(app.automatic_gain, FALSE);
    gtk_widget_set_sensitive(app.compressor, FALSE);
    gtk_widget_set_sensitive(app.enhancement, FALSE);
    gtk_widget_set_sensitive(app.input_gain, FALSE);
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
                try {
                    const auto original = audio::read_pcm16_wav(state.original_path);
                    const auto processed = audio::read_pcm16_wav(state.processed_path);
                    const auto input_rms = audio::rms_normalized(original);
                    const auto output_rms = audio::rms_normalized(processed);
                    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state.input_meter), input_rms);
                    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state.output_meter), output_rms);
                    char input_text[32]{};
                    char output_text[32]{};
                    std::snprintf(input_text, sizeof(input_text), "Input %.1f%% RMS", input_rms * 100.0);
                    std::snprintf(output_text, sizeof(output_text), "Processed %.1f%% RMS", output_rms * 100.0);
                    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state.input_meter), input_text);
                    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state.output_meter), output_text);
                    gchar* message = input_rms < 0.001
                        ? g_strdup_printf("A/B sample ready, but little or no microphone signal was detected (input RMS %.2f%%). Check mute, system microphone permissions, and the selected input.", input_rms * 100.0)
                        : g_strdup_printf("A/B sample ready. Microphone activity detected (input RMS %.2f%%, processed RMS %.2f%%). Play the original or processed recording.", input_rms * 100.0, output_rms * 100.0);
                    gtk_label_set_text(GTK_LABEL(state.service_status), message);
                    g_free(message);
                } catch (const std::exception& error) {
                    gtk_label_set_text(GTK_LABEL(state.service_status), error.what());
                }
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
        } else {
            float max_dsp_ms = 0.0F;
            float max_dsp_budget = 0.0F;
            unsigned long long overruns = 0;
            unsigned long long underruns = 0;
            unsigned long long processed_seconds = 0;
            float capture_graph_ms = 0.0F;
            float capture_queue_ms = 0.0F;
            float capture_buffered_ms = 0.0F;
            float source_graph_ms = 0.0F;
            float source_queue_ms = 0.0F;
            float source_buffered_ms = 0.0F;
            if (std::sscanf(line, "DIAG %f %f %llu %llu %llu %f %f %f %f %f %f",
                            &max_dsp_ms, &max_dsp_budget,
                            &overruns, &underruns, &processed_seconds,
                            &capture_graph_ms, &capture_queue_ms, &capture_buffered_ms,
                            &source_graph_ms, &source_queue_ms, &source_buffered_ms) == 11) {
                char text[320];
                const bool complete_latency = capture_graph_ms >= 0.0F && capture_queue_ms >= 0.0F && capture_buffered_ms >= 0.0F &&
                    source_graph_ms >= 0.0F && source_queue_ms >= 0.0F && source_buffered_ms >= 0.0F;
                if (complete_latency) {
                    const auto estimated_route_ms = max_dsp_ms + capture_graph_ms + capture_queue_ms + capture_buffered_ms +
                        source_graph_ms + source_queue_ms + source_buffered_ms;
                    std::snprintf(text, sizeof(text),
                        "DSP max %.3f ms (%.1f%% budget) · estimated route %.2f ms (capture %.2f + %.2f + %.2f; source %.2f + %.2f + %.2f ms) · processed %llu s · overruns %llu/%llu",
                        max_dsp_ms, max_dsp_budget, estimated_route_ms, capture_graph_ms, capture_queue_ms, capture_buffered_ms,
                        source_graph_ms, source_queue_ms, source_buffered_ms, processed_seconds, overruns, underruns);
                } else {
                    std::snprintf(text, sizeof(text),
                        "DSP max %.3f ms (%.1f%% budget) · PipeWire route latency unavailable · processed %llu s · overruns %llu/%llu",
                        max_dsp_ms, max_dsp_budget, processed_seconds, overruns, underruns);
                }
                gtk_label_set_text(GTK_LABEL(app.service_status), text);
            }
        }
        g_free(line);
        g_data_input_stream_read_line_async(app.service_output, G_PRIORITY_DEFAULT, app.output_cancel,
                                            read_service_output, &app);
        return;
    }
    g_free(line);
    if (error) g_error_free(error);
}

void launch_service(Application& app) {
    const int preset_index = std::clamp(gtk_combo_box_get_active(GTK_COMBO_BOX(app.preset)), 0, 2);
    const char* presets[] = {"natural", "meeting", "strong"};
    GError* error = nullptr;
    gchar* gain_option = g_strdup_printf("--input-gain-db=%.0f", gtk_range_get_value(GTK_RANGE(app.input_gain)));
    const gchar* arguments[] = {
        app.executable, "serve", app.active_device_id.c_str(), presets[preset_index],
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.enhancement)) ? "--enhancement=on" : "--enhancement=off",
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.noise_suppression)) ? "--noise-suppression=on" : "--noise-suppression=off",
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.noise_gate)) ? "--noise-gate=on" : "--noise-gate=off",
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.automatic_gain)) ? "--automatic-gain=on" : "--automatic-gain=off",
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.compressor)) ? "--compressor=on" : "--compressor=off",
        gain_option,
        nullptr};
    app.service = g_subprocess_newv(arguments, static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE), &error);
    g_free(gain_option);
    if (!app.service) {
        gtk_label_set_text(GTK_LABEL(app.service_status), error ? error->message : "Could not start audio service.");
        if (error) g_error_free(error);
        app.reconnect_enabled = false;
        update_controls(app);
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
            const bool exited_cleanly = g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &wait_error);
            if (state.output_cancel) g_cancellable_cancel(state.output_cancel);
            g_clear_object(&state.service_output);
            g_clear_object(&state.output_cancel);
            g_clear_object(&state.service);
            refresh_devices(state);
            if (!exited_cleanly && state.reconnect_enabled && !state.closing) {
                const int available_index = selected_index(state);
                state.active_device_id = available_index >= 0
                    ? state.inputs[static_cast<std::size_t>(available_index)].id
                    : std::string{};
                if (wait_error) g_error_free(wait_error);
                const unsigned int delay = std::min(2U << std::min(state.restart_attempt, 4U), 30U);
                ++state.restart_attempt;
                gchar* message = g_strdup_printf("Audio service stopped. Retrying in %u seconds…", delay);
                gtk_label_set_text(GTK_LABEL(state.service_status), message);
                g_free(message);
                state.restart_source = g_timeout_add_seconds(delay,
                    [](gpointer retry_data) -> gboolean {
                        auto& retry = *static_cast<Application*>(retry_data);
                        retry.restart_source = 0;
                        if (retry.reconnect_enabled && !retry.closing) launch_service(retry);
                        update_controls(retry);
                        return G_SOURCE_REMOVE;
                    }, &state);
            } else {
                gtk_label_set_text(GTK_LABEL(state.service_status), state.user_stopping || exited_cleanly
                    ? "Audio service stopped."
                    : (wait_error ? wait_error->message : "Audio service stopped with an error."));
                if (wait_error) g_error_free(wait_error);
            }
            update_controls(state);
        }, &app);
}

void on_start(GtkButton*, gpointer data) {
    auto& app = *static_cast<Application*>(data);
    const int index = selected_index(app);
    if (index < 0) return;
    app.active_device_id = app.inputs[static_cast<std::size_t>(index)].id;
    app.reconnect_enabled = true;
    app.user_stopping = false;
    app.restart_attempt = 0;
    launch_service(app);
}

void on_stop(GtkButton*, gpointer data) {
    auto& app = *static_cast<Application*>(data);
    app.reconnect_enabled = false;
    app.user_stopping = true;
    app.restart_attempt = 0;
    if (app.restart_source) {
        g_source_remove(app.restart_source);
        app.restart_source = 0;
        gtk_label_set_text(GTK_LABEL(app.service_status), "Automatic reconnect cancelled.");
    } else if (app.service) {
        g_subprocess_send_signal(app.service, SIGTERM);
        gtk_label_set_text(GTK_LABEL(app.service_status), "Stopping audio service…");
    }
    update_controls(app);
}

void show_window(Application& app) {
    gtk_widget_show_all(app.window);
    gtk_window_present(GTK_WINDOW(app.window));
}

void on_tray_activated(GtkStatusIcon*, gpointer data) {
    show_window(*static_cast<Application*>(data));
}

void on_tray_show(GtkMenuItem*, gpointer data) {
    show_window(*static_cast<Application*>(data));
}

void on_tray_toggle_processing(GtkMenuItem*, gpointer data) {
    auto& app = *static_cast<Application*>(data);
    if (app.service || app.restart_source) on_stop(nullptr, &app);
    else on_start(nullptr, &app);
}

void on_tray_quit(GtkMenuItem*, gpointer data) {
    auto& app = *static_cast<Application*>(data);
    gtk_widget_destroy(app.window);
}

void on_tray_popup(GtkStatusIcon* icon, guint button, guint activate_time, gpointer data) {
    auto& app = *static_cast<Application*>(data);
    auto* menu = gtk_menu_new();
    auto* show_item = gtk_menu_item_new_with_label("Open ClearMic");
    const bool processing_active = app.service || app.restart_source;
    auto* toggle_item = gtk_menu_item_new_with_label(processing_active ? "Stop enhancement" : "Start enhancement");
    auto* quit_item = gtk_menu_item_new_with_label("Quit ClearMic");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), show_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), toggle_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);
    g_signal_connect(show_item, "activate", G_CALLBACK(on_tray_show), data);
    gtk_widget_set_sensitive(toggle_item, processing_active || (!app.test_capture && selected_index(app) >= 0));
    g_signal_connect(toggle_item, "activate", G_CALLBACK(on_tray_toggle_processing), data);
    g_signal_connect(quit_item, "activate", G_CALLBACK(on_tray_quit), data);
    g_signal_connect(menu, "selection-done", G_CALLBACK(+[](GtkWidget* menu_widget, gpointer) {
        gtk_widget_destroy(menu_widget);
    }), nullptr);
    gtk_widget_show_all(menu);
    gtk_menu_popup(GTK_MENU(menu), nullptr, nullptr, gtk_status_icon_position_menu, icon, button, activate_time);
}

void on_background_mode_toggled(GtkToggleButton* button, gpointer data) {
    auto& app = *static_cast<Application*>(data);
    const bool enabled = gtk_toggle_button_get_active(button) != FALSE;
    save_background_mode(enabled);
    if (app.tray_icon) gtk_status_icon_set_visible(app.tray_icon, enabled);
    if (enabled) gtk_label_set_text(GTK_LABEL(app.service_status), "Closing this window will keep ClearMic available in the system tray.");
}

gboolean on_window_delete(GtkWidget* window, GdkEvent*, gpointer data) {
    auto& app = *static_cast<Application*>(data);
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.background_toggle))) return FALSE;
    gtk_widget_hide(window);
    gtk_label_set_text(GTK_LABEL(app.service_status), "ClearMic is running in the system tray. Use its menu to reopen or quit.");
    return TRUE;
}

void on_window_destroy(GtkWidget*, gpointer data) {
    auto& app = *static_cast<Application*>(data);
    app.closing = true;
    app.reconnect_enabled = false;
    app.user_stopping = true;
    if (app.restart_source) g_source_remove(app.restart_source);
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
    if (app.tray_icon) gtk_status_icon_set_visible(app.tray_icon, FALSE);
    g_clear_object(&app.tray_icon);
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

GtkWidget* make_processing_toggle(const char* label, const char* key) {
    auto* toggle = gtk_check_button_new_with_label(label);
    g_object_set_data_full(G_OBJECT(toggle), "clearmic-setting-key", g_strdup(key), g_free);
    return toggle;
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
    gtk_window_set_default_size(GTK_WINDOW(app.window), 680, 620);
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
    gtk_box_pack_start(GTK_BOX(layout), make_label("Processing profile and options (set before starting enhancement)"), FALSE, FALSE, 0);
    app.preset = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.preset), "Natural");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.preset), "Meeting");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.preset), "Strong Noise Reduction");
    gtk_box_pack_start(GTK_BOX(layout), app.preset, FALSE, FALSE, 0);
    auto* processing_controls = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(processing_controls), 4);
    gtk_grid_set_column_spacing(GTK_GRID(processing_controls), 20);
    app.noise_suppression = make_processing_toggle("Noise suppression", "noise-suppression");
    app.noise_gate = make_processing_toggle("Noise gate", "noise-gate");
    app.automatic_gain = make_processing_toggle("Automatic gain", "automatic-gain");
    app.compressor = make_processing_toggle("Compressor", "compressor");
    app.enhancement = gtk_check_button_new_with_label("Enable ClearMic enhancement");
    app.input_gain_label = make_label("Input gain: +0 dB");
    app.input_gain = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -12.0, 12.0, 1.0);
    gtk_scale_set_digits(GTK_SCALE(app.input_gain), 0);
    gtk_scale_set_value_pos(GTK_SCALE(app.input_gain), GTK_POS_RIGHT);
    gtk_widget_set_hexpand(app.input_gain, TRUE);
    auto* gain_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(gain_box), app.input_gain_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gain_box), app.input_gain, TRUE, TRUE, 0);
    gtk_grid_attach(GTK_GRID(processing_controls), app.noise_suppression, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(processing_controls), app.noise_gate, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(processing_controls), app.automatic_gain, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(processing_controls), app.compressor, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(processing_controls), app.enhancement, 0, 2, 2, 1);
    gtk_grid_attach(GTK_GRID(processing_controls), gain_box, 0, 3, 2, 1);
    gtk_box_pack_start(GTK_BOX(layout), processing_controls, FALSE, FALSE, 0);
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
    app.background_toggle = gtk_check_button_new_with_label("Keep running in the system tray when this window closes");
    gtk_box_pack_start(GTK_BOX(layout), app.background_toggle, FALSE, FALSE, 0);

    g_signal_connect(app.window, "destroy", G_CALLBACK(on_window_destroy), &app);
    g_signal_connect(app.window, "delete-event", G_CALLBACK(on_window_delete), &app);
    g_signal_connect(app.devices, "changed", G_CALLBACK(on_device_changed), &app);
    g_signal_connect(app.preset, "changed", G_CALLBACK(on_preset_changed), &app);
    g_signal_connect(app.refresh_button, "clicked", G_CALLBACK(on_refresh), &app);
    g_signal_connect(app.start_button, "clicked", G_CALLBACK(on_start), &app);
    g_signal_connect(app.stop_button, "clicked", G_CALLBACK(on_stop), &app);
    g_signal_connect(app.record_button, "clicked", G_CALLBACK(on_record_sample), &app);
    g_signal_connect(app.play_original_button, "clicked", G_CALLBACK(on_play_original), &app);
    g_signal_connect(app.play_processed_button, "clicked", G_CALLBACK(on_play_processed), &app);
    g_signal_connect(app.stop_playback_button, "clicked", G_CALLBACK(on_stop_playback), &app);
    g_signal_connect(app.background_toggle, "toggled", G_CALLBACK(on_background_mode_toggled), &app);
    g_signal_connect(app.noise_suppression, "toggled", G_CALLBACK(on_processing_toggle), &app);
    g_signal_connect(app.noise_gate, "toggled", G_CALLBACK(on_processing_toggle), &app);
    g_signal_connect(app.automatic_gain, "toggled", G_CALLBACK(on_processing_toggle), &app);
    g_signal_connect(app.compressor, "toggled", G_CALLBACK(on_processing_toggle), &app);
    g_object_set_data(G_OBJECT(app.enhancement), "clearmic-setting-key", const_cast<char*>("enhancement-enabled"));
    g_signal_connect(app.enhancement, "toggled", G_CALLBACK(on_processing_toggle), &app);
    g_signal_connect(app.input_gain, "value-changed", G_CALLBACK(on_input_gain_changed), &app);
    if (app.player) {
        GstBus* bus = gst_element_get_bus(app.player);
        app.player_bus_watch = gst_bus_add_watch(bus, on_player_message, &app);
        gst_object_unref(bus);
    } else {
        gtk_label_set_text(GTK_LABEL(app.service_status), "A/B playback is unavailable because GStreamer playbin could not load.");
    }
    app.tray_icon = gtk_status_icon_new_from_icon_name("audio-input-microphone");
    gtk_status_icon_set_title(app.tray_icon, "ClearMic");
    gtk_status_icon_set_tooltip_text(app.tray_icon, "ClearMic microphone enhancement");
    g_signal_connect(app.tray_icon, "activate", G_CALLBACK(on_tray_activated), &app);
    g_signal_connect(app.tray_icon, "popup-menu", G_CALLBACK(on_tray_popup), &app);
    const bool background_mode = stored_background_mode();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.background_toggle), background_mode);
    gtk_status_icon_set_visible(app.tray_icon, background_mode);
    const auto preset = stored_preset();
    const int preset_index = preset == "meeting" ? 1 : (preset == "strong" ? 2 : 0);
    const auto preset_settings = audio::settings_for_preset(static_cast<audio::Preset>(preset_index));
    app.updating_preferences = true;
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.preset), preset_index);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.noise_suppression),
        stored_processing_toggle("noise-suppression", preset_settings.noise_suppression_enabled));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.noise_gate),
        stored_processing_toggle("noise-gate", preset_settings.noise_gate_enabled));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.automatic_gain),
        stored_processing_toggle("automatic-gain", preset_settings.automatic_gain_enabled));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.compressor),
        stored_processing_toggle("compressor", preset_settings.compressor_enabled));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.enhancement),
        stored_processing_toggle("enhancement-enabled", preset_settings.enhancement_enabled));
    gtk_range_set_value(GTK_RANGE(app.input_gain), stored_input_gain());
    app.updating_preferences = false;
    refresh_devices(app);
    app.device_refresh_source = g_timeout_add_seconds(3, on_device_refresh_timer, &app);
    gtk_widget_show_all(app.window);
    gtk_main();
    g_free(resolved);
    return 0;
}
}
