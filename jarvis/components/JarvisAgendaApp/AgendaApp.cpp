/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "AgendaApp.hpp"

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "JarvisAgenda.hpp"
#include "JarvisConfig.hpp"
#include "jarvis_fonts.h"

LV_IMG_DECLARE(img_app_jarvis_agenda);

namespace esp_brookesia::apps {

namespace ag = jarvis::agenda;

namespace {

constexpr int SCREEN = 466;
constexpr int CELL_W = 42;
constexpr int CELL_H = 33;
constexpr int GRID_Y = 122;
constexpr int GRID_X0 = (SCREEN - 7 * CELL_W) / 2;

constexpr uint32_t COLOR_BG = 0x000000;
constexpr uint32_t COLOR_TEXT = 0xF2F7FA;
constexpr uint32_t COLOR_DIM = 0x6E8792;
constexpr uint32_t COLOR_CYAN = 0x31E1FF;
constexpr uint32_t COLOR_AMBER = 0xFFB347;

lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
    lv_label_set_text(l, text);
    return l;
}

std::string capitalize(const char *s)
{
    std::string out = s ? s : "";
    if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') {
        out[0] = static_cast<char>(out[0] - 'a' + 'A');
    }
    return out;
}

int32_t g_cell_dates[42] = {};

} // namespace

AgendaApp *AgendaApp::_instance = nullptr;

