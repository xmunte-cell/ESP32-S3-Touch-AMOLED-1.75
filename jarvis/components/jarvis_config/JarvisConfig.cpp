/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "JarvisConfig.hpp"
#include "JarvisAgenda.hpp"
#include "JarvisWifi.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mutex>
#include <vector>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs.h"

namespace jarvis {

namespace {

constexpr char TAG[] = "JarvisConfig";
constexpr char NVS_NS[] = "jarvis";

constexpr char DEFAULT_MODEL[] = "qwen36";
constexpr char DEFAULT_LANGUAGE[] = "ca";
constexpr char DEFAULT_NAME[] = "Jarvis";

// "ONA" -> "Ona" for sentences.
std::string titleCase(const std::string &name)
{
    std::string out = name;
    bool first = true;
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z' && !first) {
            c = static_cast<char>(c - 'A' + 'a');
        } else if (c >= 'a' && c <= 'z' && first) {
            c = static_cast<char>(c - 'a' + 'A');
        }
        first = c == ' ' || c == '-';
    }
    return out;
}

std::string defaultPrompt(const std::string &name)
{
    return "Ets " + titleCase(name) + ", un assistent de veu. Respon sempre en català, en una o dues "
           "frases curtes, sense llistes ni format.";
}
constexpr char DEFAULT_TTS_MODEL[] = "tts-1";

std::mutex g_mutex;
httpd_handle_t g_server = nullptr;

std::string nvsGet(nvs_handle_t h, const char *key)
{
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0) {
        return {};
    }
    std::string out(len, '\0');
    if (nvs_get_str(h, key, out.data(), &len) != ESP_OK) {
        return {};
    }
    out.resize(len > 0 ? len - 1 : 0);
    return out;
}

void trimSlash(std::string &s)
{
    while (!s.empty() && (s.back() == '/' || s.back() == ' ')) {
        s.pop_back();
    }
    while (!s.empty() && s.front() == ' ') {
        s.erase(0, 1);
    }
}

std::string htmlEscape(const std::string &in)
{
    std::string out;
    for (char c : in) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c; break;
        }
    }
    return out;
}

std::string urlDecode(const char *s, size_t n)
{
    std::string out;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < n) {
            char hex[3] = {s[i + 1], s[i + 2], 0};
            out += (char)strtol(hex, nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Finds "name=value" in an x-www-form-urlencoded body.
bool formField(const std::string &body, const char *name, std::string &value)
{
    size_t nlen = strlen(name);
    size_t pos = 0;
    while (pos < body.size()) {
        size_t amp = body.find('&', pos);
        if (amp == std::string::npos) {
            amp = body.size();
        }
        if (amp - pos > nlen && body.compare(pos, nlen, name) == 0 && body[pos + nlen] == '=') {
            value = urlDecode(body.data() + pos + nlen + 1, amp - pos - nlen - 1);
            return true;
        }
        pos = amp + 1;
    }
    return false;
}

const char PAGE_HEAD[] =
    "<!doctype html><html lang=ca><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Jarvis</title><style>"
    "body{font-family:system-ui,sans-serif;background:#0b1114;color:#e6eef2;margin:0;padding:24px}"
    "main{max-width:560px;margin:auto}h1{color:#31e1ff;letter-spacing:2px}"
    "label{display:block;margin:16px 0 4px;font-weight:600}"
    "input,textarea{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;"
    "border:1px solid #2a3a42;background:#121c21;color:#fff;font-size:16px}"
    "small{color:#8aa0aa}button{margin-top:20px;padding:12px 20px;border:0;border-radius:8px;"
    "background:#31e1ff;color:#00222a;font-weight:700;font-size:16px;cursor:pointer}"
    "a{color:#31e1ff}.ok{color:#3df5b5}.err{color:#ff7070}"
    "nav a{margin-right:16px}table{width:100%;border-collapse:collapse}"
    "td{padding:6px 4px;border-bottom:1px solid #1e2a30;vertical-align:top}"
    ".g{color:#8aa0aa;font-size:13px}.day{color:#ffb347;font-weight:700;padding-top:16px}"
    "button.del{margin:0;padding:4px 10px;background:#3a1d22;color:#ff9090}"
    ".row{display:flex;gap:8px}.row>div{flex:1}"
    "</style></head><body><main><h1>JARVIS</h1>"
    "<nav><a href=/>Configuració</a><a href=/agenda>Agenda</a><a href=/wifi>Wi-Fi</a></nav>";

esp_err_t sendPage(httpd_req_t *req, const std::string &body)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, body.c_str());
    httpd_resp_sendstr_chunk(req, "</main></body></html>");
    return httpd_resp_sendstr_chunk(req, nullptr);
}

