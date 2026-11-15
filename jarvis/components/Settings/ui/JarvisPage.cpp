// Jarvis: Settings page with the Open WebUI connection settings.
// Tap a row to edit it with the on-screen keyboard, or use the board's web page
// (address shown at the top) to paste values from a computer.
#include "JarvisPage.hpp"
#include "SettingsUI.hpp"
#include "esp_log.h"
#include "../Settings.hpp"
#include "JarvisConfig.hpp"

#include <string>
#include <string.h>

#define ESP_UTILS_LOG_TAG "BS:JarvisPage"
#include "esp_lib_utils.h"

namespace esp_brookesia::apps
{
    namespace {
        constexpr int FIELD_COUNT = 6;
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
            {"url", "Open WebUI server (http://IP:port)", false},
            {"key", "API key (sk-...)", true},
            {"model", "Model", false},
            {"lang", "Whisper language", false},
            {"voice", "TTS voice (empty = text only)", false},
            {"tts_model", "TTS model", false},
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
            s.url.empty() ? "(not set)" : s.url,
            key,
            s.model,
            s.language,
            s.tts_voice.empty() ? "(text only)" : s.tts_voice,
            s.tts_model,
        };

        list1 = settings_ui::create_content_list(page_root);
        lv_obj_add_style(list1, &style_list, LV_PART_MAIN);

        settings_ui::add_section(list1, "Tap a value to edit", style_list_text);
        const char *names[FIELD_COUNT] = {"Server", "API key", "Model", "Language", "Voice", "TTS model"};
        for (int i = 0; i < FIELD_COUNT; i++) {
            lv_obj_t *row = settings_ui::add_info_row(list1, nullptr, names[i], values[i].c_str(), style_list_btn);
            lv_obj_add_style(row, &style_list_btn_pressed, LV_STATE_PRESSED);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, rowClickedCb, LV_EVENT_CLICKED, (void *)&fields[i]);
        }
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
        if (strcmp(field->key, "url") == 0) current = s.url;
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
