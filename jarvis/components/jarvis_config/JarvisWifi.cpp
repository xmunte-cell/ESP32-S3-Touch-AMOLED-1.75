/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "JarvisWifi.hpp"

#include <string.h>

#include <algorithm>
#include <mutex>
#include <string>

#include "JarvisConfig.hpp"
#include "esp_eap_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

namespace jarvis::wifi {

namespace {

constexpr char TAG[] = "JarvisWifi";
constexpr char NVS_NS[] = "jarvis";
constexpr char NVS_KEY[] = "wifis";
constexpr char SEP = '\x1f';  // field separator (never in SSIDs typed by people)
// Tries per network before moving to the next saved one, and how long a
// connection must last to count as good (then the counters start again).
constexpr int TRIES_PER_NETWORK = 2;
constexpr int64_t GOOD_CONNECTION_US = 45LL * 1000 * 1000;
constexpr int PAUSE_AFTER_ROUND_MS = 4000;

std::mutex g_mutex;
bool g_enterprise_on = false;
std::string g_last_ssid;
int g_tries = 0;
size_t g_switches = 0;
int64_t g_last_call_us = 0;

std::vector<Network> loadLocked()
{
    std::vector<Network> out;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return out;
    }
    size_t len = 0;
    if (nvs_get_blob(h, NVS_KEY, nullptr, &len) == ESP_OK && len > 0) {
        std::string data(len, '\0');
        if (nvs_get_blob(h, NVS_KEY, data.data(), &len) == ESP_OK) {
            size_t pos = 0;
            while (pos < data.size() && out.size() < MAX_NETWORKS) {
                size_t nl = data.find('\n', pos);
                if (nl == std::string::npos) {
                    nl = data.size();
                }
                std::string line = data.substr(pos, nl - pos);
                pos = nl + 1;
                size_t a = line.find(SEP);
                size_t b = a == std::string::npos ? a : line.find(SEP, a + 1);
                if (b == std::string::npos) {
                    continue;
                }
                Network n;
                n.ssid = line.substr(0, a);
                n.user = line.substr(a + 1, b - a - 1);
                n.password = line.substr(b + 1);
                if (!n.ssid.empty()) {
                    out.push_back(std::move(n));
                }
            }
        }
    }
    nvs_close(h);
    return out;
}

bool saveLocked(const std::vector<Network> &list)
{
    std::string data;
    for (const Network &n : list) {
        data += n.ssid;
        data += SEP;
        data += n.user;
        data += SEP;
        data += n.password;
        data += '\n';
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = data.empty() ? nvs_erase_key(h, NVS_KEY)
                                 : nvs_set_blob(h, NVS_KEY, data.data(), data.size());
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    bool ok = err == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

std::string clean(const std::string &s, size_t max)
{
    std::string out;
    for (char c : s) {
        if (c != SEP && c != '\n' && c != '\r') {
            out += c;
        }
    }
    if (out.size() > max) {
        out.resize(max);
    }
    return out;
}

std::string ssidOf(const wifi_config_t &cfg)
{
    const char *s = reinterpret_cast<const char *>(cfg.sta.ssid);
    return std::string(s, strnlen(s, sizeof(cfg.sta.ssid)));
}

void enterpriseOff()
{
    if (g_enterprise_on) {
        esp_wifi_sta_enterprise_disable();
        esp_eap_client_clear_identity();
        esp_eap_client_clear_username();
        esp_eap_client_clear_password();
        g_enterprise_on = false;
        ESP_LOGI(TAG, "Enterprise Wi-Fi login off");
    }
}

void enterpriseOn(const std::string &user, const std::string &password)
{
    esp_eap_client_set_identity(reinterpret_cast<const unsigned char *>(user.data()), user.size());
    esp_eap_client_set_username(reinterpret_cast<const unsigned char *>(user.data()), user.size());
    esp_eap_client_set_password(reinterpret_cast<const unsigned char *>(password.data()), password.size());
    if (esp_wifi_sta_enterprise_enable() == ESP_OK) {
        g_enterprise_on = true;
        ESP_LOGI(TAG, "Enterprise Wi-Fi login on");
    }
}

} // namespace

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
    std::lock_guard<std::mutex> lock(g_mutex);
    wifi_config_t cfg = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        return;
    }
    const std::string current = ssidOf(cfg);
    std::vector<Network> list = loadLocked();

