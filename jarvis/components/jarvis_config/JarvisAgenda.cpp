/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "JarvisAgenda.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <mutex>

#include "JarvisConfig.hpp"
#include "JarvisIcs.hpp"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

namespace jarvis::agenda {

namespace {

constexpr char TAG[] = "JarvisAgenda";
constexpr char NVS_NS[] = "jarvis";
constexpr char NVS_KEY_LOCAL[] = "agenda";
// Spain (peninsula): CET/CEST.
constexpr char TIME_ZONE[] = "CET-1CEST,M3.5.0,M10.5.0/3";
constexpr uint32_t SYNC_PERIOD_MS = 30 * 60 * 1000;
constexpr size_t MAX_GOOGLE_EVENTS = 1500;
constexpr size_t MAX_LOCAL_EVENTS = 200;
constexpr size_t MAX_ICS_BYTES = 6 * 1024 * 1024;

std::mutex g_mutex;
std::vector<Event> g_google;
std::vector<Event> g_local;
std::string g_status = "Encara no s'ha llegit el calendari";
std::atomic<uint32_t> g_revision{1};
TaskHandle_t g_task = nullptr;
bool g_local_loaded = false;

bool clockValid()
{
    return time(nullptr) > 1700000000;  // after November 2023
}

std::string clean(const std::string &in, size_t max)
{
    std::string out;
    for (char c : in) {
        if (c == '|' || c == '\n' || c == '\r') {
            c = ' ';
        }
        out += c;
        if (out.size() >= max) {
            break;
        }
    }
    return out;
}

// Local events are stored as lines: YYYYMMDD|start|end|title|place
void loadLocalLocked()
{
    if (g_local_loaded) {
        return;
    }
    g_local_loaded = true;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = 0;
    if (nvs_get_blob(h, NVS_KEY_LOCAL, nullptr, &len) == ESP_OK && len > 0) {
        std::string data(len, '\0');
        if (nvs_get_blob(h, NVS_KEY_LOCAL, data.data(), &len) == ESP_OK) {
            size_t pos = 0;
            while (pos < data.size()) {
                size_t nl = data.find('\n', pos);
                if (nl == std::string::npos) {
                    nl = data.size();
                }
                std::string l = data.substr(pos, nl - pos);
                pos = nl + 1;
                std::vector<std::string> f;
                size_t p = 0;
                while (f.size() < 4) {
                    size_t bar = l.find('|', p);
                    if (bar == std::string::npos) {
                        break;
                    }
                    f.push_back(l.substr(p, bar - p));
                    p = bar + 1;
                }
                if (f.size() < 4) {
                    continue;
                }
                Event e;
                e.date = atoi(f[0].c_str());
                e.start = static_cast<int16_t>(atoi(f[1].c_str()));
                e.end = static_cast<int16_t>(atoi(f[2].c_str()));
                e.title = f[3];
                e.place = l.substr(p);
                if (e.date > 19000101) {
                    g_local.push_back(std::move(e));
                }
            }
        }
    }
    nvs_close(h);
}

bool saveLocalLocked()
{
    std::string data;
    for (const Event &e : g_local) {
        char head[40];
        snprintf(head, sizeof(head), "%ld|%d|%d|", static_cast<long>(e.date), e.start, e.end);
        data += head;
        data += e.title;
        data += '|';
        data += e.place;
        data += '\n';
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = data.empty() ? nvs_erase_key(h, NVS_KEY_LOCAL)
                                 : nvs_set_blob(h, NVS_KEY_LOCAL, data.data(), data.size());
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    bool ok = err == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool sortByTime(const Event &a, const Event &b)
{
    if (a.date != b.date) {
        return a.date < b.date;
    }
    return a.start < b.start;
}

void setStatus(const std::string &s)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status = s;
    g_revision++;
}

// Reads the Google Calendar private iCal address. Returns true on success.
bool syncOnce(const std::string &url)
{
    int32_t t = today();
    IcsParser parser(addDays(t, -31), addDays(t, 400), MAX_GOOGLE_EVENTS);

    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 20000;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 2048;
    cfg.user_agent = "Jarvis-board";
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.max_redirection_count = 5;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        setStatus("Error intern en llegir el calendari");
        return false;
    }
    int status = -1;
    esp_err_t err = ESP_FAIL;
    for (int hop = 0; hop < 5; hop++) {
        err = esp_http_client_open(c, 0);
        if (err != ESP_OK) {
            break;
        }
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            esp_http_client_set_redirection(c);
            esp_http_client_close(c);
            continue;
        }
        break;
    }
    if (err != ESP_OK) {
        esp_http_client_cleanup(c);
        setStatus("No arribo a Google Calendar (sense internet?)");
        ESP_LOGW(TAG, "Calendar open failed: %s", esp_err_to_name(err));
        return false;
    }
    if (status != 200) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        setStatus("Google Calendar ha respost HTTP " + std::to_string(status) +
                  ". Revisa l'adreça secreta.");
        return false;
    }
    static char buf[2048];
    size_t total = 0;
    int n;
    while ((n = esp_http_client_read(c, buf, sizeof(buf))) > 0) {
        parser.feed(buf, static_cast<size_t>(n));
        total += static_cast<size_t>(n);
        if (total > MAX_ICS_BYTES) {
            break;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    std::vector<Event> events = parser.finish();
    time_t now = time(nullptr);
    struct tm lt = {};
    localtime_r(&now, &lt);
    char msg[96];
    snprintf(msg, sizeof(msg), "Google Calendar llegit a les %02d:%02d (%u esdeveniments)",
             lt.tm_hour, lt.tm_min, static_cast<unsigned>(events.size()));
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_google.swap(events);
        g_status = msg;
        g_revision++;
    }
    ESP_LOGI(TAG, "%s, %u bytes", msg, static_cast<unsigned>(total));
    return true;
}

void syncTask(void *)
{
    for (;;) {
        std::string url = jarvis::load().ics_url;
        uint32_t wait_ms = SYNC_PERIOD_MS;
        if (url.empty()) {
            bool had = false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                had = !g_google.empty();
                g_google.clear();
                g_status = "Sense calendari de Google (posa'l a la pàgina web de la placa)";
            }
            if (had) {
                g_revision++;
            }
        } else if (!clockValid()) {
            setStatus("Esperant la WiFi i l'hora per llegir el calendari");
            wait_ms = 10000;
        } else {
            char ip[32];
            jarvis::deviceIp(ip, sizeof(ip));
            if (!ip[0]) {
                setStatus("Esperant la WiFi per llegir el calendari");
                wait_ms = 10000;
            } else if (!syncOnce(url)) {
                wait_ms = 5 * 60 * 1000;
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

} // namespace

void begin()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    setenv("TZ", TIME_ZONE, 1);
    tzset();
    loadLocalLocked();
    if (!g_task) {
        // Internal-RAM stack: the task writes NVS and uses TLS.
        if (xTaskCreatePinnedToCore(syncTask, "jarvis_cal", 8192, nullptr, 3, &g_task, 0) != pdPASS) {
            g_task = nullptr;
            ESP_LOGE(TAG, "Could not start the calendar task");
        }
    }
}

void requestSync()
{
    if (g_task) {
        xTaskNotifyGive(g_task);
    }
}

int32_t today()
{
    if (!clockValid()) {
        return 0;
    }
    time_t now = time(nullptr);
    struct tm lt = {};
    localtime_r(&now, &lt);
    return (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday;
}

int nowMinutes()
{
    if (!clockValid()) {
        return 0;
    }
    time_t now = time(nullptr);
    struct tm lt = {};
    localtime_r(&now, &lt);
    return lt.tm_hour * 60 + lt.tm_min;
}

std::vector<Event> eventsOn(int32_t date)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    loadLocalLocked();
    std::vector<Event> out;
    for (const Event &e : g_google) {
        if (e.date == date) {
            out.push_back(e);
        }
    }
    for (const Event &e : g_local) {
        if (e.date == date) {
            out.push_back(e);
        }
    }
    std::stable_sort(out.begin(), out.end(), sortByTime);
    return out;
}

std::vector<Event> upcoming(int days)
{
    int32_t from = today();
    if (!from) {
        return {};
    }
    int32_t to = addDays(from, days - 1);
    std::lock_guard<std::mutex> lock(g_mutex);
    loadLocalLocked();
    std::vector<Event> out;
    for (const auto *list : {&g_google, &g_local}) {
        for (const Event &e : *list) {
            if (e.date >= from && e.date <= to) {
                out.push_back(e);
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), sortByTime);
    return out;
}

uint32_t monthMask(int year, int month)
{
    int32_t lo = makeDate(year, month, 1);
    int32_t hi = makeDate(year, month, 31);
    uint32_t mask = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    loadLocalLocked();
    for (const auto *list : {&g_google, &g_local}) {
        for (const Event &e : *list) {
            if (e.date >= lo && e.date <= hi) {
                mask |= 1u << ((e.date % 100) - 1);
            }
        }
    }
    return mask;
}

std::vector<Event> localEvents()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    loadLocalLocked();
    return g_local;
}

bool addLocal(const Event &event)
{
    if (event.date < 19000101 || event.title.empty()) {
        return false;
    }
    Event e = event;
    e.google = false;
    e.title = clean(e.title, 80);
    e.place = clean(e.place, 60);
    std::lock_guard<std::mutex> lock(g_mutex);
    loadLocalLocked();
    if (g_local.size() >= MAX_LOCAL_EVENTS) {
        return false;
    }
    g_local.push_back(e);
    std::stable_sort(g_local.begin(), g_local.end(), sortByTime);
    g_revision++;
    return saveLocalLocked();
}

bool removeLocal(size_t index)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    loadLocalLocked();
    if (index >= g_local.size()) {
        return false;
    }
    g_local.erase(g_local.begin() + static_cast<long>(index));
    g_revision++;
    return saveLocalLocked();
}

bool hasGoogle()
{
    return !jarvis::load().ics_url.empty();
}

std::string status()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}

