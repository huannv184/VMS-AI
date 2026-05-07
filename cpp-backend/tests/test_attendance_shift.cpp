// ==============================================================
// Inline reproduction of the night-shift helpers from
// src/api/attendance_controller.cpp.  Update both files together
// when the helper logic changes (no link to backend; we keep this
// pure-stdlib so it builds without Qt/Crow).
//
// Targets the bug where 22:00→06:00 shifts caused next-morning
// punches to compute a NEGATIVE delta and silently report
// "on time" when the worker actually punched 8h late.
// ==============================================================

#include <gtest/gtest.h>

#include <cstdio>
#include <ctime>
#include <string>

namespace {

bool isValidHmTime(const std::string& s) {
    if (s.size() != 5) return false;
    if (s[2] != ':') return false;
    for (size_t i : {0u,1u,3u,4u}) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    int h = (s[0]-'0')*10 + (s[1]-'0');
    int m = (s[3]-'0')*10 + (s[4]-'0');
    return h >= 0 && h <= 23 && m >= 0 && m <= 59;
}

int hmToMinutes(const std::string& s) {
    return ((s[0]-'0')*10 + (s[1]-'0')) * 60 + ((s[3]-'0')*10 + (s[4]-'0'));
}

std::time_t shiftDateMidnight(std::time_t ts, bool has_shift,
                              int start_min, int end_min) {
    std::tm tm = *std::localtime(&ts);
    const int hour_min = tm.tm_hour * 60 + tm.tm_min;

    const bool overnight = has_shift && (start_min > end_min) &&
                           start_min >= 0 && end_min >= 0;
    if (overnight) {
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

int lateMinutesForPunch(std::time_t ci_ts, std::time_t shift_date_midnight,
                        int start_min, int grace_min) {
    const long long minutes_since =
        (static_cast<long long>(ci_ts) -
         static_cast<long long>(shift_date_midnight)) / 60;
    const long long delta = minutes_since - start_min - grace_min;
    if (delta <= 0) return 0;
    return static_cast<int>(delta);
}

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

// Build a local-TZ epoch for "YYYY-MM-DD HH:MM" — keeps tests deterministic
// regardless of the host's TZ (helpers themselves use localtime/mktime).
std::time_t localEpoch(int y, int mo, int d, int h, int mi) {
    std::tm tm = {};
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_sec  = 0;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

std::time_t midnightOf(int y, int mo, int d) {
    return localEpoch(y, mo, d, 0, 0);
}

}  // namespace

// ──────────────────────────────────────────────────────────────────────────
// HmTime parsing
// ──────────────────────────────────────────────────────────────────────────

TEST(HmTime, ValidStrict) {
    EXPECT_TRUE(isValidHmTime("00:00"));
    EXPECT_TRUE(isValidHmTime("23:59"));
    EXPECT_TRUE(isValidHmTime("06:00"));
    EXPECT_TRUE(isValidHmTime("22:00"));
}

TEST(HmTime, RejectShortOrMalformed) {
    EXPECT_FALSE(isValidHmTime(""));
    EXPECT_FALSE(isValidHmTime("8:00"));
    EXPECT_FALSE(isValidHmTime("08:0"));
    EXPECT_FALSE(isValidHmTime("24:00"));
    EXPECT_FALSE(isValidHmTime("12:60"));
    EXPECT_FALSE(isValidHmTime("1a:00"));
}

TEST(HmToMinutes, BoundaryValues) {
    EXPECT_EQ(hmToMinutes("00:00"), 0);
    EXPECT_EQ(hmToMinutes("06:00"), 360);
    EXPECT_EQ(hmToMinutes("14:00"), 840);
    EXPECT_EQ(hmToMinutes("22:00"), 1320);
    EXPECT_EQ(hmToMinutes("23:59"), 1439);
}

// ──────────────────────────────────────────────────────────────────────────
// shiftDateMidnight — day shifts: shift_date == calendar date of punch.
// ──────────────────────────────────────────────────────────────────────────

TEST(ShiftDate, DayShiftEqualsCalendarDate) {
    // 09:00→17:00 day shift; punch at 09:05 on 2026-05-04 → bucket = 2026-05-04.
    const auto ts = localEpoch(2026, 5, 4, 9, 5);
    const auto sd = shiftDateMidnight(ts, /*has_shift*/true,
                                      hmToMinutes("09:00"), hmToMinutes("17:00"));
    EXPECT_EQ(sd, midnightOf(2026, 5, 4));
}

TEST(ShiftDate, DayShiftAfterMidnightStillCalendarDate) {
    // Day-shift worker who somehow punches at 02:00 — bucket should still
    // be calendar date of the punch (no overnight rule applies).
    const auto ts = localEpoch(2026, 5, 5, 2, 0);
    const auto sd = shiftDateMidnight(ts, true,
                                      hmToMinutes("09:00"), hmToMinutes("17:00"));
    EXPECT_EQ(sd, midnightOf(2026, 5, 5));
}

TEST(ShiftDate, NoShiftFallsBackToCalendarDate) {
    const auto ts = localEpoch(2026, 5, 4, 23, 30);
    const auto sd = shiftDateMidnight(ts, /*has_shift*/false, -1, -1);
    EXPECT_EQ(sd, midnightOf(2026, 5, 4));
}

// ──────────────────────────────────────────────────────────────────────────
// shiftDateMidnight — night shifts: midpoint-off rule splits the day.
// 22:00→06:00 shift: off-duty = [06:00, 22:00], midpoint = 14:00.
//   • punch < 14:00 → yesterday's instance.
//   • punch ≥ 14:00 → today's instance.
// ──────────────────────────────────────────────────────────────────────────

TEST(ShiftDate, NightShiftEveningInIsTodayInstance) {
    // 22:05 on 2026-05-04 → today's instance (2026-05-04).
    const auto ts = localEpoch(2026, 5, 4, 22, 5);
    const auto sd = shiftDateMidnight(ts, true,
                                      hmToMinutes("22:00"), hmToMinutes("06:00"));
    EXPECT_EQ(sd, midnightOf(2026, 5, 4));
}

TEST(ShiftDate, NightShiftMorningOutIsYesterdayInstance) {
    // 06:05 on 2026-05-05 → previous day's instance (2026-05-04).
    const auto ts = localEpoch(2026, 5, 5, 6, 5);
    const auto sd = shiftDateMidnight(ts, true,
                                      hmToMinutes("22:00"), hmToMinutes("06:00"));
    EXPECT_EQ(sd, midnightOf(2026, 5, 4));
}

TEST(ShiftDate, NightShiftBoundaryAtMidpoint) {
    // 14:00 sharp ≥ midpoint → today's instance.
    const auto ts1 = localEpoch(2026, 5, 4, 14, 0);
    const auto sd1 = shiftDateMidnight(ts1, true, 1320, 360);
    EXPECT_EQ(sd1, midnightOf(2026, 5, 4));
    // 13:59 < midpoint → yesterday's instance.
    const auto ts2 = localEpoch(2026, 5, 4, 13, 59);
    const auto sd2 = shiftDateMidnight(ts2, true, 1320, 360);
    EXPECT_EQ(sd2, midnightOf(2026, 5, 3));
}

TEST(ShiftDate, NightShiftEarlyArrival) {
    // Worker arrives at 21:00 for a 22:00 start — still today's instance.
    const auto ts = localEpoch(2026, 5, 4, 21, 0);
    const auto sd = shiftDateMidnight(ts, true, 1320, 360);
    EXPECT_EQ(sd, midnightOf(2026, 5, 4));
}

TEST(ShiftDate, NightShiftLateCheckOut) {
    // Late check-out at 09:00 next morning still buckets to yesterday's
    // instance via the midpoint rule (09:00 < 14:00).
    const auto ts = localEpoch(2026, 5, 5, 9, 0);
    const auto sd = shiftDateMidnight(ts, true, 1320, 360);
    EXPECT_EQ(sd, midnightOf(2026, 5, 4));
}

// ──────────────────────────────────────────────────────────────────────────
// lateMinutesForPunch — the actual bug surface.
// ──────────────────────────────────────────────────────────────────────────

TEST(LateMinutes, DayShiftOnTime) {
    const auto ci = localEpoch(2026, 5, 4, 9, 5);
    const auto sd = midnightOf(2026, 5, 4);
    // 09:00 start, 10-min grace → 9:05 - 9:00 - 10 = -5 → 0.
    EXPECT_EQ(lateMinutesForPunch(ci, sd, 540, 10), 0);
}

TEST(LateMinutes, DayShiftLate) {
    const auto ci = localEpoch(2026, 5, 4, 9, 20);
    const auto sd = midnightOf(2026, 5, 4);
    // 9:20 - 9:00 - 0 grace = 20 min late.
    EXPECT_EQ(lateMinutesForPunch(ci, sd, 540, 0), 20);
}

TEST(LateMinutes, NightShiftEveningInOnTime) {
    // 22:05 on shift_date 2026-05-04, start 22:00, grace 10.
    const auto ci = localEpoch(2026, 5, 4, 22, 5);
    const auto sd = midnightOf(2026, 5, 4);
    // 22:05 - 22:00 - 10 = -5 → 0.
    EXPECT_EQ(lateMinutesForPunch(ci, sd, 1320, 10), 0);
}

TEST(LateMinutes, NightShiftEveningInSlightlyLate) {
    // 22:25 with start 22:00 grace 10 → 25 - 10 = 15 min late.
    const auto ci = localEpoch(2026, 5, 4, 22, 25);
    const auto sd = midnightOf(2026, 5, 4);
    EXPECT_EQ(lateMinutesForPunch(ci, sd, 1320, 10), 15);
}

// ── This is the regression case for the night-shift bug. ──────────────────
TEST(LateMinutes, NightShiftMorningOnlyPunchIsVeryLate) {
    // Worker missed the 22:00 IN entirely, only got picked up at 06:05
    // the next morning (calendar 2026-05-05).  shift_date is still
    // 2026-05-04 by the midpoint rule, so:
    //   minutes_since(shift_date_midnight) = 24*60 + 6*60 + 5 = 1805
    //   delta = 1805 - 1320 - 0 grace = 485 min late.
    // OLD CODE: ci_min_of_day = 365, shift_min = 1320, delta = -955 → 0
    //           silently reported "on time".
    const auto ci = localEpoch(2026, 5, 5, 6, 5);
    const auto sd = midnightOf(2026, 5, 4);
    EXPECT_EQ(lateMinutesForPunch(ci, sd, 1320, 0), 485);
}

TEST(LateMinutes, NightShiftEarlyArrivalNotNegative) {
    // 21:00 arrival for 22:00 start → 60 min EARLY → late=0 (clamped).
    const auto ci = localEpoch(2026, 5, 4, 21, 0);
    const auto sd = midnightOf(2026, 5, 4);
    EXPECT_EQ(lateMinutesForPunch(ci, sd, 1320, 0), 0);
}

TEST(LateMinutes, GraceAbsorbsExactBoundary) {
    // 9:10 with 10-min grace → exactly on boundary (delta=0) → 0.
    const auto ci = localEpoch(2026, 5, 4, 9, 10);
    const auto sd = midnightOf(2026, 5, 4);
    EXPECT_EQ(lateMinutesForPunch(ci, sd, 540, 10), 0);
}

// ──────────────────────────────────────────────────────────────────────────
// End-to-end: combine shiftDateMidnight + lateMinutesForPunch the way the
// controller does, to make sure the two play correctly together.
// ──────────────────────────────────────────────────────────────────────────

TEST(EndToEnd, NightShiftWorkerCheckInOutSpansMidnight) {
    // 22:05 IN on 2026-05-04, 06:00 OUT on 2026-05-05.  Both should bucket
    // to shift_date 2026-05-04, and "late on first IN" should be 5 min.
    const auto in_ts  = localEpoch(2026, 5, 4, 22, 5);
    const auto out_ts = localEpoch(2026, 5, 5, 6, 0);

    const auto sd_in  = shiftDateMidnight(in_ts,  true, 1320, 360);
    const auto sd_out = shiftDateMidnight(out_ts, true, 1320, 360);
    EXPECT_EQ(sd_in,  midnightOf(2026, 5, 4));
    EXPECT_EQ(sd_out, midnightOf(2026, 5, 4));

    // Late computation uses MIN(in_ts) of the bucket:
    EXPECT_EQ(lateMinutesForPunch(in_ts, sd_in, 1320, 0), 5);
}

// ──────────────────────────────────────────────────────────────────────────
// overtimeMinutesForPunch — trailing OT computation.
// ──────────────────────────────────────────────────────────────────────────

TEST(Overtime, NoCheckOutReturnsZero) {
    EXPECT_EQ(overtimeMinutesForPunch(0, midnightOf(2026, 5, 4),
                                      540, 1020, 0, 0, 720), 0);
}

TEST(Overtime, DayShiftLeftBeforeEndIsZero) {
    // 16:30 leave for 17:00 end shift → no OT.
    const auto co = localEpoch(2026, 5, 4, 16, 30);
    const auto sd = midnightOf(2026, 5, 4);
    EXPECT_EQ(overtimeMinutesForPunch(co, sd, 540, 1020, 0, 0, 720), 0);
}

TEST(Overtime, DayShiftLeftExactlyAtEndIsZero) {
    const auto co = localEpoch(2026, 5, 4, 17, 0);
    const auto sd = midnightOf(2026, 5, 4);
    EXPECT_EQ(overtimeMinutesForPunch(co, sd, 540, 1020, 0, 0, 720), 0);
}

TEST(Overtime, DayShiftSlightOTNoGrace) {
    // 17:30 leave for 17:00 end → 30 min OT.
    const auto co = localEpoch(2026, 5, 4, 17, 30);
    const auto sd = midnightOf(2026, 5, 4);
    EXPECT_EQ(overtimeMinutesForPunch(co, sd, 540, 1020, 0, 0, 720), 30);
}

TEST(Overtime, OtGraceAbsorbsTrailingMinutes) {
    // 17:25 leave for 17:00 end + 30 min ot_grace → 0 (not yet OT).
    const auto co = localEpoch(2026, 5, 4, 17, 25);
    const auto sd = midnightOf(2026, 5, 4);
    EXPECT_EQ(overtimeMinutesForPunch(co, sd, 540, 1020, 30, 0, 720), 0);
}

TEST(Overtime, OtMinFiltersTinyTrailing) {
    // 17:10 leave for 17:00 end, ot_min=15 → delta=10 < 15 → 0 (not enough).
    const auto co = localEpoch(2026, 5, 4, 17, 10);
    const auto sd = midnightOf(2026, 5, 4);
    EXPECT_EQ(overtimeMinutesForPunch(co, sd, 540, 1020, 0, 15, 720), 0);
}

TEST(Overtime, OtMinAtBoundaryStillCounts) {
    // 17:15 leave for 17:00 end, ot_min=15 → delta=15, NOT less than → 15.
    const auto co = localEpoch(2026, 5, 4, 17, 15);
    const auto sd = midnightOf(2026, 5, 4);
    EXPECT_EQ(overtimeMinutesForPunch(co, sd, 540, 1020, 0, 15, 720), 15);
}

TEST(Overtime, OtMaxClampsOutliers) {
    // Worker forgot to clock out, "left" at 9am next day → 16 hours after
    // shift end.  ot_max=720 (12h) clamps to 720.
    const auto co = localEpoch(2026, 5, 5, 9, 0);
    const auto sd = midnightOf(2026, 5, 4);
    EXPECT_EQ(overtimeMinutesForPunch(co, sd, 540, 1020, 0, 0, 720), 720);
}

TEST(Overtime, OtMaxZeroMeansNoCap) {
    // ot_max=0 sentinel disables the cap (helper returns the raw delta).
    const auto co = localEpoch(2026, 5, 4, 23, 0);
    const auto sd = midnightOf(2026, 5, 4);
    // 23:00 - 17:00 - 0 grace = 360 min OT, no cap → 360.
    EXPECT_EQ(overtimeMinutesForPunch(co, sd, 540, 1020, 0, 0, 0), 360);
}

TEST(Overtime, NightShiftOtAfterMorningEnd) {
    // Night shift 22:00 → 06:00.  Worker stays until 07:00 next day → 1h OT.
    const auto co = localEpoch(2026, 5, 5, 7, 0);
    const auto sd = midnightOf(2026, 5, 4);
    // shift_end_abs = 360 (06:00) + 1440 (next day) = 1800.
    // co minutes since sd-midnight = 24*60 + 7*60 = 1860.
    // delta = 1860 - 1800 - 0 = 60.
    EXPECT_EQ(overtimeMinutesForPunch(co, sd, 1320, 360, 0, 0, 720), 60);
}

TEST(Overtime, NightShiftCheckOutBeforeEndIsZero) {
    // Worker punches out at 05:30 (before the 06:00 end) → no OT.
    const auto co = localEpoch(2026, 5, 5, 5, 30);
    const auto sd = midnightOf(2026, 5, 4);
    EXPECT_EQ(overtimeMinutesForPunch(co, sd, 1320, 360, 0, 0, 720), 0);
}

TEST(EndToEnd, BackToBackNightShiftsBucketSeparately) {
    // Two consecutive night shifts for the same worker:
    //   2026-05-03 22:00 IN  →  2026-05-04 06:05 OUT
    //   2026-05-04 22:05 IN  →  2026-05-05 06:00 OUT
    // shift_date assignments:
    const auto t1 = localEpoch(2026, 5, 3, 22, 0);
    const auto t2 = localEpoch(2026, 5, 4, 6, 5);
    const auto t3 = localEpoch(2026, 5, 4, 22, 5);
    const auto t4 = localEpoch(2026, 5, 5, 6, 0);

    EXPECT_EQ(shiftDateMidnight(t1, true, 1320, 360), midnightOf(2026, 5, 3));
    EXPECT_EQ(shiftDateMidnight(t2, true, 1320, 360), midnightOf(2026, 5, 3));
    EXPECT_EQ(shiftDateMidnight(t3, true, 1320, 360), midnightOf(2026, 5, 4));
    EXPECT_EQ(shiftDateMidnight(t4, true, 1320, 360), midnightOf(2026, 5, 4));
}
