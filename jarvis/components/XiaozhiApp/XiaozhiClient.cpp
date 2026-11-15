/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 * SPDX-FileContributor: Jarvis (direct Open WebUI backend)
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Jarvis replacement for the Xiaozhi protocol client.
 *
 * It keeps the exact XiaozhiClient interface used by XiaozhiApp, but instead of
 * talking to a Xiaozhi server it calls Open WebUI directly over HTTP:
 *
 *   uplink Opus (16 kHz, 60 ms) -> decoded to PCM -> end of speech detected here
 *   -> POST {url}/api/v1/audio/transcriptions (WAV, language)   = user text
 *   -> POST {url}/api/chat/completions (model, short history)    = answer text
 *   -> optional POST {url}/api/v1/audio/speech (response_format=wav)
 *      -> encoded to Opus and handed to the app's normal playback path.
 *
 * Settings (URL, key, model, voice...) come from jarvis::load() (NVS).
 */

#include "XiaozhiClient.hpp"

#include <math.h>
#include <string.h>

#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "cJSON.h"
#include "decoder/impl/esp_opus_dec.h"
#include "encoder/impl/esp_opus_enc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "JarvisAgenda.hpp"
#include "JarvisConfig.hpp"

namespace esp_brookesia::apps {

namespace {

constexpr char TAG[] = "JarvisClient";

constexpr int UPLINK_RATE = 16000;
constexpr int FRAME_MS = 60;
constexpr int FRAME_SAMPLES = UPLINK_RATE * FRAME_MS / 1000;  // 960
constexpr int MAX_UTTERANCE_S = 20;
constexpr size_t MAX_PCM_BYTES = (size_t)UPLINK_RATE * 2 * MAX_UTTERANCE_S;
constexpr size_t HEAD_ROOM = 512;   // multipart header goes before the WAV
constexpr size_t TAIL_ROOM = 128;   // multipart trailer goes after it

// End-of-speech detection (energy based) for the "auto" listening mode
constexpr int VAD_RMS_SPEECH = 600;
constexpr int VAD_SILENCE_MS = 900;
constexpr int NO_SPEECH_TIMEOUT_MS = 10000;  // nobody talks -> end the conversation
constexpr int HISTORY_TURNS = 6;

constexpr size_t MAX_TEXT_RESPONSE = 64 * 1024;
constexpr size_t MAX_TTS_RESPONSE = 3 * 1024 * 1024;

const char BOUNDARY[] = "----JarvisFormBoundary7MA4YWxkTrZu0gW";

void put16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}

void put32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

void writeWavHeader(uint8_t *h, size_t pcm_bytes)
{
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + pcm_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1);
    put16(h + 22, 1);
    put32(h + 24, UPLINK_RATE);
    put32(h + 28, UPLINK_RATE * 2);
    put16(h + 32, 2);
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, pcm_bytes);
}