    // First run after the update: keep the network that was already saved.
    if (list.empty() && !current.empty()) {
        Network n;
        n.ssid = current;
        const char *pw = reinterpret_cast<const char *>(cfg.sta.password);
        n.password = std::string(pw, strnlen(pw, sizeof(cfg.sta.password)));
        if (isEnterprise(cfg.sta.threshold.authmode)) {
            n.user = jarvis::load().eap_user;
        }
        list.push_back(n);
        saveLocked(list);
        ESP_LOGI(TAG, "Saved the current Wi-Fi in the list");
    }

    // No scanning here (it would block the WLAN page's own scan). Instead,
    // when the current network keeps failing, try the next saved one.
    const int64_t now = esp_timer_get_time();
    if (now - g_last_call_us > GOOD_CONNECTION_US || current != g_last_ssid) {
        g_tries = 0;  // the last connection worked for a while, or a new network was set
        if (now - g_last_call_us > GOOD_CONNECTION_US) {
            g_switches = 0;
        }
    }
    g_last_call_us = now;
    g_tries++;

    const Network *chosen = nullptr;
    size_t index = list.size();
    for (size_t i = 0; i < list.size(); i++) {
        if (list[i].ssid == current) {
            index = i;
            break;
        }
    }
    if (index < list.size()) {
        chosen = &list[index];
    }
    if (list.size() > 1 && (index >= list.size() || g_tries > TRIES_PER_NETWORK)) {
        size_t next = index >= list.size() ? 0 : (index + 1) % list.size();
        chosen = &list[next];
        g_tries = 1;
        g_switches++;
        if (g_switches >= list.size()) {
            // None of the saved networks answered: rest a little so the
            // radio is free (the WLAN page can scan) and try again.
            g_switches = 0;
            vTaskDelay(pdMS_TO_TICKS(PAUSE_AFTER_ROUND_MS));
        }
    } else if (list.size() <= 1 && g_tries > TRIES_PER_NETWORK) {
        g_tries = 1;
        vTaskDelay(pdMS_TO_TICKS(PAUSE_AFTER_ROUND_MS));
    }

    if (chosen && chosen->ssid != current) {
        wifi_config_t next = {};
        memcpy(next.sta.ssid, chosen->ssid.data(), std::min(chosen->ssid.size(), sizeof(next.sta.ssid)));
        if (chosen->user.empty()) {
            strlcpy(reinterpret_cast<char *>(next.sta.password), chosen->password.c_str(),
                    sizeof(next.sta.password));
            next.sta.threshold.authmode = chosen->password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        } else {
            strlcpy(reinterpret_cast<char *>(next.sta.password), chosen->password.c_str(),
                    sizeof(next.sta.password));
            next.sta.threshold.authmode = WIFI_AUTH_ENTERPRISE;
        }
        if (esp_wifi_set_config(WIFI_IF_STA, &next) == ESP_OK) {
            cfg = next;
            g_last_ssid = chosen->ssid;
            ESP_LOGI(TAG, "Trying the next saved Wi-Fi (%s network)",
                     chosen->user.empty() ? "normal" : "company");
        }
    }

    g_last_ssid = ssidOf(cfg);
    if (chosen && !chosen->user.empty()) {
        enterpriseOn(chosen->user, chosen->password);
        return;
    }
    if (!chosen && isEnterprise(cfg.sta.threshold.authmode)) {
        // Older single-network path: user from settings, password from the config.
        std::string user = jarvis::load().eap_user;
        if (!user.empty()) {
            const char *pw = reinterpret_cast<const char *>(cfg.sta.password);
            enterpriseOn(user, std::string(pw, strnlen(pw, sizeof(cfg.sta.password))));
            return;
        }
        ESP_LOGW(TAG, "Company network but no Wi-Fi user (Settings > Jarvis)");
    }
    enterpriseOff();
}

std::vector<Network> savedNetworks()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return loadLocked();
}

bool rememberNetwork(const Network &network)
{
    Network n;
    n.ssid = clean(network.ssid, 32);
    n.user = clean(network.user, 64);
    n.password = clean(network.password, 63);
    if (n.ssid.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<Network> list = loadLocked();
    for (size_t i = 0; i < list.size(); i++) {
        if (list[i].ssid == n.ssid) {
            list.erase(list.begin() + static_cast<long>(i));
            break;
        }
    }
    list.insert(list.begin(), n);
    if (list.size() > MAX_NETWORKS) {
        list.resize(MAX_NETWORKS);
    }
    ESP_LOGI(TAG, "Wi-Fi saved (%u in the list)", static_cast<unsigned>(list.size()));
    return saveLocked(list);
}

bool forgetNetwork(size_t index)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<Network> list = loadLocked();
    if (index >= list.size()) {
        return false;
    }
    list.erase(list.begin() + static_cast<long>(index));
    return saveLocked(list);
}

} // namespace jarvis::wifi
