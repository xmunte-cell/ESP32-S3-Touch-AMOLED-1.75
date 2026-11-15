/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Jarvis "Agenda" app: month calendar and the selected day's events
 * (Google Calendar + the board's own events), in Catalan.
 */
#pragma once

#include <stdint.h>

#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class AgendaApp : public systems::phone::App {
public:
    static AgendaApp *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);
    ~AgendaApp();

protected:
    AgendaApp(bool use_status_bar, bool use_navigation_bar);

    bool run() override;
    bool back() override;
    bool close() override;
    bool init() override;
    bool deinit() override;
    bool pause() override;
    bool resume() override;

private:
    static constexpr int CELLS = 42;

    static AgendaApp *_instance;

    lv_obj_t *_root = nullptr;
    lv_obj_t *_clock_label = nullptr;
    lv_obj_t *_month_label = nullptr;
    lv_obj_t *_cells[CELLS] = {};
    lv_obj_t *_cell_labels[CELLS] = {};
    lv_obj_t *_cell_dots[CELLS] = {};
    lv_obj_t *_day_label = nullptr;
    lv_obj_t *_list = nullptr;
    lv_timer_t *_timer = nullptr;

    int _year = 0;
    int _month = 0;
    int32_t _selected = 0;
    int32_t _today = 0;
    uint32_t _seen_revision = 0;
    int _seen_minute = -1;

    void destroyUi();
    void refreshMonth();
    void refreshList();
    void refreshClock();
    void changeMonth(int delta);
    void select(int32_t date);

    static void timerCb(lv_timer_t *timer);
    static void prevCb(lv_event_t *event);
    static void nextCb(lv_event_t *event);
    static void cellCb(lv_event_t *event);
    static void todayCb(lv_event_t *event);
    static void exitCb(lv_event_t *event);
};

} // namespace esp_brookesia::apps
