/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Small streaming iCalendar (.ics) reader for the Google Calendar private
 * address. Keeps only the events inside [window_start, window_end] and
 * expands the common repeat rules (daily, weekly, monthly, yearly with
 * INTERVAL, COUNT, UNTIL, BYDAY, BYMONTHDAY, EXDATE and moved occurrences).
 * No ESP-IDF dependency, so it can be tested on a PC.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "JarvisAgenda.hpp"

namespace jarvis::agenda {

// Calendar date helpers shared with the agenda.
int64_t daysFromCivil(int y, int m, int d);
void civilFromDays(int64_t z, int &y, int &m, int &d);
int64_t dateToDays(int32_t date);
int32_t daysToDate(int64_t days);

class IcsParser {
public:
    IcsParser(int32_t window_start, int32_t window_end, size_t max_events);

    void feed(const char *data, size_t len);
    // Finishes parsing and returns the events, sorted by date and time.
    std::vector<Event> finish();

    size_t eventCount() const { return _events.size(); }

private:
    struct Rule {
        int freq = 0;  // 0 none, 1 daily, 2 weekly, 3 monthly, 4 yearly
        int interval = 1;
        int count = 0;
        int32_t until = 0;
        std::vector<std::pair<int, int>> byday;  // (nth or 0, weekday 0=Mon)
        int bymonthday = 0;
    };
    struct Pending {
        bool active = false;
        bool cancelled = false;
        std::string uid;
        std::string summary;
        std::string location;
        int32_t start_date = 0;
        int16_t start_min = -1;
        int32_t end_date = 0;
        int16_t end_min = -1;
        bool all_day = false;
        int32_t recurrence_id = 0;
        Rule rule;
        std::vector<int32_t> exdates;
    };

    void line(const std::string &l);
    void finishEvent();
    void addOccurrence(const Pending &p, int32_t date);
    void expand(const Pending &p);

    int32_t _start;
    int32_t _end;
    size_t _max;
    std::string _partial;   // bytes after the last newline
    std::string _logical;   // current unfolded line
    bool _have_logical = false;
    Pending _cur;
    std::vector<Pending> _masters;                      // repeating events
    std::set<std::pair<std::string, int32_t>> _moved;   // (uid, original date)
    std::vector<Event> _events;
};

} // namespace jarvis::agenda
