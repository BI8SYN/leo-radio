#include "radio_catalog.h"
#include "radio_display_settings.h"
#include "radio_easter_egg.h"
#include "fap_screenshot.h"
#include "radio_player.h"
#include "radio_sleep_timer.h"
#include "radio_ui.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ssid_manager.h"
#include "wifi_manager.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

namespace {

constexpr char kTag[] = "internet_radio";
struct KeyMessage {
    bsp_btn_t button;
    bsp_btn_ev_t event;
};

enum class ConfigInputMode {
    NetworkList,
    Keyboard,
    Connecting,
};

enum class SettingsInputMode {
    Menu,
    Volume,
    City,
    SleepTimer,
    Display,
    WifiMenu,
    SavedWifi,
    WifiAction,
    WifiDeleteConfirm,
};

struct CityChoice {
    const char *query;
    const char *display;
};

constexpr CityChoice kCityChoices[] = {
    {nullptr, "AUTO"},
    {"北京", "BEIJING"},
    {"上海", "SHANGHAI"},
    {"广州", "GUANGZHOU"},
    {"深圳", "SHENZHEN"},
    {"长沙", "CHANGSHA"},
    {"杭州", "HANGZHOU"},
    {"成都", "CHENGDU"},
};

constexpr uint16_t kSleepTimerMinutes[] = {0, 15, 30, 60, 90};

constexpr std::size_t kMaxVisibleNetworks = 12;

QueueHandle_t s_key_queue;
std::atomic<bool> s_config_mode{false};
std::atomic<bool> s_connected{false};
std::atomic<bool> s_settings_mode{false};
std::atomic<bool> s_sntp_started{false};
std::atomic<bool> s_config_can_cancel{false};
TaskHandle_t s_catalog_task;
TaskHandle_t s_wifi_scan_task;
TaskHandle_t s_wifi_connect_task;
std::mutex s_wifi_ui_mutex;
ConfigInputMode s_config_input_mode = ConfigInputMode::NetworkList;
std::array<WifiNetworkView, kMaxVisibleNetworks> s_wifi_networks = {};
std::size_t s_wifi_network_count;
std::size_t s_wifi_network_selected;
std::string s_wifi_selected_ssid;
std::string s_wifi_password;
WifiKeyboardPage s_wifi_keyboard_page = WifiKeyboardPage::Lower;
std::size_t s_wifi_key_selected;
std::string s_config_ap_ssid;
SettingsInputMode s_settings_input_mode = SettingsInputMode::Menu;
std::size_t s_settings_selected;
std::size_t s_saved_wifi_index;
std::mutex s_catalog_mutex;
uint32_t s_catalog_generation;
char s_requested_city[16];
char s_requested_city_display[16];

void start_local_catalog_search();
void start_city_catalog_search(const char *city, const char *display);
void start_remembered_catalog_search();

// Persists the city the listener picked in Settings so a restart comes back to
// the same place instead of relocating from scratch every time.
void save_selected_city(std::size_t choice_index) {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, "city", static_cast<uint8_t>(choice_index));
    nvs_commit(handle);
    nvs_close(handle);
}

std::size_t load_selected_city() {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READONLY, &handle) != ESP_OK) return 0;
    uint8_t value = 0;
    if (nvs_get_u8(handle, "city", &value) != ESP_OK) value = 0;
    nvs_close(handle);
    const std::size_t count = sizeof(kCityChoices) / sizeof(kCityChoices[0]);
    return value < count ? value : 0;
}

// Starts the search for whichever city was last chosen, falling back to
// automatic detection the first time the radio is used.
void start_remembered_catalog_search() {
    const CityChoice &choice = kCityChoices[load_selected_city()];
    if (choice.query) {
        start_city_catalog_search(choice.query, choice.display);
    } else {
        start_local_catalog_search();
    }
}

int button_from_mv(int millivolts) {
    if (millivolts < 0 || millivolts >= 1900) return -1;
    if (millivolts < 150) return BSP_BTN_UP;
    if (millivolts < 447) return BSP_BTN_DOWN;
    return BSP_BTN_OK;
}

