/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "JarvisIcs.hpp"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>

namespace jarvis::agenda {

// Howard Hinnant's civil date algorithms.
int64_t daysFromCivil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civilFromDays(int64_t z, int &y, int &m, int &d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(yoe + era * 400 + (m <= 2));
}

int64_t dateToDays(int32_t date)
{
    return daysFromCivil(date / 10000, (date / 100) % 100, date % 100);
}

int32_t daysToDate(int64_t days)
{
    int y, m, d;
    civilFromDays(days, y, m, d);
    return y * 10000 + m * 100 + d;
}

namespace {

int weekdayOfDays(int64_t days)
{
    // 1970-01-01 was a Thursday (3 with Monday = 0).
    int64_t w = (days + 3) % 7;
    return static_cast<int>(w < 0 ? w + 7 : w);
}

int monthLength(int y, int m)
{
    static const int len[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) {
        return 29;
    }
    return len[(m - 1) % 12];
}

int weekdayCode(const char *s)
{
    static const char *codes[] = {"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
    for (int i = 0; i < 7; i++) {
        if (s[0] == codes[i][0] && s[1] == codes[i][1]) {
            return i;
        }
    }
    return -1;
}

std::string unescape(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '\\' && i + 1 < in.size()) {
            char n = in[++i];
            out += (n == 'n' || n == 'N') ? ' ' : n;
        } else {
            out += in[i];
        }
    }
    return out;
}

// Parses "20261115", "20261115T090000" or "20261115T090000Z".
// UTC times are converted to the board's local time zone (TZ).
bool parseDateTime(const std::string &value, int32_t &date, int16_t &minutes, bool &all_day)
{
    if (value.size() < 8) {
        return false;
    }
    for (int i = 0; i < 8; i++) {
        if (value[i] < '0' || value[i] > '9') {
            return false;
        }
    }
    int y = atoi(value.substr(0, 4).c_str());
    int mo = atoi(value.substr(4, 2).c_str());
    int d = atoi(value.substr(6, 2).c_str());
    if (mo < 1 || mo > 12 || d < 1 || d > 31) {
        return false;
    }
    if (value.size() < 15 || value[8] != 'T') {
        date = y * 10000 + mo * 100 + d;
        minutes = -1;
        all_day = true;
        return true;
    }
    int h = atoi(value.substr(9, 2).c_str());
    int mi = atoi(value.substr(11, 2).c_str());
    all_day = false;
    if (value.size() >= 16 && value[15] == 'Z') {
        time_t t = static_cast<time_t>(daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60);
        struct tm lt = {};
        localtime_r(&t, &lt);
        date = (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday;
        minutes = static_cast<int16_t>(lt.tm_hour * 60 + lt.tm_min);
        return true;
    }
    // Local time (TZID=...): Google uses the calendar's own zone, which for
    // this board is the same as the device zone.
    date = y * 10000 + mo * 100 + d;
    minutes = static_cast<int16_t>(h * 60 + mi);
    return true;
}

} // namespace

IcsParser::IcsParser(int32_t window_start, int32_t window_end, size_t max_events)
    : _start(window_start), _end(window_end), _max(max_events)
{
}

void IcsParser::feed(const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\r') {
            continue;
        }
        if (c != '\n') {
            if (_partial.size() < 4096) {
                _partial += c;
            }
            continue;
        }
        // One physical line is complete.
        if (!_partial.empty() && (_partial[0] == ' ' || _partial[0] == '\t')) {
            if (_have_logical && _logical.size() < 4096) {
                _logical.append(_partial, 1, std::string::npos);
            }
        } else {
            if (_have_logical) {
                line(_logical);
            }
            _logical.swap(_partial);
            _have_logical = true;
        }
        _partial.clear();
    }
}