uint32_t revision()
{
    return g_revision.load();
}

std::string assistantContext()
{
    int32_t t = today();
    if (!t) {
        return {};
    }
    int wd = weekday(t);
    int now = nowMinutes();
    char head[160];
    snprintf(head, sizeof(head), "Avui és %s, %d de %s de %d, i són les %s.",
             weekdayName(wd), static_cast<int>(t % 100), monthName((t / 100) % 100),
             static_cast<int>(t / 10000), formatTime(now).c_str());
    std::string out = head;
    std::vector<Event> ev = upcoming(8);
    if (ev.empty()) {
        out += " L'agenda dels propers 7 dies és buida.";
        return out;
    }
    out += " Agenda de l'usuari (propers 7 dies):";
    int32_t last = 0;
    size_t chars = 0;
    for (const Event &e : ev) {
        if (chars > 1500) {
            out += " ...";
            break;
        }
        std::string item;
        if (e.date != last) {
            last = e.date;
            int64_t diff = dateToDays(e.date) - dateToDays(t);
            char day[64];
            if (diff == 0) {
                snprintf(day, sizeof(day), " Avui:");
            } else if (diff == 1) {
                snprintf(day, sizeof(day), " Demà:");
            } else {
                snprintf(day, sizeof(day), " %s %d:", weekdayName(weekday(e.date)),
                         static_cast<int>(e.date % 100));
            }
            item += day;
        }
        item += " ";
        if (e.start >= 0) {
            item += formatTime(e.start);
            if (e.end >= 0) {
                item += "-" + formatTime(e.end);
            }
            item += " ";
        } else {
            item += "(tot el dia) ";
        }
        item += e.title;
        if (!e.place.empty()) {
            item += " (" + e.place + ")";
        }
        item += ";";
        chars += item.size();
        out += item;
    }
    return out;
}