void button_task(void *) {
    int candidate = -1;
    int stable = -1;
    uint8_t samples = 0;
    TickType_t pressed_at = 0;
    bool long_sent = false;

    while (true) {
        const int current = button_from_mv(bsp_button_read_mv());
        if (current == candidate) {
            if (samples < 3) ++samples;
        } else {
            candidate = current;
            samples = 1;
        }

        if (samples >= 3 && candidate != stable) {
            const int previous = stable;
            stable = candidate;
            if (stable >= 0) {
                pressed_at = xTaskGetTickCount();
                long_sent = false;
            } else if (previous >= 0 && !long_sent) {
                const KeyMessage message = {static_cast<bsp_btn_t>(previous), BSP_BTN_CLICK};
                xQueueSend(s_key_queue, &message, 0);
            }
        }

        if (stable >= 0 && !long_sent &&
            xTaskGetTickCount() - pressed_at >= pdMS_TO_TICKS(850)) {
            const KeyMessage message = {static_cast<bsp_btn_t>(stable), BSP_BTN_LONG};
            xQueueSend(s_key_queue, &message, 0);
            long_sent = true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void enter_wifi_config(bool clear_saved_networks, bool allow_cancel = false) {
    if (s_config_mode.load()) return;
    ESP_LOGI(kTag, "Entering Wi-Fi configuration mode");
    radio_player_set_network(false);
    if (clear_saved_networks) SsidManager::GetInstance().Clear();
    s_config_can_cancel.store(allow_cancel &&
                              !SsidManager::GetInstance().GetSsidList().empty());
    WifiManager::GetInstance().StartConfigAp();
}

void show_saved_wifi() {
    const auto &saved = SsidManager::GetInstance().GetSsidList();
    std::array<const char *, 10> names = {};
    const std::size_t count = std::min(saved.size(), names.size());
    for (std::size_t i = 0; i < count; ++i) names[i] = saved[i].ssid.c_str();
    s_settings_selected = std::min(s_settings_selected, count);
    radio_ui_show_saved_wifi(names.data(), count, s_settings_selected);
}

std::string saved_wifi_ssid(std::size_t index) {
    const auto &saved = SsidManager::GetInstance().GetSsidList();
    return index < saved.size() ? saved[index].ssid : std::string();
}

void show_wifi_action() {
    const std::string ssid = saved_wifi_ssid(s_saved_wifi_index);
    if (ssid.empty()) {
        s_settings_input_mode = SettingsInputMode::SavedWifi;
        s_settings_selected = 0;
        show_saved_wifi();
        return;
    }
    radio_ui_show_wifi_action(ssid.c_str(), s_settings_selected);
}

void show_wifi_delete_confirm() {
    const std::string ssid = saved_wifi_ssid(s_saved_wifi_index);
    if (ssid.empty()) {
        s_settings_input_mode = SettingsInputMode::SavedWifi;
        s_settings_selected = 0;
        show_saved_wifi();
        return;
    }
    radio_ui_show_wifi_delete_confirm(ssid.c_str(), s_settings_selected);
}

void show_wifi_list(bool scanning) {
    std::array<WifiNetworkView, kMaxVisibleNetworks> networks = {};
    std::size_t count = 0;
    std::size_t selected = 0;
    std::string setup_ssid;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        networks = s_wifi_networks;
        count = s_wifi_network_count;
        selected = count ? std::min(s_wifi_network_selected, count - 1) : 0;
        setup_ssid = s_config_ap_ssid;
    }
    radio_ui_show_wifi_list(networks.data(), count, selected, scanning,
                            setup_ssid.c_str(), s_config_can_cancel.load());
}

void show_wifi_keyboard(const char *status = nullptr) {
    std::string ssid;
    std::size_t password_length = 0;
    WifiKeyboardPage page;
    std::size_t selected = 0;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        ssid = s_wifi_selected_ssid;
        password_length = s_wifi_password.size();
        page = s_wifi_keyboard_page;
        selected = s_wifi_key_selected;
    }
    radio_ui_show_wifi_keyboard(ssid.c_str(), password_length, page, selected, status);
}

void wifi_scan_task(void *) {
    for (int attempt = 0; attempt < 16 && s_config_mode.load(); ++attempt) {
        auto access_points = WifiManager::GetInstance().GetConfigAccessPoints();
        if (!access_points.empty()) {
            std::sort(access_points.begin(), access_points.end(),
                      [](const wifi_ap_record_t &left, const wifi_ap_record_t &right) {
                          return left.rssi > right.rssi;
                      });
            std::array<WifiNetworkView, kMaxVisibleNetworks> networks = {};
            std::size_t count = 0;
            for (const auto &access_point : access_points) {
                const char *ssid = reinterpret_cast<const char *>(access_point.ssid);
                if (!ssid[0]) continue;
                bool duplicate = false;
                for (std::size_t i = 0; i < count; ++i) {
                    if (std::strncmp(networks[i].ssid, ssid,
                                     sizeof(networks[i].ssid)) == 0) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate || count >= networks.size()) continue;
                std::snprintf(networks[count].ssid, sizeof(networks[count].ssid),
                              "%.*s", 32, ssid);
                networks[count].rssi = access_point.rssi;
                networks[count].secure = access_point.authmode != WIFI_AUTH_OPEN;
                ++count;
            }
            if (count) {
                {
                    std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
                    s_wifi_networks = networks;
                    s_wifi_network_count = count;
                    s_wifi_network_selected = 0;
                }
                show_wifi_list(false);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(750));
    }
    bool show_empty_list = false;
    if (s_config_mode.load()) {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        show_empty_list = s_wifi_network_count == 0;
    }
    if (show_empty_list && s_config_mode.load()) show_wifi_list(false);
    s_wifi_scan_task = nullptr;
    vTaskDelete(nullptr);
}

void start_wifi_scan_ui() {
    if (s_wifi_scan_task) return;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        s_wifi_network_count = 0;
        s_wifi_network_selected = 0;
    }
    show_wifi_list(true);
    if (xTaskCreate(wifi_scan_task, "wifi_screen_scan", 4096, nullptr, 4,
                    &s_wifi_scan_task) != pdPASS) {
        s_wifi_scan_task = nullptr;
        show_wifi_list(false);
    }
}

void wifi_connect_task(void *) {
    std::string ssid;
    std::string password;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        ssid = s_wifi_selected_ssid;
        password = s_wifi_password;
    }
    const bool connected = WifiManager::GetInstance().ConfigureNetwork(ssid, password);
    if (!connected && s_config_mode.load()) {
        {
            std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
            s_config_input_mode = ConfigInputMode::Keyboard;
        }
        show_wifi_keyboard("连接失败");
    }
    s_wifi_connect_task = nullptr;
    vTaskDelete(nullptr);
}

