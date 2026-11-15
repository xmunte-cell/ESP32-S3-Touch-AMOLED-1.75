/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "JarvisWifi.hpp"

#include <string.h>

#include <string>

#include "JarvisConfig.hpp"
#include "esp_eap_client.h"
#include "esp_log.h"
#include "esp_wifi.h"

namespace jarvis::wifi {

namespace {
constexpr char TAG[] = "JarvisWifi";
bool g_enterprise_on = false;
}

bool isEnterprise(int authmode)
{
    switch (authmode) {
    case WIFI_AUTH_ENTERPRISE:  // == WIFI_AUTH_WPA2_ENTERPRISE
    case WIFI_AUTH_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENT_192:
        return true;
    default:
        return false;
    }
}

void prepareConnect()
{
    wifi_config_t cfg = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        return;
    }
    const std::string user = jarvis::load().eap_user;
    const bool want = isEnterprise(cfg.sta.threshold.authmode) && !user.empty();
    if (!want) {
        if (g_enterprise_on) {
            esp_wifi_sta_enterprise_disable();
            esp_eap_client_clear_identity();
            esp_eap_client_clear_username();
            esp_eap_client_clear_password();
            g_enterprise_on = false;
            ESP_LOGI(TAG, "Enterprise Wi-Fi login off");
        }
        if (isEnterprise(cfg.sta.threshold.authmode)) {
            ESP_LOGW(TAG, "Enterprise network but no Wi-Fi user (Settings > Jarvis)");
        }
        return;
    }
    const char *password = reinterpret_cast<const char *>(cfg.sta.password);
    const size_t password_len = strnlen(password, sizeof(cfg.sta.password));
    esp_eap_client_set_identity(reinterpret_cast<const unsigned char *>(user.data()), user.size());
    esp_eap_client_set_username(reinterpret_cast<const unsigned char *>(user.data()), user.size());
    esp_eap_client_set_password(reinterpret_cast<const unsigned char *>(password), password_len);
    if (esp_wifi_sta_enterprise_enable() == ESP_OK) {
        g_enterprise_on = true;
        ESP_LOGI(TAG, "Enterprise Wi-Fi login on (user set, password %u chars)",
                 static_cast<unsigned>(password_len));
    }
}

} // namespace jarvis::wifi
