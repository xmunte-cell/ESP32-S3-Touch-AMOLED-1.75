/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Jarvis settings stored in NVS (namespace "jarvis") and a small web page,
 * served by the board, to edit them from a computer on the same network.
 */
#pragma once

#include <stddef.h>
#include <string>

namespace jarvis {

struct Settings {
    std::string url;          // e.g. http://192.168.131.41:3000 (no trailing slash)
    std::string api_key;      // Open WebUI API key (sk-...)
    std::string model;        // e.g. qwen36
    std::string language;     // e.g. ca
    std::string system_prompt;
    std::string tts_voice;    // empty = no spoken answers (text only)
    std::string tts_model;    // e.g. tts-1
    std::string ics_url;      // Google Calendar private iCal address (secret)
    std::string eap_user;     // user name for a company (WPA2-Enterprise) Wi-Fi
    std::string name;         // assistant name shown on screen (default Jarvis)
};

// Reads the current settings (defaults filled in for empty fields).
Settings load();

// Saves one field. Keys: "url", "key", "model", "lang", "prompt", "voice", "tts_model", "ics", "eap_user", "name".
bool saveField(const char *field, const std::string &value);

// true when URL and API key are set.
bool isComplete();

// Starts the configuration web server on port 80 (safe to call repeatedly).
void startWebServer();

// Writes "192.168.x.y" (or "" when there is no IP yet).
void deviceIp(char *out, size_t len);

} // namespace jarvis
