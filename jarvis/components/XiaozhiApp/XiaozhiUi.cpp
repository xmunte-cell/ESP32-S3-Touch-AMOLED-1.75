/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 * SPDX-FileContributor: Jarvis (round holographic UI)
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Jarvis UI for the 466x466 round AMOLED.
 *
 *            87%
 *          JARVIS
 *     . *  (animated   * .
 *        holographic head)
 *         ==pedestal==
 *          Escoltant
 *   "subtitle text, two lines"
 *          o o o o o
 *   + coloured ring around the edge (colour/animation = state)
 *
 * The head is an original procedural point cloud (no image assets), drawn into
 * a 230x222 RGB565 canvas in PSRAM (~100 KB) at ~15 fps.
 */

#include "XiaozhiUi.hpp"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "XiaozhiLocalization.hpp"
#include "system_status.hpp"

LV_FONT_DECLARE(jv_font_24);
LV_FONT_DECLARE(jv_font_bold_20);
LV_FONT_DECLARE(jv_font_bold_34);

namespace esp_brookesia::apps {

namespace {

constexpr char TAG[] = "JarvisUi";
constexpr char ASSISTANT_NAME[] = "JARVIS";

constexpr int SCREEN = 466;
constexpr int CANVAS_W = 230;
constexpr int CANVAS_H = 222;
constexpr int CANVAS_Y = 86;          // top of the canvas on screen
constexpr float MODEL_SCALE = 80.0f;  // pixels per model unit
constexpr float MODEL_CY = 88.0f;     // canvas y of model origin (head centre)
constexpr int FRAME_MS = 66;          // ~15 fps
constexpr int SPARKLES = 26;

constexpr uint32_t COLOR_IDLE = 0x2BC4D8;
constexpr uint32_t COLOR_CONNECT = 0x4F7DFF;
constexpr uint32_t COLOR_LISTEN = 0x31E1FF;
constexpr uint32_t COLOR_THINK = 0xFFB347;
constexpr uint32_t COLOR_SPEAK = 0x3DF5B5;
constexpr uint32_t COLOR_ERROR = 0xFF5050;

constexpr uint8_t KIND_SKIN = 0;
constexpr uint8_t KIND_EYE = 1;
constexpr uint8_t KIND_LIP_UP = 2;
constexpr uint8_t KIND_LIP_DOWN = 3;
constexpr uint8_t KIND_PEDESTAL = 4;
constexpr uint8_t KIND_NECK = 5;

uint32_t modeColor(XiaozhiUi::Mode mode)
{
    switch (mode) {
    case XiaozhiUi::Mode::Idle: return COLOR_IDLE;
    case XiaozhiUi::Mode::Connecting: return COLOR_CONNECT;
    case XiaozhiUi::Mode::Listening: return COLOR_LISTEN;
    case XiaozhiUi::Mode::Thinking: return COLOR_THINK;
    case XiaozhiUi::Mode::Speaking: return COLOR_SPEAK;
    case XiaozhiUi::Mode::Error: return COLOR_ERROR;
    }
    return COLOR_IDLE;
}

void prepareTransparentObject(lv_obj_t *object)
{
    lv_obj_remove_style_all(object);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(object, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(object, 0, LV_PART_MAIN);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_CLICKABLE);
}

void setLabelTextIfChanged(lv_obj_t *label, const char *text)
{
    if (!label) {
        return;
    }
    const char *value = text ? text : "";
    const char *current = lv_label_get_text(label);
    if (!current || strcmp(current, value) != 0) {
        lv_label_set_text(label, value);
    }
}

void setHidden(lv_obj_t *object, bool hidden)
{
    if (!object || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) == hidden) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    return label;
}

// Additive blend of an RGB888 colour scaled by 'k' (0..255) into an RGB565 pixel.
inline void addPixel(uint16_t *p, uint32_t rgb, int k)
{
    if (k <= 0) {
        return;
    }
    uint16_t v = *p;
    int r = (v >> 11) & 0x1F;
    int g = (v >> 5) & 0x3F;
    int b = v & 0x1F;
    r += (((rgb >> 16) & 0xFF) * k) >> 11;  // /256 then >>3
    g += (((rgb >> 8) & 0xFF) * k) >> 10;   // /256 then >>2
    b += ((rgb & 0xFF) * k) >> 11;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    *p = (uint16_t)((r << 11) | (g << 5) | b);
}

uint32_t mixWhite(uint32_t rgb, int amount)
{
    int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    r += ((255 - r) * amount) >> 8;
    g += ((255 - g) * amount) >> 8;
    b += ((255 - b) * amount) >> 8;
    return (uint32_t)((r << 16) | (g << 8) | b);
}

} // namespace