esp_err_t handleRoot(httpd_req_t *req)
{
    Settings s = load();
    std::string b;
    b += "<p>Configuració de la connexió amb Open WebUI.</p>";
    b += "<form method=post action=/save>";
    b += "<label>Adreça d'Open WebUI</label><input name=url placeholder='http://192.168.131.41:3000' value=\"";
    b += htmlEscape(s.url);
    b += "\"><small>Amb http:// i el port, sense barra final.</small>";
    b += "<label>Clau API</label><input name=key type=password autocomplete=off placeholder='";
    b += s.api_key.empty() ? "sk-..." : "(desada; deixa-ho buit per mantenir-la)";
    b += "'><small>Open WebUI &gt; Configuració &gt; Compte &gt; Claus API.</small>";
    b += "<label>Nom de l'assistent</label><input name=name maxlength=16 value=\"" + htmlEscape(s.name) + "\">";
    b += "<small>Surt a la pantalla. La paraula per activar-lo continua sent \"Jarvis\".</small>";
    b += "<label>Model</label><input name=model value=\"" + htmlEscape(s.model) + "\">";
    b += "<label>Idioma de Whisper</label><input name=lang value=\"" + htmlEscape(s.language) + "\">";
    b += "<label>Instruccions (system prompt)</label><textarea name=prompt rows=4>" +
         htmlEscape(s.system_prompt) + "</textarea>";
    b += "<label>Veu (TTS d'Open WebUI)</label><input name=voice value=\"" + htmlEscape(s.tts_voice) +
         "\" placeholder='buit = respon només amb text'>";
    b += "<small>Només si a Open WebUI hi ha un motor de veu configurat (Admin &gt; Àudio). "
         "Ha de poder tornar àudio WAV.</small>";
    b += "<label>Model de veu</label><input name=tts_model value=\"" + htmlEscape(s.tts_model) + "\">";
    b += "<label>Calendari de Google (adreça secreta en format iCal)</label>"
         "<input name=ics type=password autocomplete=off placeholder='";
    b += s.ics_url.empty() ? "https://calendar.google.com/calendar/ical/.../basic.ics"
                           : "(desada; buit per mantenir-la, - per esborrar-la)";
    b += "'><small>Google Calendar &gt; Configuració &gt; el teu calendari &gt; "
         "Integra el calendari &gt; Adreça secreta en format iCal. Només lectura.</small>";
    b += "<button type=submit>Desar</button></form>";
    b += "<p><a href=/test>Provar la connexió amb Open WebUI</a></p>";
    return sendPage(req, b);
}

bool readBody(httpd_req_t *req, std::string &body)
{
    if (req->content_len > 8192) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Massa dades");
        return false;
    }
    body.assign(req->content_len, '\0');
    size_t got = 0;
    while (got < body.size()) {
        int r = httpd_req_recv(req, body.data() + got, body.size() - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return false;
        }
        got += r;
    }
    return true;
}

esp_err_t redirectTo(httpd_req_t *req, const char *where)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", where);
    return httpd_resp_send(req, nullptr, 0);
}

// "2026-11-15" -> 20261115, "09:30" -> 570 (empty -> -1)
int32_t parseIsoDate(const std::string &s)
{
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') {
        return 0;
    }
    return atoi(s.substr(0, 4).c_str()) * 10000 + atoi(s.substr(5, 2).c_str()) * 100 +
           atoi(s.substr(8, 2).c_str());
}