void start_wifi_connection() {
    if (s_wifi_connect_task) return;
    std::string ssid;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        if (s_wifi_selected_ssid.empty()) return;
        s_config_input_mode = ConfigInputMode::Connecting;
        ssid = s_wifi_selected_ssid;
    }
    radio_ui_show_wifi_connecting(ssid.c_str(), "正在连接");
    if (xTaskCreate(wifi_connect_task, "wifi_screen_join", 5120, nullptr, 5,
                    &s_wifi_connect_task) != pdPASS) {
        s_wifi_connect_task = nullptr;
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        s_config_input_mode = ConfigInputMode::Keyboard;
    }
}

void handle_config_input(const KeyMessage &message) {
    ConfigInputMode mode;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        mode = s_config_input_mode;
    }
    if (mode == ConfigInputMode::Connecting) return;

    if (mode == ConfigInputMode::NetworkList) {
        if (message.event == BSP_BTN_LONG && message.button == BSP_BTN_OK) {
            if (s_config_can_cancel.load(std::memory_order_acquire)) {
                WifiManager::GetInstance().StopConfigAp();
            } else {
                start_wifi_scan_ui();
            }
            return;
        }
        bool redraw = false;
        bool open_keyboard = false;
        {
            std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
            if (s_wifi_network_count == 0) return;
            if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_UP) {
                s_wifi_network_selected =
                    (s_wifi_network_selected + s_wifi_network_count - 1) %
                    s_wifi_network_count;
                redraw = true;
            } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_DOWN) {
                s_wifi_network_selected =
                    (s_wifi_network_selected + 1) % s_wifi_network_count;
                redraw = true;
            } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_OK) {
                s_wifi_selected_ssid = s_wifi_networks[s_wifi_network_selected].ssid;
                s_wifi_password.clear();
                s_wifi_keyboard_page = WifiKeyboardPage::Lower;
                s_wifi_key_selected = 0;
                s_config_input_mode = ConfigInputMode::Keyboard;
                open_keyboard = true;
            }
        }
        if (redraw) show_wifi_list(false);
        if (open_keyboard) show_wifi_keyboard();
        return;
    }

    if (message.event == BSP_BTN_LONG && message.button == BSP_BTN_OK) {
        start_wifi_connection();
        return;
    }

    bool redraw = false;
    bool connect = false;
    bool back_to_list = false;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        const std::size_t count = radio_ui_wifi_key_count(s_wifi_keyboard_page);
        if (!count) return;
        if (message.button == BSP_BTN_UP && message.event == BSP_BTN_CLICK) {
            s_wifi_key_selected = (s_wifi_key_selected + count - 1) % count;
            redraw = true;
        } else if (message.button == BSP_BTN_DOWN && message.event == BSP_BTN_CLICK) {
            s_wifi_key_selected = (s_wifi_key_selected + 1) % count;
            redraw = true;
        } else if (message.button == BSP_BTN_UP && message.event == BSP_BTN_LONG) {
            s_wifi_key_selected = (s_wifi_key_selected + count - std::min<std::size_t>(6, count)) % count;
            redraw = true;
        } else if (message.button == BSP_BTN_DOWN && message.event == BSP_BTN_LONG) {
            s_wifi_key_selected = (s_wifi_key_selected + std::min<std::size_t>(6, count)) % count;
            redraw = true;
        } else if (message.button == BSP_BTN_OK && message.event == BSP_BTN_CLICK) {
            const char *key = radio_ui_wifi_key(s_wifi_keyboard_page, s_wifi_key_selected);
            if (std::strcmp(key, "ABC") == 0) {
                s_wifi_keyboard_page = WifiKeyboardPage::Upper;
                s_wifi_key_selected = 0;
            } else if (std::strcmp(key, "abc") == 0) {
                s_wifi_keyboard_page = WifiKeyboardPage::Lower;
                s_wifi_key_selected = 0;
            } else if (std::strcmp(key, "123") == 0) {
                s_wifi_keyboard_page = WifiKeyboardPage::Symbols;
                s_wifi_key_selected = 0;
            } else if (std::strcmp(key, "<") == 0) {
                if (!s_wifi_password.empty()) s_wifi_password.pop_back();
                else {
                    s_config_input_mode = ConfigInputMode::NetworkList;
                    back_to_list = true;
                }
            } else if (std::strcmp(key, "GO") == 0) {
                connect = true;
            } else if (s_wifi_password.size() < 63) {
                s_wifi_password += key;
            }
            redraw = true;
        }
    }
    if (back_to_list) show_wifi_list(false);
    else if (connect) start_wifi_connection();
    else if (redraw) show_wifi_keyboard();
}

