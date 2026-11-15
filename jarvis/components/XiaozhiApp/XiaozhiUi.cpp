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

#include "JarvisConfig.hpp"
#include "XiaozhiLocalization.hpp"
#include "system_status.hpp"

LV_FONT_DECLARE(jv_font_24);
LV_FONT_DECLARE(jv_font_bold_20);
LV_FONT_DECLARE(jv_font_bold_34);

namespace esp_brookesia::apps {

namespace {

constexpr char TAG[] = "JarvisUi";

constexpr int SCREEN = 466;
constexpr int CANVAS_W = 230;
constexpr int CANVAS_H = 222;
constexpr int CANVAS_Y = 86;          // top of the canvas on screen
constexpr int FRAME_MS = 66;          // ~15 fps
constexpr int SPARKLES = 26;

constexpr uint32_t COLOR_IDLE = 0x2BC4D8;
constexpr uint32_t COLOR_CONNECT = 0x4F7DFF;
constexpr uint32_t COLOR_LISTEN = 0x31E1FF;
constexpr uint32_t COLOR_THINK = 0xFFB347;
constexpr uint32_t COLOR_SPEAK = 0x3DF5B5;
constexpr uint32_t COLOR_ERROR = 0xFF5050;

constexpr uint8_t KIND_SKIN = 0;   // outer shell
constexpr uint8_t KIND_EYE = 1;    // inner core

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

// Builds an abstract cloud of points: a shell of evenly spread points
// (Fibonacci sphere) with a little random depth, plus a sparse inner core.
// Model units: radius about 1.
void XiaozhiUi::buildModel()
{
    constexpr int SHELL = 760;
    constexpr int CORE = 240;
    constexpr int MAX_POINTS = SHELL + CORE;
    _points = static_cast<Point *>(
                  heap_caps_malloc(sizeof(Point) * MAX_POINTS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!_points) {
        _points = static_cast<Point *>(heap_caps_malloc(sizeof(Point) * MAX_POINTS, MALLOC_CAP_8BIT));
    }
    if (!_points) {
        ESP_LOGE(TAG, "No memory for the point cloud");
        return;
    }
    uint32_t seed = 0x9E3779B9u;
    auto rnd = [&seed]() {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return (seed & 0xFFFF) / 65535.0f;
    };
    uint16_t n = 0;
    const float golden = (float)M_PI * (3.0f - sqrtf(5.0f));
    for (int i = 0; i < SHELL; i++) {
        float y = 1.0f - 2.0f * (i + 0.5f) / SHELL;
        float r = sqrtf(1.0f - y * y);
        float th = golden * i;
        float depth = 0.90f + 0.14f * rnd();
        _points[n++] = {(int16_t)(r * cosf(th) * depth * 1000), (int16_t)(y * depth * 1000),
                        (int16_t)(r * sinf(th) * depth * 1000), KIND_SKIN};
    }
    for (int i = 0; i < CORE; i++) {
        // random direction, radius biased towards the centre
        float u = rnd() * 2.0f - 1.0f;
        float ph = rnd() * 2.0f * (float)M_PI;
        float r = sqrtf(1.0f - u * u);
        float rad = 0.15f + 0.6f * rnd() * rnd();
        _points[n++] = {(int16_t)(r * cosf(ph) * rad * 1000), (int16_t)(u * rad * 1000),
                        (int16_t)(r * sinf(ph) * rad * 1000), KIND_EYE};
    }
    _point_count = n;
    ESP_LOGI(TAG, "Point cloud: %u points", (unsigned)n);
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
    {
        std::string name = jarvis::load().name;
        for (char &c : name) {
            if (c >= 'a' && c <= 'z') {
                c = static_cast<char>(c - 'a' + 'A');
            }
        }
        lv_label_set_text(_name_label, name.c_str());
    }
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

    // Cloud motion per state
    float spin = 0.35f;       // rotation speed (rad/s)
    float breathe = 0.035f;   // slow size change
    float ripple = 0.0f;      // surface waves (listening)
    float twist = 0.0f;       // swirl (thinking)
    float pulse = 0.0f;       // beat (speaking)
    float size = 1.0f;
    switch (_mode) {
    case Mode::Listening:
        spin = 0.5f;
        ripple = 0.10f;
        break;
    case Mode::Thinking:
        spin = 1.6f;
        twist = 1.1f * sinf(t * 0.9f);
        breathe = 0.02f;
        break;
    case Mode::Speaking:
        spin = 0.6f;
        pulse = fabsf(sinf(t * 9.0f) * 0.6f + sinf(t * 14.0f) * 0.4f);
        break;
    case Mode::Error:
        spin = 0.12f;
        size = 0.82f;
        break;
    case Mode::Connecting:
        spin = 0.9f;
        breathe = 0.06f;
        break;
    default:
        break;
    }
    // a gentle fade-in of the new style after a state change
    const float settle = tm < 0.6f ? tm / 0.6f : 1.0f;
    const float base = size * (1.0f + breathe * sinf(t * 1.3f)) * (1.0f + 0.14f * pulse * settle);
    const float tilt = 0.35f;
    const float ct = cosf(tilt), st = sinf(tilt);

    const uint32_t stride = _canvas_stride_px;
    memset(_canvas_buf, 0, (size_t)stride * 2 * CANVAS_H);

    const float half_w = CANVAS_W * 0.5f;
    const float half_h = CANVAS_H * 0.5f;
    const float scale = 84.0f;
    for (uint16_t i = 0; i < _point_count; i++) {
        const Point &p = _points[i];
        float x = p.x * 0.001f, y = p.y * 0.001f, z = p.z * 0.001f;
        const bool core = p.kind == KIND_EYE;

        float r = base;
        if (ripple > 0.0f && !core) {
            r *= 1.0f + ripple * settle * sinf(y * 7.0f + t * 6.0f) * cosf(x * 5.0f - t * 3.0f);
        }
        if (pulse > 0.0f && core) {
            r *= 1.0f + 0.5f * pulse;  // the core flashes outwards when speaking
        }
        x *= r;
        y *= r;
        z *= r;

        // spin around Y (twisted by height when thinking), then tilt around X
        float a = t * spin + twist * y * settle;
        float ca = cosf(a), sa = sinf(a);
        float x1 = x * ca + z * sa;
        float z1 = -x * sa + z * ca;
        float ry = y * ct - z1 * st;
        float rz = y * st + z1 * ct;
        float rx = x1;

        float persp = 1.0f + rz * 0.18f;
        int px = (int)(half_w + rx * scale * persp);
        int py = (int)(half_h + ry * scale * persp);
        if (px < 1 || px >= CANVAS_W - 1 || py < 1 || py >= CANVAS_H - 1) {
            continue;
        }
        // front points brighter than back points
        float d = (rz + 1.0f) / 2.0f;
        if (d < 0) d = 0;
        if (d > 1) d = 1;
        int k;
        uint32_t c = color;
        bool big;
        if (core) {
            c = eye_color;
            k = 70 + (int)(110 * d) + (int)(90 * pulse);
            big = pulse > 0.5f;
        } else {
            k = 35 + (int)(190 * d);
            big = rz > 0.35f;
        }
        uint16_t *row = _canvas_buf + (size_t)py * stride;
        addPixel(&row[px], c, k);
        if (big) {
            addPixel(&row[px + 1], c, k * 2 / 3);
            addPixel(&row[px + stride], c, k * 2 / 3);
            addPixel(&row[px + stride + 1], c, k / 2);
        }
    }

    // Soft glow in the centre
    {
        const int gr = (int)(26 + 18 * pulse);
        for (int dy = -gr; dy <= gr; dy++) {
            int yy = (int)half_h + dy;
            if (yy < 0 || yy >= CANVAS_H) {
                continue;
            }
            uint16_t *row = _canvas_buf + (size_t)yy * stride;
            for (int dx = -gr; dx <= gr; dx++) {
                int xx = (int)half_w + dx;
                int d2 = dx * dx + dy * dy;
                if (xx < 0 || xx >= CANVAS_W || d2 > gr * gr) {
                    continue;
                }
                addPixel(&row[xx], color, (int)(55 * (1.0f - (float)d2 / (gr * gr))));
            }
        }
    }

    // Floating sparkles
    for (int s = 0; s < SPARKLES; s++) {
        uint32_t h = (uint32_t)s * 2654435761u;
        float ang = (h & 0xFFFF) / 65535.0f * 6.2832f + t * (0.05f + (h >> 28) * 0.01f);
        float rad = 1.18f + ((h >> 16) & 0xFF) / 255.0f * 0.12f;
        int px = (int)(half_w + cosf(ang) * rad * scale);
        int py = (int)(half_h + sinf(ang) * rad * scale * 0.9f);
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