void IcsParser::line(const std::string &l)
{
    // NAME;PARAM=...;PARAM="...":VALUE
    size_t colon = std::string::npos;
    bool quoted = false;
    for (size_t i = 0; i < l.size(); i++) {
        if (l[i] == '"') {
            quoted = !quoted;
        } else if (l[i] == ':' && !quoted) {
            colon = i;
            break;
        }
    }
    if (colon == std::string::npos) {
        return;
    }
    std::string head = l.substr(0, colon);
    std::string value = l.substr(colon + 1);
    std::string name = head.substr(0, head.find(';'));

    if (name == "BEGIN" && value == "VEVENT") {
        _cur = Pending();
        _cur.active = true;
        return;
    }
    if (!_cur.active) {
        return;
    }
    if (name == "END" && value == "VEVENT") {
        finishEvent();
        _cur.active = false;
        return;
    }
    if (name == "BEGIN" || name == "END") {
        return;  // VALARM etc.
    }
    if (name == "UID") {
        _cur.uid = value;
    } else if (name == "SUMMARY") {
        _cur.summary = unescape(value);
    } else if (name == "LOCATION") {
        _cur.location = unescape(value);
    } else if (name == "STATUS") {
        _cur.cancelled = value == "CANCELLED";
    } else if (name == "DTSTART") {
        parseDateTime(value, _cur.start_date, _cur.start_min, _cur.all_day);
    } else if (name == "DTEND") {
        bool ad = false;
        parseDateTime(value, _cur.end_date, _cur.end_min, ad);
    } else if (name == "RECURRENCE-ID") {
        int16_t mi = -1;
        bool ad = false;
        parseDateTime(value, _cur.recurrence_id, mi, ad);
    } else if (name == "EXDATE") {
        size_t pos = 0;
        while (pos <= value.size()) {
            size_t comma = value.find(',', pos);
            if (comma == std::string::npos) {
                comma = value.size();
            }
            int32_t d = 0;
            int16_t mi = -1;
            bool ad = false;
            if (parseDateTime(value.substr(pos, comma - pos), d, mi, ad)) {
                _cur.exdates.push_back(d);
            }
            pos = comma + 1;
        }
    } else if (name == "RRULE") {
        Rule r;
        size_t pos = 0;
        while (pos < value.size()) {
            size_t semi = value.find(';', pos);
            if (semi == std::string::npos) {
                semi = value.size();
            }
            std::string part = value.substr(pos, semi - pos);
            size_t eq = part.find('=');
            if (eq != std::string::npos) {
                std::string k = part.substr(0, eq);
                std::string v = part.substr(eq + 1);
                if (k == "FREQ") {
                    r.freq = v == "DAILY" ? 1 : v == "WEEKLY" ? 2 : v == "MONTHLY" ? 3 : v == "YEARLY" ? 4 : -1;
                } else if (k == "INTERVAL") {
                    r.interval = std::max(1, atoi(v.c_str()));
                } else if (k == "COUNT") {
                    r.count = atoi(v.c_str());
                } else if (k == "UNTIL") {
                    int16_t mi = -1;
                    bool ad = false;
                    parseDateTime(v, r.until, mi, ad);
                } else if (k == "BYMONTHDAY") {
                    r.bymonthday = atoi(v.c_str());
                } else if (k == "BYDAY") {
                    size_t p = 0;
                    while (p < v.size()) {
                        size_t c = v.find(',', p);
                        if (c == std::string::npos) {
                            c = v.size();
                        }
                        std::string item = v.substr(p, c - p);
                        if (item.size() >= 2) {
                            int wd = weekdayCode(item.c_str() + item.size() - 2);
                            int nth = item.size() > 2 ? atoi(item.substr(0, item.size() - 2).c_str()) : 0;
                            if (wd >= 0) {
                                r.byday.emplace_back(nth, wd);
                            }
                        }
                        p = c + 1;
                    }
                }
            }
            pos = semi + 1;
        }
        _cur.rule = r;
    }
}

void IcsParser::addOccurrence(const Pending &p, int32_t date)
{
    if (_events.size() >= _max) {
        return;
    }
    // Several-day all-day events show on each day (up to two weeks).
    int span = 1;
    if (p.all_day && p.end_date > p.start_date) {
        span = static_cast<int>(dateToDays(p.end_date) - dateToDays(p.start_date));
        span = std::min(std::max(span, 1), 14);
    }
    int64_t base = dateToDays(date);
    for (int i = 0; i < span; i++) {
        int32_t d = daysToDate(base + i);
        if (d < _start || d > _end || _events.size() >= _max) {
            continue;
        }
        Event e;
        e.date = d;
        e.start = p.all_day ? -1 : p.start_min;
        e.end = p.all_day ? -1 : p.end_min;
        e.google = true;
        e.title = p.summary.empty() ? std::string("(sense títol)") : p.summary;
        e.place = p.location;
        _events.push_back(std::move(e));
    }
}

void IcsParser::finishEvent()
{
    if (_cur.cancelled || _cur.start_date == 0) {
        if (_cur.recurrence_id && !_cur.uid.empty()) {
            _moved.emplace(_cur.uid, _cur.recurrence_id);  // cancelled occurrence
        }
        return;
    }
    if (_cur.recurrence_id) {
        // A single occurrence of a repeating event that was moved/edited.
        _moved.emplace(_cur.uid, _cur.recurrence_id);
        addOccurrence(_cur, _cur.start_date);
        return;
    }
    if (_cur.rule.freq > 0) {
        if (_masters.size() < 400) {
            _masters.push_back(_cur);
        }
        return;
    }
    if (_cur.rule.freq < 0) {
        return;  // unsupported repeat rule
    }
    addOccurrence(_cur, _cur.start_date);
}