void input_task(void *) {
    KeyMessage message;
    while (true) {
        if (xQueueReceive(s_key_queue, &message, portMAX_DELAY) != pdTRUE) continue;
        if (!radio_display_note_activity()) continue;
        if (radio_easter_egg_is_playing()) {
            radio_easter_egg_stop();
            continue;
        }
        if (s_config_mode.load(std::memory_order_acquire)) {
            handle_config_input(message);
            continue;
        }

        if (s_settings_mode.load(std::memory_order_acquire)) {
            const auto redraw_settings = []() {
                switch (s_settings_input_mode) {
                    case SettingsInputMode::Menu:
                        radio_ui_show_settings(RadioSettingsPage::Menu,
                                               s_settings_selected,
                                               radio_player_volume());
                        break;
                    case SettingsInputMode::Volume:
                        radio_ui_show_settings(RadioSettingsPage::Volume, 0,
                                               radio_player_volume());
                        break;
                    case SettingsInputMode::City:
                        radio_ui_show_settings(RadioSettingsPage::City,
                                               s_settings_selected,
                                               radio_player_volume());
                        break;
                    case SettingsInputMode::SleepTimer:
                        radio_ui_show_settings(RadioSettingsPage::SleepTimer,
                                               s_settings_selected,
                                               radio_player_volume());
                        break;
                    case SettingsInputMode::Display:
                        radio_ui_show_settings(RadioSettingsPage::Display,
                                               s_settings_selected,
                                               radio_player_volume());
                        break;
                    case SettingsInputMode::WifiMenu:
                        radio_ui_show_settings(RadioSettingsPage::WifiMenu,
                                               s_settings_selected,
                                               radio_player_volume());
                        break;
                    case SettingsInputMode::SavedWifi:
                        show_saved_wifi();
                        break;
                    case SettingsInputMode::WifiAction:
                        show_wifi_action();
                        break;
                    case SettingsInputMode::WifiDeleteConfirm:
                        show_wifi_delete_confirm();
                        break;
                }
            };

            if (message.event == BSP_BTN_LONG && message.button == BSP_BTN_OK) {
                switch (s_settings_input_mode) {
                    case SettingsInputMode::Menu:
                        s_settings_mode.store(false);
                        radio_ui_show_main();
                        break;
                    case SettingsInputMode::Volume:
                    case SettingsInputMode::City:
                    case SettingsInputMode::SleepTimer:
                    case SettingsInputMode::Display:
                    case SettingsInputMode::WifiMenu: {
                        const std::size_t menu_selection =
                            s_settings_input_mode == SettingsInputMode::Volume ? 0 :
                            s_settings_input_mode == SettingsInputMode::City ? 1 :
                            s_settings_input_mode == SettingsInputMode::SleepTimer ? 2 :
                            s_settings_input_mode == SettingsInputMode::Display ? 3 : 4;
                        s_settings_input_mode = SettingsInputMode::Menu;
                        s_settings_selected = menu_selection;
                        redraw_settings();
                        break;
                    }
                    case SettingsInputMode::SavedWifi:
                        s_settings_input_mode = SettingsInputMode::WifiMenu;
                        s_settings_selected = 1;
                        redraw_settings();
                        break;
                    case SettingsInputMode::WifiAction:
                        s_settings_input_mode = SettingsInputMode::SavedWifi;
                        s_settings_selected = s_saved_wifi_index;
                        redraw_settings();
                        break;
                    case SettingsInputMode::WifiDeleteConfirm:
                        s_settings_input_mode = SettingsInputMode::WifiAction;
                        s_settings_selected = 1;
                        redraw_settings();
                        break;
                }
                continue;
            }

            if (s_settings_input_mode == SettingsInputMode::Volume) {
                if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_UP) {
                    radio_player_set_volume(radio_player_volume() + 5);
                } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_DOWN) {
                    const uint8_t volume = radio_player_volume();
                    radio_player_set_volume(volume > 15 ? volume - 5 : 10);
                } else if (message.event == BSP_BTN_CLICK &&
                           message.button == BSP_BTN_OK) {
                    s_settings_input_mode = SettingsInputMode::Menu;
                    s_settings_selected = 0;
                    radio_ui_show_settings(RadioSettingsPage::Menu, 0,
                                           radio_player_volume());
                }
                continue;
            }

            std::size_t count =
                s_settings_input_mode == SettingsInputMode::Menu ? 7 : 6;
            if (s_settings_input_mode == SettingsInputMode::City) {
                count = 8;
            }
            if (s_settings_input_mode == SettingsInputMode::Display ||
                s_settings_input_mode == SettingsInputMode::WifiMenu ||
                s_settings_input_mode == SettingsInputMode::WifiAction) {
                count = 3;
            } else if (s_settings_input_mode == SettingsInputMode::WifiDeleteConfirm) {
                count = 2;
            } else if (s_settings_input_mode == SettingsInputMode::SavedWifi) {
                count = SsidManager::GetInstance().GetSsidList().size() + 1;
            }
            if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_UP) {
                s_settings_selected = (s_settings_selected + count - 1) % count;
            } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_DOWN) {
                s_settings_selected = (s_settings_selected + 1) % count;
            } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_OK) {
                if (s_settings_input_mode == SettingsInputMode::Menu) {
                    if (s_settings_selected == 0) {
                        s_settings_input_mode = SettingsInputMode::Volume;
                        radio_ui_show_settings(RadioSettingsPage::Volume, 0,
                                               radio_player_volume());
                    } else if (s_settings_selected == 1) {
                        s_settings_input_mode = SettingsInputMode::City;
                        s_settings_selected = load_selected_city();
                        radio_ui_show_settings(RadioSettingsPage::City,
                                               s_settings_selected,
                                               radio_player_volume());
                    } else if (s_settings_selected == 2) {
                        s_settings_input_mode = SettingsInputMode::SleepTimer;
                        s_settings_selected = radio_sleep_timer_selection();
                        radio_ui_show_settings(RadioSettingsPage::SleepTimer,
                                               s_settings_selected,
                                               radio_player_volume());
                    } else if (s_settings_selected == 3) {
                        s_settings_input_mode = SettingsInputMode::Display;
                        s_settings_selected = 0;
                        redraw_settings();
                    } else if (s_settings_selected == 4) {
                        s_settings_input_mode = SettingsInputMode::WifiMenu;
                        s_settings_selected = 0;
                        redraw_settings();
                    } else if (s_settings_selected == 5) {
                        s_settings_mode.store(false);
                        if (!radio_easter_egg_start()) radio_ui_show_main();
                    } else {
                        s_settings_mode.store(false);
                        radio_ui_show_main();
                    }
                } else if (s_settings_input_mode == SettingsInputMode::SleepTimer) {
                    if (s_settings_selected < 5) {
                        radio_sleep_timer_set_minutes(
                            kSleepTimerMinutes[s_settings_selected]);
                    }
                    s_settings_input_mode = SettingsInputMode::Menu;
                    s_settings_selected = 2;
                    radio_ui_show_settings(RadioSettingsPage::Menu, 2,
                                           radio_player_volume());
                } else if (s_settings_input_mode == SettingsInputMode::City &&
                           s_settings_selected < 8) {
                    const CityChoice &choice = kCityChoices[s_settings_selected];
                    save_selected_city(s_settings_selected);
                    s_settings_mode.store(false);
                    radio_ui_show_main();
                    if (choice.query) {
                        start_city_catalog_search(choice.query, choice.display);
                    } else {
                        start_local_catalog_search();
                    }
                } else if (s_settings_input_mode == SettingsInputMode::Display) {
                    if (s_settings_selected == 0) {
                        radio_display_set_brightness_selection(
                            radio_display_brightness_selection() + 1);
                    } else if (s_settings_selected == 1) {
                        radio_display_set_timeout_selection(
                            radio_display_timeout_selection() + 1);
                    } else {
                        s_settings_input_mode = SettingsInputMode::Menu;
                        s_settings_selected = 3;
                    }
                    redraw_settings();
                } else if (s_settings_input_mode == SettingsInputMode::WifiMenu) {
                    if (s_settings_selected == 0) {
                        s_settings_mode.store(false);
                        enter_wifi_config(false, true);
                    } else if (s_settings_selected == 1) {
                        s_settings_input_mode = SettingsInputMode::SavedWifi;
                        s_settings_selected = 0;
                        redraw_settings();
                    } else {
                        s_settings_input_mode = SettingsInputMode::Menu;
                        s_settings_selected = 4;
                        redraw_settings();
                    }
                } else if (s_settings_input_mode == SettingsInputMode::SavedWifi) {
                    const std::size_t saved_count =
                        SsidManager::GetInstance().GetSsidList().size();
                    if (s_settings_selected < saved_count) {
                        s_saved_wifi_index = s_settings_selected;
                        s_settings_input_mode = SettingsInputMode::WifiAction;
                        s_settings_selected = 0;
                        redraw_settings();
                    } else {
                        s_settings_input_mode = SettingsInputMode::WifiMenu;
                        s_settings_selected = 1;
                        redraw_settings();
                    }
                } else if (s_settings_input_mode == SettingsInputMode::WifiAction) {
                    if (s_settings_selected == 0) {
                        SsidManager::GetInstance().SetDefaultSsid(
                            static_cast<int>(s_saved_wifi_index));
                        s_settings_input_mode = SettingsInputMode::SavedWifi;
                        s_settings_selected = 0;
                        redraw_settings();
                    } else if (s_settings_selected == 1) {
                        s_settings_input_mode = SettingsInputMode::WifiDeleteConfirm;
                        s_settings_selected = 0;
                        redraw_settings();
                    } else {
                        s_settings_input_mode = SettingsInputMode::SavedWifi;
                        s_settings_selected = s_saved_wifi_index;
                        redraw_settings();
                    }
                } else if (s_settings_input_mode ==
                           SettingsInputMode::WifiDeleteConfirm) {
                    if (s_settings_selected == 1) {
                        SsidManager::GetInstance().RemoveSsid(
                            static_cast<int>(s_saved_wifi_index));
                        const std::size_t saved_count =
                            SsidManager::GetInstance().GetSsidList().size();
                        s_settings_input_mode = SettingsInputMode::SavedWifi;
                        s_settings_selected =
                            std::min(s_saved_wifi_index, saved_count);
                    } else {
                        s_settings_input_mode = SettingsInputMode::WifiAction;
                        s_settings_selected = 1;
                    }
                    redraw_settings();
                }
                continue;
            }

            redraw_settings();
            continue;
        }

        if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_UP) {
            radio_player_select_relative(-1);
        } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_DOWN) {
            radio_player_select_relative(1);
        } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_OK) {
            radio_player_toggle();
        } else if (message.event == BSP_BTN_LONG && message.button == BSP_BTN_OK) {
            s_settings_mode.store(true);
            s_settings_input_mode = SettingsInputMode::Menu;
            s_settings_selected = 0;
            radio_ui_show_settings(RadioSettingsPage::Menu, 0,
                                   radio_player_volume());
        }
    }
}

