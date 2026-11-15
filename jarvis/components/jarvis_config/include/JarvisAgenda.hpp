/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Jarvis agenda: Google Calendar (read-only, private iCal address) plus the
 * board's own events (added on the board's web page). Everything is local;
 * the iCal address is stored in NVS like the Open WebUI key.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace jarvis::agenda {

struct Event {
    int32_t date = 0;     // YYYYMMDD, local time
    int16_t start = -1;   // minutes after midnight, -1 = all day
    int16_t end = -1;     // minutes after midnight, -1 = unknown
    bool google = false;  // true = from Google Calendar
    std::string title;
    std::string place;
};

// Starts the background Google Calendar sync (once). Safe to call many times.
void begin();
// Asks the sync task to read the calendar again now.
void requestSync();

// Today's date as YYYYMMDD, or 0 while the clock is not set yet.
int32_t today();
// Current time in minutes after midnight (0 if the clock is not set).
int nowMinutes();

std::vector<Event> eventsOn(int32_t date);
std::vector<Event> upcoming(int days);        // from today, sorted
uint32_t monthMask(int year, int month);      // bit (day - 1) set = has events

std::vector<Event> localEvents();             // the board's own, stored order
bool addLocal(const Event &event);
bool removeLocal(size_t index);

bool hasGoogle();                             // an iCal address is configured
std::string status();                         // short Catalan status line
uint32_t revision();                          // changes whenever data changes

// Today's date/time and the next days' events, for the assistant's prompt.
std::string assistantContext();

// Date helpers (proleptic Gregorian, dates as YYYYMMDD).
int32_t makeDate(int year, int month, int day);
int32_t addDays(int32_t date, int days);
int weekday(int32_t date);                    // 0 = Monday ... 6 = Sunday
int daysInMonth(int year, int month);
const char *monthName(int month);             // 1..12, Catalan
const char *weekdayName(int wd);              // 0 = Monday, Catalan
std::string formatTime(int minutes);          // "09:05"

} // namespace jarvis::agenda
