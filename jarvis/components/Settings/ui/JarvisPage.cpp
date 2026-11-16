// Jarvis: Settings page with the Open WebUI connection settings.
// Tap a row to edit it with the on-screen keyboard, or use the board's web page
// (address shown at the top) to paste values from a computer.
#include "JarvisPage.hpp"
#include "SettingsUI.hpp"
#include "esp_log.h"
#include "../Settings.hpp"
#include "JarvisConfig.hpp"
#include "JarvisWifi.hpp"

#include <stdint.h>

#include <string>
#include <string.h>

#define ESP_UTILS_LOG_TAG "BS:JarvisPage"
#include "esp_lib_utils.h"

namespace esp_brookesia::apps
{
    namespace {
        constexpr int FIELD_COUNT = 8;
    }

    JarvisPage *JarvisPage::_instance = nullptr;

    JarvisPage *JarvisPage::requestInstance(bool use_status_bar, bool use_navigation_bar)
    {
        if (_instance == nullptr)
        {
            _instance = new JarvisPage(use_status_bar, use_navigation_bar);
        }
        return _instance;
    }

    JarvisPage::JarvisPage(bool use_status_bar, bool use_navigation_bar)
        : App("Jarvis", nullptr, true, use_status_bar, use_navigation_bar),
          page_root(nullptr), label(nullptr), list1(nullptr), editor(nullptr),
          editor_ta(nullptr), editing(nullptr)
    {
    }

    JarvisPage::~JarvisPage()
    {
    }

    bool JarvisPage::run()
    {
        jarvis::startWebServer();
        lv_obj_clean(lv_scr_act());
        page_root = settings_ui::create_page(lv_scr_act());
        settings_ui::create_header(page_root, "Jarvis", [](lv_event_t *e) {
            (void)e;
            lv_async_call([](void *param) {
                (void)param;
                Settings::requestInstance()->showRootPage();
            }, nullptr);
        });
        settings_ui::init_list_styles(style_list, style_list_btn, style_list_text, style_list_btn_pressed);
        buildList();
        return true;
    }

    void JarvisPage::buildList()
    {
        static const Field fields[FIELD_COUNT] = {
            {"name", "Assistant name (wake word stays Jarvis)", false},
            {"url", "Open WebUI server (http://IP:port)", false},
            {"key", "API key (sk-...)", true},
            {"model", "Model", false},
            {"lang", "Whisper language", false},
            {"voice", "TTS voice (empty = text only)", false},
            {"tts_model", "TTS model", false},
            {"eap_user", "Company Wi-Fi user (empty = normal Wi-Fi)", false},
        };

        if (list1 != nullptr)
        {
            lv_obj_del(list1);
            list1 = nullptr;
        }
        jarvis::Settings s = jarvis::load();
        char ip[20] = {};
        jarvis::deviceIp(ip, sizeof(ip));
        std::string web = ip[0] ? std::string("http://") + ip : std::string("(no WiFi)");
        std::string key = "(not set)";
        if (s.api_key.size() > 8) {
            key = s.api_key.substr(0, 3) + "..." + s.api_key.substr(s.api_key.size() - 4);
        } else if (!s.api_key.empty()) {
            key = "****";
        }
        const std::string values[FIELD_COUNT] = {
            s.name,
            s.url.empty() ? "(not set)" : s.url,
            key,
            s.model,
            s.language,
            s.tts_voice.empty() ? "(text only)" : s.tts_voice,
            s.tts_model,
            s.eap_user.empty() ? "(normal Wi-Fi)" : s.eap_user,
        };

        list1 = settings_ui::create_content_list(page_root);
        lv_obj_add_style(list1, &style_list, LV_PART_MAIN);

        settings_ui::add_section(list1, "Tap a value to edit", style_list_text);
        const char *names[FIELD_COUNT] = {"Name", "Server", "API key", "Model", "Language", "Voice", "TTS model", "Wi-Fi user"};
        for (int i = 0; i < FIELD_COUNT; i++) {
            lv_obj_t *row = settings_ui::add_info_row(list1, nullptr, names[i], values[i].c_str(), style_list_btn);
            lv_obj_add_style(row, &style_list_btn_pressed, LV_STATE_PRESSED);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, rowClickedCb, LV_EVENT_CLICKED, (void *)&fields[i]);
        }
        // Saved Wi-Fi networks: the board joins whichever one is in range.
        settings_ui::add_section(list1, "Saved Wi-Fi (tap to forget)", style_list_text);
        std::vector<jarvis::wifi::Network> nets = jarvis::wifi::savedNetworks();
        if (nets.empty()) {
            settings_ui::add_info_row(list1, LV_SYMBOL_WIFI, "None yet", "Settings > WLAN", style_list_btn);
        }
        for (size_t i = 0; i < nets.size(); i++) {
            const char *kind = nets[i].user.empty() ? "password" : "user + password";
            lv_obj_t *row = settings_ui::add_info_row(list1, LV_SYMBOL_WIFI, nets[i].ssid.c_str(), kind, style_list_btn);
            lv_obj_add_style(row, &style_list_btn_pressed, LV_STATE_PRESSED);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, networkClickedCb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
        settings_ui::add_info_row(list1, nullptr, "Add a network", "Settings > WLAN or web page", style_list_btn);

        settings_ui::add_section(list1, "Or edit from a computer", style_list_text);
        settings_ui::add_info_row(list1, LV_SYMBOL_WIFI, "Web page", web.c_str(), style_list_btn);
    }