void start_sntp_once() {
    bool expected = false;
    if (!s_sntp_started.compare_exchange_strong(expected, true)) return;
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    config.start = true;
    config.server_from_dhcp = false;
    config.renew_servers_after_new_IP = false;
    const esp_err_t error = esp_netif_sntp_init(&config);
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "SNTP init failed: %s", esp_err_to_name(error));
        s_sntp_started.store(false);
    }
}

void catalog_task(void *) {
    constexpr std::size_t kDiscoveryCapacity = 8;
    auto *stations = static_cast<RadioStation *>(
        std::calloc(kDiscoveryCapacity, sizeof(RadioStation)));
    while (stations) {
        uint32_t generation = 0;
        char city[sizeof(s_requested_city)] = {};
        char display[sizeof(s_requested_city_display)] = {};
        {
            std::lock_guard<std::mutex> lock(s_catalog_mutex);
            generation = s_catalog_generation;
            std::snprintf(city, sizeof(city), "%s", s_requested_city);
            std::snprintf(display, sizeof(display), "%s", s_requested_city_display);
        }

        std::memset(stations, 0, kDiscoveryCapacity * sizeof(RadioStation));
        RadioLocation location = {};
        const std::size_t count = city[0]
                                      ? radio_catalog_discover_city(
                                            city, stations, kDiscoveryCapacity, &location)
                                      : radio_catalog_discover(
                                            stations, kDiscoveryCapacity, &location);

        std::lock_guard<std::mutex> lock(s_catalog_mutex);
        if (generation != s_catalog_generation) continue;
        if (s_connected.load(std::memory_order_acquire)) {
            if (count > 0 &&
                radio_player_replace_stations_resuming(stations, count)) {
                radio_ui_set_location(city[0]
                                          ? display
                                          : (location.city[0] ? location.city
                                                              : location.region));
            } else {
                radio_ui_set_location(city[0] ? display : "NATIONAL");
                radio_ui_set_station(
                    radio_player_station_index(), radio_player_station_count(),
                    radio_player_station(radio_player_station_index()));
            }
            radio_player_set_network(true);
        }
        s_catalog_task = nullptr;
        break;
    }
    std::free(stations);
    if (!stations) {
        std::lock_guard<std::mutex> lock(s_catalog_mutex);
        s_catalog_task = nullptr;
        if (s_connected.load()) radio_player_set_network(true);
    }
    vTaskDelete(nullptr);
}

