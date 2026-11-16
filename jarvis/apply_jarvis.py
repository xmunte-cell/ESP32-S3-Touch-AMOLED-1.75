#!/usr/bin/env python3
"""Apply the Jarvis changes to Waveshare's Brookesia factory firmware.

Usage:  python3 apply_jarvis.py <path to firmware/brookesia> [wake_word]

* copies the new/replaced files from ./components into the firmware tree
* points AIChats at Open WebUI directly (no Xiaozhi OTA, no activation code)
* adds the "Jarvis" page to the Settings app
* adds the "Agenda" app (month calendar, Google Calendar via its private
  iCal address, the board's own events) and gives Jarvis the agenda
* sets the Spanish time zone (the factory firmware uses China time)
* lets the WLAN page join company networks (user + password) as well as
  normal ones (password only)
* switches the wake word (default: Jarvis) and the device language to ca-ES

No address or API key is written anywhere: they are entered on the board
(Settings > Jarvis or the board's web page) and stored in its NVS.
Every replacement must match exactly once, otherwise the script stops.
"""

import pathlib
import shutil
import sys

HERE = pathlib.Path(__file__).resolve().parent

WAKE_WORDS = {
    "jarvis": "CONFIG_SR_WN_WN9_JARVIS_TTS",
    "hiesp": "CONFIG_SR_WN_WN9_HIESP",
    "alexa": "CONFIG_SR_WN_WN9_ALEXA",
    "nihaoxiaozhi": "CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS",
}


def patch(path, old, new, count=1):
    text = path.read_text(encoding="utf-8")
    found = text.count(old)
    if found != count:
        sys.exit(f"ERROR {path}: expected {count} match(es), found {found}:\n{old[:200]}")
    path.write_text(text.replace(old, new), encoding="utf-8")
    print(f"patched {path.name}")