int16_t parseClock(const std::string &s)
{
    if (s.size() < 4 || s.find(':') == std::string::npos) {
        return -1;
    }
    int h = atoi(s.c_str());
    int m = atoi(s.c_str() + s.find(':') + 1);
    if (h < 0 || h > 23 || m < 0 || m > 59) {
        return -1;
    }
    return static_cast<int16_t>(h * 60 + m);
}

std::string dayHeading(int32_t date)
{
    char b[64];
    snprintf(b, sizeof(b), "%s %d de %s", agenda::weekdayName(agenda::weekday(date)),
             static_cast<int>(date % 100), agenda::monthName((date / 100) % 100));
    return b;
}

std::string eventLine(const agenda::Event &e)
{
    std::string t = e.start >= 0 ? agenda::formatTime(e.start) : std::string("tot el dia");
    if (e.start >= 0 && e.end >= 0) {
        t += "&ndash;" + agenda::formatTime(e.end);
    }
    std::string out = "<td style='white-space:nowrap'>" + t + "</td><td>" + htmlEscape(e.title);
    if (!e.place.empty()) {
        out += "<div class=g>" + htmlEscape(e.place) + "</div>";
    }
    return out + "</td>";
}

esp_err_t handleAgenda(httpd_req_t *req)
{
    agenda::begin();
    std::string b;
    int32_t t = agenda::today();
    b += "<h2>Agenda</h2><p class=g>" + htmlEscape(agenda::status()) + "</p>";
    b += "<form method=post action=/agenda/sync><button type=submit>Tornar a llegir Google Calendar</button></form>";

    b += "<h3>Propers 14 dies</h3>";
    if (!t) {
        b += "<p class=err>La placa encara no té l'hora (cal WiFi).</p>";
    } else {
        std::vector<agenda::Event> ev = agenda::upcoming(14);
        if (ev.empty()) {
            b += "<p>No hi ha res a l'agenda.</p>";
        } else {
            b += "<table>";
            int32_t last = 0;
            for (const auto &e : ev) {
                if (e.date != last) {
                    last = e.date;
                    b += "<tr><td colspan=3 class=day>" + dayHeading(e.date) + "</td></tr>";
                }
                b += "<tr>" + eventLine(e) + "<td class=g>" + (e.google ? "Google" : "placa") + "</td></tr>";
            }
            b += "</table>";
        }
    }

    b += "<h3>Afegir un esdeveniment a la placa</h3>"
         "<form method=post action=/agenda/add>"
         "<label>Títol</label><input name=title maxlength=80 required>"
         "<div class=row><div><label>Dia</label><input name=date type=date required></div>"
         "<div><label>Hora</label><input name=start type=time></div>"
         "<div><label>Fins a</label><input name=end type=time></div></div>"
         "<small>Sense hora = tot el dia.</small>"
         "<label>Lloc (opcional)</label><input name=place maxlength=60>"
         "<button type=submit>Afegir</button></form>";

    std::vector<agenda::Event> local = agenda::localEvents();
    if (!local.empty()) {
        b += "<h3>Esdeveniments de la placa</h3><table>";
        for (size_t i = 0; i < local.size(); i++) {
            const auto &e = local[i];
            b += "<tr><td class=g>" + dayHeading(e.date) + "</td>" + eventLine(e) +
                 "<td><form method=post action=/agenda/del><input type=hidden name=i value=" +
                 std::to_string(i) + "><button class=del type=submit>Esborrar</button></form></td></tr>";
        }
        b += "</table>";
    }
    b += "<p class=g>Els esdeveniments de Google només es poden canviar a Google Calendar. "
         "La placa el torna a llegir cada 30 minuts.</p>";
    return sendPage(req, b);
}

esp_err_t handleAgendaAdd(httpd_req_t *req)
{
    std::string body;
    if (!readBody(req, body)) {
        return ESP_FAIL;
    }
    std::string title, date, start, end, place;
    formField(body, "title", title);
    formField(body, "date", date);
    formField(body, "start", start);
    formField(body, "end", end);
    formField(body, "place", place);
    agenda::Event e;
    e.date = parseIsoDate(date);
    e.start = parseClock(start);
    e.end = e.start >= 0 ? parseClock(end) : -1;
    e.title = title;
    e.place = place;
    if (!agenda::addLocal(e)) {
        return sendPage(req, "<p class=err>No s'ha pogut afegir (cal títol i dia).</p><p><a href=/agenda>Tornar</a></p>");
    }
    return redirectTo(req, "/agenda");
}

