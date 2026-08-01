#include <gtk/gtk.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "sonalis/core/c_api.h"
#include "sonalis/linux/linux_api.h"
#include "sonalis/linux/linux_voice_call.h"

namespace {

using sonalis::linux_platform::LinuxApi;

struct AppState final {
    sonalis_event_generation* events{sonalis_event_generation_create()};
    LinuxApi api;
    GtkWidget* window{};
    GtkWidget* content{};
    std::vector<sonalis::core::Room> rooms;
    std::vector<sonalis::core::Channel> channels;
    std::shared_ptr<sonalis::linux_platform::LinuxVoiceCall> voice;
    std::jthread voiceStart;
    std::atomic<bool> closing{};

    ~AppState() {
        closing.store(true, std::memory_order_release);
        if (voice) voice->Close();
        if (voiceStart.joinable()) voiceStart.request_stop();
        sonalis_event_generation_destroy(events);
    }
};

struct Action final { std::function<void()> invoke; };

void Clear(GtkWidget* box) {
    while (GtkWidget* child = gtk_widget_get_first_child(box)) gtk_box_remove(GTK_BOX(box), child);
}

GtkWidget* Label(const char* value, const char* css = nullptr) {
    GtkWidget* label = gtk_label_new(value);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    if (css != nullptr) gtk_widget_add_css_class(label, css);
    return label;
}

GtkWidget* Button(const char* label, std::function<void()> callback, const bool primary = false) {
    GtkWidget* button = gtk_button_new_with_label(label);
    if (primary) gtk_widget_add_css_class(button, "suggested-action");
    auto* action = new Action{std::move(callback)};
    g_signal_connect_data(button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer value) {
        static_cast<Action*>(value)->invoke();
    }), action, +[](gpointer value, GClosure*) { delete static_cast<Action*>(value); }, GConnectFlags(0));
    return button;
}

void MainThread(AppState& state, std::function<void()> callback) {
    if (state.closing.load(std::memory_order_acquire)) return;
    auto* action = new Action{[&state, callback = std::move(callback)] {
        if (!state.closing.load(std::memory_order_acquire)) callback();
    }};
    g_main_context_invoke(nullptr, +[](gpointer value) -> gboolean {
        std::unique_ptr<Action> action(static_cast<Action*>(value));
        action->invoke();
        return G_SOURCE_REMOVE;
    }, action);
}

void ShowLogin(AppState& state, std::string error = {});
void LoadRooms(AppState& state);
void ShowChannels(AppState& state, sonalis::core::Room room,
                  std::vector<sonalis::core::Channel> channels);

void ShowStatus(AppState& state, const char* text) {
    Clear(state.content);
    gtk_box_append(GTK_BOX(state.content), Label("SONALIS", "title-1"));
    gtk_box_append(GTK_BOX(state.content), Label(text, "dim-label"));
}

void ShowMessages(AppState& state, sonalis::core::Room room, sonalis::core::Channel channel,
                  std::vector<sonalis::linux_platform::LinuxEncryptedMessage> messages) {
    Clear(state.content);
    gtk_box_append(GTK_BOX(state.content), Button("Kanallara dön", [&state, room = std::move(room)] {
        ShowStatus(state, "Kanallar yükleniyor…");
        state.api.Channels(room.id, [&state, room](auto channels, std::string error) mutable {
            MainThread(state, [&state, room = std::move(room), channels = std::move(channels), error = std::move(error)]() mutable {
                if (!error.empty()) { LoadRooms(state); return; }
                ShowChannels(state, std::move(room), std::move(channels));
            });
        });
    }));
    const std::string title = "# " + channel.name;
    gtk_box_append(GTK_BOX(state.content), Label(title.c_str(), "title-1"));
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    GtkWidget* list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    for (const auto& message : messages) {
        const std::string row = message.senderId.substr(0, 8) + "  ·  " + message.createdAt
            + "\nŞifreli mesaj — içerik yalnız cihaz anahtarıyla açılır";
        GtkWidget* card = Label(row.c_str());
        gtk_widget_add_css_class(card, "card");
        gtk_box_append(GTK_BOX(list), card);
    }
    if (messages.empty()) gtk_box_append(GTK_BOX(list), Label("Bu kanalda henüz mesaj yok.", "dim-label"));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_box_append(GTK_BOX(state.content), scroll);
}

