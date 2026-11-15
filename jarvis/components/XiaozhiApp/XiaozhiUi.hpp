/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 * SPDX-FileContributor: Jarvis (round holographic UI)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

namespace esp_brookesia::apps {

// Same public API as Waveshare's original XiaozhiUi, with a different look:
// name + battery at the top, an animated holographic head in the middle,
// the state label and subtitle below and a coloured ring around the edge.
class XiaozhiUi {
public:
    using ActionCallback = void (*)(void *context);

    XiaozhiUi() = default;
    ~XiaozhiUi();

    XiaozhiUi(const XiaozhiUi &) = delete;
    XiaozhiUi &operator=(const XiaozhiUi &) = delete;

    bool preload();
    bool create(
        lv_obj_t *parent,
        ActionCallback action_callback,
        void *action_context,
        bool multiline_subtitle = false
    );
    void destroy();

    void setNetworkReady(bool ready);
    void setStatus(const char *status);
    void setChatMessage(const char *role, const char *text);
    void clearChatMessages();
    void setEmotion(const char *emotion);
    void setActivation(const char *code, const char *message, bool visible);
    // Jarvis: called when the on-screen exit button is tapped.
    void setExitCallback(ActionCallback callback, void *context);

    lv_obj_t *root() const
    {
        return _root;
    }

    enum class Mode : uint8_t { Idle, Connecting, Listening, Thinking, Speaking, Error };

private:
    struct Point {
        int16_t x, y, z;   // model space, 1/1000 units
        uint8_t kind;      // 0 skin, 1 eye, 2 mouth, 3 pedestal
    };

    void destroyView();
    void buildModel();
    void renderFrame();
    void updateRing(uint32_t now_ms);
    void updateBattery();
    void applyMode(Mode mode);
    static void animTimerCb(lv_timer_t *timer);
    static void rootClickedCb(lv_event_t *event);
    static void exitClickedCb(lv_event_t *event);

    lv_obj_t *_root = nullptr;
    lv_obj_t *_ring = nullptr;
    lv_obj_t *_battery_label = nullptr;
    lv_obj_t *_name_label = nullptr;
    lv_obj_t *_canvas = nullptr;
    lv_obj_t *_state_label = nullptr;
    lv_obj_t *_subtitle_label = nullptr;
    lv_obj_t *_dots = nullptr;
    lv_obj_t *_activation_box = nullptr;
    lv_obj_t *_activation_code_label = nullptr;
    lv_obj_t *_activation_message_label = nullptr;
    lv_timer_t *_anim_timer = nullptr;
    ActionCallback _action_cb = nullptr;
    void *_action_ctx = nullptr;
    ActionCallback _exit_cb = nullptr;
    void *_exit_ctx = nullptr;

    uint16_t *_canvas_buf = nullptr;
    uint32_t _canvas_stride_px = 0;

    Point *_points = nullptr;
    uint16_t _point_count = 0;

    Mode _mode = Mode::Connecting;
    bool _network_ready = false;
    bool _thinking = false;
    bool _activation_visible = false;
    bool _activation_state_valid = false;
    uint32_t _mode_since_ms = 0;
    uint32_t _last_battery_ms = 0;
    int _battery_percent = -2;
    uint32_t _rng = 0x1234567;
};

} // namespace esp_brookesia::apps