esp_err_t handleAgendaDel(httpd_req_t *req)
{
    std::string body, idx;
    if (!readBody(req, body)) {
        return ESP_FAIL;
    }
    if (formField(body, "i", idx)) {
        agenda::removeLocal(static_cast<size_t>(atoi(idx.c_str())));
    }
    return redirectTo(req, "/agenda");
}

// Saved Wi-Fi networks: list, add (normal or company) and forget.
esp_err_t handleWifi(httpd_req_t *req)
{
    std::vector<wifi::Network> nets = wifi::savedNetworks();
    std::string b = "<h2>Xarxes Wi-Fi desades</h2>"
                    "<p class=g>La placa es connecta sola a la xarxa desada que tingui a l'abast "
                    "(m&agrave;xim 5). Les contrasenyes no es mostren mai.</p>";
    if (nets.empty()) {
        b += "<p class=g>Encara no n'hi ha cap.</p>";
    } else {
        b += "<table>";
        for (size_t i = 0; i < nets.size(); i++) {
            b += "<tr><td>" + htmlEscape(nets[i].ssid) + "<div class=g>" +
                 (nets[i].user.empty() ? std::string("Nom&eacute;s contrasenya")
                                       : "Usuari: " + htmlEscape(nets[i].user)) +
                 "</div></td><td style='width:1%'><form method=post action=/wifi/del>"
                 "<input type=hidden name=i value=" + std::to_string(i) +
                 "><button class=del type=submit>Oblidar</button></form></td></tr>";
        }
        b += "</table>";
    }
    b += "<h2>Afegir una xarxa</h2><form method=post action=/wifi/add>"
         "<label>Nom de la xarxa (SSID)</label><input name=ssid maxlength=32 required>"
         "<label>Usuari <small>(nom&eacute;s xarxes amb usuari i contrasenya, com la del centre)</small></label>"
         "<input name=user maxlength=64 autocomplete=off>"
         "<label>Contrasenya</label><input name=pass type=password maxlength=63 autocomplete=off>"
         "<button type=submit>Desar la xarxa</button></form>"
         "<p class=g>Si una xarxa ja hi &eacute;s, es substitueix. Tamb&eacute; pots afegir-ne "
         "des de la placa: Settings &gt; WLAN.</p>";
    return sendPage(req, b);
}

esp_err_t handleWifiAdd(httpd_req_t *req)
{
    std::string body;
    if (!readBody(req, body)) {
        return ESP_FAIL;
    }
    wifi::Network n;
    formField(body, "ssid", n.ssid);
    formField(body, "user", n.user);
    formField(body, "pass", n.password);
    bool ok = !n.ssid.empty() && (n.password.empty() || n.user.size() > 0 || n.password.size() >= 8);
    if (!ok || !wifi::rememberNetwork(n)) {
        return sendPage(req, "<p class=err>No s'ha pogut desar (cal el nom de la xarxa i, "
                             "si no hi ha usuari, una contrasenya de 8 car&agrave;cters o m&eacute;s).</p>"
                             "<p><a href=/wifi>Tornar</a></p>");
    }
    return redirectTo(req, "/wifi");
}

esp_err_t handleWifiDel(httpd_req_t *req)
{
    std::string body, idx;
    if (!readBody(req, body)) {
        return ESP_FAIL;
    }
    if (formField(body, "i", idx)) {
        wifi::forgetNetwork(static_cast<size_t>(atoi(idx.c_str())));
    }
    return redirectTo(req, "/wifi");
}

esp_err_t handleAgendaSync(httpd_req_t *req)
{
    std::string body;
    readBody(req, body);
    agenda::requestSync();
    return sendPage(req, "<p class=ok>Llegint Google Calendar... pot trigar uns segons.</p>"
                         "<p><a href=/agenda>Tornar a l'agenda</a></p>");
}