void ShowChannels(AppState& state, sonalis::core::Room room, std::vector<sonalis::core::Channel> channels) {
    state.channels = channels;
    Clear(state.content);
    gtk_box_append(GTK_BOX(state.content), Label(room.name.c_str(), "title-1"));
    gtk_box_append(GTK_BOX(state.content), Button("Odalara dön", [&state] { LoadRooms(state); }));
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    GtkWidget* list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    for (const auto& channel : channels) {
        std::string title = channel.kind == sonalis::core::ChannelKind::Voice ? "Ses  " : "#  ";
        title += channel.name;
        if (channel.mentionCount > 0) title += "  @" + std::to_string(channel.mentionCount);
        gtk_box_append(GTK_BOX(list), Button(title.c_str(), [&state, room, channel] {
            if (channel.kind == sonalis::core::ChannelKind::Voice) {
                ShowStatus(state, "Güvenli ses bileti alınıyor…");
                state.api.VoiceGrant(room.id, channel.id, false, false,
                    [&state, room, channel](auto grant, std::string error) mutable {
                        MainThread(state, [&state, room = std::move(room), channel = std::move(channel),
                                           grant = std::move(grant), error = std::move(error)]() mutable {
                            if (!error.empty()) { ShowChannels(state, std::move(room), state.channels); return; }
                            auto call = std::make_shared<sonalis::linux_platform::LinuxVoiceCall>(
                                [&state](std::string voiceState) {
                                    if (voiceState == "sleeping") {
                                        MainThread(state, [&state] {
                                            gtk_box_append(GTK_BOX(state.content),
                                                Label("Voice channel is sleeping.", "dim-label"));
                                        });
                                    }
                                });
                            std::string connectError;
                            if (!call->Connect(grant, connectError)) {
                                ShowChannels(state, std::move(room), state.channels);
                                const std::string failure = "Voice connection failed: " + connectError;
                                gtk_box_append(GTK_BOX(state.content), Label(failure.c_str(), "error"));
                                return;
                            }
                            if (state.voice) state.voice->Close();
                            state.voice = std::move(call);
                            const std::string status = "Ses düğümü doğrulandı: " + grant.host + ":"
                                + std::to_string(grant.port) + " · " + grant.routeType;
                            ShowChannels(state, std::move(room), state.channels);
                            gtk_box_append(GTK_BOX(state.content), Label(status.c_str(), "success"));
                            gtk_box_append(GTK_BOX(state.content), Button("Ses kanalÄ±ndan ayrÄ±l", [&state] {
                                if (state.voice) state.voice->Close();
                                state.voice.reset();
                            }));
                        });
                    });
                return;
            }
            ShowStatus(state, "Şifreli mesajlar yükleniyor…");
            state.api.Messages(channel.id, {}, [&state, room, channel](auto messages, std::string error) mutable {
                MainThread(state, [&state, room = std::move(room), channel = std::move(channel),
                                   messages = std::move(messages), error = std::move(error)]() mutable {
                    if (!error.empty()) { ShowChannels(state, std::move(room), state.channels); return; }
                    ShowMessages(state, std::move(room), std::move(channel), std::move(messages));
                });
            });
        }));
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_box_append(GTK_BOX(state.content), scroll);
}

void ShowRooms(AppState& state, std::vector<sonalis::core::Room> rooms) {
    state.rooms = rooms;
    Clear(state.content);
    gtk_box_append(GTK_BOX(state.content), Label("Odalar", "title-1"));
    gtk_box_append(GTK_BOX(state.content), Label("Üyesi olduğunuz topluluklar", "dim-label"));
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    GtkWidget* list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    for (const auto& room : rooms) {
        const std::string title = room.name + "  ·  " + room.role;
        gtk_box_append(GTK_BOX(list), Button(title.c_str(), [&state, room] {
            ShowStatus(state, "Kanallar yükleniyor…");
            state.api.Channels(room.id, [&state, room](auto channels, std::string error) mutable {
                MainThread(state, [&state, room = std::move(room), channels = std::move(channels),
                                   error = std::move(error)]() mutable {
                    if (!error.empty()) { LoadRooms(state); return; }
                    ShowChannels(state, std::move(room), std::move(channels));
                });
            });
        }));
    }
    if (rooms.empty()) gtk_box_append(GTK_BOX(list), Label("Henüz üyesi olduğunuz bir oda yok.", "dim-label"));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_box_append(GTK_BOX(state.content), scroll);
    gtk_box_append(GTK_BOX(state.content), Button("Oturumu kapat", [&state] {
        ShowStatus(state, "Oturum kapatılıyor…");
        state.api.Logout([&state](bool, std::string) { MainThread(state, [&state] { ShowLogin(state); }); });
    }));
}

void LoadRooms(AppState& state) {
    ShowStatus(state, "Odalar yükleniyor…");
    state.api.Rooms([&state](auto rooms, std::string error) mutable {
        MainThread(state, [&state, rooms = std::move(rooms), error = std::move(error)]() mutable {
            if (!error.empty()) { ShowLogin(state, "Oturum yenilenemedi."); return; }
            ShowRooms(state, std::move(rooms));
        });
    });
}

void ShowLogin(AppState& state, std::string error) {
    Clear(state.content);
    gtk_box_append(GTK_BOX(state.content), Label("Sonalis hesabına giriş", "title-1"));
    GtkWidget* login = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(login), "Kullanıcı adı veya e-posta");
    GtkWidget* password = gtk_password_entry_new();
    g_object_set(password, "placeholder-text", "Parola", nullptr);
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(password), TRUE);
    gtk_box_append(GTK_BOX(state.content), login);
    gtk_box_append(GTK_BOX(state.content), password);
    if (!error.empty()) gtk_box_append(GTK_BOX(state.content), Label(error.c_str(), "error"));
    gtk_box_append(GTK_BOX(state.content), Button("Giriş yap", [&state, login, password] {
        std::string username = gtk_editable_get_text(GTK_EDITABLE(login));
        std::string secret = gtk_editable_get_text(GTK_EDITABLE(password));
        gtk_editable_set_text(GTK_EDITABLE(password), "");
        if (username.empty() || secret.empty()) return;
        ShowStatus(state, "Oturum doğrulanıyor…");
        state.api.Login(std::move(username), std::move(secret), [&state](bool ok, std::string failure) {
            MainThread(state, [&state, ok, failure = std::move(failure)]() mutable {
                if (ok) LoadRooms(state); else ShowLogin(state, failure == "login_failed"
                    ? "Kullanıcı adı veya parola doğrulanamadı." : "Giriş tamamlanamadı.");
            });
        });
    }, true));
    gtk_box_append(GTK_BOX(state.content), Label(
        "Parola saklanmaz. Yenileme anahtarı Secret Service içinde korunur.", "dim-label"));
}