AgendaApp *AgendaApp::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (!_instance) {
        _instance = new AgendaApp(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

AgendaApp::AgendaApp(bool use_status_bar, bool use_navigation_bar)
    : App("Agenda", &img_app_jarvis_agenda, true, use_status_bar, use_navigation_bar)
{
}

AgendaApp::~AgendaApp()
{
    destroyUi();
}

bool AgendaApp::init()
{
    // Start reading Google Calendar at boot (so Jarvis knows the agenda even
    // if this app is never opened) and make the board's web page available.
    ag::begin();
    jarvis::startWebServer();
    return true;
}

bool AgendaApp::deinit()
{
    return close();
}

bool AgendaApp::pause()
{
    if (_timer) {
        lv_timer_pause(_timer);
    }
    return true;
}

bool AgendaApp::resume()
{
    if (_timer) {
        lv_timer_resume(_timer);
    }
    return true;
}

bool AgendaApp::back()
{
    return notifyCoreClosed();
}

bool AgendaApp::close()
{
    destroyUi();
    return true;
}

void AgendaApp::destroyUi()
{
    if (_timer) {
        lv_timer_delete(_timer);
        _timer = nullptr;
    }
    // The widgets belong to the app screen, which the core cleans up.
    _root = nullptr;
    _clock_label = nullptr;
    _month_label = nullptr;
    _day_label = nullptr;
    _list = nullptr;
    memset(_cells, 0, sizeof(_cells));
    memset(_cell_labels, 0, sizeof(_cell_labels));
    memset(_cell_dots, 0, sizeof(_cell_dots));
}

bool AgendaApp::run()
{
    destroyUi();
    ag::begin();

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    _root = plain(screen);
    lv_obj_set_size(_root, SCREEN, SCREEN);
    lv_obj_center(_root);

    // Thin ring, like Jarvis
    lv_obj_t *ring = lv_arc_create(_root);
    lv_obj_set_size(ring, SCREEN - 10, SCREEN - 10);
    lv_obj_center(ring);
    lv_obj_remove_style(ring, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_bg_angles(ring, 0, 360);
    lv_arc_set_angles(ring, 0, 0);
    lv_obj_set_style_arc_width(ring, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ring, lv_color_hex(COLOR_AMBER), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(ring, LV_OPA_40, LV_PART_MAIN);

    _clock_label = label(_root, &jv_font_bold_20, COLOR_CYAN, "--:--");
    lv_obj_align(_clock_label, LV_ALIGN_TOP_MID, 0, 22);

    // Month header: < Novembre 2026 >
    lv_obj_t *prev = plain(_root);
    lv_obj_set_size(prev, 48, 44);
    lv_obj_align(prev, LV_ALIGN_TOP_MID, -136, 46);
    lv_obj_add_flag(prev, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(prev, prevCb, LV_EVENT_CLICKED, this);
    lv_obj_center(label(prev, &jv_font_bold_34, COLOR_CYAN, "<"));

    lv_obj_t *next = plain(_root);
    lv_obj_set_size(next, 48, 44);
    lv_obj_align(next, LV_ALIGN_TOP_MID, 136, 46);
    lv_obj_add_flag(next, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(next, nextCb, LV_EVENT_CLICKED, this);
    lv_obj_center(label(next, &jv_font_bold_34, COLOR_CYAN, ">"));

    _month_label = label(_root, &jv_font_24, COLOR_TEXT, "");
    lv_obj_align(_month_label, LV_ALIGN_TOP_MID, 0, 55);
    lv_obj_add_flag(_month_label, LV_OBJ_FLAG_CLICKABLE);  // tap = back to today
    lv_obj_set_ext_click_area(_month_label, 10);
    lv_obj_add_event_cb(_month_label, todayCb, LV_EVENT_CLICKED, this);

    static const char *WD[] = {"dl", "dt", "dc", "dj", "dv", "ds", "dg"};
    for (int i = 0; i < 7; i++) {
        lv_obj_t *l = label(_root, &jv_font_18, i >= 5 ? COLOR_AMBER : COLOR_DIM, WD[i]);
        lv_obj_set_width(l, CELL_W);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_pos(l, GRID_X0 + i * CELL_W, GRID_Y - 26);
    }

    for (int i = 0; i < CELLS; i++) {
        lv_obj_t *c = plain(_root);
        lv_obj_set_size(c, CELL_W - 4, CELL_H - 3);
        lv_obj_set_pos(c, GRID_X0 + (i % 7) * CELL_W + 2, GRID_Y + (i / 7) * CELL_H);
        lv_obj_set_style_radius(c, 15, LV_PART_MAIN);
        lv_obj_set_style_border_color(c, lv_color_hex(COLOR_CYAN), LV_PART_MAIN);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, cellCb, LV_EVENT_CLICKED, this);
        _cells[i] = c;

        _cell_labels[i] = label(c, &jv_font_18, COLOR_TEXT, "");
        lv_obj_align(_cell_labels[i], LV_ALIGN_CENTER, 0, -2);

        lv_obj_t *dot = plain(c);
        lv_obj_set_size(dot, 5, 5);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, lv_color_hex(COLOR_AMBER), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, 0, -1);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        _cell_dots[i] = dot;
    }

    _day_label = label(_root, &jv_font_bold_20, COLOR_AMBER, "");
    lv_obj_align(_day_label, LV_ALIGN_TOP_MID, 0, GRID_Y + 6 * CELL_H + 6);

    _list = plain(_root);
    lv_obj_set_size(_list, 256, 76);
    lv_obj_align(_list, LV_ALIGN_TOP_MID, 0, GRID_Y + 6 * CELL_H + 34);
    lv_obj_add_flag(_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(_list, 4, LV_PART_MAIN);

    _today = ag::today();
    int32_t start = _today ? _today : 20260101;
    _year = start / 10000;
    _month = (start / 100) % 100;
    _selected = _today;
    _seen_revision = ag::revision();
    refreshClock();
    refreshMonth();
    refreshList();

    _timer = lv_timer_create(timerCb, 1000, this);
    return _timer != nullptr;
}

void AgendaApp::refreshClock()
{
    if (!_clock_label) {
        return;
    }
    if (!_today) {
        lv_label_set_text(_clock_label, "Esperant l'hora (cal WiFi)");
        return;
    }
    char b[64];
    snprintf(b, sizeof(b), "%s %d  ·  %s", ag::weekdayName(ag::weekday(_today)),
             static_cast<int>(_today % 100), ag::formatTime(ag::nowMinutes()).c_str());
    lv_label_set_text(_clock_label, b);
}

void AgendaApp::refreshMonth()
{
    if (!_month_label) {
        return;
    }
    std::string title = capitalize(ag::monthName(_month)) + " " + std::to_string(_year);
    lv_label_set_text(_month_label, title.c_str());

    const int first_wd = ag::weekday(ag::makeDate(_year, _month, 1));
    const int len = ag::daysInMonth(_year, _month);
    const uint32_t mask = ag::monthMask(_year, _month);
    const int rows = (first_wd + len + 6) / 7;

    for (int i = 0; i < CELLS; i++) {
        lv_obj_t *c = _cells[i];
        const int day = i - first_wd + 1;
        if (day < 1 || day > len) {
            g_cell_dates[i] = 0;
            lv_label_set_text(_cell_labels[i], "");
            lv_obj_add_flag(_cell_dots[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_border_width(c, 0, LV_PART_MAIN);
            if (i / 7 >= rows) {
                lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(c, LV_OBJ_FLAG_HIDDEN);
            }
            continue;
        }
        lv_obj_clear_flag(c, LV_OBJ_FLAG_HIDDEN);
        const int32_t date = ag::makeDate(_year, _month, day);
        g_cell_dates[i] = date;
        lv_label_set_text_fmt(_cell_labels[i], "%d", day);

        const bool selected = date == _selected;
        const bool is_today = date == _today;
        const bool weekend = (i % 7) >= 5;
        lv_obj_set_style_bg_color(c, lv_color_hex(COLOR_CYAN), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(c, selected ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(c, is_today && !selected ? 2 : 0, LV_PART_MAIN);
        lv_obj_set_style_text_color(
            _cell_labels[i],
            lv_color_hex(selected ? 0x00222A : (weekend ? COLOR_AMBER : COLOR_TEXT)),
            LV_PART_MAIN);
        if (mask & (1u << (day - 1))) {
            lv_obj_clear_flag(_cell_dots[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(_cell_dots[i], lv_color_hex(selected ? 0x00222A : COLOR_AMBER),
                                      LV_PART_MAIN);
        } else {
            lv_obj_add_flag(_cell_dots[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void AgendaApp::refreshList()
{
    if (!_list || !_day_label) {
        return;
    }
    lv_obj_clean(_list);

    if (!_selected) {
        lv_label_set_text(_day_label, "");
        lv_obj_t *l = label(_list, &jv_font_18, COLOR_DIM, "La placa encara no té l'hora. Connecta la WiFi.");
        lv_obj_set_width(l, 256);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        return;
    }

    std::string head;
    if (_selected == _today) {
        head = "Avui";
    } else if (_today && _selected == ag::addDays(_today, 1)) {
        head = "Demà";
    } else {
        head = capitalize(ag::weekdayName(ag::weekday(_selected)));
    }
    head += " " + std::to_string(_selected % 100) + " de " + ag::monthName((_selected / 100) % 100);
    lv_label_set_text(_day_label, head.c_str());

    std::vector<ag::Event> ev = ag::eventsOn(_selected);
    if (ev.empty()) {
        std::string msg = "Res a l'agenda.";
        if (!ag::hasGoogle()) {
            msg += " Afegeix el calendari de Google a la pàgina web de la placa.";
        }
        lv_obj_t *l = label(_list, &jv_font_18, COLOR_DIM, msg.c_str());
        lv_obj_set_width(l, 256);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        return;
    }
    for (const ag::Event &e : ev) {
        lv_obj_t *row = plain(_list);
        lv_obj_set_size(row, 256, LV_SIZE_CONTENT);
        lv_obj_t *t = label(row, &jv_font_18, COLOR_CYAN,
                            e.start >= 0 ? ag::formatTime(e.start).c_str() : "tot dia");
        lv_obj_set_pos(t, 0, 0);
        std::string text = e.title;
        if (!e.place.empty()) {
            text += " · " + e.place;
        }
        lv_obj_t *l = label(row, &jv_font_18, COLOR_TEXT, text.c_str());
        lv_obj_set_width(l, 190);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_pos(l, 66, 0);
    }
}

void AgendaApp::changeMonth(int delta)
{
    _month += delta;
    while (_month > 12) {
        _month -= 12;
        _year++;
    }
    while (_month < 1) {
        _month += 12;
        _year--;
    }
    refreshMonth();
}

void AgendaApp::select(int32_t date)
{
    if (!date) {
        return;
    }
    _selected = date;
    refreshMonth();
    refreshList();
}

void AgendaApp::timerCb(lv_timer_t *timer)
{
    auto *self = static_cast<AgendaApp *>(lv_timer_get_user_data(timer));
    if (!self || !self->_root) {
        return;
    }
    const int32_t today = ag::today();
    const int minute = ag::nowMinutes();
    if (minute != self->_seen_minute) {
        self->_seen_minute = minute;
        if (today != self->_today) {
            const bool first_time = self->_today == 0;
            if (first_time || self->_selected == self->_today) {
                self->_selected = today;  // follow the day change
            }
            self->_today = today;
            if (first_time && today) {
                self->_year = today / 10000;
                self->_month = (today / 100) % 100;
            }
            self->refreshMonth();
            self->refreshList();
        }
        self->refreshClock();
    }
    const uint32_t rev = ag::revision();
    if (rev != self->_seen_revision) {
        self->_seen_revision = rev;
        self->refreshMonth();
        self->refreshList();
    }
}

void AgendaApp::prevCb(lv_event_t *event)
{
    auto *self = static_cast<AgendaApp *>(lv_event_get_user_data(event));
    if (self) {
        self->changeMonth(-1);
    }
}

void AgendaApp::nextCb(lv_event_t *event)
{
    auto *self = static_cast<AgendaApp *>(lv_event_get_user_data(event));
    if (self) {
        self->changeMonth(1);
    }
}

void AgendaApp::todayCb(lv_event_t *event)
{
    auto *self = static_cast<AgendaApp *>(lv_event_get_user_data(event));
    if (self && self->_today) {
        self->_year = self->_today / 10000;
        self->_month = (self->_today / 100) % 100;
        self->select(self->_today);
    }
}

void AgendaApp::cellCb(lv_event_t *event)
{
    auto *self = static_cast<AgendaApp *>(lv_event_get_user_data(event));
    auto *target = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
    if (!self) {
        return;
    }
    for (int i = 0; i < CELLS; i++) {
        if (self->_cells[i] == target) {
            self->select(g_cell_dates[i]);
            return;
        }
    }
}

} // namespace esp_brookesia::apps