esp_err_t handleSave(httpd_req_t *req)
{
    std::string body;
    if (!readBody(req, body)) {
        return ESP_FAIL;
    }

    std::string url, key, model, lang, prompt, voice, tts_model, ics;
    formField(body, "url", url);
    formField(body, "key", key);
    formField(body, "model", model);
    formField(body, "lang", lang);
    formField(body, "prompt", prompt);
    formField(body, "voice", voice);
    formField(body, "tts_model", tts_model);
    formField(body, "ics", ics);
    std::string name;
    formField(body, "name", name);
    trimSlash(name);
    {
        // Keep the prompt "automatic" (follows the name) unless it was edited.
        Settings before = load();
        if (prompt == before.system_prompt && prompt == defaultPrompt(before.name)) {
            prompt.clear();
        }
    }
    trimSlash(ics);
    trimSlash(url);
    trimSlash(key);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "url", url.c_str());
            if (!key.empty()) {
                nvs_set_str(h, "key", key.c_str());
            }
            nvs_set_str(h, "model", model.c_str());
            nvs_set_str(h, "lang", lang.c_str());
            nvs_set_str(h, "prompt", prompt.c_str());
            nvs_set_str(h, "voice", voice.c_str());
            nvs_set_str(h, "tts_model", tts_model.c_str());
            nvs_set_str(h, "name", name.c_str());
            if (ics == "-") {
                nvs_erase_key(h, "ics");
            } else if (!ics.empty()) {
                nvs_set_str(h, "ics", ics.c_str());
            }
            nvs_commit(h);
            nvs_close(h);
        }
    }
    ESP_LOGI(TAG, "Settings saved (url=%s, model=%s, key %s, calendar %s)", url.c_str(), model.c_str(),
             key.empty() ? "unchanged" : "updated", ics.empty() ? "unchanged" : "updated");
    if (!ics.empty()) {
        agenda::requestSync();
    }
    return sendPage(req,
                    "<p class=ok>Desat. Torna a l'app AIChats de la placa: es connectarà "
                    "en uns segons.</p><p><a href=/>Tornar</a> &middot; <a href=/test>Provar la connexió</a></p>");
}

esp_err_t handleTest(httpd_req_t *req)
{
    Settings s = load();
    if (s.url.empty() || s.api_key.empty()) {
        return sendPage(req, "<p class=err>Falta l'adreça o la clau.</p><p><a href=/>Tornar</a></p>");
    }
    std::string url = s.url + "/api/models";
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 8000;
    if (url.rfind("https", 0) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    std::string auth = "Bearer " + s.api_key;
    esp_http_client_set_header(c, "Authorization", auth.c_str());
    int status = -1;
    std::string resp;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        char buf[512];
        int n;
        while ((n = esp_http_client_read(c, buf, sizeof(buf))) > 0 && resp.size() < 32768) {
            resp.append(buf, n);
        }
    }
    esp_http_client_cleanup(c);

    std::string b;
    if (status == 200) {
        bool has_model = resp.find("\"" + s.model + "\"") != std::string::npos;
        b = "<p class=ok>Connexió correcta amb Open WebUI.</p>";
        b += has_model ? "<p class=ok>El model <b>" + htmlEscape(s.model) + "</b> hi és.</p>"
                       : "<p class=err>No trobo el model <b>" + htmlEscape(s.model) +
                             "</b> a la llista. Revisa'n el nom.</p>";
    } else if (status == 401 || status == 403) {
        b = "<p class=err>La clau API no és vàlida (HTTP " + std::to_string(status) + ").</p>";
    } else if (status < 0) {
        b = "<p class=err>No arribo a " + htmlEscape(s.url) +
            ". Revisa l'adreça i que la placa sigui a la mateixa xarxa.</p>";
    } else {
        b = "<p class=err>Resposta inesperada: HTTP " + std::to_string(status) + "</p>";
    }
    b += "<p><a href=/>Tornar</a></p>";
    return sendPage(req, b);
}

} // namespace