void IcsParser::expand(const Pending &p)
{
    const Rule &r = p.rule;
    const int64_t first = dateToDays(p.start_date);
    const int64_t win_end = dateToDays(_end);
    int emitted = 0;
    int guard = 0;
    auto emit = [&](int64_t day) -> bool {
        // Returns false when the rule has ended.
        if (day < first) {
            return true;
        }
        int32_t date = daysToDate(day);
        if (r.until && date > r.until) {
            return false;
        }
        if (r.count && emitted >= r.count) {
            return false;
        }
        if (day > win_end) {
            return false;
        }
        emitted++;
        if (std::find(p.exdates.begin(), p.exdates.end(), date) != p.exdates.end()) {
            return true;
        }
        if (_moved.count({p.uid, date})) {
            return true;
        }
        addOccurrence(p, date);
        return true;
    };

    int sy, sm, sd;
    civilFromDays(first, sy, sm, sd);

    // Without COUNT, old repeating events can skip straight to the window.
    const int64_t win_start = dateToDays(_start);
    if (r.freq == 1) {
        int64_t day = first;
        if (!r.count && day < win_start) {
            day += (win_start - day) / r.interval * r.interval;
        }
        for (; guard++ < 4000; day += r.interval) {
            if (!emit(day)) {
                break;
            }
        }
    } else if (r.freq == 2) {
        std::vector<int> days;
        for (auto &b : r.byday) {
            days.push_back(b.second);
        }
        if (days.empty()) {
            days.push_back(weekdayOfDays(first));
        }
        std::sort(days.begin(), days.end());
        days.erase(std::unique(days.begin(), days.end()), days.end());
        int64_t week = first - weekdayOfDays(first);
        if (!r.count && week + 7 < win_start) {
            const int64_t step = 7 * static_cast<int64_t>(r.interval);
            week += (win_start - 7 - week) / step * step;
        }
        bool go = true;
        while (go && guard++ < 2000) {
            for (int wd : days) {
                if (!emit(week + wd)) {
                    go = false;
                    break;
                }
            }
            week += 7 * r.interval;
        }
    } else if (r.freq == 3 || r.freq == 4) {
        int y = sy, m = sm;
        bool go = true;
        while (go && guard++ < 2400) {
            std::vector<int> mdays;
            if (r.freq == 4 && m != sm) {
                // yearly: only the start month
            } else if (!r.byday.empty() && r.byday[0].first != 0) {
                for (auto &b : r.byday) {
                    int nth = b.first;
                    int wd = b.second;
                    int len = monthLength(y, m);
                    if (nth > 0) {
                        int firstwd = weekdayOfDays(daysFromCivil(y, m, 1));
                        int day = 1 + (wd - firstwd + 7) % 7 + (nth - 1) * 7;
                        if (day <= len) {
                            mdays.push_back(day);
                        }
                    } else {
                        int lastwd = weekdayOfDays(daysFromCivil(y, m, len));
                        int day = len - (lastwd - wd + 7) % 7 + (nth + 1) * 7;
                        if (day >= 1) {
                            mdays.push_back(day);
                        }
                    }
                }
            } else {
                int day = r.bymonthday > 0 ? r.bymonthday : sd;
                if (day <= monthLength(y, m)) {
                    mdays.push_back(day);
                }
            }
            std::sort(mdays.begin(), mdays.end());
            for (int day : mdays) {
                if (!emit(daysFromCivil(y, m, day))) {
                    go = false;
                    break;
                }
            }
            if (daysFromCivil(y, m, 1) > win_end) {
                break;
            }
            int step = r.freq == 3 ? r.interval : 12 * r.interval;
            m += step;
            while (m > 12) {
                m -= 12;
                y++;
            }
        }
    }
}

std::vector<Event> IcsParser::finish()
{
    if (!_partial.empty()) {
        const char nl = '\n';
        feed(&nl, 1);
    }
    if (_have_logical) {
        line(_logical);
        _logical.clear();
        _have_logical = false;
    }
    for (const Pending &p : _masters) {
        expand(p);
    }
    std::sort(_events.begin(), _events.end(), [](const Event &a, const Event &b) {
        if (a.date != b.date) {
            return a.date < b.date;
        }
        return a.start < b.start;
    });
    return std::move(_events);
}

} // namespace jarvis::agenda