// Growable byte buffer in PSRAM for HTTP responses.
struct Bytes {
    uint8_t *data = nullptr;
    size_t len = 0;
    size_t cap = 0;
    size_t limit = 0;
    explicit Bytes(size_t max) : limit(max) {}
    ~Bytes()
    {
        if (data) {
            heap_caps_free(data);
        }
    }
    bool append(const void *p, size_t n)
    {
        if (len + n > limit) {
            return false;
        }
        if (len + n + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 4096;
            while (ncap < len + n + 1) {
                ncap *= 2;
            }
            auto *nd = static_cast<uint8_t *>(
                           heap_caps_realloc(data, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!nd) {
                return false;
            }
            data = nd;
            cap = ncap;
        }
        memcpy(data + len, p, n);
        len += n;
        data[len] = 0;
        return true;
    }
};

// One HTTP POST. Returns the HTTP status (or -1) and fills 'out'.
int httpPost(
    const std::string &url,
    const std::string &api_key,
    const char *content_type,
    const uint8_t *body,
    size_t body_len,
    Bytes &out,
    int timeout_ms
)
{
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = timeout_ms;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 2048;
    if (url.rfind("https", 0) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return -1;
    }
    std::string auth = "Bearer " + api_key;
    esp_http_client_set_header(c, "Authorization", auth.c_str());
    esp_http_client_set_header(c, "Content-Type", content_type);

    int status = -1;
    if (esp_http_client_open(c, (int)body_len) == ESP_OK) {
        size_t sent = 0;
        bool ok = true;
        while (sent < body_len) {
            int w = esp_http_client_write(c, (const char *)body + sent, (int)(body_len - sent));
            if (w <= 0) {
                ok = false;
                break;
            }
            sent += w;
        }
        if (ok && esp_http_client_fetch_headers(c) >= 0) {
            status = esp_http_client_get_status_code(c);
            char buf[1024];
            int n;
            while ((n = esp_http_client_read(c, buf, sizeof(buf))) > 0) {
                if (!out.append(buf, n)) {
                    ESP_LOGW(TAG, "Response too large, truncated");
                    break;
                }
            }
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return status;
}

std::string statusError(const char *what, int status, const Bytes &body)
{
    std::string e = what;
    if (status == 401 || status == 403) {
        e += ": clau API incorrecta";
    } else if (status < 0) {
        e += ": no arribo al servidor";
    } else {
        e += ": HTTP " + std::to_string(status);
        if (body.data) {
            ESP_LOGW(TAG, "%s body: %.200s", what, (const char *)body.data);
        }
    }
    return e;
}

} // namespace

// ---------------------------------------------------------------------------
class XiaozhiClient::Impl {
public:
    Impl(Config config, Callbacks callbacks) : _config(std::move(config)), _cb(std::move(callbacks)) {}

    ~Impl()
    {
        stop();
        if (_buf) {
            heap_caps_free(_buf);
        }
    }

    esp_err_t start()
    {
        if (_task) {
            return ESP_OK;
        }
        jarvis::startWebServer();
        if (!_buf) {
            _buf = static_cast<uint8_t *>(heap_caps_malloc(
                                              HEAD_ROOM + 44 + MAX_PCM_BYTES + TAIL_ROOM,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!_buf) {
                ESP_LOGE(TAG, "No PSRAM for the audio buffer");
                return ESP_ERR_NO_MEM;
            }
        }
        esp_opus_dec_cfg_t dec = ESP_OPUS_DEC_CONFIG_DEFAULT();
        dec.sample_rate = UPLINK_RATE;
        dec.channel = ESP_AUDIO_MONO;
        dec.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
        dec.self_delimited = false;
        if (esp_opus_dec_open(&dec, sizeof(dec), &_decoder) != ESP_AUDIO_ERR_OK || !_decoder) {
            ESP_LOGE(TAG, "Opus decoder open failed");
            _decoder = nullptr;
            return ESP_FAIL;
        }
        _history.clear();
        _exit = false;
        _work = xSemaphoreCreateBinary();
        if (!_work ||
                xTaskCreatePinnedToCore(taskEntry, "jarvis_http", 10 * 1024, this, 4, &_task, 1) != pdPASS) {
            ESP_LOGE(TAG, "Could not create the HTTP task");
            _task = nullptr;
            return ESP_ERR_NO_MEM;
        }
        _connected = true;
        jarvis::Settings s = jarvis::load();
        ESP_LOGI(TAG, "Jarvis backend ready: %s, model %s, voice %s", s.url.c_str(), s.model.c_str(),
                 s.tts_voice.empty() ? "(text only)" : s.tts_voice.c_str());
        if (_cb.onConnected) {
            _cb.onConnected();
        }
        return ESP_OK;
    }

    esp_err_t stop()
    {
        if (_task) {
            _exit = true;
            _abort = true;
            xSemaphoreGive(_work);
            for (int i = 0; i < 300 && !_task_done; i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            _task = nullptr;
            _task_done = false;
        }
        if (_work) {
            vSemaphoreDelete(_work);
            _work = nullptr;
        }
        if (_decoder) {
            esp_opus_dec_close(_decoder);
            _decoder = nullptr;
        }
        bool was = _connected.exchange(false);
        _channel_open = false;
        if (was && _cb.onDisconnected) {
            _cb.onDisconnected();
        }
        return ESP_OK;
    }

    esp_err_t openAudioChannel()
    {
        if (!_connected) {
            return ESP_ERR_INVALID_STATE;
        }
        _channel_open = true;
        if (_cb.onAudioChannelOpened) {
            _cb.onAudioChannelOpened();
        }
        return ESP_OK;
    }

    esp_err_t closeAudioChannel()
    {
        _listening = false;
        _abort = true;
        bool was = _channel_open.exchange(false);
        if (was && _cb.onAudioChannelClosed) {
            _cb.onAudioChannelClosed();
        }
        return ESP_OK;
    }

    bool isConnected() const { return _connected; }
    bool isAudioChannelOpen() const { return _channel_open; }

    esp_err_t sendAudio(const uint8_t *data, size_t len)
    {
        if (!_listening || !data || len == 0 || !_decoder) {
            return ESP_OK;
        }
        int16_t pcm[FRAME_SAMPLES * 2];
        esp_audio_dec_in_raw_t in = {};
        in.buffer = const_cast<uint8_t *>(data);
        in.len = len;
        in.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;
        esp_audio_dec_out_frame_t out = {};
        out.buffer = reinterpret_cast<uint8_t *>(pcm);
        out.len = sizeof(pcm);
        esp_audio_dec_info_t info = {};
        if (esp_opus_dec_decode(_decoder, &in, &out, &info) != ESP_AUDIO_ERR_OK || out.decoded_size == 0) {
            return ESP_OK;  // a bad packet is not fatal
        }
        size_t bytes = out.decoded_size;
        if (_pcm_len + bytes > MAX_PCM_BYTES) {
            bytes = MAX_PCM_BYTES - _pcm_len;
        }
        memcpy(_buf + HEAD_ROOM + 44 + _pcm_len, pcm, bytes);
        _pcm_len += bytes;

        // Simple end-of-speech detection
        size_t samples = out.decoded_size / 2;
        double acc = 0;
        for (size_t i = 0; i < samples; i++) {
            acc += (double)pcm[i] * pcm[i];
        }
        int rms = samples ? (int)sqrt(acc / samples) : 0;
        int ms = (int)(samples * 1000 / UPLINK_RATE);
        _listened_ms += ms;
        if (rms >= VAD_RMS_SPEECH) {
            _heard_speech = true;
            _silence_ms = 0;
        } else if (_heard_speech) {
            _silence_ms += ms;
        }

        if (_mode != ListeningMode::ManualStop) {
            if (_heard_speech && _silence_ms >= VAD_SILENCE_MS) {
                finishUtterance();
            } else if (!_heard_speech && _listened_ms >= NO_SPEECH_TIMEOUT_MS) {
                ESP_LOGI(TAG, "No speech for %d s, ending the conversation", NO_SPEECH_TIMEOUT_MS / 1000);
                _listening = false;
                if (_cb.onServerGoodbye) {
                    _cb.onServerGoodbye();
                }
                return ESP_OK;
            }
        }
        if (_pcm_len >= MAX_PCM_BYTES) {
            finishUtterance();
        }
        return ESP_OK;
    }

    esp_err_t sendStartListening(ListeningMode mode)
    {
        if (_busy) {
            // still answering the previous question: ignore the restart request
            return ESP_OK;
        }
        _mode = mode;
        _pcm_len = 0;
        _heard_speech = false;
        _silence_ms = 0;
        _listened_ms = 0;
        _abort = false;
        _listening = true;
        return ESP_OK;
    }

    esp_err_t sendStopListening()
    {
        if (_listening) {
            finishUtterance();
        }
        return ESP_OK;
    }

    esp_err_t sendAbortSpeaking()
    {
        _abort = true;
        return ESP_OK;
    }

private:
    void finishUtterance()
    {
        _listening = false;
        if (_busy) {
            return;
        }
        _busy = true;
        _turn_pcm_len = _pcm_len;
        xSemaphoreGive(_work);
    }

    static void taskEntry(void *arg)
    {
        auto *self = static_cast<Impl *>(arg);
        while (!self->_exit) {
            if (xSemaphoreTake(self->_work, pdMS_TO_TICKS(1000)) != pdTRUE) {
                continue;
            }
            if (self->_exit) {
                break;
            }
            self->runTurn();
            self->_busy = false;
        }
        self->_task_done = true;
        vTaskDelete(nullptr);
    }

    void emitText(TextRole role, const std::string &text)
    {
        if (_cb.onText) {
            _cb.onText(role, text);
        }
    }

    void emitTts(TtsState state, const std::string &text = std::string())
    {
        if (_cb.onTts) {
            _cb.onTts(state, text);
        }
    }

    // Ends a turn without an answer so the app goes back to listening.
    void endTurnSilently()
    {
        emitTts(TtsState::Start);
        vTaskDelay(pdMS_TO_TICKS(100));
        emitTts(TtsState::Stop);
    }

    void fail(const std::string &message)
    {
        ESP_LOGE(TAG, "%s", message.c_str());
        emitTts(TtsState::Start);
        emitTts(TtsState::SentenceStart, message);
        emitText(TextRole::Assistant, message);
        vTaskDelay(pdMS_TO_TICKS(2500));
        emitTts(TtsState::Stop);
    }

    void runTurn()
    {
        const size_t pcm_len = _turn_pcm_len;
        if (pcm_len < UPLINK_RATE * 2 * 3 / 10) {  // < 0.3 s
            endTurnSilently();
            return;
        }
        jarvis::Settings s = jarvis::load();
        if (s.url.empty() || s.api_key.empty()) {
            fail("Falta configurar Open WebUI (Settings > Jarvis)");
            return;
        }
        if (_cb.onEmotion) {
            _cb.onEmotion("thinking");
        }

        // ---- 1. Speech to text ----
        std::string text;
        {
            std::string head;
            head += "--"; head += BOUNDARY; head += "\r\n";
            head += "Content-Disposition: form-data; name=\"language\"\r\n\r\n";
            head += s.language; head += "\r\n";
            head += "--"; head += BOUNDARY; head += "\r\n";
            head += "Content-Disposition: form-data; name=\"file\"; filename=\"jarvis.wav\"\r\n";
            head += "Content-Type: audio/wav\r\n\r\n";
            std::string tail = "\r\n--"; tail += BOUNDARY; tail += "--\r\n";
            if (head.size() > HEAD_ROOM - 44) {
                fail("Configuració massa llarga");
                return;
            }
            uint8_t *wav = _buf + HEAD_ROOM;
            writeWavHeader(wav, pcm_len);
            uint8_t *start = wav - head.size();
            memcpy(start, head.data(), head.size());
            memcpy(wav + 44 + pcm_len, tail.data(), tail.size());
            size_t total = head.size() + 44 + pcm_len + tail.size();

            std::string ctype = std::string("multipart/form-data; boundary=") + BOUNDARY;
            Bytes resp(MAX_TEXT_RESPONSE);
            int64_t t0 = esp_log_timestamp();
            int status = httpPost(s.url + "/api/v1/audio/transcriptions", s.api_key, ctype.c_str(),
                                  start, total, resp, 30000);
            ESP_LOGI(TAG, "STT HTTP %d in %ld ms (%.1f s of audio)", status,
                     (long)(esp_log_timestamp() - t0), pcm_len / 2.0 / UPLINK_RATE);
            if (status != 200) {
                fail(statusError("Whisper", status, resp));
                return;
            }
            cJSON *root = cJSON_ParseWithLength((const char *)resp.data, resp.len);
            cJSON *t = root ? cJSON_GetObjectItem(root, "text") : nullptr;
            if (cJSON_IsString(t)) {
                text = t->valuestring;
            }
            cJSON_Delete(root);
        }
        while (!text.empty() && (text.front() == ' ' || text.front() == '\n')) {
            text.erase(0, 1);
        }
        while (!text.empty() && (text.back() == ' ' || text.back() == '\n')) {
            text.pop_back();
        }
        ESP_LOGI(TAG, "User: %s", text.c_str());
        if (text.empty() || text.find("Amara.org") != std::string::npos ||
                text.find("Subtítols") != std::string::npos) {
            endTurnSilently();
            return;
        }
        if (_abort || _exit) {
            return;
        }
        emitText(TextRole::User, text);

        // ---- 2. Chat ----
        std::string answer;
        {
            cJSON *req = cJSON_CreateObject();
            cJSON_AddStringToObject(req, "model", s.model.c_str());
            cJSON_AddNumberToObject(req, "max_tokens", 150);
            cJSON_AddBoolToObject(req, "stream", false);
            cJSON *kw = cJSON_AddObjectToObject(req, "chat_template_kwargs");
            cJSON_AddBoolToObject(kw, "enable_thinking", false);
            cJSON *msgs = cJSON_AddArrayToObject(req, "messages");
            auto addMsg = [&](const char *role, const std::string &content) {
                cJSON *m = cJSON_CreateObject();
                cJSON_AddStringToObject(m, "role", role);
                cJSON_AddStringToObject(m, "content", content.c_str());
                cJSON_AddItemToArray(msgs, m);
            };
            // Today's date and the user's agenda, so "what do I have today?" works.
            std::string agenda_ctx = jarvis::agenda::assistantContext();
            addMsg("system", agenda_ctx.empty() ? s.system_prompt
                                                : s.system_prompt + "\n\n" + agenda_ctx);
            for (auto &h : _history) {
                addMsg(h.first.c_str(), h.second);
            }
            addMsg("user", text);
            char *payload = cJSON_PrintUnformatted(req);
            cJSON_Delete(req);
            if (!payload) {
                fail("Sense memòria");
                return;
            }
            Bytes resp(MAX_TEXT_RESPONSE);
            int64_t t0 = esp_log_timestamp();
            int status = httpPost(s.url + "/api/chat/completions", s.api_key, "application/json",
                                  (const uint8_t *)payload, strlen(payload), resp, 60000);
            cJSON_free(payload);
            ESP_LOGI(TAG, "Chat HTTP %d in %ld ms", status, (long)(esp_log_timestamp() - t0));
            if (status != 200) {
                fail(statusError("Xat", status, resp));
                return;
            }
            cJSON *root = cJSON_ParseWithLength((const char *)resp.data, resp.len);
            cJSON *choices = root ? cJSON_GetObjectItem(root, "choices") : nullptr;
            cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : nullptr;
            cJSON *message = first ? cJSON_GetObjectItem(first, "message") : nullptr;
            cJSON *content = message ? cJSON_GetObjectItem(message, "content") : nullptr;
            if (cJSON_IsString(content)) {
                answer = content->valuestring;
            }
            cJSON_Delete(root);
        }
        size_t think = answer.find("</think>");
        if (think != std::string::npos) {
            answer = answer.substr(think + 8);
        }
        std::string clean;
        for (char c : answer) {
            if (c != '*' && c != '#') {
                clean += (c == '\n') ? ' ' : c;
            }
        }
        answer = clean;
        while (!answer.empty() && answer.front() == ' ') {
            answer.erase(0, 1);
        }
        ESP_LOGI(TAG, "Jarvis: %s", answer.c_str());
        if (answer.empty()) {
            fail("El model no ha respost");
            return;
        }
        _history.emplace_back("user", text);
        _history.emplace_back("assistant", answer);
        while (_history.size() > HISTORY_TURNS * 2) {
            _history.erase(_history.begin());
        }
        if (_abort || _exit) {
            return;
        }

        // ---- 3. Answer (text + optional voice) ----
        if (_cb.onEmotion) {
            _cb.onEmotion("neutral");
        }
        emitTts(TtsState::Start);
        emitTts(TtsState::SentenceStart, answer);
        emitText(TextRole::Assistant, answer);
        bool spoke = false;
        if (!s.tts_voice.empty()) {
            spoke = speak(s, answer);
        }
        if (!spoke) {
            // Text only: leave time to read it before listening again
            int ms = 1500 + (int)answer.size() * 45;
            if (ms > 7000) {
                ms = 7000;
            }
            for (int waited = 0; waited < ms && !_abort && !_exit; waited += 100) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        emitTts(TtsState::Stop);
    }

    // Asks Open WebUI for a WAV and streams it to the app as Opus packets.
    bool speak(const jarvis::Settings &s, const std::string &answer)
    {
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "input", answer.c_str());
        cJSON_AddStringToObject(req, "voice", s.tts_voice.c_str());
        cJSON_AddStringToObject(req, "model", s.tts_model.c_str());
        cJSON_AddStringToObject(req, "response_format", "wav");
        char *payload = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        if (!payload) {
            return false;
        }
        Bytes resp(MAX_TTS_RESPONSE);
        int64_t t0 = esp_log_timestamp();
        int status = httpPost(s.url + "/api/v1/audio/speech", s.api_key, "application/json",
                              (const uint8_t *)payload, strlen(payload), resp, 60000);
        cJSON_free(payload);
        ESP_LOGI(TAG, "TTS HTTP %d in %ld ms (%u bytes)", status, (long)(esp_log_timestamp() - t0),
                 (unsigned)resp.len);
        if (status != 200 || resp.len < 64) {
            return false;
        }

        // --- parse WAV ---
        const uint8_t *d = resp.data;
        if (memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WAVE", 4) != 0) {
            ESP_LOGW(TAG, "TTS answer is not WAV (first bytes %02x %02x %02x); text only. "
                     "Use a TTS engine in Open WebUI that can return WAV.", d[0], d[1], d[2]);
            return false;
        }
        int channels = 1, rate = 0, bits = 0;
        const uint8_t *pcm = nullptr;
        size_t pcm_bytes = 0;
        size_t pos = 12;
        while (pos + 8 <= resp.len) {
            uint32_t sz = d[pos + 4] | (d[pos + 5] << 8) | (d[pos + 6] << 16) | ((uint32_t)d[pos + 7] << 24);
            if (memcmp(d + pos, "fmt ", 4) == 0 && pos + 24 <= resp.len) {
                channels = d[pos + 10] | (d[pos + 11] << 8);
                rate = d[pos + 12] | (d[pos + 13] << 8) | (d[pos + 14] << 16) | (d[pos + 15] << 24);
                bits = d[pos + 22] | (d[pos + 23] << 8);
            } else if (memcmp(d + pos, "data", 4) == 0) {
                pcm = d + pos + 8;
                pcm_bytes = resp.len - (pos + 8);
                if (sz != 0xFFFFFFFF && sz < pcm_bytes) {
                    pcm_bytes = sz;
                }
                break;
            }
            pos += 8 + sz + (sz & 1);
        }
        if (!pcm || bits != 16 || rate <= 0 || channels < 1 || channels > 2) {
            ESP_LOGW(TAG, "Unsupported WAV (%d Hz, %d bits, %d ch)", rate, bits, channels);
            return false;
        }

        // --- encode to Opus 16 kHz / 60 ms and hand over packet by packet ---
        esp_opus_enc_config_t enc = ESP_OPUS_ENC_CONFIG_DEFAULT();
        enc.sample_rate = UPLINK_RATE;
        enc.channel = ESP_AUDIO_MONO;
        enc.bits_per_sample = ESP_AUDIO_BIT16;
        enc.bitrate = 32000;
        enc.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
        enc.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;
        enc.complexity = 0;
        enc.enable_fec = false;
        enc.enable_dtx = false;
        enc.enable_vbr = true;
        void *encoder = nullptr;
        int in_size = 0, out_size = 0;
        if (esp_opus_enc_open(&enc, sizeof(enc), &encoder) != ESP_AUDIO_ERR_OK || !encoder ||
                esp_opus_enc_get_frame_size(encoder, &in_size, &out_size) != ESP_AUDIO_ERR_OK ||
                in_size != FRAME_SAMPLES * 2) {
            if (encoder) {
                esp_opus_enc_close(encoder);
            }
            ESP_LOGE(TAG, "Opus encoder open failed");
            return false;
        }
        std::vector<uint8_t> packet(out_size > 0 ? out_size : 1500);
        int16_t frame[FRAME_SAMPLES];
        const size_t in_frames = pcm_bytes / (2 * channels);
        const double step = (double)rate / UPLINK_RATE;  // linear resampling to 16 kHz
        const auto *src = reinterpret_cast<const int16_t *>(pcm);
        double src_pos = 0;
        uint32_t timestamp = 0;
        TickType_t start_tick = xTaskGetTickCount();
        int sent = 0;
        bool any = false;
        while (!_abort && !_exit) {
            int filled = 0;
            for (; filled < FRAME_SAMPLES; filled++) {
                size_t i = (size_t)src_pos;
                if (i + 1 >= in_frames) {
                    break;
                }
                double f = src_pos - i;
                int a = channels == 2 ? (src[2 * i] + src[2 * i + 1]) / 2 : src[i];
                int b = channels == 2 ? (src[2 * i + 2] + src[2 * i + 3]) / 2 : src[i + 1];
                frame[filled] = (int16_t)(a + (b - a) * f);
                src_pos += step;
            }
            if (filled == 0) {
                break;
            }
            for (int k = filled; k < FRAME_SAMPLES; k++) {
                frame[k] = 0;
            }
            esp_audio_enc_in_frame_t in = {};
            in.buffer = reinterpret_cast<uint8_t *>(frame);
            in.len = sizeof(frame);
            esp_audio_enc_out_frame_t out = {};
            out.buffer = packet.data();
            out.len = packet.size();
            if (esp_opus_enc_process(encoder, &in, &out) == ESP_AUDIO_ERR_OK && out.encoded_bytes > 0) {
                if (_cb.onAudio) {
                    _cb.onAudio(packet.data(), out.encoded_bytes, UPLINK_RATE, FRAME_MS, timestamp);
                }
                any = true;
            }
            timestamp += FRAME_MS;
            sent++;
            // pace at real time, a few frames ahead
            if (sent > 4) {
                vTaskDelayUntil(&start_tick, pdMS_TO_TICKS(FRAME_MS));
            }
            if (filled < FRAME_SAMPLES) {
                break;
            }
        }
        esp_opus_enc_close(encoder);
        // let the last packets play out
        vTaskDelay(pdMS_TO_TICKS(300));
        return any;
    }

    Config _config;
    Callbacks _cb;

    uint8_t *_buf = nullptr;
    size_t _pcm_len = 0;
    size_t _turn_pcm_len = 0;
    void *_decoder = nullptr;

    std::atomic<bool> _connected{false};
    std::atomic<bool> _channel_open{false};
    std::atomic<bool> _listening{false};
    std::atomic<bool> _busy{false};
    std::atomic<bool> _abort{false};
    std::atomic<bool> _exit{false};
    std::atomic<bool> _task_done{false};
    ListeningMode _mode = ListeningMode::AutoStop;
    bool _heard_speech = false;
    int _silence_ms = 0;
    int _listened_ms = 0;

    TaskHandle_t _task = nullptr;
    SemaphoreHandle_t _work = nullptr;
    std::vector<std::pair<std::string, std::string>> _history;
};

// ---------------------------------------------------------------------------
XiaozhiClient::Config::Config(
    bool mqtt_config,
    bool websocket_config,
    int uplink_sample_rate,
    int uplink_channels,
    int frame_duration,
    int output_sample_rate
):
    has_mqtt_config(mqtt_config),
    has_websocket_config(websocket_config),
    sample_rate(uplink_sample_rate),
    channels(uplink_channels),
    frame_duration_ms(frame_duration),
    playback_sample_rate(output_sample_rate)
{
}

XiaozhiClient::XiaozhiClient(Config config, Callbacks callbacks):
    _impl(new (std::nothrow) Impl(std::move(config), std::move(callbacks)))
{
}

XiaozhiClient::~XiaozhiClient()
{
    delete _impl;
    _impl = nullptr;
}

esp_err_t XiaozhiClient::start()
{
    return _impl ? _impl->start() : ESP_ERR_NO_MEM;
}

esp_err_t XiaozhiClient::stop()
{
    return _impl ? _impl->stop() : ESP_ERR_INVALID_STATE;
}

esp_err_t XiaozhiClient::openAudioChannel()
{
    return _impl ? _impl->openAudioChannel() : ESP_ERR_INVALID_STATE;
}

esp_err_t XiaozhiClient::closeAudioChannel()
{
    return _impl ? _impl->closeAudioChannel() : ESP_ERR_INVALID_STATE;
}

bool XiaozhiClient::isConnected() const
{
    return _impl && _impl->isConnected();
}

bool XiaozhiClient::isAudioChannelOpen() const
{
    return _impl && _impl->isAudioChannelOpen();
}

esp_err_t XiaozhiClient::sendAudio(const uint8_t *data, size_t len, uint32_t timestamp)
{
    (void)timestamp;
    return _impl ? _impl->sendAudio(data, len) : ESP_ERR_INVALID_STATE;
}

esp_err_t XiaozhiClient::sendWakeWordDetected(const std::string &wake_word)
{
    ESP_LOGI(TAG, "Wake word: %s", wake_word.c_str());
    return _impl ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t XiaozhiClient::sendStartListening(ListeningMode mode)
{
    return _impl ? _impl->sendStartListening(mode) : ESP_ERR_INVALID_STATE;
}

esp_err_t XiaozhiClient::sendStopListening()
{
    return _impl ? _impl->sendStopListening() : ESP_ERR_INVALID_STATE;
}

esp_err_t XiaozhiClient::sendAbortSpeaking(AbortReason reason)
{
    (void)reason;
    return _impl ? _impl->sendAbortSpeaking() : ESP_ERR_INVALID_STATE;
}

} // namespace esp_brookesia::apps