Settings load()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    Settings s;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        s.url = nvsGet(h, "url");
        s.api_key = nvsGet(h, "key");
        s.model = nvsGet(h, "model");
        s.language = nvsGet(h, "lang");
        s.system_prompt = nvsGet(h, "prompt");
        s.tts_voice = nvsGet(h, "voice");
        s.tts_model = nvsGet(h, "tts_model");
        s.ics_url = nvsGet(h, "ics");
        s.eap_user = nvsGet(h, "eap_user");
        s.name = nvsGet(h, "name");
        nvs_close(h);
    }
    if (s.model.empty()) s.model = DEFAULT_MODEL;
    if (s.language.empty()) s.language = DEFAULT_LANGUAGE;
    if (s.name.empty()) s.name = DEFAULT_NAME;
    if (s.system_prompt.empty()) s.system_prompt = defaultPrompt(s.name);
    if (s.tts_model.empty()) s.tts_model = DEFAULT_TTS_MODEL;
    return s;
}

bool saveField(const char *field, const std::string &value)
{
    std::string v = value;
    if (strcmp(field, "url") == 0 || strcmp(field, "key") == 0 || strcmp(field, "ics") == 0 ||
            strcmp(field, "eap_user") == 0 || strcmp(field, "name") == 0) {
        trimSlash(v);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_str(h, field, v.c_str()) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    ESP_LOGI(TAG, "Setting '%s' saved from the touch screen", field);
    return ok;
}

bool isComplete()
{
    Settings s = load();
    return !s.url.empty() && !s.api_key.empty();
}

void startWebServer()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_server) {
        return;
    }
    agenda::begin();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 12;
    cfg.lru_purge_enable = true;
    if (httpd_start(&g_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the settings web server");
        g_server = nullptr;
        return;
    }
    httpd_uri_t root = {};
    root.uri = "/";
    root.method = HTTP_GET;
    root.handler = handleRoot;
    httpd_register_uri_handler(g_server, &root);
    httpd_uri_t save = {};
    save.uri = "/save";
    save.method = HTTP_POST;
    save.handler = handleSave;
    httpd_register_uri_handler(g_server, &save);
    httpd_uri_t test = {};
    test.uri = "/test";
    test.method = HTTP_GET;
    test.handler = handleTest;
    httpd_register_uri_handler(g_server, &test);
    httpd_uri_t ag = {};
    ag.uri = "/agenda";
    ag.method = HTTP_GET;
    ag.handler = handleAgenda;
    httpd_register_uri_handler(g_server, &ag);
    httpd_uri_t ag_add = {};
    ag_add.uri = "/agenda/add";
    ag_add.method = HTTP_POST;
    ag_add.handler = handleAgendaAdd;
    httpd_register_uri_handler(g_server, &ag_add);
    httpd_uri_t ag_del = {};
    ag_del.uri = "/agenda/del";
    ag_del.method = HTTP_POST;
    ag_del.handler = handleAgendaDel;
    httpd_register_uri_handler(g_server, &ag_del);
    httpd_uri_t ag_sync = {};
    ag_sync.uri = "/agenda/sync";
    ag_sync.method = HTTP_POST;
    ag_sync.handler = handleAgendaSync;
    httpd_register_uri_handler(g_server, &ag_sync);
    httpd_uri_t wf = {};
    wf.uri = "/wifi";
    wf.method = HTTP_GET;
    wf.handler = handleWifi;
    httpd_register_uri_handler(g_server, &wf);
    httpd_uri_t wf_add = {};
    wf_add.uri = "/wifi/add";
    wf_add.method = HTTP_POST;
    wf_add.handler = handleWifiAdd;
    httpd_register_uri_handler(g_server, &wf_add);
    httpd_uri_t wf_del = {};
    wf_del.uri = "/wifi/del";
    wf_del.method = HTTP_POST;
    wf_del.handler = handleWifiDel;
    httpd_register_uri_handler(g_server, &wf_del);
    ESP_LOGI(TAG, "Settings web server started on port 80");
}

void deviceIp(char *out, size_t len)
{
    if (!out || len == 0) {
        return;
    }
    out[0] = '\0';
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {};
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
        snprintf(out, len, IPSTR, IP2STR(&info.ip));
    }
}

} // namespace jarvis
