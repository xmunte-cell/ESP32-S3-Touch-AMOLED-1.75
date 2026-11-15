#pragma once

#include "esp_brookesia.hpp"
#include "lvgl.h"

namespace esp_brookesia::apps {
    class Settings;
}

namespace esp_brookesia::apps {

// Jarvis: Open WebUI connection settings (view + edit with the on-screen keyboard).
class JarvisPage : public systems::phone::App {
public:
    static JarvisPage *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);

    JarvisPage(bool use_status_bar, bool use_navigation_bar);
    virtual ~JarvisPage();

    bool run() override;
    bool back() override;
    bool close() override;

private:
    struct Field {
        const char *key;    // NVS key
        const char *title;  // shown in the editor
        bool secret;
    };

    void buildList();
    void openEditor(const Field *field);
    void closeEditor(bool save);
    static void rowClickedCb(lv_event_t *e);
    static void keyboardCb(lv_event_t *e);

    static JarvisPage *_instance;
    lv_obj_t *page_root;
    lv_obj_t *label;
    lv_obj_t *list1;
    lv_obj_t *editor;
    lv_obj_t *editor_ta;
    const Field *editing;
    lv_style_t style_list;
    lv_style_t style_list_btn;
    lv_style_t style_list_text;
    lv_style_t style_list_btn_pressed;
};

} // namespace esp_brookesia::apps