void start_catalog_search(const char *city, const char *display) {
    bool task_running = false;
    bool task_available = true;
    {
        std::lock_guard<std::mutex> lock(s_catalog_mutex);
        ++s_catalog_generation;
        std::snprintf(s_requested_city, sizeof(s_requested_city), "%s",
                      city ? city : "");
        std::snprintf(s_requested_city_display, sizeof(s_requested_city_display), "%s",
                      display ? display : "");
        task_running = s_catalog_task != nullptr;
        if (!task_running &&
            xTaskCreate(catalog_task, "radio_catalog", 6144, nullptr, 5,
                        &s_catalog_task) != pdPASS) {
            s_catalog_task = nullptr;
            task_available = false;
        }
    }
    radio_player_set_network(false);
    radio_ui_set_location(city && city[0] ? display : "LOCATING...");
    radio_ui_set_playback(RadioPlaybackState::Buffering,
                          city && city[0] ? "正在加载城市电台"
                                          : "正在搜索本地电台");
    if (!task_running && !task_available) {
        radio_ui_set_location(city && city[0] ? display : "NATIONAL");
        radio_player_set_network(true);
    }
}

void start_local_catalog_search() {
    start_catalog_search(nullptr, nullptr);
}

void start_city_catalog_search(const char *city, const char *display) {
    start_catalog_search(city, display);
}