    void JarvisPage::rowClickedCb(lv_event_t *e)
    {
        const Field *field = static_cast<const Field *>(lv_event_get_user_data(e));
        if (_instance && field) {
            _instance->openEditor(field);
        }
    }

    void JarvisPage::openEditor(const Field *field)
    {
        if (editor != nullptr) {
            return;
        }
        editing = field;
        jarvis::Settings s = jarvis::load();
        std::string current;
        if (strcmp(field->key, "name") == 0) current = s.name;
        else if (strcmp(field->key, "eap_user") == 0) current = s.eap_user;
        else if (strcmp(field->key, "url") == 0) current = s.url;
        else if (strcmp(field->key, "model") == 0) current = s.model;
        else if (strcmp(field->key, "lang") == 0) current = s.language;
        else if (strcmp(field->key, "voice") == 0) current = s.tts_voice;
        else if (strcmp(field->key, "tts_model") == 0) current = s.tts_model;
        // the API key is never shown: the field starts empty

        editor = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(editor);
        lv_obj_set_size(editor, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(editor, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(editor, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_clear_flag(editor, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(editor);
        lv_label_set_text(title, field->title);
        lv_obj_set_style_text_color(title, lv_color_hex(0x31E1FF), LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 62);

        editor_ta = lv_textarea_create(editor);
        lv_textarea_set_one_line(editor_ta, true);
        lv_textarea_set_text(editor_ta, current.c_str());
        lv_textarea_set_placeholder_text(editor_ta, field->secret ? "type or paste the new key" : "");
        lv_obj_set_width(editor_ta, 340);
        lv_obj_align(editor_ta, LV_ALIGN_TOP_MID, 0, 100);
        lv_obj_add_state(editor_ta, LV_STATE_FOCUSED);

        lv_obj_t *kb = lv_keyboard_create(editor);
        lv_obj_set_size(kb, 360, 200);
        lv_obj_align(kb, LV_ALIGN_TOP_MID, 0, 170);
        lv_keyboard_set_textarea(kb, editor_ta);
        lv_obj_add_event_cb(kb, keyboardCb, LV_EVENT_READY, this);
        lv_obj_add_event_cb(kb, keyboardCb, LV_EVENT_CANCEL, this);

        lv_obj_t *hint = lv_label_create(editor);
        lv_label_set_text(hint, LV_SYMBOL_OK " save    " LV_SYMBOL_KEYBOARD " cancel");
        lv_obj_set_style_text_color(hint, lv_color_hex(0x8AA0AA), LV_PART_MAIN);
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 380);
    }

    void JarvisPage::networkClickedCb(lv_event_t *e)
    {
        if (_instance) {
            _instance->openForget(static_cast<size_t>((intptr_t)lv_event_get_user_data(e)));
        }
    }

    // Asks before forgetting a saved network (reuses the editor overlay).
    void JarvisPage::openForget(size_t index)
    {
        std::vector<jarvis::wifi::Network> nets = jarvis::wifi::savedNetworks();
        if (editor != nullptr || index >= nets.size()) {
            return;
        }
        forget_index = index;
        editor = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(editor);
        lv_obj_set_size(editor, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(editor, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(editor, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_clear_flag(editor, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(editor);
        lv_label_set_text_fmt(title, "Forget Wi-Fi\n%s ?", nets[index].ssid.c_str());
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_color(title, lv_color_hex(0x31E1FF), LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_CENTER, 0, -60);

        const char *texts[2] = {"Cancel", "Forget"};
        for (int i = 0; i < 2; i++) {
            lv_obj_t *btn = lv_button_create(editor);
            lv_obj_set_size(btn, 140, 60);
            lv_obj_align(btn, LV_ALIGN_CENTER, i == 0 ? -80 : 80, 40);
            lv_obj_set_style_bg_color(btn, lv_color_hex(i == 0 ? 0x33414A : 0xC0392B), LV_PART_MAIN);
            lv_obj_t *l = lv_label_create(btn);
            lv_label_set_text(l, texts[i]);
            lv_obj_center(l);
            lv_obj_add_event_cb(btn, forgetCb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }

    void JarvisPage::forgetCb(lv_event_t *e)
    {
        if (!_instance) {
            return;
        }
        if ((intptr_t)lv_event_get_user_data(e) == 1) {
            jarvis::wifi::forgetNetwork(_instance->forget_index);
        }
        lv_async_call([](void *p) {
            static_cast<JarvisPage *>(p)->closeEditor(false);
        }, _instance);
    }

    void JarvisPage::keyboardCb(lv_event_t *e)
    {
        JarvisPage *self = static_cast<JarvisPage *>(lv_event_get_user_data(e));
        if (!self) {
            return;
        }
        bool save = lv_event_get_code(e) == LV_EVENT_READY;
        // Close asynchronously: the keyboard is deleted with the editor.
        self->editing = save ? self->editing : nullptr;
        if (save && self->editor_ta) {
            std::string value = lv_textarea_get_text(self->editor_ta);
            const Field *f = self->editing;
            if (f && !(f->secret && value.empty())) {
                jarvis::saveField(f->key, value);
            }
        }
        lv_async_call([](void *p) {
            static_cast<JarvisPage *>(p)->closeEditor(false);
        }, self);
    }

    void JarvisPage::closeEditor(bool save)
    {
        (void)save;
        if (editor != nullptr) {
            lv_obj_del(editor);
            editor = nullptr;
            editor_ta = nullptr;
        }
        editing = nullptr;
        if (page_root != nullptr) {
            buildList();
        }
    }

    bool JarvisPage::back()
    {
        if (editor != nullptr) {
            closeEditor(false);
            return true;
        }
        Settings::requestInstance()->showRootPage();
        return true;
    }

    bool JarvisPage::close()
    {
        if (editor != nullptr) {
            lv_obj_del(editor);
            editor = nullptr;
            editor_ta = nullptr;
        }
        if (page_root != nullptr)
        {
            lv_obj_del(page_root);
            page_root = nullptr;
            label = nullptr;
            list1 = nullptr;
            settings_ui::reset_list_styles(style_list, style_list_btn, style_list_text, style_list_btn_pressed);
        }
        return true;
    }

} // namespace esp_brookesia::apps