int32_t makeDate(int year, int month, int day)
{
    return year * 10000 + month * 100 + day;
}

int32_t addDays(int32_t date, int days)
{
    return daysToDate(dateToDays(date) + days);
}

int weekday(int32_t date)
{
    int64_t w = (dateToDays(date) + 3) % 7;
    return static_cast<int>(w < 0 ? w + 7 : w);
}

int daysInMonth(int year, int month)
{
    int ny = month == 12 ? year + 1 : year;
    int nm = month == 12 ? 1 : month + 1;
    return static_cast<int>(daysFromCivil(ny, nm, 1) - daysFromCivil(year, month, 1));
}

const char *monthName(int month)
{
    static const char *names[] = {"gener", "febrer", "març", "abril", "maig", "juny",
                                  "juliol", "agost", "setembre", "octubre", "novembre", "desembre"};
    return (month >= 1 && month <= 12) ? names[month - 1] : "";
}

const char *weekdayName(int wd)
{
    static const char *names[] = {"dilluns", "dimarts", "dimecres", "dijous",
                                  "divendres", "dissabte", "diumenge"};
    return (wd >= 0 && wd <= 6) ? names[wd] : "";
}

std::string formatTime(int minutes)
{
    char b[8];
    snprintf(b, sizeof(b), "%02d:%02d", (minutes / 60) % 24, minutes % 60);
    return b;
}

} // namespace jarvis::agenda