// ---------------------------------------------------------------------------
XiaozhiUi::~XiaozhiUi()
{
    destroy();
    if (_points) {
        heap_caps_free(_points);
        _points = nullptr;
    }
}

bool XiaozhiUi::preload()
{
    if (!_points) {
        buildModel();
    }
    return true;
}

// Builds an original, stylised head as a cloud of points (model units: the
// head spans y = -1 (crown) .. +1 (chin), x/z are about +-0.75).
void XiaozhiUi::buildModel()
{
    constexpr int MAX_POINTS = 1400;
    _points = static_cast<Point *>(
                  heap_caps_malloc(sizeof(Point) * MAX_POINTS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!_points) {
        _points = static_cast<Point *>(heap_caps_malloc(sizeof(Point) * MAX_POINTS, MALLOC_CAP_8BIT));
    }
    if (!_points) {
        ESP_LOGE(TAG, "No memory for the head model");
        return;
    }
    uint16_t n = 0;
    auto add = [&](float x, float y, float z, uint8_t kind) {
        if (n < MAX_POINTS) {
            _points[n++] = {(int16_t)(x * 1000), (int16_t)(y * 1000), (int16_t)(z * 1000), kind};
        }
    };

    auto halfWidth = [](float y) {
        float a = 0.74f;
        if (y > 0.25f) {
            float t = (y - 0.25f) / 0.75f;
            a *= 1.0f - 0.42f * powf(t, 1.4f);  // jaw tapers towards the chin
        }
        return a;
    };
    auto halfDepth = [](float y) {
        float c = 0.84f;
        if (y > 0.35f) {
            float t = (y - 0.35f) / 0.65f;
            c *= 1.0f - 0.30f * t;
        }
        return c;
    };
    // Front surface depth at (x, y), used to place facial features.
    auto surfaceZ = [&](float x, float y) {
        float s = 1.0f - y * y;
        if (s < 0) s = 0;
        float a = halfWidth(y) * sqrtf(s);
        float c = halfDepth(y) * sqrtf(s);
        float u = a > 0 ? x / a : 0;
        float w = 1.0f - u * u;
        return w > 0 ? c * sqrtf(w) : 0.0f;
    };

    const float eye_y = -0.06f, eye_x = 0.27f, eye_r = 0.085f;

    // Skull and face surface (latitude rings)
    const int rings = 24;
    for (int i = 1; i < rings; i++) {
        float th = (float)M_PI * i / rings;
        float y = -cosf(th);
        if (y > 0.97f) {
            break;
        }
        float s = sinf(th);
        float a = halfWidth(y) * s;
        float c = halfDepth(y) * s;
        int seg = (int)(10 + 34 * s);
        for (int j = 0; j < seg; j++) {
            float ph = 2.0f * (float)M_PI * (j + 0.5f * (i & 1)) / seg;
            float x = a * cosf(ph);
            float z = c * sinf(ph);
            // leave the eye sockets empty so the eyes read clearly
            if (z > 0 && (fabsf(fabsf(x) - eye_x) < eye_r * 1.4f) && fabsf(y - eye_y) < eye_r * 1.3f) {
                continue;
            }
            add(x, y, z, KIND_SKIN);
        }
    }
    // Eyes: bright rings with a pupil
    for (int side = -1; side <= 1; side += 2) {
        float cx = side * eye_x;
        float cz = surfaceZ(cx, eye_y) - 0.02f;
        for (int k = 0; k < 16; k++) {
            float ph = 2.0f * (float)M_PI * k / 16;
            add(cx + eye_r * cosf(ph), eye_y + eye_r * 0.65f * sinf(ph), cz, KIND_EYE);
        }
        add(cx, eye_y, cz + 0.01f, KIND_EYE);
        add(cx + 0.02f, eye_y, cz + 0.01f, KIND_EYE);
        add(cx - 0.02f, eye_y, cz + 0.01f, KIND_EYE);
        // brow
        for (int k = -4; k <= 4; k++) {
            float x = cx + k * 0.03f;
            add(x, eye_y - 0.14f - 0.01f * (4 - abs(k)) * 0.5f, surfaceZ(x, eye_y - 0.14f) + 0.02f, KIND_SKIN);
        }
    }
    // Nose ridge and tip
    for (int k = 0; k <= 12; k++) {
        float t = k / 12.0f;
        float y = -0.02f + 0.30f * t;
        add(0, y, surfaceZ(0, y) + 0.13f * t, KIND_SKIN);
    }
    for (int k = -3; k <= 3; k++) {
        add(k * 0.03f, 0.29f, surfaceZ(k * 0.03f, 0.29f) + 0.08f, KIND_SKIN);
    }
    // Lips (lower lip moves when speaking)
    for (int k = -9; k <= 9; k++) {
        float x = k * 0.024f;
        float curve = 0.018f * (1.0f - (k * k) / 81.0f);
        float z = surfaceZ(x, 0.48f) + 0.03f;
        add(x, 0.47f - curve, z, KIND_LIP_UP);
        add(x, 0.49f + curve, z, KIND_LIP_DOWN);
    }
    // Neck
    for (int r = 0; r < 6; r++) {
        float y = 0.88f + r * 0.09f;
        for (int j = 0; j < 22; j++) {
            float ph = 2.0f * (float)M_PI * (j + 0.5f * (r & 1)) / 22;
            add(0.30f * cosf(ph), y, 0.33f * sinf(ph), KIND_NECK);
        }
    }
    // Pedestal: two flat rings
    for (int ring = 0; ring < 2; ring++) {
        float rad = ring ? 0.98f : 0.70f;
        int seg = ring ? 64 : 48;
        for (int j = 0; j < seg; j++) {
            float ph = 2.0f * (float)M_PI * j / seg;
            add(rad * cosf(ph), 1.48f, rad * sinf(ph), KIND_PEDESTAL);
        }
    }
    _point_count = n;
    ESP_LOGI(TAG, "Head model: %u points", (unsigned)n);
}

// ---------------------------------------------------------------------------
bool XiaozhiUi::create(
    lv_obj_t *parent,
    ActionCallback action_callback,
    void *action_context,
    bool multiline_subtitle
)
{
    (void)multiline_subtitle;
    _action_cb = action_callback;
    _action_ctx = action_context;
    if (!parent) {
        return false;
    }
    destroyView();
    if (!_points) {
        buildModel();
    }

    _root = lv_obj_create(parent);
    prepareTransparentObject(_root);
    lv_obj_set_size(_root, lv_pct(100), lv_pct(100));
    lv_obj_align(_root, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_root, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_font(_root, &jv_font_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(_root, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    // A tap anywhere does the same as the wake word (or stops Jarvis talking).
    lv_obj_add_flag(_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_root, rootClickedCb, LV_EVENT_CLICKED, this);

    // Content is laid out for a 466x466 screen and centred in whatever page
    // area the launcher gives us.
    lv_obj_t *stage = lv_obj_create(_root);
    prepareTransparentObject(stage);
    lv_obj_set_size(stage, SCREEN, SCREEN);
    lv_obj_center(stage);

    // Ring around the edge
    _ring = lv_arc_create(stage);
    lv_obj_set_size(_ring, SCREEN - 10, SCREEN - 10);
    lv_obj_center(_ring);
    lv_obj_remove_style(_ring, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_bg_angles(_ring, 0, 360);
    lv_arc_set_rotation(_ring, 270);
    lv_arc_set_angles(_ring, 0, 360);
    lv_obj_set_style_arc_width(_ring, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_width(_ring, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(_ring, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(_ring, LV_OPA_20, LV_PART_MAIN);

    _battery_label = makeLabel(stage, &jv_font_bold_20, 0xB8C4CC);
    lv_obj_align(_battery_label, LV_ALIGN_TOP_MID, 0, 20);
    lv_label_set_text(_battery_label, "");

    _name_label = makeLabel(stage, &jv_font_bold_34, 0xFFFFFF);
    lv_obj_align(_name_label, LV_ALIGN_TOP_MID, 0, 44);
    lv_label_set_text(_name_label, ASSISTANT_NAME);
    lv_obj_set_style_text_letter_space(_name_label, 3, LV_PART_MAIN);

    // Holographic head canvas
    uint32_t stride_bytes = lv_draw_buf_width_to_stride(CANVAS_W, LV_COLOR_FORMAT_RGB565);
    _canvas_stride_px = stride_bytes / 2;
    size_t buf_size = (size_t)stride_bytes * CANVAS_H;
    _canvas_buf = static_cast<uint16_t *>(
                      heap_caps_aligned_alloc(64, buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (_canvas_buf) {
        memset(_canvas_buf, 0, buf_size);
        _canvas = lv_canvas_create(stage);
        lv_canvas_set_buffer(_canvas, _canvas_buf, CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(_canvas, LV_ALIGN_TOP_MID, 0, CANVAS_Y);
    } else {
        ESP_LOGW(TAG, "No PSRAM for the head canvas, continuing without it");
    }

    _state_label = makeLabel(stage, &jv_font_bold_34, 0xFFFFFF);
    lv_obj_align(_state_label, LV_ALIGN_TOP_MID, 0, CANVAS_Y + CANVAS_H + 4);

    _subtitle_label = makeLabel(stage, &jv_font_24, 0xE6EEF2);
    lv_obj_set_size(_subtitle_label, 292, 62);
    lv_label_set_long_mode(_subtitle_label, LV_LABEL_LONG_MODE_SCROLL);
    lv_obj_set_style_anim_duration(_subtitle_label, 4000, LV_PART_MAIN);
    lv_obj_align(_subtitle_label, LV_ALIGN_TOP_MID, 0, CANVAS_Y + CANVAS_H + 46);
    lv_label_set_text(_subtitle_label, "");

    // Decorative page dots
    _dots = makeLabel(stage, &jv_font_bold_20, 0x5B6B73);
    lv_label_set_recolor(_dots, true);
    lv_label_set_text(_dots, "#FFFFFF \xE2\x80\xA2#  \xE2\x80\xA2  \xE2\x80\xA2  \xE2\x80\xA2  \xE2\x80\xA2");
    lv_obj_align(_dots, LV_ALIGN_BOTTOM_MID, 0, -22);

    // Activation overlay (only shown if a server ever asks for it)
    _activation_box = lv_obj_create(stage);
    prepareTransparentObject(_activation_box);
    lv_obj_set_size(_activation_box, 320, 230);
    lv_obj_align(_activation_box, LV_ALIGN_TOP_MID, 0, CANVAS_Y);
    lv_obj_set_style_bg_color(_activation_box, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_activation_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flex_flow(_activation_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_activation_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(_activation_box, 12, LV_PART_MAIN);
    lv_obj_add_flag(_activation_box, LV_OBJ_FLAG_HIDDEN);
    _activation_code_label = makeLabel(_activation_box, &jv_font_bold_34, 0xFFFFFF);
    lv_obj_set_width(_activation_code_label, lv_pct(100));
    lv_label_set_long_mode(_activation_code_label, LV_LABEL_LONG_MODE_WRAP);
    _activation_message_label = makeLabel(_activation_box, &jv_font_24, 0xB8C4CC);
    lv_obj_set_width(_activation_message_label, lv_pct(100));
    lv_label_set_long_mode(_activation_message_label, LV_LABEL_LONG_MODE_WRAP);

    _mode_since_ms = lv_tick_get();
    applyMode(Mode::Connecting);
    updateBattery();
    _anim_timer = lv_timer_create(animTimerCb, FRAME_MS, this);
    return true;
}

void XiaozhiUi::destroy()
{
    destroyView();
}

void XiaozhiUi::destroyView()
{
    if (_anim_timer) {
        lv_timer_delete(_anim_timer);
        _anim_timer = nullptr;
    }
    if (_root) {
        lv_obj_delete(_root);
    }
    _root = nullptr;
    _ring = nullptr;
    _battery_label = nullptr;
    _name_label = nullptr;
    _canvas = nullptr;
    _state_label = nullptr;
    _subtitle_label = nullptr;
    _dots = nullptr;
    _activation_box = nullptr;
    _activation_code_label = nullptr;
    _activation_message_label = nullptr;
    if (_canvas_buf) {
        heap_caps_free(_canvas_buf);
        _canvas_buf = nullptr;
    }
    _activation_state_valid = false;
    _activation_visible = false;
    _battery_percent = -2;
}

// ---------------------------------------------------------------------------
void XiaozhiUi::setNetworkReady(bool ready)
{
    _network_ready = ready;
}

void XiaozhiUi::setStatus(const char *status)
{
    const char *value = status ? status : "";

    Mode mode = Mode::Connecting;
    auto is = [&](XiaozhiUiText t) { return strcmp(value, xiaozhiUiText(t)) == 0; };
    if (is(XiaozhiUiText::Listening)) {
        mode = Mode::Listening;
    } else if (is(XiaozhiUiText::Processing)) {
        mode = Mode::Thinking;
    } else if (is(XiaozhiUiText::Speaking)) {
        mode = Mode::Speaking;
    } else if (is(XiaozhiUiText::Ready)) {
        mode = Mode::Idle;
    } else if (is(XiaozhiUiText::Error) || is(XiaozhiUiText::NetworkRequired)) {
        mode = Mode::Error;
    }
    // While the board is still "listening" but the request is already on its
    // way to Open WebUI, the client reports the "thinking" emotion.
    if (mode == Mode::Listening && _thinking) {
        mode = Mode::Thinking;
        value = xiaozhiUiText(XiaozhiUiText::Processing);
    }
    setLabelTextIfChanged(_state_label, value);
    if (mode != _mode) {
        applyMode(mode);
    }
}

void XiaozhiUi::applyMode(Mode mode)
{
    _mode = mode;
    _mode_since_ms = lv_tick_get();
    uint32_t color = modeColor(mode);
    if (_ring) {
        lv_obj_set_style_arc_color(_ring, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_set_style_arc_color(_ring, lv_color_hex(color), LV_PART_INDICATOR);
    }
    if (_state_label) {
        lv_obj_set_style_text_color(_state_label, lv_color_hex(mixWhite(color, 170)), LV_PART_MAIN);
    }
}

void XiaozhiUi::setChatMessage(const char *role, const char *text)
{
    if (!_subtitle_label) {
        return;
    }
    bool user = role && strcmp(role, "user") == 0;
    lv_obj_set_style_text_color(
        _subtitle_label, lv_color_hex(user ? 0x9FB3BD : 0xF2F7FA), LV_PART_MAIN);
    setLabelTextIfChanged(_subtitle_label, text);
}

void XiaozhiUi::clearChatMessages()
{
    setChatMessage("system", "");
}

void XiaozhiUi::setEmotion(const char *emotion)
{
    _thinking = emotion && strcmp(emotion, "thinking") == 0;
}

void XiaozhiUi::setActivation(const char *code, const char *message, bool visible)
{
    if (!_activation_box) {
        return;
    }
    setLabelTextIfChanged(_activation_code_label, code);
    setLabelTextIfChanged(_activation_message_label, message);
    if (!_activation_state_valid || _activation_visible != visible) {
        setHidden(_activation_box, !visible);
        setHidden(_canvas, visible);
        _activation_visible = visible;
        _activation_state_valid = true;
    }
}

// ---------------------------------------------------------------------------
void XiaozhiUi::rootClickedCb(lv_event_t *event)
{
    auto *self = static_cast<XiaozhiUi *>(lv_event_get_user_data(event));
    if (self && self->_action_cb) {
        self->_action_cb(self->_action_ctx);
    }
}

void XiaozhiUi::animTimerCb(lv_timer_t *timer)
{
    auto *self = static_cast<XiaozhiUi *>(lv_timer_get_user_data(timer));
    if (!self || !self->_root) {
        return;
    }
    uint32_t now = lv_tick_get();
    self->updateRing(now);
    self->renderFrame();
    if (now - self->_last_battery_ms > 5000) {
        self->updateBattery();
    }
}

void XiaozhiUi::updateBattery()
{
    _last_battery_ms = lv_tick_get();
    if (!_battery_label) {
        return;
    }
    brookesia::system_status::Snapshot snap;
    int pct = -1;
    bool charging = false;
    if (brookesia::system_status::get_snapshot(snap) && snap.battery_valid && snap.battery_present) {
        pct = snap.battery_percent;
        charging = snap.charging;
    }
    int key = pct * 2 + (charging ? 1 : 0);
    if (key == _battery_percent) {
        return;
    }
    _battery_percent = key;
    char text[24];
    if (pct < 0) {
        text[0] = '\0';
    } else {
        snprintf(text, sizeof(text), charging ? "%d%% +" : "%d%%", pct);
    }
    lv_label_set_text(_battery_label, text);
}

void XiaozhiUi::updateRing(uint32_t now)
{
    if (!_ring) {
        return;
    }
    float t = (now - _mode_since_ms) / 1000.0f;
    switch (_mode) {
    case Mode::Thinking:
    case Mode::Connecting: {
        // A segment chasing around the ring
        int start = (int)(t * (_mode == Mode::Thinking ? 260.0f : 160.0f)) % 360;
        lv_arc_set_angles(_ring, start, start + 100);
        lv_obj_set_style_arc_opa(_ring, LV_OPA_COVER, LV_PART_INDICATOR);
        break;
    }
    case Mode::Listening: {
        lv_arc_set_angles(_ring, 0, 360);
        int opa = 150 + (int)(105 * (0.5f + 0.5f * sinf(t * 6.0f)));
        lv_obj_set_style_arc_opa(_ring, (lv_opa_t)opa, LV_PART_INDICATOR);
        break;
    }
    case Mode::Speaking: {
        lv_arc_set_angles(_ring, 0, 360);
        int opa = 170 + (int)(85 * fabsf(sinf(t * 9.0f) * sinf(t * 3.7f)));
        lv_obj_set_style_arc_opa(_ring, (lv_opa_t)opa, LV_PART_INDICATOR);
        break;
    }
    case Mode::Error:
        lv_arc_set_angles(_ring, 0, 360);
        lv_obj_set_style_arc_opa(_ring, LV_OPA_COVER, LV_PART_INDICATOR);
        break;
    case Mode::Idle:
    default: {
        lv_arc_set_angles(_ring, 0, 360);
        int opa = 90 + (int)(40 * (0.5f + 0.5f * sinf(t * 1.2f)));
        lv_obj_set_style_arc_opa(_ring, (lv_opa_t)opa, LV_PART_INDICATOR);
        break;
    }
    }
}

void XiaozhiUi::renderFrame()
{
    if (!_canvas || !_canvas_buf || !_points || lv_obj_has_flag(_canvas, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    const uint32_t now = lv_tick_get();
    const float t = now / 1000.0f;
    const float tm = (now - _mode_since_ms) / 1000.0f;
    const uint32_t color = modeColor(_mode);
    const uint32_t eye_color = mixWhite(color, 150);

    // Head motion per state
    float yaw_amp = 0.30f, yaw_speed = 0.5f, nod = 0.0f;
    switch (_mode) {
    case Mode::Listening: yaw_amp = 0.12f; yaw_speed = 0.8f; nod = 0.06f * sinf(t * 2.2f); break;
    case Mode::Thinking:  yaw_amp = 0.45f; yaw_speed = 0.9f; nod = -0.10f; break;
    case Mode::Speaking:  yaw_amp = 0.18f; yaw_speed = 0.7f; nod = 0.03f * sinf(t * 5.0f); break;
    case Mode::Error:     yaw_amp = 0.05f; nod = 0.15f; break;
    default: break;
    }
    const float yaw = yaw_amp * sinf(t * yaw_speed);
    const float cy = cosf(yaw), sy = sinf(yaw);
    const float cp = cosf(nod), sp = sinf(nod);

    float mouth = 0.0f;
    if (_mode == Mode::Speaking) {
        mouth = fabsf(sinf(t * 11.0f) * 0.7f + sinf(t * 17.0f) * 0.3f);
    }
    // Scanline sweep (a bright band moving down the head)
    const float scan_y = fmodf(t * 0.9f, 3.0f) - 1.2f;

    const uint32_t stride = _canvas_stride_px;
    memset(_canvas_buf, 0, (size_t)stride * 2 * CANVAS_H);

    const float half_w = CANVAS_W * 0.5f;
    for (uint16_t i = 0; i < _point_count; i++) {
        const Point &p = _points[i];
        float x = p.x * 0.001f, y = p.y * 0.001f, z = p.z * 0.001f;
        if (p.kind == KIND_LIP_DOWN) {
            y += mouth * 0.09f;
        }
        float rx, ry, rz;
        if (p.kind == KIND_PEDESTAL) {
            // the pedestal spins slowly on its own and does not nod
            float a = t * 0.6f;
            rx = x * cosf(a) - z * sinf(a);
            rz = x * sinf(a) + z * cosf(a);
            ry = y;
            rz *= 0.18f;  // seen almost edge-on
        } else {
            // yaw (around Y), then nod (around X)
            float x1 = x * cy + z * sy;
            float z1 = -x * sy + z * cy;
            ry = y * cp - z1 * sp;
            rz = y * sp + z1 * cp;
            rx = x1;
        }
        float persp = 1.0f + rz * 0.12f;
        int px = (int)(half_w + rx * MODEL_SCALE * persp);
        int py = (int)(MODEL_CY + ry * MODEL_SCALE * persp);
        if (p.kind == KIND_PEDESTAL) {
            py = (int)(MODEL_CY + y * MODEL_SCALE + rz * 40.0f);
        }
        if (px < 1 || px >= CANVAS_W - 1 || py < 1 || py >= CANVAS_H - 1) {
            continue;
        }

        int k;
        uint32_t c = color;
        bool big = false;
        switch (p.kind) {
        case KIND_EYE:
            c = eye_color;
            k = 255;
            big = true;
            break;
        case KIND_LIP_UP:
        case KIND_LIP_DOWN:
            k = 230;
            big = rz > 0;
            break;
        case KIND_PEDESTAL:
            c = mixWhite(color, 60);
            k = 200;
            big = true;
            break;
        default: {
            // front points brighter than back points
            float d = (rz + 0.9f) / 1.8f;
            if (d < 0) d = 0;
            if (d > 1) d = 1;
            k = 40 + (int)(170 * d);
            big = rz > 0.25f;
            if (p.kind == KIND_NECK) {
                k = k * 3 / 4;
            }
            float ds = fabsf(ry - scan_y);
            if (ds < 0.08f) {
                k += (int)(90 * (1.0f - ds / 0.08f));
            }
            break;
        }
        }
        uint16_t *row = _canvas_buf + (size_t)py * stride;
        addPixel(&row[px], c, k);
        if (big) {
            addPixel(&row[px + 1], c, k * 2 / 3);
            addPixel(&row[px + stride], c, k * 2 / 3);
            addPixel(&row[px + stride + 1], c, k / 2);
        }
    }

    // Pedestal glow (a soft ellipse under the head)
    {
        int gy = (int)(MODEL_CY + 1.48f * MODEL_SCALE);
        for (int dy = -7; dy <= 7; dy++) {
            int yy = gy + dy;
            if (yy < 0 || yy >= CANVAS_H) {
                continue;
            }
            float fy = dy / 7.0f;
            int w = (int)(70 * sqrtf(1.0f - fy * fy));
            uint16_t *row = _canvas_buf + (size_t)yy * stride;
            for (int dx = -w; dx <= w; dx++) {
                int xx = (int)half_w + dx;
                if (xx >= 0 && xx < CANVAS_W) {
                    float fx = (float)dx / (w + 1);
                    addPixel(&row[xx], color, (int)(70 * (1.0f - fx * fx) * (1.0f - fabsf(fy))));
                }
            }
        }
    }

    // Floating sparkles
    for (int s = 0; s < SPARKLES; s++) {
        uint32_t h = (uint32_t)s * 2654435761u;
        float ang = (h & 0xFFFF) / 65535.0f * 6.2832f + t * (0.05f + (h >> 28) * 0.01f);
        float rad = 1.05f + ((h >> 16) & 0xFF) / 255.0f * 0.35f;
        int px = (int)(half_w + cosf(ang) * rad * MODEL_SCALE);
        int py = (int)(MODEL_CY - 0.1f * MODEL_SCALE + sinf(ang) * rad * MODEL_SCALE * 0.9f);
        if (px < 0 || px >= CANVAS_W - 1 || py < 0 || py >= CANVAS_H - 1) {
            continue;
        }
        float tw = 0.5f + 0.5f * sinf(t * (1.5f + (h >> 29)) + s);
        int k = (int)(40 + 200 * tw * tw);
        uint16_t *row = _canvas_buf + (size_t)py * stride;
        addPixel(&row[px], color, k);
        if (tw > 0.8f) {
            addPixel(&row[px + 1], color, k / 2);
        }
    }
    (void)tm;
    lv_obj_invalidate(_canvas);
}

} // namespace esp_brookesia::apps
