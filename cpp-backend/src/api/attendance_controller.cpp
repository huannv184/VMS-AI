// ==============================================================
// File: src/api/attendance_controller.cpp
// /api/attendance — daily summary, manual punch, employee +
// camera-role CRUD, CSV export, health stats.
//
// Reads/writes the attendance_events / employees / camera_roles
// tables introduced alongside AttendanceTracker (src/core).
//
// Backward compat:
//   GET /api/attendance — when attendance_events has no rows for the
//   requested day, falls back to grouping FACE_RECOGNIZED events the
//   way the legacy event_controller did, so historical data still
//   surfaces in the UI during migration.
// ==============================================================

#include "api/attendance_controller.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>

#include <nlohmann/json.hpp>

#include "core/attendance_tracker.h"
#include "database/db_manager.h"
#include "middleware/auth_middleware.h"
#include "utils/api_utils.h"
#include "utils/config.h"
#include "utils/logger.h"

#ifdef DELETE
#undef DELETE
#endif

using json = nlohmann::json;

namespace vms::api {

namespace {

// ── RBAC ─────────────────────────────────────────────────────────────────
// Attendance mutations (manual punch, employee CRUD, camera role upsert)
// are admin-only — same lesson as BUG-23 (event_engine_controller).
// Reads (rollups, employee roster, camera-roles, health) require any
// authenticated user with ANALYTICS_READ — admin/operator/viewer all qualify,
// but anonymous network callers are rejected. Without this gate, the entire
// employee directory + per-day check-in/check-out ledger is leaked to
// anyone reachable on the API port.
// AuthConfig.enabled=false (dev mode) bypasses for local development.
std::optional<crow::response> requireAttendanceAdmin(
        vms::server::VmsApp& app,
        const crow::request& req,
        const std::string& origin) {
    if (!vms::Config::getInstance().getAuthConfig().enabled) {
        return std::nullopt;
    }
    auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
    return ApiUtils::requireAdmin(ctx, origin);
}

std::optional<crow::response> requireAttendanceRead(
        vms::server::VmsApp& app,
        const crow::request& req,
        const std::string& origin) {
    if (!vms::Config::getInstance().getAuthConfig().enabled) {
        return std::nullopt;
    }
    auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
    return ApiUtils::requirePermission(ctx, Permission::ANALYTICS_READ, origin);
}

// ── Helpers ──────────────────────────────────────────────────────────────

std::string todayLocalDate() {
    auto t = std::time(nullptr);
    auto tm = *std::localtime(&t);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return std::string(buf);
}

bool isValidIsoDate(const std::string& s) {
    if (s.size() != 10) return false;
    if (s[4] != '-' || s[7] != '-') return false;
    for (size_t i : {0,1,2,3,5,6,8,9}) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

bool isValidKind(const std::string& kind) {
    return kind == "in" || kind == "out" || kind == "seen";
}

bool isValidRole(const std::string& role) {
    return role == "entry" || role == "exit" || role == "both" || role == "observe";
}

// "HH:MM" with HH in [00,23], MM in [00,59]. Empty / wrong-length / non-digit
// rejected.  We do NOT accept "H:MM" or "HH:M" — strict format keeps the DB
// representation canonical so string comparisons (and frontend display) work
// without parsing.
bool isValidHmTime(const std::string& s) {
    if (s.size() != 5) return false;
    if (s[2] != ':') return false;
    for (size_t i : {0,1,3,4}) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    int h = (s[0]-'0')*10 + (s[1]-'0');
    int m = (s[3]-'0')*10 + (s[4]-'0');
    return h >= 0 && h <= 23 && m >= 0 && m <= 59;
}

// "HH:MM" → minutes since midnight. Caller MUST have validated via
// isValidHmTime first; behaviour on malformed input is undefined.
int hmToMinutes(const std::string& s) {
    return ((s[0]-'0')*10 + (s[1]-'0')) * 60 + ((s[3]-'0')*10 + (s[4]-'0'));
}

// Returns the local-midnight epoch second of the *shift_date* a given punch
// belongs to.  Definitions:
//   • A "shift instance" is a single occurrence of a shift, stamped with the
//     calendar day it BEGAN on (shift_date).
//   • For day-shifts (start <= end) the shift_date is just the calendar
//     date of the punch — no ambiguity.
//   • For overnight shifts (e.g. 22:00→06:00, start > end) a punch made at
//     06:05 belongs to the previous day's instance; a punch made at 22:00
//     belongs to that day's instance.  We split on the midpoint of the
//     OFF-duty window: anything before that midpoint is "morning side"
//     (yesterday's instance), anything ≥ that midpoint is "evening side"
//     (today's instance).  Off-duty for 22:00→06:00 is [06:00, 22:00], so
//     midpoint = 14:00.  This is robust against late check-outs (e.g. 09:00
//     out for a 22:00 in still buckets to yesterday) and early check-ins
//     (21:00 in for 22:00 shift buckets to today).
//
// has_shift=false bypasses the night-shift logic and just returns calendar
// midnight — keeps employees-without-a-shift on their per-calendar-day row.
std::time_t shiftDateMidnight(std::time_t ts, bool has_shift,
                              int start_min, int end_min) {
    std::tm tm = *std::localtime(&ts);
    const int hour_min = tm.tm_hour * 60 + tm.tm_min;

    const bool overnight = has_shift && (start_min > end_min) &&
                           start_min >= 0 && end_min >= 0;
    if (overnight) {
        // Off-duty window is [end_min, start_min].  Midpoint of that window
        // is the cleanest split between "yesterday's instance" and
        // "today's instance".
        const int mid_off = end_min + (start_min - end_min) / 2;
        if (hour_min < mid_off) {
            tm.tm_mday -= 1;
        }
    }
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

// Late minutes for a check-in punch, given the shift_date it belongs to and
// the shift's start minute-of-day.  A punch on the *next* calendar day
// after shift_date (i.e. an overnight worker who only managed to punch when
// the morning bell went off) is correctly accounted for via epoch-delta:
// (ci_ts - shift_date_midnight) / 60 yields minutes since shift_date 00:00,
// which can exceed 1440 for next-day punches.  Subtracting start_min + grace
// gives the lateness; clamps to 0 for on-time/early arrivals.
int lateMinutesForPunch(std::time_t ci_ts, std::time_t shift_date_midnight,
                        int start_min, int grace_min) {
    const long long minutes_since =
        (static_cast<long long>(ci_ts) -
         static_cast<long long>(shift_date_midnight)) / 60;
    const long long delta = minutes_since - start_min - grace_min;
    if (delta <= 0) return 0;
    return static_cast<int>(delta);
}

// Overtime minutes counted from the LAST punch of the day relative to the
// shift's end time.  Mirrors lateMinutesForPunch but on the trailing side.
//
// Overnight handling: when start_min > end_min (e.g. 22:00→06:00), the
// shift's end happens on the day AFTER shift_date, so we offset by +1440
// before subtracting from `co_ts - shift_date_midnight`.  Result clamped
// to [0, ot_max_minutes]; values below ot_min_minutes return 0 to filter
// out a few-minute trailing presence that isn't real OT.
//
// co_ts <= 0 (no punch) → 0.  start_min/end_min must be valid HH:MM
// minutes; caller validates via isValidHmTime.
int overtimeMinutesForPunch(std::time_t co_ts,
                            std::time_t shift_date_midnight,
                            int start_min, int end_min,
                            int ot_grace_min,
                            int ot_min_minutes,
                            int ot_max_minutes) {
    if (co_ts <= 0) return 0;
    const int shift_end_min_abs =
        (start_min > end_min) ? (end_min + 1440) : end_min;
    const long long minutes_since =
        (static_cast<long long>(co_ts) -
         static_cast<long long>(shift_date_midnight)) / 60;
    const long long delta = minutes_since - shift_end_min_abs - ot_grace_min;
    if (delta <= 0) return 0;
    if (delta < ot_min_minutes) return 0;
    if (ot_max_minutes > 0 && delta > ot_max_minutes) return ot_max_minutes;
    return static_cast<int>(delta);
}

// Build the per-person aggregation for a given local date.
// Returns rows shaped to match the existing AttendanceView UI:
//   { person_name, employee_code, dept, check_in, check_out, count, date }
// Timestamps are emitted as epoch ms so JS `new Date(...)` works directly.
json queryAttendanceForDate(QSqlDatabase& db, const std::string& date_str) {
    json out = json::array();
    const bool is_pg = vms::Config::getInstance().getDatabaseConfig().driver == "postgresql";

    // Holiday lookup. When the requested date is in the holidays table we
    // still compute the per-employee aggregation (operators want to see who
    // came in on a holiday), but we mark the rollup row `is_holiday=true` and
    // skip late_minutes / OT computation. Coming in on a holiday is not "late"
    // — it's a separate accounting concept (operators might pay holiday rate
    // or compensate-with-day-off, but that's an HRIS concern, not a tardiness
    // metric). Punted from the 2026-05-03 shifts pass; landed 2026-05-12.
    std::string holiday_name;
    bool is_holiday = false;
    {
        QSqlQuery hq(db);
        hq.prepare("SELECT name FROM holidays WHERE date = ?");
        hq.bindValue(0, QString::fromStdString(date_str));
        if (hq.exec() && hq.next()) {
            is_holiday = true;
            holiday_name = hq.value(0).toString().toStdString();
        }
    }

    // Day boundaries in local time → epoch seconds.
    // We compute the bounds in C++ (rather than relying on DB timezone) so
    // SQLite and Postgres behave identically.
    std::tm tm = {};
    {
        int y, m, d;
        if (std::sscanf(date_str.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return out;
        tm.tm_year = y - 1900;
        tm.tm_mon  = m - 1;
        tm.tm_mday = d;
        tm.tm_hour = 0;
        tm.tm_min  = 0;
        tm.tm_sec  = 0;
        tm.tm_isdst = -1;
    }
    const std::time_t day_start = std::mktime(&tm);
    // Wider window than [day_start, day_start+24h) to capture overnight
    // shifts.  A 22:00→06:00 shift instance dated 2026-05-04 emits an OUT
    // punch around 2026-05-05 06:00 — that punch must aggregate under
    // 2026-05-04's row, so we have to look ahead.  Symmetrically, the
    // 2026-05-03 instance's OUT punch at 2026-05-04 06:00 lands in our
    // window but its shift_date is 2026-05-03 and we filter it out below.
    // 12h before / 36h after is enough margin for any HH:MM shift.
    const std::time_t window_start = day_start - 12LL * 3600;
    const std::time_t window_end   = day_start + 36LL * 3600;

    // Pull raw rows (no GROUP BY) — aggregation moves to C++ so we can
    // bucket by shift_date (calendar day the shift INSTANCE belongs to)
    // rather than calendar day the punch happened to fall on.  This is the
    // correctness fix for night shifts; for day shifts shift_date == punch
    // calendar date so behaviour is unchanged.  Employees with no shift
    // assignment, or unrecognised people (employee_id NULL), still bucket
    // by calendar date and get NULL shift fields + late_minutes=null in the
    // response.
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT a.person_id, a.employee_id, a.timestamp, "
        "       e.code, e.full_name, e.dept, "
        "       s.id, s.name, s.start_time_hm, s.end_time_hm, "
        "       s.late_threshold_min, s.grace_min, "
        "       s.ot_grace_min, s.ot_min_minutes, s.ot_max_minutes "
        "FROM attendance_events a "
        "LEFT JOIN employees e ON e.id = a.employee_id "
        "LEFT JOIN shifts s ON s.id = e.shift_id "
        "WHERE a.timestamp >= ? AND a.timestamp < ? "
        "ORDER BY a.timestamp"));
    q.bindValue(0, static_cast<qlonglong>(window_start));
    q.bindValue(1, static_cast<qlonglong>(window_end));

    if (!q.exec()) {
        LOG_WARN("AttendanceController: query attendance_events failed: {}",
                 q.lastError().text().toStdString());
        return out;
    }

    struct Agg {
        qlonglong min_ts = std::numeric_limits<qlonglong>::max();
        qlonglong max_ts = std::numeric_limits<qlonglong>::lowest();
        int count = 0;
        int person_id = -1;
        int employee_id = -1;
        std::string code;
        std::string name;
        std::string dept;
        bool has_shift = false;
        int shift_id = -1;
        std::string shift_name;
        std::string shift_start_hm;
        std::string shift_end_hm;
        int shift_late_thr = 15;
        int shift_grace = 0;
        int ot_grace = 0;
        int ot_min  = 0;
        int ot_max  = 720;
        std::time_t shift_date_midnight = 0;
    };
    // Key on (person_id, employee_id, shift_date_midnight).  Matching the
    // legacy SQL GROUP-BY which keyed on (person_id, employee_id) so a
    // person whose face was sometimes attached to an employee and sometimes
    // not still produces the same row split as before.
    std::map<std::tuple<int,int,std::time_t>, Agg> by_key;

    while (q.next()) {
        const int person_id   = q.value(0).toInt();
        const int employee_id = q.value(1).isNull() ? -1 : q.value(1).toInt();
        const qlonglong ts    = q.value(2).toLongLong();

        const std::string code = q.value(3).isNull() ? std::string{} : q.value(3).toString().toStdString();
        const std::string nm   = q.value(4).isNull() ? std::string{} : q.value(4).toString().toStdString();
        const std::string dept = q.value(5).isNull() ? std::string{} : q.value(5).toString().toStdString();

        const bool has_shift          = !q.value(6).isNull();
        const int shift_id_v          = has_shift ? q.value(6).toInt() : -1;
        const std::string shift_name  = has_shift ? q.value(7).toString().toStdString() : std::string{};
        const std::string shift_start = has_shift ? q.value(8).toString().toStdString() : std::string{};
        const std::string shift_end   = has_shift ? q.value(9).toString().toStdString() : std::string{};
        const int shift_late_thr      = has_shift ? q.value(10).toInt() : 15;
        const int shift_grace         = has_shift ? q.value(11).toInt() : 0;
        const int ot_grace_v          = has_shift ? q.value(12).toInt() : 0;
        const int ot_min_v            = has_shift ? q.value(13).toInt() : 0;
        const int ot_max_v            = has_shift ? q.value(14).toInt() : 720;

        const bool valid_hm = has_shift &&
                              isValidHmTime(shift_start) &&
                              isValidHmTime(shift_end);
        const int start_min_v = valid_hm ? hmToMinutes(shift_start) : -1;
        const int end_min_v   = valid_hm ? hmToMinutes(shift_end)   : -1;

        // Compute shift_date for THIS punch and discard rows whose instance
        // doesn't belong to the requested calendar day.  This filter is what
        // keeps the wider SQL window from polluting the requested day's
        // rollup with adjacent-day instances.
        const std::time_t shift_date = shiftDateMidnight(
            static_cast<std::time_t>(ts), valid_hm, start_min_v, end_min_v);
        if (shift_date != day_start) continue;

        auto key = std::make_tuple(person_id, employee_id, shift_date);
        auto& a = by_key[key];
        a.count += 1;
        a.min_ts = std::min<qlonglong>(a.min_ts, ts);
        a.max_ts = std::max<qlonglong>(a.max_ts, ts);
        a.person_id   = person_id;
        a.employee_id = employee_id;
        // Identity columns are constant per (person_id, employee_id) by
        // SQL-join construction; first-seen capture is sufficient.
        if (a.code.empty() && !code.empty()) a.code = code;
        if (a.name.empty() && !nm.empty())   a.name = nm;
        if (a.dept.empty() && !dept.empty()) a.dept = dept;
        a.has_shift           = has_shift;
        a.shift_id            = shift_id_v;
        a.shift_name          = shift_name;
        a.shift_start_hm      = shift_start;
        a.shift_end_hm        = shift_end;
        a.shift_late_thr      = shift_late_thr;
        a.shift_grace         = shift_grace;
        a.ot_grace            = ot_grace_v;
        a.ot_min              = ot_min_v;
        a.ot_max              = ot_max_v;
        a.shift_date_midnight = shift_date;
    }

    for (auto& [key, a] : by_key) {
        const qlonglong ci_s = a.min_ts;
        const qlonglong co_s = a.max_ts;

        std::string display_name = a.name;
        if (display_name.empty() && a.person_id > 0) {
            display_name = "Person #" + std::to_string(a.person_id);
        }
        if (display_name.empty()) display_name = "Người Lạ";

        // Late computation: relative to shift_date midnight, not calendar
        // midnight.  An overnight worker who only punches at 06:05 the next
        // morning correctly registers as "very late" (~485 min) rather than
        // "on time" (the old bug: 06:05's calendar minute-of-day 365 was
        // less than 22:00's 1320 → delta negative → on time).
        json late_minutes_v = nullptr;
        json is_late_v = nullptr;
        json is_late_severe_v = nullptr;
        json overtime_minutes_v = nullptr;
        json has_overtime_v = nullptr;
        // Holiday short-circuit: leave late/OT fields null + is_late=false so
        // the UI badge logic treats this row as "On holiday" rather than the
        // ambiguous "Unscheduled" bucket (a person without an assigned shift
        // also has null late_minutes — without is_holiday we couldn't
        // distinguish the two cases).
        if (is_holiday) {
            is_late_v = false;
            is_late_severe_v = false;
        } else {
            if (a.has_shift && ci_s > 0 && isValidHmTime(a.shift_start_hm)) {
                const int shift_min = hmToMinutes(a.shift_start_hm);
                const int late = lateMinutesForPunch(
                    static_cast<std::time_t>(ci_s),
                    a.shift_date_midnight, shift_min, a.shift_grace);
                late_minutes_v   = late;
                is_late_v        = late > 0;
                is_late_severe_v = late >= a.shift_late_thr;
            }
            // OT: requires usable check-out punch + valid shift end_time_hm.
            // Worker who only punched once (max_ts==min_ts) yields delta<0 and
            // overtimeMinutesForPunch returns 0 — no false OT.
            if (a.has_shift && co_s > 0 &&
                isValidHmTime(a.shift_start_hm) && isValidHmTime(a.shift_end_hm)) {
                const int sm = hmToMinutes(a.shift_start_hm);
                const int em = hmToMinutes(a.shift_end_hm);
                const int ot = overtimeMinutesForPunch(
                    static_cast<std::time_t>(co_s),
                    a.shift_date_midnight, sm, em,
                    a.ot_grace, a.ot_min, a.ot_max);
                overtime_minutes_v = ot;
                has_overtime_v     = ot > 0;
            }
        }

        out.push_back({
            {"person_id",         a.person_id},
            {"employee_id",       a.employee_id},
            {"employee_code",     a.code},
            {"person_name",       display_name},
            {"dept",              a.dept},
            {"check_in",          static_cast<int64_t>(ci_s) * 1000},
            {"check_out",         static_cast<int64_t>(co_s) * 1000},
            {"count",             a.count},
            {"date",              date_str},
            {"source",            "attendance_events"},
            {"shift_id",          a.shift_id},
            {"shift_name",        a.shift_name},
            {"shift_start",       a.shift_start_hm},
            {"shift_end",         a.shift_end_hm},
            {"late_threshold_min", a.shift_late_thr},
            {"grace_min",         a.shift_grace},
            {"ot_grace_min",      a.ot_grace},
            {"ot_min_minutes",    a.ot_min},
            {"ot_max_minutes",    a.ot_max},
            {"late_minutes",      late_minutes_v},
            {"is_late",           is_late_v},
            {"is_late_severe",    is_late_severe_v},
            {"overtime_minutes",  overtime_minutes_v},
            {"has_overtime",      has_overtime_v},
            {"is_holiday",        is_holiday},
            {"holiday_name",      holiday_name}
        });
    }

    if (!out.empty()) return out;

    // Fallback: legacy FACE_RECOGNIZED events grouped by name. Same shape as
    // the prior /api/attendance route so existing rows still surface during
    // migration. Only fires when attendance_events has no data for the day.
    std::string fallback_sql;
    if (is_pg) {
        fallback_sql =
            "SELECT metadata_json->>'name' AS person_name, "
            "       MIN(timestamp) AS ci, MAX(timestamp) AS co, COUNT(*) AS cnt "
            "FROM events "
            "WHERE (event_type = 'FACE_RECOGNIZED' OR event_type = 'face' "
            "       OR event_type = 'face_recognition') "
            "  AND metadata_json->>'name' IS NOT NULL "
            "  AND metadata_json->>'name' NOT IN ('unknown', 'Unknown') "
            "  AND timestamp >= ? AND timestamp < ? "
            "GROUP BY person_name";
    } else {
        fallback_sql =
            "SELECT json_extract(metadata_json, '$.name') AS person_name, "
            "       MIN(timestamp) AS ci, MAX(timestamp) AS co, COUNT(*) AS cnt "
            "FROM events "
            "WHERE (event_type = 'FACE_RECOGNIZED' OR event_type = 'face' "
            "       OR event_type = 'face_recognition') "
            "  AND json_extract(metadata_json, '$.name') IS NOT NULL "
            "  AND json_extract(metadata_json, '$.name') NOT IN ('unknown', 'Unknown') "
            "  AND timestamp >= ? AND timestamp < ? "
            "GROUP BY person_name";
    }

    QSqlQuery fb(db);
    fb.prepare(QString::fromStdString(fallback_sql));
    // Legacy fallback has no shift concept — calendar-day window is correct.
    const std::time_t day_end = day_start + 24LL * 3600;
    fb.bindValue(0, static_cast<qlonglong>(day_start));
    fb.bindValue(1, static_cast<qlonglong>(day_end));

    if (!fb.exec()) {
        LOG_WARN("AttendanceController: legacy fallback query failed: {}",
                 fb.lastError().text().toStdString());
        return out;
    }

    while (fb.next()) {
        const std::string name = fb.value(0).isNull() ? "Người Lạ"
                                                       : fb.value(0).toString().toStdString();
        out.push_back({
            {"person_id",     -1},
            {"employee_id",   -1},
            {"employee_code", ""},
            {"person_name",   name},
            {"dept",          ""},
            {"check_in",      fb.value(1).toLongLong() * 1000},
            {"check_out",     fb.value(2).toLongLong() * 1000},
            {"count",         fb.value(3).toInt()},
            {"date",          date_str},
            {"source",        "events_legacy"}
        });
    }
    return out;
}

std::string buildCsv(const json& rows) {
    std::stringstream ss;
    ss << "\xEF\xBB\xBF"; // UTF-8 BOM for Excel
    ss << "Mã NV,Họ tên,Phòng ban,Ca,Giờ vào ca,Check-in,Check-out,"
          "Trễ (phút),Trạng thái,OT (phút),Ngày lễ,Số lần chấm,Nguồn\n";
    for (const auto& r : rows) {
        const auto fmt_ts = [](int64_t ms) -> std::string {
            if (ms <= 0) return "";
            std::time_t t = static_cast<std::time_t>(ms / 1000);
            std::tm tm = *std::localtime(&t);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
            return std::string(buf);
        };

        // late_minutes is nullable (no shift assigned, no check-in, or holiday).
        // Render empty cell rather than "0" so Excel filters distinguish
        // "on time" vs "no shift configured" vs "holiday".
        std::string late_cell;
        std::string status_cell;
        const bool is_holiday = r.value("is_holiday", false);
        const auto& lm = r.contains("late_minutes") ? r["late_minutes"] : json(nullptr);
        if (is_holiday) {
            status_cell = "Ngày lễ";  // holiday short-circuit wins over all
        } else if (lm.is_number_integer()) {
            late_cell = std::to_string(lm.get<int>());
            const bool severe = r.value("is_late_severe", false);
            const bool late   = r.value("is_late", false);
            status_cell = severe ? "Trễ nặng" : (late ? "Trễ" : "Đúng giờ");
        } else {
            status_cell = "Chưa gán ca";
        }
        const std::string holiday_name = r.value("holiday_name", std::string{});

        // overtime_minutes mirrors late_minutes — nullable, empty cell keeps
        // Excel filters honest ("not configured" vs "0 OT").
        std::string ot_cell;
        const auto& om = r.contains("overtime_minutes") ? r["overtime_minutes"] : json(nullptr);
        if (om.is_number_integer()) ot_cell = std::to_string(om.get<int>());

        ss << "\"" << r.value("employee_code", "") << "\","
           << "\"" << r.value("person_name",   "") << "\","
           << "\"" << r.value("dept",          "") << "\","
           << "\"" << r.value("shift_name",    "") << "\","
           << "\"" << r.value("shift_start",   "") << "\","
           << "\"" << fmt_ts(r.value("check_in",  static_cast<int64_t>(0))) << "\","
           << "\"" << fmt_ts(r.value("check_out", static_cast<int64_t>(0))) << "\","
           << "\"" << late_cell   << "\","
           << "\"" << status_cell << "\","
           << "\"" << ot_cell     << "\","
           << "\"" << holiday_name << "\","
           << r.value("count", 0) << ","
           << "\"" << r.value("source", "") << "\"\n";
    }
    return ss.str();
}

} // namespace

void AttendanceController::registerRoutes(vms::server::VmsApp& app) {
    // ─────────────────────────────────────────────────────────────────────
    // GET /api/attendance?date=YYYY-MM-DD
    // List daily attendance rollup. Requires ANALYTICS_READ.
    // ─────────────────────────────────────────────────────────────────────
    CROW_ROUTE(app, "/api/attendance")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        const std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        if (auto err = requireAttendanceRead(app, req, origin)) return std::move(*err);
        try {
            std::string date_str = req.url_params.get("date") ? req.url_params.get("date") : "";
            if (date_str.empty()) date_str = todayLocalDate();
            if (!isValidIsoDate(date_str)) {
                return ApiUtils::createErrorResponse("date must be YYYY-MM-DD", 400, origin);
            }

            auto db = vms::database::DbManager::getInstance().getThreadConnection();
            if (!db.isValid() || !db.isOpen()) {
                return ApiUtils::createErrorResponse("Database unavailable", 503, origin);
            }

            json rows = queryAttendanceForDate(db, date_str);
            return ApiUtils::createResponse({{"attendance", rows}, {"date", date_str}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // ─────────────────────────────────────────────────────────────────────
    // GET /api/attendance/export?date=YYYY-MM-DD
    // CSV download for the same day rollup.
    // ─────────────────────────────────────────────────────────────────────
    CROW_ROUTE(app, "/api/attendance/export")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        const std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        if (auto err = requireAttendanceRead(app, req, origin)) return std::move(*err);
        try {
            std::string date_str = req.url_params.get("date") ? req.url_params.get("date") : todayLocalDate();
            if (!isValidIsoDate(date_str)) {
                return ApiUtils::createErrorResponse("date must be YYYY-MM-DD", 400, origin);
            }

            auto db = vms::database::DbManager::getInstance().getThreadConnection();
            if (!db.isValid() || !db.isOpen()) {
                return ApiUtils::createErrorResponse("Database unavailable", 503, origin);
            }

            json rows = queryAttendanceForDate(db, date_str);
            crow::response res(buildCsv(rows));
            res.add_header("Content-Type", "text/csv; charset=utf-8");
            res.add_header("Content-Disposition",
                           "attachment; filename=\"attendance_" + date_str + ".csv\"");
            res.add_header("Access-Control-Allow-Origin", origin.empty() ? "*" : origin);
            return res;
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // ─────────────────────────────────────────────────────────────────────
    // POST /api/attendance/manual  body:{employee_id,camera_id,kind,timestamp?}
    // Manual punch — bypasses dedup. Admin only.
    // ─────────────────────────────────────────────────────────────────────
    CROW_ROUTE(app, "/api/attendance/manual")
    .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        const std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        if (auto err = requireAttendanceAdmin(app, req, origin)) return std::move(*err);

        try {
            auto body = json::parse(req.body);
            const int employee_id = body.value("employee_id", -1);
            const int camera_id   = body.value("camera_id",   -1);
            const std::string kind = body.value("kind", std::string{});
            int64_t ts_s = body.value("timestamp",
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());

            if (employee_id <= 0) return ApiUtils::createErrorResponse("employee_id required", 400, origin);
            if (camera_id   <= 0) return ApiUtils::createErrorResponse("camera_id required",   400, origin);
            if (!isValidKind(kind)) return ApiUtils::createErrorResponse("kind must be in|out|seen", 400, origin);

            std::string err;
            if (!vms::core::AttendanceTracker::getInstance().recordManual(
                    employee_id, camera_id, kind, ts_s, &err)) {
                return ApiUtils::createErrorResponse(err.empty() ? "manual punch failed" : err, 400, origin);
            }
            return ApiUtils::createResponse({{"queued", true}, {"timestamp", ts_s}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 400, origin);
        }
    });

    // ─────────────────────────────────────────────────────────────────────
    // GET /api/attendance/employees
    // List active employees (any authenticated user).
    // ─────────────────────────────────────────────────────────────────────
    CROW_ROUTE(app, "/api/attendance/employees")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        const std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        // GET → ANALYTICS_READ; POST → admin (gated below before any write).
        if (req.method == crow::HTTPMethod::Get) {
            if (auto err = requireAttendanceRead(app, req, origin)) return std::move(*err);
        }

        try {
            auto db = vms::database::DbManager::getInstance().getThreadConnection();
            if (!db.isValid() || !db.isOpen()) {
                return ApiUtils::createErrorResponse("Database unavailable", 503, origin);
            }

            if (req.method == crow::HTTPMethod::Get) {
                QSqlQuery q(db);
                if (!q.exec("SELECT id, person_id, code, full_name, dept, shift_id, active "
                            "FROM employees ORDER BY id DESC")) {
                    return ApiUtils::createErrorResponse(q.lastError().text().toStdString(), 500, origin);
                }
                json arr = json::array();
                while (q.next()) {
                    arr.push_back({
                        {"id",        q.value(0).toInt()},
                        {"person_id", q.value(1).toInt()},
                        {"code",      q.value(2).toString().toStdString()},
                        {"full_name", q.value(3).toString().toStdString()},
                        {"dept",      q.value(4).toString().toStdString()},
                        {"shift_id",  q.value(5).isNull() ? -1 : q.value(5).toInt()},
                        {"active",    q.value(6).toInt() == 1}
                    });
                }
                return ApiUtils::createResponse({{"employees", arr}}, 200, origin);
            }

            // POST → create. Admin only.
            if (auto err = requireAttendanceAdmin(app, req, origin)) return std::move(*err);

            auto body = json::parse(req.body);
            const int person_id = body.value("person_id", -1);
            if (person_id <= 0) return ApiUtils::createErrorResponse("person_id required", 400, origin);

            QSqlQuery q(db);
            q.prepare("INSERT INTO employees (person_id, code, full_name, dept, shift_id, active) "
                      "VALUES (?, ?, ?, ?, ?, ?)");
            q.bindValue(0, person_id);
            q.bindValue(1, QString::fromStdString(body.value("code", "")));
            q.bindValue(2, QString::fromStdString(body.value("full_name", "")));
            q.bindValue(3, QString::fromStdString(body.value("dept", "")));
            const auto shift_v = body.value("shift_id", -1);
            q.bindValue(4, shift_v > 0 ? QVariant(shift_v) : QVariant(QVariant::Int));
            q.bindValue(5, body.value("active", true) ? 1 : 0);

            if (!q.exec()) {
                return ApiUtils::createErrorResponse(q.lastError().text().toStdString(), 500, origin);
            }

            // Refresh in-memory cache so the next face event resolves the new mapping.
            vms::core::AttendanceTracker::getInstance().reloadEmployees();

            return ApiUtils::createResponse(
                {{"id", q.lastInsertId().toInt()}, {"person_id", person_id}}, 201, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // PUT/DELETE /api/attendance/employees/<int>
    CROW_ROUTE(app, "/api/attendance/employees/<int>")
    .methods(crow::HTTPMethod::Put, crow::HTTPMethod::Delete, crow::HTTPMethod::Options)
    ([&app](const crow::request& req, int id) {
        const std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        if (auto err = requireAttendanceAdmin(app, req, origin)) return std::move(*err);

        try {
            auto db = vms::database::DbManager::getInstance().getThreadConnection();
            if (!db.isValid() || !db.isOpen()) {
                return ApiUtils::createErrorResponse("Database unavailable", 503, origin);
            }

            if (req.method == crow::HTTPMethod::Delete) {
                // Soft delete — keeps historical attendance_events.employee_id valid.
                QSqlQuery q(db);
                q.prepare("UPDATE employees SET active = 0 WHERE id = ?");
                q.bindValue(0, id);
                if (!q.exec()) {
                    return ApiUtils::createErrorResponse(q.lastError().text().toStdString(), 500, origin);
                }
                vms::core::AttendanceTracker::getInstance().reloadEmployees();
                return ApiUtils::createResponse({{"deleted", true}, {"id", id}}, 200, origin);
            }

            auto body = json::parse(req.body);
            QSqlQuery q(db);
            q.prepare("UPDATE employees SET "
                      " code = COALESCE(?, code), "
                      " full_name = COALESCE(?, full_name), "
                      " dept = COALESCE(?, dept), "
                      " shift_id = COALESCE(?, shift_id), "
                      " active = COALESCE(?, active) "
                      "WHERE id = ?");
            const auto bindStr = [&](int idx, const char* k) {
                if (body.contains(k) && body[k].is_string()) {
                    q.bindValue(idx, QString::fromStdString(body[k].get<std::string>()));
                } else {
                    q.bindValue(idx, QVariant(QString()));
                }
            };
            bindStr(0, "code");
            bindStr(1, "full_name");
            bindStr(2, "dept");
            if (body.contains("shift_id") && body["shift_id"].is_number_integer()) {
                q.bindValue(3, body["shift_id"].get<int>());
            } else {
                q.bindValue(3, QVariant(QVariant::Int));
            }
            if (body.contains("active") && body["active"].is_boolean()) {
                q.bindValue(4, body["active"].get<bool>() ? 1 : 0);
            } else {
                q.bindValue(4, QVariant(QVariant::Int));
            }
            q.bindValue(5, id);

            if (!q.exec()) {
                return ApiUtils::createErrorResponse(q.lastError().text().toStdString(), 500, origin);
            }
            vms::core::AttendanceTracker::getInstance().reloadEmployees();
            return ApiUtils::createResponse({{"updated", true}, {"id", id}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // ─────────────────────────────────────────────────────────────────────
    // GET/POST /api/attendance/camera-roles
    // POST is upsert (PRIMARY KEY camera_id). Admin only.
    // ─────────────────────────────────────────────────────────────────────
    CROW_ROUTE(app, "/api/attendance/camera-roles")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        const std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        if (req.method == crow::HTTPMethod::Get) {
            if (auto err = requireAttendanceRead(app, req, origin)) return std::move(*err);
        }

        try {
            auto db = vms::database::DbManager::getInstance().getThreadConnection();
            if (!db.isValid() || !db.isOpen()) {
                return ApiUtils::createErrorResponse("Database unavailable", 503, origin);
            }

            if (req.method == crow::HTTPMethod::Get) {
                QSqlQuery q(db);
                if (!q.exec("SELECT camera_id, role FROM camera_roles ORDER BY camera_id ASC")) {
                    return ApiUtils::createErrorResponse(q.lastError().text().toStdString(), 500, origin);
                }
                json arr = json::array();
                while (q.next()) {
                    arr.push_back({
                        {"camera_id", q.value(0).toInt()},
                        {"role",      q.value(1).toString().toStdString()}
                    });
                }
                return ApiUtils::createResponse({{"camera_roles", arr}}, 200, origin);
            }

            if (auto err = requireAttendanceAdmin(app, req, origin)) return std::move(*err);
            auto body = json::parse(req.body);
            const int camera_id = body.value("camera_id", -1);
            const std::string role = body.value("role", std::string{});
            if (camera_id <= 0)      return ApiUtils::createErrorResponse("camera_id required", 400, origin);
            if (!isValidRole(role))  return ApiUtils::createErrorResponse("role must be entry|exit|both|observe", 400, origin);

            const bool is_pg = vms::Config::getInstance().getDatabaseConfig().driver == "postgresql";
            QSqlQuery q(db);
            if (is_pg) {
                q.prepare("INSERT INTO camera_roles (camera_id, role) VALUES (?, ?) "
                          "ON CONFLICT (camera_id) DO UPDATE SET role = EXCLUDED.role");
            } else {
                q.prepare("INSERT OR REPLACE INTO camera_roles (camera_id, role) VALUES (?, ?)");
            }
            q.bindValue(0, camera_id);
            q.bindValue(1, QString::fromStdString(role));
            if (!q.exec()) {
                return ApiUtils::createErrorResponse(q.lastError().text().toStdString(), 500, origin);
            }
            vms::core::AttendanceTracker::getInstance().reloadCameraRoles();
            return ApiUtils::createResponse({{"camera_id", camera_id}, {"role", role}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // ─────────────────────────────────────────────────────────────────────
    // GET/POST /api/attendance/shifts
    //   GET  → list (default active only; ?include_inactive=1 returns all)
    //   POST → create. Admin only.
    // Validates HH:MM strictly, rejects negative thresholds, returns 409 on
    // duplicate name (UNIQUE constraint at DB layer).
    // ─────────────────────────────────────────────────────────────────────
    CROW_ROUTE(app, "/api/attendance/shifts")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        const std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        if (req.method == crow::HTTPMethod::Get) {
            if (auto err = requireAttendanceRead(app, req, origin)) return std::move(*err);
        }

        try {
            auto db = vms::database::DbManager::getInstance().getThreadConnection();
            if (!db.isValid() || !db.isOpen()) {
                return ApiUtils::createErrorResponse("Database unavailable", 503, origin);
            }

            if (req.method == crow::HTTPMethod::Get) {
                const bool include_inactive =
                    req.url_params.get("include_inactive") &&
                    std::string(req.url_params.get("include_inactive")) == "1";

                QSqlQuery q(db);
                if (include_inactive) {
                    q.prepare("SELECT id, name, start_time_hm, end_time_hm, "
                              "       late_threshold_min, grace_min, "
                              "       ot_grace_min, ot_min_minutes, ot_max_minutes, "
                              "       active "
                              "FROM shifts ORDER BY active DESC, name ASC");
                } else {
                    q.prepare("SELECT id, name, start_time_hm, end_time_hm, "
                              "       late_threshold_min, grace_min, "
                              "       ot_grace_min, ot_min_minutes, ot_max_minutes, "
                              "       active "
                              "FROM shifts WHERE active = 1 ORDER BY name ASC");
                }
                if (!q.exec()) {
                    return ApiUtils::createErrorResponse(q.lastError().text().toStdString(), 500, origin);
                }
                json arr = json::array();
                while (q.next()) {
                    arr.push_back({
                        {"id",                 q.value(0).toInt()},
                        {"name",               q.value(1).toString().toStdString()},
                        {"start_time_hm",      q.value(2).toString().toStdString()},
                        {"end_time_hm",        q.value(3).toString().toStdString()},
                        {"late_threshold_min", q.value(4).toInt()},
                        {"grace_min",          q.value(5).toInt()},
                        {"ot_grace_min",       q.value(6).toInt()},
                        {"ot_min_minutes",     q.value(7).toInt()},
                        {"ot_max_minutes",     q.value(8).toInt()},
                        {"active",             q.value(9).toInt() == 1}
                    });
                }
                return ApiUtils::createResponse({{"shifts", arr}}, 200, origin);
            }

            // POST → create. Admin only.
            if (auto err = requireAttendanceAdmin(app, req, origin)) return std::move(*err);

            auto body = json::parse(req.body);
            const std::string name  = body.value("name", std::string{});
            const std::string start = body.value("start_time_hm", std::string{});
            const std::string end   = body.value("end_time_hm",   std::string{});
            const int late_thr      = body.value("late_threshold_min", 15);
            const int grace         = body.value("grace_min", 0);
            const int ot_grace     = body.value("ot_grace_min", 0);
            const int ot_min       = body.value("ot_min_minutes", 0);
            const int ot_max       = body.value("ot_max_minutes", 720);
            const bool active       = body.value("active", true);

            if (name.empty() || name.size() > 100) {
                return ApiUtils::createErrorResponse("name required (1..100 chars)", 400, origin);
            }
            if (!isValidHmTime(start)) {
                return ApiUtils::createErrorResponse("start_time_hm must be HH:MM (24h)", 400, origin);
            }
            if (!isValidHmTime(end)) {
                return ApiUtils::createErrorResponse("end_time_hm must be HH:MM (24h)", 400, origin);
            }
            if (late_thr < 0 || late_thr > 720) {
                return ApiUtils::createErrorResponse("late_threshold_min must be 0..720", 400, origin);
            }
            if (grace < 0 || grace > 720) {
                return ApiUtils::createErrorResponse("grace_min must be 0..720", 400, origin);
            }
            // OT bounds: ot_grace 0..720 (12h is plenty).  ot_min/ot_max 0..1440
            // — a worker who legitimately stayed 24h on a single shift is the
            // outlier we WANT to clamp.  ot_max=0 is a sentinel "no cap".
            if (ot_grace < 0 || ot_grace > 720) {
                return ApiUtils::createErrorResponse("ot_grace_min must be 0..720", 400, origin);
            }
            if (ot_min < 0 || ot_min > 1440) {
                return ApiUtils::createErrorResponse("ot_min_minutes must be 0..1440", 400, origin);
            }
            if (ot_max < 0 || ot_max > 1440) {
                return ApiUtils::createErrorResponse("ot_max_minutes must be 0..1440", 400, origin);
            }
            if (ot_max > 0 && ot_min > ot_max) {
                return ApiUtils::createErrorResponse("ot_min_minutes cannot exceed ot_max_minutes", 400, origin);
            }

            QSqlQuery q(db);
            q.prepare("INSERT INTO shifts (name, start_time_hm, end_time_hm, "
                      "                    late_threshold_min, grace_min, "
                      "                    ot_grace_min, ot_min_minutes, ot_max_minutes, "
                      "                    active) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
            q.bindValue(0, QString::fromStdString(name));
            q.bindValue(1, QString::fromStdString(start));
            q.bindValue(2, QString::fromStdString(end));
            q.bindValue(3, late_thr);
            q.bindValue(4, grace);
            q.bindValue(5, ot_grace);
            q.bindValue(6, ot_min);
            q.bindValue(7, ot_max);
            q.bindValue(8, active ? 1 : 0);

            if (!q.exec()) {
                const std::string msg = q.lastError().text().toStdString();
                // Sniff for UNIQUE-constraint failure across SQLite + PG so the
                // UI can show a friendly 409 instead of a raw driver string.
                if (msg.find("UNIQUE") != std::string::npos ||
                    msg.find("unique") != std::string::npos ||
                    msg.find("duplicate key") != std::string::npos) {
                    return ApiUtils::createErrorResponse("shift name already exists", 409, origin);
                }
                return ApiUtils::createErrorResponse(msg, 500, origin);
            }

            const int new_id = q.lastInsertId().toInt();
            return ApiUtils::createResponse({{"id", new_id}, {"name", name}}, 201, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // GET/PUT/DELETE /api/attendance/shifts/<int>
    CROW_ROUTE(app, "/api/attendance/shifts/<int>")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Put,
             crow::HTTPMethod::Delete, crow::HTTPMethod::Options)
    ([&app](const crow::request& req, int id) {
        const std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        if (req.method == crow::HTTPMethod::Get) {
            if (auto err = requireAttendanceRead(app, req, origin)) return std::move(*err);
        } else {
            if (auto err = requireAttendanceAdmin(app, req, origin)) return std::move(*err);
        }

        try {
            auto db = vms::database::DbManager::getInstance().getThreadConnection();
            if (!db.isValid() || !db.isOpen()) {
                return ApiUtils::createErrorResponse("Database unavailable", 503, origin);
            }

            if (req.method == crow::HTTPMethod::Get) {
                QSqlQuery q(db);
                q.prepare("SELECT id, name, start_time_hm, end_time_hm, "
                          "       late_threshold_min, grace_min, "
                          "       ot_grace_min, ot_min_minutes, ot_max_minutes, "
                          "       active "
                          "FROM shifts WHERE id = ?");
                q.bindValue(0, id);
                if (!q.exec()) {
                    return ApiUtils::createErrorResponse(q.lastError().text().toStdString(), 500, origin);
                }
                if (!q.next()) {
                    return ApiUtils::createErrorResponse("shift not found", 404, origin);
                }
                return ApiUtils::createResponse({
                    {"id",                 q.value(0).toInt()},
                    {"name",               q.value(1).toString().toStdString()},
                    {"start_time_hm",      q.value(2).toString().toStdString()},
                    {"end_time_hm",        q.value(3).toString().toStdString()},
                    {"late_threshold_min", q.value(4).toInt()},
                    {"grace_min",          q.value(5).toInt()},
                    {"ot_grace_min",       q.value(6).toInt()},
                    {"ot_min_minutes",     q.value(7).toInt()},
                    {"ot_max_minutes",     q.value(8).toInt()},
                    {"active",             q.value(9).toInt() == 1}
                }, 200, origin);
            }

            if (req.method == crow::HTTPMethod::Delete) {
                // Soft delete to preserve FK in employees.shift_id and the
                // historical record in attendance_events. Admin can re-activate
                // via PUT {active: true}.
                QSqlQuery q(db);
                q.prepare("UPDATE shifts SET active = 0 WHERE id = ?");
                q.bindValue(0, id);
                if (!q.exec()) {
                    return ApiUtils::createErrorResponse(q.lastError().text().toStdString(), 500, origin);
                }
                if (q.numRowsAffected() == 0) {
                    return ApiUtils::createErrorResponse("shift not found", 404, origin);
                }
                return ApiUtils::createResponse({{"deleted", true}, {"id", id}}, 200, origin);
            }

            // PUT — partial update with COALESCE pattern matching employees PUT.
            auto body = json::parse(req.body);

            // Validate present fields up-front so a bad value at field 4 of 6
            // doesn't half-apply via Postgres' implicit-cast quirks.
            if (body.contains("name")) {
                if (!body["name"].is_string()) {
                    return ApiUtils::createErrorResponse("name must be string", 400, origin);
                }
                const std::string n = body["name"].get<std::string>();
                if (n.empty() || n.size() > 100) {
                    return ApiUtils::createErrorResponse("name length 1..100", 400, origin);
                }
            }
            if (body.contains("start_time_hm")) {
                if (!body["start_time_hm"].is_string() ||
                    !isValidHmTime(body["start_time_hm"].get<std::string>())) {
                    return ApiUtils::createErrorResponse("start_time_hm must be HH:MM", 400, origin);
                }
            }
            if (body.contains("end_time_hm")) {
                if (!body["end_time_hm"].is_string() ||
                    !isValidHmTime(body["end_time_hm"].get<std::string>())) {
                    return ApiUtils::createErrorResponse("end_time_hm must be HH:MM", 400, origin);
                }
            }
            if (body.contains("late_threshold_min")) {
                if (!body["late_threshold_min"].is_number_integer() ||
                    body["late_threshold_min"].get<int>() < 0 ||
                    body["late_threshold_min"].get<int>() > 720) {
                    return ApiUtils::createErrorResponse("late_threshold_min must be 0..720", 400, origin);
                }
            }
            if (body.contains("grace_min")) {
                if (!body["grace_min"].is_number_integer() ||
                    body["grace_min"].get<int>() < 0 ||
                    body["grace_min"].get<int>() > 720) {
                    return ApiUtils::createErrorResponse("grace_min must be 0..720", 400, origin);
                }
            }
            // OT field validation mirrors POST.  Cross-field check (ot_min ≤
            // ot_max) only fires when both are present in the body — a PUT
            // changing only ot_min could otherwise spuriously fail against
            // the existing-row ot_max we don't have in scope here.
            const auto checkBounded = [&](const char* k, int lo, int hi) -> std::optional<crow::response> {
                if (!body.contains(k)) return std::nullopt;
                if (!body[k].is_number_integer() ||
                    body[k].get<int>() < lo || body[k].get<int>() > hi) {
                    return ApiUtils::createErrorResponse(
                        std::string(k) + " must be " + std::to_string(lo) + ".." + std::to_string(hi),
                        400, origin);
                }
                return std::nullopt;
            };
            if (auto err = checkBounded("ot_grace_min",   0,  720)) return std::move(*err);
            if (auto err = checkBounded("ot_min_minutes", 0, 1440)) return std::move(*err);
            if (auto err = checkBounded("ot_max_minutes", 0, 1440)) return std::move(*err);
            if (body.contains("ot_min_minutes") && body.contains("ot_max_minutes")) {
                const int omin = body["ot_min_minutes"].get<int>();
                const int omax = body["ot_max_minutes"].get<int>();
                if (omax > 0 && omin > omax) {
                    return ApiUtils::createErrorResponse(
                        "ot_min_minutes cannot exceed ot_max_minutes", 400, origin);
                }
            }

            QSqlQuery q(db);
            q.prepare("UPDATE shifts SET "
                      " name = COALESCE(?, name), "
                      " start_time_hm = COALESCE(?, start_time_hm), "
                      " end_time_hm = COALESCE(?, end_time_hm), "
                      " late_threshold_min = COALESCE(?, late_threshold_min), "
                      " grace_min = COALESCE(?, grace_min), "
                      " ot_grace_min = COALESCE(?, ot_grace_min), "
                      " ot_min_minutes = COALESCE(?, ot_min_minutes), "
                      " ot_max_minutes = COALESCE(?, ot_max_minutes), "
                      " active = COALESCE(?, active) "
                      "WHERE id = ?");
            const auto bindStrOrNull = [&](int idx, const char* k) {
                if (body.contains(k) && body[k].is_string()) {
                    q.bindValue(idx, QString::fromStdString(body[k].get<std::string>()));
                } else {
                    q.bindValue(idx, QVariant(QString()));
                }
            };
            const auto bindIntOrNull = [&](int idx, const char* k) {
                if (body.contains(k) && body[k].is_number_integer()) {
                    q.bindValue(idx, body[k].get<int>());
                } else {
                    q.bindValue(idx, QVariant(QVariant::Int));
                }
            };
            bindStrOrNull(0, "name");
            bindStrOrNull(1, "start_time_hm");
            bindStrOrNull(2, "end_time_hm");
            bindIntOrNull(3, "late_threshold_min");
            bindIntOrNull(4, "grace_min");
            bindIntOrNull(5, "ot_grace_min");
            bindIntOrNull(6, "ot_min_minutes");
            bindIntOrNull(7, "ot_max_minutes");
            if (body.contains("active") && body["active"].is_boolean()) {
                q.bindValue(8, body["active"].get<bool>() ? 1 : 0);
            } else {
                q.bindValue(8, QVariant(QVariant::Int));
            }
            q.bindValue(9, id);

            if (!q.exec()) {
                const std::string msg = q.lastError().text().toStdString();
                if (msg.find("UNIQUE") != std::string::npos ||
                    msg.find("unique") != std::string::npos ||
                    msg.find("duplicate key") != std::string::npos) {
                    return ApiUtils::createErrorResponse("shift name already exists", 409, origin);
                }
                return ApiUtils::createErrorResponse(msg, 500, origin);
            }
            if (q.numRowsAffected() == 0) {
                return ApiUtils::createErrorResponse("shift not found", 404, origin);
            }
            return ApiUtils::createResponse({{"updated", true}, {"id", id}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // ─────────────────────────────────────────────────────────────────────
    // GET/POST /api/attendance/holidays
    //   GET  → list (optional ?year=YYYY filter; defaults to all dates)
    //   POST → create. Admin only.
    // Validates `date` strictly as YYYY-MM-DD; rejects duplicates with 409.
    // Holiday calendar is consulted by queryAttendanceForDate which sets
    // `is_holiday=true` on the day's rollup + skips late_minutes / OT calc.
    // ─────────────────────────────────────────────────────────────────────
    CROW_ROUTE(app, "/api/attendance/holidays")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        const std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        if (req.method == crow::HTTPMethod::Get) {
            if (auto err = requireAttendanceRead(app, req, origin)) return std::move(*err);
        }

        try {
            auto db = vms::database::DbManager::getInstance().getThreadConnection();
            if (!db.isValid() || !db.isOpen()) {
                return ApiUtils::createErrorResponse("Database unavailable", 503, origin);
            }

            if (req.method == crow::HTTPMethod::Get) {
                // ?year=YYYY filters to a single year. Without it we return
                // everything so the admin UI can show multi-year history.
                const char* year_param = req.url_params.get("year");
                QSqlQuery q(db);
                if (year_param) {
                    std::string year_s = year_param;
                    // Strict 4-digit year guard — `date LIKE '2026%'` would
                    // also match malformed rows that shouldn't be there but
                    // we defend against bad input regardless.
                    if (year_s.size() != 4 ||
                        !std::all_of(year_s.begin(), year_s.end(),
                                     [](char c) { return c >= '0' && c <= '9'; })) {
                        return ApiUtils::createErrorResponse("year must be 4 digits", 400, origin);
                    }
                    q.prepare("SELECT id, date, name, description "
                              "FROM holidays WHERE date LIKE ? "
                              "ORDER BY date ASC");
                    q.bindValue(0, QString::fromStdString(year_s + "%"));
                } else {
                    q.prepare("SELECT id, date, name, description "
                              "FROM holidays ORDER BY date ASC");
                }
                if (!q.exec()) {
                    return ApiUtils::createErrorResponse(q.lastError().text().toStdString(), 500, origin);
                }
                json arr = json::array();
                while (q.next()) {
                    arr.push_back({
                        {"id",          q.value(0).toInt()},
                        {"date",        q.value(1).toString().toStdString()},
                        {"name",        q.value(2).toString().toStdString()},
                        {"description", q.value(3).isNull() ? "" : q.value(3).toString().toStdString()}
                    });
                }
                return ApiUtils::createResponse({{"holidays", arr}}, 200, origin);
            }

            // POST → create. Admin only.
            if (auto err = requireAttendanceAdmin(app, req, origin)) return std::move(*err);

            auto body = json::parse(req.body);
            const std::string date = body.value("date", std::string{});
            const std::string name = body.value("name", std::string{});
            const std::string desc = body.value("description", std::string{});

            if (!isValidIsoDate(date)) {
                return ApiUtils::createErrorResponse("date must be YYYY-MM-DD", 400, origin);
            }
            if (name.empty() || name.size() > 200) {
                return ApiUtils::createErrorResponse("name required (1..200 chars)", 400, origin);
            }
            if (desc.size() > 500) {
                return ApiUtils::createErrorResponse("description too long (max 500 chars)", 400, origin);
            }

            QSqlQuery q(db);
            q.prepare("INSERT INTO holidays (date, name, description) VALUES (?, ?, ?)");
            q.bindValue(0, QString::fromStdString(date));
            q.bindValue(1, QString::fromStdString(name));
            q.bindValue(2, desc.empty() ? QVariant() : QVariant(QString::fromStdString(desc)));

            if (!q.exec()) {
                const std::string msg = q.lastError().text().toStdString();
                if (msg.find("UNIQUE") != std::string::npos ||
                    msg.find("unique") != std::string::npos ||
                    msg.find("duplicate key") != std::string::npos) {
                    return ApiUtils::createErrorResponse("holiday on this date already exists", 409, origin);
                }
                return ApiUtils::createErrorResponse(msg, 500, origin);
            }

            const int new_id = q.lastInsertId().toInt();
            return ApiUtils::createResponse({
                {"id", new_id}, {"date", date}, {"name", name}
            }, 201, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // GET/PUT/DELETE /api/attendance/holidays/<int>
    // DELETE is hard-delete (no FK references; safe). Soft-delete pattern
    // from shifts doesn't apply — holidays don't anchor historical rows the
    // way shift_id does in attendance_events.
    CROW_ROUTE(app, "/api/attendance/holidays/<int>")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Put,
             crow::HTTPMethod::Delete, crow::HTTPMethod::Options)
    ([&app](const crow::request& req, int id) {
        const std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        if (req.method == crow::HTTPMethod::Get) {
            if (auto err = requireAttendanceRead(app, req, origin)) return std::move(*err);
        } else {
            if (auto err = requireAttendanceAdmin(app, req, origin)) return std::move(*err);
        }

        try {
            auto db = vms::database::DbManager::getInstance().getThreadConnection();
            if (!db.isValid() || !db.isOpen()) {
                return ApiUtils::createErrorResponse("Database unavailable", 503, origin);
            }

            if (req.method == crow::HTTPMethod::Get) {
                QSqlQuery q(db);
                q.prepare("SELECT id, date, name, description FROM holidays WHERE id = ?");
                q.bindValue(0, id);
                if (!q.exec()) {
                    return ApiUtils::createErrorResponse(q.lastError().text().toStdString(), 500, origin);
                }
                if (!q.next()) {
                    return ApiUtils::createErrorResponse("holiday not found", 404, origin);
                }
                return ApiUtils::createResponse({
                    {"id",          q.value(0).toInt()},
                    {"date",        q.value(1).toString().toStdString()},
                    {"name",        q.value(2).toString().toStdString()},
                    {"description", q.value(3).isNull() ? "" : q.value(3).toString().toStdString()}
                }, 200, origin);
            }

            if (req.method == crow::HTTPMethod::Delete) {
                QSqlQuery q(db);
                q.prepare("DELETE FROM holidays WHERE id = ?");
                q.bindValue(0, id);
                if (!q.exec()) {
                    return ApiUtils::createErrorResponse(q.lastError().text().toStdString(), 500, origin);
                }
                if (q.numRowsAffected() == 0) {
                    return ApiUtils::createErrorResponse("holiday not found", 404, origin);
                }
                return ApiUtils::createResponse({{"deleted", true}, {"id", id}}, 200, origin);
            }

            // PUT — partial update via COALESCE-style guards.
            auto body = json::parse(req.body);
            if (body.contains("date")) {
                if (!body["date"].is_string() || !isValidIsoDate(body["date"].get<std::string>())) {
                    return ApiUtils::createErrorResponse("date must be YYYY-MM-DD", 400, origin);
                }
            }
            if (body.contains("name")) {
                if (!body["name"].is_string() ||
                    body["name"].get<std::string>().empty() ||
                    body["name"].get<std::string>().size() > 200) {
                    return ApiUtils::createErrorResponse("name must be 1..200 chars", 400, origin);
                }
            }
            if (body.contains("description")) {
                if (!body["description"].is_string() ||
                    body["description"].get<std::string>().size() > 500) {
                    return ApiUtils::createErrorResponse("description too long (max 500 chars)", 400, origin);
                }
            }

            QSqlQuery q(db);
            q.prepare(
                "UPDATE holidays SET "
                "  date = COALESCE(?, date), "
                "  name = COALESCE(?, name), "
                "  description = COALESCE(?, description) "
                "WHERE id = ?");
            q.bindValue(0, body.contains("date") ? QVariant(QString::fromStdString(body["date"].get<std::string>())) : QVariant());
            q.bindValue(1, body.contains("name") ? QVariant(QString::fromStdString(body["name"].get<std::string>())) : QVariant());
            q.bindValue(2, body.contains("description") ? QVariant(QString::fromStdString(body["description"].get<std::string>())) : QVariant());
            q.bindValue(3, id);

            if (!q.exec()) {
                const std::string msg = q.lastError().text().toStdString();
                if (msg.find("UNIQUE") != std::string::npos ||
                    msg.find("unique") != std::string::npos ||
                    msg.find("duplicate key") != std::string::npos) {
                    return ApiUtils::createErrorResponse("another holiday already exists on this date", 409, origin);
                }
                return ApiUtils::createErrorResponse(msg, 500, origin);
            }
            if (q.numRowsAffected() == 0) {
                return ApiUtils::createErrorResponse("holiday not found", 404, origin);
            }
            return ApiUtils::createResponse({{"updated", true}, {"id", id}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });

    // ─────────────────────────────────────────────────────────────────────
    // GET /api/attendance/health — BulkWriter pending + dropped counters.
    // Used by ops to detect attendance write backlog.
    // ─────────────────────────────────────────────────────────────────────
    CROW_ROUTE(app, "/api/attendance/health")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        const std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) {
            return ApiUtils::createResponse(json::object(), 204, origin);
        }
        if (auto err = requireAttendanceRead(app, req, origin)) return std::move(*err);
        auto& tr = vms::core::AttendanceTracker::getInstance();
        return ApiUtils::createResponse({
            {"pending_rows", static_cast<uint64_t>(tr.pendingRows())},
            {"dropped_rows", static_cast<uint64_t>(tr.droppedRows())}
        }, 200, origin);
    });
}

} // namespace vms::api