void Activate(GtkApplication* application, gpointer userData) {
    auto& state = *static_cast<AppState*>(userData);
    (void)sonalis_event_generation_mark_dirty(state.events);
    state.window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(state.window), "Sonalis");
    gtk_window_set_default_size(GTK_WINDOW(state.window), 1'240, 780);
    gtk_window_set_resizable(GTK_WINDOW(state.window), TRUE);
    state.content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(state.content, 28);
    gtk_widget_set_margin_end(state.content, 28);
    gtk_widget_set_margin_top(state.content, 28);
    gtk_widget_set_margin_bottom(state.content, 20);
    gtk_window_set_child(GTK_WINDOW(state.window), state.content);
    ShowStatus(state, "Güvenli oturum yükleniyor…");
    state.api.Restore([&state](bool restored, std::string) {
        MainThread(state, [&state, restored] { if (restored) LoadRooms(state); else ShowLogin(state); });
    });
    gtk_window_present(GTK_WINDOW(state.window));
}

}  // namespace

int main(int argc, char** argv) {
    if (sonalis_core_abi_version() != SONALIS_CORE_ABI_VERSION) return 70;
    AppState state;
    if (state.events == nullptr) return 71;
    GtkApplication* application = gtk_application_new("tr.sonalis.desktop", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(Activate), &state);
    const int result = g_application_run(G_APPLICATION(application), argc, argv);
    state.closing.store(true, std::memory_order_release);
    g_object_unref(application);
    return result;
}