void wifi_event(WifiEvent event, const std::string &data) {
    auto &manager = WifiManager::GetInstance();
    switch (event) {
        case WifiEvent::Scanning:
            radio_ui_set_network(false, "正在寻找网络");
            break;
        case WifiEvent::Connecting:
            radio_ui_set_network(false, data.c_str());
            radio_ui_set_playback(RadioPlaybackState::Stopped, "正在连接无线网络");
            break;
        case WifiEvent::Connected:
            ESP_LOGI(kTag, "Wi-Fi connected: %s, IP=%s", data.c_str(),
                     manager.GetIpAddress().c_str());
            s_connected.store(true);
            s_config_mode.store(false);
            s_config_can_cancel.store(false);
            s_settings_mode.store(false);
            radio_ui_show_main();
            radio_ui_set_network(true, data.c_str());
            radio_ui_set_station(radio_player_station_index(), radio_player_station_count(),
                                 radio_player_station(radio_player_station_index()));
            manager.SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);
            start_sntp_once();
            start_remembered_catalog_search();
            break;
        case WifiEvent::Disconnected:
            s_connected.store(false);
            radio_player_set_network(false);
            radio_ui_set_network(false, "网络已断开");
            break;
        case WifiEvent::ConfigModeEnter: {
            s_config_mode.store(true);
            s_settings_mode.store(false);
            s_connected.store(false);
            radio_player_set_network(false);
            {
                std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
                s_config_input_mode = ConfigInputMode::NetworkList;
                s_wifi_networks = {};
                s_wifi_network_count = 0;
                s_wifi_network_selected = 0;
                s_wifi_selected_ssid.clear();
                s_wifi_password.clear();
                s_wifi_keyboard_page = WifiKeyboardPage::Lower;
                s_wifi_key_selected = 0;
                s_config_ap_ssid = manager.GetApSsid();
            }
            start_wifi_scan_ui();
            break;
        }
        case WifiEvent::ConfigModeExit: {
            s_config_mode.store(false);
            s_config_can_cancel.store(false);
            {
                std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
                s_config_input_mode = ConfigInputMode::NetworkList;
                s_wifi_networks = {};
                s_wifi_network_count = 0;
                s_wifi_network_selected = 0;
                s_wifi_selected_ssid.clear();
                s_wifi_password.clear();
                s_wifi_key_selected = 0;
                s_config_ap_ssid.clear();
            }
            radio_ui_show_main();
            radio_ui_set_network(false, "正在连接网络");
            if (!manager.IsConnected()) manager.StartStation();
            break;
        }
    }
}