def cut_between(path, start, end, new):
    """Replace text from `start` (inclusive) up to `end` (exclusive)."""
    text = path.read_text(encoding="utf-8")
    if text.count(start) != 1 or text.count(end) != 1:
        sys.exit(f"ERROR {path}: anchors not unique ({start[:60]!r} / {end[:60]!r})")
    a = text.index(start)
    b = text.index(end)
    if b <= a:
        sys.exit(f"ERROR {path}: anchors out of order")
    path.write_text(text[:a] + new + text[b:], encoding="utf-8")
    print(f"patched {path.name} (block)")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    fw = pathlib.Path(sys.argv[1]).resolve()
    wake = (sys.argv[2] if len(sys.argv) > 2 else "jarvis").lower()
    if wake not in WAKE_WORDS:
        sys.exit(f"unknown wake word {wake}; choose one of {', '.join(WAKE_WORDS)}")
    comp = fw / "components"
    if not (comp / "XiaozhiApp" / "XiaozhiApp.cpp").exists():
        sys.exit(f"{fw} does not look like firmware/brookesia")

    # 1. copy files -------------------------------------------------------
    src = HERE / "components"
    for f in src.rglob("*"):
        if f.is_file():
            dst = comp / f.relative_to(src)
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, dst)
            print(f"copied {dst.relative_to(fw)}")

    # 2. AIChats: talk to Open WebUI directly ------------------------------
    app = comp / "XiaozhiApp" / "XiaozhiApp.cpp"
    patch(app, '#include "XiaozhiApp.hpp"\n',
          '#include "XiaozhiApp.hpp"\n#include "JarvisConfig.hpp"\n')
    cut_between(
        app,
        "    const bool was_waiting =\n",
        "    const uint32_t client_generation = _chat_generation.fetch_add(1) + 1;\n",
        '''    // Jarvis: no Xiaozhi OTA server and no activation code. The board talks
    // to Open WebUI directly; its address and API key are stored in NVS and
    // are entered in Settings > Jarvis or on the board's own web page.
    jarvis::startWebServer();
    if (!jarvis::isComplete()) {
        char ip[32] = {};
        jarvis::deviceIp(ip, sizeof(ip));
        copyActivation(
            ip[0] ? ip : "Settings > Jarvis",
            "Obre aquesta adre\\u00e7a al navegador de l'ordinador, o ves a "
            "Settings > Jarvis, i posa-hi l'adre\\u00e7a d'Open WebUI i la clau API."
        );
        _activation_phase = ActivationPhase::CheckRequired;
        setState(State::ActivationRequired);
        schedulePrepare(activationRetryDelay(0));
        return false;
    }
    clearActivation();
    _activation_phase = ActivationPhase::Complete;
    esp_err_t ret = ESP_OK;
    const bool use_mqtt = false;
    const bool use_websocket = true;
    if (!canPrepare()) {
        return false;
    }

''')
    # On-screen exit button (the swipe-up gesture is hard to hit on the round screen).
    patch(app, "    _ui_timer = lv_timer_create(uiTimerCallback, 100, this);\n",
          "    _ui->setExitCallback([](void *context) {\n"
          "        auto *self = static_cast<XiaozhiApp *>(context);\n"
          "        if (self) {\n"
          "            self->notifyCoreClosed();\n"
          "        }\n"
          "    }, this);\n"
          "    _ui_timer = lv_timer_create(uiTimerCallback, 100, this);\n")
    # The spoken activation code makes no sense without Xiaozhi.
    patch(app, "bool XiaozhiApp::announceActivation(bool force)\n{\n",
          "bool XiaozhiApp::announceActivation(bool force)\n{\n"
          "    if (force || !force) {\n        return false;  // Jarvis: no spoken activation code\n    }\n")

    cm = comp / "XiaozhiApp" / "CMakeLists.txt"
    patch(cm, "        mcp-c-sdk\n",
          "        mcp-c-sdk\n        jarvis_config\n        jarvis_fonts\n        system_status\n")
    patch(cm, '"-Wno-missing-field-initializers"',
          '"-Wno-missing-field-initializers -Wno-unused-function"')

    # 3. Settings: "Jarvis" page -------------------------------------------
    st = comp / "Settings"
    patch(st / "Settings.hpp", '#include "./ui/AboutPage.hpp"\n',
          '#include "./ui/AboutPage.hpp"\n#include "./ui/JarvisPage.hpp"\n')
    patch(st / "Settings.hpp", "            About,\n        };",
          "            About,\n            Jarvis,\n        };")
    s = st / "Settings.cpp"
    patch(s, '        settings_ui::add_section(list1, "More", style_list_text);\n',
          '        settings_ui::add_section(list1, "Jarvis", style_list_text);\n'
          '        add_button_with_arrow(LV_SYMBOL_AUDIO, "Jarvis");\n\n'
          '        settings_ui::add_section(list1, "More", style_list_text);\n')
    patch(s, '''        else if (strcmp(txt, "About") == 0)
        {
            lv_async_call(open_page_async_cb, (void *)"About");
        }
''', '''        else if (strcmp(txt, "About") == 0)
        {
            lv_async_call(open_page_async_cb, (void *)"About");
        }
        else if (strcmp(txt, "Jarvis") == 0)
        {
            lv_async_call(open_page_async_cb, (void *)"Jarvis");
        }
''')
    patch(s, '''            AboutPage::requestInstance(false, false)->run();
        }
''', '''            AboutPage::requestInstance(false, false)->run();
        }
        else if (strcmp(page_name, "Jarvis") == 0)
        {
            active_page = ActivePage::Jarvis;
            JarvisPage::requestInstance(false, false)->run();
        }
''')
    patch(s, '''            AboutPage::requestInstance(false, false)->close();
            break;
''', '''            AboutPage::requestInstance(false, false)->close();
            break;
        case ActivePage::Jarvis:
            JarvisPage::requestInstance(false, false)->close();
            break;
''')
    patch(st / "CMakeLists.txt", "PRIV_REQUIRES espressif__esp_lvgl_adapter",
          "PRIV_REQUIRES espressif__esp_lvgl_adapter jarvis_config")

    # Spanish time zone and a European time server (factory: China, CST-8).
    patch(s, 'setenv("TZ", "CST-8", 1);', 'setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);')
    patch(s, '"SNTP time synced, timezone CST-8 applied"', '"SNTP time synced, timezone CET/CEST applied"')
    patch(s, 'esp_sntp_setservername(0, "ntp.aliyun.com");', 'esp_sntp_setservername(0, "pool.ntp.org");')

    # 4. Agenda app (calendar + Google Calendar) ----------------------------
    mainc = fw / "main" / "main.cpp"
    patch(mainc, '#include "XiaozhiApp.hpp"\n', '#include "XiaozhiApp.hpp"\n#include "AgendaApp.hpp"\n')
    patch(mainc,
          '        install_app(apps::XiaozhiApp::requestInstance(), "AIChats"),\n'
          '        "Install AIChats failed"\n    );\n',
          '        install_app(apps::XiaozhiApp::requestInstance(), "AIChats"),\n'
          '        "Install AIChats failed"\n    );\n'
          '    ESP_UTILS_CHECK_FALSE_EXIT(\n'
          '        install_app(apps::AgendaApp::requestInstance(), "Agenda"),\n'
          '        "Install Agenda failed"\n    );\n')
    patch(fw / "main" / "CMakeLists.txt", "        XiaozhiApp\n", "        XiaozhiApp\n        JarvisAgendaApp\n")

    # 5. Device language and wake word ------------------------------------
    act = comp / "XiaozhiApp" / "XiaozhiActivationClient.cpp"
    text = act.read_text(encoding="utf-8")
    if '"zh-CN"' in text:
        act.write_text(text.replace('"zh-CN"', '"ca-ES"'), encoding="utf-8")
        print("patched XiaozhiActivationClient.cpp (language)")


    # 6. Wi-Fi: normal (password) and company networks (user + password) ----
    wl = st / "ui" / "WlanPage.cpp"
    patch(wl, '#include "nvs.h"\n',
          '#include "nvs.h"\n#include "JarvisConfig.hpp"\n#include "JarvisWifi.hpp"\n\n'
          '// Jarvis: for enterprise networks the keyboard first asks for the user.\n'
          'static bool s_jv_ask_user = false;\n')
    patch(wl, '            lv_keyboard_set_textarea(kb, ta);\n\n        }\n    }\n',
          '            lv_keyboard_set_textarea(kb, ta);\n'
          '            s_jv_ask_user = jarvis::wifi::isEnterprise(page->ap_info[page->wifi_index].authmode);\n'
          '            lv_textarea_set_password_mode(ta, !s_jv_ask_user);\n'
          '            lv_textarea_set_placeholder_text(ta, s_jv_ask_user ? "User name..." : "Enter password...");\n'
          '            if (s_jv_ask_user) {\n'
          '                lv_textarea_set_text(ta, jarvis::load().eap_user.c_str());\n'
          '                if (page->password_title) {\n'
          '                    lv_label_set_text_fmt(page->password_title, "%s: user", page->wifi_ssid);\n'
          '                }\n'
          '            }\n\n        }\n    }\n')
    patch(wl, '        if (code == LV_EVENT_CANCEL) {\n',
          '        if (code == LV_EVENT_CANCEL) {\n'
          '            s_jv_ask_user = false;\n'
          '            lv_textarea_set_password_mode(self->ta, true);\n')
    patch(wl, '        if (code == LV_EVENT_READY) {\n\n            strlcpy(self->wifi_pwd, lv_textarea_get_text(ta), sizeof(self->wifi_pwd));\n',
          '        if (code == LV_EVENT_READY && s_jv_ask_user) {\n'
          '            // Step 1 of 2 on a company network: keep the user, then ask the password.\n'
          '            s_jv_ask_user = false;\n'
          '            jarvis::saveField("eap_user", lv_textarea_get_text(self->ta));\n'
          '            lv_textarea_set_text(self->ta, "");\n'
          '            lv_textarea_set_password_mode(self->ta, true);\n'
          '            lv_textarea_set_placeholder_text(self->ta, "Enter password...");\n'
          '            if (self->password_title) {\n'
          '                lv_label_set_text_fmt(self->password_title, "%s: password", self->wifi_ssid);\n'
          '            }\n'
          '            return;\n'
          '        }\n\n'
          '        if (code == LV_EVENT_READY) {\n\n'
          '            strlcpy(self->wifi_pwd, lv_textarea_get_text(ta), sizeof(self->wifi_pwd));\n')
    patch(wl, '            if (strlen(self->wifi_pwd) >= 8)\n',
          '            if (strlen(self->wifi_pwd) >= 8 ||\n'
          '                    (self->wifi_pwd[0] != 0 &&\n'
          '                     jarvis::wifi::isEnterprise(self->ap_info[self->wifi_index].authmode)))\n')

    patch(wl, "                esp_err_t ret = brookesia::system_status::connect_wifi(wifi_config);\n",
          "                {\n"
          "                    // Jarvis: remember this network so the board can come back to it.\n"
          "                    jarvis::wifi::Network jv_net;\n"
          "                    jv_net.ssid = self->wifi_ssid;\n"
          "                    jv_net.password = self->wifi_pwd;\n"
          "                    if (jarvis::wifi::isEnterprise(self->ap_info[self->wifi_index].authmode)) {\n"
          "                        jv_net.user = jarvis::load().eap_user;\n"
          "                    }\n"
          "                    jarvis::wifi::rememberNetwork(jv_net);\n"
          "                }\n"
          "                esp_err_t ret = brookesia::system_status::connect_wifi(wifi_config);\n")

    # The factory page stops scanning for good after one failed scan (for
    # example while the board is trying to join a network): retry instead.
    patch(wl, '                    ESP_UTILS_LOGW("esp_wifi_scan_start failed: %s", esp_err_to_name(ret));\n'
              '                    self->Wifi_state = DISCONNECT;\n'
              '                    queue_lvgl_async(wifi_state_cb, self, scan_generation);\n'
              '                    vTaskDelay(pdMS_TO_TICKS(300));\n',
          '                    ESP_UTILS_LOGW("esp_wifi_scan_start failed: %s", esp_err_to_name(ret));\n'
          '                    self->Wifi_state = WIFIOPEN;  // Jarvis: try the scan again\n'
          '                    vTaskDelay(pdMS_TO_TICKS(700));\n')

    ss = comp / "system_status" / "system_status.cpp"
    text = ss.read_text(encoding="utf-8")
    first_include = text.index("#include")
    ss.write_text(text[:first_include] + '#include "JarvisWifi.hpp"\n' + text[first_include:], encoding="utf-8")
    patch(ss, "        const esp_err_t result = esp_wifi_connect();\n",
          "        jarvis::wifi::prepareConnect();  // enterprise login when needed\n"
          "        const esp_err_t result = esp_wifi_connect();\n")
    patch(comp / "system_status" / "CMakeLists.txt", "        nvs_flash\n)", "        nvs_flash\n        jarvis_config\n)")

    sd = fw / "sdkconfig.defaults"
    patch(sd, "CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y\n", WAKE_WORDS[wake] + "=y\n")
    with open(sd, "a", encoding="utf-8") as f:
        f.write("\n# Jarvis: company (WPA2/WPA3-Enterprise) Wi-Fi\nCONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=y\n")
    print("Jarvis patch applied.")


if __name__ == "__main__":
    main()