void connection_watchdog_task(void *) {
    while (true) {
        int waiting_seconds = 0;
        while (!s_connected.load() && !s_config_mode.load() && waiting_seconds < 30) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            ++waiting_seconds;
        }
        if (!s_connected.load() && !s_config_mode.load()) {
            ESP_LOGW(kTag, "No usable network after 30 seconds; opening setup portal");
            enter_wifi_config(false);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

}  // namespace

extern "C" void app_main(void) {
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(error);

    ESP_ERROR_CHECK(bsp_display_init());
    if (!bsp_lvgl_init()) {
        ESP_LOGE(kTag, "Display initialization failed");
        return;
    }
    bsp_display_backlight(72);
    if (!radio_display_settings_init()) {
        ESP_LOGW(kTag, "Display settings initialization failed");
    }
    bsp_battery_init();
    if (!radio_ui_init()) {
        ESP_LOGE(kTag, "UI initialization failed");
        return;
    }
    fap_screenshot_start();
    if (!radio_player_init()) {
        radio_ui_set_playback(RadioPlaybackState::Error, "音频初始化失败");
        ESP_LOGE(kTag, "Audio player initialization failed");
        return;
    }
    if (!radio_sleep_timer_init()) {
        ESP_LOGW(kTag, "Sleep timer initialization failed");
    }

    s_key_queue = xQueueCreate(10, sizeof(KeyMessage));
    ESP_ERROR_CHECK(bsp_button_init(nullptr, nullptr));
    if (!s_key_queue ||
        xTaskCreate(button_task, "radio_keys", 2048, nullptr, 6, nullptr) != pdPASS ||
        xTaskCreate(input_task, "radio_input", 4096, nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "Button initialization failed");
        return;
    }

    auto &manager = WifiManager::GetInstance();
    WifiManagerConfig config;
    config.ssid_prefix = "LEO Radio";
    config.fixed_ap_ssid = "LEO Radio";
    config.language = "zh-CN";
    config.station_hostname = "ai-radio";
    config.station_scan_min_interval_seconds = 5;
    config.station_scan_max_interval_seconds = 30;
    if (!manager.Initialize(config)) {
        radio_ui_set_playback(RadioPlaybackState::Error, "无线网络初始化失败");
        return;
    }
    manager.SetEventCallback(wifi_event);

    auto &ssid_manager = SsidManager::GetInstance();
    const auto &saved_networks = ssid_manager.GetSsidList();
    if (saved_networks.empty()) {
        // Public/share builds contain no owner's Wi-Fi credentials. A fresh
        // device therefore opens its setup portal immediately.
        manager.StartConfigAp();
    } else {
        // Existing owners keep all networks already stored in NVS. Give those
        // networks 30 seconds to appear before opening the setup portal.
        manager.StartStation();
    }

    if (xTaskCreate(connection_watchdog_task, "radio_wifi_guard", 3072, nullptr, 4,
                    nullptr) != pdPASS) {
        ESP_LOGE(kTag, "Wi-Fi watchdog task failed");
    }
    ESP_LOGI(kTag, "Internet radio ready");
}
